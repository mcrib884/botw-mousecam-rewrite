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
#include <TlHelp32.h>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <exception>
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

// Append-only forensic trail that survives session restarts (mousecam.log is
// truncated at every startup by ClearLogFile). Every fatal handler and every
// close/exit path appends here, so a vanished companion can be classified from
// its last line as: crashed, CRT-aborted, or exited cleanly — and a clean exit
// shows WHICH decision was taken (eject / skip-eject / no pid).
void CompanionTrace(const char* fmt, ...) {
    char body[448];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    wchar_t path[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    *(slash + 1) = L'\0';
    wcscat_s(path, L"mousecam_companion_trace.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char line[576];
    int len = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
    if (len > 0) {
        DWORD written;
        WriteFile(h, line, (DWORD)len, &written, nullptr);
    }
    CloseHandle(h);
}

static void CompanionInvalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    CompanionTrace("[CRT] invalid-parameter call intercepted (debug-CRT fastfail source) — call failed, process survives");
}

static void CompanionPureCallHandler() {
    CompanionTrace("[FATAL] pure virtual call — exiting");
    ExitProcess(3);
}

static void CompanionTerminateHandler() {
    try { throw; }
    catch (const std::exception& e) { CompanionTrace("[FATAL] uncaught std::exception: %s", e.what()); }
    catch (...) { CompanionTrace("[FATAL] uncaught non-standard exception"); }
    abort();
}

// Hang watchdog: the UI thread has no supervisor. A 1 Hz background thread watches
// the WM_TIMER heartbeat; a stall longer than 3 s is traced from the watchdog thread
// itself, so the evidence survives even if the UI thread never recovers and the
// process is later terminated (termination writes no crash log of its own).
static std::atomic<uint64_t> g_lastUiTickMs{0};
static HANDLE g_hWatchdogStop = nullptr;
static HANDLE g_hWatchdogThread = nullptr;

static DWORD WINAPI WatchdogThreadProc(LPVOID) {
    bool hungLogged = false;
    for (;;) {
        if (WaitForSingleObject(g_hWatchdogStop, 1000) != WAIT_TIMEOUT) break;
        uint64_t last = g_lastUiTickMs.load(std::memory_order_relaxed);
        if (last == 0) continue;
        uint64_t stalled = GetTickCount64() - last;
        if (stalled > 3000) {
            if (!hungLogged) {
                hungLogged = true;
                CompanionTrace("[HANG] UI loop stalled %llu ms — suspect target-process polling during load",
                    (unsigned long long)stalled);
            }
        } else if (hungLogged) {
            hungLogged = false;
            CompanionTrace("[HANG] UI loop recovered after stall");
        }
    }
    return 0;
}

static void StartWatchdog() {
    g_hWatchdogStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_hWatchdogStop) return;
    g_lastUiTickMs.store(GetTickCount64(), std::memory_order_relaxed);
    g_hWatchdogThread = CreateThread(nullptr, 0, WatchdogThreadProc, nullptr, 0, nullptr);
    if (!g_hWatchdogThread) { CloseHandle(g_hWatchdogStop); g_hWatchdogStop = nullptr; }
}

static void StopWatchdog() {
    if (!g_hWatchdogStop) return;
    SetEvent(g_hWatchdogStop);
    if (g_hWatchdogThread) {
        WaitForSingleObject(g_hWatchdogThread, 2000);
        CloseHandle(g_hWatchdogThread);
        g_hWatchdogThread = nullptr;
    }
    CloseHandle(g_hWatchdogStop);
    g_hWatchdogStop = nullptr;
}

struct SiblingSearch {
    DWORD pid;
    HWND hWnd;
};

static BOOL CALLBACK FindSiblingWindowProc(HWND hWnd, LPARAM lParam) {
    auto data = reinterpret_cast<SiblingSearch*>(lParam);
    DWORD wndPid = 0;
    GetWindowThreadProcessId(hWnd, &wndPid);
    if (wndPid != data->pid) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(hWnd, cls, 64);
    if (wcscmp(cls, L"MousecamClass") == 0) {
        data->hWnd = hWnd;
        return FALSE;
    }
    return TRUE;
}

