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
// Three subsystems fundamentally require in-process execution:
//
// 1. VEH (Vectored Exception Handler) — The dynamic writer detection works by
//    arming PAGE_GUARD on camera memory and catching STATUS_GUARD_PAGE_VIOLATION.
//    VEH handlers must run in the faulting process; you cannot catch another
//    process's exceptions. This forces the writer-hunting system into the DLL.
//
// 2. Low-level mouse hook (SetWindowsHookExW / WH_MOUSE_LL) — Capturing scroll
//    events reliably requires a hook in the same desktop thread as the game.
//    A cross-process approach would miss events or add unpredictable latency.
//
// 3. Camera update latency — The camera control loop runs at ~200 Hz. Marshaling
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
        if (!g_pSharedMemory) return;

        char msg[128] = {};
        va_list args;
        va_start(args, format);
        vsnprintf(msg, sizeof(msg), format, args);
        va_end(args);

        uint32_t idx = g_pSharedMemory->m_logWriteIdx % 8;
        memcpy(g_pSharedMemory->m_logQueue[idx], msg, 128);
        g_pSharedMemory->m_logWriteIdx++;
    }

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

    static int32_t ReadInt32BE(uintptr_t address);
    static std::atomic<uint64_t> g_lastBlacklistedWriteTime = 0;

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

        // FOV at gc_addr + 0x24 = base + 0x630 + 0x24 = base + 0x654
        float fov = ReadFloatBE(base + 0x654);
        if (std::isnan(fov) || std::isinf(fov) || fov <= 0.05f || fov > 3.5f) {
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

        score = 10;
        if (fov >= 0.4f && fov <= 1.8f) score += 20;
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
    static std::atomic<bool> g_preserveMenuStateOnReset{false};

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

#ifdef _DEBUG
                            DllLog("[INFO] MenuState hook %u fired at 0x%llX (val: %d, Pattern 6E valid). Candidate sets: Writer1=%zu, Writer2=%zu",
                                   writerId, baseAddr, tempVal, g_menuStateCandidates1.size(), g_menuStateCandidates2.size());
#endif

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
#ifdef _DEBUG
                        DllLog("[WARNING] MenuState hook %u fired at address 0x%llX (val: %d), but pattern 6E** ** ** not found at -4.", writerId, tempAddr, tempVal);
#endif
                    }
                } else {
#ifdef _DEBUG
                    DllLog("[INFO] MenuState hook %u fired at address 0x%llX with val: %d (unexpected for writer %u). Ignoring for selection.", writerId, tempAddr, tempVal, writerId);
#endif
                }
            }
        }

        g_pSharedMemory->m_statusAddrGameRomCamera = g_addrGameRomCamera.load();
        g_pSharedMemory->m_statusAddrShortcutMenu  = g_addrShortcutMenu.load();
        g_pSharedMemory->m_statusAddrMenuState     = g_addrMenuState.load();
        g_pSharedMemory->m_statusAddrMagneTarget   = g_addrMagneTarget.load();
    }

    // -------------------------------------------------------------------------
    // Dynamic camera writer detection via VEH + page guard.
    // When mousecam is active we arm a PAGE_GUARD on the camera position fields.
    // Any game JIT instruction that tries to write there fires STATUS_GUARD_PAGE_VIOLATION.
    // Our VEH identifies the instruction, NOPs it, caches it, then re-arms the guard
    // so the next writer is also caught. After the hunt is satisfied we remove the guard.
    // On subsequent mousecam enables the cached NOPs are applied instantly with no hunt delay.
    // -------------------------------------------------------------------------

    struct WriterRecord {
        uintptr_t rip;                 // address of the write instruction
        uint8_t   g_originalBytes[16];   // saved original bytes
        size_t    patchSize;           // how many bytes we NOP'd
        bool      nopActive;
    };

    static std::vector<WriterRecord> g_discoveredWriters;   // protected by g_writerCS
    static CRITICAL_SECTION          g_writerCS;
    static PVOID                     g_vehHandle = nullptr;
    static std::atomic<bool>         g_guardArmed{false};
    static std::atomic<bool>         g_writerHuntActive{false};
    static uintptr_t                 g_guardPage = 0;       // base address of the guarded page
    static DWORD                     g_guardOldProtect = 0;

    // DetectWriteInstructionSize decodes x86 MOVBE and MOV store instructions
    // to determine how many bytes to NOP. We must be precise — NOPing too many
    // bytes would corrupt the next instruction; too few leaves a partial write.
    // Only MOVBE (0F 38 F1) and MOV (89) stores are handled because JIT output
    // on Wii U emulation uses these for big-endian float writes.
    static size_t DetectWriteInstructionSize(const uint8_t* p) {
        size_t offset = 0;

        // Consume optional REX prefix (0x40–0x4F)
        bool hasRex = (p[offset] >= 0x40 && p[offset] <= 0x4F);
        if (hasRex) offset++;

        // MOVBE store:  [REX] 0F 38 F1 /r
        if (p[offset] == 0x0F && p[offset+1] == 0x38 && p[offset+2] == 0xF1) {
            offset += 3; // 0F 38 F1
            uint8_t modrm = p[offset++];
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t rm  = modrm & 7;
            bool hasSib = (rm == 4);    // SIB byte follows when rm==4
            if (hasSib) offset++;       // SIB
            if      (mod == 1) offset += 1; // disp8
            else if (mod == 2) offset += 4; // disp32
            else if (mod == 0 && rm == 5) offset += 4; // RIP-relative disp32
            return offset;
        }

        // MOV r/m32, r32:  [REX] 89 /r
        if (p[offset] == 0x89) {
            offset++;
            uint8_t modrm = p[offset++];
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t rm  = modrm & 7;
            bool hasSib = (rm == 4);
            if (hasSib) offset++;
            if      (mod == 1) offset += 1;
            else if (mod == 2) offset += 4;
            else if (mod == 0 && rm == 5) offset += 4;
            return offset;
        }

        return 0; // unrecognized instruction — leaving it alone is safer than corrupting code
    }

    // NOP the write instruction at rip. Returns true and populates rec on success.
    static bool NopInstruction(uintptr_t rip, WriterRecord& rec) {
        uint8_t buf[16] = {};
        __try { memcpy(buf, (const void*)rip, 16); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }

        size_t sz = DetectWriteInstructionSize(buf);
        if (sz == 0 || sz > 15) return false;

        DWORD oldProt = 0;
        if (!VirtualProtect((LPVOID)rip, sz, PAGE_EXECUTE_READWRITE, &oldProt))
            return false;

        memcpy(rec.g_originalBytes, buf, sz);
        rec.rip       = rip;
        rec.patchSize = sz;
        rec.nopActive = true;
        memset((void*)rip, 0x90, sz);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)rip, sz);

        VirtualProtect((LPVOID)rip, sz, oldProt, &oldProt);
        return true;
    }

    // Restore a previously NOP'd instruction from its WriterRecord.
    static void RestoreInstruction(WriterRecord& rec) {
        if (!rec.nopActive || rec.patchSize == 0) return;
        DWORD oldProt = 0;
        if (VirtualProtect((LPVOID)rec.rip, rec.patchSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy((void*)rec.rip, rec.g_originalBytes, rec.patchSize);
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)rec.rip, rec.patchSize);
            VirtualProtect((LPVOID)rec.rip, rec.patchSize, oldProt, &oldProt);
        }
        rec.nopActive = false;
    }

    // Remove PAGE_GUARD from the camera page.
    static void DisarmPageGuard() {
        if (!g_guardArmed) return;
        DWORD old = 0;
        VirtualProtect((LPVOID)g_guardPage, 0x1000, g_guardOldProtect & ~PAGE_GUARD, &old);
        g_guardArmed = false;
    }

    // Arm PAGE_GUARD on the 4 KB page containing the camera position fields.
    // The guard is one-shot — after the first write the OS clears it and fires
    // STATUS_GUARD_PAGE_VIOLATION, which our VEH catches to identify the writer.
    // We then decide whether to NOP the writer or single-step and re-arm.
    static void ArmPageGuard(uintptr_t gc_addr) {
        uintptr_t page = gc_addr & ~(uintptr_t)0xFFF;
        if (g_guardArmed) {
            if (g_guardPage == page) return;
            DisarmPageGuard(); // Disarm old page before arming new one
        }
        DWORD old = 0;
        if (VirtualProtect((LPVOID)page, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old)) {
            g_guardPage       = page;
            g_guardOldProtect = old;
            g_guardArmed      = true;
        }
    }

    static thread_local bool t_isSingleStepping = false;

    // Vectored Exception Handler: catches PAGE_GUARD violations on camera memory.
    static LONG NTAPI CameraWriterVehHandler(PEXCEPTION_POINTERS ep) {
        if (ep->ExceptionRecord->ExceptionCode == STATUS_SINGLE_STEP) {
            if (t_isSingleStepping) {
                t_isSingleStepping = false;
                // Instruction finished executing, re-arm the guard page
                if (g_writerHuntActive) {
                    ArmPageGuard(g_addrGameRomCamera + 0x550);
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }

        if (ep->ExceptionRecord->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION)
            return EXCEPTION_CONTINUE_SEARCH;

        // ExceptionInformation[1] = the virtual address that was accessed
        uintptr_t faultAddr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        if (faultAddr < g_guardPage || faultAddr >= g_guardPage + 0x1000)
            return EXCEPTION_CONTINUE_SEARCH;

        // Guard is one-shot — OS has stripped it, mark disarmed immediately
        g_guardArmed = false;

        bool isWrite = (ep->ExceptionRecord->ExceptionInformation[0] == 1);
        uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;

        // We only want to NOP JIT writes to the specific camera coordinates.
        // For everything else (reads, or writes to other variables on the page),
        // we must single-step over the instruction to re-arm the guard page.
        bool shouldNop = false;

        if (isWrite && g_writerHuntActive) {
            uintptr_t base = g_addrGameRomCamera;
            if (faultAddr == base + 0x550 || faultAddr == base + 0x554 || faultAddr == base + 0x558) {
                MEMORY_BASIC_INFORMATION mbi = {};
                VirtualQuery((LPCVOID)rip, &mbi, sizeof(mbi));
                bool isJit = (mbi.Type == MEM_PRIVATE) &&
                             (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
                if (isJit) {
                    shouldNop = true;
                    if (!g_writerBlacklist.empty()) {
                        bool isBlacklisted = false;
                        __try {
                            uintptr_t win_start = rip >= 128 ? rip - 128 : 0;
                            uintptr_t win_end = rip + 128;
                            for (const auto& pat : g_writerBlacklist) {
                                size_t patLen = pat.bytes.size();
                                uintptr_t win_start = (rip >= patLen) ? (rip - patLen + 1) : 0;
                                uintptr_t win_end = rip;
                                for (uintptr_t search_ptr = win_start; search_ptr <= win_end; search_ptr++) {
                                    bool match = true;
                                    for (size_t i = 0; i < patLen; ++i) {
                                        if (!pat.isWildcard[i] && *(uint8_t*)(search_ptr + i) != pat.bytes[i]) {
                                            match = false;
                                            break;
                                        }
                                    }
                                    if (match) {
                                        isBlacklisted = true;
                                        break;
                                    }
                                }
                                if (isBlacklisted) break;
                            }
                        } __except (EXCEPTION_EXECUTE_HANDLER) {
                        }
                        if (isBlacklisted) {
                            shouldNop = false;
                            g_lastBlacklistedWriteTime.store(GetTickCount64());
                        }
                    }
                }
            }
        }

        if (shouldNop) {
            // Use EnterCriticalSection (not Try) — we must never miss a writer.
            EnterCriticalSection(&g_writerCS);
            bool alreadyKnown = false;
            for (auto& wr : g_discoveredWriters) {
                if (wr.rip == rip) {
                    alreadyKnown = true;
                    // JIT must have recompiled and overwritten our NOPs. Re-apply them.
                    if (wr.patchSize > 0) {
                        DWORD old = 0;
                        if (VirtualProtect((LPVOID)wr.rip, wr.patchSize, PAGE_EXECUTE_READWRITE, &old)) {
                            memset((void*)wr.rip, 0x90, wr.patchSize);
                            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)wr.rip, wr.patchSize);
                            VirtualProtect((LPVOID)wr.rip, wr.patchSize, old, &old);
                            wr.nopActive = true;
                        }
                    }
                    break; 
                }
            }
            if (!alreadyKnown) {
                WriterRecord rec = {};
                if (NopInstruction(rip, rec)) {
                    g_discoveredWriters.push_back(rec);
                    if (g_pSharedMemory) {
                        g_pSharedMemory->m_statusWritersFound = (uint32_t)g_discoveredWriters.size();
                    }
                } else {
                    // Failed to NOP (unrecognized instruction), we must single step it
                    shouldNop = false; 
                }
            }
            LeaveCriticalSection(&g_writerCS);

            if (shouldNop) {
                // We successfully NOP'd the instruction (or re-NOP'd it). 
                // The instruction pointer is still at the NOPs. When we continue execution, 
                // it will execute the NOPs (which don't access memory) and naturally proceed.
                // We intentionally DO NOT re-arm the guard page here so performance returns to normal.
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        // If we reach here, we are NOT NOPing the instruction.
        // We must execute it to let it access the memory, but if we do, the guard page is gone.
        // So we set the Trap Flag (Single Step). The CPU will execute this ONE instruction,
        // then fire STATUS_SINGLE_STEP, where we will catch it and re-arm the guard page.
        ep->ContextRecord->EFlags |= 0x100; // Set TF
        t_isSingleStepping = true;
        
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Apply NOPs for all already-discovered writers (instant, no hunt needed).
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

    // Restore original bytes for all discovered writers.
    static void RestoreAllWriterNops() {
        EnterCriticalSection(&g_writerCS);
        for (auto& wr : g_discoveredWriters) {
            RestoreInstruction(wr);
        }
        LeaveCriticalSection(&g_writerCS);
    }

    // Camera position fields at GameRomCamera + 0x550/0x554/0x558.
    // These are reverse-engineered offsets in the game's camera struct.
    // We guard this specific page so writes to unrelated fields on the same
    // page (which share the 4 KB guard) are single-stepped through without NOP.
    static void StartWriterHunt() {
        ApplyAllWriterNops();
        if (!g_vehHandle) {
            g_vehHandle = AddVectoredExceptionHandler(1, CameraWriterVehHandler);
        }
        g_writerHuntActive = true;

        // Always arm — even with cached writers, JIT recompile may have created new ones.
        ArmPageGuard(g_addrGameRomCamera + 0x550);
    }

    // Disable writer hunting: remove guard, restore NOPs, keep list for next time.
    static void StopWriterHunt() {
        g_writerHuntActive = false;
        DisarmPageGuard();
        // VEH stays installed (low overhead when not hunting)
        RestoreAllWriterNops();
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
            { L"ShortcutMenu",    vCfg.shortcutMenuAob, false, 0 },
            { L"MenuState 1",     vCfg.menuStateAob1,   false, 0 },
            { L"MenuState 2",     vCfg.menuStateAob2,   false, 0 },
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
                
                if (g_pSharedMemory->m_reqResetScan) {
                    g_pSharedMemory->m_reqResetScan = false;
                    bool preserveMenu = g_preserveMenuStateOnReset.exchange(false) && (g_addrMenuState.load() != 0);

                    DllLog("[INFO] Scanner reset requested (%s). Clearing addresses and reloading blacklist.",
                           preserveMenu ? "excluding MenuState" : "full reset");
                    LoadWriterBlacklist();

                    currentExperimental = g_pSharedMemory->m_cfgCemuExperimental;
                    vCfg = GetCemuVersionConfig(currentExperimental);
                    DllLog("[INFO] Scanner reset applied. Mode: %ls", currentExperimental ? L"Cemu Experimental" : L"Cemu 2.6");
                    tasks[0].patternStr = vCfg.gameRomCameraAob;
                    tasks[1].patternStr = vCfg.shortcutMenuAob;
                    tasks[2].patternStr = vCfg.menuStateAob1;
                    tasks[3].patternStr = vCfg.menuStateAob2;
                    tasks[4].patternStr = vCfg.magnesisAob;

                    for (size_t i = 0; i < tasks.size(); ++i) {
                        if (preserveMenu && (i == 2 || i == 3)) {
                            continue;
                        }
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

                    RestoreAllPatches();
                    EnterCriticalSection(&g_patchCS);
                    g_magnePatchesInitialized = false;
                    LeaveCriticalSection(&g_patchCS);

                    g_writerHuntActive = false;
                    DisarmPageGuard();
                    EnterCriticalSection(&g_writerCS);
                    for (auto& wr : g_discoveredWriters) RestoreInstruction(wr);
                    g_discoveredWriters.clear();
                    LeaveCriticalSection(&g_writerCS);
                    
                    g_pSharedMemory->m_statusWritersFound = 0;
                    g_pSharedMemory->m_statusAddrGameRomCamera = 0;
                    g_pSharedMemory->m_statusAddrShortcutMenu = 0;
                    if (!preserveMenu) {
                        g_pSharedMemory->m_statusAddrMenuState = 0;
                    }
                    g_pSharedMemory->m_statusAddrMagneTarget = 0;
                    
                    allOtherFound = false;
                    nextIdx = 1;
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
                    bool preserveMenu = (g_addrMenuState.load() != 0);
                    DllLog("[WARNING] GameRomCamera memory inaccessible. Address voided! Resetting scanner (%s).",
                           preserveMenu ? "preserving MenuState" : "full reset");
                    tasks[0].found = false;
                    tasks[0].address = 0;
                    g_addrGameRomCamera = 0;

                    // Address voided — stop any active hunt and discard all cached writers
                    // (their RIPs are tied to the old JIT layout and are now stale).
                    g_writerHuntActive = false;
                    DisarmPageGuard();
                    EnterCriticalSection(&g_writerCS);
                    for (auto& wr : g_discoveredWriters) RestoreInstruction(wr);
                    g_discoveredWriters.clear();
                    LeaveCriticalSection(&g_writerCS);

                    for (size_t i = 1; i < tasks.size(); ++i) {
                        if (preserveMenu && (i == 2 || i == 3)) continue;
                        tasks[i].found = false;
                        tasks[i].address = 0;
                    }

                    g_addrShortcutMenu = 0;
                    if (!preserveMenu) {
                        g_addrMenuState = 0;
                        EnterCriticalSection(&g_menuCandidateCS);
                        g_menuStateCandidates1.clear();
                        g_menuStateCandidates2.clear();
                        LeaveCriticalSection(&g_menuCandidateCS);
                    }
                    g_addrMagneTarget = 0;

                    RestoreAllPatches();

                    EnterCriticalSection(&g_patchCS);
                    g_magnePatchesInitialized = false;
                    LeaveCriticalSection(&g_patchCS);

                    ResetScannerState();
                }
            }

            if (!tasks[0].found) {
                DllLog("[INFO] Scanning for GameRomCamera...");
                Pattern pat = ParseAOB(tasks[0].patternStr);
                std::vector<uintptr_t> rawCandidates;

                if (ScanProcessAOBAll(pat, rawCandidates)) {
#ifdef _DEBUG
                    DllLog("[INFO] Found %zu GameRomCamera match candidate(s). Verifying offsets and telemetry...", rawCandidates.size());
#endif
                    uintptr_t bestCandidate = 0;
                    int bestScore = -1;

                    for (size_t i = 0; i < rawCandidates.size(); ++i) {
                        uintptr_t cand = rawCandidates[i] - 0x10;
                        int score = 0;
                        bool valid = VerifyGameRomCamera(cand, score);
#ifdef _DEBUG
                        float fov = ReadFloatBE(cand + 0x654);
                        float posX = ReadFloatBE(cand + 0x550);
                        float posY = ReadFloatBE(cand + 0x554);
                        float posZ = ReadFloatBE(cand + 0x558);
                        float focX = ReadFloatBE(cand + 0x63C);
                        float focY = ReadFloatBE(cand + 0x640);
                        float focZ = ReadFloatBE(cand + 0x644);

                        DllLog("[INFO] Match [%zu/%zu] at 0x%llX: Score=%d (FOV=%.2f, Pos=[%.1f, %.1f, %.1f], Foc=[%.1f, %.1f, %.1f]) -> %s",
                               i + 1, rawCandidates.size(), cand, score, fov, posX, posY, posZ, focX, focY, focZ, valid ? "VALID" : "REJECTED");
#endif

                        if (valid && score > bestScore) {
                            bestScore = score;
                            bestCandidate = cand;
                        }
                    }

                    if (bestCandidate != 0) {
                        tasks[0].found = true;
                        tasks[0].address = bestCandidate;
                        g_addrGameRomCamera = bestCandidate;
                        if (g_pSharedMemory) {
                            g_pSharedMemory->m_statusAddrGameRomCamera = bestCandidate;
                        }
                        DllLog("[SUCCESS] Verified active GameRomCamera at 0x%llX (Score: %d)", bestCandidate, bestScore);
                    } else {
                        // Fallback: If in a loading screen where coords are uninitialized, take first candidate if available
                        if (!rawCandidates.empty()) {
                            uintptr_t fallback = rawCandidates[0] - 0x10;
                            tasks[0].found = true;
                            tasks[0].address = fallback;
                            g_addrGameRomCamera = fallback;
                            if (g_pSharedMemory) {
                                g_pSharedMemory->m_statusAddrGameRomCamera = fallback;
                            }
                            DllLog("[INFO] Selected initial GameRomCamera at 0x%llX (will re-verify on gameplay)", fallback);
                        } else {
                            DllLog("[WARNING] GameRomCamera candidates rejected. Retrying in 500ms...");
                        }
                    }
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
                tasks[1].found = false;
                tasks[1].address = 0;
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
                tasks[2].found = false;
                tasks[2].address = 0;
                tasks[3].found = false;
                tasks[3].address = 0;
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

            // Find the next unfound task and scan it
            size_t targetIdx = 0;
            for (size_t i = 0; i < tasks.size() - 1; ++i) {
                size_t idx = 1 + ((nextIdx - 1 + i) % (tasks.size() - 1));
                bool enabled = true;
                if (idx == 1) enabled = scanShortcutMenu;
                else if (idx == 2 || idx == 3) enabled = scanMenuState;
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
                tasks[1].found = true;
            }
            if (scanMenuState && g_addrMenuState != 0) {
                tasks[2].found = true;
                tasks[3].found = true;
            }

            // 2. Perform AOB pattern scanning for the current target task
            if (targetIdx != 0) {
                nextIdx = (targetIdx % (tasks.size() - 1)) + 1;

                if (targetIdx == 1 && scanShortcutMenu && !tasks[1].found && !g_shortcutHookActive) {
                    DllLog("[INFO] Scanning for ShortcutMenu instruction pattern...");
                    Pattern pat = ParseAOB(tasks[1].patternStr);
                    uintptr_t foundAddress = 0;
                    if (ScanProcessAOB(pat, foundAddress)) {
                        DllLog("[SUCCESS] Found ShortcutMenu instruction at 0x%llX. Setting up detour hook...", foundAddress);
                        tasks[1].address = foundAddress;
                        if (SetupShortcutHook(foundAddress)) {
                            DllLog("[SUCCESS] Detour hook set up successfully. Waiting for game write...");
                        } else {
                            DllLog("[ERROR] Failed to set up detour hook for ShortcutMenu.");
                        }
                    } else {
                        DllLog("[WARNING] ShortcutMenu instruction pattern not found. Retrying in 1s...");
                    }
                } else if (targetIdx == 2 && scanMenuState && !tasks[2].found && !g_menuStateHook1Active && g_addrMenuState == 0) {
                    DllLog("[INFO] Scanning for MenuState AOB 1 instruction pattern...");
                    Pattern pat = ParseAOB(tasks[2].patternStr);
                    uintptr_t foundAddress = 0;
                    if (ScanProcessAOB(pat, foundAddress)) {
                        DllLog("[SUCCESS] Found MenuState AOB 1 instruction at 0x%llX. Setting up trampoline hook 1...", foundAddress);
                        tasks[2].address = foundAddress;
                        if (SetupMenuStateHook(foundAddress, 1)) {
                            DllLog("[SUCCESS] MenuState trampoline hook 1 set up successfully. Waiting for game write...");
                        } else {
                            DllLog("[ERROR] Failed to set up trampoline hook 1 for MenuState.");
                        }
                    } else {
                        DllLog("[WARNING] MenuState AOB 1 instruction pattern not found. Retrying in 1s...");
                    }
                } else if (targetIdx == 3 && scanMenuState && !tasks[3].found && !g_menuStateHook2Active && g_addrMenuState == 0) {
                    DllLog("[INFO] Scanning for MenuState AOB 2 instruction pattern...");
                    Pattern pat = ParseAOB(tasks[3].patternStr);
                    uintptr_t foundAddress = 0;
                    if (ScanProcessAOB(pat, foundAddress)) {
                        DllLog("[SUCCESS] Found MenuState AOB 2 instruction at 0x%llX. Setting up trampoline hook 2...", foundAddress);
                        tasks[3].address = foundAddress;
                        if (SetupMenuStateHook(foundAddress, 2)) {
                            DllLog("[SUCCESS] MenuState trampoline hook 2 set up successfully. Waiting for game write...");
                        } else {
                            DllLog("[ERROR] Failed to set up trampoline hook 2 for MenuState.");
                        }
                    } else {
                        DllLog("[WARNING] MenuState AOB 2 instruction pattern not found. Retrying in 1s...");
                    }
                } else if (targetIdx == 4 && scanMagneTarget && !tasks[4].found) {
                    DllLog("[INFO] Scanning for Magne Target Sig...");
                    Pattern pat = ParseAOB(tasks[4].patternStr);
                    std::vector<uintptr_t> rawCandidates;
                    if (ScanProcessAOBAll(pat, rawCandidates)) {
                        DllLog("[INFO] Found %zu Magne Target Sig candidate(s). Verifying offsets and values...", rawCandidates.size());
                        uintptr_t bestCandidate = 0;
                        int bestScore = -1;

                        for (size_t i = 0; i < rawCandidates.size(); ++i) {
                            uintptr_t cand = rawCandidates[i];
                            int score = 0;
                            bool valid = VerifyMagneTargetSig(cand, vCfg, currentExperimental, score);

                            DllLog("[INFO] Magne candidate [%zu/%zu] at 0x%llX: Score=%d -> %s",
                                   i + 1, rawCandidates.size(), cand, score, valid ? "VALID" : "REJECTED");

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
                        } else {
                            DllLog("[WARNING] All Magne Target Sig candidates rejected by offset/opcode verification. Retrying in 1s...");
                        }
                    } else {
                        DllLog("[WARNING] Magne Target Sig not found. Retrying in 1s...");
                    }
                }
            }

            allOtherFound = true;
            for (size_t i = 1; i < tasks.size(); ++i) {
                bool enabled = true;
                if (i == 1) enabled = scanShortcutMenu;
                else if (i == 2 || i == 3) enabled = scanMenuState;
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
#ifdef _DEBUG
        int aob_dump_countdown = 0;
#endif
        
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

        bool last_should_nop = false;
        float last_written_x = 0.0f;  // what we wrote to +0x550 last frame
        float last_written_y = 0.0f;  // what we wrote to +0x554 last frame
        float last_written_z = 0.0f;  // what we wrote to +0x558 last frame
        bool  has_written_once = false; // avoid false overwrite on first frame after enable
        auto last_frame_time = std::chrono::steady_clock::now();

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
                last_gc_addr = gc_addr;
                // Camera reset! Clear cached writers to prevent stale NOPs and corruption
                if (g_writerHuntActive) {
                    EnterCriticalSection(&g_writerCS);
                    for (auto& wr : g_discoveredWriters) RestoreInstruction(wr);
                    g_discoveredWriters.clear();
                    if (g_pSharedMemory) g_pSharedMemory->m_statusWritersFound = 0;
                    LeaveCriticalSection(&g_writerCS);
                }
                if (gc_addr == 0) {
                    last_should_nop = false;
                    g_writerHuntActive = false;
                    has_written_once = false;
                }
            }

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
            static bool resetTriggeredOnState2 = false;
            if (scanMenuState && g_addrMenuState != 0) {
                int32_t val = ReadInt32BE(g_addrMenuState);
                g_liveMenuState = val;
                if (g_pSharedMemory) {
                    g_pSharedMemory->m_teleLiveMenuState = static_cast<uint8_t>(val);
                }
                menu_active = (val == 6 || val == 10);

                if (val == 2) {
                    if (!resetTriggeredOnState2) {
                        resetTriggeredOnState2 = true;
                        DllLog("[INFO] MenuState is 2 (Game Reload / Save Load detected). Triggering scanner reset (excluding MenuState).");
                        g_preserveMenuStateOnReset = true;
                        if (g_pSharedMemory) {
                            g_pSharedMemory->m_reqResetScan = true;
                        }
                    }
                } else {
                    resetTriggeredOnState2 = false;
                }
            } else {
                menu_active = false;
                g_liveMenuState = 3;
                if (g_pSharedMemory) {
                    g_pSharedMemory->m_teleLiveMenuState = 3;
                }
                resetTriggeredOnState2 = false;
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

            bool should_nop = g_mousecamActive && (!magnesis_mode || fps_magne_active) && !menu_active && !is_shortcut_open;
            if (should_nop && !g_writerHuntActive) {
                last_should_nop = false;
            }
            if (should_nop != last_should_nop) {
                last_should_nop = should_nop;
                if (should_nop) {
                    // Start the hunt: apply cached NOPs instantly, arm guard to catch new writers
                    StartWriterHunt();
                } else {
                    // Stop hunting: disarm guard, restore all writer instructions
                    StopWriterHunt();
                    // Reset tracking so we don't get a false overwrite on re-enable
                    last_written_x = 0.0f;
                    last_written_y = 0.0f;
                    last_written_z = 0.0f;
                    has_written_once = false;
                }

                // SMOOTH TRANSITION: Restore orbital camera angles from the game camera on recapture (1:1 with Rust)
                if (should_nop && gc_addr != 0) {
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

            if (g_pSharedMemory) {
                g_pSharedMemory->m_cfgMagnesisEnabled = magnesis_mode;
            }

            if (gc_addr != 0) {
                g_liveCamPosX = ReadFloatBE(gc_addr + 0);
                g_liveCamPosY = ReadFloatBE(gc_addr + 4);
                g_liveCamPosZ = ReadFloatBE(gc_addr + 8);
                g_liveCamFocX = ReadFloatBE(gc_addr + 0xC);
                g_liveCamFocY = ReadFloatBE(gc_addr + 0x10);
                g_liveCamFocZ = ReadFloatBE(gc_addr + 0x14);
                g_liveCamFOV = ReadFloatBE(gc_addr + 0x24);

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

                if (g_mousecamActive && is_foreground) {
                    POINT pt = {0, 0};
                    GetCursorPos(&pt);
                    POINT center = GetCemuWindowCenter(hwndFg);

                    dx = static_cast<float>(pt.x - center.x);
                    dy = static_cast<float>(pt.y - center.y);

                    if (dx != 0.0f || dy != 0.0f) {
                        SetCursorPos(center.x, center.y);
                    }
                }

                if (g_mousecamActive) {
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

                    // Suspend guard page during our own background reads/writes to avoid triggering the VEH
                    bool wasArmed = g_guardArmed;
                    if (wasArmed) {
                        DisarmPageGuard();
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

                    if ((!magnesis_mode || fps_magne_active) && !menu_active) {
                        static int g_huntFramesLeft = 0;
                        static uint32_t last_writers_found = 0;

                        if (g_pSharedMemory) {
                            uint32_t curr_writers = g_pSharedMemory->m_statusWritersFound;
                            if (curr_writers > last_writers_found) {
                                // We successfully caught a writer! Stop hunting immediately.
                                g_huntFramesLeft = 0;
                                last_writers_found = curr_writers;
                            }
                        }

                        // --- Overwrite detection ---
                        // Read back what's in camera memory only once every 100ms (10Hz).
                        // If the game stomped our values, a new writer appeared. Arm the guard for a brief window.
                        static uint64_t last_overwrite_check_ms = 0;
                        uint64_t now_ms = GetTickCount64();
                        if (has_written_once && g_writerHuntActive && (now_ms - last_overwrite_check_ms >= 100)) {
                            last_overwrite_check_ms = now_ms;
                            float cur_x = ReadFloatBE(g_addrGameRomCamera + 0x550);
                            float cur_y = ReadFloatBE(g_addrGameRomCamera + 0x554);
                            float cur_z = ReadFloatBE(g_addrGameRomCamera + 0x558);
                            if (cur_x != last_written_x || cur_y != last_written_y || cur_z != last_written_z) {
                                // Overwrite detected — hunt for the next ~40ms (10 frames)
                                g_huntFramesLeft = 10;
                            }
                        }

                        uint64_t now = GetTickCount64();
                        if (now - g_lastBlacklistedWriteTime.load() <= 50) {
                            // A blacklisted writer is currently in control. Pause mousecam.
                            virt_cam_initialized = false;
                        } else {
                            WriteFloatBE(g_addrGameRomCamera + 0x550, vcam_pos_x);
                            WriteFloatBE(g_addrGameRomCamera + 0x554, vcam_pos_y);
                            WriteFloatBE(g_addrGameRomCamera + 0x558, vcam_pos_z);

                            last_written_x = vcam_pos_x;
                            last_written_y = vcam_pos_y;
                            last_written_z = vcam_pos_z;
                            has_written_once = true;
                        }

                        // Arm guard if we are currently hunting
                        if (g_writerHuntActive && g_huntFramesLeft > 0) {
                            ArmPageGuard(g_addrGameRomCamera + 0x550);
                            g_huntFramesLeft--;
                        }
                    } else {
                        // Not writing. If we were hunting, maintain the guard page.
                        static int g_huntFramesLeft = 0; // shadowing, but menu state shouldn't leak hunt
                        if (wasArmed) {
                            ArmPageGuard(g_addrGameRomCamera + 0x550);
                        }
                    }
                }
            }

#ifdef _DEBUG
            if (g_pSharedMemory && g_pSharedMemory->m_reqDumpAob) {
                g_pSharedMemory->m_reqDumpAob = false;
                
                // Constraint: Only work when camera is on and magnesis is off
                if (g_mousecamActive && !magnesis_auto_active) {
                    // 1. Clear known writers and restore them
                    EnterCriticalSection(&g_writerCS);
                    for (auto& wr : g_discoveredWriters) RestoreInstruction(wr);
                    g_discoveredWriters.clear();
                    LeaveCriticalSection(&g_writerCS);
                    
                    // 2. We don't sleep here. We set a countdown. 
                    // The main loop's overwrite detection will naturally catch the 3 writers over the next few frames.
                    // We wait 125 frames (~500ms) to ensure all 3 writers (X, Y, Z) are caught even at 30fps.
                    aob_dump_countdown = 125;
                    g_pSharedMemory->m_statusWritersFound = 0;
                }
            }

            if (aob_dump_countdown > 0) {
                aob_dump_countdown--;
                if (aob_dump_countdown == 0) {
                    // Dump newly discovered (currently active) writers
                    EnterCriticalSection(&g_writerCS);
                    if (!g_discoveredWriters.empty()) {
                        uintptr_t min_rip = UINTPTR_MAX;
                        uintptr_t max_rip = 0;
                        size_t max_patch = 0;
                        for (auto& wr : g_discoveredWriters) {
                            if (wr.rip < min_rip) min_rip = wr.rip;
                            if (wr.rip > max_rip) { max_rip = wr.rip; max_patch = wr.patchSize; }
                        }
                        
                        uintptr_t start_dump = min_rip - 32;
                        uintptr_t end_dump = max_rip + max_patch + 32;
                        
                        FILE* f = nullptr;
                        if (_wfopen_s(&f, L"cemu_aob_dump.txt", L"a") == 0) {
                            fwprintf(f, L"AOB Dump from %llX to %llX\n", start_dump, end_dump);
                            for (uintptr_t ptr = start_dump; ptr < end_dump; ptr++) {
                                bool isStart = false;
                                bool isEnd = false;
                                bool insideWriter = false;
                                uint8_t byte_to_print = *(uint8_t*)ptr;
                                
                                for (auto& wr : g_discoveredWriters) {
                                    if (wr.rip == ptr) isStart = true;
                                    if (wr.rip + wr.patchSize - 1 == ptr) isEnd = true;
                                    if (ptr >= wr.rip && ptr < wr.rip + wr.patchSize) {
                                        insideWriter = true;
                                        byte_to_print = wr.g_originalBytes[ptr - wr.rip];
                                        // Don't break, so we correctly set isStart and isEnd if overlapping (though they shouldn't overlap)
                                    }
                                }
                                
                                if (isStart) fprintf(f, "[ ");
                                fprintf(f, "%02X", byte_to_print);
                                if (isEnd) fprintf(f, " ] ");
                                else fprintf(f, " ");
                            }
                            fprintf(f, "\n");
                            fclose(f);
                        }
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
            }
        }

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

        g_writerHuntActive = false;
        DisarmPageGuard();
        RestoreAllWriterNops();

        if (g_vehHandle) {
            RemoveVectoredExceptionHandler(g_vehHandle);
            g_vehHandle = nullptr;
        }

        RestoreAllPatches();

        DeleteCriticalSection(&g_patchCS);
        DeleteCriticalSection(&g_writerCS);
        DeleteCriticalSection(&g_menuCandidateCS);

        g_sharedMemory.Close();
    }

} // namespace Mod
