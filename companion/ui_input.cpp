#include <windows.h>
#include <commdlg.h>
#include <string>
#include "ui_input.h"
#include "theme.h"
#include "ui_layout.h"
#include "ui_draw.h"
#include "config.h"
#include "console.h"
#include "injector_ops.h"
#include "shared_memory_manager.h"
#include "cemu_key_injector.h"
#include "string_utils.h"

int g_hoverDropdown = -1;
int g_openDropdown = -1;
float g_dragSlider = -1;

bool g_hoverInject = false, g_hoverReinject = false, g_hoverReset = false, g_hoverToggleCam = false;
bool g_hoverPath = false, g_hoverPathReset = false, g_hoverDarkBtn = false, g_hoverLightBtn = false;
bool g_downPath = false;
bool g_hoverScrollHelper = false, g_hoverOrbitCam = false, g_hoverIndepSens = false, g_hoverIndepMagneSens = false, g_hoverCemuExperimental = false;
bool g_hoverSensH = false, g_hoverSensV = false;
bool g_hoverMagneSens = false, g_hoverMagneSensV = false;
bool g_hoverMagnePullSens = false;
bool g_hoverFpsMagnesis = false, g_hoverFpsMagneEyeHeight = false, g_hoverFpsMagneOffsetForward = false, g_hoverFpsMagneOffsetSide = false;
bool g_hoverClearLog = false;
bool g_hoverLogToFile = false;
Rect g_clearLogRect;

bool g_downInject = false, g_downReinject = false, g_downReset = false, g_downToggleCam = false;

float g_animInject = 0, g_animReinject = 0, g_animReset = 0, g_animToggleCam = 0;
float g_animDarkBtn = 0, g_animLightBtn = 0, g_animPath = 0, g_animPathReset = 0;
float g_animScrollHelper = 0, g_animOrbitCam = 0, g_animIndepSens = 0, g_animIndepMagneSens = 0, g_animCemuExperimental = 0;
float g_animToggleMagneTarget = 0, g_animToggleShortcutMenu = 0, g_animToggleMenuState = 0;
float g_animSensH = 0, g_animSensV = 0, g_animClearLog = 0, g_animLogToFile = 0;
float g_animMagneSens = 0, g_animMagneSensV = 0;
float g_animMagneSpeedBtn[3] = {0, 0, 0};
bool  g_hoverMagneSpeedBtn[3] = {false, false, false};
float g_animMagnePullSens = 0;
float g_animFpsMagnesis = 0, g_animFpsMagneEyeHeight = 0, g_animFpsMagneOffsetForward = 0, g_animFpsMagneOffsetSide = 0;
float g_animDrop[5] = {0, 0, 0, 0, 0};

bool g_trackingMouse = false;
int g_hoverDrop = -1;
int g_hoverDropMenuRow = -1;

// Scrollable dropdown popup: the 18-row list is clamped to the monitor work area at creation
// and the remainder is reached with the mouse wheel. g_dropScroll = first visible row index.
static int g_dropScroll = 0;
static const int DROP_TOTAL_ROWS = 18;
static const int DROP_ROW_H = 18;

static int DropVisibleRows(HWND hWnd) {
    float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
    if (dpiScale <= 0) dpiScale = 1.0f;
    RECT rc;
    GetClientRect(hWnd, &rc);
    int vr = (int)((rc.bottom / dpiScale) / DROP_ROW_H);
    if (vr < 1) vr = 1;
    if (vr > DROP_TOTAL_ROWS) vr = DROP_TOTAL_ROWS;
    return vr;
}

static void DropClampScroll(HWND hWnd) {
    int maxScroll = DROP_TOTAL_ROWS - DropVisibleRows(hWnd);
    if (maxScroll < 0) maxScroll = 0;
    if (g_dropScroll > maxScroll) g_dropScroll = maxScroll;
    if (g_dropScroll < 0) g_dropScroll = 0;
}

