// injector_ops.cpp — Target discovery, inject/eject orchestration, telemetry mirrors.
// Extracted verbatim from companion/main.cpp lines 150-206, 207-222, 998-1205.

#define NOMINMAX
#include <Windows.h>
#include <string>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <cmath>

#include "injector_ops.h"
#include "injector.h"
#include "shared_memory_manager.h" // g_pSharedMemory, MapSharedMemory, UnmapSharedMemory
#include "shared_memory_layout.h"
#include "config.h"        // g_config
#include "cemu_key_injector.h" // g_ki
#include "console.h"      // LogToConsole, SetStatus
#include "string_utils.h" // Utf8ToWstr
#include "theme.h"        // g_hWnd (main window handle for InvalidateRect)

// Process state.
HANDLE g_hTargetProcess = nullptr;
DWORD g_targetPid = 0;
bool g_targetInjected = false;

// Telemetry mirrors.
uintptr_t g_addrGameRomCamera = 0;
uintptr_t g_addrMagneTarget = 0;
uintptr_t g_addrShortcutMenu = 0;
uintptr_t g_addrMenuState = 0;

float g_liveCamPosX = 0.0f;
float g_liveCamPosY = 0.0f;
float g_liveCamPosZ = 0.0f;
float g_liveCamFocX = 0.0f;
float g_liveCamFocY = 0.0f;
float g_liveCamFocZ = 0.0f;
float g_liveCamFOV = 0.0f;
int32_t g_liveShortcutMenu = -1;
uint8_t g_liveMenuState = 1;

bool g_mousecamActive = false;
uint32_t g_writersFound = 0;
bool g_magneDetourActive = false;

float g_liveMagneTargetX = 0.0f;
float g_liveMagneTargetY = 0.0f;
float g_liveMagneTargetZ = 0.0f;
float g_magneSpeedH = 0.0f;
float g_magneSpeedV = 0.0f;
float g_magneDeactSpeedH = 0.0f;
float g_magneDeactSpeedV = 0.0f;
bool  g_hasDeactSpeed = false;

std::wstring GetCompanionDllPath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring exePath(path);
    size_t pos = exePath.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return L"botw-mousecam-rewrite.dll";
    }
    return exePath.substr(0, pos + 1) + L"botw-mousecam-rewrite.dll";
}

BOOL CALLBACK FindTargetWindowProc(HWND hWnd, LPARAM lParam) {
    auto data = reinterpret_cast<TargetWndData*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid == data->pid && IsWindowVisible(hWnd)) {
        wchar_t className[256] = {};
        GetClassNameW(hWnd, className, 256);
        if (wcscmp(className, L"wxWindowNR") == 0 || GetWindow(hWnd, GW_OWNER) == nullptr) {
            data->hWnd = hWnd;
            return FALSE;
        }
    }
    return TRUE;
}

HWND GetTargetWindow(DWORD pid) {
    TargetWndData data = { pid, nullptr };
    EnumWindows(FindTargetWindowProc, reinterpret_cast<LPARAM>(&data));
    return data.hWnd;
}

POINT GetWindowCenter(HWND hWnd) {
    RECT rect;
    GetWindowRect(hWnd, &rect);
    POINT pt;
    pt.x = rect.left + (rect.right - rect.left) / 2;
    pt.y = rect.top + (rect.bottom - rect.top) / 2;
    return pt;
}

DWORD FindCemuProcess() {
    if (!g_config.cemu_path_override.empty()) {
        std::wstring exeName = Utf8ToWstr(g_config.cemu_path_override);
        size_t lastSlash = exeName.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            exeName = exeName.substr(lastSlash + 1);
        }
        DWORD pid = Injector::FindProcessByName(exeName);
        if (pid != 0) return pid;
    } else {
        DWORD pid = Injector::FindProcessByName(L"cemu.exe");
        if (pid != 0) return pid;
        pid = Injector::FindProcessByName(L"cemu_release.exe");
        if (pid != 0) return pid;
        pid = Injector::FindProcessByName(L"Cemu_release.exe");
        if (pid != 0) return pid;
    }

    return 0;
}

DWORD GetSelectedOrTargetPid() {
    return FindCemuProcess();
}

