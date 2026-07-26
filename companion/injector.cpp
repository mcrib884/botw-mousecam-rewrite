#include <Windows.h>
#include "injector.h"
#include <TlHelp32.h>

namespace Injector {

struct SafeHandle {
    HANDLE handle = nullptr;
    SafeHandle() = default;
    explicit SafeHandle(HANDLE h) : handle(h) {}
    ~SafeHandle() { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }

    SafeHandle(const SafeHandle&) = delete;
    SafeHandle& operator=(const SafeHandle&) = delete;
    SafeHandle(SafeHandle&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    SafeHandle& operator=(SafeHandle&& other) noexcept {
        if (this != &other) {
            if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    operator HANDLE() const { return handle; }
    bool IsValid() const { return handle != nullptr && handle != INVALID_HANDLE_VALUE; }
};

struct SafeRemoteMemory {
    HANDLE hProcess = nullptr;
    void* address = nullptr;

    SafeRemoteMemory(HANDLE proc, void* addr) : hProcess(proc), address(addr) {}
    ~SafeRemoteMemory() {
        if (hProcess && address) {
            VirtualFreeEx(hProcess, address, 0, MEM_RELEASE);
        }
    }

    SafeRemoteMemory(const SafeRemoteMemory&) = delete;
    SafeRemoteMemory& operator=(const SafeRemoteMemory&) = delete;
};

DWORD FindProcessByName(const std::wstring& name) {
    SafeHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap.IsValid()) {
        return 0;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name.c_str()) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }

    return pid;
}

bool InjectDLL(DWORD pid, const std::wstring& dllPath) {
    SafeHandle hProc(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid));
    if (!hProc.IsValid()) {
        return false;
    }

    SIZE_T pathSize = (dllPath.size() + 1) * sizeof(wchar_t);
    void* rawRemoteMem = VirtualAllocEx(hProc, nullptr, pathSize,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!rawRemoteMem) {
        return false;
    }
    SafeRemoteMemory remoteMem(hProc, rawRemoteMem);

    if (!WriteProcessMemory(hProc, remoteMem.address, dllPath.c_str(), pathSize, nullptr)) {
        return false;
    }

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibW = GetProcAddress(hK32, "LoadLibraryW");

    SafeHandle hThread(CreateRemoteThread(hProc, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibW),
        remoteMem.address, 0, nullptr));

    if (!hThread.IsValid()) {
        return false;
    }

    WaitForSingleObject(hThread, 5000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    return (exitCode != 0);
}

bool EjectDLL(DWORD pid, const std::wstring& dllPath) {
    std::wstring moduleName = GetFileName(dllPath);

    SafeHandle hProc(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid));
    if (!hProc.IsValid()) {
        return false;
    }

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC freeLib = GetProcAddress(hK32, "FreeLibrary");
    if (!freeLib) {
        return false;
    }

    bool success = false;
    int safetyLimit = 2;
    while (safetyLimit-- > 0) {
        SafeHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        if (!snap.IsValid()) {
            break;
        }

        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        HMODULE hMod = nullptr;

        if (Module32FirstW(snap, &me)) {
            do {
                if (_wcsicmp(me.szModule, moduleName.c_str()) == 0) {
                    hMod = me.hModule;
                    break;
                }
            } while (Module32NextW(snap, &me));
        }
        if (!hMod) {
            success = true;
            break;
        }

        SafeHandle hThread(CreateRemoteThread(hProc, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(freeLib),
            hMod, 0, nullptr));

        if (!hThread.IsValid()) {
            break;
        }

        DWORD waitResult = WaitForSingleObject(hThread, 500);
        if (waitResult == WAIT_TIMEOUT) {
            break;
        }

        DWORD exitCode = 0;
        GetExitCodeThread(hThread, &exitCode);
        if (exitCode == 0) {
            break;
        }

        Sleep(20);
    }

    return success;
}

bool IsModuleLoaded(DWORD pid, const std::wstring& moduleName) {
    SafeHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snap.IsValid()) {
        return false;
    }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    bool loaded = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, moduleName.c_str()) == 0) {
                loaded = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    return loaded;
}

std::wstring GetFileName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

} // namespace Injector