LRESULT CALLBACK DropdownPopupWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        g_hoverDropMenuRow = -1;
        g_dropScroll = 0;
        DropClampScroll(hWnd);
        return 0;
    }
    case WM_ACTIVATE:
        if (WA_INACTIVE == LOWORD(wParam)) DestroyWindow(hWnd);
        return 0;
    case WM_MOUSEWHEEL: {
        int notches = ((int)(short)HIWORD(wParam)) / WHEEL_DELTA;
        if (notches != 0) {
            g_dropScroll -= notches * 3; // 3 rows per wheel notch
            DropClampScroll(hWnd);
            g_hoverDropMenuRow = -1;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = (int)(short)LOWORD(lParam);
        int y = (int)(short)HIWORD(lParam);
        float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
        if (dpiScale <= 0) dpiScale = 1.0f;
        x = (int)(x / dpiScale);
        y = (int)(y / dpiScale);
        int visRow = y / DROP_ROW_H;
        if (visRow >= 0 && visRow < DropVisibleRows(hWnd)) g_hoverDropMenuRow = g_dropScroll + visRow;
        else g_hoverDropMenuRow = -1;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = (int)(short)LOWORD(lParam);
        int y = (int)(short)HIWORD(lParam);
        float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
        if (dpiScale <= 0) dpiScale = 1.0f;
        x = (int)(x / dpiScale);
        y = (int)(y / dpiScale);
        int visRow = y / DROP_ROW_H;
        if (visRow >= 0 && visRow < DropVisibleRows(hWnd)) {
            int row = g_dropScroll + visRow;
            if (row >= 0 && row < DROP_TOTAL_ROWS) {
                int dropdownIdx = (int)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
                ButtonItem* buttons = g_ki.is_gamepad ? GAMEPAD_BUTTONS : PRO_BUTTONS;
                g_config.mouse_bindings[dropdownIdx] = buttons[row].val;
                g_ki.mouse_bindings[dropdownIdx] = buttons[row].val;
                SaveConfig();
                WriteConfigToSharedMemory();
            }
        }
        DestroyWindow(hWnd);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        int w = rc.right;
        int h = rc.bottom;
        float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
        if (dpiScale <= 0) dpiScale = 1.0f;
        int logicalW = (int)(w / dpiScale);
        int logicalH = (int)(h / dpiScale);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBmp = SelectObject(memDC, memBitmap);
        Graphics g(memDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.ScaleTransform(dpiScale, dpiScale);

        SolidBrush bgBrush(g_theme.bg);
        g.FillRectangle(&bgBrush, 0, 0, logicalW, logicalH);
        Pen borderPen(g_theme.border);
        g.DrawRectangle(&borderPen, 0, 0, logicalW - 1, logicalH - 1);

        FontFamily ff(L"Segoe UI");
        Font font(&ff, 12, FontStyleRegular, UnitPixel);
        SolidBrush textBrush(g_theme.text);
        ButtonItem* buttons = g_ki.is_gamepad ? GAMEPAD_BUTTONS : PRO_BUTTONS;
        DropClampScroll(hWnd);
        int visible = DropVisibleRows(hWnd);
        for (int v = 0; v < visible; v++) {
            int i = g_dropScroll + v;
            if (i >= DROP_TOTAL_ROWS) break;
            if (g_hoverDropMenuRow == i) {
                SolidBrush hoverBrush(Color(255, 30, 30, 40));
                g.FillRectangle(&hoverBrush, 0, v * DROP_ROW_H, logicalW, DROP_ROW_H);
            }
            std::wstring name = std::wstring(buttons[i].name, buttons[i].name + strlen(buttons[i].name));
            g.DrawString(name.c_str(), -1, &font, PointF(5.0f, (REAL)(v * DROP_ROW_H)), &textBrush);
        }
        // Slim scrollbar when rows are hidden
        if (visible < DROP_TOTAL_ROWS) {
            int trackH = visible * DROP_ROW_H;
            int thumbH = (trackH * visible) / DROP_TOTAL_ROWS;
            if (thumbH < 14) thumbH = 14;
            int maxScroll = DROP_TOTAL_ROWS - visible;
            int thumbY = maxScroll > 0 ? ((trackH - thumbH) * g_dropScroll) / maxScroll : 0;
            SolidBrush trackBrush(Color(60, 255, 255, 255));
            g.FillRectangle(&trackBrush, logicalW - 5, 0, 4, trackH);
            SolidBrush thumbBrush(Color(200, 255, 255, 255));
            g.FillRectangle(&thumbBrush, logicalW - 5, thumbY, 4, thumbH);
        }

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBitmap);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        g_openDropdown = -1;
        HWND parent = GetParent(hWnd);
        if (parent) InvalidateRect(parent, nullptr, FALSE);
        return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

static void DpiScaleClient(HWND hWnd, int& x, int& y, int& logicalW, int& logicalH) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
    if (dpiScale <= 0) dpiScale = 1.0f;
    x = (int)(x / dpiScale);
    y = (int)(y / dpiScale);
    logicalW = (int)(rc.right / dpiScale);
    logicalH = (int)(rc.bottom / dpiScale);
}

LRESULT HandleLButtonDown(HWND hWnd, WPARAM wParam, LPARAM lParam) {
    int x = (int)(short)LOWORD(lParam);
    int y = (int)(short)HIWORD(lParam);
    int logicalW, logicalH;
    DpiScaleClient(hWnd, x, y, logicalW, logicalH);
    UIRects ui;
    CalculateUIRects(ui, logicalW, logicalH);

    if (Rect(ui.rMemPanel.X, ui.rMemPanel.Y, ui.rMemPanel.Width, 30).Contains(x, y)) {
        g_collapsedMem = !g_collapsedMem;
        SaveConfig();
        InvalidateUIRectsCache();
        UpdateConsoleEditPosition(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (Rect(ui.rTelePanel.X, ui.rTelePanel.Y, ui.rTelePanel.Width, 30).Contains(x, y)) {
        g_collapsedTele = !g_collapsedTele;
        SaveConfig();
        InvalidateUIRectsCache();
        UpdateConsoleEditPosition(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (Rect(ui.rSetPanel.X, ui.rSetPanel.Y, ui.rSetPanel.Width, 30).Contains(x, y)) {
        g_collapsedSet = !g_collapsedSet;
        SaveConfig();
        InvalidateUIRectsCache();
        UpdateConsoleEditPosition(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (Rect(ui.rBindPanel.X, ui.rBindPanel.Y, ui.rBindPanel.Width, 30).Contains(x, y)) {
        g_collapsedBind = !g_collapsedBind;
        SaveConfig();
        InvalidateUIRectsCache();
        UpdateConsoleEditPosition(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (!g_collapsedLog && ui.rLogToFile.Contains(x, y)) {
        g_config.log_to_file = !g_config.log_to_file;
        SaveConfig();
        if (g_config.log_to_file) {
            LogToConsole(L"[INFO] Log to file enabled — writing to mousecam.log");
        } else {
            LogToConsole(L"[INFO] Log to file disabled");
        }
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (ui.rClearLog.Contains(x, y)) {
        ClearConsole();
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (Rect(ui.rLog.X, ui.rLog.Y, ui.rLog.Width, 30).Contains(x, y)) {
        g_collapsedLog = !g_collapsedLog;
        SaveConfig();
        InvalidateUIRectsCache();
        UpdateConsoleEditPosition(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (ui.rDarkBtn.Contains(x, y)) {
        g_config.use_light_theme = false;
        ApplyTheme();
        SaveConfig();
        InvalidateUIRectsCache();
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (ui.rLightBtn.Contains(x, y)) {
        g_config.use_light_theme = true;
        ApplyTheme();
        SaveConfig();
        InvalidateUIRectsCache();
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (ui.rInj.Contains(x, y)) {
        g_downInject = true;
        InvalidateRect(hWnd, nullptr, FALSE);
        SetCapture(hWnd);
        return 0;
    }
    if (g_targetInjected && ui.rReinj.Contains(x, y)) {
        g_downReinject = true;
        InvalidateRect(hWnd, nullptr, FALSE);
        SetCapture(hWnd);
        return 0;
    }
    if (g_targetInjected && ui.rRst.Contains(x, y)) {
        g_downReset = true;
        InvalidateRect(hWnd, nullptr, FALSE);
        SetCapture(hWnd);
        return 0;
    }
    if (g_targetInjected && ui.rToggleCam.Contains(x, y)) {
        g_downToggleCam = true;
        InvalidateRect(hWnd, nullptr, FALSE);
        SetCapture(hWnd);
        return 0;
    }
    if (ui.rPath.Contains(x, y)) {
        g_downPath = true;
        InvalidateRect(hWnd, nullptr, FALSE);
        SetCapture(hWnd);
        return 0;
    }
    if (!g_config.cemu_path_override.empty() && ui.rPathReset.Contains(x, y)) {
        g_config.cemu_path_override.clear();
        InvalidateUIRectsCache();
        SaveConfig();
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (ui.rToggleMagneTarget.Contains(x, y)) {
        g_config.scan_magne_target = !g_config.scan_magne_target;
        SaveConfig();
        WriteConfigToSharedMemory();
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (ui.rToggleShortcutMenu.Contains(x, y)) {
        g_config.scan_shortcut_menu = !g_config.scan_shortcut_menu;
        SaveConfig();
        WriteConfigToSharedMemory();
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (ui.rToggleMenuState.Contains(x, y)) {
        g_config.scan_menu_state = !g_config.scan_menu_state;
        SaveConfig();
        WriteConfigToSharedMemory();
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (ui.rScrollHelper.Contains(x, y)) {
        g_config.scroll_helper = !g_config.scroll_helper;
        SaveConfig();
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    if (ui.rOrbitCam.Contains(x, y)) {
        g_config.full_orbit_camera = !g_config.full_orbit_camera;
        SaveConfig();
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    if (ui.rCemuExperimental.Contains(x, y)) {
        g_config.cemu_experimental = !g_config.cemu_experimental;
        SaveConfig();
        WriteConfigToSharedMemory();
        // Purged: Cemu mode switch no longer triggers scanner reset
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    if (ui.rIndepSens.Contains(x, y)) {
        g_config.use_independent_sens = !g_config.use_independent_sens;
        SaveConfig();
        InvalidateUIRectsCache();
        UpdateConsoleEditPosition(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    if (ui.rIndepMagneSens.Contains(x, y)) {
        g_config.use_independent_magne_sens = !g_config.use_independent_magne_sens;
        SaveConfig();
        InvalidateUIRectsCache();
        UpdateConsoleEditPosition(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    for (int i = 0; i < 3; ++i) {
        if (ui.rMagneSpeedBtn[i].Contains(x, y)) {
            g_config.magnesis_speed_mode = i;
            SaveConfig();
            WriteConfigToSharedMemory();
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
    }
    if (ui.rFpsMagnesis.Contains(x, y)) {
        g_config.fps_magnesis = !g_config.fps_magnesis;
        SaveConfig();
        InvalidateUIRectsCache();
        WriteConfigToSharedMemory();
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    Rect hBoxH = ui.rSensH;
    hBoxH.Y += 15;
    hBoxH.Height = 24;
    if (hBoxH.Contains(x, y)) {
        g_dragSlider = 0;
        SetCapture(hWnd);
        return 0;
    }
    if (g_config.use_independent_sens) {
        Rect hBoxV = ui.rSensV;
        hBoxV.Y += 15;
        hBoxV.Height = 24;
        if (hBoxV.Contains(x, y)) {
            g_dragSlider = 1;
            SetCapture(hWnd);
            return 0;
        }
    }
    if (!g_collapsedSet) {
        Rect hBoxM = ui.rMagneSens;
        hBoxM.Y += 15;
        hBoxM.Height = 24;
        if (hBoxM.Contains(x, y)) {
            g_dragSlider = 2;
            SetCapture(hWnd);
            return 0;
        }
        if (g_config.use_independent_magne_sens) {
            Rect hBoxMV = ui.rMagneSensV;
            hBoxMV.Y += 15;
            hBoxMV.Height = 24;
            if (hBoxMV.Contains(x, y)) {
                g_dragSlider = 3;
                SetCapture(hWnd);
                return 0;
            }
        }
        Rect hBoxP = ui.rMagnePullSens;
        hBoxP.Y += 15;
        hBoxP.Height = 24;
        if (hBoxP.Contains(x, y)) {
            g_dragSlider = 4;
            SetCapture(hWnd);
            return 0;
        }
        if (g_config.fps_magnesis) {
            Rect hBoxEye = ui.rFpsMagneEyeHeight;
            hBoxEye.Y += 15;
            hBoxEye.Height = 24;
            if (hBoxEye.Contains(x, y)) {
                g_dragSlider = 5;
                SetCapture(hWnd);
                return 0;
            }
            Rect hBoxFwd = ui.rFpsMagneOffsetForward;
            hBoxFwd.Y += 15;
            hBoxFwd.Height = 24;
            if (hBoxFwd.Contains(x, y)) {
                g_dragSlider = 6;
                SetCapture(hWnd);
                return 0;
            }
            Rect hBoxSide = ui.rFpsMagneOffsetSide;
            hBoxSide.Y += 15;
            hBoxSide.Height = 24;
            if (hBoxSide.Contains(x, y)) {
                g_dragSlider = 7;
                SetCapture(hWnd);
                return 0;
            }
        }
    }
    for (int i = 0; i < 5; i++) {
        if (ui.rDrops[i].Contains(x, y)) {
            g_openDropdown = i;
            InvalidateUIRectsCache();
            CalculateUIRects(ui, logicalW, logicalH);
            POINT pt = { (int)(ui.rDropMenu.X * (GetDpiForWindow(hWnd) / 96.0f)), (int)(ui.rDropMenu.Y * (GetDpiForWindow(hWnd) / 96.0f)) };
            ClientToScreen(hWnd, &pt);
            float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
            if (dpiScale <= 0) dpiScale = 1.0f;
            int popupW = (int)(ui.rDropMenu.Width * dpiScale);
            int popupH = (int)(ui.rDropMenu.Height * dpiScale);
            // Fit the list to the screen: clamp to the monitor work area (min 3 rows);
            // hidden rows are reachable via mouse wheel in the popup.
            {
                MONITORINFO mi = { sizeof(mi) };
                if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) {
                    int availPx = mi.rcWork.bottom - pt.y;
                    if (popupH > availPx) {
                        int minH = (int)(3 * DROP_ROW_H * dpiScale);
                        popupH = (availPx > minH) ? availPx : minH;
                    }
                    int availW = mi.rcWork.right - pt.x;
                    if (popupW > availW) popupW = availW;
                }
            }
            HWND hPopup = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"DropdownPopupClass", L"",
                WS_POPUP | WS_VISIBLE, pt.x, pt.y, popupW, popupH,
                hWnd, nullptr, GetModuleHandle(nullptr), (LPVOID)(INT_PTR)i);
            SetFocus(hPopup);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
    }
    return 0;
}

LRESULT HandleLButtonUp(HWND hWnd, WPARAM wParam, LPARAM lParam) {
    int x = (int)(short)LOWORD(lParam);
    int y = (int)(short)HIWORD(lParam);
    int logicalW, logicalH;
    DpiScaleClient(hWnd, x, y, logicalW, logicalH);
    UIRects ui;
    CalculateUIRects(ui, logicalW, logicalH);

    if (g_downPath) {
        g_downPath = false;
        ReleaseCapture();
        if (ui.rPath.Contains(x, y)) {
            OPENFILENAMEW ofn = {0};
            wchar_t szFile[MAX_PATH] = {0};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
            ofn.lpstrFilter = L"Executable Files\0*.exe\0All Files\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.lpstrTitle = L"Select Cemu Executable";
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                g_config.cemu_path_override = WstrToUtf8(ofn.lpstrFile);
                InvalidateUIRectsCache();
                SaveConfig();
            }
        }
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    if (g_dragSlider != -1) {
        g_dragSlider = -1;
        ReleaseCapture();
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    if (g_downInject) {
        g_downInject = false;
        ReleaseCapture();
        DoInjectOrEject();
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    if (g_downReinject) {
        g_downReinject = false;
        ReleaseCapture();
        DoReinject();
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    if (g_downReset) {
        g_downReset = false;
        ReleaseCapture();
        if (g_pSharedMemory) {
            g_pSharedMemory->m_reqResetScan = true;
            g_pSharedMemory->m_reqResetPreserveMenu = false;
            LogToConsole(L"[INFO] Reset requested — full scanner reset.");
        } else {
            LogToConsole(L"[WARNING] Reset failed: shared memory not mapped (not injected).");
        }
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    if (g_downToggleCam) {
        g_downToggleCam = false;
        ReleaseCapture();
        if (ui.rToggleCam.Contains(x, y)) {
            if (g_pSharedMemory) g_pSharedMemory->m_reqToggleMousecam = true;
        }
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    return 0;
}

LRESULT HandleMouseMove(HWND hWnd, WPARAM wParam, LPARAM lParam) {
    int x = (int)(short)LOWORD(lParam);
    int y = (int)(short)HIWORD(lParam);
    int logicalW, logicalH;
    DpiScaleClient(hWnd, x, y, logicalW, logicalH);
    UIRects ui;
    CalculateUIRects(ui, logicalW, logicalH);

    if (!g_trackingMouse) {
        TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);
        g_trackingMouse = true;
    }

    bool needRedraw = false;
    auto checkHov = [&](bool& state, bool cond) {
        if (state != cond) { state = cond; needRedraw = true; }
    };
    auto checkHovI = [&](int& state, int val) {
        if (state != val) { state = val; needRedraw = true; }
    };

    checkHovI(g_hoverDropMenuRow, (g_openDropdown != -1 && ui.rDropMenu.Contains(x, y)) ? (y - ui.rDropMenu.Y) / 18 : -1);
    checkHov(g_hoverInject, ui.rInj.Contains(x, y));
    checkHov(g_hoverReinject, ui.rReinj.Contains(x, y) && g_targetInjected);
    checkHov(g_hoverReset, ui.rRst.Contains(x, y) && g_targetInjected);
    checkHov(g_hoverToggleCam, ui.rToggleCam.Contains(x, y) && g_targetInjected);
    checkHov(g_hoverDarkBtn, ui.rDarkBtn.Contains(x, y));
    checkHov(g_hoverLightBtn, ui.rLightBtn.Contains(x, y));
    checkHov(g_hoverPath, ui.rPath.Contains(x, y));
    checkHov(g_hoverPathReset, !g_config.cemu_path_override.empty() && ui.rPathReset.Contains(x, y));
    checkHov(g_hoverScrollHelper, ui.rScrollHelper.Contains(x, y));
    checkHov(g_hoverOrbitCam, ui.rOrbitCam.Contains(x, y));
    checkHov(g_hoverIndepSens, ui.rIndepSens.Contains(x, y));
    checkHov(g_hoverIndepMagneSens, ui.rIndepMagneSens.Contains(x, y));
    checkHov(g_hoverCemuExperimental, ui.rCemuExperimental.Contains(x, y) && !g_targetInjected);
    checkHov(g_hoverFpsMagnesis, ui.rFpsMagnesis.Contains(x, y));
    Rect hBoxH = ui.rSensH;
    hBoxH.Y += 15;
    hBoxH.Height = 24;
    checkHov(g_hoverSensH, hBoxH.Contains(x, y));
    if (g_config.use_independent_sens) {
        Rect hBoxV = ui.rSensV;
        hBoxV.Y += 15;
        hBoxV.Height = 24;
        checkHov(g_hoverSensV, hBoxV.Contains(x, y));
    } else {
        checkHov(g_hoverSensV, false);
    }
    if (!g_collapsedSet) {
        Rect hBoxM = ui.rMagneSens; hBoxM.Y += 15; hBoxM.Height = 24;
        checkHov(g_hoverMagneSens, hBoxM.Contains(x, y));

        if (g_config.use_independent_magne_sens) {
            Rect hBoxMV = ui.rMagneSensV; hBoxMV.Y += 15; hBoxMV.Height = 24;
            checkHov(g_hoverMagneSensV, hBoxMV.Contains(x, y));
        } else {
            checkHov(g_hoverMagneSensV, false);
        }

        for (int i = 0; i < 3; ++i) {
            checkHov(g_hoverMagneSpeedBtn[i], ui.rMagneSpeedBtn[i].Contains(x, y));
        }
        Rect hBoxP = ui.rMagnePullSens; hBoxP.Y += 15; hBoxP.Height = 24;
        checkHov(g_hoverMagnePullSens, hBoxP.Contains(x, y));

        if (g_config.fps_magnesis) {
            Rect hBoxEye = ui.rFpsMagneEyeHeight; hBoxEye.Y += 15; hBoxEye.Height = 24;
            checkHov(g_hoverFpsMagneEyeHeight, hBoxEye.Contains(x, y));

            Rect hBoxFwd = ui.rFpsMagneOffsetForward; hBoxFwd.Y += 15; hBoxFwd.Height = 24;
            checkHov(g_hoverFpsMagneOffsetForward, hBoxFwd.Contains(x, y));

            Rect hBoxSide = ui.rFpsMagneOffsetSide; hBoxSide.Y += 15; hBoxSide.Height = 24;
            checkHov(g_hoverFpsMagneOffsetSide, hBoxSide.Contains(x, y));
        } else {
            checkHov(g_hoverFpsMagneEyeHeight, false);
            checkHov(g_hoverFpsMagneOffsetForward, false);
            checkHov(g_hoverFpsMagneOffsetSide, false);
        }
    } else {
        checkHov(g_hoverMagneSens, false);
        checkHov(g_hoverMagneSensV, false);
        for (int i = 0; i < 3; ++i) checkHov(g_hoverMagneSpeedBtn[i], false);
        checkHov(g_hoverMagnePullSens, false);
        checkHov(g_hoverFpsMagnesis, false);
        checkHov(g_hoverFpsMagneEyeHeight, false);
        checkHov(g_hoverFpsMagneOffsetForward, false);
        checkHov(g_hoverFpsMagneOffsetSide, false);
    }
    int dropHov = -1;
    for (int i = 0; i < 5; i++) {
        if (ui.rDrops[i].Contains(x, y)) { dropHov = i; break; }
    }
    checkHovI(g_hoverDrop, dropHov);
    checkHov(g_hoverClearLog, ui.rClearLog.Contains(x, y));
    checkHov(g_hoverLogToFile, !g_collapsedLog && ui.rLogToFile.Contains(x, y));

    if (g_dragSlider != -1) {
        int pad = 15;
        int w = ui.rSensH.Width;
        float pct = (float)(x - pad - 10) / w;
        if (pct < 0) pct = 0;
        if (pct > 1) pct = 1;
        if (g_dragSlider == 0) {
            float val = SENS_MIN + pct * (SENS_MAX - SENS_MIN);
            g_config.sensitivity_x = val;
        } else if (g_dragSlider == 1) {
            float val = SENS_MIN + pct * (SENS_MAX - SENS_MIN);
            g_config.sensitivity_y = val;
        } else if (g_dragSlider == 2) {
            float val = MAGNE_SENS_MIN + pct * (MAGNE_SENS_MAX - MAGNE_SENS_MIN);
            g_config.magnesis_sensitivity = val;
        } else if (g_dragSlider == 3) {
            float val = MAGNE_SENS_MIN + pct * (MAGNE_SENS_MAX - MAGNE_SENS_MIN);
            g_config.magnesis_sensitivity_y = val;
        } else if (g_dragSlider == 4) {
            float val = 1.0f + pct * (10.0f - 1.0f);
            g_config.magnesis_pull_sensitivity = val;
        } else if (g_dragSlider == 5) {
            float val = -2.0f + pct * (5.0f - (-2.0f));
            g_config.fps_magne_eye_height = val;
        } else if (g_dragSlider == 6) {
            float val = -5.0f + pct * (5.0f - (-5.0f));
            g_config.fps_magne_offset_forward = val;
        } else if (g_dragSlider == 7) {
            float val = -5.0f + pct * (5.0f - (-5.0f));
            g_config.fps_magne_offset_side = val;
        }
        SaveConfig();
        WriteConfigToSharedMemory();
        needRedraw = true;
    }

    if (!g_tooltipActive || g_tooltipText != nullptr) {
        const wchar_t* tip = nullptr;
        if (ui.rInj.Contains(x, y))
            tip = g_targetInjected ? L"Eject the DLL from the target process" : L"Inject the DLL into the target process";
        else if (ui.rReinj.Contains(x, y) && g_targetInjected)
            tip = L"Re-eject and re-inject to reload settings";
        else if (ui.rRst.Contains(x, y) && g_targetInjected)
            tip = L"Reset AOB scanner to re-scan memory signatures";
        else if (ui.rToggleCam.Contains(x, y) && g_targetInjected)
            tip = L"Toggle mouse camera control on or off (F2)";
        else if (ui.rDarkBtn.Contains(x, y))
            tip = L"Dark theme";
        else if (ui.rLightBtn.Contains(x, y))
            tip = L"Light theme";
        else if (ui.rCemuExperimental.Contains(x, y))
            tip = g_targetInjected ? L"Cemu Experimental Mode (Cannot change settings while injected)" : L"Use AOB patterns and offsets optimized for Cemu Experimental";
        else if (ui.rMagneSpeedBtn[0].Contains(x, y))
            tip = L"Magnesis Speed: Vanilla (original game speed)";
        else if (ui.rMagneSpeedBtn[1].Contains(x, y))
            tip = L"Magnesis Speed: Extended (faster speed)";
        else if (ui.rMagneSpeedBtn[2].Contains(x, y))
            tip = L"Magnesis Speed: Unlimited (no speed limit)";
        else if (ui.rMagneSens.Contains(x, y))
            tip = L"Adjust sensitivity multiplier for Magnesis movement (0.01 to 2.0)";
        else if (ui.rMagneSensV.Contains(x, y))
            tip = L"Adjust vertical sensitivity multiplier for Magnesis movement (0.01 to 2.0)";
        else if (ui.rFpsMagnesis.Contains(x, y))
            tip = L"Enable First-Person Camera view looking directly at Magnesis target";
        else if (!g_collapsedLog && ui.rLogToFile.Contains(x, y))
            tip = L"Also write log to mousecam.log next to the companion exe";
        if (tip) ShowTooltip(hWnd, tip, x, y);
        else if (g_tooltipActive) ShowTooltip(hWnd, nullptr, 0, 0);
    }

    if (needRedraw) InvalidateRect(hWnd, nullptr, FALSE);
    return 0;
}

LRESULT HandleMouseWheel(HWND hWnd, WPARAM wParam, LPARAM lParam) {
    if (g_hConsoleEdit && !g_collapsedLog) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
        if (dpiScale <= 0) dpiScale = 1.0f;
        pt.x = (int)(pt.x / dpiScale);
        pt.y = (int)(pt.y / dpiScale);
        int logicalW = (int)(((RECT{ 0 }) .right) / dpiScale);
        RECT rc; GetClientRect(hWnd, &rc);
        logicalW = (int)(rc.right / dpiScale);
        int logicalH = (int)(rc.bottom / dpiScale);
        UIRects ui;
        CalculateUIRects(ui, logicalW, logicalH);
        if (ui.rLog.Contains(pt.x, pt.y)) {
            SendMessageW(g_hConsoleEdit, WM_MOUSEWHEEL, wParam, lParam);
            return 0;
        }
    }
    return DefWindowProc(hWnd, WM_MOUSEWHEEL, wParam, lParam);
}

LRESULT HandleMouseLeave(HWND hWnd, WPARAM wParam, LPARAM lParam) {
    g_trackingMouse = false;
    g_hoverInject = g_hoverReinject = g_hoverReset = g_hoverToggleCam = false;
    g_hoverDarkBtn = g_hoverLightBtn = false;
    g_hoverPath = g_hoverPathReset = false;
    g_hoverScrollHelper = g_hoverOrbitCam = g_hoverIndepSens = g_hoverCemuExperimental = false;
    g_hoverSensH = g_hoverSensV = g_hoverMagneSens = g_hoverMagneSensV = g_hoverMagnePullSens = g_hoverFpsMagnesis = g_hoverClearLog = g_hoverLogToFile = false;
    for (int i = 0; i < 3; ++i) g_hoverMagneSpeedBtn[i] = false;
    g_hoverFpsMagneEyeHeight = g_hoverFpsMagneOffsetForward = g_hoverFpsMagneOffsetSide = false;
    g_hoverDrop = g_hoverDropMenuRow = -1;
    if (g_tooltipActive) ShowTooltip(hWnd, nullptr, 0, 0);
    InvalidateRect(hWnd, nullptr, FALSE);
    return 0;
}

LRESULT HandleKillFocus(HWND hWnd, WPARAM wParam, LPARAM lParam) {
    g_dragSlider = -1;
    g_downInject = g_downReinject = g_downReset = g_downToggleCam = g_downPath = false;
    ReleaseCapture();
    InvalidateRect(hWnd, nullptr, FALSE);
    return 0;
}

LRESULT HandleSetCursor(HWND hWnd, WPARAM wParam, LPARAM lParam) {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hWnd, &pt);
    float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
    if (dpiScale <= 0) dpiScale = 1.0f;
    pt.x = (int)(pt.x / dpiScale);
    pt.y = (int)(pt.y / dpiScale);
    RECT rc; GetClientRect(hWnd, &rc);
    int logicalW = (int)(rc.right / dpiScale);
    int logicalH = (int)(rc.bottom / dpiScale);
    UIRects ui;
    CalculateUIRects(ui, logicalW, logicalH);

    bool overClickable = false;
    if (Rect(ui.rMemPanel.X, ui.rMemPanel.Y, ui.rMemPanel.Width, 30).Contains(pt.x, pt.y)) overClickable = true;
    else if (Rect(ui.rTelePanel.X, ui.rTelePanel.Y, ui.rTelePanel.Width, 30).Contains(pt.x, pt.y)) overClickable = true;
    else if (Rect(ui.rSetPanel.X, ui.rSetPanel.Y, ui.rSetPanel.Width, 30).Contains(pt.x, pt.y)) overClickable = true;
    else if (Rect(ui.rBindPanel.X, ui.rBindPanel.Y, ui.rBindPanel.Width, 30).Contains(pt.x, pt.y)) overClickable = true;
    else if (Rect(ui.rLog.X, ui.rLog.Y, ui.rLog.Width, 30).Contains(pt.x, pt.y)) overClickable = true;
    else if (ui.rInj.Contains(pt.x, pt.y)) overClickable = true;
    else if (g_targetInjected && ui.rReinj.Contains(pt.x, pt.y)) overClickable = true;
    else if (g_targetInjected && ui.rRst.Contains(pt.x, pt.y)) overClickable = true;
    else if (g_targetInjected && ui.rToggleCam.Contains(pt.x, pt.y)) overClickable = true;
    else if (ui.rDarkBtn.Contains(pt.x, pt.y)) overClickable = true;
    else if (ui.rLightBtn.Contains(pt.x, pt.y)) overClickable = true;
    else if (ui.rPath.Contains(pt.x, pt.y)) overClickable = true;
    else if (!g_config.cemu_path_override.empty() && ui.rPathReset.Contains(pt.x, pt.y)) overClickable = true;
    else if (ui.rScrollHelper.Contains(pt.x, pt.y)) overClickable = true;
    else if (ui.rOrbitCam.Contains(pt.x, pt.y)) overClickable = true;
    else if (ui.rIndepSens.Contains(pt.x, pt.y)) overClickable = true;
    else if (ui.rIndepMagneSens.Contains(pt.x, pt.y)) overClickable = true;
    else if (ui.rMagneSpeedBtn[0].Contains(pt.x, pt.y) || ui.rMagneSpeedBtn[1].Contains(pt.x, pt.y) || ui.rMagneSpeedBtn[2].Contains(pt.x, pt.y)) overClickable = true;
    else if (!g_targetInjected && ui.rCemuExperimental.Contains(pt.x, pt.y)) overClickable = true;
    else if (ui.rClearLog.Contains(pt.x, pt.y)) overClickable = true;
    else if (!g_collapsedLog && ui.rLogToFile.Contains(pt.x, pt.y)) overClickable = true;
    else {
        for (int i = 0; i < 5; i++) {
            if (ui.rDrops[i].Contains(pt.x, pt.y)) { overClickable = true; break; }
        }
    }
    if (overClickable) {
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
    }
    return DefWindowProc(hWnd, WM_SETCURSOR, wParam, lParam);
}

LRESULT HandleGetMinMaxInfo(HWND hWnd, WPARAM wParam, LPARAM lParam) {
    MINMAXINFO* mmi = (MINMAXINFO*)lParam;
    float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
    if (dpiScale <= 0) dpiScale = 1.0f;
    mmi->ptMinTrackSize.x = (int)(WND_W * dpiScale);
    mmi->ptMinTrackSize.y = (int)(WND_H * dpiScale);
    return 0;
}