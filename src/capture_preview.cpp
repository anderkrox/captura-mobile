#include "capture_preview.h"

#include "capture_core.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <utility>

using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

namespace yourots {
namespace {

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return "erro Win32 sem mensagem";
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
}

IDirect3DDevice CreateWinrtDirect3DDevice(winrt::com_ptr<ID3D11Device>& d3d_device,
                                          winrt::com_ptr<ID3D11DeviceContext>& d3d_context) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    static constexpr D3D_FEATURE_LEVEL levels[]{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected{};
    winrt::check_hresult(D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        d3d_device.put(),
        &selected,
        d3d_context.put()));

    auto dxgi_device = d3d_device.as<IDXGIDevice>();
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put()));
    return inspectable.as<IDirect3DDevice>();
}

GraphicsCaptureItem CreateCaptureItemForWindow(HWND hwnd) {
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    for (unsigned attempt = 0; ; ++attempt) {
        const auto result = interop->CreateForWindow(
            hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item));
        if (result != E_INVALIDARG || attempt == 4 || !IsWindow(hwnd) ||
            !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
            winrt::check_hresult(result);
            break;
        }
        // Windows 10 can reject the first activation of a newly presented window.
        // Bound retries and retain the final HRESULT for genuinely unavailable sources.
        item = nullptr;
        Sleep(100U << attempt);
    }
    return item;
}

winrt::com_ptr<ID3D11Texture2D> GetTextureFromSurface(
    const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface& surface) {
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
    return texture;
}

} // namespace

std::wstring GetWindowTitleText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length + 1), L'\0');
    const int copied = GetWindowTextW(hwnd, result.data(), length + 1);
    result.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0U);
    return result;
}

std::wstring GetWindowProcessName(HWND hwnd) {
    DWORD process_id{};
    GetWindowThreadProcessId(hwnd, &process_id);
    if (process_id == 0) {
        return {};
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        return {};
    }
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
        CloseHandle(process);
        return {};
    }
    CloseHandle(process);
    path.resize(length);
    return std::filesystem::path(path).filename().wstring();
}

std::wstring GetWindowClassName(HWND hwnd) {
    std::wstring result(256, L'\0');
    const int copied = GetClassNameW(hwnd, result.data(), static_cast<int>(result.size()));
    if (copied <= 0) {
        return {};
    }
    result.resize(static_cast<std::size_t>(copied));
    return result;
}

