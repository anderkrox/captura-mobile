#include "application_controls.h"
#include "recorder_core.h"

#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
using namespace yourots;
namespace {
void Require(bool condition) { if (!condition) throw std::runtime_error("Contract violated"); }
template<class Action> void Reject(Action action) {
    bool rejected{};
    try { action(); } catch (const std::exception&) { rejected = true; }
    Require(rejected);
}
struct Test { std::string name; std::function<void()> action; };
struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        wchar_t base[MAX_PATH]{};
        Require(GetTempPathW(MAX_PATH, base) > 0);
        path = fs::path(base) / (L"yourots-controls-" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(path);
    }
    ~TemporaryDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};
void Touch(const fs::path& path) { std::ofstream file(path); file << "preserved"; Require(bool(file)); }
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    TemporaryDirectory temporary;
    std::vector<Test> tests;
    for (unsigned i = 1; i <= 24; ++i) {
        tests.push_back({"function_key_" + std::to_string(i), [i] {
            const auto key = ParseHotkey(L" shift + control + alt + f" + std::to_wstring(i) + L" ");
            Require(key && key->vk == VK_F1 + i - 1);
            Require(key->modifiers == (MOD_NOREPEAT | MOD_SHIFT | MOD_CONTROL | MOD_ALT));
            Require(key->text == L"Ctrl+Alt+Shift+F" + std::to_wstring(i));
        }});
    }
    for (const auto token : {L"A", L"Z", L"0", L"9", L"F9", L"F24"}) {
        tests.push_back({"hotkey_roundtrip_" + std::to_string(tests.size()), [token] {
            const auto key = ParseHotkey(L" windows + shift + " + std::wstring(token));
            Require(key && key->text == L"Shift+Win+" + std::wstring(token));
            const auto roundtrip = ParseHotkey(key->text);
            Require(roundtrip && roundtrip->vk == key->vk && roundtrip->modifiers == key->modifiers);
        }});
    }
    const std::vector<std::wstring> invalid{L"", L" \t ", L"Ctrl", L"Ctrl+", L"+F9", L"Ctrl++F9",
        L"F9+", L"Ctrl+F9+F10", L"F0", L"F25", L"F-1", L"F 1", L"F1x", L"F9999999999999999999",
        L"Ctrl+Escape", L"Alt+é", L"Ctrl+🎮", L"Ctrl+AA", L"Hyper+F9", L"F9\nF10"};
    for (std::size_t i = 0; i < invalid.size(); ++i) {
        tests.push_back({"reject_hotkey_" + std::to_string(i), [value = invalid[i]] { Require(!ParseHotkey(value)); }});
    }
    tests.push_back({"default_hotkeys_and_duplicate_aliases", [] {
        const auto defaults = DefaultHotkeys();
        Require(DistinctHotkeys(defaults));
        Require(defaults[0].text == L"Ctrl+Alt+F9" && defaults[1].text == L"Ctrl+Alt+F10" &&
                defaults[2].text == L"Ctrl+Alt+F11");
        auto duplicate = defaults;
        duplicate[1] = *ParseHotkey(L"alt+CONTROL+f9");
        Require(!DistinctHotkeys(duplicate));
        duplicate[1].modifiers &= ~MOD_NOREPEAT;
        Require(!DistinctHotkeys(duplicate));
        duplicate[1] = *ParseHotkey(L"Shift+F9");
        Require(DistinctHotkeys(duplicate));
    }});
    const std::vector<std::pair<double, std::wstring>> durations{{-1, L"00:00:00"}, {0, L"00:00:00"},
        {0.99,L"00:00:00"}, {59.99,L"00:00:59"}, {60,L"00:01:00"}, {3599,L"00:59:59"},
        {3600,L"01:00:00"}, {360001,L"100:00:01"}};
    for (std::size_t i = 0; i < durations.size(); ++i) {
        tests.push_back({"duration_" + std::to_string(i), [value = durations[i]] {
            Require(FormatDuration(value.first) == value.second);
        }});
    }
    for (const auto value : std::vector<double>{NAN, INFINITY, -INFINITY, std::numeric_limits<double>::max()}) {
        tests.push_back({"reject_duration_" + std::to_string(tests.size()), [value] {
            Reject([&] { FormatDuration(value); });
        }});
    }
    const std::vector<std::pair<RecordingState, std::wstring>> states{
        {RecordingState::Ready,L"Pronto"}, {RecordingState::Recording,L"Gravando"},
        {RecordingState::Paused,L"Pausado"}, {RecordingState::Finalizing,L"Finalizando"},
        {RecordingState::Error,L"Erro"}};
    for (const auto& [state, label] : states) {
        tests.push_back({"state_controls_" + std::to_string(static_cast<int>(state)), [state, label] {
            Require(RecordingStateName(state) == label);
            const auto controls = EnabledRecordingControls(state, true, true, true, true);
            const bool idle = state == RecordingState::Ready || state == RecordingState::Error;
            Require(controls.start == idle && controls.recover == idle && controls.busy == !idle);
            Require(controls.pause == (state == RecordingState::Recording));
            Require(controls.resume == (state == RecordingState::Paused));
            Require(controls.stop == (state == RecordingState::Recording || state == RecordingState::Paused));
            Require(controls.apply_hotkeys == (state != RecordingState::Finalizing));
            Require(!EnabledRecordingControls(state, false, true, true, true).start);
            Require(!EnabledRecordingControls(state, true, false, true, true).start);
            Require(!EnabledRecordingControls(state, false, true, true, true).resume);
            Require(!EnabledRecordingControls(state, true, true, false, true).resume);
            Require(!EnabledRecordingControls(state, true, true, true, false).recover);
        }});
    }
    tests.push_back({"preferences_missing_defaults", [&] {
        const auto value = LoadApplicationPreferences(temporary.path / L"absent.ini", L"default 🎮");
        Require(value.output_folder == L"default 🎮" && value.hotkeys[0].text == L"Ctrl+Alt+F9");
    }});
    tests.push_back({"preferences_unicode_roundtrip_and_replacement", [&] {
        const auto path = temporary.path / L"preferências 🎮" / L"preferences.ini";
        ApplicationPreferences expected{temporary.path / L"vídeos com espaços 🎮", DefaultHotkeys()};
        expected.hotkeys[0] = *ParseHotkey(L"Ctrl+Shift+G");
        SaveApplicationPreferences(path, expected);
        const auto bytes = Read(path);
        Require(bytes.size() > 2 && static_cast<unsigned char>(bytes[0]) == 0xff &&
                static_cast<unsigned char>(bytes[1]) == 0xfe);
        auto actual = LoadApplicationPreferences(path, L"unused");
        Require(actual.output_folder == expected.output_folder && actual.hotkeys[0].text == L"Ctrl+Shift+G");
        expected.output_folder = temporary.path / L"novo destino";
        SaveApplicationPreferences(path, expected);
        actual = LoadApplicationPreferences(path, L"unused");
        Require(actual.output_folder == expected.output_folder);
    }});
    tests.push_back({"preferences_invalid_and_duplicate_hotkeys_fall_back", [&] {
        const auto path = temporary.path / L"fallback.ini";
        SaveApplicationPreferences(path, {temporary.path, DefaultHotkeys()});
        Require(WritePrivateProfileStringW(L"application", L"hotkeyStart", L"bad", path.c_str()));
        Require(LoadApplicationPreferences(path, L"default").hotkeys[0].text == L"Ctrl+Alt+F9");
        Require(WritePrivateProfileStringW(L"application", L"hotkeyStart", L"Ctrl+Alt+F10", path.c_str()));
        const auto actual = LoadApplicationPreferences(path, L"default");
        Require(DistinctHotkeys(actual.hotkeys) && actual.hotkeys[0].text == L"Ctrl+Alt+F9");
    }});
    tests.push_back({"preferences_empty_folder_and_failed_write", [&] {
        const auto path = temporary.path / L"empty.ini";
        SaveApplicationPreferences(path, {{}, DefaultHotkeys()});
        Require(LoadApplicationPreferences(path, L"fallback").output_folder == L"fallback");
        const auto blocker = temporary.path / L"blocker";
        Touch(blocker);
        Reject([&] { SaveApplicationPreferences(blocker / L"preferences.ini", {temporary.path, DefaultHotkeys()}); });
        Require(Read(blocker) == "preserved");
    }});
    tests.push_back({"unique_output_preserves_mp4_and_mkv_collisions", [&] {
        SYSTEMTIME timestamp{}; timestamp.wYear=2026; timestamp.wMonth=10; timestamp.wDay=1;
        timestamp.wHour=18; timestamp.wMinute=2; timestamp.wSecond=3;
        const auto folder = temporary.path / L"saída 🎮";
        fs::create_directories(folder);
        const auto first = UniqueOutputPath(folder, L"Yourots", &timestamp);
        Require(first.filename() == L"Yourots_20261001_180203.mp4");
        Touch(first);
        const auto second = UniqueOutputPath(folder, L"Yourots", &timestamp);
        Require(second.filename() == L"Yourots_20261001_180203_02.mp4");
        auto mkv = second; mkv.replace_extension(L".recording.mkv"); Touch(mkv);
        const auto third = UniqueOutputPath(folder, L"Yourots", &timestamp);
        Require(third.filename() == L"Yourots_20261001_180203_03.mp4");
        Require(Read(first) == "preserved" && Read(mkv) == "preserved");
        Require(UniqueOutputPath(folder, L"Yourots_recuperado", &timestamp).filename() ==
                L"Yourots_recuperado_20261001_180203.mp4");
    }});
    const std::vector<std::string> counts{"", " \r\n", "0", "-1", "+1", "1.0", "1x", "N/A",
        "18446744073709551616", "1\n2", "1 2"};
    for (std::size_t i=0; i<counts.size(); ++i) {
        tests.push_back({"reject_recovery_frame_count_" + std::to_string(i), [raw=counts[i]] {
            Reject([&] { detail::ParseRecoveryFrameCount(raw); });
        }});
    }
    tests.push_back({"recovery_frame_count_bounds_and_whitespace", [] {
        Require(detail::ParseRecoveryFrameCount(" \t60\r\n") == 60);
        Require(detail::ParseRecoveryFrameCount("1") == 1);
        Require(detail::ParseRecoveryFrameCount("18446744073709551615") == UINT64_MAX);
    }});
    for (const bool starting : {false,true}) {
        tests.push_back({starting ? "start_space_threshold" : "continue_space_threshold", [starting] {
            const std::uintmax_t limit=(starting ? 64ULL : 32ULL)*1024*1024;
            Reject([&] { detail::ValidateAvailableRecordingSpace(0,starting); });
            Reject([&] { detail::ValidateAvailableRecordingSpace(limit-1,starting); });
            detail::ValidateAvailableRecordingSpace(limit,starting);
            detail::ValidateAvailableRecordingSpace(UINT64_MAX,starting);
        }});
    }
    int failures{}; std::string cases;
    for (const auto& test : tests) {
        cases += "  <testcase name=\"" + test.name + "\">";
        try { test.action(); std::cout << "PASS " << test.name << '\n'; }
        catch (const std::exception& error) {
            ++failures; cases += "<failure message=\"Contract violated\"/>";
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
        cases += "</testcase>\n";
    }
    if (argc>1) {
        const fs::path path=argv[1]; fs::create_directories(path.parent_path());
        std::ofstream report(path);
        report << "<testsuite name=\"application_controls\" tests=\"" << tests.size() << "\" failures=\""
               << failures << "\">\n" << cases << "</testsuite>\n";
        if (!report) return 2;
    }
    std::cout << "tests=" << tests.size() << " failures=" << failures << '\n';
    return failures==0 ? 0 : 1;
}
