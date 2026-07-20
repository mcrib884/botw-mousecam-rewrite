#pragma once
// string_utils.h — UTF-8 <-> wide string conversion (TU: string_utils.cpp)
// Extracted from companion/main.cpp. No global state, no dependencies beyond Win32.

#include <string>

// TD3: UTF-8 conversion helpers using WideCharToMultiByte instead of static_cast loops.
std::string WstrToUtf8(const std::wstring& wstr);
std::wstring Utf8ToWstr(const std::string& str);