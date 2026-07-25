#pragma once
#include <cstdint>

// SharedMemoryLayout is the single source of truth for the memory-mapped file
// between the companion (UI process) and the mod DLL (injected into Cemu).
//
// Why a shared header and not two independent definitions:
// If the companion and DLL define this struct independently, any field added,
// removed, or reordered in one but not the other causes silent memory corruption
// with zero compiler warnings — the layouts drift and offsets become wrong.
// A single header included by both projects eliminates this risk entirely.
//
// Ownership split:
//   Companion writes: m_cfg* fields (config), m_req* fields (commands)
//   DLL writes:       m_status* fields (scan results), m_tele* fields (telemetry)

struct SharedMemoryLayout {
    // --- Pointers set by DLL on init (read by companion) ---
    uint64_t m_dllBaseAddr;

    // --- Config (written by companion, read by DLL) ---
    uint32_t m_companionPid;
    bool     m_cfgMagnesisEnabled;
    bool     m_cfgScrollHelper;
    float    m_cfgSensitivityX;
    float    m_cfgSensitivityY;
    uint32_t m_cfgUseIndependentSens;
    uint16_t m_cfgDpadUpKey;
    uint16_t m_cfgDpadDownKey;
    uint16_t m_cfgDpadLeftKey;
    uint16_t m_cfgDpadRightKey;
    uint16_t m_cfgRstickLeftKey;
    uint16_t m_cfgRstickRightKey;
    uint16_t m_cfgMouseBindingKeys[5];
    uint64_t m_cfgHCemuWnd;
    uint8_t  m_cfgFullOrbitCamera;
    bool     m_cfgCemuExperimental;
    uint8_t  m_cfgMagnesisSpeedMode; // 0=Vanilla, 1=Extended, 2=Unlimited
    bool     m_cfgFpsMagnesis;
    float    m_cfgFpsMagneEyeHeight;
    float    m_cfgFpsMagneOffsetForward;
    float    m_cfgFpsMagneOffsetSide;
    float    m_cfgMagneSens;         // Magnesis sensitivity multiplier (H)
    float    m_cfgMagneSensY;        // Magnesis sensitivity multiplier (V)
    bool     m_cfgUseIndependentMagneSens; // Separate vertical Magnesis sensitivity
    float    m_cfgMagnePullSens;     // Magnesis pull sensitivity multiplier


    // --- Status / scan results (written by DLL, read by companion) ---
    uint64_t m_statusAddrGameRomCamera;
    uint64_t m_statusAddrShortcutMenu;
    uint64_t m_statusAddrMenuState;
    uint64_t m_statusAddrMagneTarget;

    // --- Live telemetry (written by DLL, read by companion) ---
    float   m_teleLiveCamPosX;
    float   m_teleLiveCamPosY;
    float   m_teleLiveCamPosZ;
    float   m_teleLiveCamFocX;
    float   m_teleLiveCamFocY;
    float   m_teleLiveCamFocZ;
    float   m_teleLiveCamFOV;
    float   m_telePivotX;
    float   m_telePivotY;
    float   m_telePivotZ;
    float   m_teleMagneTargetX;
    float   m_teleMagneTargetY;
    float   m_teleMagneTargetZ;
    int32_t m_teleLiveShortcutMenu;
    uint8_t m_teleLiveMenuState;
    bool    m_statusScanning;
    uint32_t m_statusWritersFound;
    bool    m_patchMagneDetourActive;

    // --- Log queue (written by DLL, read by companion) ---
    char     m_logQueue[8][128];
    uint32_t m_logWriteIdx;
    bool     m_statusShutdownDone;

    // --- Commands (written by companion, read by DLL) ---
    bool m_reqDumpAob;
    bool m_reqResetScan;
    bool m_reqShutdown;
};

