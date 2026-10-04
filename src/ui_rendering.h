#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace yourots {

// Publish the completed paint in one blit; reuse the bitmap between frames.
class PaintBuffer {
public:
    ~PaintBuffer();
    PaintBuffer() = default;
    PaintBuffer(const PaintBuffer&) = delete;
    PaintBuffer& operator=(const PaintBuffer&) = delete;

    bool Paint(HDC target, const RECT& bounds, const std::function<void(HDC)>& draw);

private:
    bool EnsureSize(HDC target, int width, int height);
    void Reset() noexcept;

    HDC dc_{};
    HBITMAP bitmap_{};
    HGDIOBJ original_bitmap_{};
    int width_{};
    int height_{};
};

// Sending WM_SETTEXT/WM_ENABLE again can repaint even unchanged controls.
bool SetWindowTextIfChanged(HWND control, const std::wstring& text);
void SetWindowEnabledIfChanged(HWND control, bool enabled);

} // namespace yourots
