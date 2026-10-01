#include "calibration.h"
#include "capture_preview.h"
#include "recorder.h"

#include <windows.h>

#include <winrt/Windows.Foundation.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

struct Options {
    int seconds{5};
    fs::path output{L"artifacts\\phase3\\phase3-recording.mp4"};
    fs::path ffmpeg;
    fs::path ffprobe;
    HWND hwnd{};
    yourots::CropRect crop{};
    bool crop_set{};
};

// Keep the owner alive while stopping WGC on the control thread, including errors.
struct CaptureStopGuard {
    std::shared_ptr<yourots::WindowCapture> capture;
    ~CaptureStopGuard() { capture->Stop(); }
};

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return "erro Win32 sem mensagem";
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
}

fs::path SettingsPath() {
    std::wstring local_app_data(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local_app_data.data(), static_cast<DWORD>(local_app_data.size()));
    if (length > 0 && length < local_app_data.size()) {
        local_app_data.resize(length);
        return fs::path(local_app_data) / L"YourotsCapture" / L"settings.ini";
    }
    return fs::current_path() / L"settings.ini";
}

fs::path FindTool(const wchar_t* name) {
    wchar_t module[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    const fs::path exe_dir = length > 0 ? fs::path(std::wstring(module, length)).parent_path() : fs::current_path();
    const std::vector<fs::path> candidates{
        exe_dir / L"third_party" / L"ffmpeg" / L"bin" / name,
        exe_dir.parent_path().parent_path() / L"third_party" / L"ffmpeg" / L"bin" / name,
        fs::current_path() / L"third_party" / L"ffmpeg" / L"bin" / name,
    };
    for (const auto& candidate : candidates) {
        if (fs::is_regular_file(candidate)) return candidate;
    }
    return {};
}

Options ParseOptions(int argc, wchar_t** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        auto value = [&]() -> fs::path {
            if (i + 1 >= argc) throw std::runtime_error("Argumento sem valor.");
            return argv[++i];
        };
        if (arg == L"--seconds") {
            result.seconds = yourots::ParseDuration(value().wstring());
        } else if (arg == L"--output") {
            result.output = value();
        } else if (arg == L"--ffmpeg") {
            result.ffmpeg = value();
        } else if (arg == L"--ffprobe") {
            result.ffprobe = value();
        } else if (arg == L"--hwnd") {
            const auto raw = value().wstring();
            std::size_t consumed{};
            if (raw.empty() || raw.front() == L'-' || raw.front() == L'+' || raw.front() == L' ') {
                throw std::runtime_error("HWND invalido.");
            }
            const auto handle = std::stoull(raw, &consumed, 0);
            if (consumed != raw.size() || handle == 0) { throw std::runtime_error("HWND invalido."); }
            result.hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
        } else if (arg == L"--crop") {
            result.crop = yourots::ParseCrop(value().wstring());
            result.crop_set = true;
        } else {
            throw std::runtime_error("Argumento desconhecido: " + WideToUtf8(std::wstring(arg)));
        }
    }
    if (result.ffmpeg.empty()) result.ffmpeg = FindTool(L"ffmpeg.exe");
    if (result.ffprobe.empty()) result.ffprobe = FindTool(L"ffprobe.exe");
    return result;
}

yourots::WindowInfo FindSavedSource(const yourots::Calibration& saved) {
    yourots::WindowInfo match{};
    int count = 0;
    for (const auto& window : yourots::EnumerateCaptureWindows()) {
        if (yourots::SameCalibrationSource(saved, window.process, window.title, window.class_name)) {
            match = window;
            ++count;
        }
    }
    if (count != 1) {
        throw std::runtime_error("A fonte salva precisa corresponder a exatamente uma janela visivel.");
    }
    return match;
}

std::shared_ptr<const yourots::PreviewFrame> WaitForFrame(
    const std::shared_ptr<yourots::WindowCapture>& capture) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto error = capture->LastError(); !error.empty()) {
            throw std::runtime_error(error);
        }
        if (auto frame = capture->LatestFrame(); frame && !frame->bgra.empty()) {
            return frame;
        }
        std::this_thread::sleep_for(20ms);
    }
    throw std::runtime_error("Tempo excedido aguardando o primeiro quadro da captura.");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const auto options = ParseOptions(argc, argv);
        yourots::Calibration calibration;
        yourots::WindowInfo source;
        if (options.hwnd != nullptr || options.crop_set) {
            if (options.hwnd == nullptr || !options.crop_set) {
                throw std::runtime_error("Use --hwnd e --crop juntos ao substituir a calibracao salva.");
            }
            if (!IsWindow(options.hwnd)) {
                throw std::runtime_error("O HWND informado nao corresponde a uma janela valida.");
            }
            source = {
                options.hwnd,
                yourots::GetWindowProcessName(options.hwnd),
                yourots::GetWindowTitleText(options.hwnd),
                yourots::GetWindowClassName(options.hwnd),
                {},
            };
            calibration.exists = true;
            calibration.crop = options.crop;
        } else {
            calibration = yourots::LoadCalibration(SettingsPath());
            if (!calibration.exists) {
                throw std::runtime_error("Nao ha calibracao salva. Use o aplicativo principal ou informe --hwnd e --crop.");
            }
            source = FindSavedSource(calibration);
        }
        auto capture = std::make_shared<yourots::WindowCapture>(source.hwnd);
        const CaptureStopGuard stop_guard{capture};
        capture->Start();
        const auto frame = WaitForFrame(capture);
        if (options.hwnd != nullptr) {
            yourots::ResolveCrop(calibration.crop, frame->width, frame->height);
        } else {
            const auto check = yourots::CheckCalibration(
                calibration, frame->width, frame->height, GetDpiForWindow(source.hwnd));
            if (check != yourots::CalibrationCheck::Valid) {
                capture->Stop();
                throw std::runtime_error("A calibracao salva nao e valida para o quadro/DPI atuais.");
            }
        }

        yourots::Recorder recorder(capture, yourots::RecorderOptions{
            options.ffmpeg,
            options.ffprobe,
            options.output,
            calibration.crop,
            30,
            4,
        });
        recorder.Start();
        std::cout << "recording_started=true" << std::endl;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds);
        while (std::chrono::steady_clock::now() < deadline && recorder.LastError().empty()) {
            std::this_thread::sleep_for(10ms);
        }
        const auto result = recorder.Stop();
        capture->Stop();

        std::cout << "encoder=" << WideToUtf8(result.encoder) << '\n'
                  << "frames=" << result.frames_written << '\n'
                  << "duration=" << result.duration_seconds << '\n'
                  << "source_frames_skipped=" << result.source_frames_skipped << '\n'
                  << "queue_overflows=" << result.queue_overflows << '\n'
                  << "output=" << WideToUtf8(fs::absolute(result.output_path).wstring()) << '\n';
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::cerr << "WinRT: " << WideToUtf8(error.message().c_str()) << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Erro: " << error.what() << '\n';
        return 1;
    }
}
