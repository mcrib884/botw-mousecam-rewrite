#include <Windows.h>
#include "injector.h"
#include <TlHelp32.h>

namespace Injector {

DWORD FindProcessByName(const std::wstring& name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
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

    CloseHandle(snap);
    return pid;
}

bool InjectDLL(DWORD pid, const std::wstring& dllPath) {
    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc) {
        return false;
    }

    SIZE_T pathSize = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(hProc, nullptr, pathSize,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        CloseHandle(hProc);
        return false;
    }

    if (!WriteProcessMemory(hProc, remoteMem, dllPath.c_str(), pathSize, nullptr)) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibW = GetProcAddress(hK32, "LoadLibraryW");

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibW),
        remoteMem, 0, nullptr);

    if (!hThread) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    WaitForSingleObject(hThread, 5000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return (exitCode != 0);
}

bool EjectDLL(DWORD pid, const std::wstring& dllPath) {
    std::wstring moduleName = GetFileName(dllPath);

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc) {
        return false;
    }

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC freeLib = GetProcAddress(hK32, "FreeLibrary");
    if (!freeLib) {
        CloseHandle(hProc);
        return false;
    }

    bool success = false;
    int safetyLimit = 15;
    while (safetyLimit-- > 0) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) {
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
        CloseHandle(snap);
        if (!hMod) {
            success = true;
            break;
        }

        HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(freeLib),
            hMod, 0, nullptr);

        if (!hThread) {
            break;
        }

        WaitForSingleObject(hThread, 5000);
        DWORD exitCode = 0;
        GetExitCodeThread(hThread, &exitCode);
        CloseHandle(hThread);
        if (exitCode == 0) {
            break;
        }

        Sleep(50);
    }

    CloseHandle(hProc);
    return success;
}

bool IsModuleLoaded(DWORD pid, const std::wstring& moduleName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) {
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
    CloseHandle(snap);
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
