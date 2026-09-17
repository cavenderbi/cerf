#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include "ford_sync2_media_progress.h"
#include <memory>

namespace {
constexpr wchar_t kClass[] = L"CERF.Sync2MediaProgress";
struct Popup {
    std::function<FordSync2MediaProgress()> read;
    HWND owner = nullptr, time = nullptr, bar = nullptr;
    bool window_owned = false;
};
void Refresh(Popup& popup) {
    const auto status = popup.read();
    SetWindowTextW(popup.time, status.Text().c_str());
    SendMessageW(popup.bar, PBM_SETPOS, status.Position(), 0);
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* popup = reinterpret_cast<Popup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        popup = static_cast<Popup*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(popup));
    }
    if (message == WM_TIMER && popup) { Refresh(*popup); return 0; }
    if ((message == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) ||
        (message == WM_KEYDOWN && wp == VK_ESCAPE) || message == WM_CLOSE) {
        DestroyWindow(hwnd); return 0;
    }
    if (message == WM_NCDESTROY && popup) {
        KillTimer(hwnd, 1);
        RemovePropW(popup->owner, kClass);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (popup->window_owned) delete popup;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
}

void ShowFordSync2MediaProgress(std::function<FordSync2MediaProgress()> read) {
    HWND owner = GetActiveWindow();
    if (!owner) return;
    if (auto existing = static_cast<HWND>(GetPropW(owner, kClass))) {
        SetForegroundWindow(existing); return;
    }
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW cls{}; cls.lpfnWndProc = WindowProc; cls.hInstance = instance;
    cls.lpszClassName = kClass; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    RegisterClassW(&cls);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    HDC dc = GetDC(owner); const int dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(owner, dc);
    auto scale = [dpi](int value) { return MulDiv(value, dpi, 96); };
    POINT point{}; GetCursorPos(&point);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor);
    const int width = scale(244), height = scale(100);
    const int x = std::clamp<int>(point.x - width / 2, monitor.rcWork.left, monitor.rcWork.right - width);
    const int y = std::clamp<int>(point.y - height - scale(8), monitor.rcWork.top, monitor.rcWork.bottom - height);
    auto pending = std::make_unique<Popup>();
    pending->read = std::move(read); pending->owner = owner;
    auto* popup = pending.get();
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"USB media progress",
        WS_POPUP | WS_BORDER, x, y, width, height, owner, nullptr, instance, popup);
    if (!hwnd) return;
    popup->window_owned = true; pending.release();
    SetPropW(owner, kClass, hwnd);
    auto label = [&](const wchar_t* text, int top) {
        HWND child = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
            scale(12), scale(top), scale(218), scale(20), hwnd, nullptr, instance, nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return child;
    };
    label(L"USB", 10);
    popup->time = label(L"--:-- / --:--", 34);
    popup->bar = CreateWindowExW(0, PROGRESS_CLASSW, L"Track progress", WS_CHILD | WS_VISIBLE,
        scale(12), scale(65), scale(218), scale(12), hwnd, nullptr, instance, nullptr);
    SendMessageW(popup->bar, PBM_SETRANGE32, 0, 1000);
    Refresh(*popup); SetTimer(hwnd, 1, 250, nullptr);
    ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd);
}