// Returns true when this instance must exit: a responsive sibling already owns the
// session. A hung or windowless stale instance does not block takeover.
static bool YieldToResponsiveSibling() {
    wchar_t selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    std::wstring selfName(selfPath);
    size_t pos = selfName.find_last_of(L"\\/");
    if (pos != std::wstring::npos) selfName = selfName.substr(pos + 1);
    if (selfName.empty()) return false;

    DWORD selfPid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    DWORD siblingPid = 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID != selfPid && _wcsicmp(pe.szExeFile, selfName.c_str()) == 0) {
                siblingPid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (siblingPid == 0) return false;

    SiblingSearch search{ siblingPid, nullptr };
    EnumWindows(FindSiblingWindowProc, reinterpret_cast<LPARAM>(&search));
    if (search.hWnd) {
        DWORD_PTR dummy = 0;
        if (SendMessageTimeoutW(search.hWnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000, &dummy) != 0) {
            CompanionTrace("[EXIT] duplicate launch — responsive sibling (pid %lu) owns session, exiting", siblingPid);
            SetForegroundWindow(search.hWnd);
            MessageBoxW(nullptr, L"Mousecam Companion is already running.", L"Mousecam Companion", MB_OK | MB_ICONINFORMATION);
            return true;
        }
        CompanionTrace("[BOOT] stale sibling (pid %lu) unresponsive — taking over session", siblingPid);
    } else {
        CompanionTrace("[BOOT] stale sibling (pid %lu) has no window — taking over session", siblingPid);
    }
    return false;
}

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
        g_lastUiTickMs.store(GetTickCount64(), std::memory_order_relaxed);
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
        g_animToggleMagneTarget += (g_config.scan_magne_target ? 0.075f : -0.075f);
        g_animToggleShortcutMenu += (g_config.scan_shortcut_menu ? 0.075f : -0.075f);
        g_animToggleMenuState += (g_config.scan_menu_state ? 0.075f : -0.075f);
        g_animSensH += (g_hoverSensH ? 0.05f : -0.05f);
        g_animSensV += (g_hoverSensV ? 0.05f : -0.05f);
        g_animMagneSens += (g_hoverMagneSens ? 0.05f : -0.05f);
        g_animMagneSensV += (g_hoverMagneSensV ? 0.05f : -0.05f);
        for (int i = 0; i < 3; ++i) g_animMagneSpeedBtn[i] += (g_hoverMagneSpeedBtn[i] ? 0.05f : -0.05f);
        g_animMagnePullSens += (g_hoverMagnePullSens ? 0.05f : -0.05f);
        g_animFpsMagnesis += (g_config.fps_magnesis ? 0.075f : -0.075f);
        g_animFpsMagneEyeHeight += (g_hoverFpsMagneEyeHeight ? 0.05f : -0.05f);
        g_animFpsMagneOffsetForward += (g_hoverFpsMagneOffsetForward ? 0.05f : -0.05f);
        g_animFpsMagneOffsetSide += (g_hoverFpsMagneOffsetSide ? 0.05f : -0.05f);
        g_animLogToFile += (g_config.log_to_file ? 0.075f : -0.075f);
        g_animClearLog += (g_hoverClearLog ? 0.05f : -0.05f);
        for (int i = 0; i < 5; ++i) g_animDrop[i] += (g_hoverDrop == i ? 0.05f : -0.05f);

        auto clampF = [](float& val) { if (val < 0) val = 0; if (val > 1) val = 1; };
        clampF(g_animInject); clampF(g_animReinject); clampF(g_animReset); clampF(g_animToggleCam);
        clampF(g_animDarkBtn); clampF(g_animLightBtn);
        clampF(g_animPath); clampF(g_animPathReset);
        clampF(g_animScrollHelper); clampF(g_animOrbitCam);
        clampF(g_animIndepSens); clampF(g_animIndepMagneSens); clampF(g_animCemuExperimental);
        clampF(g_animToggleMagneTarget); clampF(g_animToggleShortcutMenu); clampF(g_animToggleMenuState);
        clampF(g_animSensH); clampF(g_animSensV); clampF(g_animMagneSens); clampF(g_animMagneSensV);
        for (int i = 0; i < 3; ++i) clampF(g_animMagneSpeedBtn[i]);
        clampF(g_animMagnePullSens);
        clampF(g_animFpsMagnesis); clampF(g_animFpsMagneEyeHeight); clampF(g_animFpsMagneOffsetForward); clampF(g_animFpsMagneOffsetSide); clampF(g_animLogToFile); clampF(g_animClearLog);
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
                       (g_animToggleMagneTarget > 0 && g_animToggleMagneTarget < 1) ||
                       (g_animToggleShortcutMenu > 0 && g_animToggleShortcutMenu < 1) ||
                       (g_animToggleMenuState > 0 && g_animToggleMenuState < 1) ||
                       (g_animSensH > 0 && g_animSensH < 1) || (g_animSensV > 0 && g_animSensV < 1) ||
                       (g_animMagneSens > 0 && g_animMagneSens < 1) || (g_animMagneSensV > 0 && g_animMagneSensV < 1) || (g_animMagnePullSens > 0 && g_animMagnePullSens < 1) ||
                        (g_animFpsMagnesis > 0 && g_animFpsMagnesis < 1) || (g_animFpsMagneEyeHeight > 0 && g_animFpsMagneEyeHeight < 1) ||
                        (g_animFpsMagneOffsetForward > 0 && g_animFpsMagneOffsetForward < 1) || (g_animFpsMagneOffsetSide > 0 && g_animFpsMagneOffsetSide < 1) ||
                        (g_animLogToFile > 0 && g_animLogToFile < 1) || (g_animClearLog > 0 && g_animClearLog < 1);
        for (int i = 0; i < 3; ++i) if (g_animMagneSpeedBtn[i] > 0 && g_animMagneSpeedBtn[i] < 1) hasAnim = true;
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
        CompanionTrace("[EXIT] WM_CLOSE received");
        DoEjectOnClose();
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        CompanionTrace("[EXIT] WM_DESTROY — window gone, quitting after eject decision");
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

