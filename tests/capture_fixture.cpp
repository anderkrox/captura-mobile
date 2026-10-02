#include <windows.h>

#include <string_view>

namespace {
constexpr wchar_t kClass[] = L"YourotsCaptureE2EFixture";
constexpr int kX = 57, kY = 50, kWidth = 486, kHeight = 864;
bool animated = true;
bool stress = false;
unsigned tick{};

void Fill(HDC dc, int x, int y, int width, int height, COLORREF color) {
    const RECT rect{x, y, x + width, y + height};
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_TIMER:
        if (animated) {
            ++tick;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_APP + 1:
        animated = wparam != 0;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC target = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
        HGDIOBJ previous = SelectObject(dc, bitmap);
        Fill(dc, 0, 0, client.right, client.bottom, RGB(255, 0, 255));
        Fill(dc, kX, kY, kWidth, kHeight, RGB(24, 24, 24));
        if (stress && animated) {
            // A busy, deterministic source exercises CPU encoding and bounded queues.
            // Keep the four corners and white movement marker identical to the normal fixture.
            for (int y = 64; y < kHeight - 64; y += 24) {
                for (int x = 64; x < kWidth - 64; x += 24) {
                    const unsigned value = tick * 29 + x * 17 + y * 31;
                    Fill(dc, kX + x, kY + y, 24, 24,
                         RGB(value % 200, (value * 3) % 200, (value * 7) % 200));
                }
            }
        }
        Fill(dc, kX, kY, 32, 32, RGB(255, 0, 0));
        Fill(dc, kX + kWidth - 32, kY, 32, 32, RGB(0, 255, 0));
        Fill(dc, kX, kY + kHeight - 32, 32, 32, RGB(0, 0, 255));
        Fill(dc, kX + kWidth - 32, kY + kHeight - 32, 32, 32, RGB(255, 255, 0));
        Fill(dc, kX + 70 + static_cast<int>(tick % 60) * 4, kY + 416, 32, 32, RGB(255, 255, 255));
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    animated = std::wstring_view(command).find(L"--static") == std::wstring_view::npos;
    stress = std::wstring_view(command).find(L"--stress") != std::wstring_view::npos;
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kClass;
    if (!RegisterClassW(&window_class)) { return 1; }
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClass,
        L"Yourots capture E2E source", WS_POPUP, 20, 20, 600, 964,
        nullptr, nullptr, instance, nullptr);
    if (!hwnd) { return 2; }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    if (!SetTimer(hwnd, 1, stress ? 16 : 33, nullptr)) { DestroyWindow(hwnd); return 3; }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