void WriteConfigToSharedMemory() {
    if (g_pSharedMemory) {
        g_pSharedMemory->m_companionPid = GetCurrentProcessId();
        g_pSharedMemory->m_cfgMagnesisEnabled = g_config.magnesis_enabled;
        g_pSharedMemory->m_cfgScrollHelper = g_config.scroll_helper ? 1 : 0;
        g_pSharedMemory->m_cfgFullOrbitCamera = g_config.full_orbit_camera ? 1 : 0;
        g_pSharedMemory->m_cfgCemuExperimental = g_config.cemu_experimental;
        g_pSharedMemory->m_cfgMagnesisSpeedMode = static_cast<uint8_t>(g_config.magnesis_speed_mode);
        g_pSharedMemory->m_cfgFpsMagnesis = g_config.fps_magnesis;
        g_pSharedMemory->m_cfgFpsMagneEyeHeight = g_config.fps_magne_eye_height;
        g_pSharedMemory->m_cfgFpsMagneOffsetForward = g_config.fps_magne_offset_forward;
        g_pSharedMemory->m_cfgFpsMagneOffsetSide = g_config.fps_magne_offset_side;
        g_pSharedMemory->m_cfgMagneSens = g_config.magnesis_sensitivity;
        g_pSharedMemory->m_cfgMagneSensY = g_config.magnesis_sensitivity_y;
        g_pSharedMemory->m_cfgUseIndependentMagneSens = g_config.use_independent_magne_sens;
        g_pSharedMemory->m_cfgMagnePullSens = g_config.magnesis_pull_sensitivity;
        g_pSharedMemory->m_cfgSensitivityX = g_config.sensitivity_x;
        g_pSharedMemory->m_cfgSensitivityY = g_config.sensitivity_y;
        g_pSharedMemory->m_cfgUseIndependentSens = g_config.use_independent_sens ? 1 : 0;

        g_pSharedMemory->m_cfgDpadUpKey = g_ki.GetDpadUpKey();
        g_pSharedMemory->m_cfgDpadDownKey = g_ki.GetDpadDownKey();
        g_pSharedMemory->m_cfgDpadLeftKey = g_ki.GetDpadLeftKey();
        g_pSharedMemory->m_cfgDpadRightKey = g_ki.GetDpadRightKey();
        g_pSharedMemory->m_cfgRstickLeftKey = g_ki.GetRstickLeftKey();
        g_pSharedMemory->m_cfgRstickRightKey = g_ki.GetRstickRightKey();

        for (int i = 0; i < 5; ++i) {
            uint32_t gpid = g_config.mouse_bindings[i];
            g_pSharedMemory->m_cfgMouseBindingKeys[i] = (gpid != 0) ? g_ki.GetKeyForGamepadId(gpid) : 0;
        }
        g_pSharedMemory->m_cfgHCemuWnd = reinterpret_cast<uint64_t>(GetTargetWindow(g_targetPid));
    }
}

void UpdateUiState() {
    static int findTicks = 0, refreshTicks = 0;
    // P2-2: Track previous injected state to invalidate ONLY on actual state
    // flips, not every timer tick. The 125 Hz invalidate from here was the
    // other half of the idle repaint churn — paired with the WM_TIMER
    // `!g_targetInjected` invalidate, both pinned the CPU at 3-5% on desktop.
    static bool s_prevInjected = false;
    bool injectedChanged = false;

    if (g_hTargetProcess != nullptr) {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(g_hTargetProcess, &exitCode) || exitCode != STILL_ACTIVE) {
            CloseHandle(g_hTargetProcess); g_hTargetProcess = nullptr; g_targetPid = 0; g_targetInjected = false;
            SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0); ClipCursor(nullptr);
            UnmapSharedMemory();
            SetStatus(L"Target process exited. Waiting...");
        } else {
            g_targetInjected = Injector::IsModuleLoaded(g_targetPid, Injector::GetFileName(GetCompanionDllPath()));
        }
    }

    if (g_hTargetProcess == nullptr) {
        findTicks++;
        if (findTicks >= 10) {
            findTicks = 0; g_targetPid = GetSelectedOrTargetPid();
            if (g_targetPid != 0) {
                g_hTargetProcess = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, g_targetPid);
                if (g_hTargetProcess) g_targetInjected = Injector::IsModuleLoaded(g_targetPid, Injector::GetFileName(GetCompanionDllPath()));
                else {
                    // IM-12: surface low-level OpenProcess failures so the user
                    // can distinguish "no Cemu running" (pid=0) from "found pid
                    // but can't open handle" (access denied, killed target, etc.)
                    DWORD err = GetLastError();
                    if (err != 0) {
                        LogToConsole(L"[WARNING] OpenProcess(%lu) failed (err=%lu). May need administrator or the process may have exited.", g_targetPid, err);
                    }
                }
            }
        }
    } else { findTicks = 0; }

    if (g_targetInjected && g_targetPid != 0) MapSharedMemory();
    else if (!g_targetInjected) UnmapSharedMemory();

    HWND fg = GetForegroundWindow();
    bool isCompanionFocused = (fg != nullptr && (fg == g_hWnd || IsChild(g_hWnd, fg)));
    static bool wasF2Pressed = false;
    bool isF2Pressed = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (isCompanionFocused && isF2Pressed && !wasF2Pressed) {
        if (g_targetInjected && g_pSharedMemory) {
            g_pSharedMemory->m_reqToggleMousecam = true;
        }
    }
    wasF2Pressed = isF2Pressed;