static LONG WINAPI CompanionExceptionHandler(PEXCEPTION_POINTERS ep) {
    // Best-effort crash trace — dedicated append-only file, never truncated by
    // session restarts, so a death is still readable after the companion reopens.
    // Note: must not use C++ objects with destructors inside __try (C2712)
    __try {
        CompanionTrace("[CRASH] unhandled exception 0x%08X at 0x%p tid=%lu — companion will exit. Share mousecam_companion_trace.log",
            (unsigned)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS);
    SetUnhandledExceptionFilter(CompanionExceptionHandler);
    _set_invalid_parameter_handler(CompanionInvalidParameterHandler);
    _set_purecall_handler(CompanionPureCallHandler);
    std::set_terminate(CompanionTerminateHandler);
    CompanionTrace("[BOOT] companion starting");
    if (YieldToResponsiveSibling()) return 0;
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

    // Every companion start clears the log file (fresh session) — do before LoadConfig
    // so early config warnings are not erased and file starts empty
    ClearLogFile();
    // Load config BEFORE window creation so theme colors are known (I2)
    LoadConfig();
    // Init log-to-file anim to match saved state (avoid 1s lerp on startup)
    extern float g_animLogToFile;
    g_animLogToFile = g_config.log_to_file ? 1.0f : 0.0f;
    if (g_config.log_to_file) {
        LogToConsole(L"[INFO] Log to file enabled — writing to mousecam.log");
    }

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

    StartWatchdog();
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CompanionTrace("[EXIT] message loop ended (wParam=%lld) — process returning", (long long)msg.wParam);
    StopWatchdog();
    GdiplusShutdown(g_gdiplusToken);
    return (int)msg.wParam;
}