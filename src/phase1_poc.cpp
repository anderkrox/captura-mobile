#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

namespace {

struct CropRect {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};

    bool IsFullFrame() const noexcept {
        return width == 0 || height == 0;
    }
};

struct FrameBuffer {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> bgra;
    std::uint64_t sequence{};
};

struct Options {
    bool list_windows{};
    HWND hwnd{};
    CropRect crop{};
    fs::path snapshot;
    fs::path overlay_snapshot;
    fs::path record;
    fs::path ffmpeg{L"third_party\\ffmpeg\\bin\\ffmpeg.exe"};
    int seconds{};
    bool overlay_probe{};
};

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        throw std::runtime_error("Falha ao converter UTF-8 para UTF-16.");
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required);
    return result;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error("Falha ao converter UTF-16 para UTF-8.");
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
}

std::wstring GetWindowTitle(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return {};
    }
    std::wstring title(static_cast<std::size_t>(length + 1), L'\0');
    const int copied = GetWindowTextW(hwnd, title.data(), length + 1);
    title.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0U);
    return title;
}

std::wstring GetProcessImageName(HWND hwnd) {
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
    return fs::path(path).filename().wstring();
}

struct WindowInfo {
    HWND hwnd{};
    std::wstring process;
    std::wstring title;
    RECT rect{};
};

std::vector<WindowInfo> EnumerateWindows() {
    std::vector<WindowInfo> result;
    EnumWindows(
        [](HWND hwnd, LPARAM parameter) -> BOOL {
            auto* windows = reinterpret_cast<std::vector<WindowInfo>*>(parameter);
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
                return TRUE;
            }
            const auto title = GetWindowTitle(hwnd);
            if (title.empty()) {
                return TRUE;
            }
            RECT rect{};
            if (!GetWindowRect(hwnd, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) {
                return TRUE;
            }
            windows->push_back(WindowInfo{hwnd, GetProcessImageName(hwnd), title, rect});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result));
    return result;
}

HWND FindDefaultEdgeWindow() {
    auto windows = EnumerateWindows();
    HWND best{};
    long long best_area{};
    for (const auto& window : windows) {
        std::wstring process = window.process;
        std::transform(process.begin(), process.end(), process.begin(), ::towlower);
        if (process != L"msedge.exe") {
            continue;
        }
        const long long width = window.rect.right - window.rect.left;
        const long long height = window.rect.bottom - window.rect.top;
        const long long area = width * height;
        if (area > best_area) {
            best_area = area;
            best = window.hwnd;
        }
    }
    return best;
}

std::wstring QuoteCommandArgument(const std::wstring& argument) {
    if (argument.find_first_of(L" \t\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

CropRect ParseCrop(const std::wstring& text) {
    std::wstringstream stream(text);
    CropRect crop{};
    wchar_t comma1{}, comma2{}, comma3{};
    if (!(stream >> crop.x >> comma1 >> crop.y >> comma2 >> crop.width >> comma3 >> crop.height) ||
        comma1 != L',' || comma2 != L',' || comma3 != L',' || crop.width == 0 || crop.height == 0) {
        throw std::runtime_error("Formato invalido para --crop. Use X,Y,LARGURA,ALTURA.");
    }
    return crop;
}

Options ParseOptions(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument = argv[index];
        auto require_value = [&](const wchar_t* name) -> std::wstring {
            if (index + 1 >= argc) {
                throw std::runtime_error("Argumento sem valor: " + WideToUtf8(name));
            }
            return argv[++index];
        };

        if (argument == L"--list-windows") {
            options.list_windows = true;
        } else if (argument == L"--hwnd") {
            const auto value = require_value(L"--hwnd");
            options.hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(std::stoull(value, nullptr, 0)));
        } else if (argument == L"--crop") {
            options.crop = ParseCrop(require_value(L"--crop"));
        } else if (argument == L"--snapshot") {
            options.snapshot = require_value(L"--snapshot");
        } else if (argument == L"--overlay-snapshot") {
            options.overlay_snapshot = require_value(L"--overlay-snapshot");
            options.overlay_probe = true;
        } else if (argument == L"--record") {
            options.record = require_value(L"--record");
        } else if (argument == L"--seconds") {
            options.seconds = std::stoi(require_value(L"--seconds"));
        } else if (argument == L"--ffmpeg") {
            options.ffmpeg = require_value(L"--ffmpeg");
        } else if (argument == L"--help" || argument == L"-h") {
            std::cout
                << "YourotsCapturePoc\n"
                << "  --list-windows\n"
                << "  --hwnd <valor>                 Janela a capturar; sem este argumento usa o maior Edge visivel\n"
                << "  --crop X,Y,W,H                 Recorte em pixels fisicos; omitido = quadro inteiro\n"
                << "  --snapshot <arquivo.png>       Salva um quadro\n"
                << "  --overlay-snapshot <png>       Sobrepoe uma janela magenta e salva o quadro capturado\n"
                << "  --record <arquivo.mp4>         Grava o recorte via FFmpeg\n"
                << "  --seconds <N>                  Duracao da gravacao (padrao: 30)\n"
                << "  --ffmpeg <ffmpeg.exe>          Caminho do FFmpeg\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Argumento desconhecido: " + WideToUtf8(std::wstring(argument)));
        }
    }
    if (!options.record.empty() && options.seconds <= 0) {
        options.seconds = 30;
    }
    return options;
}