#ifdef _DEBUG
    static bool wasF5Pressed = false;
    bool isF5Pressed = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (isF5Pressed && !wasF5Pressed) {
        if (g_pSharedMemory) {
            g_pSharedMemory->m_reqDumpAob = true;
            LogToConsole(L"[INFO] Requested AOB dump.");
        }
    }
    wasF5Pressed = isF5Pressed;
#endif

    if (s_prevInjected != g_targetInjected) {
        s_prevInjected = g_targetInjected;
        injectedChanged = true;
    }
    // P2-2: only invalidate when the injected flag actually flipped. When
    // we're idle (no target process, timer ticking at 125 Hz), the previous
    // unconditional InvalidateRect was the major cause of constant CPU.
    if (injectedChanged && g_hWnd) InvalidateRect(g_hWnd, nullptr, FALSE);
}

bool SafeEjectDLL(DWORD pid, const std::wstring& dllPath) {
    if (g_pSharedMemory) {
        g_pSharedMemory->m_statusShutdownDone = false;
        g_pSharedMemory->m_reqShutdown = true;
        // P2-5: Push the wait from 250 ms to 5 s. The previous 250 ms ceiling
        // was tight enough that even a normal scanner join (magnesis detour
        // teardown, hook restore) could miss the window, leaving the FreeLibrary
        // caller racing a trampoline write — which then crashes Cemu. 5 s is
        // still bounded but accommodates worst-case teardown under load. We
        // also surface the timeout to the log so the user sees when their
        // Eject didn't actually wait for a clean DLL shutdown.
        constexpr int kWaitIterations = 500;  // 500 * 10 ms = 5000 ms
        int waitLimit = kWaitIterations;
        while (waitLimit-- > 0 && !g_pSharedMemory->m_statusShutdownDone) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(10);
        }
        if (waitLimit < 0 && !g_pSharedMemory->m_statusShutdownDone) {
            LogToConsole(L"[WARNING] DLL shutdown did not complete in 5s — proceeding with eject (possible trampoline race).");
        }
    }
    return Injector::EjectDLL(pid, dllPath);
}

void DoInjectOrEject() {
    DWORD pid = GetSelectedOrTargetPid();
    if (!pid) { SetStatus(L"Error: cemu.exe not found."); return; }
    std::wstring dllPath = GetCompanionDllPath();
    if (Injector::IsModuleLoaded(pid, Injector::GetFileName(dllPath))) {
        SetStatus(L"Ejecting...");
        if (SafeEjectDLL(pid, dllPath)) { SetStatus(L"Ejection successful!"); UpdateUiState(); }
        else SetStatus(L"Error: ejection failed.");
    } else {
        SetStatus(L"Injecting...");
        if (MapSharedMemory()) {
            WriteConfigToSharedMemory();
        }
        if (Injector::InjectDLL(pid, dllPath)) { SetStatus(L"Injection successful!"); UpdateUiState(); }
        else SetStatus(L"Error: injection failed \x2014 try running as Administrator.");
    }
}

void DoReinject() {
    DWORD pid = GetSelectedOrTargetPid();
    if (!pid) return;
    std::wstring dllPath = GetCompanionDllPath();
    if (Injector::IsModuleLoaded(pid, Injector::GetFileName(dllPath))) {
        SetStatus(L"Unloading DLL...");
        SafeEjectDLL(pid, dllPath);
        Sleep(100);
    }
    g_ki.ReloadSettings();
    SetStatus(L"Injecting fresh DLL...");
    if (MapSharedMemory()) {
        WriteConfigToSharedMemory();
    }
    if (Injector::InjectDLL(pid, dllPath)) SetStatus(L"Reinjected successfully!");
    else SetStatus(L"Reinject error: injection failed.");
}

