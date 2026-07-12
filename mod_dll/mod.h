#pragma once

// Include Windows.h for HMODULE and base Win32 types needed by consumers
// (dllmain.cpp, mod.cpp). For a 2-file DLL this adds zero meaningful compile
// cost and avoids transitive-include fragility with TlHelp32.h and friends.
#include <Windows.h>
#include <cstdint>
#include "shared_memory_layout.h"

namespace Mod {
    // SharedMemoryLayout is defined in shared/include/shared_memory_layout.h
    // and included by both the companion and the DLL to prevent layout drift.

    void Init(HMODULE hModule);
    void Shutdown();
}
