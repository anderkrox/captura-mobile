#include "capture_core.h"
#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {
std::wstring Environment(const wchar_t* name) {
    wchar_t value[32768]{};
    const auto count = GetEnvironmentVariableW(name,value,32768);
    return count > 0 && count < 32768 ? std::wstring(value,count) : std::wstring{};
}
std::wstring Mode() {
    const auto path = Environment(L"YOUROTS_TEST_TOOL_MODE_FILE");
    if (!path.empty()) {
        std::ifstream file{std::filesystem::path(path)};
        std::string value; std::getline(file, value);
        if (!value.empty()) return {value.begin(), value.end()};
    }
    return Environment(L"YOUROTS_TEST_TOOL_MODE");
}
}

// Inject failures/delay around the actual FFmpeg/FFprobe, preserving real video encoding.
int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout),_O_BINARY);
    const auto mode = Mode();
    bool probe{}, nvenc{}, raw{}, remux{}, audio{};
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        probe |= arg == L"-show_entries";
        nvenc |= arg == L"h264_nvenc";
        raw |= arg == L"rawvideo";
        remux |= arg == L"copy";
        audio |= arg == L"a";
    }
    if (mode == L"all_encoders_fail" || nvenc) {
        if (mode == L"verbose_cpu") { std::cerr << std::string(256 * 1024,'x') << '\n'; }
        std::cerr << "Injected encoder probe failure\n"; return 7;
    }
    if (mode == L"encoder_exit" && raw) { std::cerr << "Injected encoder exit\n"; return 9; }
    if (mode == L"remux_fail" && remux) { std::cerr << "Injected remux failure\n"; return 10; }
    if (mode == L"probe_fail" && probe) { std::cerr << "Injected FFprobe failure\n"; return 11; }
    if (mode == L"audio_fail" && probe && audio) { std::cerr << "Injected audio probe failure\n"; return 12; }
    if (mode == L"unexpected_audio" && probe && audio) { std::cout << "0\n"; return 0; }
    if (mode == L"slow_cpu" && raw) { Sleep(900); }
    const auto executable = Environment(probe ? L"YOUROTS_TEST_FFPROBE" : L"YOUROTS_TEST_FFMPEG");
    if (executable.empty()) { std::cerr << "Missing actual media tool\n"; return 20; }
    auto command = yourots::QuoteCommandArgument(executable);
    for (int i = 1; i < argc; ++i) { command += L" " + yourots::QuoteCommandArgument(argv[i]); }
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE read{}, write{};
    if (mode == L"probe_nan" && probe && !audio) {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
        if (!CreatePipe(&read,&write,&security,0)) { return 23; }
        SetHandleInformation(read,HANDLE_FLAG_INHERIT,0);
        startup.hStdOutput = write;
    }
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,
                        nullptr,nullptr,&startup,&process)) {
        if (read) { CloseHandle(read); } if (write) { CloseHandle(write); } return 21;
    }
    CloseHandle(process.hThread);
    if (write) {
        CloseHandle(write);
        std::string output; char buffer[4096]; DWORD count{};
        while (ReadFile(read,buffer,sizeof(buffer),&count,nullptr) && count > 0) { output.append(buffer,count); }
        CloseHandle(read);
        const auto start = output.find("duration=");
        if (start != std::string::npos) {
            const auto end = output.find('\n',start);
            output.replace(start,end == std::string::npos ? output.size()-start : end-start,"duration=nan");
        }
        std::cout << output;
    }
    DWORD wait{};
    if (raw && !probe) {
        const auto deadline = GetTickCount64() + 90000;
        while ((wait = WaitForSingleObject(process.hProcess, 40)) == WAIT_TIMEOUT && GetTickCount64() < deadline) {
            if (Mode() == L"encoder_abort") {
                TerminateProcess(process.hProcess, 9);
                WaitForSingleObject(process.hProcess, 5000);
                CloseHandle(process.hProcess);
                std::cerr << "Injected running encoder exit\n"; return 9;
            }
        }
    } else {
        wait = WaitForSingleObject(process.hProcess,90000);
    }
    if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess,22); WaitForSingleObject(process.hProcess,5000); }
    DWORD code{}; GetExitCodeProcess(process.hProcess,&code); CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
