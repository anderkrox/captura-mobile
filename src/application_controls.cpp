#include "application_controls.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace yourots {

std::wstring ReadPreference(const fs::path& path, const wchar_t* key, std::wstring_view fallback = {}) {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetPrivateProfileStringW(
        L"application", key, std::wstring(fallback).c_str(), buffer.data(),
        static_cast<DWORD>(buffer.size()), path.c_str());
    buffer.resize(length);
    return buffer;
}

std::wstring Trim(std::wstring value) {
    const auto is_space = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    while (!value.empty() && is_space(value.front())) value.erase(value.begin());
    while (!value.empty() && is_space(value.back())) value.pop_back();
    return value;
}

std::optional<HotkeyBinding> ParseHotkey(std::wstring text) {
    text = Trim(std::move(text));
    if (text.empty()) return std::nullopt;

    UINT modifiers = MOD_NOREPEAT;
    UINT vk{};
    std::wstring key_name;
    std::size_t start{};
    while (start <= text.size()) {
        const auto separator = text.find(L'+', start);
        std::wstring token = Trim(text.substr(
            start, separator == std::wstring::npos ? std::wstring::npos : separator - start));
        std::transform(token.begin(), token.end(), token.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towupper(ch));
        });
        if (token == L"CTRL" || token == L"CONTROL") modifiers |= MOD_CONTROL;
        else if (token == L"ALT") modifiers |= MOD_ALT;
        else if (token == L"SHIFT") modifiers |= MOD_SHIFT;
        else if (token == L"WIN" || token == L"WINDOWS") modifiers |= MOD_WIN;
        else {
            if (vk != 0 || token.empty()) return std::nullopt;
            if (token.size() == 1 && ((token[0] >= L'A' && token[0] <= L'Z') ||
                                     (token[0] >= L'0' && token[0] <= L'9'))) {
                vk = static_cast<UINT>(token[0]);
                key_name = token;
            } else if (token.size() >= 2 && token[0] == L'F' &&
                       std::all_of(token.begin() + 1, token.end(), [](wchar_t ch) {
                           return ch >= L'0' && ch <= L'9';
                       })) {
                try {
                    std::size_t consumed{};
                    const int number = std::stoi(token.substr(1), &consumed);
                    if (consumed != token.size() - 1 || number < 1 || number > 24) return std::nullopt;
                    vk = VK_F1 + static_cast<UINT>(number - 1);
                    key_name = L"F" + std::to_wstring(number);
                } catch (...) {
                    return std::nullopt;
                }
            } else {
                return std::nullopt;
            }
        }
        if (separator == std::wstring::npos) break;
        start = separator + 1;
    }
    if (vk == 0) return std::nullopt;

    std::wstring canonical;
    auto append = [&](std::wstring_view part) {
        if (!canonical.empty()) canonical += L"+";
        canonical += part;
    };
    if (modifiers & MOD_CONTROL) append(L"Ctrl");
    if (modifiers & MOD_ALT) append(L"Alt");
    if (modifiers & MOD_SHIFT) append(L"Shift");
    if (modifiers & MOD_WIN) append(L"Win");
    append(key_name);
    return HotkeyBinding{modifiers, vk, canonical};
}

std::array<HotkeyBinding, 3> DefaultHotkeys() {
    return {*ParseHotkey(L"Ctrl+Alt+F9"), *ParseHotkey(L"Ctrl+Alt+F10"), *ParseHotkey(L"Ctrl+Alt+F11")};
}

void SaveApplicationPreferences(const fs::path& path, const ApplicationPreferences& preferences) {
    fs::create_directories(path.parent_path());
    if (!fs::exists(path)) {
        HANDLE file = CreateFileW(
            path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Nao foi possivel criar o arquivo de preferencias.");
        }
        const BYTE bom[]{0xFF, 0xFE};
        DWORD written{};
        const BOOL ok = WriteFile(file, bom, sizeof(bom), &written, nullptr);
        CloseHandle(file);
        if (!ok || written != sizeof(bom)) {
            std::error_code ignored;
            fs::remove(path, ignored);
            throw std::runtime_error("Nao foi possivel inicializar o arquivo de preferencias.");
        }
    }
    const auto write = [&](const wchar_t* key, const std::wstring& value) {
        if (!WritePrivateProfileStringW(L"application", key, value.c_str(), path.c_str())) {
            throw std::runtime_error("Nao foi possivel salvar as preferencias do aplicativo.");
        }
    };
    write(L"outputFolder", preferences.output_folder.wstring());
    write(L"hotkeyStart", preferences.hotkeys[0].text);
    write(L"hotkeyPauseResume", preferences.hotkeys[1].text);
    write(L"hotkeyStop", preferences.hotkeys[2].text);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
}