void DoEjectOnClose() {
    DWORD pid = GetSelectedOrTargetPid();
    if (pid && Injector::IsModuleLoaded(pid, Injector::GetFileName(GetCompanionDllPath()))) {
        SetStatus(L"Ejecting DLL on close...");
        UpdateWindow(g_hWnd); // Force immediate redraw to show status
        SafeEjectDLL(pid, GetCompanionDllPath());
    }
}

static uint32_t g_logReadIdx = 0;

void UpdateTelemetryGui() {
    if (!g_pSharedMemory) {
        g_addrGameRomCamera = 0; g_addrMagneTarget = 0; g_addrShortcutMenu = 0; g_addrMenuState = 0; g_writersFound = 0;
        g_liveCamPosX = 0; g_liveCamPosY = 0; g_liveCamPosZ = 0; g_liveCamFocX = 0; g_liveCamFocY = 0; g_liveCamFocZ = 0; g_liveCamFOV = 0;
        g_liveShortcutMenu = -1; g_liveMenuState = 1; g_magneDetourActive = false;
        g_mousecamActive = false;
        g_liveMagneTargetX = 0.0f; g_liveMagneTargetY = 0.0f; g_liveMagneTargetZ = 0.0f;
        g_magneSpeedH = 0.0f; g_magneSpeedV = 0.0f;
        g_logReadIdx = 0;
    } else {
        if (g_pSharedMemory->m_statusAddrGameRomCamera != 0 && g_addrGameRomCamera == 0) {
            LogToConsole(L"[INFO] Found GameRomCamera");
        } else if (g_pSharedMemory->m_statusAddrGameRomCamera == 0 && g_addrGameRomCamera != 0) {
            LogToConsole(L"[WARNING] GameRomCamera lost! Re-scanning memory...");
        }
        if (g_pSharedMemory->m_statusAddrMagneTarget != 0 && g_addrMagneTarget == 0) LogToConsole(L"[INFO] Found Magne Target Sig");
        if (g_pSharedMemory->m_statusAddrShortcutMenu != 0 && g_addrShortcutMenu == 0) LogToConsole(L"[INFO] Found ShortcutMenu");
        if (g_pSharedMemory->m_statusAddrMenuState != 0 && g_addrMenuState == 0) LogToConsole(L"[INFO] Found MenuState");

        g_addrGameRomCamera = g_pSharedMemory->m_statusAddrGameRomCamera;
        g_addrMagneTarget = g_pSharedMemory->m_statusAddrMagneTarget;
        g_addrShortcutMenu = g_pSharedMemory->m_statusAddrShortcutMenu;
        g_addrMenuState = g_pSharedMemory->m_statusAddrMenuState;
        g_writersFound = g_pSharedMemory->m_statusWritersFound;
        g_mousecamActive = g_pSharedMemory->m_statusMousecamActive;

        g_liveCamPosX = g_pSharedMemory->m_teleLiveCamPosX; g_liveCamPosY = g_pSharedMemory->m_teleLiveCamPosY; g_liveCamPosZ = g_pSharedMemory->m_teleLiveCamPosZ;
        g_liveCamFocX = g_pSharedMemory->m_teleLiveCamFocX; g_liveCamFocY = g_pSharedMemory->m_teleLiveCamFocY; g_liveCamFocZ = g_pSharedMemory->m_teleLiveCamFocZ;
        g_liveCamFOV = g_pSharedMemory->m_teleLiveCamFOV; g_liveShortcutMenu = g_pSharedMemory->m_teleLiveShortcutMenu; g_liveMenuState = g_pSharedMemory->m_teleLiveMenuState;
        g_magneDetourActive = g_pSharedMemory->m_patchMagneDetourActive;

        g_liveMagneTargetX = g_pSharedMemory->m_teleMagneTargetX;
        g_liveMagneTargetY = g_pSharedMemory->m_teleMagneTargetY;
        g_liveMagneTargetZ = g_pSharedMemory->m_teleMagneTargetZ;

        // Compute magnesis object horizontal angular speed (from player) and vertical linear speed.
        static float s_prevMagneX = 0.0f, s_prevMagneY = 0.0f, s_prevMagneZ = 0.0f;
        static bool s_prevMagneValid = false;
        static std::chrono::steady_clock::time_point s_prevMagneTime = std::chrono::steady_clock::now();
        static float s_lastActiveSpeedH = 0.0f;
        static float s_lastActiveSpeedV = 0.0f;
        static bool s_wasMagneActive = false;

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - s_prevMagneTime).count();
        bool isMagneCurrentlyActive = g_magneDetourActive && (g_liveMagneTargetX != 0.0f || g_liveMagneTargetY != 0.0f || g_liveMagneTargetZ != 0.0f);

        float px = g_pSharedMemory ? g_pSharedMemory->m_telePivotX : 0.0f;
        float pz = g_pSharedMemory ? g_pSharedMemory->m_telePivotZ : 0.0f;

        if (isMagneCurrentlyActive) {
            if (s_prevMagneValid && dt >= 0.010f && dt < 1.0f) {
                float dxPrev = s_prevMagneX - px;
                float dzPrev = s_prevMagneZ - pz;
                float dxCurr = g_liveMagneTargetX - px;
                float dzCurr = g_liveMagneTargetZ - pz;

                float distPrevSq = dxPrev * dxPrev + dzPrev * dzPrev;
                float distCurrSq = dxCurr * dxCurr + dzCurr * dzCurr;

                if (distPrevSq > 0.1f && distCurrSq > 0.1f) {
                    float anglePrev = std::atan2(dzPrev, dxPrev);
                    float angleCurr = std::atan2(dzCurr, dxCurr);

                    float dTheta = angleCurr - anglePrev;
                    constexpr float PI = 3.14159265358979323846f;
                    while (dTheta > PI)  dTheta -= 2.0f * PI;
                    while (dTheta < -PI) dTheta += 2.0f * PI;

                    float angularSpeedRad = std::fabs(dTheta) / dt;
                    float angularSpeedDeg = angularSpeedRad * (180.0f / PI);

                    if (angularSpeedDeg < 2000.0f) {
                        g_magneSpeedH = 0.6f * g_magneSpeedH + 0.4f * angularSpeedDeg;
                    }

                    float dy = g_liveMagneTargetY - s_prevMagneY;
                    float vSpeed = dy / dt;
                    if (std::fabs(vSpeed) < 500.0f) {
                        g_magneSpeedV = 0.6f * g_magneSpeedV + 0.4f * vSpeed;
                    }

                    if (g_magneSpeedH > 0.1f || std::fabs(g_magneSpeedV) > 0.1f) {
                        s_lastActiveSpeedH = g_magneSpeedH;
                        s_lastActiveSpeedV = g_magneSpeedV;
                    }
                }

                s_prevMagneX = g_liveMagneTargetX;
                s_prevMagneY = g_liveMagneTargetY;
                s_prevMagneZ = g_liveMagneTargetZ;
                s_prevMagneTime = now;
            } else if (!s_prevMagneValid) {
                s_prevMagneX = g_liveMagneTargetX;
                s_prevMagneY = g_liveMagneTargetY;
                s_prevMagneZ = g_liveMagneTargetZ;
                s_prevMagneValid = true;
                s_prevMagneTime = now;
                g_magneSpeedH = 0.0f;
                g_magneSpeedV = 0.0f;
            }
            s_wasMagneActive = true;
        } else {
            if (s_wasMagneActive) {
                g_magneDeactSpeedH = s_lastActiveSpeedH;
                g_magneDeactSpeedV = s_lastActiveSpeedV;
                g_hasDeactSpeed = true;
                s_wasMagneActive = false;
            }
            s_prevMagneValid = false;
            g_magneSpeedH = 0.0f;
            g_magneSpeedV = 0.0f;
        }
        s_prevMagneX = g_liveMagneTargetX;
        s_prevMagneY = g_liveMagneTargetY;
        s_prevMagneZ = g_liveMagneTargetZ;
        s_prevMagneValid = isMagneCurrentlyActive;
        s_prevMagneTime = now;

        // Read new logs from shared memory
        uint32_t writeIdx = g_pSharedMemory->m_logWriteIdx;
        if (writeIdx < g_logReadIdx) {
            g_logReadIdx = writeIdx;
        }
        if (writeIdx - g_logReadIdx > 8) {
            g_logReadIdx = writeIdx - 8;
        }
        while (g_logReadIdx < writeIdx) {
            uint32_t idx = g_logReadIdx % 8;
            char localLog[129];
            memcpy(localLog, g_pSharedMemory->m_logQueue[idx], 128);
            localLog[128] = '\0'; // Guarantee null termination
            std::string logMsg(localLog);
            std::wstring wLogMsg = Utf8ToWstr(logMsg);
            LogToConsole(wLogMsg.c_str());
            g_logReadIdx++;
        }
    }
}