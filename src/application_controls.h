#pragma once

#include <windows.h>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace yourots {

enum class RecordingState { Ready, Recording, Paused, Finalizing, Error };

struct HotkeyBinding {
    UINT modifiers{};
    UINT vk{};
    std::wstring text;
};

std::optional<HotkeyBinding> ParseHotkey(std::wstring text);
std::array<HotkeyBinding, 3> DefaultHotkeys();
bool DistinctHotkeys(const std::array<HotkeyBinding, 3>& bindings);
std::wstring RecordingStateName(RecordingState state);
std::wstring FormatDuration(double seconds);
std::filesystem::path UniqueOutputPath(const std::filesystem::path& folder,
    std::wstring_view prefix = L"Yourots", const SYSTEMTIME* timestamp = nullptr);

struct ApplicationPreferences {
    std::filesystem::path output_folder;
    std::array<HotkeyBinding, 3> hotkeys;
};
ApplicationPreferences LoadApplicationPreferences(const std::filesystem::path& path,
    const std::filesystem::path& default_folder);
void SaveApplicationPreferences(const std::filesystem::path& path,
    const ApplicationPreferences& preferences);

struct RecordingControls {
    bool busy{}, start{}, pause{}, resume{}, stop{}, recover{}, apply_hotkeys{};
};
RecordingControls EnabledRecordingControls(RecordingState state, bool calibration_valid,
    bool capture_available, bool source_available, bool temporary_available);

} // namespace yourots
