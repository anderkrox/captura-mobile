#include "ui_rendering.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace yourots;
namespace {

void Require(bool condition) {
    if (!condition) throw std::runtime_error("Contract violated");
}

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    Require(brush != nullptr);
    const bool filled = FillRect(dc, &rect, brush) != FALSE;
    DeleteObject(brush);
    Require(filled);
}

// Real GDI surfaces and hidden controls, without a GPU, capture or message loop.
struct Surface {
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    RECT bounds{0, 0, 96, 80};

    Surface() {
        dc = CreateCompatibleDC(nullptr);
        Require(dc != nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = bounds.right;
        info.bmiHeader.biHeight = -bounds.bottom;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels{};
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        Require(bitmap != nullptr);
        previous = SelectObject(dc, bitmap);
        Require(previous != nullptr && previous != HGDI_ERROR);
        Fill(dc, bounds, RGB(10, 20, 30));
    }

    ~Surface() {
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
};

struct Control {
    HWND hwnd{};
    WNDPROC original{};
    unsigned text_messages{};
    unsigned enable_messages{};

    Control() {
        hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 100, 30,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(hwnd != nullptr);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Observe)));
        Require(original != nullptr);
    }

    ~Control() {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
        DestroyWindow(hwnd);
    }

    static LRESULT CALLBACK Observe(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto& control = *reinterpret_cast<Control*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_SETTEXT) ++control.text_messages;
        if (message == WM_ENABLE) ++control.enable_messages;
        return CallWindowProcW(control.original, hwnd, message, wparam, lparam);
    }
};