IDirect3DDevice CreateWinrtDirect3DDevice(winrt::com_ptr<ID3D11Device>& d3d_device,
                                          winrt::com_ptr<ID3D11DeviceContext>& d3d_context) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL selected_level{};
    static constexpr D3D_FEATURE_LEVEL feature_levels[]{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    winrt::check_hresult(D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        feature_levels,
        static_cast<UINT>(std::size(feature_levels)),
        D3D11_SDK_VERSION,
        d3d_device.put(),
        &selected_level,
        d3d_context.put()));

    auto dxgi_device = d3d_device.as<IDXGIDevice>();
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put()));
    return inspectable.as<IDirect3DDevice>();
}

GraphicsCaptureItem CreateCaptureItemForWindow(HWND hwnd) {
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForWindow(hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}

winrt::com_ptr<ID3D11Texture2D> GetTextureFromSurface(
    const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface& surface) {
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
    return texture;
}

void SavePng(const fs::path& path, const FrameBuffer& frame) {
    if (frame.width == 0 || frame.height == 0 || frame.bgra.empty()) {
        throw std::runtime_error("Nao ha pixels para salvar.");
    }
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path());
    }

    winrt::com_ptr<IWICImagingFactory> factory;
    winrt::check_hresult(CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.put())));

    winrt::com_ptr<IWICStream> stream;
    winrt::check_hresult(factory->CreateStream(stream.put()));
    winrt::check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));

    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));

    winrt::com_ptr<IWICBitmapFrameEncode> frame_encode;
    winrt::com_ptr<IPropertyBag2> properties;
    winrt::check_hresult(encoder->CreateNewFrame(frame_encode.put(), properties.put()));
    winrt::check_hresult(frame_encode->Initialize(properties.get()));
    winrt::check_hresult(frame_encode->SetSize(frame.width, frame.height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    winrt::check_hresult(frame_encode->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat32bppBGRA) {
        throw std::runtime_error("O encoder PNG nao aceitou BGRA 32 bpp.");
    }
    const UINT stride = frame.width * 4U;
    winrt::check_hresult(frame_encode->WritePixels(
        frame.height,
        stride,
        static_cast<UINT>(frame.bgra.size()),
        const_cast<BYTE*>(frame.bgra.data())));
    winrt::check_hresult(frame_encode->Commit());
    winrt::check_hresult(encoder->Commit());
}

class WindowCapture {
public:
    WindowCapture(HWND hwnd, CropRect crop)
        : hwnd_(hwnd), crop_(crop) {
        if (!IsWindow(hwnd_)) {
            throw std::runtime_error("HWND invalido.");
        }
    }

    ~WindowCapture() {
        Stop();
    }

    void Start() {
        if (!GraphicsCaptureSession::IsSupported()) {
            throw std::runtime_error("Windows.Graphics.Capture nao e suportado nesta execucao.");
        }

        winrt_device_ = CreateWinrtDirect3DDevice(d3d_device_, d3d_context_);
        item_ = CreateCaptureItemForWindow(hwnd_);
        const auto size = item_.Size();
        if (size.Width <= 0 || size.Height <= 0) {
            throw std::runtime_error("A janela selecionada nao possui dimensoes capturaveis.");
        }

        pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrt_device_,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            3,
            size);
        session_ = pool_.CreateCaptureSession(item_);

        border_property_supported_ = ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired");
        if (border_property_supported_) {
            border_required_ = session_.IsBorderRequired();
        }

        frame_token_ = pool_.FrameArrived([this](const auto& sender, const auto&) {
            OnFrame(sender);
        });
        session_.StartCapture();
        started_.store(true);
    }

    void Stop() noexcept {
        if (!started_.exchange(false)) {
            return;
        }
        try {
            if (pool_ && frame_token_.value != 0) {
                pool_.FrameArrived(frame_token_);
                frame_token_ = {};
            }
            if (session_) {
                session_.Close();
            }
            if (pool_) {
                pool_.Close();
            }
        } catch (...) {
        }
        cv_.notify_all();
    }

    FrameBuffer WaitForFrame(std::chrono::milliseconds timeout, std::uint64_t after_sequence = 0) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [&] {
                return latest_.sequence > after_sequence || !last_error_.empty() || !started_.load();
            })) {
            throw std::runtime_error("Tempo excedido aguardando quadro da captura.");
        }
        if (!last_error_.empty()) {
            throw std::runtime_error(last_error_);
        }
        if (latest_.sequence <= after_sequence) {
            throw std::runtime_error("A captura foi encerrada antes de receber um novo quadro.");
        }
        return latest_;
    }

    FrameBuffer LatestFrame() const {
        std::scoped_lock lock(mutex_);
        return latest_;
    }

    bool BorderPropertySupported() const noexcept {
        return border_property_supported_;
    }

    bool BorderRequired() const noexcept {
        return border_required_;
    }