std::vector<WindowInfo> EnumerateCaptureWindows(HWND excluded_window) {
    struct Context {
        HWND excluded{};
        std::vector<WindowInfo> windows;
    } context{excluded_window, {}};

    EnumWindows(
        [](HWND hwnd, LPARAM parameter) -> BOOL {
            auto& context = *reinterpret_cast<Context*>(parameter);
            if (hwnd == context.excluded || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
                return TRUE;
            }
            const auto title = GetWindowTitleText(hwnd);
            if (title.empty()) {
                return TRUE;
            }
            RECT rect{};
            if (!GetWindowRect(hwnd, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) {
                return TRUE;
            }
            context.windows.push_back(WindowInfo{
                hwnd,
                GetWindowProcessName(hwnd),
                title,
                GetWindowClassName(hwnd),
                rect,
            });
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));

    std::ranges::sort(context.windows, [](const WindowInfo& a, const WindowInfo& b) {
        auto lhs = a.process + L"\n" + a.title;
        auto rhs = b.process + L"\n" + b.title;
        std::ranges::transform(lhs, lhs.begin(), ::towlower);
        std::ranges::transform(rhs, rhs.begin(), ::towlower);
        return lhs < rhs;
    });
    return context.windows;
}

struct WindowCapture::Impl {
    explicit Impl(HWND source) : hwnd(source) {}

    void Start(const std::weak_ptr<WindowCapture>& owner) {
        const char* operation = "GraphicsCaptureSession::IsSupported";
        try {
            if (!GraphicsCaptureSession::IsSupported()) {
                throw std::runtime_error("Windows.Graphics.Capture nao e suportado nesta execucao.");
            }
            operation = "D3D11CreateDevice";
            winrt_device = CreateWinrtDirect3DDevice(d3d_device, d3d_context);
            operation = "GraphicsCaptureItem::CreateForWindow";
            item = CreateCaptureItemForWindow(hwnd);
            frame_size = item.Size();
            if (frame_size.Width <= 0 || frame_size.Height <= 0) {
                throw std::runtime_error("A janela selecionada nao possui dimensoes capturaveis.");
            }
            operation = "Direct3D11CaptureFramePool::CreateFreeThreaded";
            pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrt_device,
                DirectXPixelFormat::B8G8R8A8UIntNormalized,
                3,
                frame_size);
            operation = "CreateCaptureSession";
            session = pool.CreateCaptureSession(item);
            frame_token = pool.FrameArrived([owner](const auto& sender, const auto&) {
                if (auto strong = owner.lock()) {
                    strong->impl_->OnFrame(sender);
                }
            });
            frame_registered = true;
            closed_token = item.Closed([owner](const auto&, const auto&) {
                if (auto strong = owner.lock()) {
                    strong->impl_->SetError("A janela fonte foi fechada.");
                }
            });
            closed_registered = true;
            started.store(true);
            operation = "GraphicsCaptureSession::StartCapture";
            session.StartCapture();
        } catch (const winrt::hresult_error& error) {
            throw std::runtime_error(std::string(operation) + " falhou (HRESULT=" +
                std::to_string(error.code().value) + "): " + WideToUtf8(error.message().c_str()));
        }
    }

    void Stop() noexcept {
        started.store(false);
        try {
            // Revocation/Close can wait for callbacks. Never hold their mutex while
            // calling those APIs: a queued callback may be waiting for this mutex.
            if (pool && frame_registered) {
                pool.FrameArrived(frame_token);
                frame_registered = false;
            }
            if (item && closed_registered) {
                item.Closed(closed_token);
                closed_registered = false;
            }
            {
                std::scoped_lock callback_lock(callback_mutex);
                // Barrier: an in-flight OnFrame has finished using the resources.
            }
            if (session) {
                session.Close();
                session = nullptr;
            }
            if (pool) {
                pool.Close();
                pool = nullptr;
            }
            staging = nullptr;
        } catch (...) {
        }
    }

    void OnFrame(const Direct3D11CaptureFramePool& sender) noexcept {
        if (!started.load()) return;
        std::scoped_lock callback_lock(callback_mutex);
        if (!started.load()) {
            return;
        }
        try {
            auto frame = sender.TryGetNextFrame();
            if (!frame) {
                return;
            }
            const auto content_size = frame.ContentSize();
            if (content_size.Width <= 0 || content_size.Height <= 0) {
                return;
            }
            if (content_size.Width != frame_size.Width || content_size.Height != frame_size.Height) {
                frame_size = content_size;
                frame.Close();
                sender.Recreate(
                    winrt_device,
                    DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    3,
                    frame_size);
                staging = nullptr;
                return;
            }

            auto source = GetTextureFromSurface(frame.Surface());
            D3D11_TEXTURE2D_DESC source_desc{};
            source->GetDesc(&source_desc);
            const auto width = static_cast<std::uint32_t>(content_size.Width);
            const auto height = static_cast<std::uint32_t>(content_size.Height);
            if (width > source_desc.Width || height > source_desc.Height) {
                return;
            }
            EnsureStaging(width, height, source_desc.Format);
            const D3D11_BOX box{0, 0, 0, width, height, 1};
            d3d_context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, source.get(), 0, &box);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(d3d_context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
            auto next = std::make_shared<PreviewFrame>();
            next->width = width;
            next->height = height;
            try {
                next->bgra = PackBgraRows(
                    {static_cast<const std::uint8_t*>(mapped.pData),
                     static_cast<std::size_t>(mapped.RowPitch) * height},
                    width,
                    height,
                    mapped.RowPitch);
            } catch (...) {
                d3d_context->Unmap(staging.get(), 0);
                throw;
            }
            d3d_context->Unmap(staging.get(), 0);

            {
                std::scoped_lock lock(mutex);
                next->sequence = latest ? latest->sequence + 1 : 1;
                latest = std::move(next);
            }
        } catch (const winrt::hresult_error& error) {
            SetError("Falha na captura: " + WideToUtf8(error.message().c_str()));
        } catch (const std::exception& error) {
            SetError(std::string("Falha na captura: ") + error.what());
        } catch (...) {
            SetError("Falha desconhecida na captura.");
        }
    }

    void EnsureStaging(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format) {
        if (staging && staging_width == width && staging_height == height && staging_format == format) {
            return;
        }
        staging = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::check_hresult(d3d_device->CreateTexture2D(&desc, nullptr, staging.put()));
        staging_width = width;
        staging_height = height;
        staging_format = format;
    }

    void SetError(std::string message) noexcept {
        std::scoped_lock lock(mutex);
        if (last_error.empty()) {
            last_error = std::move(message);
        }
    }

    HWND hwnd{};
    winrt::com_ptr<ID3D11Device> d3d_device;
    winrt::com_ptr<ID3D11DeviceContext> d3d_context;
    IDirect3DDevice winrt_device{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 frame_size{};
    winrt::event_token frame_token{};
    winrt::event_token closed_token{};
    bool frame_registered{};
    bool closed_registered{};
    winrt::com_ptr<ID3D11Texture2D> staging;
    std::uint32_t staging_width{};
    std::uint32_t staging_height{};
    DXGI_FORMAT staging_format{DXGI_FORMAT_UNKNOWN};
    std::atomic_bool started{false};
    mutable std::mutex mutex;
    std::mutex callback_mutex;
    std::shared_ptr<const PreviewFrame> latest;
    std::string last_error;
};

WindowCapture::WindowCapture(HWND hwnd) : impl_(std::make_unique<Impl>(hwnd)), hwnd_(hwnd) {
    if (!IsWindow(hwnd_)) {
        throw std::runtime_error("A janela selecionada nao e mais valida.");
    }
}

WindowCapture::~WindowCapture() {
    Stop();
}

void WindowCapture::Start() {
    impl_->Start(weak_from_this());
}

void WindowCapture::Stop() noexcept {
    if (impl_) {
        impl_->Stop();
    }
}

std::shared_ptr<const PreviewFrame> WindowCapture::LatestFrame() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->latest;
}

std::string WindowCapture::LastError() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->last_error;
}

} // namespace yourots