struct Test { std::string name; std::function<void()> action; };

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<Test> tests;
    tests.push_back({"destination_keeps_previous_frame_during_composition", [] {
        Surface target;
        PaintBuffer buffer;
        Require(buffer.Paint(target.dc, target.bounds, [&](HDC dc) {
            Require(dc != target.dc);
            Fill(dc, target.bounds, RGB(0, 0, 0));
            Require(GetPixel(target.dc, 40, 30) == RGB(10, 20, 30));
            Fill(dc, target.bounds, RGB(240, 80, 10));
            Require(GetPixel(target.dc, 40, 30) == RGB(10, 20, 30));
        }));
        Require(GetPixel(target.dc, 40, 30) == RGB(240, 80, 10));
    }});
    tests.push_back({"publishes_background_image_and_overlay_together", [] {
        Surface target;
        PaintBuffer buffer;
        Require(buffer.Paint(target.dc, target.bounds, [&](HDC dc) {
            Fill(dc, target.bounds, RGB(28, 30, 34));
            Fill(dc, {10, 10, 70, 60}, RGB(180, 50, 40));
            Fill(dc, {20, 20, 50, 23}, RGB(0, 220, 120));
        }));
        Require(GetPixel(target.dc, 0, 0) == RGB(28, 30, 34));
        Require(GetPixel(target.dc, 40, 40) == RGB(180, 50, 40));
        Require(GetPixel(target.dc, 30, 21) == RGB(0, 220, 120));
    }});
    tests.push_back({"repainting_removes_previous_crop_and_letterbox_pixels", [] {
        Surface target;
        PaintBuffer buffer;
        Require(buffer.Paint(target.dc, target.bounds, [&](HDC dc) {
            Fill(dc, target.bounds, RGB(28, 30, 34));
            Fill(dc, {10, 10, 70, 70}, RGB(255, 255, 255));
            Fill(dc, {10, 10, 13, 70}, RGB(0, 220, 120));
        }));
        Require(buffer.Paint(target.dc, target.bounds, [&](HDC dc) {
            Fill(dc, target.bounds, RGB(28, 30, 34));
            Fill(dc, {30, 10, 60, 70}, RGB(255, 255, 255));
        }));
        Require(GetPixel(target.dc, 11, 30) == RGB(28, 30, 34));
        Require(GetPixel(target.dc, 25, 30) == RGB(28, 30, 34));
        Require(GetPixel(target.dc, 40, 30) == RGB(255, 255, 255));
    }});
    tests.push_back({"nonzero_bounds_preserve_coordinates_and_surroundings", [] {
        Surface target;
        PaintBuffer buffer;
        const RECT bounds{11, 13, 40, 50};
        Require(buffer.Paint(target.dc, bounds, [&](HDC dc) {
            Fill(dc, bounds, RGB(200, 100, 50));
            Fill(dc, {12, 14, 15, 17}, RGB(80, 60, 40));
        }));
        Require(GetPixel(target.dc, 11, 13) == RGB(200, 100, 50));
        Require(GetPixel(target.dc, 12, 14) == RGB(80, 60, 40));
        Require(GetPixel(target.dc, 39, 49) == RGB(200, 100, 50));
        Require(GetPixel(target.dc, 10, 13) == RGB(10, 20, 30));
        Require(GetPixel(target.dc, 40, 50) == RGB(10, 20, 30));
    }});
    tests.push_back({"negative_bounds_are_clipped_at_destination", [] {
        Surface target;
        PaintBuffer buffer;
        const RECT bounds{-3, -5, 20, 30};
        Require(buffer.Paint(target.dc, bounds, [&](HDC dc) { Fill(dc, bounds, RGB(1, 2, 3)); }));
        Require(GetPixel(target.dc, 0, 0) == RGB(1, 2, 3));
        Require(GetPixel(target.dc, 19, 29) == RGB(1, 2, 3));
        Require(GetPixel(target.dc, 20, 30) == RGB(10, 20, 30));
    }});
    tests.push_back({"presentation_respects_destination_update_region", [] {
        Surface target;
        PaintBuffer buffer;
        const int saved = SaveDC(target.dc);
        Require(IntersectClipRect(target.dc, 15, 17, 40, 42) != ERROR);
        Require(buffer.Paint(target.dc, target.bounds, [&](HDC dc) {
            Fill(dc, target.bounds, RGB(1, 2, 3));
        }));
        RestoreDC(target.dc, saved);
        Require(GetPixel(target.dc, 20, 20) == RGB(1, 2, 3));
        Require(GetPixel(target.dc, 14, 20) == RGB(10, 20, 30));
        Require(GetPixel(target.dc, 40, 42) == RGB(10, 20, 30));
    }});
    tests.push_back({"reuses_bitmap_for_same_size", [] {
        Surface target;
        PaintBuffer buffer;
        HGDIOBJ first{};
        Require(buffer.Paint(target.dc, target.bounds, [&](HDC dc) {
            first = GetCurrentObject(dc, OBJ_BITMAP);
            Fill(dc, target.bounds, RGB(1, 2, 3));
        }));
        for (int i = 0; i < 30; ++i) {
            Require(buffer.Paint(target.dc, target.bounds, [&](HDC dc) {
                Require(GetCurrentObject(dc, OBJ_BITMAP) == first);
                Fill(dc, target.bounds, RGB(4, 5, 6));
            }));
        }
    }});
    tests.push_back({"recreates_bitmap_after_growing_and_shrinking", [] {
        Surface target;
        PaintBuffer buffer;
        for (const RECT bounds : {RECT{0, 0, 20, 30}, target.bounds, RECT{4, 6, 14, 16}}) {
            Require(buffer.Paint(target.dc, bounds, [&](HDC dc) {
                BITMAP bitmap{};
                Require(GetObjectW(GetCurrentObject(dc, OBJ_BITMAP), sizeof(bitmap), &bitmap) != 0);
                Require(bitmap.bmWidth == bounds.right - bounds.left);
                Require(bitmap.bmHeight == bounds.bottom - bounds.top);
                Require(bitmap.bmBitsPixel >= 24);
                Fill(dc, bounds, RGB(220, 110, 55));
            }));
            Require(GetPixel(target.dc, bounds.right - 1, bounds.bottom - 1) == RGB(220, 110, 55));
        }
    }});
    tests.push_back({"restores_drawing_state_between_frames", [] {
        Surface target;
        PaintBuffer buffer;
        int original_mode{};
        COLORREF original_color{};
        Require(buffer.Paint(target.dc, target.bounds, [&](HDC dc) {
            original_mode = GetBkMode(dc);
            original_color = GetTextColor(dc);
            Fill(dc, target.bounds, RGB(1, 2, 3));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(90, 100, 110));
            IntersectClipRect(dc, 5, 5, 10, 10);
            SetViewportOrgEx(dc, 50, 50, nullptr);
        }));
        Require(buffer.Paint(target.dc, target.bounds, [&](HDC dc) {
            Require(GetBkMode(dc) == original_mode);
            Require(GetTextColor(dc) == original_color);
            POINT origin{};
            Require(GetViewportOrgEx(dc, &origin) && origin.x == 0 && origin.y == 0);
            Fill(dc, target.bounds, RGB(4, 5, 6));
        }));
        Require(GetPixel(target.dc, 90, 70) == RGB(4, 5, 6));
    }});
    tests.push_back({"incomplete_paint_is_never_presented_and_next_paint_recovers", [] {
        Surface target;
        PaintBuffer buffer;
        bool rejected{};
        try {
            buffer.Paint(target.dc, target.bounds, [&](HDC dc) {
                Fill(dc, target.bounds, RGB(0, 0, 0));
                SetViewportOrgEx(dc, 50, 50, nullptr);
                throw std::runtime_error("Interrupted frame");
            });
        } catch (const std::runtime_error&) { rejected = true; }
        Require(rejected && GetPixel(target.dc, 40, 30) == RGB(10, 20, 30));
        Require(buffer.Paint(target.dc, target.bounds, [&](HDC dc) {
            Fill(dc, target.bounds, RGB(4, 5, 6));
        }));
        Require(GetPixel(target.dc, 40, 30) == RGB(4, 5, 6));
    }});
    const LONG low = std::numeric_limits<LONG>::min();
    const LONG high = std::numeric_limits<LONG>::max();
    const std::vector<RECT> invalid{
        {0, 0, 0, 10}, {0, 0, 10, 0}, {10, 0, 0, 10}, {0, 10, 10, 0},
        {low, 0, high, 10}, {0, low, 10, high}, {low, 0, low + 10, 10}, {0, low, 10, low + 10},
    };
    for (std::size_t i = 0; i < invalid.size(); ++i) {
        tests.push_back({"reject_invalid_bounds_" + std::to_string(i), [bounds = invalid[i]] {
            Surface target;
            PaintBuffer buffer;
            bool called{};
            Require(!buffer.Paint(target.dc, bounds, [&](HDC) { called = true; }));
            Require(!called && GetPixel(target.dc, 40, 30) == RGB(10, 20, 30));
        }});
    }
    tests.push_back({"reject_missing_target_or_callback", [] {
        Surface target;
        PaintBuffer buffer;
        bool called{};
        Require(!buffer.Paint(nullptr, target.bounds, [&](HDC) { called = true; }));
        Require(!called);
        Require(!buffer.Paint(target.dc, target.bounds, {}));
    }});
    tests.push_back({"releases_gdi_resources_after_resize_and_destruction", [] {
        Surface target;
        const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int i = 0; i < 30; ++i) {
            PaintBuffer buffer;
            for (const RECT bounds : {target.bounds, RECT{0, 0, 20, 30}}) {
                Require(buffer.Paint(target.dc, bounds, [&](HDC dc) { Fill(dc, bounds, RGB(1, 2, 3)); }));
            }
        }
        Require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == before);
    }});
    tests.push_back({"unchanged_text_sends_no_repaint_messages_at_capture_cadence", [] {
        Control control;
        Require(SetWindowTextIfChanged(control.hwnd, L"Estado: Pronto"));
        for (int i = 0; i < 300; ++i) {
            Require(!SetWindowTextIfChanged(control.hwnd, L"Estado: Pronto"));
        }
        Require(control.text_messages == 1);
    }});
    tests.push_back({"changed_text_with_same_length_is_published", [] {
        Control control;
        Require(SetWindowTextIfChanged(control.hwnd, L"00:00:01"));
        Require(SetWindowTextIfChanged(control.hwnd, L"00:00:02"));
        wchar_t text[32]{};
        GetWindowTextW(control.hwnd, text, 32);
        Require(std::wstring(text) == L"00:00:02" && control.text_messages == 2);
    }});
    tests.push_back({"clearing_text_changes_control_only_once", [] {
        Control control;
        Require(!SetWindowTextIfChanged(control.hwnd, L""));
        Require(SetWindowTextIfChanged(control.hwnd, L"Ready"));
        Require(SetWindowTextIfChanged(control.hwnd, L""));
        Require(!SetWindowTextIfChanged(control.hwnd, L""));
        Require(control.text_messages == 2 && GetWindowTextLengthW(control.hwnd) == 0);
    }});
    tests.push_back({"unicode_and_long_text_are_compared_without_truncation", [] {
        Control control;
        std::wstring text = L"Destino: C:\\Gravações 🎮\\" + std::wstring(20000, L'x');
        Require(SetWindowTextIfChanged(control.hwnd, text));
        Require(!SetWindowTextIfChanged(control.hwnd, text));
        text.back() = L'y';
        Require(SetWindowTextIfChanged(control.hwnd, text));
        Require(!SetWindowTextIfChanged(control.hwnd, text));
        Require(control.text_messages == 2);
    }});
    tests.push_back({"external_text_change_is_not_hidden_by_cached_value", [] {
        Control control;
        Require(SetWindowTextIfChanged(control.hwnd, L"Ready"));
        Require(SetWindowTextW(control.hwnd, L"External"));
        Require(SetWindowTextIfChanged(control.hwnd, L"Ready"));
        Require(control.text_messages == 3);
    }});
    tests.push_back({"unchanged_enabled_state_sends_no_repaint_messages", [] {
        Control control;
        for (int i = 0; i < 300; ++i) SetWindowEnabledIfChanged(control.hwnd, true);
        Require(control.enable_messages == 0);
        SetWindowEnabledIfChanged(control.hwnd, false);
        Require(!IsWindowEnabled(control.hwnd) && control.enable_messages == 1);
        for (int i = 0; i < 300; ++i) SetWindowEnabledIfChanged(control.hwnd, false);
        Require(control.enable_messages == 1);
        SetWindowEnabledIfChanged(control.hwnd, true);
        Require(IsWindowEnabled(control.hwnd) && control.enable_messages == 2);
    }});
    tests.push_back({"invalid_controls_are_ignored", [] {
        Require(!SetWindowTextIfChanged(nullptr, L"Ready"));
        SetWindowEnabledIfChanged(nullptr, false);
        HWND destroyed{};
        { Control control; destroyed = control.hwnd; }
        Require(!SetWindowTextIfChanged(destroyed, L"Ready"));
        SetWindowEnabledIfChanged(destroyed, false);
    }});

    int failures{};
    std::string cases;
    for (const auto& test : tests) {
        cases += "  <testcase name=\"" + test.name + "\">";
        try {
            test.action();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            cases += "<failure message=\"Contract violated\"/>";
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
        cases += "</testcase>\n";
    }
    if (argc > 1) {
        const std::filesystem::path path = argv[1];
        std::filesystem::create_directories(path.parent_path());
        std::ofstream report(path);
        report << "<testsuite name=\"ui_rendering\" tests=\"" << tests.size()
               << "\" failures=\"" << failures << "\">\n" << cases << "</testsuite>\n";
        if (!report) return 2;
    }
    std::cout << "tests=" << tests.size() << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