private:
    void OnFrame(const Direct3D11CaptureFramePool& sender) noexcept {
        try {
            auto frame = sender.TryGetNextFrame();
            if (!frame) {
                return;
            }
            const auto content_size = frame.ContentSize();
            if (content_size.Width <= 0 || content_size.Height <= 0) {
                return;
            }

            auto source = GetTextureFromSurface(frame.Surface());
            D3D11_TEXTURE2D_DESC source_desc{};
            source->GetDesc(&source_desc);

            const auto content_width = static_cast<std::uint32_t>(content_size.Width);
            const auto content_height = static_cast<std::uint32_t>(content_size.Height);
            CropRect effective = crop_;
            if (effective.IsFullFrame()) {
                effective = CropRect{0, 0, content_width, content_height};
            }

            if (effective.x >= content_width || effective.y >= content_height ||
                effective.width > content_width - effective.x ||
                effective.height > content_height - effective.y) {
                std::ostringstream message;
                message << "Recorte fora do quadro. Quadro=" << content_width << "x" << content_height
                        << ", crop=" << effective.x << ',' << effective.y << ','
                        << effective.width << ',' << effective.height;
                throw std::runtime_error(message.str());
            }

            EnsureStagingTexture(effective.width, effective.height, source_desc.Format);

            const D3D11_BOX box{
                effective.x,
                effective.y,
                0,
                effective.x + effective.width,
                effective.y + effective.height,
                1,
            };
            d3d_context_->CopySubresourceRegion(staging_.get(), 0, 0, 0, 0, source.get(), 0, &box);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(d3d_context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped));

            FrameBuffer next;
            next.width = effective.width;
            next.height = effective.height;
            const std::size_t row_bytes = static_cast<std::size_t>(effective.width) * 4U;
            next.bgra.resize(row_bytes * effective.height);
            const auto* source_row = static_cast<const std::uint8_t*>(mapped.pData);
            auto* destination_row = next.bgra.data();
            for (std::uint32_t y = 0; y < effective.height; ++y) {
                std::copy_n(source_row, row_bytes, destination_row);
                source_row += mapped.RowPitch;
                destination_row += row_bytes;
            }
            d3d_context_->Unmap(staging_.get(), 0);

            {
                std::scoped_lock lock(mutex_);
                next.sequence = latest_.sequence + 1;
                latest_ = std::move(next);
            }
            cv_.notify_all();
        } catch (const winrt::hresult_error& error) {
            SetError("Falha no callback de captura: " + WideToUtf8(error.message().c_str()));
        } catch (const std::exception& error) {
            SetError(std::string("Falha no callback de captura: ") + error.what());
        } catch (...) {
            SetError("Falha desconhecida no callback de captura.");
        }
    }

    void EnsureStagingTexture(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format) {
        if (staging_ && staging_width_ == width && staging_height_ == height && staging_format_ == format) {
            return;
        }
        staging_ = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::check_hresult(d3d_device_->CreateTexture2D(&desc, nullptr, staging_.put()));
        staging_width_ = width;
        staging_height_ = height;
        staging_format_ = format;
    }

    void SetError(std::string error) noexcept {
        {
            std::scoped_lock lock(mutex_);
            if (last_error_.empty()) {
                last_error_ = std::move(error);
            }
        }
        cv_.notify_all();
    }

    HWND hwnd_{};
    CropRect crop_{};
    winrt::com_ptr<ID3D11Device> d3d_device_;
    winrt::com_ptr<ID3D11DeviceContext> d3d_context_;
    IDirect3DDevice winrt_device_{nullptr};
    GraphicsCaptureItem item_{nullptr};
    Direct3D11CaptureFramePool pool_{nullptr};
    GraphicsCaptureSession session_{nullptr};
    winrt::event_token frame_token_{};
    winrt::com_ptr<ID3D11Texture2D> staging_;
    std::uint32_t staging_width_{};
    std::uint32_t staging_height_{};
    DXGI_FORMAT staging_format_{DXGI_FORMAT_UNKNOWN};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    FrameBuffer latest_;
    std::string last_error_;
    std::atomic_bool started_{false};
    bool border_property_supported_{};
    bool border_required_{};
};

