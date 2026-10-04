#include "ui_rendering.h"

#include <cstdint>
#include <limits>

namespace yourots {

PaintBuffer::~PaintBuffer() {
    Reset();
}

void PaintBuffer::Reset() noexcept {
    if (dc_) {
        SelectObject(dc_, original_bitmap_);
        DeleteObject(bitmap_);
        DeleteDC(dc_);
    }
    dc_ = nullptr;
    bitmap_ = nullptr;
    original_bitmap_ = nullptr;
    width_ = height_ = 0;
}

bool PaintBuffer::EnsureSize(HDC target, int width, int height) {
    if (dc_ && width_ == width && height_ == height) return true;

    HDC next_dc = CreateCompatibleDC(target);
    if (!next_dc) return false;
    // Use the target, not the initially monochrome memory DC.
    HBITMAP next_bitmap = CreateCompatibleBitmap(target, width, height);
    if (!next_bitmap) {
        DeleteDC(next_dc);
        return false;
    }
    HGDIOBJ previous = SelectObject(next_dc, next_bitmap);
    if (!previous || previous == HGDI_ERROR) {
        DeleteObject(next_bitmap);
        DeleteDC(next_dc);
        return false;
    }

    Reset();
    dc_ = next_dc;
    bitmap_ = next_bitmap;
    original_bitmap_ = previous;
    width_ = width;
    height_ = height;
    return true;
}

bool PaintBuffer::Paint(HDC target, const RECT& bounds, const std::function<void(HDC)>& draw) {
    const auto width = static_cast<std::int64_t>(bounds.right) - bounds.left;
    const auto height = static_cast<std::int64_t>(bounds.bottom) - bounds.top;
    if (!target || !draw || width <= 0 || height <= 0 ||
        width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max() ||
        bounds.left == std::numeric_limits<LONG>::min() ||
        bounds.top == std::numeric_limits<LONG>::min()) return false;
    if (!EnsureSize(target, static_cast<int>(width), static_cast<int>(height))) return false;

    const int saved = SaveDC(dc_);
    if (!saved) return false;
    if (!SetViewportOrgEx(dc_, -bounds.left, -bounds.top, nullptr)) {
        RestoreDC(dc_, saved);
        return false;
    }
    try {
        draw(dc_);
    } catch (...) {
        RestoreDC(dc_, saved);
        throw;
    }
    RestoreDC(dc_, saved);
    return BitBlt(target, bounds.left, bounds.top, width_, height_, dc_, 0, 0, SRCCOPY) != FALSE;
}

bool SetWindowTextIfChanged(HWND control, const std::wstring& text) {
    if (!IsWindow(control)) return false;
    const int length = GetWindowTextLengthW(control);
    if (static_cast<std::size_t>(length) == text.size()) {
        std::wstring current(static_cast<std::size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(control, current.data(), length + 1);
        current.resize(static_cast<std::size_t>(copied));
        if (current == text) return false;
    }
    return SetWindowTextW(control, text.c_str()) != FALSE;
}

void SetWindowEnabledIfChanged(HWND control, bool enabled) {
    if (IsWindow(control) && (IsWindowEnabled(control) != FALSE) != enabled) {
        EnableWindow(control, enabled);
    }
}

} // namespace yourots
