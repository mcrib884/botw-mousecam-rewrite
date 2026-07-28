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
// P2-1: True when g_hConsoleEdit is an actual MSFTEDIT/RICHEDIT class. False
// when we fell back to a standard EDIT control (msftedit.dll missing). The
// flag gates which window messages we can send — plain EDIT supports
// WM_GETTEXTLENGTH/EM_GETSEL/EM_REPLACESEL/WM_VSCROLL, but NOT
// EM_EXGETSEL/EM_GETSCROLLPOS/EM_SETSCROLLPOS/EM_GETTEXTLENGTHEX.
bool g_consoleIsRichEdit = false;

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

static WNDPROC g_oldConsoleEditProc = nullptr;

static HMENU g_consoleContextMenu = nullptr;

static LRESULT CALLBACK ConsoleEditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // P3-11: Real right-click context menu with Copy / Select All / Clear.
    // Previously the subclass silently copied on right-click only when a
    // selection was already present, and did nothing otherwise — no menu,
    // no feedback. Standard Windows rich-edit UX expects a context menu.
    if (msg == WM_RBUTTONUP || msg == WM_CONTEXTMENU) {
        // Allow default rich-edit context menu behavior on systems where
        // SetWindowLongPtrW(GWLP_WNDPROC) was bypassed (rare on plain EDIT
        // fallback where there's no built-in menu to suppress), but on a
        // real MSFTEDIT the default "Copy" menu interlocks with our option
        // set, so we render our own. Show at cursor position.
        if (!g_consoleContextMenu) {
            g_consoleContextMenu = CreatePopupMenu();
            AppendMenuW(g_consoleContextMenu, MF_STRING, 1, L"Copy\tCtrl+C");
            AppendMenuW(g_consoleContextMenu, MF_STRING, 2, L"Select All");
            AppendMenuW(g_consoleContextMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(g_consoleContextMenu, MF_STRING, 3, L"Clear Log");
        }

        // Anchor at cursor for WM_RBUTTONUP, at (LOWORD(lParam), HIWORD(lParam)) for WM_CONTEXTMENU.
        POINT pt;
        if (msg == WM_RBUTTONUP) {
            pt.x = (short)LOWORD(lParam);
            pt.y = (short)HIWORD(lParam);
            ClientToScreen(hWnd, &pt);
        } else {
            pt.x = (short)LOWORD(lParam);
            pt.y = (short)HIWORD(lParam);
            // lParam for WM_CONTEXTMENU is already screen coords when sent by Windows
            if (pt.x == -1 && pt.y == -1) {
                // Shift+F10 / context menu key — anchor at caret.
                GetCaretPos(&pt);
                ClientToScreen(hWnd, &pt);
            }
        }

        // Pre-check Copy enabled state.
        CHARRANGE selRange = {0, 0};
        SendMessageW(hWnd, EM_EXGETSEL, 0, (LPARAM)&selRange);
        if (selRange.cpMin == selRange.cpMax) {
            EnableMenuItem(g_consoleContextMenu, 1, MF_BYCOMMAND | MF_GRAYED);
        } else {
            EnableMenuItem(g_consoleContextMenu, 1, MF_BYCOMMAND | MF_ENABLED);
        }

        int sel = (int)TrackPopupMenuEx(g_consoleContextMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                        pt.x, pt.y, hWnd, nullptr);
        switch (sel) {
            case 1: {  // Copy
                SendMessageW(hWnd, WM_COPY, 0, 0);
                break;
            }
            case 2: {  // Select All
                if (g_consoleIsRichEdit) {
                    SendMessageW(hWnd, EM_SETSEL, 0, -1);
                } else {
                    SendMessageW(hWnd, EM_SETSEL, 0, (LPARAM)SendMessageW(hWnd, WM_GETTEXTLENGTH, 0, 0));
                }
                break;
            }
            case 3: {  // Clear Log
                ClearConsole();
                break;
            }
            default:
                // 0 = user dismissed menu; do nothing.
                break;
        }
        return 0;
    }
    // Keys: also handle Ctrl+C / Ctrl+A via the subclass so plain EDIT
    // fallback gets them too (richedit handles these natively).
    if (msg == WM_KEYDOWN) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && (wParam == 'C' || wParam == 'c')) {
            SendMessageW(hWnd, WM_COPY, 0, 0);
            return 0;
        }
        if (ctrl && (wParam == 'A' || wParam == 'a')) {
            if (g_consoleIsRichEdit) {
                SendMessageW(hWnd, EM_SETSEL, 0, -1);
            } else {
                SendMessageW(hWnd, EM_SETSEL, 0, (LPARAM)SendMessageW(hWnd, WM_GETTEXTLENGTH, 0, 0));
            }
            return 0;
        }
    }
    return CallWindowProcW(g_oldConsoleEditProc, hWnd, msg, wParam, lParam);
}