class FfmpegProcess {
public:
    FfmpegProcess(const fs::path& ffmpeg, const fs::path& output, std::uint32_t width, std::uint32_t height) {
        if (!fs::is_regular_file(ffmpeg)) {
            throw std::runtime_error("FFmpeg nao encontrado: " + WideToUtf8(ffmpeg.wstring()));
        }
        if (!output.parent_path().empty()) {
            fs::create_directories(output.parent_path());
        }

        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        if (!CreatePipe(&stdin_read_, &stdin_write_, &security, 1U << 20U)) {
            throw std::runtime_error("CreatePipe falhou.");
        }
        if (!SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0)) {
            CloseHandles();
            throw std::runtime_error("SetHandleInformation falhou.");
        }

        std::wstringstream command;
        command << QuoteCommandArgument(ffmpeg.wstring())
                << L" -hide_banner -loglevel warning -y"
                << L" -f rawvideo -pixel_format bgra -video_size " << width << L'x' << height
                << L" -framerate 30 -i -"
                << L" -vf scale=1080:1920:flags=lanczos,format=yuv420p"
                << L" -an -c:v libx264 -preset veryfast -crf 18 -movflags +faststart "
                << QuoteCommandArgument(output.wstring());

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = stdin_read_;
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        PROCESS_INFORMATION process{};
        std::wstring mutable_command = command.str();
        if (!CreateProcessW(
                nullptr,
                mutable_command.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process)) {
            const DWORD error = GetLastError();
            CloseHandles();
            throw std::runtime_error("Nao foi possivel iniciar FFmpeg. Win32=" + std::to_string(error));
        }

