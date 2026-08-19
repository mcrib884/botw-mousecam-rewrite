// main.cpp — thin shell for the Mousecam companion app.
//
// After the modular split, this file only owns:
//   * wWinMain (process boot, GDI+ startup, window class registration, message loop)
//   * WndProc (message dispatcher)
//   * g_hWnd, g_gdiplusToken
//
// All drawing, layout, input handling, config, theme, console, injection, and shared
// memory live in their respective translation units. See AGENTS.md for the build layout.

// -------------------------------------------------------------------
// Architecture rationale (kept verbatim from the pre-split main.cpp)
// -------------------------------------------------------------------
// Why all the global state (g_* pattern):
//   Single single-window single-process Win32 GUI app. A singleton adds ceremony
//   without safety — everything is on the UI thread or on a fixed worker pair.
//   Globals are intentional.
// Why manual JSON:
//   Config is ~10 simple keys. No library keeps the distribution trivial (single static exe).
// Why CemuKeyInjector is duplicated in companion + DLL:
//   Different processes, separate input state, can't share one instance across the boundary.
// Why CRITICAL_SECTION over std::mutex:
//   User-mode spinlock, faster than kernel object for short-held locks.

#define NOMINMAX
#include <Windows.h>
#include <richedit.h>
#include <gdiplus.h>
using namespace Gdiplus;
#include <commctrl.h>
#include <shellscalingapi.h>
#include <string>
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")

#include "string_utils.h"
#include "shared_memory_manager.h"
#include "theme.h"
#include "config.h"
#include "console.h"
#include "cemu_key_injector.h"
#include "injector_ops.h"
#include "ui_layout.h"
#include "ui_draw.h"
#include "ui_paint.h"
#include "ui_input.h"

// Main window handle. Declared `extern HWND g_hWnd` in theme.h for ApplyTheme/DWM dark mode.
HWND g_hWnd = nullptr;

// GDI+ token for GdiplusStartup/GdiplusShutdown. Lives here because startup/shutdown
// happens in wWinMain.
static ULONG_PTR g_gdiplusToken = 0;

