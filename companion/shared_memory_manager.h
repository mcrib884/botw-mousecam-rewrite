#pragma once
// shared_memory_manager.h — RAII wrapper over CreateFileMappingW/MapViewOfFile.
// Extracted from companion/main.cpp. Single shared instance g_sharedMemory.

#include <Windows.h>
#include "shared_memory_layout.h"

// SharedMemoryLayout is defined in shared/include/shared_memory_layout.h —
// single source of truth shared with mod_dll to prevent layout drift.

class SharedMemoryManager {
public:
    SharedMemoryManager() = default;
    ~SharedMemoryManager() { Close(); }

    SharedMemoryManager(const SharedMemoryManager&) = delete;
    SharedMemoryManager& operator=(const SharedMemoryManager&) = delete;

    bool Open(const wchar_t* name);
    void Close();
    SharedMemoryLayout* GetLayout() const { return m_layout; }

private:
    HANDLE m_hFile = nullptr;
    SharedMemoryLayout* m_layout = nullptr;
};

// Single instance — defined in shared_memory_manager.cpp
extern SharedMemoryManager g_sharedMemory;

// Convenience macro preserved from the original file. Use g_pSharedMemory
// exactly as before the split; it evaluates to g_sharedMemory.GetLayout().
#define g_pSharedMemory (g_sharedMemory.GetLayout())

// Helpers used by injector_ops + ui_paint.
bool MapSharedMemory();
void UnmapSharedMemory();