        process_ = process.hProcess;
        CloseHandle(process.hThread);
        CloseHandle(stdin_read_);
        stdin_read_ = nullptr;
    }

    FfmpegProcess(const FfmpegProcess&) = delete;
    FfmpegProcess& operator=(const FfmpegProcess&) = delete;

    ~FfmpegProcess() {
        CloseHandles();
    }

    void WriteFrame(const FrameBuffer& frame) {
        const auto* data = frame.bgra.data();
        std::size_t remaining = frame.bgra.size();
        while (remaining > 0) {
            DWORD written{};
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1U << 20U));
            if (!WriteFile(stdin_write_, data, chunk, &written, nullptr) || written == 0) {
                throw std::runtime_error("Falha escrevendo quadro no pipe do FFmpeg.");
            }
            data += written;
            remaining -= written;
        }
    }

    void Finish() {
        if (stdin_write_ != nullptr) {
            CloseHandle(stdin_write_);
            stdin_write_ = nullptr;
        }
        if (process_ == nullptr) {
            return;
        }
        const DWORD wait = WaitForSingleObject(process_, 30000);
        if (wait != WAIT_OBJECT_0) {
            TerminateProcess(process_, 2);
            WaitForSingleObject(process_, 5000);
            throw std::runtime_error("FFmpeg nao finalizou dentro de 30 segundos.");
        }
        DWORD exit_code{};
        GetExitCodeProcess(process_, &exit_code);
        CloseHandle(process_);
        process_ = nullptr;
        if (exit_code != 0) {
            throw std::runtime_error("FFmpeg terminou com codigo " + std::to_string(exit_code));
        }
    }

private:
    void CloseHandles() noexcept {
        if (stdin_read_ != nullptr) {
            CloseHandle(stdin_read_);
            stdin_read_ = nullptr;
        }
        if (stdin_write_ != nullptr) {
            CloseHandle(stdin_write_);
            stdin_write_ = nullptr;
        }
        if (process_ != nullptr) {
            CloseHandle(process_);
            process_ = nullptr;
        }
    }

    HANDLE stdin_read_{};
    HANDLE stdin_write_{};
    HANDLE process_{};
};

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH brush = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

HWND CreateOverlayWindow(HWND source, CropRect crop) {
    static constexpr wchar_t kClassName[] = L"YourotsCapturePocOverlay";
    static std::once_flag register_once;
    std::call_once(register_once, [] {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = OverlayProc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = kClassName;
        if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("Falha registrando janela de sobreposicao.");
        }
    });

    RECT client{};
    if (!GetClientRect(source, &client)) {
        throw std::runtime_error("GetClientRect falhou para o teste de sobreposicao.");
    }
    POINT origin{client.left, client.top};
    if (!ClientToScreen(source, &origin)) {
        throw std::runtime_error("ClientToScreen falhou para o teste de sobreposicao.");
    }

    constexpr int width = 240;
    constexpr int height = 240;
    const LONG client_width = client.right - client.left;
    const LONG client_height = client.bottom - client.top;
    const LONG center_x = crop.IsFullFrame()
        ? client_width / 2
        : static_cast<LONG>(crop.x + crop.width / 2U);
    const LONG center_y = crop.IsFullFrame()
        ? client_height / 2
        : static_cast<LONG>(crop.y + crop.height / 2U);
    const int x = origin.x + static_cast<int>(std::clamp<LONG>(center_x - width / 2, 0, std::max<LONG>(0, client_width - width)));
    const int y = origin.y + static_cast<int>(std::clamp<LONG>(center_y - height / 2, 0, std::max<LONG>(0, client_height - height)));

    HWND overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClassName,
        L"Overlay probe",
        WS_POPUP,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (overlay == nullptr) {
        throw std::runtime_error("Falha criando janela de sobreposicao.");
    }
    ShowWindow(overlay, SW_SHOWNOACTIVATE);
    UpdateWindow(overlay);
    SetWindowPos(overlay, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    return overlay;
}

std::size_t CountMagentaPixels(const FrameBuffer& frame) {
    std::size_t count{};
    for (std::size_t index = 0; index + 3 < frame.bgra.size(); index += 4) {
        const auto b = frame.bgra[index];
        const auto g = frame.bgra[index + 1];
        const auto r = frame.bgra[index + 2];
        if (r > 245 && g < 10 && b > 245) {
            ++count;
        }
    }
    return count;
}

void Record(WindowCapture& capture, const Options& options, const FrameBuffer& initial) {
    if (options.seconds <= 0) {
        throw std::runtime_error("Duracao invalida para gravacao.");
    }
    FfmpegProcess ffmpeg(options.ffmpeg, options.record, initial.width, initial.height);

    const int frame_count = options.seconds * 30;
    auto deadline = std::chrono::steady_clock::now();
    FrameBuffer current = initial;
    for (int index = 0; index < frame_count; ++index) {
        deadline += std::chrono::microseconds(33333);
        FrameBuffer latest = capture.LatestFrame();
        if (!latest.bgra.empty()) {
            current = std::move(latest);
        }
        ffmpeg.WriteFrame(current);
        std::this_thread::sleep_until(deadline);
        if ((index + 1) % 150 == 0 || index + 1 == frame_count) {
            std::cout << "record_progress=" << (index + 1) << '/' << frame_count << '\n';
        }
    }
    ffmpeg.Finish();
}

