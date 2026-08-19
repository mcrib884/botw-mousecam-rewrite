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
#include "string_utils.h"  // P3-5: Utf8ToWstr for proper narrow->wide

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
bool g_hoverCopyLog = false;
Rect g_clearLogRect;

bool g_downInject = false, g_downReinject = false, g_downReset = false, g_downToggleCam = false;

float g_animInject = 0, g_animReinject = 0, g_animReset = 0, g_animToggleCam = 0;
float g_animDarkBtn = 0, g_animLightBtn = 0, g_animPath = 0, g_animPathReset = 0;
float g_animScrollHelper = 0, g_animOrbitCam = 0, g_animIndepSens = 0, g_animIndepMagneSens = 0, g_animCemuExperimental = 0;
float g_animSensH = 0, g_animSensV = 0, g_animClearLog = 0, g_animCopyLog = 0;
float g_animMagneSens = 0, g_animMagneSensV = 0;
float g_animMagnePullSens = 0;
float g_animFpsMagnesis = 0, g_animFpsMagneEyeHeight = 0, g_animFpsMagneOffsetForward = 0, g_animFpsMagneOffsetSide = 0;
float g_animDrop[5] = {0, 0, 0, 0, 0};

bool g_trackingMouse = false;
int g_hoverDrop = -1;
int g_hoverDropMenuRow = -1;

