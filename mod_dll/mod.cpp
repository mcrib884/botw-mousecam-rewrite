#include "mod.h"
#pragma comment(lib, "winmm.lib")
#include <vector>
#include <set>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <string>
#include <algorithm>
#include <TlHelp32.h>
#include <cstdint>
#include <stdio.h>
#include <immintrin.h>
#include <intrin.h>

// ============================================================================
// Why this DLL contains camera logic rather than being a pure memory relay:
//
// Two subsystems fundamentally require in-process execution:
//
// 1. Low-level mouse hook (SetWindowsHookExW / WH_MOUSE_LL) — Capturing scroll
//    events reliably requires a hook in the same desktop thread as the game.
//    A cross-process approach would miss events or add unpredictable latency.
//
// 2. Camera update latency — The camera control loop runs at ~200 Hz. Marshaling
//    mouse deltas across process boundaries via shared memory polling every 5ms
//    would add jitter and input lag. Direct in-process memory reads are instant.
//
// Result: the companion acts as a configuration UI + injection launcher, while
// the DLL handles all runtime camera logic. Some code appears duplicated between
// them (SetGlobalCursorVisibility, FindTargetWindow, key injection) because both
// processes need to manage cursor state and send input independently — the
// companion during reinject/reload, the DLL during active gameplay.
//
// Two threads touch game memory here:
//   - ScanAobThread: reads memory for pattern scanning (mostly read-only regions)
//   - CameraControlThread: reads/writes camera state at known offsets
// They operate on different address ranges and the game's memory is page-stable
// (Cemu doesn't unmap emulated RAM mid-frame). No formal mutex guards the
// overlap because in practice they never contend for the same bytes.
//
// CRITICAL_SECTION is used over std::mutex because on Windows it's a lightweight
// user-mode spinlock that only falls back to a kernel event under contention.
// For short-held locks in a game mod context (tens of microseconds), this
// avoids unnecessary kernel transitions.
// ============================================================================

extern "C" {
    uint8_t g_magnesisEnabled = 0;
    uint64_t g_magneHeartbeatCounter = 0;
    uint64_t g_magneIdealBase = 0;
    uint64_t g_magnesisZWriterReturn = 0;
    uint64_t g_magnesisYWriterReturn = 0;

    void AsmMagnesisZWriter();
    void AsmMagnesisYWriterExp();
    void AsmMagnesisZWriterExp();
}

namespace Mod {

    struct CemuVersionConfig {
        std::wstring name;
        std::string gameRomCameraAob;
        std::string magnesisAob;
        std::string shortcutMenuAob;
        std::string menuStateAob1;
        std::string menuStateAob2;
        size_t magnesisXOffset;
        size_t magnesisYOffset;
        size_t magnesisZOffset;
        char detourTargetAxis;
        size_t magnesisDetourSize;
    };

    static CemuVersionConfig GetCemuVersionConfig(bool experimental) {
        CemuVersionConfig cfg;
        if (experimental) {
            cfg.name = L"Cemu Experimental";
            cfg.gameRomCameraAob = "47 61 6D 65 52 6F 6D 43 61 6D 65 72 61 00";
            cfg.magnesisAob      = "45 0F 38 F1 74 2D 68 F3 0F 5A C0 F2 0F 10 AC 24 68 02 00 00 31 F6 66 0F 2E E5 41 0F 9B C6 40 0F 92 C6 44 20 F6 31 FF 66 0F 2E E5 40 0F 97 C7 45 31 C0 66 0F 2E E5 41 0F 9B C6 41 0F 94 C0 45 20 F0 45 31 C9 66 0F 2E E5 41 0F 9A C1 F2 0F 11 A4 24 88 00 00 00 41 89 5C 0D 70 0F CA 89 54 24 2C 45 0F 38 F0 74 0D 70 66 41 0F 6E E6 F2 0F 10 FC F3 0F 5A FF F2 0F 11 BC 24 60 01 00 00 66 41 0F 7E F6 45 0F 38 F1 74 2D 6C F3 0F 5A F6 F2 0F 11 B4 24 48 01 00 00 66 41 0F 7E E6 45 0F 38 F1 74 2D 70";
            cfg.shortcutMenuAob  = "41 0F 38 F1 9C 15 04 1C 00 00";
            cfg.menuStateAob1    = "41 0F 38 F1 44 0D 3C 89 44 24 34 C7 84 24 B8 02 00 00 58 3A 7E 03 BA 9C";
            cfg.menuStateAob2    = "41 0F 38 F1 44 0D 3C 89 44 24 30 83 AC 24 B0 02 00 00 02 8B 4C 24 78 89 C8 89 44 24 10 C7 84 24 B8 02 00 00 D4 3A";
            cfg.magnesisXOffset = 0x00;
            cfg.magnesisYOffset = 0x82;
            cfg.magnesisZOffset = 0x8D;
            cfg.detourTargetAxis = 'Z';
            cfg.magnesisDetourSize = 21;
        } else {
            cfg.name = L"Cemu 2.6";
            cfg.gameRomCameraAob = "47 61 6D 65 52 6F 6D 43 61 6D 65 72 61 00";
            cfg.magnesisAob      = "38 F0 74 1D 6C 66 41 0F 6E FE F2 44 0F 5A FD 66 45 0F 7E FE 45 0F 38 F1 74 1D 64 41 8B 54 1D 64 8B AC 24 80 00 00 00 45 0F 38 F0 74 2D 74 66 41 0F 6E D6 F3 0F 5A D2 F2 0F 12 D2 66 41 0F 7E F6 45 0F 38 F1 74 2D 68 F3 0F 5A F6 F2 0F 12 F6 66 44 0F 10 84 E4 68 02 00 00 66 41 0F 2E D0 0F 9A 84 24 8F 02 00 00 7A 1A 0F 92 84 24 8C 02 00 00 0F 97 84 24 8D 02 00 00 0F 94 84 24 8E 02 00 00 EB 18 C6 84 24 8C 02 00 00 00 C6 84 24 8D 02 00 00 00 C6 84 24 8E 02 00 00 00 41 89 54 1D 70 66 44 0F 10 8C E4 58 01 00 00 45 0F 38 F0 74 1D 70 66 45 0F 6E CE 66 41 0F 7E FE 45 0F 38 F1 74 2D 6C F3 0F 5A FF F2 0F 12 FF 66 45 0F 7E CE 45 0F 38 F1 74 2D 70 F3 45 0F 5A C9 F2 45 0F 12 C9 0F C8 89 44 24 2C 0F CA 89 54 24 04 66 0F 11 84 E4 08 01 00 00 66 0F 11 8C E4 F8 00 00 00 66 0F 11 94 E4 88 00 00 00 66 0F 11 9C E4 28 01 00 00 66 0F 11 A4 E4 78 02 00 00 66 0F 11 AC E4 18 01 00";
            cfg.shortcutMenuAob  = "41 0F 38 F1 9C 15 04 1C 00 00";
            cfg.menuStateAob1    = "41 0F 38 F1 44 15 3C 89 44 24 34 C7 84 24 B8 02 00 00 58 3A 7E 03 BA 9C";
            cfg.menuStateAob2    = "41 0F 38 F1 44 15 3C 89 44 24 30 83 AC 24 B0 02 00 00 02 8B 44 24 78 89 C2 89 54 24 10 C7 84 24 B8 02 00 00 D4 3A";
            cfg.magnesisXOffset = 0x40;
            cfg.magnesisYOffset = 0xBA;
            cfg.magnesisZOffset = 0xCE;
            cfg.detourTargetAxis = 'Z';
            cfg.magnesisDetourSize = 17;
        }
        return cfg;
    }

    class SharedMemoryCreator {
        public:
            SharedMemoryCreator() = default;
            ~SharedMemoryCreator() { Close(); }

            SharedMemoryCreator(const SharedMemoryCreator&) = delete;
            SharedMemoryCreator& operator=(const SharedMemoryCreator&) = delete;

            bool Create(const wchar_t* name) {
                if (m_layout) return true;
                m_hFile = CreateFileMappingW(
                    INVALID_HANDLE_VALUE,
                    nullptr,
                    PAGE_READWRITE,
                    0,
                    sizeof(SharedMemoryLayout),
                    name
                );
                if (!m_hFile) return false;

                m_layout = static_cast<SharedMemoryLayout*>(MapViewOfFile(
                    m_hFile,
                    FILE_MAP_ALL_ACCESS,
                    0,
                    0,
                    sizeof(SharedMemoryLayout)
                ));

                if (!m_layout) {
                    CloseHandle(m_hFile);
                    m_hFile = nullptr;
                    return false;
                }
                return true;
            }

            void Close() {
                if (m_layout) {
                    UnmapViewOfFile(m_layout);
                    m_layout = nullptr;
                }
                if (m_hFile) {
                    CloseHandle(m_hFile);
                    m_hFile = nullptr;
                }
            }

            SharedMemoryLayout* GetLayout() const { return m_layout; }

        private:
            HANDLE m_hFile = nullptr;
            SharedMemoryLayout* m_layout = nullptr;
    };

    static HMODULE g_hModule = nullptr;
    static SharedMemoryCreator g_sharedMemory;
    #define g_pSharedMemory (g_sharedMemory.GetLayout())

    static void DllLog(const char* format, ...) {
        // Clean test: suppress DIAG spam at source (both Debug/Release) — INFO still passes
        if (format[0] == '[' && format[1] == 'D' && format[2] == 'I' && format[3] == 'A' && format[4] == 'G') {
            return;
        }
        if (!g_pSharedMemory) return;
#ifndef _DEBUG
        // In Release, suppress [DEBUG] spam at the source — no queue slot wasted
        if (format[0] == '[' && format[1] == 'D' && format[2] == 'E' && format[3] == 'B' && format[4] == 'U' && format[5] == 'G') {
            return;
        }
#endif

        char msg[256] = {};
        va_list args;
        va_start(args, format);
        vsnprintf(msg, sizeof(msg), format, args);
        va_end(args);

        uint32_t idx = g_pSharedMemory->m_logWriteIdx % 8;
        memcpy(g_pSharedMemory->m_logQueue[idx], msg, sizeof(g_pSharedMemory->m_logQueue[idx]));
        g_pSharedMemory->m_logWriteIdx++;
    }

    // Master switch for non-essential per-candidate / per-frame camera diagnostics. The deployed
    // build is Debug config, so #ifdef _DEBUG guards do NOT keep spam out of the log. Flip this
    // to true ONLY while hunting a camera-flow bug; all gated sites emit as [DIAG] which DllLog
    // suppresses at source in every build. Zombie-camera recovery lines stay plain INFO.
    static constexpr bool g_verboseCamDiag = false;

    static std::atomic<int> g_runningThreads{0};

    static void OnThreadExit();
    static bool IsCompanionAlive();

    static HANDLE g_hScanThread = nullptr;
    static HANDLE g_hCameraControlThread = nullptr;
    static std::atomic<bool> g_scanning = false;
    static std::atomic<bool> g_cameraControlRunning = false;

    static std::atomic<uintptr_t> g_addrGameRomCamera{0};
    static std::atomic<uintptr_t> g_addrMagneTarget{0};
    static std::atomic<uintptr_t> g_addrShortcutMenu{0};
    static std::atomic<uintptr_t> g_addrMenuState{0};

    static std::atomic<float> g_liveCamPosX{0.0f};
    static std::atomic<float> g_liveCamPosY{0.0f};
    static std::atomic<float> g_liveCamPosZ{0.0f};
    static std::atomic<float> g_liveCamFocX{0.0f};
    static std::atomic<float> g_liveCamFocY{0.0f};
    static std::atomic<float> g_liveCamFocZ{0.0f};
    static std::atomic<float> g_liveCamFOV{0.0f};
    static std::atomic<float> g_livePivotX{0.0f};
    static std::atomic<float> g_livePivotY{0.0f};
    static std::atomic<float> g_livePivotZ{0.0f};

    static std::atomic<float> g_magneTargetX{0.0f};
    static std::atomic<float> g_magneTargetY{0.0f};
    static std::atomic<float> g_magneTargetZ{0.0f};

    static std::atomic<int32_t> g_liveShortcutMenu{-1};
    static std::atomic<uint8_t> g_liveMenuState{1};

    static std::atomic<bool> g_mousecamActive{false};

    enum class ScrollMenuType {
        None,
        Left,
        Right
    };

    struct Pattern {
        std::vector<unsigned char> bytes;
        std::vector<uint8_t> isWildcard;
    };

    static Pattern ParseAOB(const std::string& aobStr) {
        Pattern pat;
        size_t i = 0;
        auto hexCharToVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };

