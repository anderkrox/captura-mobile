#include "recorder.h"
#include <winrt/Windows.Foundation.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace {
void Require(bool value) { if (!value) { throw std::runtime_error("Lifecycle contract violated"); } }
template <typename Action> void Reject(Action action) {
    bool failed{}; try { action(); } catch (const std::exception&) { failed = true; } Require(failed);
}
struct CaptureStopGuard {
    std::shared_ptr<yourots::WindowCapture> capture;
    ~CaptureStopGuard() { capture->Stop(); }
};
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 6) { throw std::runtime_error("Expected HWND FFmpeg FFprobe output mode"); }
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(std::stoull(argv[1])));
        const fs::path output = argv[4]; const std::wstring mode = argv[5];
        auto capture = std::make_shared<yourots::WindowCapture>(hwnd);
        const CaptureStopGuard stop_guard{capture};
        const yourots::RecorderOptions options{argv[2],argv[3],output,{57,50,486,864},30,1};
        yourots::Recorder recorder(capture,options);
        Require(!recorder.IsRecording() && recorder.LastError().empty());
        Reject([&] { recorder.Stop(); });
        Reject([&] { recorder.Pause(); });
        Reject([&] { recorder.Resume(); });
        Require(!recorder.IsPaused() && recorder.RecordedDurationSeconds() == 0);
        Reject([&] { recorder.Start(); }); // No initial preview; same object must be retryable.
        Require(!recorder.IsRecording());
        capture->Start();
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!capture->LatestFrame()) {
            if (!capture->LastError().empty()) { throw std::runtime_error(capture->LastError()); }
            Require(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(20ms);
        }
        if (mode == L"destructor") {
            { yourots::Recorder abandoned(capture,options); abandoned.Start(); std::this_thread::sleep_for(600ms); }
            Require(!fs::exists(output));
            auto mkv = output; mkv.replace_extension(L".recording.mkv"); Require(fs::file_size(mkv) > 0);
            std::cout << "destructor_finished=true\n"; return 0;
        }
        if (mode == L"recovery") {
            recorder.Start();
            const auto error_deadline = std::chrono::steady_clock::now() + 5s;
            while (recorder.LastError().empty()) {
                Require(std::chrono::steady_clock::now() < error_deadline); std::this_thread::sleep_for(10ms);
            }
            Reject([&] { recorder.Stop(); }); Require(!recorder.IsRecording());
            SetEnvironmentVariableW(L"YOUROTS_TEST_TOOL_MODE",L"cpu");
        }
        const int cycles = mode == L"cycles" ? 2 : 1;
        for (int i = 1; i <= cycles; ++i) {
            recorder.Start(); Require(recorder.IsRecording());
            Reject([&] { recorder.Start(); }); Require(recorder.IsRecording());
            std::this_thread::sleep_for(1100ms);
            const auto result = recorder.Stop();
            Require(!recorder.IsRecording() && recorder.LastError().empty());
            Require(result.frames_written >= 33 && result.frames_written <= 37);
            Require(!fs::exists(result.temporary_mkv_path));
            auto copy = output; copy.replace_extension(L".cycle" + std::to_wstring(i) + L".mp4");
            fs::copy_file(output,copy,fs::copy_options::overwrite_existing);
            Reject([&] { recorder.Start(); }); // Export already exists and must remain intact.
            Require(!recorder.IsRecording() && fs::file_size(output) == fs::file_size(copy));
            // A subsequent session must never overwrite the previous export.
            if (i < cycles) fs::remove(output);
            std::cout << "cycle" << i << "_frames=" << result.frames_written << '\n'
                      << "cycle" << i << "_overflows=" << result.queue_overflows << '\n'
                      << "cycle" << i << "_encoder=" << (result.encoder == L"libx264" ? "libx264" : "h264_nvenc") << '\n';
            if (mode == L"pressure") { Require(result.queue_overflows > 0); }
            Reject([&] { recorder.Stop(); });
        }
        capture->Stop(); std::cout << "lifecycle_verified=true\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "Erro: " << error.what() << '\n'; return 1;
    }
}