void SetStatus(const wchar_t* msg) {
    g_statusText = msg;
    if (g_hWnd) InvalidateRect(g_hWnd, nullptr, FALSE);
}

void LogToConsole(const wchar_t* format, ...) {
    if (!g_hConsoleEdit) return;

    wchar_t msg[512] = {};
    va_list args;
    va_start(args, format);
    // vswprintf_s returns negative on truncation/overflow and may NOT null-terminate
    // in the release CRT. We init msg to all-zeros above so the buffer is guaranteed
    // null-terminated regardless of the function's error behavior, and we treat any
    // non-positive return as truncation (the preceding wcsncmp/swprintf_s would
    // otherwise read uninitialized data and the final swprintf_s could also fail
    // and leave buf un-terminated -> EM_REPLACESEL overrun).
    int written = vswprintf_s(msg, format, args);
    va_end(args);
    if (written < 0) {
        msg[sizeof(msg) / sizeof(msg[0]) - 1] = L'\0';
    }

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
    wchar_t buf[768] = {};
    // Same hardening as above: zero-init the buffer and treat a non-positive
    // return as truncation. msg_content is at most 511 chars by construction
    // (its source buffer is 512 and bounded above), plus 8 chars for the
    // "HH:MM:SS " prefix and 1 for newline — well under 768 — but be defensive.
    int written2 = swprintf_s(buf, L"%02d:%02d:%02d %s\n", st.wHour, st.wMinute, st.wSecond, msg_content);
    if (written2 < 0) {
        buf[sizeof(buf) / sizeof(buf[0]) - 1] = L'\0';
    }

    if (g_consoleIsRichEdit) {
        // ---- Rich-edit path (msftedit.dll loaded) ----
        // Smart auto-scroll check: only scroll to bottom if scrollbar is already at or near bottom
        SCROLLINFO si = { sizeof(SCROLLINFO), SIF_ALL };
        BOOL hasScroll = GetScrollInfo(g_hConsoleEdit, SB_VERT, &si);
        bool isAtBottom = true;
        if (hasScroll && si.nMax > 0 && si.nPage > 0) {
            isAtBottom = (si.nPos + (int)si.nPage >= si.nMax - 15);
        }

        POINT scrollPos = { 0, 0 };
        CHARRANGE selRange = { 0, 0 };
        if (!isAtBottom) {
            SendMessageW(g_hConsoleEdit, EM_GETSCROLLPOS, 0, (LPARAM)&scrollPos);
            SendMessageW(g_hConsoleEdit, EM_EXGETSEL, 0, (LPARAM)&selRange);
        }

        GETTEXTLENGTHEX gtl = { GTL_DEFAULT, 1200 };
        // Cap total content at ~1 GB chars to keep the EM_SETSEL signed-position math
        // well clear of INT_MAX (~2 GB UTF-16). The rich edit doesn't auto-trim, so
        // an extremely long-running companion with verbose DLL logging could otherwise
        // push content past 2 GB chars and turn `len` negative → EM_SETSEL interprets
        // negative position wrong → log appends at top instead of bottom (see P2-8).
        LONGLONG len = (LONGLONG)SendMessageW(g_hConsoleEdit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
        // Truncate ahead if we ever cross ~1 GB; trim oldest half to keep history readable.
        if (len > 0x40000000LL) {
            // Order-of-magnitude trim: set selection at 0..(len/2), replace with empty,
            // then continue. EM_SETSEL takes signed positions so cast carefully.
            LONGLONG half = len / 2;
            int targetSel = (int)(half > INT_MAX ? INT_MAX : half);
            SendMessageW(g_hConsoleEdit, EM_SETSEL, 0, (LPARAM)targetSel);
            SendMessageW(g_hConsoleEdit, EM_REPLACESEL, FALSE, (LPARAM)L"");
            len = (LONGLONG)SendMessageW(g_hConsoleEdit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
        }
        if (len < 0) len = 0;
        SendMessageW(g_hConsoleEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);


        CHARFORMATW cf;
        ZeroMemory(&cf, sizeof(cf));
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        if (type == 1) cf.crTextColor = RGB(g_theme.success.GetR(), g_theme.success.GetG(), g_theme.success.GetB());
        else if (type == 2) cf.crTextColor = RGB(220, 180, 50);
        else if (type == 3) cf.crTextColor = RGB(g_theme.error.GetR(), g_theme.error.GetG(), g_theme.error.GetB());
        else cf.crTextColor = RGB(g_theme.text.GetR(), g_theme.text.GetG(), g_theme.text.GetB());

        SendMessageW(g_hConsoleEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
        SendMessageW(g_hConsoleEdit, EM_REPLACESEL, 0, (LPARAM)buf);

        if (isAtBottom) {
            SendMessageW(g_hConsoleEdit, WM_VSCROLL, SB_BOTTOM, 0);
        } else {
            SendMessageW(g_hConsoleEdit, EM_EXSETSEL, 0, (LPARAM)&selRange);
            SendMessageW(g_hConsoleEdit, EM_SETSCROLLPOS, 0, (LPARAM)&scrollPos);
        }
    } else {
        // ---- Plain EDIT fallback path (P2-1, msftedit.dll missing) ----
        // We lose colored type-tag rendering, but the user sees visible log text
        // instead of a permanently empty panel.
        DWORD selStart = 0, selEnd = 0;
        SendMessageW(g_hConsoleEdit, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
        LRESULT len = SendMessageW(g_hConsoleEdit, WM_GETTEXTLENGTH, 0, 0);
        if (len < 0) len = 0;

        // Cap total content at ~256 KB chars for the plain EDIT path — plain
        // edits are much slower at replaces and don't have the richedit's
        // optimization. ~256 KB is plenty for typical session logs.
        if (len > 0x40000LL) {
            // Trim oldest half: select [0, len/2) and replace with empty.
            SendMessageW(g_hConsoleEdit, EM_SETSEL, 0, (LPARAM)(len / 2));
            SendMessageW(g_hConsoleEdit, EM_REPLACESEL, FALSE, (LPARAM)L"");
            len = SendMessageW(g_hConsoleEdit, WM_GETTEXTLENGTH, 0, 0);
            if (len < 0) len = 0;
        }

        // Smart auto-scroll: only scroll to bottom if scrollbar is at bottom.
        SCROLLINFO si = { sizeof(SCROLLINFO), SIF_ALL };
        BOOL hasScroll = GetScrollInfo(g_hConsoleEdit, SB_VERT, &si);
        bool isAtBottom = true;
        if (hasScroll && si.nMax > 0 && si.nPage > 0) {
            isAtBottom = (si.nPos + (int)si.nPage >= si.nMax - 15);
        }

        SendMessageW(g_hConsoleEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageW(g_hConsoleEdit, EM_REPLACESEL, 0, (LPARAM)buf);
        if (isAtBottom) {
            SendMessageW(g_hConsoleEdit, WM_VSCROLL, SB_BOTTOM, 0);
        }
    }
}

void ClearConsole() {
    if (g_hConsoleEdit) {
        SendMessageW(g_hConsoleEdit, EM_SETSEL, 0, -1);
        SendMessageW(g_hConsoleEdit, EM_REPLACESEL, 0, (LPARAM)L"");
    }
}

void UpdateConsoleEditPosition(HWND hWnd) {
    if (!g_hConsoleEdit) return;
    if (!g_oldConsoleEditProc) {
        g_oldConsoleEditProc = (WNDPROC)SetWindowLongPtrW(g_hConsoleEdit, GWLP_WNDPROC, (LONG_PTR)ConsoleEditSubclassProc);
    }
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