        while (i < aobStr.size()) {
            if (isspace(aobStr[i]) || aobStr[i] == '[' || aobStr[i] == ']') {
                i++;
                continue;
            }
            if (aobStr[i] == '?' && i + 1 < aobStr.size() && aobStr[i+1] == '?') {
                pat.bytes.push_back(0);
                pat.isWildcard.push_back(true);
                i += 2;
            } else if (aobStr[i] == '?') {
                pat.bytes.push_back(0);
                pat.isWildcard.push_back(true);
                i += 1;
            } else {
                int high = hexCharToVal(aobStr[i]);
                int low = (i + 1 < aobStr.size()) ? hexCharToVal(aobStr[i+1]) : -1;
                if (high != -1 && low != -1) {
                    unsigned char val = static_cast<unsigned char>((high << 4) | low);
                    pat.bytes.push_back(val);
                    pat.isWildcard.push_back(false);
                    i += 2;
                } else {
                    i++;
                }
            }
        }
        return pat;
    }

    enum class CpuSimdTier {
        SSE2 = 0,
        AVX2 = 1,
        AVX512 = 2
    };

    static CpuSimdTier GetCpuSimdTier() {
        static CpuSimdTier tier = []() {
            int cpuInfo[4] = { 0 };
            __cpuid(cpuInfo, 0);
            int numIds = cpuInfo[0];
            if (numIds >= 7) {
                __cpuid(cpuInfo, 7);
                bool hasAvx512F  = (cpuInfo[1] & (1 << 16)) != 0;
                bool hasAvx512BW = (cpuInfo[1] & (1 << 30)) != 0;
                if (hasAvx512F && hasAvx512BW) {
                    return CpuSimdTier::AVX512;
                }
                bool hasAvx2 = (cpuInfo[1] & (1 << 5)) != 0;
                if (hasAvx2) {
                    return CpuSimdTier::AVX2;
                }
            }
            return CpuSimdTier::SSE2;
        }();
        return tier;
    }

    static const char* GetCpuSimdTierName() {
        CpuSimdTier tier = GetCpuSimdTier();
        switch (tier) {
            case CpuSimdTier::AVX512: return "AVX-512 (64-byte ZMM)";
            case CpuSimdTier::AVX2:   return "AVX2 (32-byte YMM)";
            case CpuSimdTier::SSE2:   return "SSE2 (16-byte XMM)";
            default:                  return "Scalar Fallback";
        }
    }

    static bool SearchPatternAVX512(const unsigned char* buffer, size_t bufferSize, const Pattern& pattern, size_t firstNonWildcard, unsigned char firstByte, size_t& foundOffset) {
        size_t patternLen = pattern.bytes.size();
        if (bufferSize < patternLen) return false;
        size_t limit = bufferSize - patternLen;

        __m512i firstByteVec = _mm512_set1_epi8(firstByte);

        size_t i = 0;
        size_t maxSimdIndex = (bufferSize >= firstNonWildcard + 64) ? (bufferSize - firstNonWildcard - 64) : 0;
        for (; i <= limit && i <= maxSimdIndex; i += 64) {
            __m512i data = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(buffer + i + firstNonWildcard));
            uint64_t mask = _mm512_cmpeq_epi8_mask(data, firstByteVec);

            while (mask != 0) {
                unsigned long bitIdx;
                _BitScanForward64(&bitIdx, mask);
                mask &= ~(1ULL << bitIdx);

                size_t offset = i + bitIdx;
                if (offset + patternLen <= bufferSize) {
                    bool match = true;
                    for (size_t j = 0; j < patternLen; ++j) {
                        if (!pattern.isWildcard[j] && buffer[offset + j] != pattern.bytes[j]) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        uintptr_t matchAddr = reinterpret_cast<uintptr_t>(buffer + offset);
                        uintptr_t patData = reinterpret_cast<uintptr_t>(pattern.bytes.data());
                        if (patData != 0 && matchAddr >= patData && matchAddr < patData + patternLen) {
                            continue;
                        }
                        foundOffset = offset;
                        return true;
                    }
                }
            }
        }

        for (; i <= limit; ++i) {
            if (buffer[i + firstNonWildcard] == firstByte) {
                bool match = true;
                for (size_t j = 0; j < patternLen; ++j) {
                    if (!pattern.isWildcard[j] && buffer[i + j] != pattern.bytes[j]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    uintptr_t matchAddr = reinterpret_cast<uintptr_t>(buffer + i);
                    uintptr_t patData = reinterpret_cast<uintptr_t>(pattern.bytes.data());
                    if (patData != 0 && matchAddr >= patData && matchAddr < patData + patternLen) {
                        continue;
                    }
                    foundOffset = i;
                    return true;
                }
            }
        }

        return false;
    }

    static bool SearchPatternAVX2(const unsigned char* buffer, size_t bufferSize, const Pattern& pattern, size_t firstNonWildcard, unsigned char firstByte, size_t& foundOffset) {
        size_t patternLen = pattern.bytes.size();
        if (bufferSize < patternLen) return false;
        size_t limit = bufferSize - patternLen;
        
        __m256i firstByteVec = _mm256_set1_epi8(firstByte);
        
        size_t i = 0;
        size_t maxSimdIndex = (bufferSize >= firstNonWildcard + 32) ? (bufferSize - firstNonWildcard - 32) : 0;
        for (; i <= limit && i <= maxSimdIndex; i += 32) {
            __m256i data = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(buffer + i + firstNonWildcard));
            __m256i cmp = _mm256_cmpeq_epi8(data, firstByteVec);
            unsigned int mask = _mm256_movemask_epi8(cmp);
            
            while (mask != 0) {
                unsigned long bitIdx;
                _BitScanForward(&bitIdx, mask);
                mask &= ~(1u << bitIdx);
                
                size_t offset = i + bitIdx;
                if (offset + patternLen <= bufferSize) {
                    bool match = true;
                    for (size_t j = 0; j < patternLen; ++j) {
                        if (!pattern.isWildcard[j] && buffer[offset + j] != pattern.bytes[j]) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        uintptr_t matchAddr = reinterpret_cast<uintptr_t>(buffer + offset);
                        uintptr_t patData = reinterpret_cast<uintptr_t>(pattern.bytes.data());
                        if (patData != 0 && matchAddr >= patData && matchAddr < patData + patternLen) {
                            continue;
                        }
                        foundOffset = offset;
                        return true;
                    }
                }
            }
        }
        
        for (; i <= limit; ++i) {
            if (buffer[i + firstNonWildcard] == firstByte) {
                bool match = true;
                for (size_t j = 0; j < patternLen; ++j) {
                    if (!pattern.isWildcard[j] && buffer[i + j] != pattern.bytes[j]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    uintptr_t matchAddr = reinterpret_cast<uintptr_t>(buffer + i);
                    uintptr_t patData = reinterpret_cast<uintptr_t>(pattern.bytes.data());
                    if (patData != 0 && matchAddr >= patData && matchAddr < patData + patternLen) {
                        continue;
                    }
                    foundOffset = i;
                    return true;
                }
            }
        }
        
        return false;
    }

    static bool SearchPatternSSE2(const unsigned char* buffer, size_t bufferSize, const Pattern& pattern, size_t firstNonWildcard, unsigned char firstByte, size_t& foundOffset) {
        size_t patternLen = pattern.bytes.size();
        if (bufferSize < patternLen) return false;
        size_t limit = bufferSize - patternLen;
        
        __m128i firstByteVec = _mm_set1_epi8(firstByte);
        
        size_t i = 0;
        size_t maxSimdIndex = (bufferSize >= firstNonWildcard + 16) ? (bufferSize - firstNonWildcard - 16) : 0;
        for (; i <= limit && i <= maxSimdIndex; i += 16) {
            __m128i data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(buffer + i + firstNonWildcard));
            __m128i cmp = _mm_cmpeq_epi8(data, firstByteVec);
            unsigned int mask = _mm_movemask_epi8(cmp);
            
            while (mask != 0) {
                unsigned long bitIdx;
                _BitScanForward(&bitIdx, mask);
                mask &= ~(1u << bitIdx);
                
                size_t offset = i + bitIdx;
                if (offset + patternLen <= bufferSize) {
                    bool match = true;
                    for (size_t j = 0; j < patternLen; ++j) {
                        if (!pattern.isWildcard[j] && buffer[offset + j] != pattern.bytes[j]) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        uintptr_t matchAddr = reinterpret_cast<uintptr_t>(buffer + offset);
                        uintptr_t patData = reinterpret_cast<uintptr_t>(pattern.bytes.data());
                        if (patData != 0 && matchAddr >= patData && matchAddr < patData + patternLen) {
                            continue;
                        }
                        foundOffset = offset;
                        return true;
                    }
                }
            }
        }
        
        for (; i <= limit; ++i) {
            if (buffer[i + firstNonWildcard] == firstByte) {
                bool match = true;
                for (size_t j = 0; j < patternLen; ++j) {
                    if (!pattern.isWildcard[j] && buffer[i + j] != pattern.bytes[j]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    uintptr_t matchAddr = reinterpret_cast<uintptr_t>(buffer + i);
                    uintptr_t patData = reinterpret_cast<uintptr_t>(pattern.bytes.data());
                    if (patData != 0 && matchAddr >= patData && matchAddr < patData + patternLen) {
                        continue;
                    }
                    foundOffset = i;
                    return true;
                }
            }
        }
        
        return false;
    }

    static bool SearchPattern(const unsigned char* buffer, size_t bufferSize, const Pattern& pattern, size_t& foundOffset) {
        if (pattern.bytes.empty() || bufferSize < pattern.bytes.size()) {
            return false;
        }

        size_t firstNonWildcard = 0;
        while (firstNonWildcard < pattern.bytes.size() && pattern.isWildcard[firstNonWildcard]) {
            firstNonWildcard++;
        }

        if (firstNonWildcard == pattern.bytes.size()) {
            foundOffset = 0;
            return true;
        }

        unsigned char firstByte = pattern.bytes[firstNonWildcard];
        
        CpuSimdTier tier = GetCpuSimdTier();
        if (tier == CpuSimdTier::AVX512) {
            return SearchPatternAVX512(buffer, bufferSize, pattern, firstNonWildcard, firstByte, foundOffset);
        } else if (tier == CpuSimdTier::AVX2) {
            return SearchPatternAVX2(buffer, bufferSize, pattern, firstNonWildcard, firstByte, foundOffset);
        } else {
            return SearchPatternSSE2(buffer, bufferSize, pattern, firstNonWildcard, firstByte, foundOffset);
        }
    }

    static uintptr_t g_emulatedRamBase = 0;
    static size_t g_emulatedRamSize = 0;

    // Lost-camera rejection (death/level-change zombie bug): the game creates a fresh camera struct
    // each load and the previous block stays allocated — its bytes remain readable, plausible, and
    // it still scores 110 in VerifyGameRomCamera, but it is no longer what the renderer reads, so
    // writes land on a corpse. When a loss is detected (FOV stall, hunter 3x-no-writer, or the
    // scanner's marker-verify failure) we reject that exact address on the next GameRomCamera scan
    // and wait out the load before scanning at all. DLL-process atomics: camera thread arms,
    // scanner thread consumes. Declared here so ScanGameRomCameraImmediate can see them.
    static std::atomic<uintptr_t>    g_rejectCamAddr{0};
    static std::atomic<int>          g_resetDelayMs{0};
    // Decisive = the marker-verify failure proves the block died. Stall- and hunter-armed rejections
    // are non-decisive: cutscenes and title screens freeze FOV writes on the LIVE camera too, and
    // permanently rejecting it would loop "candidates rejected" forever. Non-decisive rejections
    // expire after 2 consecutive passes in which the rejected address was the only valid candidate.
    static std::atomic<bool>         g_camDefinitivelyLost{false};
    static std::atomic<int>          g_rejectEmptyPasses{0};

    static int32_t ReadInt32BE(uintptr_t address);

    static std::vector<Pattern> g_writerBlacklist;

    static void LoadWriterBlacklist() {
        g_writerBlacklist.clear();

        // 1. Internalized default patterns for non-camera or cutscene-specific writers that should NOT be NOP'd
        std::vector<std::string> internalPatterns = {
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 45 0F 38 F0 74 ?? ?? 66 41 0F 6E ?? ?? ?? ?? ?? ?? 66 41 0F 7E EE 45 0F 38 F1 74 ?? 00 F3 0F 5A ED F2 0F 12 ED 45 0F 38 F0 74 ?? ?? 66 41 0F 6E ?? 66 41 0F 7E D6 45 0F 38 F1 74 ?? 04 F3 0F 5A D2 F2 0F 12 D2 45 0F 38 F0 74 ?? ?? 66 41 0F 6E ?? 66 41 0F 7E DE 45 0F 38 F1 74 ?? 08 F3 0F 5A DB F2 0F 12 DB",
            "41 0F 6E F6 0F C8 89 44 24 1C 0F CB 89 5C 24 24 45 89 4C 15 4C 0F CD 89 6C 24 2C 66 41 0F 7E F6 45 0F 38 F1 74 3D 00 F3 0F 5A F6 F2 0F 12 F6 45 0F 38 F0 74 15 48 66 41 0F 6E C6 66 41 0F 7E C6 45 0F 38 F1 74 3D 04 F3 0F 5A C0 F2 0F 12 C0 45 0F 38 F0 74 15 4C 66 41 0F 6E CE 8B 54 24 70 89 D0 89 44 24 10 66 41 0F 7E CE 45 0F 38 F1 74 3D 08 F3 0F 5A C9 F2 0F 12 C9",
            "66 41 0F 6E F6 F2 44 0F 5A FC 66 45 0F 7E FE 45 0F 38 F1 74 1D 7C 41 8B 74 1D 7C 66 41 0F 7E EE [ 45 0F 38 F1 74 15 00 ] F3 0F 5A ED F2 0F 12 ED 41 89 74 1D 1C 45 0F 38 F0 74 1D 1C 66 41 0F 6E D6 8B 7C 24 7C 89 FB 89 5C 24 10 66 41 0F 7E F6 [ 45 0F 38 F1 74 15 04 ] F3 0F 5A F6 F2 0F 12 F6 89 D3 89 5C 24 14 66 41 0F 7E D6 [ 45 0F 38 F1 74 15 08 ] F3 0F 5A D2 F2 0F 12 D2 0F C8 89 44 24 1C 0F CD 89 6C 24 24 0F CE 89 74 24 04 66 0F 11 84 E4 38",
            "6E C6 F2 45 0F 10 C8 F3 45 0F 5A C9 F2 44 0F 11 8C 24 10 01 00 00 41 89 4C 2D 1C 66 45 0F 7E C6 [ 45 0F 38 F1 74 1D 00 ] F3 45 0F 5A C0 F2 44 0F 11 84 24 08 01 00 00 45 0F 38 F0 74 2D 18 66 41 0F 6E E6 F2 0F 10 EC F3 0F 5A ED 66 41 0F 7E E6 [ 45 0F 38 F1 74 1D 04 ] F3 0F 5A E4 45 0F 38 F0 74 2D 1C 66 41 0F 6E F6 F2 0F 10 D6 0F C8 89 44 24 28 F3 0F 5A D2 66 41 0F 7E F6 [ 45 0F 38 F1 74 1D 08 ] F3 0F 5A F6 8B 54 24 7C 45 0F 38 F0 B4 15 84 00 00 00 66 41 0F 6E DE F3 0F 5A DB F2 0F 10 FB F2",
            "66 41 0F 6E E6 F2 0F 10 EC F3 0F 5A ED F2 0F 11 AC 24 00 01 00 00 41 89 4C 05 4C 66 41 0F 7E E6 [ 45 0F 38 F1 74 2D 00 ] F3 0F 5A E4 F2 0F 11 A4 24 F8 00 00 00 45 0F 38 F0 74 05 48 66 41 0F 6E C6 F2 0F 10 C8 F3 0F 5A C9 F2 0F 11 8C 24 10 01 00 00 66 41 0F 7E C6 [ 45 0F 38 F1 74 2D 04 ] F3 0F 5A C0 F2 0F 11 84 24 08 01 00 00 45 0F 38 F0 74 05 4C 66 41 0F 6E D6 0F CE 89 74 24 20 0F CF 89 7C 24 28 F2 0F 10 DA F3 0F 5A DB F2 0F 11 9C 24 20 01 00 00 8B 54 24 70 89 D0 89 44 24 10 66 41 0F 7E D6 [ 45 0F 38 F1 74 2D 08 ] F3 0F 5A D2 F2 0F 11 94 24 18 01 00 00 0F C9 89 4C 24 30 C7 84 24 B8 02 00 00 20 3B BA 02 BA C0",
            "F8 66 45 0F 7E FE 45 0F 38 F1 74 ?? 7C F2 0F 11 84 24 F8 00 00 00 41 8B 6C 0D 7C 66 41 0F 7E CE 45 0F 38 F1 74 ?? 00 F3 0F 5A C9 F2 0F 11 8C 24 88 00 00 00 41 89 6C 0D 1C 45 0F 38 F0 74 ?? 1C 66 41 0F 6E E6 F2 0F 10 EC F3 0F 5A ED F2 0F 11 AC 24 30 01 00 00 8B 74 24 7C 89 F1 89 4C 24 10 0F CA 89 54 24 1C 66 41 0F 7E D6 45 0F 38 F1 74 ?? 04 F3 0F 5A D2 F2 0F 11 94 24 08 01 00 00 0F CB 89 5C 24 24 89 C1 89 4C 24 14 66 41 0F 7E E6 45 0F 38 F1 74 ?? 08 F3 0F 5A E4 F2 0F 11 A4 24 28 01 00 00 0F CD 89 6C 24 04"
        };
        for (const auto& s : internalPatterns) {
            Pattern pat = ParseAOB(s);
            if (!pat.bytes.empty()) {
                g_writerBlacklist.push_back(pat);
            }
        }

        // 2. Load external patterns relative to DLL path
        wchar_t dllPath[MAX_PATH];
        if (GetModuleFileNameW(g_hModule, dllPath, MAX_PATH) > 0) {
            wchar_t* lastSlash = wcsrchr(dllPath, L'\\');
            if (lastSlash) {
                *lastSlash = L'\0';
                std::wstring blacklistPath = std::wstring(dllPath) + L"\\writer_blacklist.txt";
                FILE* f = nullptr;
                if (_wfopen_s(&f, blacklistPath.c_str(), L"r") == 0) {
                    char line[1024];
                    while (fgets(line, sizeof(line), f)) {
                        std::string s(line);
                        // Trim leading spaces
                        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                            return !std::isspace(ch);
                        }));
                        if (s.empty() || s[0] == '#' || (s.size() >= 2 && s[0] == '/' && s[1] == '/')) {
                            continue;
                        }
                        if (s.find("AOB Dump") != std::string::npos) continue;
                        Pattern pat = ParseAOB(s);
                        if (!pat.bytes.empty()) {
                            // Avoid duplicates
                            bool duplicate = false;
                            for (const auto& existing : g_writerBlacklist) {
                                if (existing.bytes == pat.bytes && existing.isWildcard == pat.isWildcard) {
                                    duplicate = true;
                                    break;
                                }
                            }
                            if (!duplicate) {
                                g_writerBlacklist.push_back(pat);
                            }
                        }
                    }
                    fclose(f);
                }
            }
        }
    }

    static void ResetScannerState() {
        g_emulatedRamBase = 0;
        g_emulatedRamSize = 0;
    }

    static bool FindEmulatedRam(uintptr_t& ramBase, size_t& ramSize) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        uintptr_t start = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        uintptr_t current = start;
        MEMORY_BASIC_INFORMATION mbi;

        // Pass 1: Look for a single committed read-write region >= 1GB (Rust parity)
        while (current < end) {
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(current), &mbi, sizeof(mbi))) {
                break;
            }

            if (mbi.State == MEM_COMMIT && mbi.RegionSize >= 0x40000000 &&
                (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
                !(mbi.Protect & PAGE_GUARD)) {
                ramBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                ramSize = mbi.RegionSize;
                return true;
            }

            current += mbi.RegionSize;
        }

        // Pass 2: Fallback to original AllocationBase logic
        current = start;
        while (current < end) {
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(current), &mbi, sizeof(mbi))) {
                break;
            }

            if (mbi.State == MEM_COMMIT && mbi.AllocationBase != nullptr) {
                uintptr_t allocBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
                size_t totalSize = 0;
                uintptr_t checkAddr = allocBase;
                MEMORY_BASIC_INFORMATION subMbi;

                while (checkAddr < end) {
                    if (!VirtualQuery(reinterpret_cast<LPCVOID>(checkAddr), &subMbi, sizeof(subMbi))) {
                        break;
                    }
                    if (subMbi.AllocationBase != mbi.AllocationBase) {
                        break;
                    }
                    totalSize += subMbi.RegionSize;
                    checkAddr += subMbi.RegionSize;
                }

                if (totalSize >= 0x40000000) {
                    ramBase = allocBase;
                    ramSize = totalSize;
                    return true;
                }
                
                current = checkAddr;
                continue;
            }

            current += mbi.RegionSize;
        }

        return false;
    }

    static bool SafeReadMarker(uintptr_t addr, uint16_t& out);
    static float ReadFloatBE(uintptr_t address);
    static void PollHooksAndSyncSharedMemory();

    static inline bool IsResetRequested() {
        return g_pSharedMemory && (g_pSharedMemory->m_reqResetScan || g_pSharedMemory->m_reqResetPreserveMenu);
    }

    static bool ScanProcessAOB(const Pattern& pattern, uintptr_t& foundAddress) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        uintptr_t start = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t current = start;
        size_t chunkSize = 2 * 1024 * 1024;
        size_t overlap = pattern.bytes.size();

        // 1. Get our own module's allocation base to avoid matching patterns in our own binary
        uintptr_t ourAllocBase = 0;
        MEMORY_BASIC_INFORMATION ourMbi;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(g_hModule), &ourMbi, sizeof(ourMbi))) {
            ourAllocBase = reinterpret_cast<uintptr_t>(ourMbi.AllocationBase);
        }

        // 2. Get the stack's allocation base (using a local variable address)
        uintptr_t stackAllocBase = 0;
        MEMORY_BASIC_INFORMATION stackMbi;
        int stackVar = 0;
        if (VirtualQuery(&stackVar, &stackMbi, sizeof(stackMbi))) {
            stackAllocBase = reinterpret_cast<uintptr_t>(stackMbi.AllocationBase);
        }

        while (current < end) {
            if (g_pSharedMemory && g_pSharedMemory->m_reqShutdown) {
                return false;
            }
            if (IsResetRequested()) return false;
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(current), &mbi, sizeof(mbi))) {
                break;
            }

            uintptr_t pageAllocBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
            bool isOurMemory = (ourAllocBase != 0 && pageAllocBase == ourAllocBase) ||
                               (stackAllocBase != 0 && pageAllocBase == stackAllocBase);

            bool scanThisPage = !isOurMemory && (mbi.State == MEM_COMMIT) &&
                                (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED) &&
                                (mbi.Protect != 0) &&
                                !(mbi.Protect & PAGE_NOACCESS) &&
                                !(mbi.Protect & PAGE_GUARD);

            if (scanThisPage) {
                uintptr_t regionAddress = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                size_t regionSize = mbi.RegionSize;

                for (size_t offset = 0; offset < regionSize; offset += (chunkSize > overlap ? chunkSize - overlap : chunkSize)) {
                    if (g_pSharedMemory && g_pSharedMemory->m_reqShutdown) {
                        return false;
                    }
                    if (IsResetRequested()) return false;
                    PollHooksAndSyncSharedMemory();
                    size_t toRead = (std::min)(chunkSize, regionSize - offset);
                    __try {
                        size_t matchOffset = 0;
                        if (SearchPattern(reinterpret_cast<const unsigned char*>(regionAddress + offset), toRead, pattern, matchOffset)) {
                            foundAddress = regionAddress + offset + matchOffset;
                            return true;
                        }
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {
                    }
                    if (toRead < chunkSize) break;
                }
            }
            current += mbi.RegionSize;
        }
        return false;
    }

    static bool ScanProcessAOBAll(const Pattern& pattern, std::vector<uintptr_t>& foundAddresses) {
        foundAddresses.clear();
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        uintptr_t start = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t current = start;
        size_t chunkSize = 2 * 1024 * 1024;
        size_t overlap = pattern.bytes.size();

        uintptr_t ourAllocBase = 0;
        MEMORY_BASIC_INFORMATION ourMbi;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(g_hModule), &ourMbi, sizeof(ourMbi))) {
            ourAllocBase = reinterpret_cast<uintptr_t>(ourMbi.AllocationBase);
        }

        uintptr_t stackAllocBase = 0;
        MEMORY_BASIC_INFORMATION stackMbi;
        int stackVar = 0;
        if (VirtualQuery(&stackVar, &stackMbi, sizeof(stackMbi))) {
            stackAllocBase = reinterpret_cast<uintptr_t>(stackMbi.AllocationBase);
        }

        while (current < end) {
            if (g_pSharedMemory && g_pSharedMemory->m_reqShutdown) {
                return false;
            }
            if (IsResetRequested()) return false;
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(current), &mbi, sizeof(mbi))) {
                break;
            }

            uintptr_t pageAllocBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
            bool isOurMemory = (ourAllocBase != 0 && pageAllocBase == ourAllocBase) ||
                               (stackAllocBase != 0 && pageAllocBase == stackAllocBase);

            bool scanThisPage = !isOurMemory && (mbi.State == MEM_COMMIT) &&
                                (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED) &&
                                (mbi.Protect != 0) &&
                                !(mbi.Protect & PAGE_NOACCESS) &&
                                !(mbi.Protect & PAGE_GUARD);

            if (scanThisPage) {
                uintptr_t regionAddress = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                size_t regionSize = mbi.RegionSize;

                for (size_t offset = 0; offset < regionSize; offset += (chunkSize > overlap ? chunkSize - overlap : chunkSize)) {
                    if (g_pSharedMemory && g_pSharedMemory->m_reqShutdown) {
                        return false;
                    }
                    if (IsResetRequested()) return false;
                    PollHooksAndSyncSharedMemory();
                    size_t toRead = (std::min)(chunkSize, regionSize - offset);
                    __try {
                        size_t searchPos = 0;
                        while (searchPos + pattern.bytes.size() <= toRead) {
                            size_t matchOffset = 0;
                            if (SearchPattern(reinterpret_cast<const unsigned char*>(regionAddress + offset + searchPos), toRead - searchPos, pattern, matchOffset)) {
                                uintptr_t matchAddr = regionAddress + offset + searchPos + matchOffset;
                                foundAddresses.push_back(matchAddr);
                                searchPos += matchOffset + 1;
                            } else {
                                break;
                            }
                        }
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {
                    }
                    if (toRead < chunkSize) break;
                }
            }
            current += mbi.RegionSize;
        }
        return !foundAddresses.empty();
    }

    static bool VerifyGameRomCamera(uintptr_t base, int& score) {
        score = 0;
        if (base == 0) return false;

        uint16_t marker = 0;
        if (!SafeReadMarker(base, marker)) return false;

        // FOV at base + 0x654 — original Verify (not touched for hunter)
        float fov = ReadFloatBE(base + 0x654);
        if (std::isnan(fov) || std::isinf(fov) || fov < 0.1f || fov > 0.99f) {
            return false;
        }

        // Camera position at base + 0x550, + 0x554, + 0x558
        float posX = ReadFloatBE(base + 0x550);
        float posY = ReadFloatBE(base + 0x554);
        float posZ = ReadFloatBE(base + 0x558);
        if (std::isnan(posX) || std::isinf(posX) || std::isnan(posY) || std::isinf(posY) || std::isnan(posZ) || std::isinf(posZ)) {
            return false;
        }
        if (fabs(posX) > 30000.0f || fabs(posZ) > 30000.0f || posY < -3000.0f || posY > 15000.0f) {
            return false;
        }

        // Camera focus at gc_addr + 0x0C, + 0x10, + 0x14 = base + 0x63C, 0x640, 0x644
        float focX = ReadFloatBE(base + 0x63C);
        float focY = ReadFloatBE(base + 0x640);
        float focZ = ReadFloatBE(base + 0x644);
        if (std::isnan(focX) || std::isinf(focX) || std::isnan(focY) || std::isinf(focY) || std::isnan(focZ) || std::isinf(focZ)) {
            return false;
        }
        if (fabs(focX) > 30000.0f || fabs(focZ) > 30000.0f) {
            return false;
        }

        // Pivot coordinates at base + 0x674, + 0x678, + 0x67C
        float pivX = ReadFloatBE(base + 0x674);
        float pivY = ReadFloatBE(base + 0x678);
        float pivZ = ReadFloatBE(base + 0x67C);
        if (std::isnan(pivX) || std::isinf(pivX) || std::isnan(pivY) || std::isinf(pivY) || std::isnan(pivZ) || std::isinf(pivZ)) {
            return false;
        }

        // Hard reject: any field exactly 1.0 is a known fake signature (e.g. Foc=[1,1,1])
        auto isOne = [](float v) { return v == 1.0f; };
        if (isOne(fov) || isOne(posX) || isOne(posY) || isOne(posZ) ||
            isOne(focX) || isOne(focY) || isOne(focZ) ||
            isOne(pivX) || isOne(pivY) || isOne(pivZ)) {
            return false;
        }

        score = 10;
        if (fov >= 0.4f && fov <= 0.99f) score += 20;
        if (posX != 0.0f || posY != 0.0f || posZ != 0.0f) score += 25;
        if (focX != 0.0f || focY != 0.0f || focZ != 0.0f) score += 20;
        if (pivX != 0.0f || pivY != 0.0f || pivZ != 0.0f) score += 10;

        float dist = sqrtf((posX - focX) * (posX - focX) + (posY - focY) * (posY - focY) + (posZ - focZ) * (posZ - focZ));
        if (dist >= 0.5f && dist <= 150.0f) score += 25;

        return true;
    }

    static bool VerifyMagneTargetSig(uintptr_t cand, const CemuVersionConfig& vCfg, bool experimental, int& score) {
        score = 0;
        if (cand == 0) return false;

        // 1. Verify memory page is committed and executable
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(cand), &mbi, sizeof(mbi))) {
            return false;
        }
        if (mbi.State != MEM_COMMIT || !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            return false;
        }
        score += 40;

        // 2. Verify instruction opcodes at Y and Z write offsets
        uint8_t yBytes[7] = {0}, zBytes[7] = {0};
        SIZE_T readBytes = 0;

        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(cand + vCfg.magnesisYOffset), yBytes, sizeof(yBytes), &readBytes) && readBytes == sizeof(yBytes)) {
            if (yBytes[0] == 0x45 && yBytes[1] == 0x0F && yBytes[2] == 0x38 && yBytes[3] == 0xF1 && yBytes[6] == 0x6C) {
                score += 30;
            }
        }
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(cand + vCfg.magnesisZOffset), zBytes, sizeof(zBytes), &readBytes) && readBytes == sizeof(zBytes)) {
            if (zBytes[0] == 0x45 && zBytes[1] == 0x0F && zBytes[2] == 0x38 && zBytes[3] == 0xF1 && zBytes[6] == 0x70) {
                score += 30;
            }
        }

        return (score >= 40);
    }

    // -------------------------------------------------------------------------
    // GameRomCamera immediate-verify scan.
    // Scans memory like ScanProcessAOBAll but verifies each candidate immediately
    // via VerifyGameRomCamera instead of batch-collecting all matches first.
    // This ensures the best candidate is tracked incrementally and logging
    // happens per-hit as the scan progresses.
    // Separated into its own function to keep __try out of ScanAobThread
    // (which owns std::vector tasks and would trigger C2712).
    // -------------------------------------------------------------------------
    static void ScanGameRomCameraImmediate(const Pattern& pat, uintptr_t& outBest, int& outBestScore, uintptr_t& outFallback, size_t& outTotal, int& outRejectedWasValid) {
        outBest = 0;
        outBestScore = -1;
        outFallback = 0;
        outTotal = 0;
        outRejectedWasValid = 0;

        if (pat.bytes.empty()) return;

        SYSTEM_INFO si;
        GetSystemInfo(&si);
        uintptr_t start = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        uintptr_t current = start;
        size_t chunkSize = 2 * 1024 * 1024;
        size_t overlap = pat.bytes.size();

        uintptr_t ourAllocBase = 0;
        MEMORY_BASIC_INFORMATION ourMbi;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(g_hModule), &ourMbi, sizeof(ourMbi))) {
            ourAllocBase = reinterpret_cast<uintptr_t>(ourMbi.AllocationBase);
        }
        uintptr_t stackAllocBase = 0;
        MEMORY_BASIC_INFORMATION stackMbi;
        int stackVar = 0;
        if (VirtualQuery(&stackVar, &stackMbi, sizeof(stackMbi))) {
            stackAllocBase = reinterpret_cast<uintptr_t>(stackMbi.AllocationBase);
        }

        MEMORY_BASIC_INFORMATION mbi;
        while (current < end) {
            if (g_pSharedMemory && g_pSharedMemory->m_reqShutdown) break;
            if (IsResetRequested()) return;
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(current), &mbi, sizeof(mbi))) break;

            uintptr_t pageAllocBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
            bool isOurMemory = (ourAllocBase != 0 && pageAllocBase == ourAllocBase) ||
                               (stackAllocBase != 0 && pageAllocBase == stackAllocBase);
            bool scanThisPage = !isOurMemory && (mbi.State == MEM_COMMIT) &&
                                (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED) &&
                                (mbi.Protect != 0) &&
                                !(mbi.Protect & PAGE_NOACCESS) &&
                                !(mbi.Protect & PAGE_GUARD);

            if (scanThisPage) {
                uintptr_t regionAddress = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                size_t regionSize = mbi.RegionSize;
                for (size_t offset = 0; offset < regionSize; offset += (chunkSize > overlap ? chunkSize - overlap : chunkSize)) {
                    if (g_pSharedMemory && g_pSharedMemory->m_reqShutdown) break;
                    if (IsResetRequested()) return;
                    PollHooksAndSyncSharedMemory();
                    size_t toRead = (std::min)(chunkSize, regionSize - offset);
                    __try {
                        size_t searchPos = 0;
                        while (searchPos + pat.bytes.size() <= toRead) {
                            size_t matchOffset = 0;
                            if (SearchPattern(reinterpret_cast<const unsigned char*>(regionAddress + offset + searchPos), toRead - searchPos, pat, matchOffset)) {
                                uintptr_t rawAddr = regionAddress + offset + searchPos + matchOffset;
                                uintptr_t cand = rawAddr - 0x10;
                                outTotal++;
                                uintptr_t rej = g_rejectCamAddr.load();
                                if (rej != 0 && cand == rej) {
                                    int rejScore = 0;
                                    if (VerifyGameRomCamera(cand, rejScore)) outRejectedWasValid = 1;
                                    DllLog("[INFO] Skipping rejected zombie camera 0x%llX (score would be %d)", (unsigned long long)cand, rejScore);
                                    searchPos += matchOffset + 1;
                                    continue;
                                }
                                if (outFallback == 0) outFallback = cand;
                                int score = 0;
                                bool valid = VerifyGameRomCamera(cand, score);
                                if (g_verboseCamDiag) {
                                    float fov = ReadFloatBE(cand + 0x654);
                                    float posX = ReadFloatBE(cand + 0x550);
                                    float posY = ReadFloatBE(cand + 0x554);
                                    float posZ = ReadFloatBE(cand + 0x558);
                                    float focX = ReadFloatBE(cand + 0x63C);
                                    float focY = ReadFloatBE(cand + 0x640);
                                    float focZ = ReadFloatBE(cand + 0x644);
                                    DllLog("[DIAG] Match [%zu] at 0x%llX: Score=%d (FOV=%.2f, Pos=[%.1f, %.1f, %.1f], Foc=[%.1f, %.1f, %.1f]) -> %s",
                                           outTotal, cand, score, fov, posX, posY, posZ, focX, focY, focZ, valid ? "VALID" : "REJECTED");
                                }
                                if (valid) {
                                    if (score > outBestScore) {
                                        outBestScore = score;
                                        outBest = cand;
                                    }
                                    // Stop scanning immediately once a valid candidate is verified
                                    return;
                                }
                                searchPos += matchOffset + 1;
                            } else {
                                break;
                            }
                        }
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                    }
                    if (toRead < chunkSize) break;
                }
            }
            current += mbi.RegionSize;
        }
    }

    // -------------------------------------------------------------------------
    // CodePatch: used for magnesis detour only. Camera writers are now handled
    // dynamically via the VEH page-guard system below.
    // -------------------------------------------------------------------------
    struct CodePatch {
        uintptr_t address;
        size_t size;
        std::vector<uint8_t> g_originalBytes;
        bool active;

        bool Backup() {
            if (address == 0 || size == 0) return false;
            g_originalBytes.resize(size);
            memcpy(g_originalBytes.data(), (const void*)address, size);
            return true;
        }

        bool Restore() {
            if (address == 0 || size == 0 || g_originalBytes.empty()) return false;
            DWORD oldProtect;
            if (VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                memcpy((LPVOID)address, g_originalBytes.data(), size);
                VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(), (LPCVOID)address, size);
                active = false;
                return true;
            }
            return false;
        }

        bool ApplyNop() {
            if (address == 0 || size == 0 || g_originalBytes.empty()) return false;
            std::vector<uint8_t> nops(size, 0x90);
            DWORD oldProtect;
            if (VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                memcpy((LPVOID)address, nops.data(), size);
                VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(), (LPCVOID)address, size);
                active = true;
                return true;
            }
            return false;
        }

        bool ApplyBytes(const uint8_t* bytes, size_t len) {
            if (address == 0 || size == 0 || g_originalBytes.empty() || len != size) return false;
            DWORD oldProtect;
            if (VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                if (len >= 14 && bytes[0] == 0xFF && bytes[1] == 0x25 && bytes[2] == 0x00 && bytes[3] == 0x00 && bytes[4] == 0x00 && bytes[5] == 0x00) {
                    // Atomic detour write to prevent race conditions:
                    // 1. Write the target address at offset 6 (and NOPs) first
                    memcpy((LPVOID)(address + 6), bytes + 6, len - 6);
                    // 2. Perform atomic 64-bit write of the first 8 bytes
                    uint64_t first8;
                    memcpy(&first8, bytes, 8);
                    InterlockedExchange64((LONG64*)address, (LONG64)first8);
                } else {
                    memcpy((LPVOID)address, bytes, len);
                }
                VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(), (LPCVOID)address, len);
                active = true;
                return true;
            }
            return false;
        }

        bool InjectDetour(uintptr_t targetFunction) {
            if (address == 0 || size < 14 || g_originalBytes.empty()) return false;
            std::vector<uint8_t> bytes(size, 0x90);
            bytes[0] = 0xFF;
            bytes[1] = 0x25;
            bytes[2] = 0x00;
            bytes[3] = 0x00;
            bytes[4] = 0x00;
            bytes[5] = 0x00;
            *(uint64_t*)(&bytes[6]) = targetFunction;
            return ApplyBytes(bytes.data(), size);
        }

    };

    static CRITICAL_SECTION g_patchCS;

    static CodePatch g_magneDetourPatch = {};
    static CodePatch g_magneXPatch = {};
    static CodePatch g_magneYPatch = {};
    static CodePatch g_magneZPatch = {};
    static bool g_magnePatchesInitialized = false;

    static CodePatch g_shortcutHookPatch = {};
    static LPVOID g_shortcutTrampoline = nullptr;
    static std::atomic<uintptr_t> g_tempShortcutAddress{0};
    static std::atomic<int32_t> g_tempShortcutValue{0};
    static bool g_shortcutHookActive = false;

    // --- MenuState hook ---
    struct MenuStateWrite {
        uintptr_t address;
        int32_t value;
        uint32_t writerId;
    };
    static CodePatch g_menuStateHookPatch1 = {};
    static CodePatch g_menuStateHookPatch2 = {};
    static LPVOID g_menuStateTrampoline1 = nullptr;
    static LPVOID g_menuStateTrampoline2 = nullptr;
    static MenuStateWrite g_menuStateQueue[32] = {};
    static std::atomic<uint32_t> g_menuStateQueueWriteIdx{0};
    static uint32_t g_menuStateQueueReadIdx{0};
    static bool g_menuStateHook1Active = false;
    static bool g_menuStateHook2Active = false;
    static std::set<uintptr_t> g_menuStateCandidates1;
    static std::set<uintptr_t> g_menuStateCandidates2;
    static CRITICAL_SECTION g_menuCandidateCS;

    static LPVOID AllocateWithin2GB(uintptr_t targetAddr, size_t size) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);

        for (intptr_t offset = si.dwAllocationGranularity; offset < 0x70000000LL; offset += si.dwAllocationGranularity) {
            // Try +offset
            uintptr_t highAddr = targetAddr + offset;
            highAddr &= ~static_cast<uintptr_t>(si.dwAllocationGranularity - 1);
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((LPCVOID)highAddr, &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_FREE) {
                LPVOID allocated = VirtualAlloc((LPVOID)highAddr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (allocated) {
                    intptr_t diff = (intptr_t)allocated - (intptr_t)(targetAddr + 5);
                    if (diff >= -2147483647LL && diff <= 2147483647LL) {
                        return allocated;
                    }
                    VirtualFree(allocated, 0, MEM_RELEASE);
                }
            }

            // Try -offset
            if (targetAddr > static_cast<uintptr_t>(offset)) {
                uintptr_t lowAddr = targetAddr - offset;
                lowAddr &= ~static_cast<uintptr_t>(si.dwAllocationGranularity - 1);
                if (VirtualQuery((LPCVOID)lowAddr, &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_FREE) {
                    LPVOID allocated = VirtualAlloc((LPVOID)lowAddr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                    if (allocated) {
                        intptr_t diff = (intptr_t)allocated - (intptr_t)(targetAddr + 5);
                        if (diff >= -2147483647LL && diff <= 2147483647LL) {
                            return allocated;
                        }
                        VirtualFree(allocated, 0, MEM_RELEASE);
                    }
                }
            }
        }
        return nullptr;
    }

    static void RemoveShortcutHook() {
        if (!g_shortcutHookActive) return;
        DllLog("[INFO] Removing ShortcutMenu detour hook.");
        g_shortcutHookPatch.Restore();
        Sleep(50); // Allow any game thread inside the trampoline to exit
        if (g_shortcutTrampoline) {
            VirtualFree(g_shortcutTrampoline, 0, MEM_RELEASE);
            g_shortcutTrampoline = nullptr;
        }
        g_shortcutHookActive = false;
    }

    static bool SetupShortcutHook(uintptr_t foundAddress) {
        if (g_shortcutHookActive) return true;

        g_shortcutTrampoline = AllocateWithin2GB(foundAddress, 64);
        if (!g_shortcutTrampoline) {
            return false;
        }

        g_shortcutHookPatch.address = foundAddress;
        g_shortcutHookPatch.size = 10;
        if (!g_shortcutHookPatch.Backup()) {
            VirtualFree(g_shortcutTrampoline, 0, MEM_RELEASE);
            g_shortcutTrampoline = nullptr;
            return false;
        }

        g_tempShortcutAddress = 0;
        g_tempShortcutValue = 0;

        uint8_t code[64] = {};
        size_t idx = 0;

        code[idx++] = 0x9C; // pushf
        code[idx++] = 0x50; // push rax
        code[idx++] = 0x51; // push rcx

        // lea rax, [r13 + rdx + 0x1C04]
        code[idx++] = 0x49;
        code[idx++] = 0x8D;
        code[idx++] = 0x84;
        code[idx++] = 0x15;
        code[idx++] = 0x04;
        code[idx++] = 0x1C;
        code[idx++] = 0x00;
        code[idx++] = 0x00;

        // mov rcx, <address of g_tempShortcutAddress>
        code[idx++] = 0x48;
        code[idx++] = 0xB9;
        uintptr_t addrAddress = (uintptr_t)&g_tempShortcutAddress;
        memcpy(&code[idx], &addrAddress, 8);
        idx += 8;

        // mov [rcx], rax
        code[idx++] = 0x48;
        code[idx++] = 0x89;
        code[idx++] = 0x01;

        // mov rcx, <address of g_tempShortcutValue>
        code[idx++] = 0x48;
        code[idx++] = 0xB9;
        uintptr_t valAddress = (uintptr_t)&g_tempShortcutValue;
        memcpy(&code[idx], &valAddress, 8);
        idx += 8;

        // mov [rcx], ebx
        code[idx++] = 0x89;
        code[idx++] = 0x19;

        code[idx++] = 0x59; // pop rcx
        code[idx++] = 0x58; // pop rax
        code[idx++] = 0x9D; // popf

        // Original instruction: movbe [r13 + rdx + 0x1C04], ebx
        memcpy(&code[idx], "\x41\x0F\x38\xF1\x9C\x15\x04\x1C\x00\x00", 10);
        idx += 10;

        // jmp [rip + 0]
        code[idx++] = 0xFF;
        code[idx++] = 0x25;
        code[idx++] = 0x00;
        code[idx++] = 0x00;
        code[idx++] = 0x00;
        code[idx++] = 0x00;

        uintptr_t returnAddress = foundAddress + 10;
        memcpy(&code[idx], &returnAddress, 8);
        idx += 8;

        memcpy(g_shortcutTrampoline, code, idx);

        std::vector<uint8_t> patchBytes(10, 0x90);
        patchBytes[0] = 0xE9;
        intptr_t diff = (intptr_t)g_shortcutTrampoline - (intptr_t)(foundAddress + 5);
        *(int32_t*)(&patchBytes[1]) = (int32_t)diff;

        if (!g_shortcutHookPatch.ApplyBytes(patchBytes.data(), 10)) {
            VirtualFree(g_shortcutTrampoline, 0, MEM_RELEASE);
            g_shortcutTrampoline = nullptr;
            return false;
        }

        g_shortcutHookActive = true;
        return true;
    }

    static void RemoveMenuStateHooks() {
        if (g_menuStateHook1Active) {
            g_menuStateHookPatch1.Restore();
            g_menuStateHook1Active = false;
        }
        if (g_menuStateHook2Active) {
            g_menuStateHookPatch2.Restore();
            g_menuStateHook2Active = false;
        }
        Sleep(50); // Allow any game thread inside the trampoline to exit
        if (g_menuStateTrampoline1) {
            VirtualFree(g_menuStateTrampoline1, 0, MEM_RELEASE);
            g_menuStateTrampoline1 = nullptr;
        }
        if (g_menuStateTrampoline2) {
            VirtualFree(g_menuStateTrampoline2, 0, MEM_RELEASE);
            g_menuStateTrampoline2 = nullptr;
        }
    }

    static bool SetupMenuStateHook(uintptr_t foundAddress, int writerId) {
        if (g_addrMenuState != 0) return false;
        if (writerId == 1 && g_menuStateHook1Active) return true;
        if (writerId == 2 && g_menuStateHook2Active) return true;

        LPVOID& trampoline = (writerId == 1) ? g_menuStateTrampoline1 : g_menuStateTrampoline2;
        CodePatch& patch = (writerId == 1) ? g_menuStateHookPatch1 : g_menuStateHookPatch2;
        bool& active = (writerId == 1) ? g_menuStateHook1Active : g_menuStateHook2Active;

        trampoline = AllocateWithin2GB(foundAddress, 128);
        if (!trampoline) return false;

        patch.address = foundAddress;
        patch.size = 7;
        if (!patch.Backup()) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            trampoline = nullptr;
            return false;
        }

        uint8_t originalSib = patch.g_originalBytes[5];

        uint8_t code[128] = {};
        size_t idx = 0;

        code[idx++] = 0x9C; // pushfq
        code[idx++] = 0x50; // push rax
        code[idx++] = 0x51; // push rcx
        code[idx++] = 0x52; // push rdx
        code[idx++] = 0x53; // push rbx

        // lea rax, [r13 + index + 0x3C]
        code[idx++] = 0x49;
        code[idx++] = 0x8D;
        code[idx++] = 0x44;
        code[idx++] = originalSib;
        code[idx++] = 0x3C;

        // mov rcx, <address of g_menuStateQueueWriteIdx>
        code[idx++] = 0x48;
        code[idx++] = 0xB9;
        uintptr_t writeIdxAddr = (uintptr_t)&g_menuStateQueueWriteIdx;
        memcpy(&code[idx], &writeIdxAddr, 8);
        idx += 8;

        // mov ebx, 1
        code[idx++] = 0xBB;
        uint32_t one = 1;
        memcpy(&code[idx], &one, 4);
        idx += 4;

        // lock xadd dword ptr [rcx], ebx
        code[idx++] = 0xF0;
        code[idx++] = 0x0F;
        code[idx++] = 0xC1;
        code[idx++] = 0x19;

        // and ebx, 31
        code[idx++] = 0x83;
        code[idx++] = 0xE3;
        code[idx++] = 0x1F;

        // shl rbx, 4 (16 bytes = sizeof(MenuStateWrite))
        code[idx++] = 0x48;
        code[idx++] = 0xC1;
        code[idx++] = 0xE3;
        code[idx++] = 0x04;

        // mov rcx, <address of g_menuStateQueue>
        code[idx++] = 0x48;
        code[idx++] = 0xB9;
        uintptr_t queueAddr = (uintptr_t)&g_menuStateQueue;
        memcpy(&code[idx], &queueAddr, 8);
        idx += 8;

        // add rcx, rbx
        code[idx++] = 0x48;
        code[idx++] = 0x03;
        code[idx++] = 0xCB;

        // mov [rcx], rax (address)
        code[idx++] = 0x48;
        code[idx++] = 0x89;
        code[idx++] = 0x01;

        // mov eax, [rsp + 0x18] (value of rax saved on entry)
        code[idx++] = 0x8B;
        code[idx++] = 0x44;
        code[idx++] = 0x24;
        code[idx++] = 0x18;

        // mov [rcx + 8], eax
        code[idx++] = 0x89;
        code[idx++] = 0x41;
        code[idx++] = 0x08;

        // mov dword ptr [rcx + 12], writerId
        code[idx++] = 0xC7;
        code[idx++] = 0x41;
        code[idx++] = 0x0C;
        uint32_t wId = static_cast<uint32_t>(writerId);
        memcpy(&code[idx], &wId, 4);
        idx += 4;

        code[idx++] = 0x5B; // pop rbx
        code[idx++] = 0x5A; // pop rdx
        code[idx++] = 0x59; // pop rcx
        code[idx++] = 0x58; // pop rax
        code[idx++] = 0x9D; // popfq

        // Original instruction: movbe [r13 + index + 0x3C], eax
        memcpy(&code[idx], patch.g_originalBytes.data(), 7);
        idx += 7;

        // jmp [rip + 0]
        code[idx++] = 0xFF;
        code[idx++] = 0x25;
        code[idx++] = 0x00;
        code[idx++] = 0x00;
        code[idx++] = 0x00;
        code[idx++] = 0x00;

        uintptr_t returnAddress = foundAddress + 7;
        memcpy(&code[idx], &returnAddress, 8);
        idx += 8;

        memcpy(trampoline, code, idx);

        std::vector<uint8_t> patchBytes(7, 0x90);
        patchBytes[0] = 0xE9;
        intptr_t diff = (intptr_t)trampoline - (intptr_t)(foundAddress + 5);
        *(int32_t*)(&patchBytes[1]) = (int32_t)diff;

        if (!patch.ApplyBytes(patchBytes.data(), 7)) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            trampoline = nullptr;
            return false;
        }

        active = true;
        return true;
    }

    static void RestoreAllPatches() {
        EnterCriticalSection(&g_patchCS);
        if (g_magnePatchesInitialized) {
            g_magneDetourPatch.Restore();
            g_magneXPatch.Restore();
            g_magneYPatch.Restore();
            g_magneZPatch.Restore();
        }
        RemoveShortcutHook();
        RemoveMenuStateHooks();
        LeaveCriticalSection(&g_patchCS);
    }

    static void PollHooksAndSyncSharedMemory() {
        if (!g_pSharedMemory) return;

        if (g_shortcutHookActive && g_addrShortcutMenu == 0) {
            uintptr_t tempAddr = g_tempShortcutAddress.load();
            if (tempAddr != 0) {
                int32_t tempVal = g_tempShortcutValue.load();
                if (tempVal >= -1 && tempVal <= 4) {
                    DllLog("[SUCCESS] Hook fired! Verified ShortcutMenu address: 0x%llX (value: %d). Hook removed.", tempAddr - 128, tempVal);
                    g_addrShortcutMenu = tempAddr - 128;
                    g_pSharedMemory->m_statusAddrShortcutMenu = g_addrShortcutMenu;
                    RemoveShortcutHook();
                } else {
#ifdef _DEBUG
                    DllLog("[WARNING] Hook fired on incorrect value %d at address 0x%llX. Ignoring and waiting.", tempVal, tempAddr);
#endif
                    g_tempShortcutAddress = 0;
                    g_tempShortcutValue = 0;
                }
            }
        }

        if ((g_menuStateHook1Active || g_menuStateHook2Active) && g_addrMenuState == 0) {
            uint32_t writeIdx = g_menuStateQueueWriteIdx.load();
            while (g_menuStateQueueReadIdx != writeIdx && g_addrMenuState == 0) {
                uint32_t idx = g_menuStateQueueReadIdx & 31;
                uintptr_t tempAddr = g_menuStateQueue[idx].address;
                int32_t tempVal = g_menuStateQueue[idx].value;
                uint32_t writerId = g_menuStateQueue[idx].writerId;
                g_menuStateQueueReadIdx++;

                bool valValid = false;
                if (writerId == 1 && (tempVal == 6 || tempVal == 10)) {
                    valValid = true;
                } else if (writerId == 2 && (tempVal == 3 || tempVal == 5)) {
                    valValid = true;
                }

                if (valValid) {
                    bool patternValid = false;
                    uint16_t marker = 0;
                    if (SafeReadMarker(tempAddr - 4, marker) && (marker & 0xFF) == 0x6E) {
                        patternValid = true;
                    }

                    if (patternValid) {
                        uintptr_t baseAddr = tempAddr;
                        EnterCriticalSection(&g_menuCandidateCS);
                        if (g_addrMenuState == 0) {
                            if (writerId == 1) g_menuStateCandidates1.insert(baseAddr);
                            if (writerId == 2) g_menuStateCandidates2.insert(baseAddr);

                            bool inBoth = (g_menuStateCandidates1.count(baseAddr) > 0) && (g_menuStateCandidates2.count(baseAddr) > 0);

                            // Fire logging silenced for both writers in all builds

                            if (inBoth) {
                                g_addrMenuState = baseAddr;
                                g_menuStateCandidates1.clear();
                                g_menuStateCandidates2.clear();

                                RemoveMenuStateHooks();
                                g_menuStateQueueWriteIdx = 0;
                                g_menuStateQueueReadIdx = 0;
                                memset(g_menuStateQueue, 0, sizeof(g_menuStateQueue));
                                g_pSharedMemory->m_statusAddrMenuState = baseAddr;
                                DllLog("[SUCCESS] MenuState candidate WINNER selected at 0x%llX (FIRED BY BOTH PAIRED AOB WRITERS WITH MATCHING VALUES & 6E VALID!). Trampoline hooks removed.", baseAddr);
                            }
                        }
                        LeaveCriticalSection(&g_menuCandidateCS);
                    } else {
                        // Pattern 6E miss — silenced in all builds
                    }
                } else {
                    // Unexpected value — silenced in all builds
                }
            }
        }

        g_pSharedMemory->m_statusAddrGameRomCamera = g_addrGameRomCamera.load();
        g_pSharedMemory->m_statusAddrShortcutMenu  = g_addrShortcutMenu.load();
        g_pSharedMemory->m_statusAddrMenuState     = g_addrMenuState.load();
        g_pSharedMemory->m_statusAddrMagneTarget   = g_addrMagneTarget.load();
    }

    // -------------------------------------------------------------------------
    // Hunter — Page-Guard VEH, mouse-gated, 250ms detect / 100ms catch
    //   detect: every 250ms if mouse moved and cur != lastWritten → overwrite
    //   catch:  arm guard for 100ms (25 frames @4ms) → VEH captures RIP
    //   AOB:    dump RIP±32 like F5, test vs blacklist, NOP else pause 50ms
    //   life:   F2 off restores (keeps list), F2 on re-NOPs; Reset forgets, Reset2 keeps
    // -------------------------------------------------------------------------

    struct WriterRecord {
        uintptr_t rip;
        uint8_t   origBytes[16];
        size_t    patchSize;
        bool      nopActive;
    };

    static std::vector<WriterRecord> g_discoveredWriters; // protected by g_writerCS
    static std::vector<uintptr_t>    g_pendingRips; // accumulated during 10-frame window, protected by g_writerCS
    static CRITICAL_SECTION          g_writerCS;
    static PVOID                     g_vehHandle = nullptr;
    static std::atomic<bool>         g_guardArmed{false};
    static std::atomic<bool>         g_writerHuntActive{false};
    static uintptr_t                 g_guardPage = 0;
    static DWORD                     g_guardOldProtect = 0;
    static std::atomic<uint64_t>     g_lastBlacklistedWriteTime{0};
    static std::atomic<int>          g_huntFramesLeft{0};
    static std::atomic<bool>         g_blacklistedMode{false};
    static std::atomic<bool>         g_hunterResetPending{false}; // explicit reset atomic: ScanAobThread sets on reset, CameraControlThread consumes after new GameRomCamera

    // FOV level-change detector — counts actual writes to FOV via code hook (captured via one-shot data guard)
    static uintptr_t                 g_fovGuardPage = 0;
    static DWORD                     g_fovOldProtect = 0;
    static std::atomic<bool>         g_fovGuardArmed{false};
    static std::atomic<uint64_t>     g_fovWriteCount{0};
    static std::atomic<uint64_t>     g_lastFovWriteTick{0};
    static std::atomic<uintptr_t>    g_fovPendingRIP{0};
    static CodePatch                 g_fovHookPatch = {};
    static LPVOID                    g_fovTrampoline = nullptr;
    static std::atomic<bool>         g_fovHookActive{false};
    // FOV writer discovery — data guard for a few frames to log writer RIP (no hook yet)
    static std::atomic<bool>         g_fovHuntActive{false};
    static std::atomic<int>          g_fovHuntFramesLeft{0};
    static std::vector<uintptr_t>    g_fovHuntRips;
    static CRITICAL_SECTION          g_fovHuntCS;
    static bool                      g_fovHuntCSInit = false;
    // DR0 hardware breakpoint for FOV (write-only, no PAGE_GUARD reads)
    static std::atomic<bool>         g_fovHwActive{false};
    static uintptr_t                 g_fovHwAddr = 0;
    static std::atomic<bool>         g_fovStallReenablePending{false};
    static std::atomic<bool>         g_forceShouldControlReset{false};

    static size_t DetectWriteInstructionSize(const uint8_t* p) {
        size_t off = 0;
        // REX
        bool hasRex = (p[off] >= 0x40 && p[off] <= 0x4F);
        if (hasRex) off++;
        // Prefixes: 66/F2/F3
        bool has66 = false, hasF2 = false, hasF3 = false;
        if (p[off] == 0x66) { has66 = true; off++; if (p[off] >= 0x40 && p[off] <= 0x4F) { hasRex = true; off++; } }
        if (p[off] == 0xF2) { hasF2 = true; off++; }
        if (p[off] == 0xF3) { hasF3 = true; off++; }
        // MOVBE: 0F 38 F1
        if (p[off] == 0x0F && p[off+1] == 0x38 && p[off+2] == 0xF1) {
            off += 3;
            uint8_t modrm = p[off++];
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t rm  = modrm & 7;
            bool hasSib = (rm == 4);
            if (hasSib) off++;
            if      (mod == 1) off += 1;
            else if (mod == 2) off += 4;
            else if (mod == 0 && rm == 5) off += 4;
            return off;
        }
        // MOV r/m32, r32: 89 /r
        if (p[off] == 0x89) {
            off++;
            uint8_t modrm = p[off++];
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t rm  = modrm & 7;
            bool hasSib = (rm == 4);
            if (hasSib) off++;
            if      (mod == 1) off += 1;
            else if (mod == 2) off += 4;
            else if (mod == 0 && rm == 5) off += 4;
            return off;
        }
        // MOVSS m32, xmm: F3 0F 11 /r  and MOVUPS/SD variants: 0F 11, 66 0F 11, F2 0F 11
        if (p[off] == 0x0F && (p[off+1] == 0x11 || p[off+1] == 0x29 || p[off+1] == 0x7F)) {
            // 0F 11 = MOVUPS, 0F 29 = MOVAPS, 0F 7F = MOVQ
            off += 2;
            uint8_t modrm = p[off++];
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t rm  = modrm & 7;
            bool hasSib = (rm == 4);
            if (hasSib) off++;
            if      (mod == 1) off += 1;
            else if (mod == 2) off += 4;
            else if (mod == 0 && rm == 5) off += 4;
            return off;
        }
        // With F3 prefix already consumed, check again
        if (hasF3 && p[off] == 0x0F && p[off+1] == 0x11) {
            off += 2;
            uint8_t modrm = p[off++];
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t rm  = modrm & 7;
            bool hasSib = (rm == 4);
            if (hasSib) off++;
            if      (mod == 1) off += 1;
            else if (mod == 2) off += 4;
            else if (mod == 0 && rm == 5) off += 4;
            return off;
        }
        return 0;
    }

    static bool NopInstruction(uintptr_t rip, WriterRecord& rec) {
        uint8_t buf[16] = {};
        __try { memcpy(buf, (const void*)rip, 16); } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        size_t sz = DetectWriteInstructionSize(buf);
        if (sz == 0 || sz > 15) {
            // Fallback: many JIT stores are 5-7 bytes; try 7 with warning
            DllLog("[WARNING] Detect size 0 for RIP 0x%llX bytes %02X %02X %02X %02X — fallback to 7", rip, buf[0], buf[1], buf[2], buf[3]);
            sz = 7;
        }
        DWORD old = 0;
        if (!VirtualProtect((LPVOID)rip, sz, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy(rec.origBytes, buf, sz);
        rec.rip = rip;
        rec.patchSize = sz;
        rec.nopActive = true;
        memset((void*)rip, 0x90, sz);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)rip, sz);
        VirtualProtect((LPVOID)rip, sz, old, &old);
        return true;
    }

    static void RestoreInstruction(WriterRecord& rec) {
        if (!rec.nopActive || rec.patchSize == 0) return;
        DWORD old = 0;
        if (VirtualProtect((LPVOID)rec.rip, rec.patchSize, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy((void*)rec.rip, rec.origBytes, rec.patchSize);
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)rec.rip, rec.patchSize);
            VirtualProtect((LPVOID)rec.rip, rec.patchSize, old, &old);
        }
        rec.nopActive = false;
    }

    static void DisarmPageGuard() {
        if (!g_guardArmed) return;
        // If FOV shares same page, keep guard for FOV
        if (g_fovGuardArmed && g_guardPage == g_fovGuardPage) {
            g_guardArmed = false;
            return;
        }
        DWORD old = 0;
        VirtualProtect((LPVOID)g_guardPage, 0x1000, g_guardOldProtect & ~PAGE_GUARD, &old);
        g_guardArmed = false;
    }

    static void ArmPageGuard(uintptr_t gc_addr) {
        uintptr_t page = gc_addr & ~(uintptr_t)0xFFF;
        if (g_guardArmed) {
            if (g_guardPage == page) return;
            DisarmPageGuard();
        }
        DWORD old = 0;
        if (VirtualProtect((LPVOID)page, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old)) {
            g_guardPage = page;
            g_guardOldProtect = old;
            g_guardArmed = true;
            // Per-frame guard churn — spam unrelated to camera recovery; gated by verbose switch
            if (g_verboseCamDiag && g_huntFramesLeft.load() > 0) {
                DllLog("[DIAG] ArmPageGuard page 0x%llX huntFrames %d", page, g_huntFramesLeft.load());
            }
        } else {
            DllLog("[WARNING] ArmPageGuard failed page 0x%llX err %u", page, GetLastError());
        }
    }

    static void DisarmFovGuard() {
        if (!g_fovGuardArmed) return;
        // If FOV shares page with pos hunter guard, don't actually unprotect while hunter still guards it
        if (g_guardArmed && g_fovGuardPage == g_guardPage) {
            g_fovGuardArmed = false;
            return;
        }
        DWORD old = 0;
        VirtualProtect((LPVOID)g_fovGuardPage, 0x1000, g_fovOldProtect & ~PAGE_GUARD, &old);
        g_fovGuardArmed = false;
    }

    static void ArmFovGuard(uintptr_t fovAddr) {
        uintptr_t page = fovAddr & ~(uintptr_t)0xFFF;
        if (g_fovGuardArmed && g_fovGuardPage == page) return;
        if (g_fovGuardArmed) DisarmFovGuard();
        // If hunter already guards this page, just share it
        if (g_guardArmed && g_guardPage == page) {
            g_fovGuardPage = page;
            g_fovOldProtect = g_guardOldProtect;
            g_fovGuardArmed = true;
            return;
        }
        DWORD old = 0;
        if (VirtualProtect((LPVOID)page, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old)) {
            g_fovGuardPage = page;
            g_fovOldProtect = old;
            g_fovGuardArmed = true;
        } else {
            DllLog("[WARNING] ArmFovGuard failed page 0x%llX err %u", page, GetLastError());
        }
    }

    static bool SafeReadU8_SEH(uintptr_t addr, uint8_t& out);
    static void RemoveFovHook() {
        if (!g_fovHookActive.load()) {
            g_fovPendingRIP.store(0);
            return;
        }
        g_fovHookPatch.Restore();
        Sleep(50);
        if (g_fovTrampoline) {
            VirtualFree(g_fovTrampoline, 0, MEM_RELEASE);
            g_fovTrampoline = nullptr;
        }
        g_fovHookActive.store(false);
        g_fovPendingRIP.store(0);
        DllLog("[INFO] FOV hook removed");
    }

    static bool SetupFovHook(uintptr_t rip) {
        if (g_fovHookActive.load()) return true;
        if (rip == 0) return false;
        uint8_t tmp[16] = {};
        for (int i = 0; i < 16; ++i) { uint8_t b = 0; SafeReadU8_SEH(rip + i, b); tmp[i] = b; }
        size_t sz = DetectWriteInstructionSize(tmp);
        if (sz == 0 || sz > 15) {
            DllLog("[WARNING] FOV hook size 0 at 0x%llX bytes %02X %02X %02X %02X — fallback 7", rip, tmp[0], tmp[1], tmp[2], tmp[3]);
            sz = 7;
        }
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery((LPCVOID)rip, &mbi, sizeof(mbi))) return false;
        if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
        g_fovHookPatch.address = rip;
        g_fovHookPatch.size = sz;
        if (!g_fovHookPatch.Backup()) return false;
        g_fovTrampoline = AllocateWithin2GB(rip, 64);
        if (!g_fovTrampoline) {
            g_fovHookPatch.Restore();
            return false;
        }
        uint8_t code[64] = {};
        size_t idx = 0;
        code[idx++] = 0x9C; // pushf
        code[idx++] = 0x50; // push rax
        code[idx++] = 0x51; // push rcx
        // mov rcx, &g_fovWriteCount
        code[idx++] = 0x48; code[idx++] = 0xB9;
        uintptr_t cntAddr = (uintptr_t)&g_fovWriteCount;
        memcpy(&code[idx], &cntAddr, 8); idx += 8;
        // lock inc qword ptr [rcx]
        code[idx++] = 0xF0; code[idx++] = 0x48; code[idx++] = 0xFF; code[idx++] = 0x01;
        code[idx++] = 0x59; // pop rcx
        code[idx++] = 0x58; // pop rax
        code[idx++] = 0x9D; // popf
        // original instruction
        memcpy(&code[idx], (void*)rip, sz); idx += sz;
        // jmp [rip+0]
        code[idx++] = 0xFF; code[idx++] = 0x25; code[idx++] = 0x00; code[idx++] = 0x00; code[idx++] = 0x00; code[idx++] = 0x00;
        uintptr_t retAddr = rip + sz;
        memcpy(&code[idx], &retAddr, 8); idx += 8;
        memcpy(g_fovTrampoline, code, idx);
        std::vector<uint8_t> patchBytes(sz, 0x90);
        patchBytes[0] = 0xE9;
        intptr_t diff = (intptr_t)g_fovTrampoline - (intptr_t)(rip + 5);
        *(int32_t*)(&patchBytes[1]) = (int32_t)diff;
        if (!g_fovHookPatch.ApplyBytes(patchBytes.data(), sz)) {
            VirtualFree(g_fovTrampoline, 0, MEM_RELEASE);
            g_fovTrampoline = nullptr;
            return false;
        }
        g_fovHookActive.store(true);
        DllLog("[SUCCESS] FOV hook installed at 0x%llX size %zu", rip, sz);
        return true;
    }

    static void SetFovHwBreakpoint(uintptr_t addr) {
        g_fovHwAddr = addr;
        g_fovHwActive.store(true);
        DWORD currentTid = GetCurrentThreadId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te; te.dwSize = sizeof(te);
            DWORD pid = GetCurrentProcessId();
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == pid && te.th32ThreadID != currentTid) {
                        HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                        if (hThread) {
                            SuspendThread(hThread);
                            CONTEXT ctx; memset(&ctx, 0, sizeof(ctx));
                            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                            if (GetThreadContext(hThread, &ctx)) {
                                ctx.Dr0 = addr;
                                ctx.Dr7 &= ~0xF0001ULL;
                                ctx.Dr7 |= 0x1ULL; // L0
                                ctx.Dr7 |= (0x1ULL << 16); // type 01 = write
                                ctx.Dr7 |= (0x3ULL << 18); // len 11 = 4 bytes
                                ctx.Dr6 = 0;
                                SetThreadContext(hThread, &ctx);
                            }
                            ResumeThread(hThread);
                            CloseHandle(hThread);
                        }
                    }
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
        // Current thread — don't suspend self
        {
            CONTEXT ctx; memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            // GetThreadContext on pseudo handle works without suspend for current thread
            HANDLE hSelf = GetCurrentThread();
            if (GetThreadContext(hSelf, &ctx)) {
                ctx.Dr0 = addr;
                ctx.Dr7 &= ~0xF0001ULL;
                ctx.Dr7 |= 0x1ULL;
                ctx.Dr7 |= (0x1ULL << 16);
                ctx.Dr7 |= (0x3ULL << 18);
                ctx.Dr6 = 0;
                SetThreadContext(hSelf, &ctx);
            }
        }
        DllLog("[INFO] DR0 set for FOV 0x%llX", addr);
    }
    static void ClearFovHwBreakpoint() {
        if (!g_fovHwActive.load() && g_fovHwAddr == 0) return;
        g_fovHwActive.store(false);
        g_fovHwAddr = 0;
        DWORD currentTid = GetCurrentThreadId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te; te.dwSize = sizeof(te);
            DWORD pid = GetCurrentProcessId();
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == pid && te.th32ThreadID != currentTid) {
                        HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                        if (hThread) {
                            SuspendThread(hThread);
                            CONTEXT ctx; memset(&ctx, 0, sizeof(ctx));
                            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                            if (GetThreadContext(hThread, &ctx)) {
                                ctx.Dr0 = 0;
                                ctx.Dr7 &= ~0xF0001ULL;
                                ctx.Dr6 = 0;
                                SetThreadContext(hThread, &ctx);
                            }
                            ResumeThread(hThread);
                            CloseHandle(hThread);
                        }
                    }
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
        {
            CONTEXT ctx; memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            HANDLE hSelf = GetCurrentThread();
            if (GetThreadContext(hSelf, &ctx)) {
                ctx.Dr0 = 0;
                ctx.Dr7 &= ~0xF0001ULL;
                ctx.Dr6 = 0;
                SetThreadContext(hSelf, &ctx);
            }
        }
        DllLog("[INFO] DR0 cleared");
    }

    static thread_local bool t_isSingleStepping = false;
    static thread_local bool t_lastFovWasWrite = false;

    static inline bool SafeReadU8_SEH(uintptr_t addr, uint8_t& out) {
        __try { out = *(const uint8_t*)addr; return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static std::string GenerateWriterAob(uintptr_t rip, size_t patchSize) {
        std::string out;
        uintptr_t start = rip >= 32 ? rip - 32 : 0;
        uintptr_t end   = rip + patchSize + 32;
        char buf[8];
        for (uintptr_t p = start; p < end; ++p) {
            uint8_t b = 0;
            SafeReadU8_SEH(p, b);
            if (p == rip) out += "[ ";
            snprintf(buf, sizeof(buf), "%02X ", b);
            out += buf;
            if (p == rip + patchSize - 1) out += "] ";
        }
        return out;
    }

    static uintptr_t FindFovWriterFromHwRip(uintptr_t nextRip) {
        // Like Cheat Engine F6: DR0 fires at nextRip — the writer is the instruction that ends at nextRip, prefer longest (with REX)
        uintptr_t best = 0;
        size_t bestSize = 0;
        for (int back = 1; back <= 15; ++back) {
            uintptr_t cand = nextRip - back;
            uint8_t tmp[16] = {};
            for (int i = 0; i < 16; ++i) { uint8_t b = 0; SafeReadU8_SEH(cand + i, b); tmp[i] = b; }
            size_t sz = DetectWriteInstructionSize(tmp);
            if (sz == 0 || sz > 15) continue;
            if (cand + sz == nextRip) {
                if (sz > bestSize) { best = cand; bestSize = sz; }
            }
        }
        if (best != 0) return best;
        return nextRip;
    }

    static bool IsWriterBlacklisted(uintptr_t rip) {
        if (g_writerBlacklist.empty()) return false;
        // Generate writer AOB like F5 does (RIP-32 .. RIP+size+32), then see if any blacklist pattern lives inside that AOB
        uint8_t tmp[16] = {};
        for (int i = 0; i < 16; ++i) { uint8_t b = 0; if (!SafeReadU8_SEH(rip + i, b)) break; tmp[i] = b; }
        size_t patchSize = DetectWriteInstructionSize(tmp);
        if (patchSize == 0 || patchSize > 15) patchSize = 7;
        uintptr_t start = rip >= 32 ? rip - 32 : 0;
        uintptr_t end = rip + patchSize + 32;
        size_t bufLen = (size_t)(end - start);
        if (bufLen == 0 || bufLen > 512) return false;
        std::vector<uint8_t> buf(bufLen);
        for (size_t i = 0; i < bufLen; ++i) { uint8_t b = 0; SafeReadU8_SEH(start + i, b); buf[i] = b; }
        for (const auto& pat : g_writerBlacklist) {
            size_t patLen = pat.bytes.size();
            if (patLen == 0 || patLen > bufLen) continue;
            for (size_t off = 0; off + patLen <= bufLen; ++off) {
                bool match = true;
                for (size_t j = 0; j < patLen; ++j) {
                    if (!pat.isWildcard[j] && buf[off + j] != pat.bytes[j]) { match = false; break; }
                }
                if (match) return true;
            }
        }
        return false;
    }

    static LONG NTAPI CameraWriterVehHandler(PEXCEPTION_POINTERS ep) {
        if (ep->ExceptionRecord->ExceptionCode == STATUS_SINGLE_STEP) {
            // DR0 hardware breakpoint for FOV (write-only) — must be checked before software single-step
            if ((ep->ContextRecord->Dr6 & 0x1) && g_fovHwActive.load()) {
                uintptr_t nextRip = (uintptr_t)ep->ContextRecord->Rip;
                uintptr_t writer = FindFovWriterFromHwRip(nextRip);
                if (g_fovHuntActive.load()) {
                    if (TryEnterCriticalSection(&g_fovHuntCS)) {
                        bool already = false;
                        for (auto r : g_fovHuntRips) if (r == writer) already = true;
                        if (!already) g_fovHuntRips.push_back(writer);
                        LeaveCriticalSection(&g_fovHuntCS);
                    }
                } else {
                    // Stall counting via DR0 — no hook, just count writes
                    g_fovWriteCount.fetch_add(1);
                    g_lastFovWriteTick.store(GetTickCount64());
                }
                ep->ContextRecord->Dr6 = 0;
                ep->ContextRecord->EFlags &= ~0x100ULL;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (t_isSingleStepping) {
                t_isSingleStepping = false;
                if (g_huntFramesLeft.load() > 0) {
                    uintptr_t base = g_addrGameRomCamera.load();
                    if (base != 0) ArmPageGuard(base + 0x550);
                }
                // FOV hunter per-frame only — single-step re-arm crashes on this page
                t_lastFovWasWrite = false;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (ep->ExceptionRecord->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION)
            return EXCEPTION_CONTINUE_SEARCH;
        uintptr_t faultAddr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        bool isFovPage = g_fovGuardArmed.load() && faultAddr >= g_fovGuardPage && faultAddr < g_fovGuardPage + 0x1000;
        bool isHunterPage = g_guardArmed.load() && faultAddr >= g_guardPage && faultAddr < g_guardPage + 0x1000;
        if (!isFovPage && !isHunterPage)
            return EXCEPTION_CONTINUE_SEARCH;
        // Windows clears PAGE_GUARD on fault — mark both as disarmed if they share page
        if (isHunterPage) g_guardArmed = false;
        if (isFovPage) g_fovGuardArmed = false;
        if (isFovPage && isHunterPage && g_fovGuardPage == g_guardPage) {
            // Shared page — both cleared
            g_guardArmed = false;
            g_fovGuardArmed = false;
        }
        bool isWrite = (ep->ExceptionRecord->ExceptionInformation[0] == 1);
        uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
        t_lastFovWasWrite = false;
        // FOV hunter — collect writer RIP for a few frames after GameRomCamera found (no hook, just log)
        // FOV shares page with XYZ (0x550) so hunter guard also traps FOV writes — check either guard
        // Diagnostic: log any guard fault while FOV hunt active (even not FOV) to see if guard fires
        if (g_fovHuntActive.load() && (isFovPage || isHunterPage)) {
            static uint64_t lastAnyDbg = 0;
            uint64_t nowAny = GetTickCount64();
            if (nowAny - lastAnyDbg > 1000) {
                lastAnyDbg = nowAny;
                DllLog("[INFO] FOV hunt guard fault RIP 0x%llX fault 0x%llX isWrite %d isFovPage %d isHunterPage %d base 0x%llX", rip, faultAddr, isWrite?1:0, isFovPage?1:0, isHunterPage?1:0, g_addrGameRomCamera.load());
            }
        }
        if (isWrite && (isFovPage || isHunterPage)) {
            uintptr_t baseFov = g_addrGameRomCamera.load();
            if (baseFov != 0 && faultAddr == baseFov + 0x68C) {
                t_lastFovWasWrite = true;
                if (g_fovHuntActive.load()) {
                    // Use TryEnter to avoid blocking game thread if CameraControl holds CS
                    if (TryEnterCriticalSection(&g_fovHuntCS)) {
                        bool already = false;
                        for (auto r : g_fovHuntRips) if (r == rip) already = true;
                        if (!already) {
                            g_fovHuntRips.push_back(rip);
                        }
                        LeaveCriticalSection(&g_fovHuntCS);
                    }
                }
                else if (g_verboseCamDiag) {
                    static uint64_t lastFovDbg = 0;
                    uint64_t nowFov = GetTickCount64();
                    if (nowFov - lastFovDbg > 1000) {
                        lastFovDbg = nowFov;
                        DllLog("[DIAG] FOV write RIP 0x%llX fault 0x%llX (no hunt)", rip, faultAddr);
                    }
                }
            }
        }
        // Log every guard fault for diagnostics (even if not collected)
        bool shouldCollect = false;
        if (isWrite && g_writerHuntActive.load() && isHunterPage) {
            uintptr_t base = g_addrGameRomCamera.load();
            if (base != 0 && (faultAddr == base + 0x550 || faultAddr == base + 0x554 || faultAddr == base + 0x558)) {
                MEMORY_BASIC_INFORMATION mbi = {};
                VirtualQuery((LPCVOID)rip, &mbi, sizeof(mbi));
                bool isExec = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
                bool isJit = (mbi.Type == MEM_PRIVATE) && isExec;
                if (!isJit) {
                    // Strict: only JIT (MEM_PRIVATE + exec) is a real camera writer; our own writes are MEM_IMAGE and must be single-stepped
                    // Don't log MEM_IMAGE faults at high frequency to avoid spam, but log once per burst
                    static uint64_t lastLogMs = 0;
                    uint64_t nowDbg = GetTickCount64();
                    if (g_verboseCamDiag && nowDbg - lastLogMs > 500) {
                        lastLogMs = nowDbg;
                        DllLog("[DIAG] Guard fault RIP 0x%llX fault 0x%llX Type %u Protect 0x%X — not JIT, single-step", rip, faultAddr, mbi.Type, mbi.Protect);
                    }
                } else {
                    shouldCollect = true;
                }
            } else {
                // Not XYZ — page guard hit for other var on same 4KB page, just single-step, no log to avoid flood
            }
        } else if (g_writerHuntActive.load() && isWrite && isHunterPage) {
            // Write fault outside our XYZ — single-step, no log unless verbose
            if (g_verboseCamDiag) {
                DllLog("[DIAG] Guard write fault RIP 0x%llX fault 0x%llX not XYZ or not huntActive", rip, faultAddr);
            }
        }
        if (shouldCollect) {
            EnterCriticalSection(&g_writerCS);
            bool alreadyPending = false, alreadyDiscovered = false;
            for (auto r : g_pendingRips) if (r == rip) alreadyPending = true;
            for (auto &wr : g_discoveredWriters) if (wr.rip == rip) alreadyDiscovered = true;
            if (!alreadyPending && !alreadyDiscovered) {
                g_pendingRips.push_back(rip);
                if (g_verboseCamDiag) {
                    DllLog("[DIAG] Collected writer RIP 0x%llX (fault 0x%llX) pending %zu (accumulating 10 frames)", rip, faultAddr, g_pendingRips.size());
                }
                if (g_pendingRips.size() >= 3) {
                    // Got X/Y/Z — stop immediately, don't wait full 10 frames
                    g_huntFramesLeft.store(1);
                }
            }
            LeaveCriticalSection(&g_writerCS);
        }
        ep->ContextRecord->EFlags |= 0x100;
        t_isSingleStepping = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Called after 10-frame window closes — build ONE combined AOB like F5 (minRip-32 .. maxRip+size+32) and compare that single AOB vs blacklist
    static void ProcessPendingWriters() {
        std::vector<uintptr_t> toProcess;
        EnterCriticalSection(&g_writerCS);
        if (g_pendingRips.empty()) {
            bool wasBlacklisted = g_blacklistedMode.load();
            LeaveCriticalSection(&g_writerCS);
            if (wasBlacklisted) {
                DllLog("[INFO] Blacklist re-check: no writers — re-enabling mouse");
                g_blacklistedMode.store(false);
            }
            return;
        }
        toProcess.swap(g_pendingRips);
        LeaveCriticalSection(&g_writerCS);
        // Sort to find earliest/latest like F5 does
        std::sort(toProcess.begin(), toProcess.end());
        // Build combined AOB buffer like F5 dump does (min-32 .. max+size+32)
        uintptr_t minRip = toProcess.front();
        uintptr_t maxRip = toProcess.back();
        // Need max patch size — compute for maxRip
        uint8_t tmpMax[16] = {}; for (int i=0;i<16;++i){ uint8_t b=0; SafeReadU8_SEH(maxRip+i,b); tmpMax[i]=b; }
        size_t maxPatch = DetectWriteInstructionSize(tmpMax);
        if (maxPatch==0||maxPatch>15) maxPatch=7;
        uintptr_t start = minRip >= 32 ? minRip - 32 : 0;
        uintptr_t end = maxRip + maxPatch + 32;
        size_t bufLen = (size_t)(end - start);
        if (bufLen==0 || bufLen>2048) bufLen = 0;
        std::vector<uint8_t> combined;
        if (bufLen) {
            combined.resize(bufLen);
            for (size_t i=0;i<bufLen;++i){ uint8_t b=0; SafeReadU8_SEH(start+i,b); combined[i]=b; }
            // Restore original bytes for inside-writer regions like F5 does (so blacklist wildcards still match)
            EnterCriticalSection(&g_writerCS);
            for (auto &wr : g_discoveredWriters) {
                if (wr.rip >= start && wr.rip + wr.patchSize <= end) {
                    for (size_t k=0;k<wr.patchSize;++k) combined[wr.rip - start + k] = wr.origBytes[k];
                }
            }
            // Also for toProcess pending writers, ensure their original bytes are used (they are not yet NOP'd, but use live bytes which are original)
            LeaveCriticalSection(&g_writerCS);
        }
        // Check this ONE combined AOB vs blacklist — if any blacklist pattern lives inside it, the whole set is blacklisted
        bool blacklisted = false;
        if (!combined.empty() && !g_writerBlacklist.empty()) {
            for (const auto& pat : g_writerBlacklist) {
                size_t patLen = pat.bytes.size();
                if (patLen==0 || patLen > combined.size()) continue;
                for (size_t off=0; off+patLen <= combined.size(); ++off) {
                    bool match=true;
                    for (size_t j=0;j<patLen;++j) if (!pat.isWildcard[j] && combined[off+j] != pat.bytes[j]) { match=false; break; }
                    if (match) { blacklisted=true; break; }
                }
                if (blacklisted) break;
            }
        }
        // Hex string is for logging only (and expensive: per-byte re-read + size detect). Build it
        // only when we will actually print: on a blacklist event (always) or under the verbose switch.
        std::string combinedAobStr;
        if (bufLen && (blacklisted || g_verboseCamDiag)) {
            for (uintptr_t p=start; p<end; ++p) {
                bool isStart=false,isEnd=false;
                for (auto r: toProcess) {
                    uint8_t tm2[16]={}; for(int i=0;i<16;++i){uint8_t b=0; SafeReadU8_SEH(r+i,b); tm2[i]=b;}
                    size_t sz2 = DetectWriteInstructionSize(tm2); if(sz2==0||sz2>15) sz2=7;
                    if (r==p) isStart=true;
                    if (r+sz2-1==p) isEnd=true;
                }
                uint8_t b = combined[p - start];
                if (isStart) combinedAobStr += "[ ";
                char bb[4]; snprintf(bb,sizeof(bb),"%02X ", b); combinedAobStr += bb;
                if (isEnd) combinedAobStr += "] ";
            }
        }
        if (g_verboseCamDiag) {
            DllLog("[DIAG] Processing %zu pending writers as ONE AOB [%llX..%llX] len %zu: %s", toProcess.size(), start, end, bufLen, combinedAobStr.c_str());
        }
        if (blacklisted) {
            g_lastBlacklistedWriteTime.store(GetTickCount64());
            g_blacklistedMode.store(true);
            DllLog("[INFO] Pending set BLACKLISTED — AOB: %s — mouse/render DISABLED, re-check every 100ms", combinedAobStr.c_str());
            return;
        }
        // Not blacklisted — if we were blacklisted, re-enable
        if (g_blacklistedMode.load()) {
            DllLog("[INFO] Blacklist no longer active — re-enabling mouse");
            g_blacklistedMode.store(false);
        }
        // Not blacklisted — NOP all pending at once
        for (uintptr_t rip : toProcess) {
            EnterCriticalSection(&g_writerCS);
            bool alreadyKnown=false;
            for (auto &wr: g_discoveredWriters) if (wr.rip==rip) alreadyKnown=true;
            if (!alreadyKnown) {
                WriterRecord rec={};
                size_t sz = 0;
                // Safe size detect without SEH objects
                {
                    uint8_t tmp[16] = {};
                    for (int i = 0; i < 16; ++i) { uint8_t b = 0; SafeReadU8_SEH(rip + i, b); tmp[i] = b; }
                    sz = DetectWriteInstructionSize(tmp);
                    if (sz == 0 || sz > 15) sz = 7;
                }
                std::string aob = GenerateWriterAob(rip, sz);
                if (rip == 0 || sz == 0) {
                    DllLog("[WARNING] Pending writer at 0x%llX invalid — skip", rip);
                } else if (NopInstruction(rip, rec)) {
                    DllLog("[SUCCESS] Writer NOP'd at 0x%llX size %zu — AOB: %s — total %zu", rip, rec.patchSize, aob.c_str(), g_discoveredWriters.size()+1);
                    g_discoveredWriters.push_back(rec);
                    if (g_pSharedMemory) g_pSharedMemory->m_statusWritersFound = (uint32_t)g_discoveredWriters.size();
                } else {
                    DllLog("[WARNING] Pending writer at 0x%llX unrecognized — skip", rip);
                }
            }
            LeaveCriticalSection(&g_writerCS);
        }
    }

    static void ApplyAllWriterNops() {
        EnterCriticalSection(&g_writerCS);
        for (auto& wr : g_discoveredWriters) {
            if (!wr.nopActive && wr.patchSize > 0) {
                DWORD old = 0;
                if (VirtualProtect((LPVOID)wr.rip, wr.patchSize, PAGE_EXECUTE_READWRITE, &old)) {
                    memset((void*)wr.rip, 0x90, wr.patchSize);
                    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)wr.rip, wr.patchSize);
                    VirtualProtect((LPVOID)wr.rip, wr.patchSize, old, &old);
                    wr.nopActive = true;
                }
            }
        }
        LeaveCriticalSection(&g_writerCS);
    }

    static void RestoreAllWriterNops() {
        EnterCriticalSection(&g_writerCS);
        for (auto& wr : g_discoveredWriters) RestoreInstruction(wr);
        LeaveCriticalSection(&g_writerCS);
    }

    static void StartWriterHunt() {
        ApplyAllWriterNops();
        if (!g_vehHandle) g_vehHandle = AddVectoredExceptionHandler(1, CameraWriterVehHandler);
        g_writerHuntActive = true;
        // Do not arm immediately — arm only on overwrite detection (250ms gate)
        DllLog("[INFO] Writer hunt START (cached %zu)", g_discoveredWriters.size());
    }

    static void StopWriterHunt() {
        g_writerHuntActive = false;
        g_huntFramesLeft.store(0);
        DisarmPageGuard();
        RestoreAllWriterNops();
        EnterCriticalSection(&g_writerCS);
        g_pendingRips.clear();
        LeaveCriticalSection(&g_writerCS);
        if (g_blacklistedMode.load()) {
            g_blacklistedMode.store(false);
            g_lastBlacklistedWriteTime.store(0);
            DllLog("[INFO] Blacklist cleared on hunt STOP");
        }
        DllLog("[INFO] Writer hunt STOP — NOPs restored (%zu remembered)", g_discoveredWriters.size());
    }

    static float ReadFloatBE(uintptr_t address) {
        uint32_t val = 0;
        __try {
            val = *(const uint32_t*)address;
            val = _byteswap_ulong(val);
            float result;
            memcpy(&result, &val, sizeof(float));
            return result;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0.0f;
        }
    }

    static bool WriteFloatBE(uintptr_t address, float val) {
        uint32_t raw;
        memcpy(&raw, &val, sizeof(float));
        raw = _byteswap_ulong(raw);
        __try {
            *(uint32_t*)address = raw;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static int32_t ReadInt32BE(uintptr_t address) {
        uint32_t val = 0;
        __try {
            val = *(const uint32_t*)address;
            return (int32_t)_byteswap_ulong(val);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    static uint8_t ReadByte(uintptr_t address) {
        __try {
            return *(const uint8_t*)address;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    static bool SafeMemcpy(void* dest, const void* src, size_t size) {
        __try {
            memcpy(dest, src, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static void OnThreadExit() {
        if (--g_runningThreads == 0) {
            DllLog("[INFO] All DLL threads exited cleanly.");
            if (g_pSharedMemory) {
                g_pSharedMemory->m_statusScanning = false;
                g_pSharedMemory->m_statusShutdownDone = true;
            }
        }
    }

    static bool IsCompanionAlive() {
        if (!g_pSharedMemory) return true;
        uint32_t compPid = g_pSharedMemory->m_companionPid;
        if (compPid == 0) return true;

        HANDLE hComp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, compPid);
        if (!hComp) {
            return false;
        }
        DWORD exitCode = 0;
        if (GetExitCodeProcess(hComp, &exitCode)) {
            if (exitCode != STILL_ACTIVE) {
                CloseHandle(hComp);
                return false;
            }
        }
        CloseHandle(hComp);
        return true;
    }

    // Helper: safely read 2 bytes from the target process (SEH-guarded, no C++ unwinding)
    static bool SafeReadMarker(uintptr_t addr, uint16_t& out) {
        __try {
            out = *(const uint16_t*)addr;
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

        static DWORD WINAPI ScanAobThread(LPVOID lpParam) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        LoadWriterBlacklist();

        struct AobTask {
            std::wstring name;
            std::string patternStr;
            bool found;
            uintptr_t address;
        };

        bool currentExperimental = false;
        if (g_pSharedMemory) {
            currentExperimental = g_pSharedMemory->m_cfgCemuExperimental;
        }
        DllLog("[INFO] Scanner started. SIMD: %s | Mode: %ls", GetCpuSimdTierName(), currentExperimental ? L"Cemu Experimental" : L"Cemu 2.6");
        CemuVersionConfig vCfg = GetCemuVersionConfig(currentExperimental);

        std::vector<AobTask> tasks = {
            { L"GameRomCamera",  vCfg.gameRomCameraAob, false, 0 },
            { L"MenuState 1",     vCfg.menuStateAob1,   false, 0 },
            { L"MenuState 2",     vCfg.menuStateAob2,   false, 0 },
            { L"ShortcutMenu",    vCfg.shortcutMenuAob, false, 0 },
            { L"Magne Target Sig", vCfg.magnesisAob,     false, 0 }
        };

        bool allOtherFound = false;
        size_t nextIdx = 1;
        while (g_scanning) {
            if (!IsCompanionAlive()) {
                if (g_pSharedMemory) g_pSharedMemory->m_reqShutdown = true;
                break;
            }
            if (g_pSharedMemory) {
                if (g_pSharedMemory->m_reqShutdown) {
                    break;
                }
                g_pSharedMemory->m_statusScanning = true;
                
                // --- Reset pathways ---
                // reset  : full fresh start (Reset button)
                // reset2 : preserve MenuState (MenuState==2)
                if (g_pSharedMemory->m_reqResetScan || g_pSharedMemory->m_reqResetPreserveMenu) {
                    bool preserveMenu = g_pSharedMemory->m_reqResetPreserveMenu && !g_pSharedMemory->m_reqResetScan;
                    // if MenuState not yet valid, preserve is meaningless — do full reset
                    if (preserveMenu && g_addrMenuState.load() == 0) preserveMenu = false;
                    g_pSharedMemory->m_reqResetScan = false;
                    g_pSharedMemory->m_reqResetPreserveMenu = false;

                    DllLog("[INFO] Scanner reset requested (%s). Clearing and reloading blacklist.",
                           preserveMenu ? "preserve MenuState" : "full");
                    LoadWriterBlacklist();

                    currentExperimental = g_pSharedMemory->m_cfgCemuExperimental;
                    vCfg = GetCemuVersionConfig(currentExperimental);
                    DllLog("[INFO] Reset applied. Mode: %ls", currentExperimental ? L"Cemu Experimental" : L"Cemu 2.6");
                    tasks[0].patternStr = vCfg.gameRomCameraAob;
                    tasks[1].patternStr = vCfg.menuStateAob1;
                    tasks[2].patternStr = vCfg.menuStateAob2;
                    tasks[3].patternStr = vCfg.shortcutMenuAob;
                    tasks[4].patternStr = vCfg.magnesisAob;

                    for (size_t i = 0; i < tasks.size(); ++i) {
                        if (preserveMenu && (i == 1 || i == 2)) continue;
                        tasks[i].found = false;
                        tasks[i].address = 0;
                    }
                    g_addrGameRomCamera = 0;
                    g_addrShortcutMenu = 0;
                    if (!preserveMenu) {
                        g_addrMenuState = 0;
                        RemoveMenuStateHooks();
                        EnterCriticalSection(&g_menuCandidateCS);
                        g_menuStateCandidates1.clear();
                        g_menuStateCandidates2.clear();
                        LeaveCriticalSection(&g_menuCandidateCS);
                        g_pSharedMemory->m_statusAddrMenuState = 0;
                        g_pSharedMemory->m_statusMenuTrampolinesReady = false;
                    }
                    g_addrMagneTarget = 0;

                    RemoveShortcutHook();
                    g_tempShortcutAddress = 0;
                    g_tempShortcutValue = 0;
                    if (!preserveMenu) {
                        g_menuStateQueueWriteIdx = 0;
                        g_menuStateQueueReadIdx = 0;
                        memset(g_menuStateQueue, 0, sizeof(g_menuStateQueue));
                    }

                    EnterCriticalSection(&g_patchCS);
                    if (g_magnePatchesInitialized) {
                        g_magneDetourPatch.Restore();
                        g_magneXPatch.Restore();
                        g_magneYPatch.Restore();
                        g_magneZPatch.Restore();
                        g_magnePatchesInitialized = false;
                    }
                    LeaveCriticalSection(&g_patchCS);

                    g_pSharedMemory->m_statusAddrGameRomCamera = 0;
                    g_pSharedMemory->m_statusAddrShortcutMenu = 0;
                    if (!preserveMenu) g_pSharedMemory->m_statusAddrMenuState = 0;
                    g_pSharedMemory->m_statusAddrMagneTarget = 0;
                    g_pSharedMemory->m_patchMagneDetourActive = false;
                    g_pSharedMemory->m_statusShortcutHookReady = false;

                    // Hunter: Reset forgets (full), Reset2 keeps (preserve)
                    if (!preserveMenu) {
                        g_writerHuntActive.store(false);
                        g_huntFramesLeft.store(0);
                        DisarmPageGuard();
                        RemoveFovHook();
                        ClearFovHwBreakpoint();
                        DisarmFovGuard();
                        g_fovWriteCount.store(0);
                        g_lastFovWriteTick.store(0);
                        g_fovHuntActive.store(false);
                        g_fovHuntFramesLeft.store(0);
                        if (g_fovHuntCSInit) { EnterCriticalSection(&g_fovHuntCS); g_fovHuntRips.clear(); LeaveCriticalSection(&g_fovHuntCS); }
                        EnterCriticalSection(&g_writerCS);
                        for (auto& wr : g_discoveredWriters) RestoreInstruction(wr);
                        g_discoveredWriters.clear();
                        g_pendingRips.clear();
                        if (g_pSharedMemory) g_pSharedMemory->m_statusWritersFound = 0;
                        LeaveCriticalSection(&g_writerCS);
                        g_blacklistedMode.store(false);
                        g_lastBlacklistedWriteTime.store(0);
                        DllLog("[INFO] Hunter reset — all writer NOPs cleared, blacklist cleared");
                    } else {
                        g_huntFramesLeft.store(0);
                        DisarmPageGuard();
                        RemoveFovHook();
                        ClearFovHwBreakpoint();
                        DisarmFovGuard();
                        g_fovWriteCount.store(0);
                        g_lastFovWriteTick.store(0);
                        g_fovHuntActive.store(false);
                        g_fovHuntFramesLeft.store(0);
                        if (g_fovHuntCSInit) { EnterCriticalSection(&g_fovHuntCS); g_fovHuntRips.clear(); LeaveCriticalSection(&g_fovHuntCS); }
                        EnterCriticalSection(&g_writerCS);
                        for (auto& wr : g_discoveredWriters) RestoreInstruction(wr);
                        g_discoveredWriters.clear();
                        g_pendingRips.clear();
                        if (g_pSharedMemory) g_pSharedMemory->m_statusWritersFound = 0;
                        LeaveCriticalSection(&g_writerCS);
                        g_blacklistedMode.store(false);
                        g_lastBlacklistedWriteTime.store(0);
                        DllLog("[INFO] Hunter preserve — all writer NOPs cleared (reset2 forgets per your report)");
                    }
                    g_hunterResetPending.store(true);
                    DllLog("[INFO] Hunter explicit reset atomic armed (reset pending until new GameRomCamera)");

                    ResetScannerState();
                    allOtherFound = false;
                    nextIdx = 1;

                    // Load-out delay: loss detectors arm this so we don't race the level load and
                    // lock onto the previous camera block. g_rejectCamAddr stays armed (the reset
                    // itself must not forget which address just died).
                    int delayMs = g_resetDelayMs.exchange(0);
                    if (delayMs > 0) {
                        DllLog("[INFO] Waiting %dms for the load to finish before scanning...", delayMs);
                        uint64_t deadline = GetTickCount64() + (uint64_t)delayMs;
                        while (g_scanning && GetTickCount64() < deadline) {
                            if (g_pSharedMemory && g_pSharedMemory->m_reqShutdown) break;
                            if (IsResetRequested()) break; // newer reset wins — handle next pass
                            Sleep(100);
                        }
                        if (g_pSharedMemory) g_pSharedMemory->m_statusScanning = true;
                    }
                }
            }

            if (tasks[0].found) {
                bool verifySuccess = false;
                if (g_addrGameRomCamera != 0) {
                    uint16_t marker = 0;
                    if (SafeReadMarker(g_addrGameRomCamera, marker)) {
                        verifySuccess = true;
                    }
                }

                if (!verifySuccess) {
                    DllLog("[WARNING] GameRomCamera lost — triggering full scanner reset.");
                    uintptr_t deadCam = g_addrGameRomCamera.load();
                    if (deadCam != 0) {
                        g_rejectCamAddr = deadCam;
                        g_camDefinitivelyLost.store(true);
                        g_rejectEmptyPasses.store(0);
                        DllLog("[INFO] Zombie watch (decisive): camera 0x%llX rejected forever until a different address is found",
                               (unsigned long long)deadCam);
                    }
                    g_resetDelayMs.store(3000);
                    if (g_pSharedMemory) {
                        g_pSharedMemory->m_reqResetScan = true;
                        g_pSharedMemory->m_reqResetPreserveMenu = false;
                    } else {
                        // Fallback when shared memory unavailable: clear locally
                        tasks[0].found = false;
                        tasks[0].address = 0;
                        g_addrGameRomCamera = 0;
                    }
                    // Let the reset handler above do the heavy lifting next iteration
                    // Sleep briefly then continue to reset path
                    for (int i = 0; i < 2 && g_scanning; ++i) Sleep(50);
                    continue;
                }
            }

            if (!tasks[0].found) {
                DllLog("[INFO] Scanning for GameRomCamera...");
                Pattern pat = ParseAOB(tasks[0].patternStr);

                uintptr_t bestCandidate = 0;
                int bestScore = -1;
                uintptr_t fallbackCandidate = 0;
                size_t totalCandidates = 0;
                int rejectedWasValid = 0;

                ScanGameRomCameraImmediate(pat, bestCandidate, bestScore, fallbackCandidate, totalCandidates, rejectedWasValid);
                if (IsResetRequested()) continue;

                // Escape hatch: a rejection that keeps matching the ONLY valid pass after pass was
                // likely a cutscene FOV freeze on the live camera (stall-armed) or heap recycle of
                // the rejected address (decisive). Release before it starves us forever.
                if (bestCandidate == 0 && rejectedWasValid) {
                    int strikes = g_rejectEmptyPasses.fetch_add(1) + 1;
                    int threshold = g_camDefinitivelyLost.load() ? 5 : 2;
                    if (strikes >= threshold) {
                        DllLog("[WARNING] Rejected camera 0x%llX was the only valid candidate %d passes in a row — releasing rejection",
                               (unsigned long long)g_rejectCamAddr.load(), strikes);
                        g_rejectCamAddr = 0;
                        g_rejectEmptyPasses.store(0);
                        g_camDefinitivelyLost.store(false);
                    }
                }

                if (bestCandidate != 0) {
                    tasks[0].found = true;
                    tasks[0].address = bestCandidate;
                    g_addrGameRomCamera = bestCandidate;
                    if (g_pSharedMemory) {
                        g_pSharedMemory->m_statusAddrGameRomCamera = bestCandidate;
                    }
                    DllLog("[SUCCESS] Verified active GameRomCamera at 0x%llX (Score: %d) [%zu candidates scanned]", bestCandidate, bestScore, totalCandidates);
                    // A different live camera was accepted — the game may recycle the rejected
                    // address later, so stop rejecting it.
                    g_rejectCamAddr = 0;
                    g_rejectEmptyPasses.store(0);
                    g_camDefinitivelyLost.store(false);
                } else if (fallbackCandidate != 0) {
                    // Fallback: only if fallback looks sane (FOV 0.1-0.99, not 1.0, marker readable)
                    // Prevents picking a fake like [1.9,4.8,1.8]/Foc[1,1,1]/FOV 1.00 and later corrupting game memory
                    float fbFov = ReadFloatBE(fallbackCandidate + 0x654);
                    uint16_t fbMarker = 0;
                    bool fbMarkerOk = SafeReadMarker(fallbackCandidate, fbMarker);
                    bool fbFovOk = !std::isnan(fbFov) && !std::isinf(fbFov) && fbFov >= 0.1f && fbFov <= 0.99f && fbFov != 1.0f;
                    // extra hard reject for 1.0 signature on fallback too
                    float fbPosX = ReadFloatBE(fallbackCandidate + 0x550);
                    float fbPosY = ReadFloatBE(fallbackCandidate + 0x554);
                    float fbPosZ = ReadFloatBE(fallbackCandidate + 0x558);
                    float fbFocX = ReadFloatBE(fallbackCandidate + 0x63C);
                    float fbFocY = ReadFloatBE(fallbackCandidate + 0x640);
                    float fbFocZ = ReadFloatBE(fallbackCandidate + 0x644);
                    bool fbHasOne = (fbFov==1.0f || fbPosX==1.0f || fbPosY==1.0f || fbPosZ==1.0f || fbFocX==1.0f || fbFocY==1.0f || fbFocZ==1.0f);
                    if (fbFovOk && fbMarkerOk && !fbHasOne) {
                        tasks[0].found = true;
                        tasks[0].address = fallbackCandidate;
                        g_addrGameRomCamera = fallbackCandidate;
                        if (g_pSharedMemory) {
                            g_pSharedMemory->m_statusAddrGameRomCamera = fallbackCandidate;
                        }
                        DllLog("[INFO] Selected initial GameRomCamera at 0x%llX (will re-verify on gameplay) [%zu candidates scanned]", fallbackCandidate, totalCandidates);
                        g_rejectCamAddr = 0;
                        g_rejectEmptyPasses.store(0);
                        g_camDefinitivelyLost.store(false);
                    } else {
                        DllLog("[WARNING] Fallback candidate at 0x%llX rejected (FOV=%.2f markerOk=%d hasOne=%d) — rescanning in 500ms...", fallbackCandidate, fbFov, fbMarkerOk?1:0, fbHasOne?1:0);
                    }
                } else if (totalCandidates > 0) {
                    DllLog("[WARNING] GameRomCamera candidates rejected (%zu scanned). Retrying in 500ms...", totalCandidates);
                } else {
                    DllLog("[WARNING] GameRomCamera pattern not found in memory. Retrying in 500ms...");
                }
                
                // Sleep 500ms before checking again if GameRomCamera not found
                if (!tasks[0].found) {
                    for (int i = 0; i < 5 && g_scanning; ++i) {
                        Sleep(100);
                    }
                    continue;
                }
            }

            bool scanShortcutMenu = g_pSharedMemory ? g_pSharedMemory->m_cfgScanShortcutMenu : true;
            bool scanMenuState = g_pSharedMemory ? g_pSharedMemory->m_cfgScanMenuState : true;
            bool scanMagneTarget = g_pSharedMemory ? g_pSharedMemory->m_cfgScanMagneTarget : true;

            if (!scanShortcutMenu) {
                if (g_shortcutHookActive) RemoveShortcutHook();
                tasks[3].found = false;
                tasks[3].address = 0;
                g_addrShortcutMenu = 0;
                if (g_pSharedMemory) {
                    g_pSharedMemory->m_statusAddrShortcutMenu = 0;
                    g_pSharedMemory->m_teleLiveShortcutMenu = -1;
                }
            }

            if (!scanMenuState) {
                if (g_menuStateHook1Active || g_menuStateHook2Active) RemoveMenuStateHooks();
                EnterCriticalSection(&g_menuCandidateCS);
                g_menuStateCandidates1.clear();
                g_menuStateCandidates2.clear();
                LeaveCriticalSection(&g_menuCandidateCS);
                tasks[1].found = false;
                tasks[1].address = 0;
                tasks[2].found = false;
                tasks[2].address = 0;
                g_addrMenuState = 0;
                if (g_pSharedMemory) {
                    g_pSharedMemory->m_statusAddrMenuState = 0;
                    g_pSharedMemory->m_teleLiveMenuState = 3;
                }
            }

            if (!scanMagneTarget) {
                EnterCriticalSection(&g_patchCS);
                if (g_magnePatchesInitialized) {
                    g_magneDetourPatch.Restore();
                    g_magneXPatch.Restore();
                    g_magneYPatch.Restore();
                    g_magneZPatch.Restore();
                    g_magnePatchesInitialized = false;
                }
                LeaveCriticalSection(&g_patchCS);
                tasks[4].found = false;
                tasks[4].address = 0;
                g_addrMagneTarget = 0;
                if (g_pSharedMemory) {
                    g_pSharedMemory->m_statusAddrMagneTarget = 0;
                    g_pSharedMemory->m_patchMagneDetourActive = false;
                }
            }

            // Find the next unfound task and scan it (MenuState 1/2 before ShortcutMenu, Magnesis last)
            size_t targetIdx = 0;
            for (size_t i = 0; i < tasks.size() - 1; ++i) {
                size_t idx = 1 + ((nextIdx - 1 + i) % (tasks.size() - 1));
                bool enabled = true;
                if (idx == 3) enabled = scanShortcutMenu;
                else if (idx == 1 || idx == 2) enabled = scanMenuState;
                else if (idx == 4) enabled = scanMagneTarget;

                if (enabled && !tasks[idx].found) {
                    targetIdx = idx;
                    break;
                }
            }

            bool foundAny = false;

            // 1. Asynchronously poll active detour hooks on every loop pass
            PollHooksAndSyncSharedMemory();

            if (scanShortcutMenu && g_addrShortcutMenu != 0) {
                tasks[3].found = true;
            }
            if (scanMenuState && g_addrMenuState != 0) {
                tasks[1].found = true;
                tasks[2].found = true;
            }

            // 2. Perform AOB pattern scanning for the current target task
            if (targetIdx != 0) {
                nextIdx = (targetIdx % (tasks.size() - 1)) + 1;

                if (targetIdx == 1 && scanMenuState && !tasks[1].found && !g_menuStateHook1Active && g_addrMenuState == 0) {
                    DllLog("[INFO] Scanning for MenuState AOB 1 instruction pattern...");
                    Pattern pat = ParseAOB(tasks[1].patternStr);
                    uintptr_t foundAddress = 0;
                    if (ScanProcessAOB(pat, foundAddress)) {
                        DllLog("[SUCCESS] Found MenuState AOB 1 instruction at 0x%llX. Setting up trampoline hook 1...", foundAddress);
                        tasks[1].address = foundAddress;
                        if (SetupMenuStateHook(foundAddress, 1)) {
                            DllLog("[SUCCESS] MenuState trampoline hook 1 set up successfully. Waiting for game write...");
                        } else {
                            DllLog("[ERROR] Failed to set up trampoline hook 1 for MenuState.");
                        }
                    } else if (!IsResetRequested()) {
                        DllLog("[WARNING] MenuState AOB 1 instruction pattern not found. Retrying in 1s...");
                    }
                } else if (targetIdx == 2 && scanMenuState && !tasks[2].found && !g_menuStateHook2Active && g_addrMenuState == 0) {
                    DllLog("[INFO] Scanning for MenuState AOB 2 instruction pattern...");
                    Pattern pat = ParseAOB(tasks[2].patternStr);
                    uintptr_t foundAddress = 0;
                    if (ScanProcessAOB(pat, foundAddress)) {
                        DllLog("[SUCCESS] Found MenuState AOB 2 instruction at 0x%llX. Setting up trampoline hook 2...", foundAddress);
                        tasks[2].address = foundAddress;
                        if (SetupMenuStateHook(foundAddress, 2)) {
                            DllLog("[SUCCESS] MenuState trampoline hook 2 set up successfully. Waiting for game write...");
                        } else {
                            DllLog("[ERROR] Failed to set up trampoline hook 2 for MenuState.");
                        }
                    } else if (!IsResetRequested()) {
                        DllLog("[WARNING] MenuState AOB 2 instruction pattern not found. Retrying in 1s...");
                    }
                } else if (targetIdx == 3 && scanShortcutMenu && !tasks[3].found && !g_shortcutHookActive) {
                    DllLog("[INFO] Scanning for ShortcutMenu instruction pattern...");
                    Pattern pat = ParseAOB(tasks[3].patternStr);
                    uintptr_t foundAddress = 0;
                    if (ScanProcessAOB(pat, foundAddress)) {
                        DllLog("[SUCCESS] Found ShortcutMenu instruction at 0x%llX. Setting up detour hook...", foundAddress);
                        tasks[3].address = foundAddress;
                        if (SetupShortcutHook(foundAddress)) {
                            DllLog("[SUCCESS] Detour hook set up successfully. Waiting for game write...");
                        } else {
                            DllLog("[ERROR] Failed to set up detour hook for ShortcutMenu.");
                        }
                    } else if (!IsResetRequested()) {
                        DllLog("[WARNING] ShortcutMenu instruction pattern not found. Retrying in 1s...");
                    }
                } else if (targetIdx == 4 && scanMagneTarget && !tasks[4].found) {
                    DllLog("[INFO] Scanning for Magne Target Sig...");
                    Pattern pat = ParseAOB(tasks[4].patternStr);
                    std::vector<uintptr_t> rawCandidates;
                    if (ScanProcessAOBAll(pat, rawCandidates)) {
                        if (g_verboseCamDiag) {
                            DllLog("[DIAG] Found %zu Magne Target Sig candidate(s). Verifying offsets and values...", rawCandidates.size());
                        }
                        uintptr_t bestCandidate = 0;
                        int bestScore = -1;

                        for (size_t i = 0; i < rawCandidates.size(); ++i) {
                            uintptr_t cand = rawCandidates[i];
                            int score = 0;
                            bool valid = VerifyMagneTargetSig(cand, vCfg, currentExperimental, score);

                            if (g_verboseCamDiag) {
                                DllLog("[DIAG] Magne candidate [%zu/%zu] at 0x%llX: Score=%d -> %s",
                                       i + 1, rawCandidates.size(), cand, score, valid ? "VALID" : "REJECTED");
                            }

                            if (valid && score > bestScore) {
                                bestScore = score;
                                bestCandidate = cand;
                            }
                        }

                        if (bestCandidate != 0) {
                            tasks[4].address = bestCandidate;
                            tasks[4].found = true;
                            foundAny = true;
                            DllLog("[SUCCESS] Verified Magne Target Sig at 0x%llX (Score: %d). Detour hooks injected.", bestCandidate, bestScore);

                            if (g_pSharedMemory) {
                                g_pSharedMemory->m_statusAddrMagneTarget = bestCandidate;
                            }
                            g_addrMagneTarget = bestCandidate;

                            EnterCriticalSection(&g_patchCS);
                            if (!g_magnePatchesInitialized) {
                                g_magneXPatch = { bestCandidate + vCfg.magnesisXOffset, 7, {}, false };
                                g_magneYPatch = { bestCandidate + vCfg.magnesisYOffset, 7, {}, false };
                                g_magneZPatch = { bestCandidate + vCfg.magnesisZOffset, 7, {}, false };

                                g_magneXPatch.Backup();
                                g_magneYPatch.Backup();
                                g_magneZPatch.Backup();

                                if (vCfg.detourTargetAxis == 'Z') {
                                    g_magneDetourPatch = { bestCandidate + vCfg.magnesisZOffset, vCfg.magnesisDetourSize, {}, false };
                                    g_magneDetourPatch.Backup();
                                    g_magnesisZWriterReturn = bestCandidate + vCfg.magnesisZOffset + vCfg.magnesisDetourSize;
                                    if (currentExperimental) {
                                        g_magneDetourPatch.InjectDetour((uintptr_t)&AsmMagnesisZWriterExp);
                                    } else {
                                        g_magneDetourPatch.InjectDetour((uintptr_t)&AsmMagnesisZWriter);
                                    }
                                } else if (vCfg.detourTargetAxis == 'Y') {
                                    g_magneDetourPatch = { bestCandidate + vCfg.magnesisYOffset, vCfg.magnesisDetourSize, {}, false };
                                    g_magneDetourPatch.Backup();
                                    g_magnesisYWriterReturn = bestCandidate + vCfg.magnesisYOffset + vCfg.magnesisDetourSize;
                                    g_magneDetourPatch.InjectDetour((uintptr_t)&AsmMagnesisYWriterExp);
                                }

                                g_magnePatchesInitialized = true;
                                if (g_pSharedMemory) {
                                    g_pSharedMemory->m_patchMagneDetourActive = true;
                                }
                            }
                            LeaveCriticalSection(&g_patchCS);
                        } else if (!IsResetRequested()) {
                            DllLog("[WARNING] All Magne Target Sig candidates rejected by offset/opcode verification. Retrying in 1s...");
                        }
                    } else if (!IsResetRequested()) {
                        DllLog("[WARNING] Magne Target Sig not found. Retrying in 1s...");
                    }
                }
            }

            if (IsResetRequested()) continue;

            allOtherFound = true;
            for (size_t i = 1; i < tasks.size(); ++i) {
                bool enabled = true;
                if (i == 3) enabled = scanShortcutMenu;
                else if (i == 1 || i == 2) enabled = scanMenuState;
                else if (i == 4) enabled = scanMagneTarget;

                if (enabled && !tasks[i].found) {
                    allOtherFound = false;
                    break;
                }
            }

            if (g_pSharedMemory) {
                g_pSharedMemory->m_statusAddrGameRomCamera = g_addrGameRomCamera.load();
                g_pSharedMemory->m_statusAddrShortcutMenu  = scanShortcutMenu ? g_addrShortcutMenu.load() : 0;
                g_pSharedMemory->m_statusAddrMenuState     = scanMenuState ? g_addrMenuState.load() : 0;
                g_pSharedMemory->m_statusAddrMagneTarget   = scanMagneTarget ? g_addrMagneTarget.load() : 0;
                g_pSharedMemory->m_statusMenuTrampolinesReady = scanMenuState && g_menuStateHook1Active && g_menuStateHook2Active;
                g_pSharedMemory->m_statusShortcutHookReady = scanShortcutMenu && g_shortcutHookActive;

                EnterCriticalSection(&g_patchCS);
                g_pSharedMemory->m_patchMagneDetourActive = scanMagneTarget && g_magnePatchesInitialized && g_magneDetourPatch.active;
                LeaveCriticalSection(&g_patchCS);
            }

            int sleepTicks = 10;
            if (allOtherFound) {
                sleepTicks = 10; // Sleep 1 second when all found
            } else if (foundAny) {
                sleepTicks = 1;  // Sleep 100ms when we just found one to quickly scan the next
            } else {
                sleepTicks = 10; // Sleep 1 second before scanning again
            }

            for (int i = 0; i < sleepTicks && g_scanning; ++i) {
                Sleep(100);
            }
        }

        RemoveShortcutHook();

        if (g_pSharedMemory) {
            g_pSharedMemory->m_statusScanning = false;
        }
        OnThreadExit();
        return 0;
    }

    struct TargetWndData {
        DWORD pid;
        HWND hWnd;
    };

    static BOOL CALLBACK FindTargetWindowProc(HWND hWnd, LPARAM lParam) {
        TargetWndData* data = reinterpret_cast<TargetWndData*>(lParam);
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

    static HWND GetTargetWindow(DWORD pid) {
        TargetWndData data = { pid, nullptr };
        EnumWindows(FindTargetWindowProc, reinterpret_cast<LPARAM>(&data));
        return data.hWnd;
    }

    static POINT GetCemuWindowCenter(HWND hWnd) {
        POINT pt = {0, 0};
        RECT rect;
        if (hWnd && GetClientRect(hWnd, &rect)) {
            pt.x = (rect.left + rect.right) / 2;
            pt.y = (rect.top + rect.bottom) / 2;
            ClientToScreen(hWnd, &pt);
            return pt;
        }
        pt.x = GetSystemMetrics(SM_CXSCREEN) / 2;
        pt.y = GetSystemMetrics(SM_CYSCREEN) / 2;
        return pt;
    }

    static bool g_originalCursorsRestored = true;

    static void SetGlobalCursorVisibility(bool visible) {
        const UINT local_OCR_NORMAL = 32512;
        if (visible) {
            if (!g_originalCursorsRestored) {
                SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
                g_originalCursorsRestored = true;
            }
        } else {
            if (g_originalCursorsRestored) {
                int w = GetSystemMetrics(SM_CXCURSOR);
                int h = GetSystemMetrics(SM_CYCURSOR);
                std::vector<uint8_t> andMask(w * h / 8, 0xFF);
                std::vector<uint8_t> xorMask(w * h / 8, 0x00);
                HCURSOR transparentCursor = CreateCursor(
                    nullptr,
                    0,
                    0,
                    w,
                    h,
                    andMask.data(),
                    xorMask.data()
                );
                if (transparentCursor) {
                    SetSystemCursor(transparentCursor, local_OCR_NORMAL);
                    g_originalCursorsRestored = false;
                }
            }
        }
    }

    static HHOOK g_hMouseHook = nullptr;
    static std::atomic<int> g_scrollDelta{0};

    static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode >= 0 && wParam == WM_MOUSEWHEEL) {
            MSLLHOOKSTRUCT* pMouse = (MSLLHOOKSTRUCT*)lParam;
            int delta = (short)HIWORD(pMouse->mouseData);
            g_scrollDelta.fetch_add(delta);
        }
        return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
    }

    static void StartMouseHook() {
        if (!g_hMouseHook) {
            g_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, g_hModule, 0);
        }
    }

    static void StopMouseHook() {
        if (g_hMouseHook) {
            UnhookWindowsHookEx(g_hMouseHook);
            g_hMouseHook = nullptr;
        }
    }

    static HWND g_hCemuWnd = nullptr;

    static void CemuKeyInjector_SendKey(uint16_t keycode, bool up) {
        UINT scan = MapVirtualKeyW(keycode, MAPVK_VK_TO_VSC);
        DWORD flags = up ? KEYEVENTF_KEYUP : 0;
        flags |= KEYEVENTF_SCANCODE;

        bool is_extended = false;
        if ((keycode >= 0x21 && keycode <= 0x28) || keycode == 0x2D || keycode == 0x2E) is_extended = true;
        else if (keycode >= 0x5B && keycode <= 0x5D) is_extended = true;
        else if (keycode >= 0xA2 && keycode <= 0xA5) is_extended = true;
        else if (keycode == 0x90 || keycode == 0x91) is_extended = true;
        else if (keycode == 0x6F) is_extended = true;

        if (is_extended) {
            flags |= KEYEVENTF_EXTENDEDKEY;
        }

        INPUT input = {0};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = 0;
        input.ki.wScan = static_cast<WORD>(scan);
        input.ki.dwFlags = flags;
        input.ki.time = 0;
        input.ki.dwExtraInfo = 0;

        // SendInput works when Cemu has focus
        SendInput(1, &input, sizeof(INPUT));

        // Post WM_KEYDOWN/WM_KEYUP directly to Cemu window message queue so input works when out of focus
        if (!g_hCemuWnd) {
            g_hCemuWnd = GetTargetWindow(GetCurrentProcessId());
        }
        if (g_hCemuWnd) {
            LPARAM lp = 1 | (scan << 16);
            if (is_extended) lp |= (1 << 24);
            if (up) {
                lp |= (1u << 30) | (1u << 31);
                PostMessageW(g_hCemuWnd, WM_KEYUP, keycode, lp);
            } else {
                PostMessageW(g_hCemuWnd, WM_KEYDOWN, keycode, lp);
            }
        }
    }

    static int MouseVk(int idx) {
        switch (idx) {
            case 0: return 0x01; // VK_LBUTTON
            case 1: return 0x02; // VK_RBUTTON
            case 2: return 0x04; // VK_MBUTTON
            case 3: return 0x05; // VK_XBUTTON1
            case 4: return 0x06; // VK_XBUTTON2
            default: return 0;
        }
    }

    static DWORD WINAPI CameraControlThread(LPVOID lpParam) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        
        HWND hCemuWnd = GetTargetWindow(GetCurrentProcessId());

        timeBeginPeriod(1);

        bool last_f2_state = false;
        bool last_f3_state = false;
        
        bool virt_cam_initialized = false;
        float vcam_pos_x = 0.0f, vcam_pos_y = 0.0f, vcam_pos_z = 0.0f;
        float orbit_angle = 0.0f;
        float orbit_radius = 20.0f;
        float orbit_pitch = 0.0f;
        float current_orbit_angle = 0.0f;
        float current_orbit_pitch = 0.0f;
        
        bool magne_initialized = false;
        float magne_off_x = 0.0f, magne_off_y = 0.0f, magne_off_z = 0.0f;
        uint64_t last_heartbeat = 0;
        auto last_heartbeat_time = std::chrono::steady_clock::now();

        float mouse_menu_accum_x = 0.0f;
        float mouse_menu_accum_y = 0.0f;
        int scroll_accumulator = 0;
        ScrollMenuType active_menu = ScrollMenuType::None;
        auto menu_hold_timer = std::chrono::steady_clock::now();

        static bool last_should_control = false;
        auto last_frame_time = std::chrono::steady_clock::now();

        // Hunter: 250ms detect, 100ms catch (10 frames @4ms) + 100ms blacklist re-check
        float hunter_lastWrittenX = 0.0f, hunter_lastWrittenY = 0.0f, hunter_lastWrittenZ = 0.0f;
        bool  hunter_hasWritten = false;
        bool  hunter_mouseMovedSinceLastCheck = false;
        uint64_t hunter_lastCheckMs = 0;
        uint64_t hunter_blacklistRecheckMs = 0;
        int   hunter_aobDumpCountdown = 0;

        bool ki_enabled = true;
        bool prev_pressed[5] = {false, false, false, false, false};
        std::chrono::steady_clock::time_point press_time[5];
        bool press_time_valid[5] = {false, false, false, false, false};
        float magne_accum_dt = 0.0f;

        StartMouseHook();

        while (g_cameraControlRunning) {
            if (g_pSharedMemory && g_pSharedMemory->m_reqShutdown) {
                break;
            }
            if (!IsCompanionAlive()) {
                if (g_pSharedMemory) g_pSharedMemory->m_reqShutdown = true;
                break;
            }
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            }

            auto loop_now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(loop_now - last_frame_time).count();
            if (dt > 0.1f) dt = 0.1f;
            last_frame_time = loop_now;

            uintptr_t gc_addr = g_addrGameRomCamera ? (g_addrGameRomCamera + 0x630) : 0;
            if (g_pSharedMemory && g_addrGameRomCamera != 0) {
                g_pSharedMemory->m_telePivotX = ReadFloatBE(g_addrGameRomCamera + 0x674);
                g_pSharedMemory->m_telePivotY = ReadFloatBE(g_addrGameRomCamera + 0x678);
                g_pSharedMemory->m_telePivotZ = ReadFloatBE(g_addrGameRomCamera + 0x67C);
            }
            static uintptr_t last_gc_addr = 0;
            if (gc_addr != last_gc_addr) {
                uintptr_t baseForFov = g_addrGameRomCamera.load();
                if (baseForFov != 0 && gc_addr != 0) {
                    EnterCriticalSection(&g_fovHuntCS);
                    g_fovHuntRips.clear();
                    LeaveCriticalSection(&g_fovHuntCS);
                    g_fovHuntFramesLeft.store(90);
                    g_fovHuntActive.store(true);
                    SetFovHwBreakpoint(baseForFov + 0x68C);
                    DllLog("[INFO] FOV hunter armed 90 frames base 0x%llX DR0 0x%llX", baseForFov, baseForFov + 0x68C);
                } else if (gc_addr == 0) {
                    g_fovHuntActive.store(false);
                    g_fovHuntFramesLeft.store(0);
                    ClearFovHwBreakpoint();
                }
                last_gc_addr = gc_addr;
            }
            // FOV hunter DR0 — write-only, no PAGE_GUARD, re-arms if 90 expire empty
            if (g_fovHuntActive.load()) {
                // Immediate log for new RIPs (like Cheat Engine F6) — don't wait 90 frames
                {
                    static size_t lastLogCount = 0;
                    if (TryEnterCriticalSection(&g_fovHuntCS)) {
                        if (g_fovHuntRips.size() > lastLogCount) {
                            for (size_t i = lastLogCount; i < g_fovHuntRips.size(); ++i) {
                                uintptr_t r = g_fovHuntRips[i];
                                DllLog("[INFO] FOV hunter RIP 0x%llX (%zu total)", r, g_fovHuntRips.size());
                            }
                            lastLogCount = g_fovHuntRips.size();
                        }
                        if (g_fovHuntRips.empty()) lastLogCount = 0;
                        LeaveCriticalSection(&g_fovHuntCS);
                    }
                }
                if (g_fovHuntFramesLeft.load() > 0) {
                    static int fovRearmCount = 0;
                    int prev = g_fovHuntFramesLeft.fetch_sub(1);
                    if (prev == 1) {
                        EnterCriticalSection(&g_fovHuntCS);
                        bool found = !g_fovHuntRips.empty();
                        LeaveCriticalSection(&g_fovHuntCS);
                        if (found) {
                            fovRearmCount = 0;
                            g_fovHuntActive.store(false);
                            // Keep DR0 for stall counting — don't clear, don't hook (hook crashes JIT)
                            EnterCriticalSection(&g_fovHuntCS);
                            DllLog("[INFO] FOV hunter done — %zu writer(s)", g_fovHuntRips.size());
                            for (uintptr_t r : g_fovHuntRips) {
                                DllLog("[INFO] FOV writer RIP 0x%llX", r);
                            }
                            LeaveCriticalSection(&g_fovHuntCS);
                            g_fovWriteCount.store(0);
                            g_lastFovWriteTick.store(GetTickCount64());
                            DllLog("[INFO] FOV DR0 keeper for stall/new-addr watch (no hook, hw count)");
                            // DR0 stays armed — VEH will now count writes via g_fovWriteCount
                        } else {
                            fovRearmCount++;
                            DllLog("[INFO] FOV hunter no writer in 90 frames — re-arming (%d/3)", fovRearmCount);
                            if (fovRearmCount > 3) {
                                DllLog("[WARNING] FOV hunter 3x no writer — GameRomCamera invalid, triggering full reset2 (ignore MenuState 1/2)");
                                fovRearmCount = 0;
                                g_fovHuntActive.store(false);
                                ClearFovHwBreakpoint();
                                g_rejectCamAddr = g_addrGameRomCamera.load();
                                g_resetDelayMs.store(3000);
                                g_camDefinitivelyLost.store(false); // static live scenes also write no FOV — allow escape hatch
                                g_rejectEmptyPasses.store(0);
                                DllLog("[INFO] Zombie watch (hunter 3x): camera 0x%llX rejected, 3s load delay armed",
                                       (unsigned long long)g_rejectCamAddr.load());
                                if (g_pSharedMemory) {
                                    g_pSharedMemory->m_reqResetScan = true; // full reset, ignore MenuState 1/2 per lord
                                    g_pSharedMemory->m_reqResetPreserveMenu = false;
                                }
                                g_fovWriteCount.store(0);
                                g_lastFovWriteTick.store(0);
                                if (g_fovHuntCSInit) { EnterCriticalSection(&g_fovHuntCS); g_fovHuntRips.clear(); LeaveCriticalSection(&g_fovHuntCS); }
                            } else {
                                g_fovHuntFramesLeft.store(90);
                                if (g_addrGameRomCamera.load() != 0) SetFovHwBreakpoint(g_addrGameRomCamera.load() + 0x68C);
                            }
                        }
                    }
                } else {
                    g_fovHuntActive.store(false);
                    ClearFovHwBreakpoint();
                }
            }

            // FOV DR0 watcher — stall 1.5s -> reset2, new-addr via re-hunt after stall
            if (g_fovHwActive.load() && !g_fovHuntActive.load()) {
                uint64_t cnt = g_fovWriteCount.load();
                static uint64_t lastSeenCnt = 0;
                static uint64_t lastSeenTick = 0;
                uint64_t now = GetTickCount64();
                if (cnt != lastSeenCnt) {
                    lastSeenCnt = cnt;
                    lastSeenTick = now;
                    g_lastFovWriteTick.store(now);
                } else if (cnt > 3 && lastSeenTick != 0 && now - lastSeenTick >= 3000) {
                    DllLog("[INFO] FOV stall %.1fs (writes %llu) — level change, triggering full reset2 (ignore MenuState 1/2)", (now - lastSeenTick) / 1000.0, cnt);
                    if (g_mousecamActive) g_fovStallReenablePending.store(true);
                    uintptr_t stallCam = g_addrGameRomCamera.load();
                    if (stallCam != 0) {
                        g_rejectCamAddr = stallCam;
                        g_camDefinitivelyLost.store(false); // cutscenes stall FOV on LIVE cameras too — allow escape hatch
                        g_rejectEmptyPasses.store(0);
                        DllLog("[INFO] Zombie watch (stall): camera 0x%llX rejected for next scan, 3s load delay armed",
                               (unsigned long long)stallCam);
                    }
                    if (g_pSharedMemory) {
                        g_pSharedMemory->m_reqResetScan = true;
                        g_pSharedMemory->m_reqResetPreserveMenu = false;
                    }
                    g_fovWriteCount.store(0);
                    lastSeenCnt = 0;
                    lastSeenTick = now;
                    g_lastFovWriteTick.store(now);
                }
                // New-addr is handled via stall->reset2->re-hunt DR0
            }
            // Auto re-enable mousecam after FOV stall reset — must re-capture mouse even before writers found
            // FIX thrash: was firing every 4ms while pending, causing SHOULD_CTRL thrash (14:40:02 spam)
            {
                static bool s_fovStallForceSent = false;
                if (g_fovStallReenablePending.load() && !g_fovHuntActive.load() && g_addrGameRomCamera.load() != 0) {
                    bool isBlacklisted = g_blacklistedMode.load();
                    if (!isBlacklisted) {
                        if (!s_fovStallForceSent) {
                            bool wasInactive = !g_mousecamActive.load();
                            if (wasInactive) {
                                g_mousecamActive = true;
                                if (g_pSharedMemory) g_pSharedMemory->m_statusMousecamActive = true;
                            }
                            g_forceShouldControlReset.store(true);
                            g_originalCursorsRestored = true;
                            s_fovStallForceSent = true;
                            DllLog("[INFO] FOV stall auto re-enable mousecam %s (forcing should_control) blacklisted=%d writers=%zu", wasInactive?"":"already active", isBlacklisted?1:0, g_discoveredWriters.size());
                            HWND fg = GetForegroundWindow();
                            DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
                            if (fg && pid == GetCurrentProcessId() && g_addrGameRomCamera.load() != 0) {
                                POINT center = GetCemuWindowCenter(fg);
                                SetCursorPos(center.x, center.y);
                                DllLog("[INFO] Auto center cursor to %d,%d (forcing should_control)", center.x, center.y);
                            }
                        }
                    } else if (g_verboseCamDiag) {
                        DllLog("[DIAG] FOV stall pending but blacklisted — keep waiting");
                    }
                } else if (!g_fovStallReenablePending.load()) {
                    s_fovStallForceSent = false;
                }
            }
            // Diagnostic: if mousecam should be active but mouse is free, log why
            HWND hwndFg = GetForegroundWindow();
            DWORD fgPid = 0;
            if (hwndFg != nullptr) {
                GetWindowThreadProcessId(hwndFg, &fgPid);
            }
            bool is_foreground = (fgPid == GetCurrentProcessId());

            static bool prev_foreground = false;
            bool foreground_transition = is_foreground && !prev_foreground;
            prev_foreground = is_foreground;

            bool req_toggle = false;
            if (g_pSharedMemory && g_pSharedMemory->m_reqToggleMousecam) {
                g_pSharedMemory->m_reqToggleMousecam = false;
                req_toggle = true;
            }

            bool f2_pressed = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
            bool f2_triggered = f2_pressed && !last_f2_state;
            last_f2_state = f2_pressed;

            static auto last_toggle_time = std::chrono::steady_clock::now() - std::chrono::milliseconds(500);
            auto now_toggle = std::chrono::steady_clock::now();
            auto elapsed_toggle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_toggle - last_toggle_time).count();

            if ((f2_triggered || req_toggle) && elapsed_toggle_ms >= 200) {
                last_toggle_time = now_toggle;
                if (gc_addr != 0) {
                    g_mousecamActive = !g_mousecamActive;
                    virt_cam_initialized = false;
                    
                    if (g_mousecamActive) {
                        DllLog("[INFO] Mouse camera ENABLED (F2)");
                        if (is_foreground) {
                            POINT center = GetCemuWindowCenter(hwndFg);
                            SetCursorPos(center.x, center.y);
                        }
                    } else {
                        DllLog("[INFO] Mouse camera DISABLED (F2)");
                    }
                } else {
                    DllLog("[WARNING] Cannot toggle camera: GameRomCamera not found yet.");
                }
            }

            // F3 — full scanner reset (same as UI Reset button)
            bool f3_pressed = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
            bool f3_triggered = f3_pressed && !last_f3_state;
            last_f3_state = f3_pressed;
            static auto last_f3_time = std::chrono::steady_clock::now() - std::chrono::milliseconds(500);
            auto now_f3 = std::chrono::steady_clock::now();
            auto elapsed_f3_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_f3 - last_f3_time).count();
            if (f3_triggered && elapsed_f3_ms >= 200) {
                last_f3_time = now_f3;
                if (g_pSharedMemory) {
                    g_pSharedMemory->m_reqResetScan = true;
                    g_pSharedMemory->m_reqResetPreserveMenu = false;
                }
                DllLog("[INFO] Reset requested via F3 — full scanner reset.");
            }

            if (g_pSharedMemory) {
                g_pSharedMemory->m_statusMousecamActive = g_mousecamActive.load();
            }

            if (g_mousecamActive && foreground_transition) {
                POINT center = GetCemuWindowCenter(hwndFg);
                SetCursorPos(center.x, center.y);
            }

            if (g_mousecamActive && is_foreground) {
                SetGlobalCursorVisibility(false);
            } else {
                SetGlobalCursorVisibility(true);
            }

            bool scanMenuState = g_pSharedMemory ? g_pSharedMemory->m_cfgScanMenuState : true;
            bool scanShortcutMenu = g_pSharedMemory ? g_pSharedMemory->m_cfgScanShortcutMenu : true;

            bool menu_active = false;
            if (scanMenuState && g_addrMenuState != 0) {
                int32_t val = ReadInt32BE(g_addrMenuState);
                g_liveMenuState = val;
                if (g_pSharedMemory) {
                    g_pSharedMemory->m_teleLiveMenuState = static_cast<uint8_t>(val);
                }
                menu_active = (val == 6 || val == 10);
            } else {
                menu_active = false;
                g_liveMenuState = 3;
                if (g_pSharedMemory) {
                    g_pSharedMemory->m_teleLiveMenuState = 3;
                }
            }

            bool is_shortcut_open = false;
            if (scanShortcutMenu && g_addrShortcutMenu != 0) {
                is_shortcut_open = (ReadInt32BE(g_addrShortcutMenu + 128) != -1);
            }

            bool magnesis_auto_active = false;
            uint64_t current_heartbeat = g_magneHeartbeatCounter;
            // The assembly detour increments the heartbeat counter every frame
            // while magnesis is active. If the counter changed, magnesis is on.
            // We also keep it "active" for 500 ms after the last heartbeat to
            // bridge brief gaps (loading screens, menu transitions) without
            // snapping the magnesis object back to its default position.
            if (current_heartbeat != last_heartbeat) {
                magnesis_auto_active = true;
                last_heartbeat = current_heartbeat;
                last_heartbeat_time = std::chrono::steady_clock::now();
            } else {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - last_heartbeat_time).count();
                if (elapsed < 500) {
                    magnesis_auto_active = true;
                }
            }

            bool magnesis_mode = magnesis_auto_active && g_mousecamActive;
            bool fps_magne_active = magnesis_mode && g_pSharedMemory && g_pSharedMemory->m_cfgFpsMagnesis;

            // The JIT compiler periodically recompiles code paths, which can
            // overwrite our injected detour bytes (jmp [AsmMagnesisXWriter]).
            // We poll the first byte of the detour site every frame and
            CemuVersionConfig vCfg = GetCemuVersionConfig(g_pSharedMemory ? g_pSharedMemory->m_cfgCemuExperimental : false);

            // re-inject if it's been stomped back to the original instruction.
            if (g_magnePatchesInitialized && g_magneDetourPatch.address != 0) {
                uint8_t currentByte = 0;
                SIZE_T bytesRead = 0;
                if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)g_magneDetourPatch.address, &currentByte, 1, &bytesRead)) {
                    if (bytesRead == 1 && currentByte != 0xFF) {
                        // The detour was overwritten! Re-inject it!
                        g_magneDetourPatch.active = false; // Force it to allow reinjection
                        bool experimental = g_pSharedMemory ? g_pSharedMemory->m_cfgCemuExperimental : false;
                        if (vCfg.detourTargetAxis == 'Z') {
                            if (experimental) {
                                g_magneDetourPatch.InjectDetour((uintptr_t)&AsmMagnesisZWriterExp);
                            } else {
                                g_magneDetourPatch.InjectDetour((uintptr_t)&AsmMagnesisZWriter);
                            }
                            if (magnesis_mode) {
                                g_magneXPatch.active = false;
                                g_magneYPatch.active = false;
                                g_magneXPatch.ApplyNop();
                                g_magneYPatch.ApplyNop();
                            }
                        } else if (vCfg.detourTargetAxis == 'Y') {
                            g_magneDetourPatch.InjectDetour((uintptr_t)&AsmMagnesisYWriterExp);
                            if (magnesis_mode) {
                                g_magneXPatch.active = false;
                                g_magneZPatch.active = false;
                                g_magneXPatch.ApplyNop();
                                g_magneZPatch.ApplyNop();
                            }
                        }
                    }
                }
            }
            static bool last_magnesis_mode = false;
            if (magnesis_mode != last_magnesis_mode) {
                last_magnesis_mode = magnesis_mode;
                g_magnesisEnabled = magnesis_mode ? 1 : 0;
                if (!magnesis_mode) {
                    g_magneIdealBase = 0;
                    magne_initialized = false;
                    magne_accum_dt = 0.0f;
                }
            }

            if (g_magnePatchesInitialized) {
                EnterCriticalSection(&g_patchCS);
                if (magnesis_mode) {
                    if (vCfg.detourTargetAxis == 'Z') {
                        if (!g_magneXPatch.active) g_magneXPatch.ApplyNop();
                        if (!g_magneYPatch.active) g_magneYPatch.ApplyNop();
                    } else if (vCfg.detourTargetAxis == 'Y') {
                        if (!g_magneXPatch.active) g_magneXPatch.ApplyNop();
                        if (!g_magneZPatch.active) g_magneZPatch.ApplyNop();
                    }
                } else {
                    if (g_magneXPatch.active) g_magneXPatch.Restore();
                    if (g_magneYPatch.active) g_magneYPatch.Restore();
                    if (g_magneZPatch.active) g_magneZPatch.Restore();
                }
                LeaveCriticalSection(&g_patchCS);
            }

            if (g_forceShouldControlReset.exchange(false)) last_should_control = false;
            bool should_control = g_mousecamActive && (!magnesis_mode || fps_magne_active) && !menu_active && !is_shortcut_open;
            if (should_control != last_should_control) {
                last_should_control = should_control;
                if (should_control) {
                    StartWriterHunt();
                    hunter_hasWritten = false;
                    hunter_mouseMovedSinceLastCheck = false;
                    hunter_lastCheckMs = GetTickCount64();
                    g_huntFramesLeft.store(0);
                } else {
                    StopWriterHunt();
                    hunter_hasWritten = false;
                    hunter_mouseMovedSinceLastCheck = false;
                    g_huntFramesLeft.store(0);
                }
                // SMOOTH TRANSITION: Restore orbital camera angles from the game camera on recapture (1:1 with Rust)
                if (should_control && gc_addr != 0) {
                    float gc_pos_x = ReadFloatBE(gc_addr + 0);
                    float gc_pos_y = ReadFloatBE(gc_addr + 4);
                    float gc_pos_z = ReadFloatBE(gc_addr + 8);
                    float gc_focus_x = ReadFloatBE(gc_addr + 0xC);
                    float gc_focus_y = ReadFloatBE(gc_addr + 0x10);
                    float gc_focus_z = ReadFloatBE(gc_addr + 0x14);

                    vcam_pos_x = gc_pos_x;
                    vcam_pos_y = gc_pos_y;
                    vcam_pos_z = gc_pos_z;

                    float d_x = gc_pos_x - gc_focus_x;
                    float d_y = gc_pos_y - gc_focus_y;
                    float d_z = gc_pos_z - gc_focus_z;

                    orbit_radius = sqrt(d_x * d_x + d_y * d_y + d_z * d_z);
                    orbit_angle = atan2(d_x, d_z);
                    orbit_pitch = asin(d_y / orbit_radius);

                    if (orbit_radius < 5.0f) {
                        orbit_radius = 20.0f;
                    }
                    current_orbit_angle = orbit_angle;
                    current_orbit_pitch = orbit_pitch;
                    virt_cam_initialized = true;

                    if (is_foreground) {
                        POINT center = GetCemuWindowCenter(hwndFg);
                        SetCursorPos(center.x, center.y);
                    }
                }
            }

            // Explicit reset atomic: restart writer hunt after GameRomCamera re-found (reset2) — delayed to let level load finish
            if (g_hunterResetPending.load() && gc_addr != 0) {
                if (g_hunterResetPending.exchange(false)) {
                    bool isFovStall = g_fovStallReenablePending.load();
                    DllLog("[INFO] Writer hunt explicit reset — new GameRomCamera 0x%llX, restarting detect window (%s)", g_addrGameRomCamera.load(), isFovStall ? "250ms FOV stall fast" : "delayed 2.5s for load");
                    hunter_hasWritten = false;
                    hunter_mouseMovedSinceLastCheck = false;
                    hunter_lastCheckMs = GetTickCount64();
                    hunter_lastWrittenX = hunter_lastWrittenY = hunter_lastWrittenZ = 0.0f;
                    g_huntFramesLeft.store(0);
                    DisarmPageGuard();
                    EnterCriticalSection(&g_writerCS);
                    g_pendingRips.clear();
                    LeaveCriticalSection(&g_writerCS);
                    g_blacklistedMode.store(false);
                    g_lastBlacklistedWriteTime.store(0);
                    hunter_blacklistRecheckMs = GetTickCount64();
                    // FIX: mimic F2 exactly — Stop then Start, like SHOULD_CTRL 0->1 transition, not keep-alive
                    if (should_control) {
                        if (g_writerHuntActive.load()) StopWriterHunt();
                        StartWriterHunt();
                        hunter_hasWritten = false;
                        hunter_mouseMovedSinceLastCheck = false;
                        hunter_lastCheckMs = GetTickCount64();
                        g_huntFramesLeft.store(0);
                        if (gc_addr != 0) {
                            float gc_pos_x = ReadFloatBE(gc_addr + 0);
                            float gc_pos_y = ReadFloatBE(gc_addr + 4);
                            float gc_pos_z = ReadFloatBE(gc_addr + 8);
                            float gc_focus_x = ReadFloatBE(gc_addr + 0xC);
                            float gc_focus_y = ReadFloatBE(gc_addr + 0x10);
                            float gc_focus_z = ReadFloatBE(gc_addr + 0x14);
                            vcam_pos_x = gc_pos_x;
                            vcam_pos_y = gc_pos_y;
                            vcam_pos_z = gc_pos_z;
                            float d_x = gc_pos_x - gc_focus_x;
                            float d_y = gc_pos_y - gc_focus_y;
                            float d_z = gc_pos_z - gc_focus_z;
                            orbit_radius = sqrt(d_x*d_x + d_y*d_y + d_z*d_z);
                            orbit_angle = atan2(d_x, d_z);
                            orbit_pitch = asin(d_y / orbit_radius);
                            if (orbit_radius < 5.0f) orbit_radius = 20.0f;
                            current_orbit_angle = orbit_angle;
                            current_orbit_pitch = orbit_pitch;
                            virt_cam_initialized = true;
                            if (is_foreground) {
                                POINT center = GetCemuWindowCenter(hwndFg);
                                SetCursorPos(center.x, center.y);
                            }
                        } else {
                            virt_cam_initialized = false;
                        }
                    } else {
                        if (g_writerHuntActive.load()) StopWriterHunt();
                        hunter_hasWritten = false;
                        hunter_mouseMovedSinceLastCheck = false;
                        virt_cam_initialized = false;
                    }
                    if (should_control && gc_addr != 0 && g_addrGameRomCamera.load() != 0) {
                        uintptr_t baseForFov = g_addrGameRomCamera.load();
                        EnterCriticalSection(&g_fovHuntCS);
                        g_fovHuntRips.clear();
                        LeaveCriticalSection(&g_fovHuntCS);
                        g_fovHuntFramesLeft.store(90);
                        g_fovHuntActive.store(true);
                        SetFovHwBreakpoint(baseForFov + 0x68C);
                        last_gc_addr = gc_addr;
                    }
                    if (isFovStall) g_fovStallReenablePending.store(false);
                    if (g_fovStallReenablePending.load()) g_fovStallReenablePending.store(false);
                }
            }

            if (g_pSharedMemory) {
                g_pSharedMemory->m_cfgMagnesisEnabled = magnesis_mode;
            }

            // Diagnostic: hunter state every second when mousecam on — verbose switch only
            if (g_verboseCamDiag) {
                static uint64_t lastDbgMs = 0;
                uint64_t nowDbg = GetTickCount64();
                if (nowDbg - lastDbgMs >= 1000 && g_mousecamActive) {
                    lastDbgMs = nowDbg;
                    DllLog("[DIAG] should_ctrl %d blacklisted %d huntActive %d huntFrames %d hasWritten %d mouseMoved %d gc %llX", should_control, g_blacklistedMode.load(), g_writerHuntActive.load(), g_huntFramesLeft.load(), hunter_hasWritten, hunter_mouseMovedSinceLastCheck, gc_addr);
                }
            }

            if (gc_addr != 0) {
                g_liveCamPosX = ReadFloatBE(gc_addr + 0);
                g_liveCamPosY = ReadFloatBE(gc_addr + 4);
                g_liveCamPosZ = ReadFloatBE(gc_addr + 8);
                g_liveCamFocX = ReadFloatBE(gc_addr + 0xC);
                g_liveCamFocY = ReadFloatBE(gc_addr + 0x10);
                g_liveCamFocZ = ReadFloatBE(gc_addr + 0x14);
                g_liveCamFOV = ReadFloatBE(g_addrGameRomCamera + 0x68C); // new FOV for display (was gc+0x24 = 0x654)

                if (g_pSharedMemory) {
                    g_pSharedMemory->m_teleLiveCamPosX = g_liveCamPosX;
                    g_pSharedMemory->m_teleLiveCamPosY = g_liveCamPosY;
                    g_pSharedMemory->m_teleLiveCamPosZ = g_liveCamPosZ;
                    g_pSharedMemory->m_teleLiveCamFocX = g_liveCamFocX;
                    g_pSharedMemory->m_teleLiveCamFocY = g_liveCamFocY;
                    g_pSharedMemory->m_teleLiveCamFocZ = g_liveCamFocZ;
                    g_pSharedMemory->m_teleLiveCamFOV = g_liveCamFOV;
                }

                if (scanShortcutMenu && g_addrShortcutMenu != 0) {
                    g_liveShortcutMenu = ReadInt32BE(g_addrShortcutMenu + 128);
                } else {
                    g_liveShortcutMenu = -1;
                }
                if (g_pSharedMemory) {
                    g_pSharedMemory->m_teleLiveShortcutMenu = g_liveShortcutMenu;
                }

                float dx = 0.0f;
                float dy = 0.0f;

                if (g_blacklistedMode.load()) {
                    dx = 0.0f; dy = 0.0f;
                } else if (g_mousecamActive && is_foreground) {
                    POINT pt = {0, 0};
                    GetCursorPos(&pt);
                    POINT center = GetCemuWindowCenter(hwndFg);

                    dx = static_cast<float>(pt.x - center.x);
                    dy = static_cast<float>(pt.y - center.y);

                    if (dx != 0.0f || dy != 0.0f) {
                        SetCursorPos(center.x, center.y);
                    }
                }
                if ((dx != 0.0f || dy != 0.0f) && should_control && !g_blacklistedMode.load()) {
                    hunter_mouseMovedSinceLastCheck = true;
                }

                if (g_blacklistedMode.load()) {
                    // Blacklisted — mouse dead AND writes suppressed.
                    //
                    // This branch must be self-sufficient: the recovery machinery below lives inside
                    // `else if (g_mousecamActive)`, and a true blacklist skips that entire branch — so the
                    // old 100ms re-check and auto-clear here were dead code. Symptom: a blacklisted write
                    // caught during the death cinematic (no menu opens, so no StopWriterHunt anywhere) latched
                    // the mouse dead until F2. Level changes never showed it because the load menu opens and
                    // the should_control transition runs StopWriterHunt, which clears the flag.
                    uint64_t nowBl = GetTickCount64();
                    uint64_t blTimer = g_lastBlacklistedWriteTime.load();
                    if (blTimer == 0) {
                        // mode==true without a timestamp (scanner-thread reset raced ProcessPendingWriters)
                        DllLog("[INFO] Blacklist stale (timer=0) — releasing and re-hunting");
                        g_blacklistedMode.store(false);
                        hunter_hasWritten = false;
                        hunter_mouseMovedSinceLastCheck = false;
                        virt_cam_initialized = false; // re-capture orbit from the game camera on resume
                        g_huntFramesLeft.store(0);
                        DisarmPageGuard();
                        EnterCriticalSection(&g_writerCS);
                        g_pendingRips.clear();
                        LeaveCriticalSection(&g_writerCS);
                    } else if (nowBl - blTimer >= 2500) {
                        DllLog("[INFO] Blacklist held %llums with no fresh blacklisted write — releasing and re-hunting",
                               (unsigned long long)(nowBl - blTimer));
                        g_blacklistedMode.store(false);
                        g_lastBlacklistedWriteTime.store(0);
                        hunter_hasWritten = false;
                        hunter_mouseMovedSinceLastCheck = false;
                        hunter_lastCheckMs = nowBl;
                        hunter_blacklistRecheckMs = nowBl;
                        virt_cam_initialized = false; // re-capture orbit from the game camera on resume
                        g_huntFramesLeft.store(0);
                        DisarmPageGuard();
                        EnterCriticalSection(&g_writerCS);
                        g_pendingRips.clear();
                        LeaveCriticalSection(&g_writerCS);
                    } else if (g_writerHuntActive.load()) {
                        // Flawless suppression: re-check the current writers every 100ms. If they are no
                        // longer blacklisted (cutscene over), ProcessPendingWriters clears the flag itself.
                        if (nowBl >= hunter_blacklistRecheckMs && nowBl - hunter_blacklistRecheckMs >= 100) {
                            hunter_blacklistRecheckMs = nowBl;
                            uintptr_t baseBl = g_addrGameRomCamera.load();
                            if (baseBl != 0) {
                                g_huntFramesLeft.store(10);
                                ArmPageGuard(baseBl + 0x550);
                            }
                        }
                        if (g_huntFramesLeft.load() > 0) {
                            uintptr_t baseBl2 = g_addrGameRomCamera.load();
                            if (baseBl2 != 0) ArmPageGuard(baseBl2 + 0x550);
                            int prevBl = g_huntFramesLeft.fetch_sub(1);
                            if (prevBl == 1) ProcessPendingWriters();
                        }
                    }
                } else if (g_mousecamActive) {
                        float sensitivity_x = 1.0f;
                    float sensitivity_y = 1.0f;
                    if (g_pSharedMemory) {
                        sensitivity_x = g_pSharedMemory->m_cfgSensitivityX;
                        if (g_pSharedMemory->m_cfgUseIndependentSens) {
                            sensitivity_y = g_pSharedMemory->m_cfgSensitivityY;
                        } else {
                            sensitivity_y = g_pSharedMemory->m_cfgSensitivityX;
                        }
                    }
                    float sens_x = 0.001f * (sensitivity_x <= 0.0f ? 1.0f : sensitivity_x);
                    float sens_y = 0.001f * (sensitivity_y <= 0.0f ? 1.0f : sensitivity_y);

                    if (ki_enabled && g_pSharedMemory) {
                        if (is_foreground) {
                            for (int i = 0; i < 5; ++i) {
                                uint16_t keycode = g_pSharedMemory->m_cfgMouseBindingKeys[i];
                                if (keycode == 0) continue;

                                bool down = (GetAsyncKeyState(MouseVk(i)) & 0x8000) != 0;
                                if (down && !prev_pressed[i]) {
                                    CemuKeyInjector_SendKey(keycode, false);
                                    prev_pressed[i] = true;
                                    press_time[i] = std::chrono::steady_clock::now();
                                    press_time_valid[i] = true;
                                } else if (!down && prev_pressed[i]) {
                                    bool can_release = true;
                                    if (press_time_valid[i]) {
                                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - press_time[i]
                                        ).count();
                                        // Minimum 100 ms hold to prevent key bounce — Cemu's
                                        // input polling runs at ~10 ms so this guarantees the
                                        // press is registered before we release.
                                        if (elapsed < 100) can_release = false;
                                    }
                                    if (can_release) {
                                        CemuKeyInjector_SendKey(keycode, true);
                                        prev_pressed[i] = false;
                                        press_time_valid[i] = false;
                                    }
                                }
                            }
                        } else {
                            for (int i = 0; i < 5; ++i) {
                                if (prev_pressed[i]) {
                                    uint16_t key = g_pSharedMemory ? g_pSharedMemory->m_cfgMouseBindingKeys[i] : 0;
                                    if (key != 0) CemuKeyInjector_SendKey(key, true);
                                    prev_pressed[i] = false;
                                    press_time_valid[i] = false;
                                }
                            }
                        }
                    }

                    uintptr_t magne_ideal_base = g_magneIdealBase;

                    if (magnesis_mode && magne_ideal_base != 0) {
                        scroll_accumulator = 0;
                        float link_x = ReadFloatBE(g_addrGameRomCamera + 0x7D4);
                        float link_y = ReadFloatBE(g_addrGameRomCamera + 0x7D8);
                        float link_z = ReadFloatBE(g_addrGameRomCamera + 0x7DC);

                        if (!magne_initialized) {
                            float cur_x = ReadFloatBE(magne_ideal_base);
                            float cur_y = ReadFloatBE(magne_ideal_base + 4);
                            float cur_z = ReadFloatBE(magne_ideal_base + 8);
                            magne_off_x = cur_x - link_x;
                            magne_off_y = cur_y - link_y;
                            magne_off_z = cur_z - link_z;
                            magne_initialized = true;
                        }

                        float magne_sens_h = g_pSharedMemory ? g_pSharedMemory->m_cfgMagneSens : 1.0f;
                        bool indep_magne_sens = g_pSharedMemory ? g_pSharedMemory->m_cfgUseIndependentMagneSens : false;

                        float d_theta = dx * sens_x * magne_sens_h * 5.0f;

                        float dy_world = 0.0f;
                        if (indep_magne_sens) {
                            float magne_sens_v = g_pSharedMemory ? g_pSharedMemory->m_cfgMagneSensY : 1.0f;
                            dy_world = -dy * sens_y * magne_sens_v * 42.5f;
                        } else {
                            // Dynamic 1:1 Auto-Match: scale vertical speed with distance R
                            // Arc length S_H = R * d_theta = R * (dx * sens_x * magne_sens_h * 5.0)
                            // Matching dy_world = dy * sens_y * magne_sens_h * (5.0 * R)
                            float h_dist = sqrt(magne_off_x * magne_off_x + magne_off_z * magne_off_z);
                            if (h_dist < 1.0f) h_dist = 1.0f;
                            dy_world = -dy * sens_y * magne_sens_h * (5.0f * h_dist);
                        }

                        // Apply magnesis speed & distance limits based on configuration
                        uint8_t speedMode = g_pSharedMemory ? g_pSharedMemory->m_cfgMagnesisSpeedMode : 2;
                        float maxAngularSpeedH = 0.0f; // 0 = unlimited
                        float maxSpeedV = 0.0f;
                        float v_clamp = 999999.0f;     // 999999 = unlimited height
                        float h_clamp_max = 999999.0f; // 999999 = unlimited radius
                        const float PI = 3.14159265f;

                        if (speedMode == 0) {
                            maxAngularSpeedH = (2.5f * PI) / 7.0f; // 1.25x Vanilla
                            maxSpeedV = 18.75f;
                            v_clamp = 15.0f;
                            h_clamp_max = 22.0f;
                        } else if (speedMode == 1) {
                            maxAngularSpeedH = (6.25f * PI) / 7.0f; // 2.5x Vanilla
                            maxSpeedV = 46.875f;
                            v_clamp = 30.0f;
                            h_clamp_max = 50.0f;
                        }

                        // Accumulate dt for speed clamping to match input frequency
                        float time_delta = dt;
                        if (dx != 0.0f || dy != 0.0f) {
                            time_delta += magne_accum_dt;
                            magne_accum_dt = 0.0f;
                        } else {
                            magne_accum_dt += dt;
                        }
                        if (time_delta > 0.1f) time_delta = 0.1f;

                        if (maxAngularSpeedH > 0.0f && time_delta > 0.0f) {
                            float max_d_theta = maxAngularSpeedH * time_delta;
                            if (fabs(d_theta) > max_d_theta) {
                                d_theta = (d_theta > 0.0f ? max_d_theta : -max_d_theta);
                            }
                        }

                        if (maxSpeedV > 0.0f && time_delta > 0.0f) {
                            float max_dy_world = maxSpeedV * time_delta;
                            if (fabs(dy_world) > max_dy_world) {
                                dy_world = (dy_world > 0.0f ? max_dy_world : -max_dy_world);
                            }
                        }

                        if (d_theta != 0.0f || dy_world != 0.0f) {
                            float cos_t = cos(d_theta);
                            float sin_t = sin(d_theta);
                            float new_off_x = magne_off_x * cos_t - magne_off_z * sin_t;
                            float new_off_z = magne_off_x * sin_t + magne_off_z * cos_t;
                            magne_off_y += dy_world;
                            magne_off_x = new_off_x;
                            magne_off_z = new_off_z;
                        }

                        if (magne_off_y < -v_clamp) magne_off_y = -v_clamp;
                        if (magne_off_y > v_clamp) magne_off_y = v_clamp;

                        int scroll = g_scrollDelta.exchange(0);
                        if (scroll != 0) {
                            float pull_sens = g_pSharedMemory ? g_pSharedMemory->m_cfgMagnePullSens : 1.0f;
                            if (fps_magne_active) {
                                float r_3d = sqrt(magne_off_x * magne_off_x + magne_off_y * magne_off_y + magne_off_z * magne_off_z);
                                if (r_3d > 0.01f) {
                                    float new_r_3d = r_3d + (static_cast<float>(scroll) * 0.5f / 120.0f) * pull_sens;
                                    if (new_r_3d < 1.0f) new_r_3d = 1.0f;
                                    if (new_r_3d > h_clamp_max) new_r_3d = h_clamp_max;
                                    float scale = new_r_3d / r_3d;
                                    magne_off_x *= scale;
                                    magne_off_y *= scale;
                                    magne_off_z *= scale;
                                }
                            } else {
                                float h_dist = sqrt(magne_off_x * magne_off_x + magne_off_z * magne_off_z);
                                if (h_dist > 0.01f) {
                                    float new_h_dist = h_dist + (static_cast<float>(scroll) * 0.5f / 120.0f) * pull_sens;
                                    if (new_h_dist < 2.0f) new_h_dist = 2.0f;
                                    if (new_h_dist > h_clamp_max) new_h_dist = h_clamp_max;
                                    float scale = new_h_dist / h_dist;
                                    magne_off_x *= scale;
                                    magne_off_z *= scale;
                                }
                            }
                        }

                        if (magne_off_y < -v_clamp) magne_off_y = -v_clamp;
                        if (magne_off_y > v_clamp) magne_off_y = v_clamp;

                        float magne_pos_x = link_x + magne_off_x;
                        float magne_pos_y = link_y + magne_off_y;
                        float magne_pos_z = link_z + magne_off_z;

                        WriteFloatBE(magne_ideal_base, magne_pos_x);
                        WriteFloatBE(magne_ideal_base + 4, magne_pos_y);
                        WriteFloatBE(magne_ideal_base + 8, magne_pos_z);
                    } else {
                        if (!magnesis_mode) {
                            bool is_main_menu_open = menu_active && !is_shortcut_open;

                            if (is_main_menu_open) {
                                mouse_menu_accum_x += dx;
                                mouse_menu_accum_y += dy;

                                float threshold = 120.0f;
                                uint16_t dpad_left_key = g_pSharedMemory ? g_pSharedMemory->m_cfgDpadLeftKey : 0;
                                uint16_t dpad_right_key = g_pSharedMemory ? g_pSharedMemory->m_cfgDpadRightKey : 0;
                                uint16_t dpad_up_key = g_pSharedMemory ? g_pSharedMemory->m_cfgDpadUpKey : 0;
                                uint16_t dpad_down_key = g_pSharedMemory ? g_pSharedMemory->m_cfgDpadDownKey : 0;

                                if (fabs(mouse_menu_accum_x) >= threshold) {
                                    int notches = static_cast<int>(fabs(mouse_menu_accum_x) / threshold);
                                    float sign = mouse_menu_accum_x > 0.0f ? 1.0f : -1.0f;
                                    mouse_menu_accum_x -= sign * notches * threshold;

                                    uint16_t key = sign > 0.0f ? dpad_right_key : dpad_left_key;
                                    if (key != 0) {
                                        for (int n = 0; n < notches; ++n) {
                                            CemuKeyInjector_SendKey(key, false);
                                            Sleep(15);
                                            CemuKeyInjector_SendKey(key, true);
                                            Sleep(15);
                                        }
                                    }
                                }

                                if (fabs(mouse_menu_accum_y) >= threshold) {
                                    int notches = static_cast<int>(fabs(mouse_menu_accum_y) / threshold);
                                    float sign = mouse_menu_accum_y > 0.0f ? 1.0f : -1.0f;
                                    mouse_menu_accum_y -= sign * notches * threshold;

                                    uint16_t key = sign > 0.0f ? dpad_down_key : dpad_up_key;
                                    if (key != 0) {
                                        for (int n = 0; n < notches; ++n) {
                                            CemuKeyInjector_SendKey(key, false);
                                            Sleep(15);
                                            CemuKeyInjector_SendKey(key, true);
                                            Sleep(15);
                                        }
                                    }
                                }
                            } else {
                                mouse_menu_accum_x = 0.0f;
                                mouse_menu_accum_y = 0.0f;
                                orbit_angle -= dx * sens_x;
                                orbit_pitch += dy * sens_y * 0.85f;
                            }

                            bool scroll_helper = g_pSharedMemory ? g_pSharedMemory->m_cfgScrollHelper : false;
                            if (scroll_helper && g_mousecamActive && is_foreground) {
                                int scroll = g_scrollDelta.exchange(0);
                                uint16_t dpad_left_key = g_pSharedMemory ? g_pSharedMemory->m_cfgDpadLeftKey : 0;
                                uint16_t dpad_right_key = g_pSharedMemory ? g_pSharedMemory->m_cfgDpadRightKey : 0;
                                uint16_t rstick_left_key = g_pSharedMemory ? g_pSharedMemory->m_cfgRstickLeftKey : 0;
                                uint16_t rstick_right_key = g_pSharedMemory ? g_pSharedMemory->m_cfgRstickRightKey : 0;

                                bool is_shortcut_in_game = (scanShortcutMenu && g_addrShortcutMenu != 0 && ReadInt32BE(g_addrShortcutMenu + 128) != -1);
                                bool is_any_menu_active = is_main_menu_open || is_shortcut_in_game || (active_menu != ScrollMenuType::None);

                                if (is_any_menu_active) {
                                    if (scroll != 0) {
                                        scroll_accumulator += scroll;
                                    }
                                    if (abs(scroll_accumulator) >= 120) {
                                        int notches = abs(scroll_accumulator) / 120;
                                        int sign = scroll_accumulator > 0 ? 1 : -1;
                                        scroll_accumulator -= sign * notches * 120;

                                        uint16_t target_key = (sign > 0) ? rstick_right_key : rstick_left_key;
                                        if (target_key != 0) {
                                            for (int n = 0; n < notches; ++n) {
                                                CemuKeyInjector_SendKey(target_key, false);
                                                Sleep(15);
                                                CemuKeyInjector_SendKey(target_key, true);
                                                Sleep(15);
                                            }
                                        }
                                        menu_hold_timer = std::chrono::steady_clock::now();
                                    }

                                    // Release D-Pad key and clear active_menu state only after scroll inactivity timeout
                                    if (active_menu != ScrollMenuType::None) {
                                        auto elapsed_sec = std::chrono::duration<float>(std::chrono::steady_clock::now() - menu_hold_timer).count();
                                        if (elapsed_sec > 0.6f) {
                                            if (active_menu == ScrollMenuType::Left && dpad_left_key != 0) {
                                                CemuKeyInjector_SendKey(dpad_left_key, true);
                                            } else if (active_menu == ScrollMenuType::Right && dpad_right_key != 0) {
                                                CemuKeyInjector_SendKey(dpad_right_key, true);
                                            }
                                            active_menu = ScrollMenuType::None;
                                            scroll_accumulator = 0;
                                        }
                                    }
                                } else {
                                    if (scroll != 0) {
                                        scroll_accumulator += scroll;
                                    }

                                    if (abs(scroll_accumulator) >= 120) {
                                        int notches = abs(scroll_accumulator) / 120;
                                        int sign = scroll_accumulator > 0 ? 1 : -1;
                                        scroll_accumulator -= sign * notches * 120;

                                        active_menu = (sign > 0) ? ScrollMenuType::Right : ScrollMenuType::Left;
                                        menu_hold_timer = std::chrono::steady_clock::now();

                                        uint16_t dpad = (sign > 0) ? dpad_right_key : dpad_left_key;
                                        if (dpad != 0) { CemuKeyInjector_SendKey(dpad, false); }

                                        uint16_t rstick = (sign > 0) ? rstick_right_key : rstick_left_key;
                                        if (rstick != 0) {
                                            for (int n = 0; n < notches; ++n) {
                                                CemuKeyInjector_SendKey(rstick, false);
                                                Sleep(15);
                                                CemuKeyInjector_SendKey(rstick, true);
                                                Sleep(15);
                                            }
                                        }
                                    }
                                }
                            } else {
                                g_scrollDelta.store(0);
                                scroll_accumulator = 0;
                                if (active_menu != ScrollMenuType::None) {
                                    uint16_t dpad_left_key = g_pSharedMemory ? g_pSharedMemory->m_cfgDpadLeftKey : 0;
                                    uint16_t dpad_right_key = g_pSharedMemory ? g_pSharedMemory->m_cfgDpadRightKey : 0;
                                    if (dpad_left_key != 0) { CemuKeyInjector_SendKey(dpad_left_key, true); }
                                    if (dpad_right_key != 0) { CemuKeyInjector_SendKey(dpad_right_key, true); }
                                    active_menu = ScrollMenuType::None;
                                }
                            }
                        }
                    }

                    const float MAX_PITCH_RAD = 1.4835299f; // 85.0 degrees (170 degrees total range)
                    orbit_pitch = (std::max)(-MAX_PITCH_RAD, (std::min)(MAX_PITCH_RAD, orbit_pitch));

                    if (!virt_cam_initialized) {
                        vcam_pos_x = ReadFloatBE(gc_addr + 0);
                        vcam_pos_y = ReadFloatBE(gc_addr + 4);
                        vcam_pos_z = ReadFloatBE(gc_addr + 8);

                        float ideal_x = ReadFloatBE(g_addrGameRomCamera + 0x594);
                        float ideal_y = ReadFloatBE(g_addrGameRomCamera + 0x598);
                        float ideal_z = ReadFloatBE(g_addrGameRomCamera + 0x590);

                        float gc_focus_x = ReadFloatBE(gc_addr + 0xC);
                        float gc_focus_y = ReadFloatBE(gc_addr + 0x10);
                        float gc_focus_z = ReadFloatBE(gc_addr + 0x14);

                        float d_x = ideal_x - gc_focus_x;
                        float d_y = ideal_y - gc_focus_y;
                        float d_z = ideal_z - gc_focus_z;

                        orbit_radius = sqrt(d_x * d_x + d_y * d_y + d_z * d_z);
                        orbit_angle = atan2(d_x, d_z);
                        orbit_pitch = asin(d_y / orbit_radius);

                        if (orbit_radius < 5.0f) {
                            orbit_radius = 20.0f;
                        }
                        current_orbit_angle = orbit_angle;
                        current_orbit_pitch = orbit_pitch;
                        virt_cam_initialized = true;
                    }

                    float pivot_x = 0.0f, pivot_y = 0.0f, pivot_z = 0.0f;
                    float raw_pivot_x = ReadFloatBE(g_addrGameRomCamera + 0x674);
                    float raw_pivot_y = ReadFloatBE(g_addrGameRomCamera + 0x678);
                    float raw_pivot_z = ReadFloatBE(g_addrGameRomCamera + 0x67C);

                    if (magnesis_mode && magne_ideal_base != 0) {
                        float mag_x = ReadFloatBE(magne_ideal_base);
                        float mag_y = ReadFloatBE(magne_ideal_base + 4);
                        float mag_z = ReadFloatBE(magne_ideal_base + 8);
                        if (mag_x != 0.0f || mag_y != 0.0f || mag_z != 0.0f) {
                            float link_x = ReadFloatBE(g_addrGameRomCamera + 0x7D4);
                            float link_y = ReadFloatBE(g_addrGameRomCamera + 0x7D8);
                            float link_z = ReadFloatBE(g_addrGameRomCamera + 0x7DC);
                            float dist = sqrtf((mag_x - link_x) * (mag_x - link_x) + 
                                               (mag_y - link_y) * (mag_y - link_y) + 
                                               (mag_z - link_z) * (mag_z - link_z));
                            if (dist < 100.0f && dist > 0.1f) {
                                pivot_x = mag_x;
                                pivot_y = mag_y;
                                pivot_z = mag_z;
                            } else {
                                pivot_x = raw_pivot_x;
                                pivot_y = raw_pivot_y;
                                pivot_z = raw_pivot_z;
                            }
                        } else {
                            pivot_x = raw_pivot_x;
                            pivot_y = raw_pivot_y;
                            pivot_z = raw_pivot_z;
                        }
                    } else {
                        pivot_x = raw_pivot_x;
                        pivot_y = raw_pivot_y;
                        pivot_z = raw_pivot_z;
                    }

                    float mouse_factor = 1.0f - exp(-45.0f * dt);
                    current_orbit_angle += (orbit_angle - current_orbit_angle) * mouse_factor;
                    current_orbit_pitch += (orbit_pitch - current_orbit_pitch) * mouse_factor;
                    current_orbit_pitch = (std::max)(-MAX_PITCH_RAD, (std::min)(MAX_PITCH_RAD, current_orbit_pitch));

                    float current_radius = 5.5f;
                    bool full_orbit = (g_pSharedMemory && g_pSharedMemory->m_cfgFullOrbitCamera);
                    if (!full_orbit && current_orbit_pitch > 0.18f) {
                        current_radius += (current_orbit_pitch - 0.18f) * 5.5f;
                    }

                    float horizontal_r = current_radius * cos(current_orbit_pitch);
                    vcam_pos_x = pivot_x + horizontal_r * sin(current_orbit_angle);
                    vcam_pos_y = pivot_y + current_radius * sin(current_orbit_pitch);
                    vcam_pos_z = pivot_z + horizontal_r * cos(current_orbit_angle);

                    if (fps_magne_active && magne_ideal_base != 0) {
                        float eye_height = g_pSharedMemory ? g_pSharedMemory->m_cfgFpsMagneEyeHeight : 0.5f;
                        float fwd_offset = g_pSharedMemory ? g_pSharedMemory->m_cfgFpsMagneOffsetForward : 0.0f;
                        float side_offset = g_pSharedMemory ? g_pSharedMemory->m_cfgFpsMagneOffsetSide : 0.0f;

                        float link_x = ReadFloatBE(g_addrGameRomCamera + 0x7D4);
                        float link_y = ReadFloatBE(g_addrGameRomCamera + 0x7D8);
                        float link_z = ReadFloatBE(g_addrGameRomCamera + 0x7DC);
                        if (link_x == 0.0f && link_y == 0.0f && link_z == 0.0f) {
                            link_x = raw_pivot_x; link_y = raw_pivot_y; link_z = raw_pivot_z;
                        }

                        vcam_pos_x = link_x + side_offset;
                        vcam_pos_y = link_y + eye_height;
                        vcam_pos_z = link_z + fwd_offset;

                        float focus_x = ReadFloatBE(magne_ideal_base);
                        float focus_y = ReadFloatBE(magne_ideal_base + 4);
                        float focus_z = ReadFloatBE(magne_ideal_base + 8);

                        WriteFloatBE(g_addrGameRomCamera + 0x55C, focus_x);
                        WriteFloatBE(g_addrGameRomCamera + 0x560, focus_y);
                        WriteFloatBE(g_addrGameRomCamera + 0x564, focus_z);
                    }

                    if (g_pSharedMemory) {
                        g_pSharedMemory->m_telePivotX = pivot_x;
                        g_pSharedMemory->m_telePivotY = pivot_y;
                        g_pSharedMemory->m_telePivotZ = pivot_z;
                        
                        if (magnesis_auto_active && magne_ideal_base != 0) {
                            g_pSharedMemory->m_teleMagneTargetX = ReadFloatBE(magne_ideal_base);
                            g_pSharedMemory->m_teleMagneTargetY = ReadFloatBE(magne_ideal_base + 4);
                            g_pSharedMemory->m_teleMagneTargetZ = ReadFloatBE(magne_ideal_base + 8);
                        } else {
                            g_pSharedMemory->m_teleMagneTargetX = 0.0f;
                            g_pSharedMemory->m_teleMagneTargetY = 0.0f;
                            g_pSharedMemory->m_teleMagneTargetZ = 0.0f;
                        }
                    }

                    // Blacklist auto-recovery lives in the g_blacklistedMode branch near the dx/dy
                    // capture — it cannot run here, because reaching this line already proves the
                    // blacklist is clear (blacklisted frames skip the whole else-if below).

                    if ((!magnesis_mode || fps_magne_active) && !menu_active) {
                        // 250ms overwrite check — FIX wrap guard
                        uint64_t nowMs = GetTickCount64();
                        bool shouldCheck = hunter_hasWritten && g_writerHuntActive.load() && nowMs >= hunter_lastCheckMs && (nowMs - hunter_lastCheckMs >= 250);
                        if (shouldCheck) {
                            hunter_lastCheckMs = nowMs;
                            if (hunter_mouseMovedSinceLastCheck) {
                                float curX = ReadFloatBE(g_addrGameRomCamera + 0x550);
                                float curY = ReadFloatBE(g_addrGameRomCamera + 0x554);
                                float curZ = ReadFloatBE(g_addrGameRomCamera + 0x558);
                                if (curX != hunter_lastWrittenX || curY != hunter_lastWrittenY || curZ != hunter_lastWrittenZ) {
                                    if (g_verboseCamDiag) {
                                        DllLog("[DIAG] Overwrite detected (wrote [%.2f,%.2f,%.2f] -> cur [%.2f,%.2f,%.2f]) — arming guard 10 frames", hunter_lastWrittenX, hunter_lastWrittenY, hunter_lastWrittenZ, curX, curY, curZ);
                                    }
                                    g_huntFramesLeft.store(10); // 10 frames
                                    if (g_addrGameRomCamera.load() != 0) ArmPageGuard(g_addrGameRomCamera.load() + 0x550);
                                }
                                hunter_mouseMovedSinceLastCheck = false;
                            }
                        }
                        // Reaching here proves the blacklist is clear — the blacklisted state is handled
                        // (suppression + recovery) in its own branch above the dx/dy capture.
                        WriteFloatBE(g_addrGameRomCamera + 0x550, vcam_pos_x);
                        WriteFloatBE(g_addrGameRomCamera + 0x554, vcam_pos_y);
                        WriteFloatBE(g_addrGameRomCamera + 0x558, vcam_pos_z);
                        hunter_lastWrittenX = vcam_pos_x;
                        hunter_lastWrittenY = vcam_pos_y;
                        hunter_lastWrittenZ = vcam_pos_z;
                        hunter_hasWritten = true;
                        if (g_writerHuntActive.load() && g_huntFramesLeft.load() > 0) {
                            if (g_addrGameRomCamera.load() != 0) ArmPageGuard(g_addrGameRomCamera.load() + 0x550);
                            int prev = g_huntFramesLeft.fetch_sub(1);
                            if (prev == 1) ProcessPendingWriters();
                        }
                    } else {
                        if (g_huntFramesLeft.load() > 0) {
                            int prev = g_huntFramesLeft.fetch_sub(1);
                            if (g_addrGameRomCamera.load() != 0 && g_writerHuntActive.load()) ArmPageGuard(g_addrGameRomCamera.load() + 0x550);
                            if (prev == 1) ProcessPendingWriters();
                        } else {
                            hunter_hasWritten = false;
                        }
                    }
                }
            }

#ifdef _DEBUG
            if (g_pSharedMemory && g_pSharedMemory->m_reqDumpAob) {
                g_pSharedMemory->m_reqDumpAob = false;
                if (g_mousecamActive && !magnesis_auto_active) {
                    if (!g_discoveredWriters.empty()) {
                        EnterCriticalSection(&g_writerCS);
                        uintptr_t minRip = UINTPTR_MAX, maxRip = 0;
                        size_t maxPatch = 0;
                        for (auto& wr : g_discoveredWriters) {
                            if (wr.rip < minRip) minRip = wr.rip;
                            if (wr.rip > maxRip) { maxRip = wr.rip; maxPatch = wr.patchSize; }
                        }
                        uintptr_t startDump = minRip >= 32 ? minRip - 32 : 0;
                        uintptr_t endDump = maxRip + maxPatch + 32;
                        FILE* f = nullptr;
                        if (_wfopen_s(&f, L"cemu_aob_dump.txt", L"a") == 0) {
                            fwprintf(f, L"AOB Dump from %llX to %llX (%zu writers)\n", startDump, endDump, g_discoveredWriters.size());
                            for (uintptr_t ptr = startDump; ptr < endDump; ++ptr) {
                                bool isStart = false, isEnd = false;
                                uint8_t b = 0;
                                SafeReadU8_SEH(ptr, b);
                                for (auto& wr : g_discoveredWriters) {
                                    if (wr.rip == ptr) isStart = true;
                                    if (wr.rip + wr.patchSize - 1 == ptr) isEnd = true;
                                    if (ptr >= wr.rip && ptr < wr.rip + wr.patchSize) { b = wr.origBytes[ptr - wr.rip]; }
                                }
                                if (isStart) fprintf(f, "[ ");
                                fprintf(f, "%02X", b);
                                if (isEnd) fprintf(f, " ] ");
                                else fprintf(f, " ");
                                if ((ptr - startDump + 1) % 16 == 0) fprintf(f, "\n");
                            }
                            fprintf(f, "\n");
                            fclose(f);
                            DllLog("[INFO] AOB dump written to cemu_aob_dump.txt (%zu writers immediate)", g_discoveredWriters.size());
                        }
                        LeaveCriticalSection(&g_writerCS);
                    } else {
                        DllLog("[INFO] No writers yet — hunting 500ms then dumping");
                        hunter_aobDumpCountdown = 125;
                        g_huntFramesLeft.store(125);
                        if (g_addrGameRomCamera.load() != 0) ArmPageGuard(g_addrGameRomCamera.load() + 0x550);
                    }
                } else {
                    DllLog("[WARNING] F5 dump ignored — mousecam must be ON and magnesis OFF");
                }
            }
            if (hunter_aobDumpCountdown > 0) {
                hunter_aobDumpCountdown--;
                if (hunter_aobDumpCountdown == 0) {
                    EnterCriticalSection(&g_writerCS);
                    if (!g_discoveredWriters.empty()) {
                        uintptr_t minRip = UINTPTR_MAX, maxRip = 0;
                        size_t maxPatch = 0;
                        for (auto& wr : g_discoveredWriters) {
                            if (wr.rip < minRip) minRip = wr.rip;
                            if (wr.rip > maxRip) { maxRip = wr.rip; maxPatch = wr.patchSize; }
                        }
                        uintptr_t startDump = minRip >= 32 ? minRip - 32 : 0;
                        uintptr_t endDump = maxRip + maxPatch + 32;
                        FILE* f = nullptr;
                        if (_wfopen_s(&f, L"cemu_aob_dump.txt", L"a") == 0) {
                            fwprintf(f, L"AOB Dump (delayed) from %llX to %llX (%zu writers)\n", startDump, endDump, g_discoveredWriters.size());
                            for (uintptr_t ptr = startDump; ptr < endDump; ++ptr) {
                                bool isStart = false, isEnd = false;
                                uint8_t b = 0;
                                SafeReadU8_SEH(ptr, b);
                                for (auto& wr : g_discoveredWriters) {
                                    if (wr.rip == ptr) isStart = true;
                                    if (wr.rip + wr.patchSize - 1 == ptr) isEnd = true;
                                    if (ptr >= wr.rip && ptr < wr.rip + wr.patchSize) { b = wr.origBytes[ptr - wr.rip]; }
                                }
                                if (isStart) fprintf(f, "[ ");
                                fprintf(f, "%02X", b);
                                if (isEnd) fprintf(f, " ] ");
                                else fprintf(f, " ");
                                if ((ptr - startDump + 1) % 16 == 0) fprintf(f, "\n");
                            }
                            fprintf(f, "\n");
                            fclose(f);
                            DllLog("[INFO] Delayed AOB dump written (%zu writers)", g_discoveredWriters.size());
                        }
                    } else {
                        DllLog("[INFO] Delayed AOB dump: no writers caught in 500ms");
                    }
                    LeaveCriticalSection(&g_writerCS);
                }
            }
#endif

            Sleep(4);
        }

        SetGlobalCursorVisibility(true);
        StopMouseHook();

        for (int i = 0; i < 5; ++i) {
            if (prev_pressed[i]) {
                uint16_t key = g_pSharedMemory ? g_pSharedMemory->m_cfgMouseBindingKeys[i] : 0;
                if (key != 0) CemuKeyInjector_SendKey(key, true);
                prev_pressed[i] = false;
                press_time_valid[i] = false;
            }
        }

        timeEndPeriod(1);
        OnThreadExit();
        return 0;
    }

    void Init(HMODULE hModule) {
        g_hModule = hModule;
        InitializeCriticalSection(&g_patchCS);
        InitializeCriticalSection(&g_writerCS);
        InitializeCriticalSection(&g_menuCandidateCS);
        InitializeCriticalSection(&g_fovHuntCS);
        g_fovHuntCSInit = true;

        if (g_sharedMemory.Create(L"Local\\BotwMousecamSharedMemory")) {
            if (g_pSharedMemory) {
                g_pSharedMemory->m_dllBaseAddr = (uint64_t)hModule;
                g_pSharedMemory->m_statusAddrGameRomCamera = 0;
                g_pSharedMemory->m_statusAddrMagneTarget = 0;
                g_pSharedMemory->m_statusAddrShortcutMenu = 0;
                g_pSharedMemory->m_statusAddrMenuState = 0;
                g_pSharedMemory->m_statusScanning = false;
                g_pSharedMemory->m_statusWritersFound = 0;
                g_pSharedMemory->m_patchMagneDetourActive = false;
                g_pSharedMemory->m_statusMousecamActive = false;
                g_pSharedMemory->m_logWriteIdx = 0;
                memset(g_pSharedMemory->m_logQueue, 0, sizeof(g_pSharedMemory->m_logQueue));
                g_pSharedMemory->m_statusShutdownDone = false;
                g_pSharedMemory->m_reqToggleMousecam = false;
                g_pSharedMemory->m_reqResetScan = false;
                g_pSharedMemory->m_reqResetPreserveMenu = false;
                g_pSharedMemory->m_reqDumpAob = false;
                g_pSharedMemory->m_reqShutdown = false;
            }
        }

        // Install VEH for FOV + hunter page guards (level-change + writer hunt)
        if (!g_vehHandle) g_vehHandle = AddVectoredExceptionHandler(1, CameraWriterVehHandler);

        // Start scanning thread
        g_scanning = true;
        g_runningThreads++;
        g_hScanThread = CreateThread(nullptr, 0, ScanAobThread, nullptr, 0, nullptr);

        // Start camera control thread
        g_cameraControlRunning = true;
        g_runningThreads++;
        g_hCameraControlThread = CreateThread(nullptr, 0, CameraControlThread, nullptr, 0, nullptr);
    }

    void Shutdown() {
        g_scanning = false;
        g_cameraControlRunning = false;

        DWORD currentThreadId = GetCurrentThreadId();

        if (g_hScanThread && GetThreadId(g_hScanThread) != currentThreadId) {
            WaitForSingleObject(g_hScanThread, 1000);
            CloseHandle(g_hScanThread);
            g_hScanThread = nullptr;
        }
        if (g_hCameraControlThread && GetThreadId(g_hCameraControlThread) != currentThreadId) {
            WaitForSingleObject(g_hCameraControlThread, 1000);
            CloseHandle(g_hCameraControlThread);
            g_hCameraControlThread = nullptr;
        }

        RemoveShortcutHook();
        RemoveMenuStateHooks();

        RestoreAllPatches();

        g_writerHuntActive.store(false);
        g_huntFramesLeft.store(0);
        DisarmPageGuard();
        RemoveFovHook();
        DisarmFovGuard();
        g_fovWriteCount.store(0);
        g_lastFovWriteTick.store(0);
        EnterCriticalSection(&g_writerCS);
        for (auto& wr : g_discoveredWriters) RestoreInstruction(wr);
        g_discoveredWriters.clear();
        g_pendingRips.clear();
        g_blacklistedMode.store(false);
        g_lastBlacklistedWriteTime.store(0);
        if (g_pSharedMemory) g_pSharedMemory->m_statusWritersFound = 0;
        LeaveCriticalSection(&g_writerCS);
        if (g_vehHandle) { RemoveVectoredExceptionHandler(g_vehHandle); g_vehHandle = nullptr; }

        DeleteCriticalSection(&g_patchCS);
        DeleteCriticalSection(&g_writerCS);
        DeleteCriticalSection(&g_menuCandidateCS);
        if (g_fovHuntCSInit) DeleteCriticalSection(&g_fovHuntCS);

        g_sharedMemory.Close();
    }

} // namespace Mod
