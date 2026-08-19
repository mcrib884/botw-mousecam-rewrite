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
        // P3-7: Drive animations by wall-clock delta (QPC) instead of fixed
        // per-tick increments. The original code added 0.05f per 8 ms tick,
        // which makes animations frame-rate-sensitive: under load (Cemu running
        // + heavy paint) timer ticks can drop to 30 Hz or worse, stretching
        // hover-in durations from ~160 ms to ~640 ms. With a QPC delta, a
        // 1.0/sec ramp completes in 1 s regardless of frame pacing.
        static LARGE_INTEGER s_qpcFreq = {0};
        static LARGE_INTEGER s_lastTick = {0};
        if (s_qpcFreq.QuadPart == 0) {
            QueryPerformanceFrequency(&s_qpcFreq);
            QueryPerformanceCounter(&s_lastTick);
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - s_lastTick.QuadPart) / (double)s_qpcFreq.QuadPart;
        s_lastTick = now;
        // Clamp dt to a sane range so a paused/hibernated session doesn't snap
        // animations to the far state instantly.
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.25) dt = 0.25;
        // Normalize the previous 0.05f-per-8ms constants: that's 6.25/sec.
        // Hover ramps (0.05f per 8 ms) become 6.25*dt; the toggle-state ramps
        // (0.075f per 8 ms) become 9.375*dt.
        const float hoverRate = 6.25f * (float)dt;
        const float toggleRate = 9.375f * (float)dt;
        const float themeRate = 6.25f * (float)dt;  // was 0.05f @ 8 ms

        g_animInject += (g_hoverInject ? hoverRate : -hoverRate);
        g_animReinject += (g_hoverReinject ? hoverRate : -hoverRate);
        g_animReset += (g_hoverReset ? hoverRate : -hoverRate);
        g_animToggleCam += (g_hoverToggleCam ? hoverRate : -hoverRate);
        g_animDarkBtn += (g_hoverDarkBtn ? hoverRate : -hoverRate);
        g_animLightBtn += (g_hoverLightBtn ? hoverRate : -hoverRate);
        g_animPath += (g_hoverPath ? hoverRate : -hoverRate);
        g_animPathReset += (g_hoverPathReset ? hoverRate : -hoverRate);
        g_animScrollHelper += (g_config.scroll_helper ? toggleRate : -toggleRate);
        g_animOrbitCam += (g_config.full_orbit_camera ? toggleRate : -toggleRate);
        g_animIndepSens += (g_config.use_independent_sens ? toggleRate : -toggleRate);
        g_animIndepMagneSens += (g_config.use_independent_magne_sens ? toggleRate : -toggleRate);
        g_animCemuExperimental += (g_config.cemu_experimental ? toggleRate : -toggleRate);
        g_animSensH += (g_hoverSensH ? hoverRate : -hoverRate);
        g_animSensV += (g_hoverSensV ? hoverRate : -hoverRate);
        g_animMagneSens += (g_hoverMagneSens ? hoverRate : -hoverRate);
        g_animMagneSensV += (g_hoverMagneSensV ? hoverRate : -hoverRate);
        g_animMagnePullSens += (g_hoverMagnePullSens ? hoverRate : -hoverRate);
        g_animFpsMagnesis += (g_config.fps_magnesis ? toggleRate : -toggleRate);
        g_animFpsMagneEyeHeight += (g_hoverFpsMagneEyeHeight ? hoverRate : -hoverRate);
        g_animFpsMagneOffsetForward += (g_hoverFpsMagneOffsetForward ? hoverRate : -hoverRate);
        g_animFpsMagneOffsetSide += (g_hoverFpsMagneOffsetSide ? hoverRate : -hoverRate);
        g_animClearLog += (g_hoverClearLog ? hoverRate : -hoverRate);
        g_animCopyLog += (g_hoverCopyLog ? hoverRate : -hoverRate);
        for (int i = 0; i < 5; ++i) g_animDrop[i] += (g_hoverDrop == i ? hoverRate : -hoverRate);

        auto clampF = [](float& val) { if (val < 0) val = 0; if (val > 1) val = 1; };
        clampF(g_animInject); clampF(g_animReinject); clampF(g_animReset); clampF(g_animToggleCam);
        clampF(g_animDarkBtn); clampF(g_animLightBtn);
        clampF(g_animPath); clampF(g_animPathReset);
        clampF(g_animScrollHelper); clampF(g_animOrbitCam);
        clampF(g_animIndepSens); clampF(g_animIndepMagneSens); clampF(g_animCemuExperimental); clampF(g_animSensH); clampF(g_animSensV); clampF(g_animMagneSens); clampF(g_animMagneSensV); clampF(g_animMagnePullSens);
        clampF(g_animFpsMagnesis); clampF(g_animFpsMagneEyeHeight); clampF(g_animFpsMagneOffsetForward); clampF(g_animFpsMagneOffsetSide); clampF(g_animClearLog); clampF(g_animCopyLog);
        for (int i = 0; i < 5; ++i) clampF(g_animDrop[i]);

        float targetTheme = g_config.use_light_theme ? 0.0f : 1.0f;
        if (g_animTheme == -1.0f) g_animTheme = targetTheme;
        if (g_animTheme != targetTheme) {
            // P3-7: theme crossfade step also uses wall-clock delta so it stays
            // ~160 ms even under load.
            if (targetTheme > g_animTheme) {
                g_animTheme += themeRate;
                if (g_animTheme > targetTheme) g_animTheme = targetTheme;
            } else {
                g_animTheme -= themeRate;
                if (g_animTheme < targetTheme) g_animTheme = targetTheme;
            }
            ApplyTheme();
        }

        UpdateUiState();
        WriteConfigToSharedMemory();
        UpdateTelemetryGui();
        // Track live telemetry deltas so we only invalidate when something
        // the user can actually SEE on screen has changed. Previously the
        // companion invalidated at 125 Hz whenever it wasn't injected, which
        // pinned the CPU at 3-5% on idle desktops.
        static float lastFocX = 0, lastPosX = 0, lastCamFOV = 0;
        static int32_t lastShortcut = -1; static uint8_t lastMenu = 1;
        static bool lastMousecamActive = false;
        bool changed = (lastFocX != g_liveCamFocX || lastPosX != g_liveCamPosX || lastCamFOV != g_liveCamFOV ||
                        lastShortcut != g_liveShortcutMenu || lastMenu != g_liveMenuState ||
                        lastMousecamActive != g_mousecamActive);
        lastFocX = g_liveCamFocX; lastPosX = g_liveCamPosX; lastCamFOV = g_liveCamFOV;
        lastShortcut = g_liveShortcutMenu; lastMenu = g_liveMenuState;
        lastMousecamActive = g_mousecamActive;

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
                       (g_animClearLog > 0 && g_animClearLog < 1) ||
                       (g_animCopyLog > 0 && g_animCopyLog < 1);
        for (int i = 0; i < 5; ++i) if (g_animDrop[i] > 0 && g_animDrop[i] < 1) hasAnim = true;
        // P2-2: only invalidate if SOMETHING is actually changing on screen.
        // Previously `!g_targetInjected` triggered 125 Hz repaints even when
        // the status dot was static. We now skip invalidate when nothing's
        // animating, telemetry is unchanged, and the user isn't dragging a
        // slider. UpdateUiState already calls InvalidateRect when the
        // injected state flips, so we don't need to keep repainting after
        // the transition settles.
        if (changed || g_dragSlider != -1 || hasAnim) {
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SIZE: {
        InvalidateUIRectsCache(); // IM-11: force recalculation on resize
        UpdateConsoleEditPosition(hWnd);
        // IM-11: no InvalidateRect here — let WM_TIMER paint with the correct
        // layout. During a drag-resize, Windows sends dozens of WM_SIZE events
        // per second. Unconditional invalidation on each event triggered an
        // equivalent number of Gd GDI+ paint passes, many of them wasted
        // (the user's still dragging). The WM_TIMER fires ~16 ms after the
        // last resize event, so there's no visible lag.
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
        // IM-7: save window rect to config before destroying, so the user's
        // last position/size is restored on next launch.
        {
            WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
            if (GetWindowPlacement(hWnd, &wp) && wp.showCmd == SW_SHOWNORMAL) {
                RECT norm = wp.rcNormalPosition;
                g_config.window_x = norm.left;
                g_config.window_y = norm.top;
                g_config.window_w = norm.right - norm.left;
                g_config.window_h = norm.bottom - norm.top;
                SaveConfig();
            }
        }
        DoEjectOnClose();
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        // WM_DESTROY fires synchronously from DestroyWindow above. DoEjectOnClose
        // already ran in WM_CLOSE — calling it again here would re-eject / re-close
        // an already-torn-down handle. Per Windows docs, session/logoff paths
        // (WM_QUERYENDSESSION) terminate the process without re-entering here, so
        // we don't need a fallback eject for those.
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
    // P2-1: Detect msftedit.dll load failure and surface it to the user. The
    // richedit log panel silently renders empty on systems missing msftedit
    // (some IoT/LTSB Windows builds, hardened enterprise configs). The
    // existing code called LoadLibraryW and ignored the return — the
    // subsequent CreateWindowExW(MSFTEDIT_CLASS, ...) then silently fell back
    // to Default (a 0-size static, no visible child). We at least log the
    // failure to OutputDebugString so it shows up in DbgView / VS debugger
    // output, and the console subclass now also detects a non-richedit
    // fallback (see console.cpp).
    HMODULE hMsftEdit = LoadLibraryW(L"msftedit.dll");
    if (!hMsftEdit) {
        wchar_t dbg[160];
        swprintf_s(dbg, L"[Mousecam] msftedit.dll failed to load (err=%lu); rich log disabled.\n", GetLastError());
        OutputDebugStringW(dbg);
    }

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

    // IM-7: use saved window position/size from config when available.
    int initW = (g_config.window_w > 0) ? g_config.window_w : WND_W;
    int initH = (g_config.window_h > 0) ? g_config.window_h : WND_H;
    int initX = CW_USEDEFAULT;
    int initY = CW_USEDEFAULT;
    // Only use saved X/Y if both are valid (the window may have been moved to
    // a monitor that no longer exists; Windows can fix that via
    // CW_USEDEFAULT but we avoid passing a stale off-screen coords).
    bool hasSavedPos = (g_config.window_x >= 0 && g_config.window_y >= 0);
    if (hasSavedPos) {
        initX = g_config.window_x;
        initY = g_config.window_y;
    }

    RECT rc = { 0, 0, initW, initH };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

    g_hWnd = CreateWindowExW(0, L"MousecamClass", L"Mousecam Companion",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        initX, initY, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);

    g_hConsoleEdit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 0, 0, g_hWnd, nullptr, hInstance, nullptr);

    // P2-1: Detect if MSFTEDIT_CLASS failed to register. CreateWindowExW then
    // returns a "Default" static control (not a rich edit), and EM_REPLACESEL
    // would silently do nothing. Fall back to a plain EDIT control so the user
    // at least sees visible log text (without colored type tags, but readable).
    extern bool g_consoleIsRichEdit;
    if (g_hConsoleEdit) {
        wchar_t clsName[64] = {};
        GetClassNameW(g_hConsoleEdit, clsName, 64);
        if (wcscmp(clsName, L"RICHEDIT50W") == 0 || wcscmp(clsName, L"RichEdit20W") == 0) {
            g_consoleIsRichEdit = true;
        } else {
            // msftedit.dll missing — recreate as standard EDIT class.
            DestroyWindow(g_hConsoleEdit);
            g_hConsoleEdit = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                0, 0, 0, 0, g_hWnd, nullptr, hInstance, nullptr);
            g_consoleIsRichEdit = false;
        }
    }

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