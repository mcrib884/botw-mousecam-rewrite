#define NOMINMAX
#include <Windows.h>
#include "shared_memory_manager.h"

SharedMemoryManager g_sharedMemory;

bool SharedMemoryManager::Open(const wchar_t* name) {
    if (m_layout) return true;
    m_hFile = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(SharedMemoryLayout),
        name
    );
    if (!m_hFile) {
        m_hFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    }
    if (!m_hFile) {
        return false;
    }
    m_layout = static_cast<SharedMemoryLayout*>(MapViewOfFile(m_hFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemoryLayout)));
    if (!m_layout) {
        CloseHandle(m_hFile);
        m_hFile = nullptr;
        return false;
    }
    return true;
}

void SharedMemoryManager::Close() {
    if (m_layout) {
        UnmapViewOfFile(m_layout);
        m_layout = nullptr;
    }
    if (m_hFile) {
        CloseHandle(m_hFile);
        m_hFile = nullptr;
    }
}

bool MapSharedMemory() {
    return g_sharedMemory.Open(L"Local\\BotwMousecamSharedMemory");
}

void UnmapSharedMemory() {
    g_sharedMemory.Close();
}