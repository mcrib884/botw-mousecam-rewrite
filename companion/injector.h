#pragma once
#include <string>

namespace Injector {
    DWORD FindProcessByName(const std::wstring& name);
    bool  InjectDLL(DWORD pid, const std::wstring& dllPath);
    bool  EjectDLL(DWORD pid, const std::wstring& dllPath);
    bool  IsModuleLoaded(DWORD pid, const std::wstring& moduleName);
    std::wstring GetFileName(const std::wstring& path);
}
