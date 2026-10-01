#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace yourots {

struct PreviewFrame {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> bgra;
    std::uint64_t sequence{};
};

struct WindowInfo {
    HWND hwnd{};
    std::wstring process;
    std::wstring title;
    std::wstring class_name;
    RECT rect{};
};

std::vector<WindowInfo> EnumerateCaptureWindows(HWND excluded_window = nullptr);
std::wstring GetWindowProcessName(HWND hwnd);
std::wstring GetWindowTitleText(HWND hwnd);
std::wstring GetWindowClassName(HWND hwnd);

class WindowCapture : public std::enable_shared_from_this<WindowCapture> {
public:
    explicit WindowCapture(HWND hwnd);
    ~WindowCapture();

    WindowCapture(const WindowCapture&) = delete;
    WindowCapture& operator=(const WindowCapture&) = delete;

    void Start();
    void Stop() noexcept;
    std::shared_ptr<const PreviewFrame> LatestFrame() const;
    std::string LastError() const;
    HWND SourceWindow() const noexcept { return hwnd_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    HWND hwnd_{};
};

} // namespace yourots
