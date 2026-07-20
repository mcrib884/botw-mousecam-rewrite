// console.cpp — Rich-edit console control, status text, tooltip.
// Extracted verbatim from companion/main.cpp lines 872-940, 942-996, 1435-1449.

#define NOMINMAX
#include <Windows.h>
#include <richedit.h>
#include <commctrl.h>
#include <string>

#include "console.h"
#include "theme.h"      // g_theme for LogToConsole colors
#include "ui_layout.h"  // CalculateUIRects + g_collapsedLog for UpdateConsoleEditPosition

HWND g_hConsoleEdit = nullptr;
std::wstring g_statusText = L"Ready.";
HWND g_hTooltip = nullptr;
const wchar_t* g_tooltipText = nullptr;
bool g_tooltipActive = false;

void ShowTooltip(HWND hWnd, const wchar_t* text, int x, int y) {
    if (!g_hTooltip) return;
    if (g_tooltipActive && g_tooltipText == text) return;
    if (g_tooltipActive) {
        SendMessageW(g_hTooltip, TTM_TRACKACTIVATE, FALSE, 0);
        g_tooltipActive = false;
    }
    if (text) {
        TOOLINFOW ti = {0};
        ti.cbSize = sizeof(ti);
        ti.hwnd = hWnd;
        ti.uId = 1;
        ti.lpszText = (LPWSTR)text;
        SendMessageW(g_hTooltip, TTM_UPDATETIPTEXT, 0, (LPARAM)&ti);
        SendMessageW(g_hTooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(x + 15, y + 15));
        SendMessageW(g_hTooltip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
        g_tooltipActive = true;
        g_tooltipText = text;
    }
}

void SetStatus(const wchar_t* msg) {
    g_statusText = msg;
    if (g_hWnd) InvalidateRect(g_hWnd, nullptr, FALSE);
}

void LogToConsole(const wchar_t* format, ...) {
    if (!g_hConsoleEdit) return;

    wchar_t msg[512];
    va_list args;
    va_start(args, format);
    vswprintf_s(msg, format, args);
    va_end(args);

    int type = 0;
    const wchar_t* msg_content = msg;

    if (wcsncmp(msg, L"[INFO] ", 7) == 0) {
        msg_content = msg + 7;
    } else if (wcsncmp(msg, L"[SUCCESS] ", 10) == 0) {
        type = 1; msg_content = msg + 10;
    } else if (wcsncmp(msg, L"[ERROR] ", 8) == 0) {
        type = 3; msg_content = msg + 8;
    } else if (wcsncmp(msg, L"[WARNING] ", 10) == 0) {
        type = 2; msg_content = msg + 10;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[768];
    swprintf_s(buf, L"%02d:%02d:%02d %s\n", st.wHour, st.wMinute, st.wSecond, msg_content);

    GETTEXTLENGTHEX gtl = { GTL_DEFAULT, 1200 };
    int len = SendMessageW(g_hConsoleEdit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
    SendMessageW(g_hConsoleEdit, EM_SETSEL, len, len);

    CHARFORMATW cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    if (type == 1) cf.crTextColor = RGB(g_theme.success.GetR(), g_theme.success.GetG(), g_theme.success.GetB());
    else if (type == 2) cf.crTextColor = RGB(220, 180, 50);
    else if (type == 3) cf.crTextColor = RGB(g_theme.error.GetR(), g_theme.error.GetG(), g_theme.error.GetB());
    else cf.crTextColor = RGB(g_theme.text.GetR(), g_theme.text.GetG(), g_theme.text.GetB());

    // B6: EM_SETCHARFORMAT + CFM_COLOR is correct for per-line coloring in rich edit.
    // No change needed — this is the standard Win32 approach for colored console output.
    SendMessageW(g_hConsoleEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(g_hConsoleEdit, EM_REPLACESEL, 0, (LPARAM)buf);
    SendMessageW(g_hConsoleEdit, WM_VSCROLL, SB_BOTTOM, 0);
}

void ClearConsole() {
    if (g_hConsoleEdit) {
        SendMessageW(g_hConsoleEdit, EM_SETSEL, 0, -1);
        SendMessageW(g_hConsoleEdit, EM_REPLACESEL, 0, (LPARAM)L"");
    }
}

void UpdateConsoleEditPosition(HWND hWnd) {
    if (!g_hConsoleEdit) return;
    if (g_collapsedLog) {
        ShowWindow(g_hConsoleEdit, SW_HIDE);
        return;
    }
    ShowWindow(g_hConsoleEdit, SW_SHOW);
    RECT rc;
    GetClientRect(hWnd, &rc);
    float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
    if (dpiScale <= 0) dpiScale = 1.0f;
    int w = (int)(rc.right / dpiScale);
    int h = (int)(rc.bottom / dpiScale);
    UIRects ui;
    CalculateUIRects(ui, w, h);
    MoveWindow(g_hConsoleEdit,
               (int)((ui.rLog.X + 5) * dpiScale),
               (int)((ui.rLog.Y + 30) * dpiScale),
               (int)((ui.rLog.Width - 10) * dpiScale),
               (int)((ui.rLog.Height - 35) * dpiScale),
               TRUE);
}