LRESULT CALLBACK DropdownPopupWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // P3-12: Snapshot is_gamepad at popup creation so a profile-change (or any
    // other mutation of g_ki.is_gamepad) while the popup is open doesn't cause
    // the row the user clicks to silently map to a different button than they
    // saw when they opened the popup. The snapshot lives for the popup's
    // lifetime; a subsequent open re-snapshots.
    static bool s_popupIsGamepad = false;
    switch (message) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        s_popupIsGamepad = g_ki.is_gamepad;
        g_hoverDropMenuRow = -1;
        return 0;
    }
    case WM_ACTIVATE:
        if (WA_INACTIVE == LOWORD(wParam)) DestroyWindow(hWnd);
        return 0;
    case WM_MOUSEMOVE: {
        int x = (int)(short)LOWORD(lParam);
        int y = (int)(short)HIWORD(lParam);
        float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
        if (dpiScale <= 0) dpiScale = 1.0f;
        x = (int)(x / dpiScale);
        y = (int)(y / dpiScale);
        int row = y / 18;
        if (row >= 0 && row < 18) g_hoverDropMenuRow = row;
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
        int row = y / 18;
        if (row >= 0 && row < 18) {
            int dropdownIdx = (int)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
            // P3-12: use the snapshotted is_gamepad rather than the live one.
            ButtonItem* buttons = s_popupIsGamepad ? GAMEPAD_BUTTONS : PRO_BUTTONS;
            g_config.mouse_bindings[dropdownIdx] = buttons[row].val;
            g_ki.mouse_bindings[dropdownIdx] = buttons[row].val;
            SaveConfig();
            WriteConfigToSharedMemory();
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
        // P2-3: cache the dropdown popup's row font across paints (the popup
        // can repaint at 30-60 Hz while the mouse hovers over rows).
        static Font s_font(&ff, 12, FontStyleRegular, UnitPixel);
        SolidBrush textBrush(g_theme.text);
        // P3-12: use the snapshotted is_gamepad for the whole popup lifetime.
        ButtonItem* buttons = s_popupIsGamepad ? GAMEPAD_BUTTONS : PRO_BUTTONS;
        for (int i = 0; i < 18; i++) {
            if (g_hoverDropMenuRow == i) {
                SolidBrush hoverBrush(Color(255, 30, 30, 40));
                g.FillRectangle(&hoverBrush, 0, i * 18, logicalW, 18);
            }
            // P3-5: proper UTF-8 -> wide instead of byte-widening cast.
            std::wstring name = Utf8ToWstr(std::string(buttons[i].name));
            g.DrawString(name.c_str(), -1, &s_font, PointF(5.0f, (REAL)(i * 18)), &textBrush);
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
    if (ui.rClearLog.Contains(x, y)) {
        ClearConsole();
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (ui.rCopyLog.Contains(x, y)) {
        // AD-1: Copy entire log to clipboard.
        if (g_hConsoleEdit) {
            if (g_consoleIsRichEdit) {
                SendMessageW(g_hConsoleEdit, EM_SETSEL, 0, -1);
                SendMessageW(g_hConsoleEdit, WM_COPY, 0, 0);
                SendMessageW(g_hConsoleEdit, EM_SETSEL, -1, -1);
            } else {
                SendMessageW(g_hConsoleEdit, EM_SETSEL, 0,
                    (LPARAM)SendMessageW(g_hConsoleEdit, WM_GETTEXTLENGTH, 0, 0));
                SendMessageW(g_hConsoleEdit, WM_COPY, 0, 0);
                SendMessageW(g_hConsoleEdit, EM_SETSEL, -1, -1);
            }
            LogToConsole(L"[INFO] Log copied to clipboard.");
        }
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
    // Phase 5: 7 individual theme preset buttons. Clicking any one directly
    // selects that preset.
    for (int i = 0; i < 7; ++i) {
        if (ui.rThemeBtns[i].Contains(x, y)) {
            g_config.theme_preset = i;
            g_config.use_light_theme = (i == 1);
            g_animTheme = -1.0f;
            ApplyTheme();
            SaveConfig();
            InvalidateUIRectsCache();
            InvalidateRect(hWnd, nullptr, FALSE);
            wchar_t buf[64];
            swprintf_s(buf, L"Theme: %s", GetThemePresetName(i));
            SetStatus(buf);
            return 0;
        }
    }

    if (ui.rDarkBtn.Contains(x, y)) {
        // Phase 5: Dark button cycles through dark presets (skip light=1).
        static const int darkPresets[] = {0, 2, 3, 4, 5, 6};
        int idx = 0;
        for (int i = 0; i < 6; ++i) if (darkPresets[i] == g_config.theme_preset) { idx = i; break; }
        g_config.theme_preset = darkPresets[(idx + 1) % 6];
        g_config.use_light_theme = false;
        g_animTheme = -1.0f;
        ApplyTheme();
        SaveConfig();
        InvalidateUIRectsCache();
        InvalidateRect(hWnd, nullptr, FALSE);
        // Show a brief status toast
        wchar_t buf[64];
        swprintf_s(buf, L"Theme: %s", GetThemePresetName(g_config.theme_preset));
        SetStatus(buf);
        return 0;
    }
    if (ui.rLightBtn.Contains(x, y)) {
        // Phase 5: Light button jumps to light preset.
        g_config.theme_preset = 1;
        g_config.use_light_theme = true;
        g_animTheme = -1.0f;
        ApplyTheme();
        SaveConfig();
        InvalidateUIRectsCache();
        InvalidateRect(hWnd, nullptr, FALSE);
        SetStatus(L"Theme: Light");
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
    // P3-10: Add `return 0;` to each toggle handler. The toggle rects don't
    // overlap today, but if any layout change introduces an overlap, the
    // current fall-through would silently fire both toggles on a single click.
    // Each toggle is a complete action — early return is the correct semantic.
    if (ui.rScrollHelper.Contains(x, y)) {
        g_config.scroll_helper = !g_config.scroll_helper;
        SaveConfig();
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (ui.rOrbitCam.Contains(x, y)) {
        g_config.full_orbit_camera = !g_config.full_orbit_camera;
        SaveConfig();
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (ui.rCemuExperimental.Contains(x, y) && !g_targetInjected) {
        g_config.cemu_experimental = !g_config.cemu_experimental;
        SaveConfig();
        WriteConfigToSharedMemory();
        if (g_pSharedMemory) {
            g_pSharedMemory->m_reqResetScan = true;
            LogToConsole(L"[INFO] Switched Cemu mode. Triggering scanner reset...");
        }
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (ui.rIndepSens.Contains(x, y)) {
        g_config.use_independent_sens = !g_config.use_independent_sens;
        SaveConfig();
        InvalidateUIRectsCache();
        UpdateConsoleEditPosition(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (ui.rIndepMagneSens.Contains(x, y)) {
        g_config.use_independent_magne_sens = !g_config.use_independent_magne_sens;
        SaveConfig();
        InvalidateUIRectsCache();
        UpdateConsoleEditPosition(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (ui.rMagneSpeedMode.Contains(x, y)) {
        // IM-10: Direct-segment click instead of round-robin cycling. Three
        // segments (Vanilla | Extended | Unlimited) side by side; the click
        // X offset maps to segment 0/1/2.
        int segW = (ui.rMagneSpeedMode.Width - 4) / 3;
        int relX = x - (ui.rMagneSpeedMode.X + 2);
        if (relX < 0) relX = 0;
        int segIdx = relX / (segW + 2);
        if (segIdx < 0) segIdx = 0;
        if (segIdx > 2) segIdx = 2;
        g_config.magnesis_speed_mode = segIdx;
        SaveConfig();
        WriteConfigToSharedMemory();
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    if (ui.rFpsMagnesis.Contains(x, y)) {
        g_config.fps_magnesis = !g_config.fps_magnesis;
        SaveConfig();
        InvalidateUIRectsCache();
        WriteConfigToSharedMemory();
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
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
            // P2-4: Use a much larger buffer than MAX_PATH (260). Long Cemu
            // installations inside layered Steam libraries / OneDrive folders
            // can easily exceed 260 chars and silently truncate via
            // FNERR_BUFFERTOOSMALL — the user clicked OK and nothing happened.
            // 32K is the extended-length path cap on Windows; we size just
            // under to be safe. If the picker returns < 0 we log it.
            OPENFILENAMEW ofn = {0};
            constexpr size_t kPathBufChars = 32768;
            std::wstring szFile(kPathBufChars, L'\0');
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFile = &szFile[0];
            ofn.nMaxFile = (DWORD)kPathBufChars;
            ofn.lpstrFilter = L"Executable Files\0*.exe\0All Files\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.lpstrTitle = L"Select Cemu Executable";
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_LONGNAMES;
            if (GetOpenFileNameW(&ofn)) {
                g_config.cemu_path_override = WstrToUtf8(ofn.lpstrFile);
                InvalidateUIRectsCache();
                SaveConfig();
            } else {
                // P2-4: Surface the FNERR_BUFFERTOOSMALL / cancel cases so the
                // user isn't left wondering why nothing happened.
                DWORD err = CommDlgExtendedError();
                if (err == FNERR_BUFFERTOOSMALL) {
                    LogToConsole(L"[ERROR] Selected path exceeded %u chars (max supported).", (unsigned)kPathBufChars);
                }
                // FNERR_BUFFERTOOSMALL == 0x3003, CDERR_DIALOGFAILURE == 0, etc.
                // On user cancel (no error), CommDlgExtendedError returns 0 — we
                // don't log that, it's an expected action.
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
        if (g_pSharedMemory) g_pSharedMemory->m_reqResetScan = true;
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
    checkHov(g_hoverCopyLog, ui.rCopyLog.Contains(x, y));

    if (g_dragSlider != -1) {
        int pad = 15;
        int w = ui.rSensH.Width;
        // Guard against Settings being collapsed mid-drag (g_collapsedSet true =>
        // CalculateUIRects zeroes rSensH => w == 0). Also guards against impl NaN:
        // once pct is NaN, the subsequent clamp `pct < 0` / `pct > 1` returns
        // false for all comparisons (IEEE 754), so NaN would propagate into
        // g_config and into the saved JSON file as "nan".
        if (w <= 0) return 0;
        float pct = (float)(x - pad - 10) / w;
        if (pct < 0) pct = 0;
        if (pct > 1) pct = 1;
        if (pct != pct) return 0; // NaN guard (defensive)
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

    // P3-3: Removed dead outer tooltip guard. The condition
    // `(!g_tooltipActive || g_tooltipText != nullptr)` was always true because
    // ShowTooltip sets g_tooltipText to a non-null pointer when activating. We
    // always enter the block now and let ShowTooltip's early-out handle the
    // no-op case (it already does, see ShowTooltip line 2 in console.cpp).
    {
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
            tip = L"Cycle theme (Dark/Nord/Solarized/Catppuccin/Gruvbox/Tokyo Night)";
        else if (ui.rLightBtn.Contains(x, y))
            tip = L"Switch to Light theme";
        else if (ui.rCemuExperimental.Contains(x, y))
            tip = g_targetInjected ? L"Cemu Experimental Mode (Cannot change settings while injected)" : L"Use AOB patterns and offsets optimized for Cemu Experimental";
        else if (ui.rMagneSpeedMode.Contains(x, y))
            tip = L"Magnesis object speed limit: Vanilla (default) | Extended (faster) | Unlimited (no limit)";
        else if (ui.rMagneSens.Contains(x, y))
            tip = L"Adjust sensitivity multiplier for Magnesis movement (0.01 to 2.0)";
        else if (ui.rMagneSensV.Contains(x, y))
            tip = L"Adjust vertical sensitivity multiplier for Magnesis movement (0.01 to 2.0)";
        else if (ui.rFpsMagnesis.Contains(x, y))
            tip = L"Enable First-Person Camera view looking directly at Magnesis target";
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
        // P3-2: Removed the dead `RECT{0}.right` computation that was
        // immediately overwritten by the real GetClientRect call below.
        RECT rc; GetClientRect(hWnd, &rc);
        int logicalW = (int)(rc.right / dpiScale);
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
    g_hoverSensH = g_hoverSensV = g_hoverMagneSens = g_hoverMagnePullSens = g_hoverFpsMagnesis = g_hoverClearLog = g_hoverCopyLog = false;
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
    else if (ui.rMagneSpeedMode.Contains(pt.x, pt.y)) overClickable = true;
    else if (!g_targetInjected && ui.rCemuExperimental.Contains(pt.x, pt.y)) overClickable = true;
    else if (ui.rClearLog.Contains(pt.x, pt.y)) overClickable = true;
    else if (ui.rCopyLog.Contains(pt.x, pt.y)) overClickable = true;
    else {
        for (int i = 0; i < 7; ++i) if (ui.rThemeBtns[i].Contains(pt.x, pt.y)) { overClickable = true; break; }
        if (!overClickable) {
            for (int i = 0; i < 5; i++) {
                if (ui.rDrops[i].Contains(pt.x, pt.y)) { overClickable = true; break; }
            }
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