fs::path UniqueOutputPath(const fs::path& folder, std::wstring_view prefix, const SYSTEMTIME* timestamp) {
    SYSTEMTIME now{};
    if (timestamp) now = *timestamp;
    else GetLocalTime(&now);
    std::wstringstream base;
    base << prefix << L'_' << std::setfill(L'0')
         << std::setw(4) << now.wYear << std::setw(2) << now.wMonth << std::setw(2) << now.wDay
         << L'_' << std::setw(2) << now.wHour << std::setw(2) << now.wMinute << std::setw(2) << now.wSecond;
    for (int suffix = 0; suffix < 10000; ++suffix) {
        std::wstring name = base.str();
        if (suffix > 0) {
            std::wstringstream extra;
            extra << L'_' << std::setw(2) << std::setfill(L'0') << (suffix + 1);
            name += extra.str();
        }
        fs::path output = folder / (name + L".mp4");
        fs::path temporary = output;
        temporary.replace_extension(L".recording.mkv");
        if (!fs::exists(output) && !fs::exists(temporary)) return output;
    }
    throw std::runtime_error("Nao foi possivel gerar um nome de arquivo unico para a gravacao.");
}

std::wstring RecordingStateName(RecordingState state) {
    switch (state) {
    case RecordingState::Ready: return L"Pronto";
    case RecordingState::Recording: return L"Gravando";
    case RecordingState::Paused: return L"Pausado";
    case RecordingState::Finalizing: return L"Finalizando";
    case RecordingState::Error: return L"Erro";
    }
    return L"Desconhecido";
}

std::wstring FormatDuration(double seconds) {
    if (!std::isfinite(seconds) || seconds > static_cast<double>(std::numeric_limits<unsigned long long>::max() / 2)) {
        throw std::invalid_argument("Duracao invalida.");
    }
    const auto total = static_cast<unsigned long long>(std::max(0.0, seconds));
    const auto hours = total / 3600ULL;
    const auto minutes = (total / 60ULL) % 60ULL;
    const auto secs = total % 60ULL;
    std::wstringstream text;
    text << std::setfill(L'0') << std::setw(2) << hours << L':'
         << std::setw(2) << minutes << L':' << std::setw(2) << secs;
    return text.str();
}

bool DistinctHotkeys(const std::array<HotkeyBinding, 3>& bindings) {
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        for (std::size_t j = i + 1; j < bindings.size(); ++j) {
            if ((bindings[i].modifiers & ~MOD_NOREPEAT) == (bindings[j].modifiers & ~MOD_NOREPEAT) &&
                bindings[i].vk == bindings[j].vk) return false;
        }
    }
    return true;
}

ApplicationPreferences LoadApplicationPreferences(const fs::path& path, const fs::path& default_folder) {
    ApplicationPreferences result{default_folder, DefaultHotkeys()};
    const auto folder = ReadPreference(path, L"outputFolder");
    if (!folder.empty()) result.output_folder = folder;
    const wchar_t* keys[]{L"hotkeyStart", L"hotkeyPauseResume", L"hotkeyStop"};
    for (std::size_t i = 0; i < result.hotkeys.size(); ++i) {
        result.hotkeys[i] = ParseHotkey(ReadPreference(path, keys[i], result.hotkeys[i].text)).value_or(result.hotkeys[i]);
    }
    if (!DistinctHotkeys(result.hotkeys)) result.hotkeys = DefaultHotkeys();
    return result;
}

RecordingControls EnabledRecordingControls(RecordingState state, bool valid,
    bool capture, bool source, bool temporary) {
    const bool active = state == RecordingState::Recording;
    const bool paused = state == RecordingState::Paused;
    const bool finalizing = state == RecordingState::Finalizing;
    const bool busy = active || paused || finalizing;
    return {busy, !busy && valid && capture, active, paused && valid && source,
        active || paused, !busy && temporary, !finalizing};
}

} // namespace yourots