// Forward decl of WndProc — registered as the main window class's window procedure.
static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        SetTimer(hWnd, 1, 8, nullptr);
        // UX5: Create tooltip control
        g_hTooltip = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr,
            TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hWnd, nullptr, (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE), nullptr);
        if (g_hTooltip) {
            auto addTool = [&](HWND hwnd, const wchar_t* text) {
                TOOLINFOW ti = {0};
                ti.cbSize = sizeof(ti);
                ti.hwnd = hWnd;
                ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                ti.uId = (UINT_PTR)hwnd;
                ti.lpszText = (LPWSTR)text;
                SendMessageW(g_hTooltip, TTM_ADDTOOL, 0, (LPARAM)&ti);
            };
            // Since all our UI is owner-drawn, we can't use HWND-based tooltips easily.
            // We'll use the TTM_TRACKPOSITION/TTM_TRACKACTIVATE approach instead.
            // For now, just create the tooltip control — a full tooltip implementation
            // would require tracking mouse position and showing tooltip text manually.
            SendMessageW(g_hTooltip, TTM_SETMAXTIPWIDTH, 0, 200);
        }
        return 0;
    }
    case WM_TIMER: {
        g_animInject += (g_hoverInject ? 0.05f : -0.05f);
        g_animReinject += (g_hoverReinject ? 0.05f : -0.05f);
        g_animReset += (g_hoverReset ? 0.05f : -0.05f);
        g_animToggleCam += (g_hoverToggleCam ? 0.05f : -0.05f);
        g_animDarkBtn += (g_hoverDarkBtn ? 0.05f : -0.05f);
        g_animLightBtn += (g_hoverLightBtn ? 0.05f : -0.05f);
        g_animPath += (g_hoverPath ? 0.05f : -0.05f);
        g_animPathReset += (g_hoverPathReset ? 0.05f : -0.05f);
        g_animScrollHelper += (g_config.scroll_helper ? 0.075f : -0.075f);
        g_animOrbitCam += (g_config.full_orbit_camera ? 0.075f : -0.075f);
        g_animIndepSens += (g_config.use_independent_sens ? 0.075f : -0.075f);
        g_animIndepMagneSens += (g_config.use_independent_magne_sens ? 0.075f : -0.075f);
        g_animCemuExperimental += (g_config.cemu_experimental ? 0.075f : -0.075f);
        g_animSensH += (g_hoverSensH ? 0.05f : -0.05f);
        g_animSensV += (g_hoverSensV ? 0.05f : -0.05f);
        g_animMagneSens += (g_hoverMagneSens ? 0.05f : -0.05f);
        g_animMagneSensV += (g_hoverMagneSensV ? 0.05f : -0.05f);
        g_animMagnePullSens += (g_hoverMagnePullSens ? 0.05f : -0.05f);
        g_animFpsMagnesis += (g_config.fps_magnesis ? 0.075f : -0.075f);
        g_animFpsMagneEyeHeight += (g_hoverFpsMagneEyeHeight ? 0.05f : -0.05f);
        g_animFpsMagneOffsetForward += (g_hoverFpsMagneOffsetForward ? 0.05f : -0.05f);
        g_animFpsMagneOffsetSide += (g_hoverFpsMagneOffsetSide ? 0.05f : -0.05f);
        g_animClearLog += (g_hoverClearLog ? 0.05f : -0.05f);
        for (int i = 0; i < 5; ++i) g_animDrop[i] += (g_hoverDrop == i ? 0.05f : -0.05f);

        auto clampF = [](float& val) { if (val < 0) val = 0; if (val > 1) val = 1; };
        clampF(g_animInject); clampF(g_animReinject); clampF(g_animReset); clampF(g_animToggleCam);
        clampF(g_animDarkBtn); clampF(g_animLightBtn);
        clampF(g_animPath); clampF(g_animPathReset);
        clampF(g_animScrollHelper); clampF(g_animOrbitCam);
        clampF(g_animIndepSens); clampF(g_animIndepMagneSens); clampF(g_animCemuExperimental); clampF(g_animSensH); clampF(g_animSensV); clampF(g_animMagneSens); clampF(g_animMagneSensV); clampF(g_animMagnePullSens);
        clampF(g_animFpsMagnesis); clampF(g_animFpsMagneEyeHeight); clampF(g_animFpsMagneOffsetForward); clampF(g_animFpsMagneOffsetSide); clampF(g_animClearLog);
        for (int i = 0; i < 5; ++i) clampF(g_animDrop[i]);

        float targetTheme = g_config.use_light_theme ? 0.0f : 1.0f;
        if (g_animTheme == -1.0f) g_animTheme = targetTheme;
        if (g_animTheme != targetTheme) {
            float step = 0.05f;
            if (targetTheme > g_animTheme) {
                g_animTheme += step;
                if (g_animTheme > targetTheme) g_animTheme = targetTheme;
            } else {
                g_animTheme -= step;
                if (g_animTheme < targetTheme) g_animTheme = targetTheme;
            }
            ApplyTheme();
        }

        UpdateUiState();
        WriteConfigToSharedMemory();
        UpdateTelemetryGui();
        // Only invalidate if there's a reason (or hover animations if implemented later)
        static float lastFocX = 0, lastPosX = 0, lastCamFOV = 0;
        static int32_t lastShortcut = -1; static uint8_t lastMenu = 1;
        static bool lastMousecamActive = false;
        bool changed = (lastFocX != g_liveCamFocX || lastPosX != g_liveCamPosX || lastCamFOV != g_liveCamFOV ||
                        lastShortcut != g_liveShortcutMenu || lastMenu != g_liveMenuState ||
                        lastMousecamActive != g_mousecamActive);
        lastFocX = g_liveCamFocX; lastPosX = g_liveCamPosX; lastCamFOV = g_liveCamFOV;
        lastShortcut = g_liveShortcutMenu; lastMenu = g_liveMenuState;
        lastMousecamActive = g_mousecamActive;

        // Always invalidate if we are polling (for the status dot) or dragging
        bool animating = true; // Always invalidate with a 16ms timer for smooth UI since it's cheap enough, but only if an animation is actually happening.
        bool hasAnim = (g_animInject > 0 && g_animInject < 1) || (g_animReinject > 0 && g_animReinject < 1) || (g_animReset > 0 && g_animReset < 1) || (g_animToggleCam > 0 && g_animToggleCam < 1) ||
                       (g_animDarkBtn > 0 && g_animDarkBtn < 1) || (g_animLightBtn > 0 && g_animLightBtn < 1) ||
                       (g_animPath > 0 && g_animPath < 1) || (g_animPathReset > 0 && g_animPathReset < 1) ||
                       (g_animTheme != (g_config.use_light_theme ? 0.0f : 1.0f)) ||
                       (g_animScrollHelper > 0 && g_animScrollHelper < 1) ||
                       (g_animOrbitCam > 0 && g_animOrbitCam < 1) || (g_animIndepSens > 0 && g_animIndepSens < 1) || (g_animIndepMagneSens > 0 && g_animIndepMagneSens < 1) ||
                       (g_animCemuExperimental > 0 && g_animCemuExperimental < 1) ||
                       (g_animSensH > 0 && g_animSensH < 1) || (g_animSensV > 0 && g_animSensV < 1) ||
                       (g_animMagneSens > 0 && g_animMagneSens < 1) || (g_animMagneSensV > 0 && g_animMagneSensV < 1) || (g_animMagnePullSens > 0 && g_animMagnePullSens < 1) ||
                       (g_animFpsMagnesis > 0 && g_animFpsMagnesis < 1) || (g_animFpsMagneEyeHeight > 0 && g_animFpsMagneEyeHeight < 1) ||
                       (g_animFpsMagneOffsetForward > 0 && g_animFpsMagneOffsetForward < 1) || (g_animFpsMagneOffsetSide > 0 && g_animFpsMagneOffsetSide < 1) ||
                       (g_animClearLog > 0 && g_animClearLog < 1);
        for (int i = 0; i < 5; ++i) if (g_animDrop[i] > 0 && g_animDrop[i] < 1) hasAnim = true;
        if (changed || g_dragSlider != -1 || !g_targetInjected || hasAnim) {
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SIZE: {
        InvalidateUIRectsCache(); // UX7: force recalculation on resize
        UpdateConsoleEditPosition(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_APP + 1:
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1; // Prevent background from being erased, fixes screen flashing/flicker
    case WM_PAINT:
        PaintWindow(hWnd);
        return 0;
    case WM_LBUTTONDOWN:
        return HandleLButtonDown(hWnd, wParam, lParam);
    case WM_LBUTTONUP:
        return HandleLButtonUp(hWnd, wParam, lParam);
    case WM_MOUSEMOVE:
        return HandleMouseMove(hWnd, wParam, lParam);
    case WM_MOUSEWHEEL:
        return HandleMouseWheel(hWnd, wParam, lParam);
    case WM_MOUSELEAVE:
        return HandleMouseLeave(hWnd, wParam, lParam);
    case WM_KILLFOCUS:
        return HandleKillFocus(hWnd, wParam, lParam);
    case WM_SETCURSOR:
        return HandleSetCursor(hWnd, wParam, lParam);
    case WM_GETMINMAXINFO:
        return HandleGetMinMaxInfo(hWnd, wParam, lParam);
    case WM_CLOSE:
        DoEjectOnClose();
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        DoEjectOnClose();
        if (g_hTargetProcess) {
            CloseHandle(g_hTargetProcess);
            g_hTargetProcess = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    LoadLibraryW(L"msftedit.dll");

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);

    // DPI awareness — must be set before window creation (UX2)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(101));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = L"MousecamClass";

    RegisterClassExW(&wcex);

    WNDCLASSEXW wcexPopup = {};
    wcexPopup.cbSize = sizeof(WNDCLASSEXW);
    wcexPopup.style = CS_HREDRAW | CS_VREDRAW | CS_SAVEBITS;
    wcexPopup.lpfnWndProc = DropdownPopupWndProc;
    wcexPopup.hInstance = hInstance;
    wcexPopup.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcexPopup.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcexPopup.lpszClassName = L"DropdownPopupClass";
    RegisterClassExW(&wcexPopup);

    // Load config BEFORE window creation so theme colors are known (I2)
    LoadConfig();

    // Initial rect
    RECT rc = { 0, 0, WND_W, WND_H };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

    g_hWnd = CreateWindowExW(0, L"MousecamClass", L"Mousecam Companion",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);

    g_hConsoleEdit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 0, 0, g_hWnd, nullptr, hInstance, nullptr);

    SendMessageW(g_hConsoleEdit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), MAKELPARAM(TRUE, 0));
    // Theme BG is applied by ApplyTheme() below — no duplicate line needed (I2)

    if (!g_hWnd) return 0;

    // Apply theme now that g_hConsoleEdit exists (sets console BG, etc.)
    ApplyTheme();
    UpdateConsoleEditPosition(g_hWnd);
    g_ki.ReloadSettings();

    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(g_gdiplusToken);
    return (int)msg.wParam;
}