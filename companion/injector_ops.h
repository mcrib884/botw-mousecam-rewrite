#pragma once
// injector_ops.h — Target process discovery, DLL inject/eject orchestration, and
// telemetry mirrors copied from shared memory. Extracted from companion/main.cpp.
//
// Owns:
//   - Process handles: g_hTargetProcess, g_targetPid, g_targetInjected.
//   - Telemetry mirrors (read/written only on UI thread via WM_TIMER -> UpdateTelemetryGui).
//   - Inject/eject/reinject orchestration that talks to Injector:: and shared memory.
//   - Target window enumeration (wxWindowNR class).

#include <Windows.h>
#include <string>
#include <cstdint>

struct TargetWndData {
    DWORD pid;
    HWND hWnd;
};

// Process state. Mutated by UpdateUiState on WM_TIMER, and by SafeEjectDLL/DoInjectOrEject.
extern HANDLE g_hTargetProcess;
extern DWORD g_targetPid;
extern bool g_targetInjected;

// TD5: Thread safety — telemetry mirrors are read/written ONLY on the UI thread via
// WM_TIMER -> UpdateTelemetryGui(). The shared memory (g_pSharedMemory) is written by
// the DLL in a different process — that's safe across process boundaries. No
// cross-thread access within the companion.
extern uintptr_t g_addrGameRomCamera;
extern uintptr_t g_addrMagneTarget;
extern uintptr_t g_addrShortcutMenu;
extern uintptr_t g_addrMenuState;

extern float g_liveCamPosX;
extern float g_liveCamPosY;
extern float g_liveCamPosZ;
extern float g_liveCamFocX;
extern float g_liveCamFocY;
extern float g_liveCamFocZ;
extern float g_liveCamFOV;
extern int32_t g_liveShortcutMenu;
extern uint8_t g_liveMenuState;

extern float g_liveMagneTargetX;
extern float g_liveMagneTargetY;
extern float g_liveMagneTargetZ;
extern float g_magneSpeedH;
extern float g_magneSpeedV;

extern bool g_mousecamActive;
extern uint32_t g_writersFound;
extern bool g_magneDetourActive;

enum class ScrollMenuType {
    None,
    Left,
    Right
};

// Returns path to botw-mousecam-rewrite.dll sitting next to this exe.
std::wstring GetCompanionDllPath();

// Finds Cemu by override exe name (if set) else by default names cemu.exe / cemu_release.exe / Cemu_release.exe.
DWORD FindCemuProcess();
DWORD GetSelectedOrTargetPid();

// Window enumeration helpers (used by WriteConfigToSharedMemory and externals).
BOOL CALLBACK FindTargetWindowProc(HWND hWnd, LPARAM lParam);
HWND GetTargetWindow(DWORD pid);
POINT GetWindowCenter(HWND hWnd);

// Syncs g_config + g_ki key bindings into shared memory layout.
void WriteConfigToSharedMemory();

// Polls target state: detects process exit, re-finds target every 10 ticks, maps/unmaps
// shared memory, handles F5 AOB dump in _DEBUG. Called from WM_TIMER.
void UpdateUiState();

// Eject sequence: sets m_reqShutdown, pumps messages up to 1500ms, then Injector::EjectDLL.
bool SafeEjectDLL(DWORD pid, const std::wstring& dllPath);

// Toggle inject/eject based on current module-loaded state.
void DoInjectOrEject();

// Eject (if loaded) -> Sleep(50) -> ReloadSettings -> inject fresh.
void DoReinject();

// Called from WM_CLOSE/WM_DESTROY to eject before the companion exits.
void DoEjectOnClose();

// Copies telemetry + drains the 8-entry log ring from shared memory into the console.
void UpdateTelemetryGui();