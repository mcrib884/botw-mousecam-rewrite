#include "mod.h"
#pragma comment(lib, "winmm.lib")
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <string>
#include <algorithm>
#include <TlHelp32.h>
#include <cstdint>
#include <stdio.h>

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
    uint64_t g_magnesisXWriterReturn = 0;

    void AsmMagnesisXWriter();
}

namespace Mod {

    static HMODULE g_hModule = nullptr;
    static HANDLE g_hMapFile = nullptr;
    static SharedMemoryLayout* g_pSharedMemory = nullptr;

    static void DllLog(const char* format, ...) {
        if (!g_pSharedMemory) return;

        char msg[128];
        va_list args;
        va_start(args, format);
        vsprintf_s(msg, format, args);
        va_end(args);

        uint32_t idx = g_pSharedMemory->m_logWriteIdx % 8;
        strcpy_s(g_pSharedMemory->m_logQueue[idx], msg);
        g_pSharedMemory->m_logWriteIdx++;
    }

    static std::atomic<int> g_runningThreads{0};

    static void OnThreadExit();

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
        std::vector<bool> isWildcard;
    };

    static Pattern ParseAOB(const std::string& aobStr) {
        Pattern pat;
        size_t i = 0;
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
                unsigned int val = 0;
                std::string byteStr = aobStr.substr(i, 2);
                sscanf_s(byteStr.c_str(), "%x", &val);
                pat.bytes.push_back(static_cast<unsigned char>(val));
                pat.isWildcard.push_back(false);
                i += 2;
            }
        }
        return pat;
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
        size_t i = 0;
        while (i + pattern.bytes.size() <= bufferSize) {
            const void* p = memchr(buffer + i + firstNonWildcard, firstByte, bufferSize - (i + firstNonWildcard));
            if (!p) {
                break;
            }

            i = static_cast<const unsigned char*>(p) - buffer - firstNonWildcard;

            bool match = true;
            for (size_t j = 0; j < pattern.bytes.size(); ++j) {
                if (!pattern.isWildcard[j] && buffer[i + j] != pattern.bytes[j]) {
                    match = false;
                    break;
                }
            }

            if (match) {
                foundOffset = i;
                return true;
            }

            i++;
        }

        return false;
    }

    static uintptr_t g_emulatedRamBase = 0;
    static size_t g_emulatedRamSize = 0;

    static std::atomic<uint64_t> g_lastBlacklistedWriteTime = 0;

    static std::vector<Pattern> g_writerBlacklist;

    static void LoadWriterBlacklist() {
        g_writerBlacklist.clear();

        // 1. Internalized default patterns (including the new one)
        std::vector<std::string> internalPatterns = {
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 45 0F 38 F0 74 ?? ?? 66 41 0F 6E ?? ?? ?? ?? ?? ?? 66 41 0F 7E EE 45 0F 38 F1 74 ?? 00 F3 0F 5A ED F2 0F 12 ED 45 0F 38 F0 74 ?? ?? 66 41 0F 6E ?? 66 41 0F 7E D6 45 0F 38 F1 74 ?? 04 F3 0F 5A D2 F2 0F 12 D2 45 0F 38 F0 74 ?? ?? 66 41 0F 6E ?? 66 41 0F 7E DE 45 0F 38 F1 74 ?? 08 F3 0F 5A DB F2 0F 12 DB",
            "41 0F 6E F6 0F C8 89 44 24 1C 0F CB 89 5C 24 24 45 89 4C 15 4C 0F CD 89 6C 24 2C 66 41 0F 7E F6 45 0F 38 F1 74 3D 00 F3 0F 5A F6 F2 0F 12 F6 45 0F 38 F0 74 15 48 66 41 0F 6E C6 66 41 0F 7E C6 45 0F 38 F1 74 3D 04 F3 0F 5A C0 F2 0F 12 C0 45 0F 38 F0 74 15 4C 66 41 0F 6E CE 8B 54 24 70 89 D0 89 44 24 10 66 41 0F 7E CE 45 0F 38 F1 74 3D 08 F3 0F 5A C9 F2 0F 12 C9",
            "66 41 0F 6E F6 F2 44 0F 5A FC 66 45 0F 7E FE 45 0F 38 F1 74 1D 7C 41 8B 74 1D 7C 66 41 0F 7E EE [ 45 0F 38 F1 74 15 00 ] F3 0F 5A ED F2 0F 12 ED 41 89 74 1D 1C 45 0F 38 F0 74 1D 1C 66 41 0F 6E D6 8B 7C 24 7C 89 FB 89 5C 24 10 66 41 0F 7E F6 [ 45 0F 38 F1 74 15 04 ] F3 0F 5A F6 F2 0F 12 F6 89 D3 89 5C 24 14 66 41 0F 7E D6 [ 45 0F 38 F1 74 15 08 ] F3 0F 5A D2 F2 0F 12 D2 0F C8 89 44 24 1C 0F CD 89 6C 24 24 0F CE 89 74 24 04 66 0F 11 84 E4 38"
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

    static bool ScanProcessAOB(const Pattern& pattern, bool isCode, uintptr_t& foundAddress) {
        uintptr_t start = 0;
        uintptr_t end = 0;

        if (!isCode) {
            uintptr_t ramBase = 0;
            size_t ramSize = 0;
            if (g_emulatedRamBase != 0) {
                ramBase = g_emulatedRamBase;
                ramSize = g_emulatedRamSize;
            } else {
                if (FindEmulatedRam(ramBase, ramSize)) {
                    g_emulatedRamBase = ramBase;
                    g_emulatedRamSize = ramSize;
                }
            }
            if (ramBase != 0) {
                start = ramBase;
                end = ramBase + ramSize;
            } else {
                return false;
            }
        } else {
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            start = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
            end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        }

        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t current = start;
        size_t chunkSize = 2 * 1024 * 1024;
        size_t overlap = pattern.bytes.size();

        while (current < end) {
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(current), &mbi, sizeof(mbi))) {
                break;
            }

            bool scanThisPage = false;
            if (!isCode) {
                scanThisPage = (mbi.State == MEM_COMMIT) &&
                               (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_READONLY)) &&
                               !(mbi.Protect & PAGE_GUARD);
            } else {
                scanThisPage = (mbi.State == MEM_COMMIT) &&
                               (mbi.Type != MEM_IMAGE) &&
                               (mbi.Protect & (PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ)) &&
                               !(mbi.Protect & PAGE_GUARD);
            }

            if (scanThisPage) {
                uintptr_t regionAddress = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                size_t regionSize = mbi.RegionSize;

                for (size_t offset = 0; offset < regionSize; offset += (chunkSize > overlap ? chunkSize - overlap : chunkSize)) {
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
    static CodePatch g_magneYPatch = {};
    static CodePatch g_magneZPatch = {};
    static bool g_magnePatchesInitialized = false;

    static CodePatch g_shortcutHookPatch = {};
    static LPVOID g_shortcutTrampoline = nullptr;
    static std::atomic<uintptr_t> g_tempShortcutAddress{0};
    static std::atomic<int32_t> g_tempShortcutValue{0};
    static bool g_shortcutHookActive = false;

    static LPVOID AllocateWithin2GB(uintptr_t targetAddr, size_t size) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        
        // Search in a range of +/- 1 GB around targetAddr
        uintptr_t minAddr = targetAddr > 0x3FFFFFFF ? targetAddr - 0x3FFFFFFF : reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        uintptr_t maxAddr = targetAddr < UINTPTR_MAX - 0x3FFFFFFF ? targetAddr + 0x3FFFFFFF : reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

        // Align to dwAllocationGranularity
        minAddr = (minAddr + si.dwAllocationGranularity - 1) & ~static_cast<uintptr_t>(si.dwAllocationGranularity - 1);
        maxAddr = maxAddr & ~static_cast<uintptr_t>(si.dwAllocationGranularity - 1);

        for (uintptr_t addr = minAddr; addr < maxAddr; addr += si.dwAllocationGranularity) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) != 0) {
                if (mbi.State == MEM_FREE) {
                    LPVOID allocated = VirtualAlloc((LPVOID)addr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                    if (allocated) {
                        return allocated;
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

    static void RestoreAllPatches() {
        EnterCriticalSection(&g_patchCS);
        if (g_magnePatchesInitialized) {
            g_magneDetourPatch.Restore();
            g_magneYPatch.Restore();
            g_magneZPatch.Restore();
        }
        RemoveShortcutHook();
        LeaveCriticalSection(&g_patchCS);
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
                                for (uintptr_t search_ptr = win_start; search_ptr + pat.bytes.size() <= win_end; search_ptr++) {
                                    bool match = true;
                                    for (size_t i = 0; i < pat.bytes.size(); ++i) {
                                        if (!pat.isWildcard[i] && *(uint8_t*)(search_ptr + i) != pat.bytes[i]) {
                                            match = false;
                                            break;
                                        }
                                    }
                                    if (match && rip >= search_ptr && rip < search_ptr + pat.bytes.size()) {
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
            DllLog("[INFO] All DLL threads are exiting. Performing final cleanup...");
            RemoveShortcutHook();
            
            g_writerHuntActive = false;
            DisarmPageGuard();
            RestoreAllWriterNops();

            if (g_vehHandle) {
                RemoveVectoredExceptionHandler(g_vehHandle);
                g_vehHandle = nullptr;
            }

            RestoreAllPatches();

            if (g_pSharedMemory) {
                g_pSharedMemory->m_statusScanning = false;
                g_pSharedMemory->m_statusShutdownDone = true;
            }
        }
    }

    static DWORD WINAPI ScanAobThread(LPVOID lpParam) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        LoadWriterBlacklist();

        struct AobTask {
            std::wstring name;
            std::string patternStr;
            bool isCode;
            bool found;
            uintptr_t address;
        };

        std::vector<AobTask> tasks = {
            { L"GameRomCamera",  "10 1B F9 FC 70 ?? ?? ?? 10 31 97 58 00 00 00 40 47 61 6D 65 52 6F 6D 43 61 6D 65 72 61 00", false, false, 0 },
            { L"Magne Target Sig", "38 F0 74 1D 6C 66 41 0F 6E FE F2 44 0F 5A FD 66 45 0F 7E FE 45 0F 38 F1 74 1D 64 41 8B 54 1D 64 8B AC 24 80 00 00 00 45 0F 38 F0 74 2D 74 66 41 0F 6E D6 F3 0F 5A D2 F2 0F 12 D2 66 41 0F 7E F6 45 0F 38 F1 74 2D 68 F3 0F 5A F6 F2 0F 12 F6 66 44 0F 10 84 E4 68 02 00 00 66 41 0F 2E D0 0F 9A 84 24 8F 02 00 00 7A 1A 0F 92 84 24 8C 02 00 00 0F 97 84 24 8D 02 00 00 0F 94 84 24 8E 02 00 00 EB 18 C6 84 24 8C 02 00 00 00 C6 84 24 8D 02 00 00 00 C6 84 24 8E 02 00 00 00 41 89 54 1D 70 66 44 0F 10 8C E4 58 01 00 00 45 0F 38 F0 74 1D 70 66 45 0F 6E CE 66 41 0F 7E FE 45 0F 38 F1 74 2D 6C F3 0F 5A FF F2 0F 12 FF 66 45 0F 7E CE 45 0F 38 F1 74 2D 70 F3 45 0F 5A C9 F2 45 0F 12 C9 0F C8 89 44 24 2C 0F CA 89 54 24 04 66 0F 11 84 E4 08 01 00 00 66 0F 11 8C E4 F8 00 00 00 66 0F 11 94 E4 88 00 00 00 66 0F 11 9C E4 28 01 00 00 66 0F 11 A4 E4 78 02 00 00 66 0F 11 AC E4 18 01 00", true, false, 0 },
            { L"ShortcutMenu",    "41 0F 38 F1 9C 15 04 1C 00 00", true, false, 0 },
            { L"MenuState",       "00 19 29 29 08 19 29 29 08 00 00 00 00 1A ?? ?? ?? 0C 00 00 00 00 00 00 05 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00 00 54 5A 89 78 10 2F D4 14 00 00 00 00 00 00 00 00 10 31 F3 34 00 00 00 00 00 00 00 00 54 61 E3 AC", false, false, 0 }
        };

        bool allOtherFound = false;
        size_t nextIdx = 1;
        while (g_scanning) {
            if (g_pSharedMemory) {
                if (g_pSharedMemory->m_reqShutdown) {
                    break;
                }
                g_pSharedMemory->m_statusScanning = true;
                
                if (g_pSharedMemory->m_reqResetScan) {
                    g_pSharedMemory->m_reqResetScan = false;
                    DllLog("[INFO] Scanner reset requested. Clearing addresses and reloading blacklist.");
                    LoadWriterBlacklist();
                    for (size_t i = 0; i < tasks.size(); ++i) {
                        tasks[i].found = false;
                        tasks[i].address = 0;
                    }
                    g_addrGameRomCamera = 0;
                    g_addrMagneTarget = 0;
                    g_addrShortcutMenu = 0;
                    g_addrMenuState = 0;
                    
                    RemoveShortcutHook();

                    g_writerHuntActive = false;
                    DisarmPageGuard();
                    EnterCriticalSection(&g_writerCS);
                    for (auto& wr : g_discoveredWriters) RestoreInstruction(wr);
                    g_discoveredWriters.clear();
                    LeaveCriticalSection(&g_writerCS);
                    
                    g_pSharedMemory->m_statusWritersFound = 0;
                    g_pSharedMemory->m_statusAddrGameRomCamera = 0;
                    g_pSharedMemory->m_statusAddrMagneTarget = 0;
                    g_pSharedMemory->m_statusAddrShortcutMenu = 0;
                    g_pSharedMemory->m_statusAddrMenuState = 0;
                    
                    allOtherFound = false;
                    nextIdx = 1;
                }
            }

            if (tasks[0].found) {
                bool verifySuccess = false;
                if (g_addrGameRomCamera != 0) {
                    float test_z = ReadFloatBE(g_addrGameRomCamera + 0x67C);
                    if (test_z != 0.0f) {
                        verifySuccess = true;
                    }
                }

                if (!verifySuccess) {
                    DllLog("[WARNING] GameRomCamera verification failed (Z-coord is 0.0). Address voided! Resetting scanner.");
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
                        tasks[i].found = false;
                        tasks[i].address = 0;
                    }

                    g_addrMagneTarget = 0;
                    g_addrShortcutMenu = 0;
                    g_addrMenuState = 0;

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
                uintptr_t foundAddress = 0;

                if (ScanProcessAOB(pat, tasks[0].isCode, foundAddress)) {
                    tasks[0].found = true;
                    tasks[0].address = foundAddress;
                    g_addrGameRomCamera = foundAddress;
                    if (g_pSharedMemory) {
                        g_pSharedMemory->m_statusAddrGameRomCamera = foundAddress;
                    }
                    DllLog("[SUCCESS] Found GameRomCamera at 0x%llX", foundAddress);
                } else {
                    DllLog("[WARNING] GameRomCamera not found. Retrying in 500ms...");
                }
                
                // Sleep 500ms before checking again if GameRomCamera not found
                if (!tasks[0].found) {
                    for (int i = 0; i < 5 && g_scanning; ++i) {
                        Sleep(100);
                    }
                    continue;
                }
            }

            // Find the next unfound task and scan it
            size_t targetIdx = 0;
            for (size_t i = 0; i < tasks.size() - 1; ++i) {
                size_t idx = 1 + ((nextIdx - 1 + i) % (tasks.size() - 1));
                if (!tasks[idx].found) {
                    targetIdx = idx;
                    break;
                }
            }

            bool foundAny = false;
            if (targetIdx != 0) {
                nextIdx = (targetIdx % (tasks.size() - 1)) + 1;

                if (targetIdx == 2) {
                    if (g_shortcutHookActive) {
                        uintptr_t tempAddr = g_tempShortcutAddress.load();
                        if (tempAddr != 0) {
                            int32_t tempVal = g_tempShortcutValue.load();
                            if (tempVal == -1 || tempVal == 0 || tempVal == 1 || tempVal == 2 || tempVal == 3 || tempVal == 4) {
                                DllLog("[SUCCESS] Hook fired! Verified ShortcutMenu address: 0x%llX (value: %d). Hook removed.", tempAddr - 128, tempVal);
                                // Correct!
                                tasks[2].found = true;
                                g_addrShortcutMenu = tempAddr - 128;
                                if (g_pSharedMemory) {
                                    g_pSharedMemory->m_statusAddrShortcutMenu = g_addrShortcutMenu;
                                }
                                RemoveShortcutHook();
                                foundAny = true;
                            } else {
                                DllLog("[WARNING] Hook fired on incorrect value %d at address 0x%llX. Ignoring and waiting.", tempVal, tempAddr);
                                // Incorrect target address. Reset and wait for another write.
                                g_tempShortcutAddress = 0;
                                g_tempShortcutValue = 0;
                            }
                        } else {
                            static int waitCounter = 0;
                            if (waitCounter++ % 5 == 0) {
                                DllLog("[INFO] ShortcutMenu detour hook is active. Waiting for game write...");
                            }
                        }
                    } else {
                        DllLog("[INFO] Scanning for ShortcutMenu instruction pattern...");
                        Pattern pat = ParseAOB(tasks[2].patternStr);
                        uintptr_t foundAddress = 0;
                        if (ScanProcessAOB(pat, tasks[2].isCode, foundAddress)) {
                            DllLog("[SUCCESS] Found ShortcutMenu instruction at 0x%llX. Setting up detour hook...", foundAddress);
                            tasks[2].address = foundAddress;
                            if (SetupShortcutHook(foundAddress)) {
                                DllLog("[SUCCESS] Detour hook set up successfully. Waiting for game write...");
                            } else {
                                DllLog("[ERROR] Failed to set up detour hook for ShortcutMenu.");
                            }
                        } else {
                            DllLog("[WARNING] ShortcutMenu instruction pattern not found. Retrying in 1s...");
                        }
                    }
                } else {
                    if (targetIdx == 1) {
                        DllLog("[INFO] Scanning for Magne Target Sig...");
                    } else if (targetIdx == 3) {
                        DllLog("[INFO] Scanning for MenuState...");
                    }
                    Pattern pat = ParseAOB(tasks[targetIdx].patternStr);
                    uintptr_t foundAddress = 0;
                    if (ScanProcessAOB(pat, tasks[targetIdx].isCode, foundAddress)) {
                        tasks[targetIdx].address = foundAddress;
                        tasks[targetIdx].found = true;
                        foundAny = true;

                        if (targetIdx == 1) {
                            DllLog("[SUCCESS] Found Magne Target Sig at 0x%llX. Detour hooks injected.", foundAddress);
                        } else if (targetIdx == 3) {
                            DllLog("[SUCCESS] Found MenuState at 0x%llX.", foundAddress);
                        }

                        // Write to shared memory immediately!
                        if (g_pSharedMemory) {
                            switch (targetIdx) {
                                case 1: g_pSharedMemory->m_statusAddrMagneTarget  = foundAddress; break;
                                case 3: g_pSharedMemory->m_statusAddrMenuState    = foundAddress; break;
                            }
                        }

                        // Assign to global variable immediately!
                        switch (targetIdx) {
                            case 1: g_addrMagneTarget  = foundAddress; break;
                            case 3: g_addrMenuState    = foundAddress; break;
                        }

                        // Initialize magnesis detour patches when MagneTarget is found
                        if (targetIdx == 1) {
                            EnterCriticalSection(&g_patchCS);
                            if (!g_magnePatchesInitialized) {
                                g_magneDetourPatch = { foundAddress + 0x40, 15, {}, false };
                                g_magneYPatch      = { foundAddress + 0xBA,  7, {}, false };
                                g_magneZPatch      = { foundAddress + 0xCE,  7, {}, false };

                                g_magneDetourPatch.Backup();
                                g_magneYPatch.Backup();
                                g_magneZPatch.Backup();
                                
                                g_magnesisXWriterReturn = foundAddress + 0x40 + 15;
                                g_magneDetourPatch.InjectDetour((uintptr_t)&AsmMagnesisXWriter);

                                g_magnePatchesInitialized = true;
                                if (g_pSharedMemory) {
                                    g_pSharedMemory->m_patchMagneDetourActive = true;
                                }
                            }
                            LeaveCriticalSection(&g_patchCS);
                        }
                    } else {
                        if (targetIdx == 1) {
                            DllLog("[WARNING] Magne Target Sig not found. Retrying in 1s...");
                        } else if (targetIdx == 3) {
                            DllLog("[WARNING] MenuState not found. Retrying in 1s...");
                        }
                    }
                }
            }

            allOtherFound = true;
            for (size_t i = 1; i < tasks.size(); ++i) {
                if (!tasks[i].found) {
                    allOtherFound = false;
                    break;
                }
            }

            if (g_pSharedMemory) {
                g_pSharedMemory->m_statusAddrGameRomCamera = g_addrGameRomCamera.load();
                g_pSharedMemory->m_statusAddrMagneTarget   = g_addrMagneTarget.load();
                g_pSharedMemory->m_statusAddrShortcutMenu  = g_addrShortcutMenu.load();
                g_pSharedMemory->m_statusAddrMenuState     = g_addrMenuState.load();

                EnterCriticalSection(&g_patchCS);
                g_pSharedMemory->m_patchMagneDetourActive = g_magnePatchesInitialized && g_magneDetourPatch.active;
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

        // SendInput can fail under UIPI; log to debug output since this runs
        // inside the DLL where we have no console.
        UINT sent = SendInput(1, &input, sizeof(INPUT));
        if (sent != 1) {
            OutputDebugStringW(L"[Mousecam DLL] SendInput failed — possible UIPI block.\n");
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

        StartMouseHook();

        while (g_cameraControlRunning) {
            if (g_pSharedMemory && g_pSharedMemory->m_reqShutdown) {
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

            bool f2_pressed = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
            if (f2_pressed && !last_f2_state) {
                if (gc_addr != 0) {
                    g_mousecamActive = !g_mousecamActive;
                    virt_cam_initialized = false;
                    
                    if (g_mousecamActive && is_foreground) {
                        POINT center = GetCemuWindowCenter(hwndFg);
                        SetCursorPos(center.x, center.y);
                    }
                }
            }
            last_f2_state = f2_pressed;

            if (g_mousecamActive && foreground_transition) {
                POINT center = GetCemuWindowCenter(hwndFg);
                SetCursorPos(center.x, center.y);
            }

            if (g_mousecamActive && is_foreground) {
                SetGlobalCursorVisibility(false);
            } else {
                SetGlobalCursorVisibility(true);
            }

            bool menu_active = false;
            if (g_addrMenuState != 0) {
                g_liveMenuState = ReadByte(g_addrMenuState + 96);
                if (g_pSharedMemory) {
                    g_pSharedMemory->m_teleLiveMenuState = g_liveMenuState;
                }
                menu_active = (g_liveMenuState == 2);
            }

            bool is_shortcut_open = false;
            if (g_addrShortcutMenu != 0) {
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

            // The JIT compiler periodically recompiles code paths, which can
            // overwrite our injected detour bytes (jmp [AsmMagnesisXWriter]).
            // We poll the first byte of the detour site every frame and
            // re-inject if it's been stomped back to the original instruction.
            if (g_magnePatchesInitialized && g_magneDetourPatch.address != 0) {
                uint8_t currentByte = 0;
                SIZE_T bytesRead = 0;
                if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)g_magneDetourPatch.address, &currentByte, 1, &bytesRead)) {
                    if (bytesRead == 1 && currentByte != 0xFF) {
                        // The detour was overwritten! Re-inject it!
                        g_magneDetourPatch.active = false; // Force it to allow reinjection
                        g_magneDetourPatch.InjectDetour((uintptr_t)&AsmMagnesisXWriter);
                        if (magnesis_mode) {
                            g_magneYPatch.active = false;
                            g_magneZPatch.active = false;
                            g_magneYPatch.ApplyNop();
                            g_magneZPatch.ApplyNop();
                        }
                    }
                }
            }
            static bool last_magnesis_mode = false;
            if (magnesis_mode != last_magnesis_mode) {
                last_magnesis_mode = magnesis_mode;
                g_magnesisEnabled = magnesis_mode ? 1 : 0;

                EnterCriticalSection(&g_patchCS);
                if (g_magnePatchesInitialized) {
                    if (magnesis_mode && !g_magneYPatch.active) {
                        g_magneYPatch.ApplyNop();
                        g_magneZPatch.ApplyNop();
                    } else if (!magnesis_mode && g_magneYPatch.active) {
                        g_magneYPatch.Restore();
                        g_magneZPatch.Restore();
                        g_magneIdealBase = 0;
                        magne_initialized = false;
                    }
                }
                LeaveCriticalSection(&g_patchCS);
            }

            bool should_nop = g_mousecamActive && !magnesis_mode && !menu_active && !is_shortcut_open;
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
                    if (active_menu != ScrollMenuType::None) {
                        uint16_t dpad_left_key = g_pSharedMemory ? g_pSharedMemory->m_cfgDpadLeftKey : 0;
                        uint16_t dpad_right_key = g_pSharedMemory ? g_pSharedMemory->m_cfgDpadRightKey : 0;
                        if (dpad_left_key != 0) { CemuKeyInjector_SendKey(dpad_left_key, true); }
                        if (dpad_right_key != 0) { CemuKeyInjector_SendKey(dpad_right_key, true); }
                        active_menu = ScrollMenuType::None;
                    }
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

                if (g_addrShortcutMenu != 0) {
                    g_liveShortcutMenu = ReadInt32BE(g_addrShortcutMenu + 128);
                    if (g_pSharedMemory) {
                        g_pSharedMemory->m_teleLiveShortcutMenu = g_liveShortcutMenu;
                    }
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

                        float d_theta = dx * sens_x * 5.0f;
                        float dy_world = -dy * sens_y * 42.5f;

                        if (d_theta != 0.0f || dy_world != 0.0f) {
                            float cos_t = cos(d_theta);
                            float sin_t = sin(d_theta);
                            float new_off_x = magne_off_x * cos_t - magne_off_z * sin_t;
                            float new_off_z = magne_off_x * sin_t + magne_off_z * cos_t;
                            magne_off_y += dy_world;
                            magne_off_x = new_off_x;
                            magne_off_z = new_off_z;
                        }

                        float v_clamp = 15.0f;
                        if (magne_off_y < -v_clamp) magne_off_y = -v_clamp;
                        if (magne_off_y > v_clamp) magne_off_y = v_clamp;

                        int scroll = g_scrollDelta.exchange(0);
                        float h_clamp_max = 22.0f;
                        if (scroll != 0) {
                            float h_dist = sqrt(magne_off_x * magne_off_x + magne_off_z * magne_off_z);
                            if (h_dist > 0.01f) {
                                float new_h_dist = h_dist + (static_cast<float>(scroll) * 0.5f / 120.0f);
                                if (new_h_dist < 2.0f) new_h_dist = 2.0f;
                                if (new_h_dist > h_clamp_max) new_h_dist = h_clamp_max;
                                float scale = new_h_dist / h_dist;
                                magne_off_x *= scale;
                                magne_off_z *= scale;
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
                            if (scroll_helper) {
                                int scroll = g_scrollDelta.exchange(0);
                                if (is_main_menu_open || (is_shortcut_open && active_menu == ScrollMenuType::None)) {
                                    if (scroll != 0) {
                                        scroll_accumulator += scroll;
                                    }
                                    if (abs(scroll_accumulator) >= 120) {
                                        int notches = abs(scroll_accumulator) / 120;
                                        int sign = scroll_accumulator > 0 ? 1 : -1;
                                        scroll_accumulator -= sign * notches * 120;

                                        uint16_t rstick_left_key = g_pSharedMemory ? g_pSharedMemory->m_cfgRstickLeftKey : 0;
                                        uint16_t rstick_right_key = g_pSharedMemory ? g_pSharedMemory->m_cfgRstickRightKey : 0;
                                        uint16_t target_key = (sign > 0) ? rstick_right_key : rstick_left_key;
                                        if (target_key != 0) {
                                            for (int n = 0; n < notches; ++n) {
                                                CemuKeyInjector_SendKey(target_key, false);
                                                Sleep(15);
                                                CemuKeyInjector_SendKey(target_key, true);
                                                Sleep(15);
                                            }
                                        }
                                    }
                                } else {
                                    uint16_t dpad_left_key = g_pSharedMemory ? g_pSharedMemory->m_cfgDpadLeftKey : 0;
                                    uint16_t dpad_right_key = g_pSharedMemory ? g_pSharedMemory->m_cfgDpadRightKey : 0;
                                    uint16_t rstick_left_key = g_pSharedMemory ? g_pSharedMemory->m_cfgRstickLeftKey : 0;
                                    uint16_t rstick_right_key = g_pSharedMemory ? g_pSharedMemory->m_cfgRstickRightKey : 0;

                                    if (scroll != 0) {
                                        scroll_accumulator += scroll;
                                    }

                                    if (abs(scroll_accumulator) >= 120) {
                                        int notches = abs(scroll_accumulator) / 120;
                                        int sign = scroll_accumulator > 0 ? 1 : -1;
                                        scroll_accumulator -= sign * notches * 120;

                                        if (active_menu == ScrollMenuType::None) {
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
                                        } else {
                                            menu_hold_timer = std::chrono::steady_clock::now();
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

                                    if (active_menu != ScrollMenuType::None) {
                                        auto elapsed_sec = std::chrono::duration<float>(std::chrono::steady_clock::now() - menu_hold_timer).count();
                                        if (elapsed_sec > 0.5f) {
                                            switch (active_menu) {
                                                case ScrollMenuType::Left:
                                                    if (dpad_left_key != 0) { CemuKeyInjector_SendKey(dpad_left_key, true); }
                                                    break;
                                                case ScrollMenuType::Right:
                                                    if (dpad_right_key != 0) { CemuKeyInjector_SendKey(dpad_right_key, true); }
                                                    break;
                                            }
                                            active_menu = ScrollMenuType::None;
                                            scroll_accumulator = 0;
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

                    orbit_pitch = (std::max)(-1.5f, (std::min)(1.5f, orbit_pitch));

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

                    float current_radius = 5.5f;
                    bool full_orbit = (g_pSharedMemory && g_pSharedMemory->m_cfgFullOrbitCamera);
                    if (!full_orbit && current_orbit_pitch > 0.18f) {
                        current_radius += (current_orbit_pitch - 0.18f) * 5.5f;
                    }

                    float horizontal_r = current_radius * cos(current_orbit_pitch);
                    vcam_pos_x = pivot_x + horizontal_r * sin(current_orbit_angle);
                    vcam_pos_y = pivot_y + current_radius * sin(current_orbit_pitch);
                    vcam_pos_z = pivot_z + horizontal_r * cos(current_orbit_angle);

                    if (g_pSharedMemory) {
                        g_pSharedMemory->m_telePivotX = pivot_x;
                        g_pSharedMemory->m_telePivotY = pivot_y;
                        g_pSharedMemory->m_telePivotZ = pivot_z;
                        
                        if (magnesis_mode && magne_ideal_base != 0) {
                            g_pSharedMemory->m_teleMagneTargetX = ReadFloatBE(magne_ideal_base);
                            g_pSharedMemory->m_teleMagneTargetY = ReadFloatBE(magne_ideal_base + 4);
                            g_pSharedMemory->m_teleMagneTargetZ = ReadFloatBE(magne_ideal_base + 8);
                        } else {
                            g_pSharedMemory->m_teleMagneTargetX = 0.0f;
                            g_pSharedMemory->m_teleMagneTargetY = 0.0f;
                            g_pSharedMemory->m_teleMagneTargetZ = 0.0f;
                        }
                    }

                    if (!magnesis_mode && !menu_active) {
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
                        // Read back what's in camera memory and compare to what we wrote.
                        // If the game stomped our values, a new writer appeared (JIT recompile,
                        // new code path, etc.). Arm the guard so the VEH identifies it.
                        if (has_written_once && g_writerHuntActive) {
                            float cur_x = ReadFloatBE(g_addrGameRomCamera + 0x550);
                            float cur_y = ReadFloatBE(g_addrGameRomCamera + 0x554);
                            float cur_z = ReadFloatBE(g_addrGameRomCamera + 0x558);
                            if (cur_x != last_written_x || cur_y != last_written_y || cur_z != last_written_z) {
                                // Overwrite detected — hunt for the next ~40ms
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

        g_hMapFile = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            sizeof(SharedMemoryLayout),
            L"Local\\BotwMousecamSharedMemory"
        );

        if (g_hMapFile) {
            g_pSharedMemory = (SharedMemoryLayout*)MapViewOfFile(
                g_hMapFile,
                FILE_MAP_ALL_ACCESS,
                0,
                0,
                sizeof(SharedMemoryLayout)
            );

            if (g_pSharedMemory) {
                // Zero the memory first
                memset(g_pSharedMemory, 0, sizeof(SharedMemoryLayout));

                g_pSharedMemory->m_dllBaseAddr = (uint64_t)hModule;
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

        DeleteCriticalSection(&g_patchCS);
        DeleteCriticalSection(&g_writerCS);

        if (g_pSharedMemory) {
            UnmapViewOfFile(g_pSharedMemory);
            g_pSharedMemory = nullptr;
        }
        if (g_hMapFile) {
            CloseHandle(g_hMapFile);
            g_hMapFile = nullptr;
        }
    }

} // namespace Mod