void PrintCaptureDiagnostics(HWND hwnd, const WindowCapture& capture, const FrameBuffer& frame) {
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    std::cout
        << "graphics_capture_supported=true\n"
        << "hwnd=" << reinterpret_cast<std::uintptr_t>(hwnd) << '\n'
        << "process=" << WideToUtf8(GetProcessImageName(hwnd)) << '\n'
        << "title=" << WideToUtf8(GetWindowTitle(hwnd)) << '\n'
        << "window_rect=" << rect.left << ',' << rect.top << ',' << (rect.right - rect.left) << ',' << (rect.bottom - rect.top) << '\n'
        << "captured_frame=" << frame.width << 'x' << frame.height << '\n'
        << "callback_thread=" << "free_threaded_frame_pool" << '\n'
        << "border_property_supported=" << (capture.BorderPropertySupported() ? "true" : "false") << '\n';
    if (capture.BorderPropertySupported()) {
        std::cout << "border_required=" << (capture.BorderRequired() ? "true" : "false") << '\n';
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const Options options = ParseOptions(argc, argv);

        if (options.list_windows) {
            for (const auto& window : EnumerateWindows()) {
                std::cout
                    << reinterpret_cast<std::uintptr_t>(window.hwnd) << '\t'
                    << WideToUtf8(window.process) << '\t'
                    << (window.rect.right - window.rect.left) << 'x' << (window.rect.bottom - window.rect.top) << '\t'
                    << WideToUtf8(window.title) << '\n';
            }
            if (options.snapshot.empty() && options.record.empty() && !options.overlay_probe) {
                return 0;
            }
        }

        HWND hwnd = options.hwnd;
        if (hwnd == nullptr) {
            hwnd = FindDefaultEdgeWindow();
        }
        if (hwnd == nullptr) {
            throw std::runtime_error("Nenhuma janela visivel do Microsoft Edge foi encontrada.");
        }

        if (!GraphicsCaptureSession::IsSupported()) {
            std::cout << "graphics_capture_supported=false\n";
            return 3;
        }

        WindowCapture capture(hwnd, options.crop);
        capture.Start();
        FrameBuffer first = capture.WaitForFrame(10s);
        PrintCaptureDiagnostics(hwnd, capture, first);

        if (!options.snapshot.empty()) {
            SavePng(options.snapshot, first);
            std::cout << "snapshot=" << WideToUtf8(fs::absolute(options.snapshot).wstring()) << '\n';
        }

        if (options.overlay_probe) {
            HWND overlay = CreateOverlayWindow(hwnd, options.crop);
            std::this_thread::sleep_for(750ms);
            const FrameBuffer before_overlay_frame = capture.LatestFrame();
            const std::uint64_t before_sequence = before_overlay_frame.sequence;
            FrameBuffer overlay_frame = capture.WaitForFrame(3s, before_sequence);
            if (!options.overlay_snapshot.empty()) {
                SavePng(options.overlay_snapshot, overlay_frame);
                std::cout << "overlay_snapshot=" << WideToUtf8(fs::absolute(options.overlay_snapshot).wstring()) << '\n';
            }
            std::cout << "overlay_magenta_pixels=" << CountMagentaPixels(overlay_frame) << '\n';
            DestroyWindow(overlay);
        }

        if (!options.record.empty()) {
            Record(capture, options, capture.LatestFrame());
            std::cout << "recording=" << WideToUtf8(fs::absolute(options.record).wstring()) << '\n';
        }

        capture.Stop();
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::cerr << "WinRT error 0x" << std::hex << static_cast<std::uint32_t>(error.code().value)
                  << ": " << WideToUtf8(error.message().c_str()) << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Erro: " << error.what() << '\n';
        return 1;
    }
}
