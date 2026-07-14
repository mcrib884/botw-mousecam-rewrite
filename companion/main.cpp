#define NOMINMAX
#include <Windows.h>
#include <richedit.h>
#include <gdiplus.h>
using namespace Gdiplus; // saves prefixing every GDI+ type in the UI drawing layer below
#include <shlobj.h>
#include <commctrl.h>
#include <commdlg.h>
#include <TlHelp32.h>
#include <shellscalingapi.h>
#include <string>
#include <cstring>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include "resource.h"
#include "injector.h"
#include "shared_memory_layout.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")

// TD3: UTF-8 conversion helpers using WideCharToMultiByte instead of static_cast loops
static std::string WstrToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &result[0], len, nullptr, nullptr);
    return result;
}

static std::wstring Utf8ToWstr(const std::string& str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring result(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &result[0], len);
    return result;
}

// ============================================================================
// Companion application — configuration UI and injection launcher.
//
// Why all the global state (g_* pattern):
// This is a single-window Win32 GUI app — there is exactly one window, one
// target process, and one config. A singleton App class would add ceremony
// without adding safety. The g_ prefix distinguishes globals from locals.
//
// Why manual JSON parsing instead of a library:
// The config is ~10 keys with simple types. Pulling in nlohmann/json or
// similar would add a build dependency and hundreds of KB to the binary
// for something that's 60 lines of string scanning. Zero-dependency keeps
// distribution trivial for end users.
//
// Why the CemuKeyInjector class exists in both companion and DLL (I6):
// The companion needs key injection during the reinject/reload flow (testing
// bindings before the DLL is active). The DLL needs it at runtime during
// gameplay. They run in different processes and each manages its own input
// state, so they can't share a single instance. The duplicated SendKey in both
// is intentional — same rationale applies.
//
// Why CRITICAL_SECTION over std::mutex:
// On Windows, CRITICAL_SECTION is a user-mode spinlock — faster than std::mutex
// (which wraps a kernel object) for short-held locks in a UI + injection tool.
// ============================================================================

static HWND g_hDlg = nullptr;
static HWND g_hConsoleEdit = nullptr;
static HANDLE g_hTargetProcess = nullptr;
static DWORD g_targetPid = 0;
static bool g_targetInjected = false;

// SharedMemoryLayout defined in shared/include/shared_memory_layout.h —
// single source of truth shared with mod_dll to prevent layout drift.

static HANDLE g_hSharedMemoryFile = nullptr;
static SharedMemoryLayout* g_pSharedMemory = nullptr;

static bool MapSharedMemory() {
    if (g_pSharedMemory) return true;
    g_hSharedMemoryFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"Local\\BotwMousecamSharedMemory");
    if (!g_hSharedMemoryFile) {
        return false;
    }
    g_pSharedMemory = (SharedMemoryLayout*)MapViewOfFile(g_hSharedMemoryFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemoryLayout));
    if (!g_pSharedMemory) {
        CloseHandle(g_hSharedMemoryFile);
        g_hSharedMemoryFile = nullptr;
        return false;
    }
    return true;
}

static void UnmapSharedMemory() {
    if (g_pSharedMemory) {
        UnmapViewOfFile(g_pSharedMemory);
        g_pSharedMemory = nullptr;
    }
    if (g_hSharedMemoryFile) {
        CloseHandle(g_hSharedMemoryFile);
        g_hSharedMemoryFile = nullptr;
    }
}






struct TargetWndData {
    DWORD pid;
    HWND hWnd;
};

static BOOL CALLBACK FindTargetWindowProc(HWND hWnd, LPARAM lParam) {
    auto data = reinterpret_cast<TargetWndData*>(lParam);
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

static POINT GetWindowCenter(HWND hWnd) {
    RECT rect;
    GetWindowRect(hWnd, &rect);
    POINT pt;
    pt.x = rect.left + (rect.right - rect.left) / 2;
    pt.y = rect.top + (rect.bottom - rect.top) / 2;
    return pt;
}

// TD5: Thread safety — these globals are read/written ONLY on the UI thread via
// WM_TIMER → UpdateTelemetryGui(). The shared memory (g_pSharedMemory) is written by
// the DLL in a different process — that's safe across process boundaries. No
// cross-thread access within the companion.
static uintptr_t g_addrGameRomCamera = 0;
static uintptr_t g_addrMagneTarget = 0;
static uintptr_t g_addrShortcutMenu = 0;
static uintptr_t g_addrMenuState = 0;

static float g_liveCamPosX = 0.0f;
static float g_liveCamPosY = 0.0f;
static float g_liveCamPosZ = 0.0f;
static float g_liveCamFocX = 0.0f;
static float g_liveCamFocY = 0.0f;
static float g_liveCamFocZ = 0.0f;
static float g_liveCamFOV = 0.0f;
static int32_t g_liveShortcutMenu = -1;
static uint8_t g_liveMenuState = 1;

static bool g_mousecamActive = false;
static uint32_t g_writersFound = 0;
static bool g_magneDetourActive = false;
static std::wstring GetCompanionDllPath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring exePath(path);
    size_t pos = exePath.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return L"botw-mousecam-rewrite.dll";
    }
    return exePath.substr(0, pos + 1) + L"botw-mousecam-rewrite.dll";
}

enum class ScrollMenuType {
    None,
    Left,
    Right
};

static void LogToConsole(const wchar_t* format, ...);

struct AppConfig {
    bool magnesis_enabled = false;
    bool scroll_helper = true; // Default to true!
    std::string controller_profile_path = "";
    uint32_t mouse_bindings[5] = {0, 0, 0, 0, 0};
    float sensitivity_x = 1.0f;
    float sensitivity_y = 1.0f;
    bool use_independent_sens = false;
    bool full_orbit_camera = false;
    std::string cemu_path_override;
    bool theme_initialized = false;
    bool use_light_theme = false;
};

static AppConfig g_config;

static std::string extract_tag(const std::string& s, const std::string& open, const std::string& close, size_t start_pos = 0) {
    size_t start = s.find(open, start_pos);
    if (start == std::string::npos) return "";
    start += open.length();
    size_t end = s.find(close, start);
    if (end == std::string::npos) return "";
    return s.substr(start, end - start);
}

static bool IsWindowsLightTheme() {
    DWORD data = 0;
    DWORD dataSize = sizeof(DWORD);
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&data, &dataSize);
        RegCloseKey(hKey);
    }
    return data != 0;
}

struct ThemeColors {
    Color bg;
    Color panel;
    Color border;
    Color accent;
    Color text;
    Color textMuted;
    Color success;
    Color error;
    Color consoleBg;
};

#include <dwmapi.h>
extern HWND g_hWnd;
static ThemeColors g_theme;

static Color LerpColor(Color a, Color b, float t) {
    return Color(255, a.GetR() + (b.GetR() - a.GetR())*t, a.GetG() + (b.GetG() - a.GetG())*t, a.GetB() + (b.GetB() - a.GetB())*t);
}

static float g_animTheme = -1.0f;
static void ApplyTheme() {
    if (g_hWnd) {
        BOOL dark = !g_config.use_light_theme;
        DwmSetWindowAttribute(g_hWnd, 20, &dark, sizeof(dark));
    }
    if (g_hConsoleEdit) {
        COLORREF bg = g_config.use_light_theme ? RGB(248, 248, 248) : RGB(10, 12, 16);
        SendMessageW(g_hConsoleEdit, EM_SETBKGNDCOLOR, 0, bg);
        InvalidateRect(g_hConsoleEdit, nullptr, TRUE);
    }
    ThemeColors lightTheme = {
        Color(255, 245, 245, 247), Color(255, 255, 255, 255), Color(255, 220, 220, 225),
        Color(255, 0, 102, 204), Color(255, 30, 30, 35), Color(255, 120, 120, 125),
        Color(255, 30, 150, 60), Color(255, 220, 40, 40), Color(255, 248, 248, 248)
    };
    ThemeColors darkTheme = {
        Color(255, 13, 17, 23), Color(255, 22, 27, 34), Color(255, 48, 54, 61),
        Color(255, 47, 129, 247), Color(255, 201, 209, 217), Color(255, 139, 148, 158),
        Color(255, 35, 134, 54), Color(255, 218, 54, 51), Color(255, 10, 12, 16)
    };
    float t = g_animTheme == -1.0f ? (g_config.use_light_theme ? 0.0f : 1.0f) : g_animTheme;
    g_theme.bg = LerpColor(lightTheme.bg, darkTheme.bg, t);
    g_theme.panel = LerpColor(lightTheme.panel, darkTheme.panel, t);
    g_theme.border = LerpColor(lightTheme.border, darkTheme.border, t);
    g_theme.accent = LerpColor(lightTheme.accent, darkTheme.accent, t);
    g_theme.text = LerpColor(lightTheme.text, darkTheme.text, t);
    g_theme.textMuted = LerpColor(lightTheme.textMuted, darkTheme.textMuted, t);
    g_theme.success = LerpColor(lightTheme.success, darkTheme.success, t);
    g_theme.error = LerpColor(lightTheme.error, darkTheme.error, t);
    g_theme.consoleBg = LerpColor(lightTheme.consoleBg, darkTheme.consoleBg, t);
}

static std::string GetConfigPath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring wpath(path);
    size_t last_slash = wpath.find_last_of(L"\\/");
    if (last_slash != std::wstring::npos) {
        wpath = wpath.substr(0, last_slash + 1) + L"mousecam_config.json";
    } else {
        wpath = L"mousecam_config.json";
    }
    return WstrToUtf8(wpath); // TD3: proper UTF-8 conversion
}

static void SaveConfig() {
    std::string path = GetConfigPath();
    std::ofstream f(path);
    if (!f.is_open()) return;

    std::string escaped_path;
    for (char c : g_config.controller_profile_path) {
        if (c == '\\') {
            escaped_path += "\\\\";
        } else {
            escaped_path += c;
        }
    }

    f << "{\n";
    f << "  \"magnesis_enabled\": " << (g_config.magnesis_enabled ? "true" : "false") << ",\n";
    f << "  \"scroll_helper\": " << (g_config.scroll_helper ? "true" : "false") << ",\n";
    f << "  \"full_orbit_camera\": " << (g_config.full_orbit_camera ? "true" : "false") << ",\n";
    f << "  \"controller_profile_path\": \"" << escaped_path << "\",\n";
    
    std::string escaped_cemu_override;
    for (char c : g_config.cemu_path_override) {
        if (c == '\\') escaped_cemu_override += "\\\\";
        else escaped_cemu_override += c;
    }
    f << "  \"cemu_path_override\": \"" << escaped_cemu_override << "\",\n";
    f << "  \"mouse_bindings\": [";
    for (int i = 0; i < 5; ++i) {
        f << g_config.mouse_bindings[i];
        if (i < 4) f << ", ";
    }
    f << "],\n";
    f << "  \"sensitivity_x\": " << g_config.sensitivity_x << ",\n";
    f << "  \"sensitivity_y\": " << g_config.sensitivity_y << ",\n";
    f << "  \"use_independent_sens\": " << (g_config.use_independent_sens ? "true" : "false") << ",\n";
    f << "  \"theme_initialized\": " << (g_config.theme_initialized ? "true" : "false") << ",\n";
    f << "  \"use_light_theme\": " << (g_config.use_light_theme ? "true" : "false") << "\n";
    f << "}\n";
    f.close();
}

static std::string UnescapeJsonString(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == '\\' && i + 1 < s.length()) {
            if (s[i+1] == '\\') {
                r += '\\';
                i++;
            } else if (s[i+1] == '"') {
                r += '"';
                i++;
            } else if (s[i+1] == 'n') {
                r += '\n';
                i++;
            } else if (s[i+1] == 't') {
                r += '\t';
                i++;
            } else {
                r += s[i];
            }
        } else {
            r += s[i];
        }
    }
    return r;
}

static void LoadConfig() {
    std::string path = GetConfigPath();
    std::ifstream f(path);
    if (!f.is_open()) {
        SaveConfig();
        return;
    }

    std::stringstream buffer;
    buffer << f.rdbuf();
    std::string content = buffer.str();
    f.close();

    auto trim = [](std::string s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), s.end());
        return s;
    };

    auto extract_json_string = [&](const std::string& key) -> std::string {
        size_t key_pos = content.find("\"" + key + "\"");
        if (key_pos == std::string::npos) return "";
        size_t colon_pos = content.find(":", key_pos);
        if (colon_pos == std::string::npos) return "";
        size_t quote_start = content.find("\"", colon_pos);
        if (quote_start == std::string::npos) return "";
        // TD1: Handle escaped quotes (\") — don't stop at a quote preceded by backslash
        size_t quote_end = quote_start + 1;
        while (quote_end < content.size()) {
            quote_end = content.find("\"", quote_end);
            if (quote_end == std::string::npos) return "";
            // Check if this quote is escaped (preceded by odd number of backslashes)
            int backslash_count = 0;
            size_t check = quote_end;
            while (check > 0 && content[check - 1] == '\\') { backslash_count++; check--; }
            if (backslash_count % 2 == 0) break; // not escaped
            quote_end++;
        }
        if (quote_end == std::string::npos) return "";
        return content.substr(quote_start + 1, quote_end - quote_start - 1);
    };

    auto extract_json_bool = [&](const std::string& key, bool default_val) -> bool {
        size_t key_pos = content.find("\"" + key + "\"");
        if (key_pos == std::string::npos) return default_val;
        size_t colon_pos = content.find(":", key_pos);
        if (colon_pos == std::string::npos) return default_val;
        std::string val_part = trim(content.substr(colon_pos + 1, 15));
        if (val_part.rfind("true", 0) == 0) return true;
        if (val_part.rfind("false", 0) == 0) return false;
        return default_val;
    };

    auto extract_json_double = [&](const std::string& key, double default_val) -> double {
        size_t key_pos = content.find("\"" + key + "\"");
        if (key_pos == std::string::npos) return default_val;
        size_t colon_pos = content.find(":", key_pos);
        if (colon_pos == std::string::npos) return default_val;
        try {
            return std::stod(content.substr(colon_pos + 1));
        } catch (...) {
            LogToConsole(L"[WARNING] Config parse error for numeric value.");
            return default_val;
        }
    };

    g_config.magnesis_enabled = extract_json_bool("magnesis_enabled", false);
    g_config.scroll_helper = extract_json_bool("scroll_helper", true);
    g_config.full_orbit_camera = extract_json_bool("full_orbit_camera", false);
    g_config.controller_profile_path = UnescapeJsonString(extract_json_string("controller_profile_path"));
    g_config.cemu_path_override = UnescapeJsonString(extract_json_string("cemu_path_override"));

    // Sensitivity migration: handle old single-sens format vs new independent sens
    bool has_x = (content.find("\"sensitivity_x\"") != std::string::npos);
    if (has_x) {
        g_config.sensitivity_x = (float)extract_json_double("sensitivity_x", 1.0);
        g_config.sensitivity_y = (float)extract_json_double("sensitivity_y", 1.0);
        g_config.use_independent_sens = extract_json_bool("use_independent_sens", false);
    } else {
        float old_sens = (float)extract_json_double("sensitivity", 1.0);
        g_config.sensitivity_x = old_sens;
        g_config.sensitivity_y = old_sens;
        g_config.use_independent_sens = false;
    }

    // Theme initialization (unconditional — not gated behind sensitivity_x)
    g_config.theme_initialized = extract_json_bool("theme_initialized", false);
    g_config.use_light_theme = extract_json_bool("use_light_theme", false);
    if (!g_config.theme_initialized) {
        g_config.use_light_theme = IsWindowsLightTheme();
        g_config.theme_initialized = true;
        SaveConfig();
    }
    ApplyTheme();

    size_t array_pos = content.find("\"mouse_bindings\"");
    if (array_pos != std::string::npos) {
        size_t start_bracket = content.find("[", array_pos);
        size_t end_bracket = content.find("]", start_bracket);
        if (start_bracket != std::string::npos && end_bracket != std::string::npos) {
            std::string array_str = content.substr(start_bracket + 1, end_bracket - start_bracket - 1);
            std::stringstream ss(array_str);
            std::string item;
            int idx = 0;
            while (std::getline(ss, item, ',') && idx < 5) {
                try {
                    g_config.mouse_bindings[idx++] = std::stoul(trim(item));
                } catch (...) {
                    LogToConsole(L"[WARNING] Config parse error for numeric value.");
                }
            }
        }
    }
}

static std::wstring ExpandEnv(const std::wstring& s) {
    std::wstring r = s;
    wchar_t appdata[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
        size_t pos;
        while ((pos = r.find(L"%APPDATA%")) != std::wstring::npos) {
            r.replace(pos, 9, appdata);
        }
    }
    wchar_t userprofile[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", userprofile, MAX_PATH)) {
        size_t pos;
        while ((pos = r.find(L"%USERPROFILE%")) != std::wstring::npos) {
            r.replace(pos, 13, userprofile);
        }
    }
    return r;
}

static std::wstring ResolveProfilePath(const std::string& custom_path) {
    if (!g_config.cemu_path_override.empty()) {
        std::wstring override_path = Utf8ToWstr(g_config.cemu_path_override); // TD3
        override_path += L"\\controllerProfiles\\controller0.xml";
        if (GetFileAttributesW(override_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return override_path;
        }
    }
    std::wstring profile_path = L"";
    if (!custom_path.empty()) {
        std::wstring wcustom = Utf8ToWstr(custom_path); // TD3
        profile_path = ExpandEnv(wcustom);
    }

    if (profile_path.empty() || GetFileAttributesW(profile_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wchar_t exe_path[MAX_PATH];
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        std::wstring wpath(exe_path);
        size_t last_slash = wpath.find_last_of(L"\\/");
        if (last_slash != std::wstring::npos) {
            std::wstring local_path = wpath.substr(0, last_slash + 1) + L"controllerProfiles\\controller0.xml";
            if (GetFileAttributesW(local_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                profile_path = local_path;
            } else {
                std::wstring appdata_path = ExpandEnv(L"%APPDATA%\\Cemu\\controllerProfiles\\controller0.xml");
                if (GetFileAttributesW(appdata_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    profile_path = appdata_path;
                }
            }
        }
    }
    return profile_path;
}

class CemuKeyInjector {
public:
    std::wstring profile_path;
    uint32_t mouse_bindings[5] = {0, 0, 0, 0, 0};
    std::unordered_map<uint32_t, uint32_t> gamepad_to_key;
    bool prev_pressed[5] = {false, false, false, false, false};
    std::chrono::steady_clock::time_point press_time[5];
    bool press_time_valid[5] = {false, false, false, false, false};
    bool enabled = true;
    bool is_gamepad = false;

    void ReloadSettings() {
        ReleaseAll();
        LoadConfig();
        
        for (int i = 0; i < 5; ++i) {
            mouse_bindings[i] = g_config.mouse_bindings[i];
        }
        
        LoadCemuProfile(g_config.controller_profile_path);
    }

    void LoadCemuProfile(const std::string& custom_path) {
        profile_path = ResolveProfilePath(custom_path);
        if (profile_path.empty() || GetFileAttributesW(profile_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            LogToConsole(L"[INFO] Cemu profile not found - key mapping unavailable.");
            return;
        }

        std::ifstream f(profile_path);
        if (!f.is_open()) {
            LogToConsole(L"[INFO] Failed to open Cemu profile.");
            return;
        }

        std::stringstream buffer;
        buffer << f.rdbuf();
        std::string content = buffer.str();
        f.close();

        if (content.size() >= 3 && (unsigned char)content[0] == 0xEF && (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF) {
            content = content.substr(3);
        }

        gamepad_to_key.clear();

        std::string controller_type = extract_tag(content, "<type>", "</type>");
        if (controller_type.empty()) {
            controller_type = "Wii U Pro Controller";
        }
        
        std::string controller_type_lower = controller_type;
        std::transform(controller_type_lower.begin(), controller_type_lower.end(), controller_type_lower.begin(), ::tolower);
        is_gamepad = (controller_type_lower.find("gamepad") != std::string::npos);

        size_t pos = 0;
        std::string keyboard_block = "";
        while (true) {
            size_t ctrl_start = content.find("<controller>", pos);
            if (ctrl_start == std::string::npos) break;
            ctrl_start += 12;
            size_t ctrl_end = content.find("</controller>", ctrl_start);
            if (ctrl_end == std::string::npos) break;
            std::string block = content.substr(ctrl_start, ctrl_end - ctrl_start);
            std::string api_val = extract_tag(block, "<api>", "</api>");
            
            auto trim = [](std::string s) {
                s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                    return !std::isspace(ch);
                }));
                s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
                    return !std::isspace(ch);
                }).base(), s.end());
                return s;
            };
            std::string trimmed_api = trim(api_val);
            if (_stricmp(trimmed_api.c_str(), "Keyboard") == 0) {
                keyboard_block = block;
                break;
            }
            pos = ctrl_end + 13;
        }

        if (keyboard_block.empty()) {
            LogToConsole(L"[INFO] No Keyboard API section found in Cemu profile.");
            return;
        }

        std::string entries_section = extract_tag(keyboard_block, "<mappings>", "</mappings>");
        if (entries_section.empty()) {
            LogToConsole(L"[INFO] No <mappings> tag found within Keyboard section.");
            return;
        }

        auto trim = [](std::string s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
            s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
                return !std::isspace(ch);
            }).base(), s.end());
            return s;
        };

        size_t entry_pos = 0;
        int found = 0;
        while (true) {
            size_t end_pos = entries_section.find("</entry>", entry_pos);
            if (end_pos == std::string::npos) break;
            std::string entry = entries_section.substr(entry_pos, end_pos - entry_pos);
            
            std::string id_str = trim(extract_tag(entry, "<mapping>", "</mapping>"));
            std::string kc_str = trim(extract_tag(entry, "<button>", "</button>"));
            if (!id_str.empty() && !kc_str.empty()) {
                auto parse_u32 = [](const std::string& s) -> uint32_t {
                    if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
                        return std::stoul(s, nullptr, 16);
                    }
                    return std::stoul(s, nullptr, 10);
                };
                try {
                    uint32_t id = parse_u32(id_str);
                    uint32_t kc = parse_u32(kc_str);
                    gamepad_to_key[id] = kc;
                    found++;
                } catch (...) {
                    LogToConsole(L"[WARNING] Failed to parse Cemu profile entry.");
                }
            }
            entry_pos = end_pos + 8;
        }

        LogToConsole(L"[INFO] Loaded %d keyboard mappings from Cemu profile.", found);
    }

    static int MouseVk(int idx) {
        // Virtual-key codes for the 5 standard mouse buttons.
        // Used to translate our logical button indices (0-4) to GetAsyncKeyState VK codes.
        switch (idx) {
            case 0: return 0x01; // VK_LBUTTON
            case 1: return 0x02; // VK_RBUTTON
            case 2: return 0x04; // VK_MBUTTON
            case 3: return 0x05; // VK_XBUTTON1
            case 4: return 0x06; // VK_XBUTTON2
            default: return 0;
        }
    }

    void Update(HWND hCemuWnd) {
        if (!enabled) return;

        HWND hwndFg = GetForegroundWindow();
        if (hwndFg != hCemuWnd) {
            for (int i = 0; i < 5; ++i) {
                if (prev_pressed[i]) {
                    ReleaseKey(i);
                    prev_pressed[i] = false;
                    press_time_valid[i] = false;
                }
            }
            return;
        }

        for (int i = 0; i < 5; ++i) {
            uint32_t gpid = mouse_bindings[i];
            if (gpid == 0) continue;

            auto it = gamepad_to_key.find(gpid);
            if (it == gamepad_to_key.end()) continue;
            uint32_t keycode = it->second;

            bool down = (GetAsyncKeyState(MouseVk(i)) & 0x8000) != 0;

            if (down && !prev_pressed[i]) {
                SendKey(static_cast<uint16_t>(keycode), false);
                prev_pressed[i] = true;
                press_time[i] = std::chrono::steady_clock::now();
                press_time_valid[i] = true;
            } else if (!down && prev_pressed[i]) {
                bool can_release = true;
                if (press_time_valid[i]) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - press_time[i]
                    ).count();
                    // Enforce a 100 ms minimum hold time to prevent key bounce
                    // from being misinterpreted as a tap-then-release. Cemu's
                    // input polling runs at ~10 ms intervals so 100 ms guarantees
                    // the press is registered before we send the release.
                    if (elapsed < 100) can_release = false;
                }
                if (can_release) {
                    SendKey(static_cast<uint16_t>(keycode), true);
                    prev_pressed[i] = false;
                    press_time_valid[i] = false;
                }
            }
        }
    }

    void ReleaseKey(int idx) {
        uint32_t gpid = mouse_bindings[idx];
        if (gpid == 0) return;
        auto it = gamepad_to_key.find(gpid);
        if (it != gamepad_to_key.end()) {
            SendKey(static_cast<uint16_t>(it->second), true);
        }
    }

    void ReleaseAll() {
        for (int i = 0; i < 5; ++i) {
            if (prev_pressed[i]) {
                ReleaseKey(i);
                prev_pressed[i] = false;
                press_time_valid[i] = false;
            }
        }
    }

    static void SendKey(uint16_t keycode, bool up) {
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

        // SendInput can fail under UIPI if the target window has higher integrity.
        // The companion requests admin in its manifest so this is rare, but log it.
        UINT sent = SendInput(1, &input, sizeof(INPUT));
        if (sent != 1) {
            OutputDebugStringW(L"[Mousecam] SendInput failed — possible UIPI block.\n");
        }
    }

    uint16_t GetKeyForGamepadId(uint32_t id) const {
        auto it = gamepad_to_key.find(id);
        if (it != gamepad_to_key.end()) return static_cast<uint16_t>(it->second);
        return 0;
    }

    uint16_t GetDpadUpKey() const {
        uint32_t id = is_gamepad ? 11 : 12;
        return GetKeyForGamepadId(id);
    }

    uint16_t GetDpadDownKey() const {
        uint32_t id = is_gamepad ? 12 : 13;
        return GetKeyForGamepadId(id);
    }

    uint16_t GetDpadLeftKey() const {
        uint32_t id = is_gamepad ? 13 : 14;
        return GetKeyForGamepadId(id);
    }

    uint16_t GetDpadRightKey() const {
        uint32_t id = is_gamepad ? 14 : 15;
        return GetKeyForGamepadId(id);
    }

    uint16_t GetRstickLeftKey() const {
        return GetKeyForGamepadId(24);
    }

    uint16_t GetRstickRightKey() const {
        return GetKeyForGamepadId(25);
    }
};

static CemuKeyInjector g_ki;


#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")


// --- Globals for UI ---
static HWND g_hWnd = nullptr;
static ULONG_PTR g_gdiplusToken;

struct ProcessInfo {
    DWORD pid;
    std::wstring name;
    std::wstring title;
    HICON hIcon = nullptr;
};

static std::atomic<bool> g_isPolling(false);
static bool g_hoverPath = false;
static bool g_hoverPathReset = false;
static bool g_hoverDarkBtn = false, g_hoverLightBtn = false;
static bool g_downPath = false;


static std::wstring g_statusText = L"Ready.";



// UI State
static bool g_hoverInject = false, g_hoverReinject = false, g_hoverReset = false;
static float g_animInject = 0, g_animReinject = 0, g_animReset = 0;
static float g_animDarkBtn = 0, g_animLightBtn = 0, g_animPath = 0, g_animPathReset = 0;
static float g_animScrollHelper = 0, g_animOrbitCam = 0, g_animIndepSens = 0;
static float g_animSensH = 0, g_animSensV = 0, g_animClearLog = 0;
static float g_animDrop[5] = {0,0,0,0,0};
static bool g_downInject = false, g_downReinject = false, g_downReset = false;
static int g_hoverDropdown = -1; // 0..4 for mouse bindings
static int g_openDropdown = -1;
static float g_dragSlider = -1; // 0=H, 1=V

// UIRects cache (I4) — defined after UIRects struct below

// UX6: Clear log button state
static bool g_hoverClearLog = false;
static Rect g_clearLogRect;

// UX5: Tooltip control
static HWND g_hTooltip = nullptr;
static const wchar_t* g_tooltipText = nullptr;
static bool g_tooltipActive = false;

static void ShowTooltip(HWND hWnd, const wchar_t* text, int x, int y) {
    if (!g_hTooltip) return;
    if (g_tooltipActive && g_tooltipText == text) return;
    if (g_tooltipActive) {
        SendMessageW(g_hTooltip, TTM_TRACKACTIVATE, FALSE, 0);
        g_tooltipActive = false;
    }
    if (text) {
        TOOLINFOW ti = {0};
        ti.cbSize = sizeof(ti);
        ti.hwnd = hWnd;
        ti.uId = 1;
        ti.lpszText = (LPWSTR)text;
        SendMessageW(g_hTooltip, TTM_UPDATETIPTEXT, 0, (LPARAM)&ti);
        SendMessageW(g_hTooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(x + 15, y + 15));
        SendMessageW(g_hTooltip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
        g_tooltipActive = true;
        g_tooltipText = text;
    }
}

static void SetStatus(const wchar_t* msg) {
    g_statusText = msg;
    if (g_hWnd) InvalidateRect(g_hWnd, nullptr, FALSE);
}

static void LogToConsole(const wchar_t* format, ...) {
    if (!g_hConsoleEdit) return;
    
    wchar_t msg[512];
    va_list args;
    va_start(args, format);
    vswprintf_s(msg, format, args);
    va_end(args);

    int type = 0;
    const wchar_t* msg_content = msg;

    if (wcsncmp(msg, L"[INFO] ", 7) == 0) {
        msg_content = msg + 7;
    } else if (wcsncmp(msg, L"[SUCCESS] ", 10) == 0) {
        type = 1; msg_content = msg + 10;
    } else if (wcsncmp(msg, L"[ERROR] ", 8) == 0) {
        type = 3; msg_content = msg + 8;
    } else if (wcsncmp(msg, L"[WARNING] ", 10) == 0) {
        type = 2; msg_content = msg + 10;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[768];
    swprintf_s(buf, L"%02d:%02d:%02d %s\n", st.wHour, st.wMinute, st.wSecond, msg_content);

    // TD6: Optional file logging — simple append-write each call
    static bool g_logToFile = true;
    if (g_logToFile) {
        wchar_t logPath[MAX_PATH];
        GetModuleFileNameW(nullptr, logPath, MAX_PATH);
        wchar_t* lastSlash = wcsrchr(logPath, L'\\');
        if (lastSlash) {
            wcscpy_s(lastSlash + 1, MAX_PATH - (lastSlash - logPath) - 1, L"mousecam_companion.log");
            std::ofstream logFile(WstrToUtf8(logPath), std::ios::app);
            if (logFile.is_open()) {
                logFile << WstrToUtf8(buf);
                logFile.close();
            }
        }
    }

    GETTEXTLENGTHEX gtl = { GTL_DEFAULT, 1200 };
    int len = SendMessageW(g_hConsoleEdit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
    SendMessageW(g_hConsoleEdit, EM_SETSEL, len, len);

    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    if (type == 1) cf.crTextColor = RGB(g_theme.success.GetR(), g_theme.success.GetG(), g_theme.success.GetB());
    else if (type == 2) cf.crTextColor = RGB(220, 180, 50);
    else if (type == 3) cf.crTextColor = RGB(g_theme.error.GetR(), g_theme.error.GetG(), g_theme.error.GetB());
    else cf.crTextColor = RGB(g_theme.text.GetR(), g_theme.text.GetG(), g_theme.text.GetB());
    
    // B6: EM_SETCHARFORMAT + CFM_COLOR is correct for per-line coloring in rich edit.
    // No change needed — this is the standard Win32 approach for colored console output.
    SendMessageW(g_hConsoleEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(g_hConsoleEdit, EM_REPLACESEL, 0, (LPARAM)buf);
    SendMessageW(g_hConsoleEdit, WM_VSCROLL, SB_BOTTOM, 0);
}

static void ClearConsole() {
    if (g_hConsoleEdit) {
        SendMessageW(g_hConsoleEdit, EM_SETSEL, 0, -1);
        SendMessageW(g_hConsoleEdit, EM_REPLACESEL, 0, (LPARAM)L"");
    }
}

static DWORD FindCemuProcess() {
    if (!g_config.cemu_path_override.empty()) {
        std::wstring exeName = Utf8ToWstr(g_config.cemu_path_override);
        size_t lastSlash = exeName.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            exeName = exeName.substr(lastSlash + 1);
        }
        DWORD pid = Injector::FindProcessByName(exeName);
        if (pid != 0) return pid;
    } else {
        DWORD pid = Injector::FindProcessByName(L"cemu.exe");
        if (pid != 0) return pid;
        pid = Injector::FindProcessByName(L"cemu_release.exe");
        if (pid != 0) return pid;
        pid = Injector::FindProcessByName(L"Cemu_release.exe");
        if (pid != 0) return pid;
    }

    return 0;
}

static DWORD GetSelectedOrTargetPid() {
    return FindCemuProcess();
}

static void WriteConfigToSharedMemory() {
    if (g_pSharedMemory) {
        g_pSharedMemory->m_cfgMagnesisEnabled = g_config.magnesis_enabled;
        g_pSharedMemory->m_cfgScrollHelper = g_config.scroll_helper ? 1 : 0;
        g_pSharedMemory->m_cfgFullOrbitCamera = g_config.full_orbit_camera ? 1 : 0;
        g_pSharedMemory->m_cfgSensitivityX = g_config.sensitivity_x;
        g_pSharedMemory->m_cfgSensitivityY = g_config.sensitivity_y;
        g_pSharedMemory->m_cfgUseIndependentSens = g_config.use_independent_sens ? 1 : 0;
        
        g_pSharedMemory->m_cfgDpadUpKey = g_ki.GetDpadUpKey();
        g_pSharedMemory->m_cfgDpadDownKey = g_ki.GetDpadDownKey();
        g_pSharedMemory->m_cfgDpadLeftKey = g_ki.GetDpadLeftKey();
        g_pSharedMemory->m_cfgDpadRightKey = g_ki.GetDpadRightKey();
        g_pSharedMemory->m_cfgRstickLeftKey = g_ki.GetRstickLeftKey();
        g_pSharedMemory->m_cfgRstickRightKey = g_ki.GetRstickRightKey();

        for (int i = 0; i < 5; ++i) {
            uint32_t gpid = g_config.mouse_bindings[i];
            g_pSharedMemory->m_cfgMouseBindingKeys[i] = (gpid != 0) ? g_ki.GetKeyForGamepadId(gpid) : 0;
        }
        g_pSharedMemory->m_cfgHCemuWnd = reinterpret_cast<uint64_t>(GetTargetWindow(g_targetPid));
    }
}

static void UpdateUiState() {
    static int findTicks = 0, refreshTicks = 0;
    
    if (g_hTargetProcess != nullptr) {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(g_hTargetProcess, &exitCode) || exitCode != STILL_ACTIVE) {
            CloseHandle(g_hTargetProcess); g_hTargetProcess = nullptr; g_targetPid = 0; g_targetInjected = false;
            SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0); ClipCursor(nullptr);
            UnmapSharedMemory();
            SetStatus(L"Target process exited. Waiting...");
        } else {
            g_targetInjected = Injector::IsModuleLoaded(g_targetPid, Injector::GetFileName(GetCompanionDllPath()));
        }
    }

    if (g_hTargetProcess == nullptr) {
        findTicks++;
        if (findTicks >= 10) {
            findTicks = 0; g_targetPid = GetSelectedOrTargetPid();
            if (g_targetPid != 0) {
                g_hTargetProcess = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, g_targetPid);
                if (g_hTargetProcess) g_targetInjected = Injector::IsModuleLoaded(g_targetPid, Injector::GetFileName(GetCompanionDllPath()));
            }
        }
    } else { findTicks = 0; }

    if (g_targetInjected && g_targetPid != 0) MapSharedMemory();
    else if (!g_targetInjected) UnmapSharedMemory();
    
#ifdef _DEBUG
    static bool wasF5Pressed = false;
    bool isF5Pressed = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (isF5Pressed && !wasF5Pressed) {
        if (g_pSharedMemory) {
            g_pSharedMemory->m_reqDumpAob = true;
            LogToConsole(L"[INFO] Requested AOB dump.");
        }
    }
    wasF5Pressed = isF5Pressed;
#endif

    if (g_hWnd) InvalidateRect(g_hWnd, nullptr, FALSE);
}

static void DoInjectOrEject() {
    DWORD pid = GetSelectedOrTargetPid();
    if (!pid) { SetStatus(L"Error: cemu.exe not found."); return; }
    std::wstring dllPath = GetCompanionDllPath();
    if (Injector::IsModuleLoaded(pid, Injector::GetFileName(dllPath))) {
        SetStatus(L"Ejecting...");
        if (g_pSharedMemory) {
            g_pSharedMemory->m_reqShutdown = true;
            Sleep(100);
        }
        if (Injector::EjectDLL(pid, dllPath)) { SetStatus(L"Ejection successful!"); UpdateUiState(); }
        else SetStatus(L"Error: ejection failed.");
    } else {
        SetStatus(L"Injecting...");
        if (Injector::InjectDLL(pid, dllPath)) { SetStatus(L"Injection successful!"); UpdateUiState(); }
        else SetStatus(L"Error: injection failed \x2014 try running as Administrator.");
    }
}

static void DoReinject() {
    DWORD pid = GetSelectedOrTargetPid();
    if (!pid) return;
    std::wstring dllPath = GetCompanionDllPath();
    if (Injector::IsModuleLoaded(pid, Injector::GetFileName(dllPath))) {
        SetStatus(L"Reloading...");
        if (g_pSharedMemory) {
            g_pSharedMemory->m_reqShutdown = true;
            Sleep(100);
        }
        if (!Injector::EjectDLL(pid, dllPath)) { SetStatus(L"Error: reload failed."); return; }
        Sleep(150);
    }
    g_ki.ReloadSettings();
    SetStatus(L"Reloading...");
    if (Injector::InjectDLL(pid, dllPath)) SetStatus(L"Reinjected successfully!");
    else SetStatus(L"Reinject error: injection failed.");
}

static void DoEjectOnClose() {
    DWORD pid = GetSelectedOrTargetPid();
    if (pid && Injector::IsModuleLoaded(pid, Injector::GetFileName(GetCompanionDllPath()))) {
        if (g_pSharedMemory) {
            g_pSharedMemory->m_reqShutdown = true;
            Sleep(100);
        }
        Injector::EjectDLL(pid, GetCompanionDllPath());
    }
}

static void UpdateTelemetryGui() {
    if (!g_pSharedMemory) {
        g_addrGameRomCamera = 0; g_addrMagneTarget = 0; g_addrShortcutMenu = 0; g_addrMenuState = 0; g_writersFound = 0;
        g_liveCamPosX = 0; g_liveCamPosY = 0; g_liveCamPosZ = 0; g_liveCamFocX = 0; g_liveCamFocY = 0; g_liveCamFocZ = 0; g_liveCamFOV = 0;
        g_liveShortcutMenu = -1; g_liveMenuState = 1; g_magneDetourActive = false;
    } else {
        if (g_pSharedMemory->m_statusAddrGameRomCamera != 0 && g_addrGameRomCamera == 0) LogToConsole(L"[INFO] Found GameRomCamera");
        if (g_pSharedMemory->m_statusAddrMagneTarget != 0 && g_addrMagneTarget == 0) LogToConsole(L"[INFO] Found Magne Target Sig");
        if (g_pSharedMemory->m_statusAddrShortcutMenu != 0 && g_addrShortcutMenu == 0) LogToConsole(L"[INFO] Found ShortcutMenu");
        if (g_pSharedMemory->m_statusAddrMenuState != 0 && g_addrMenuState == 0) LogToConsole(L"[INFO] Found MenuState");
        if (g_pSharedMemory->m_statusWritersFound > g_writersFound) LogToConsole(L"[INFO] Dynamic writer discovered");
        
        g_addrGameRomCamera = g_pSharedMemory->m_statusAddrGameRomCamera;
        g_addrMagneTarget = g_pSharedMemory->m_statusAddrMagneTarget;
        g_addrShortcutMenu = g_pSharedMemory->m_statusAddrShortcutMenu;
        g_addrMenuState = g_pSharedMemory->m_statusAddrMenuState;
        g_writersFound = g_pSharedMemory->m_statusWritersFound;

        g_liveCamPosX = g_pSharedMemory->m_teleLiveCamPosX; g_liveCamPosY = g_pSharedMemory->m_teleLiveCamPosY; g_liveCamPosZ = g_pSharedMemory->m_teleLiveCamPosZ;
        g_liveCamFocX = g_pSharedMemory->m_teleLiveCamFocX; g_liveCamFocY = g_pSharedMemory->m_teleLiveCamFocY; g_liveCamFocZ = g_pSharedMemory->m_teleLiveCamFocZ;
        g_liveCamFOV = g_pSharedMemory->m_teleLiveCamFOV; g_liveShortcutMenu = g_pSharedMemory->m_teleLiveShortcutMenu; g_liveMenuState = g_pSharedMemory->m_teleLiveMenuState;
        g_magneDetourActive = g_pSharedMemory->m_patchMagneDetourActive;
    }
}

// --- GDI+ Drawing Helpers ---
static void DrawRoundedRect(Graphics& g, const Rect& bounds, int radius, const Pen* pen, const Brush* brush) {
    GraphicsPath path;
    int d = radius * 2;
    path.AddArc(bounds.X, bounds.Y, d, d, 180, 90);
    path.AddArc(bounds.X + bounds.Width - d, bounds.Y, d, d, 270, 90);
    path.AddArc(bounds.X + bounds.Width - d, bounds.Y + bounds.Height - d, d, d, 0, 90);
    path.AddArc(bounds.X, bounds.Y + bounds.Height - d, d, d, 90, 90);
    path.CloseFigure();
    if (brush) g.FillPath(brush, &path);
    if (pen) g.DrawPath(pen, &path);
}

static void DrawToggle(Graphics& g, int x, int y, float stateAnim, const wchar_t* label, FontFamily& ff) {
    Rect r(x, y, 36, 20);
    Color cOff = Color(255, 60, 60, 75);
    Color cOn = g_theme.accent;
    SolidBrush bg(LerpColor(cOff, cOn, stateAnim));
    DrawRoundedRect(g, r, 10, nullptr, &bg);
    SolidBrush thumb(Color::White);
    float thumbX = 2.0f + 16.0f * stateAnim;
    g.FillEllipse(&thumb, x + (int)thumbX, y + 2, 16, 16);
    
    SolidBrush textBrush(g_theme.text);
    Font f(&ff, 12, FontStyleRegular, UnitPixel);
    g.DrawString(label, -1, &f, PointF((REAL)x + 45, (REAL)y + 2), &textBrush);
}

static void DrawSlider(Graphics& g, int x, int y, int width, float value, float min_v, float max_v, float hoverAnim, const wchar_t* label, FontFamily& ff) {
    SolidBrush textBrush(g_theme.text);
    Font f(&ff, 12, FontStyleRegular, UnitPixel);
    g.DrawString(label, -1, &f, PointF((REAL)x, (REAL)y), &textBrush);
    
    wchar_t valBuf[32]; swprintf_s(valBuf, L"%.2f", value);
    g.DrawString(valBuf, -1, &f, PointF((REAL)x + width - 30, (REAL)y), &textBrush);

    y += 20;
    SolidBrush track(Color(255, 60, 60, 75));
    g.FillRectangle(&track, x, y + 4, width, 4);
    
    float pct = (value - min_v) / (max_v - min_v);
    if (pct < 0) pct = 0; if (pct > 1) pct = 1;
    SolidBrush fill(LerpColor(g_theme.accent, Color(255, 60, 160, 255), hoverAnim));
    g.FillRectangle(&fill, x, y + 4, (int)(width * pct), 4);
    g.FillEllipse(&fill, x + (int)(width * pct) - 6, y - 2, 12, 12);
}

struct ButtonItem { const char* name; uint32_t val; };
static const ButtonItem GAMEPAD_BUTTONS[] = { {"Disabled",0},{"Button A",1},{"Button B",2},{"Button X",3},{"Button Y",4},{"L",5},{"R",6},{"ZL",7},{"ZR",8},{"Plus",9},{"Minus",10},{"D-Pad Up",11},{"D-Pad Down",12},{"D-Pad Left",13},{"D-Pad Right",14},{"L-Stick Click",15},{"R-Stick Click",16},{"Home",19} };
static const ButtonItem PRO_BUTTONS[] = { {"Disabled",0},{"Button A",1},{"Button B",2},{"Button X",3},{"Button Y",4},{"L",5},{"R",6},{"ZL",7},{"ZR",8},{"Plus",9},{"Minus",10},{"Home",11},{"D-Pad Up",12},{"D-Pad Down",13},{"D-Pad Left",14},{"D-Pad Right",15},{"L-Stick Click",16},{"R-Stick Click",17} };

static void DrawDropdown(Graphics& g, int x, int y, int width, const wchar_t* label, uint32_t selectedVal, int ddIdx, float hoverAnim, FontFamily& ff) {
    SolidBrush textBrush(g_theme.text);
    Font f(&ff, 12, FontStyleRegular, UnitPixel);
    g.DrawString(label, -1, &f, PointF((REAL)x, (REAL)y + 4), &textBrush);
    
    Rect r(x + 80, y, width - 80, 24);
    SolidBrush bg(LerpColor(g_theme.bg, Color(255, 35, 35, 45), hoverAnim)); Pen pen(g_theme.border);
    g.FillRectangle(&bg, r); g.DrawRectangle(&pen, r);
    
    const ButtonItem* buttons = g_ki.is_gamepad ? GAMEPAD_BUTTONS : PRO_BUTTONS;
    int count = g_ki.is_gamepad ? 18 : 18;
    std::wstring selName = L"Disabled";
    for (int i=0; i<count; ++i) if (buttons[i].val == selectedVal) {
        selName = std::wstring(buttons[i].name, buttons[i].name + strlen(buttons[i].name)); break;
    }
    
    g.DrawString(selName.c_str(), -1, &f, PointF((REAL)r.X + 5, (REAL)r.Y + 4), &textBrush);
    
    SolidBrush arrow(LerpColor(g_theme.textMuted, Color(255, 200, 200, 200), hoverAnim));
    PointF pts[] = { PointF((REAL)r.X + r.Width - 15, (REAL)r.Y + 10), PointF((REAL)r.X + r.Width - 5, (REAL)r.Y + 10), PointF((REAL)r.X + r.Width - 10, (REAL)r.Y + 15) };
    g.FillPolygon(&arrow, pts, 3);
}

// Layout constants
const int WND_W = 900;
const int WND_H = 880;

struct UIRects {
    Rect rConnPanel;
    Rect rMemPanel;
    Rect rTelePanel;
    Rect rSetPanel;
    Rect rBindPanel;

    Rect rInj, rReinj, rRst;
    Rect rScrollHelper, rOrbitCam, rIndepSens, rSensH, rSensV;
    Rect rDrops[5], rDropMenu;
    Rect rPath, rPathReset;
    Rect rDarkBtn, rLightBtn;
    Rect rStatusDot; // I3: status indicator dot
    Rect rClearLog;  // UX6: [Clear] button
    Rect rLog;
};

// I4: UIRects cache globals — defined after struct
static UIRects g_cachedUIRects;
static int g_cachedWidth = 0;
static int g_cachedHeight = 0;
static bool g_cachedIndepSens = false;

static void InvalidateUIRectsCache() { g_cachedWidth = 0; }

static void CalculateUIRects(UIRects& r, int w, int h) {
    // I4: Cache check — skip recalculation if nothing relevant changed
    bool indepSens = g_config.use_independent_sens;
    if (w == g_cachedWidth && h == g_cachedHeight && indepSens == g_cachedIndepSens && g_openDropdown == -1) {
        r = g_cachedUIRects;
        return;
    }

    const int pad = 15;
    const int panelW = w - pad * 2;
    const int spacing = 10;
    int curY = 10;

    r.rDarkBtn = Rect(w / 2 - 20, 15, 14, 14);
    r.rLightBtn = Rect(w / 2 + 5, 15, 14, 14);
    
    // CONNECTION PANEL
    const int connH = 85;
    r.rConnPanel = Rect(pad, curY, panelW, connH);
    r.rInj = Rect(pad + panelW - 260, curY + 45, 80, 26);
    r.rReinj = Rect(pad + panelW - 175, curY + 45, 80, 26);
    r.rRst = Rect(pad + panelW - 90, curY + 45, 80, 26);
    r.rPath = Rect(pad + 10, curY + 50, 115, 20);
    r.rPathReset = g_config.cemu_path_override.empty() ? Rect(0,0,0,0) : Rect(pad + 130, curY + 50, 18, 20);
    r.rStatusDot = Rect(pad + panelW - 25, curY + 15, 12, 12);
    
    curY += connH + spacing;
    
    // MEMORY ADDRESSES
    const int memH = 115;
    r.rMemPanel = Rect(pad, curY, panelW, memH);
    curY += memH + spacing;
    
    // VECTORS
    const int teleH = 115;
    r.rTelePanel = Rect(pad, curY, panelW, teleH);
    curY += teleH + spacing;
    
    // CAMERA SETTINGS
    const int setH = g_config.use_independent_sens ? 130 : 100;
    r.rSetPanel = Rect(pad, curY, panelW, setH);
    int gap = (panelW - 515) / 2;
    if (gap < 5) gap = 5;
    r.rScrollHelper = Rect(pad + 5, curY + 35, 135, 20);
    r.rOrbitCam = Rect(pad + 5 + 135 + gap, curY + 35, 145, 20);
    r.rIndepSens = Rect(pad + 5 + 135 + gap + 145 + gap, curY + 35, 235, 20);
    
    r.rSensH = Rect(pad + 10, curY + 65, panelW - 40, 24);
    if (g_config.use_independent_sens) {
        r.rSensV = Rect(pad + 10, curY + 95, panelW - 40, 24);
    } else {
        r.rSensV = Rect(0,0,0,0);
    }
    curY += setH + spacing;
    
    // MOUSE BINDINGS
    const int bindH = 120;
    r.rBindPanel = Rect(pad, curY, panelW, bindH);
    int bw = (panelW - 30) / 2;
    r.rDrops[0] = Rect(pad + 10, curY + 35, bw, 24);
    r.rDrops[1] = Rect(pad + 20 + bw, curY + 35, bw, 24);
    r.rDrops[2] = Rect(pad + 10, curY + 60, bw, 24);
    r.rDrops[3] = Rect(pad + 20 + bw, curY + 60, bw, 24);
    r.rDrops[4] = Rect(pad + 10, curY + 85, bw, 24);
    
    if (g_openDropdown != -1) {
        int cx = pad + (g_openDropdown == 1 || g_openDropdown == 3 ? 20 + bw : 10) + 80;
        int cy = (g_openDropdown < 2 ? 35 : g_openDropdown < 4 ? 60 : 85) + curY;
        int count = 18;
        r.rDropMenu = Rect(cx, cy + 24, bw - 80, count * 18);
    } else {
        r.rDropMenu = Rect(0,0,0,0);
    }
    curY += bindH + spacing;

    // LOG
    r.rClearLog = Rect(pad + panelW - 60, curY + 10, 45, 18);
    r.rLog = Rect(pad, curY, panelW, std::max(10, h - curY - 10));
    
    // Cache for next time
    g_cachedUIRects = r;
    g_cachedWidth = w;
    g_cachedHeight = h;
    g_cachedIndepSens = indepSens;
}

static void UpdateConsoleEditPosition(HWND hWnd) {
    if (!g_hConsoleEdit) return;
    RECT rc; GetClientRect(hWnd, &rc);
    float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
    if (dpiScale <= 0) dpiScale = 1.0f;
    int w = rc.right / dpiScale, h = rc.bottom / dpiScale;
    UIRects ui;
    CalculateUIRects(ui, w, h);
    MoveWindow(g_hConsoleEdit, (ui.rLog.X + 5) * dpiScale, (ui.rLog.Y + 30) * dpiScale, (ui.rLog.Width - 10) * dpiScale, (ui.rLog.Height - 35) * dpiScale, TRUE);
}

// Hover states tracking
static bool g_trackingMouse = false;
static bool g_hoverScrollHelper = false, g_hoverOrbitCam = false, g_hoverIndepSens = false;
static bool g_hoverSensH = false, g_hoverSensV = false;
static int g_hoverDrop = -1;
static int g_hoverDropMenuRow = -1;

static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        SetTimer(hWnd, 1, 8, nullptr);
        
        // Title bar is updated in ApplyTheme()
        
        // UX5: Create tooltip control
        g_hTooltip = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr,
            TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hWnd, nullptr, (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE), nullptr);
        if (g_hTooltip) {
            auto addTool = [&](HWND hwnd, const wchar_t* text) {
                TOOLINFOW ti = {0};
                ti.cbSize = sizeof(ti);
                ti.hwnd = hWnd;
                ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                ti.uId = (UINT_PTR)hwnd;
                ti.lpszText = (LPWSTR)text;
                SendMessageW(g_hTooltip, TTM_ADDTOOL, 0, (LPARAM)&ti);
            };
            // Since all our UI is owner-drawn, we can't use HWND-based tooltips easily.
            // We'll use the TTM_TRACKPOSITION/TTM_TRACKACTIVATE approach instead.
            // For now, just create the tooltip control — a full tooltip implementation
            // would require tracking mouse position and showing tooltip text manually.
            SendMessageW(g_hTooltip, TTM_SETMAXTIPWIDTH, 0, 200);
        }
        return 0;
    }
    case WM_TIMER: {
        g_animInject += (g_hoverInject ? 0.05f : -0.05f);
        g_animReinject += (g_hoverReinject ? 0.05f : -0.05f);
        g_animReset += (g_hoverReset ? 0.05f : -0.05f);
        g_animDarkBtn += (g_hoverDarkBtn ? 0.05f : -0.05f);
        g_animLightBtn += (g_hoverLightBtn ? 0.05f : -0.05f);
        g_animPath += (g_hoverPath ? 0.05f : -0.05f);
        g_animPathReset += (g_hoverPathReset ? 0.05f : -0.05f);
        g_animScrollHelper += (g_config.scroll_helper ? 0.075f : -0.075f);
        g_animOrbitCam += (g_config.full_orbit_camera ? 0.075f : -0.075f);
        g_animIndepSens += (g_config.use_independent_sens ? 0.075f : -0.075f);
        g_animSensH += (g_hoverSensH ? 0.05f : -0.05f);
        g_animSensV += (g_hoverSensV ? 0.05f : -0.05f);
        g_animClearLog += (g_hoverClearLog ? 0.05f : -0.05f);
        for (int i=0; i<5; ++i) g_animDrop[i] += (g_hoverDrop == i ? 0.05f : -0.05f);

        auto clampF = [](float& val) { if (val < 0) val = 0; if (val > 1) val = 1; };
        clampF(g_animInject); clampF(g_animReinject); clampF(g_animReset);
        clampF(g_animDarkBtn); clampF(g_animLightBtn);
        clampF(g_animPath); clampF(g_animPathReset);
        clampF(g_animScrollHelper); clampF(g_animOrbitCam);
        clampF(g_animIndepSens); clampF(g_animSensH); clampF(g_animSensV); clampF(g_animClearLog);
        for (int i=0; i<5; ++i) clampF(g_animDrop[i]);

        float targetTheme = g_config.use_light_theme ? 0.0f : 1.0f;
        if (g_animTheme == -1.0f) g_animTheme = targetTheme;
        if (g_animTheme != targetTheme) {
            float step = 0.05f;
            if (targetTheme > g_animTheme) {
                g_animTheme += step;
                if (g_animTheme > targetTheme) g_animTheme = targetTheme;
            } else {
                g_animTheme -= step;
                if (g_animTheme < targetTheme) g_animTheme = targetTheme;
            }
            ApplyTheme();
        }

        UpdateUiState();
        WriteConfigToSharedMemory();
        UpdateTelemetryGui();
        // Only invalidate if there's a reason (or hover animations if implemented later)
        static float lastFocX = 0, lastPosX = 0, lastCamFOV = 0;
        static int32_t lastShortcut = -1; static uint8_t lastMenu = 1;
        bool changed = (lastFocX != g_liveCamFocX || lastPosX != g_liveCamPosX || lastCamFOV != g_liveCamFOV ||
                        lastShortcut != g_liveShortcutMenu || lastMenu != g_liveMenuState);
        lastFocX = g_liveCamFocX; lastPosX = g_liveCamPosX; lastCamFOV = g_liveCamFOV;
        lastShortcut = g_liveShortcutMenu; lastMenu = g_liveMenuState;
        
        // Always invalidate if we are polling (for the status dot) or dragging
        bool animating = true; // Always invalidate with a 16ms timer for smooth UI since it's cheap enough, but only if an animation is actually happening.
        bool hasAnim = (g_animInject > 0 && g_animInject < 1) || (g_animReinject > 0 && g_animReinject < 1) || (g_animReset > 0 && g_animReset < 1) ||
                       (g_animDarkBtn > 0 && g_animDarkBtn < 1) || (g_animLightBtn > 0 && g_animLightBtn < 1) ||
                       (g_animPath > 0 && g_animPath < 1) || (g_animPathReset > 0 && g_animPathReset < 1) ||
                       (g_animTheme != (g_config.use_light_theme ? 0.0f : 1.0f)) ||
                       (g_animScrollHelper > 0 && g_animScrollHelper < 1) ||
                       (g_animOrbitCam > 0 && g_animOrbitCam < 1) || (g_animIndepSens > 0 && g_animIndepSens < 1) ||
                       (g_animSensH > 0 && g_animSensH < 1) || (g_animSensV > 0 && g_animSensV < 1) || (g_animClearLog > 0 && g_animClearLog < 1);
        for (int i=0; i<5; ++i) if (g_animDrop[i] > 0 && g_animDrop[i] < 1) hasAnim = true;
        if (changed || g_dragSlider != -1 || !g_targetInjected || hasAnim) {
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }    case WM_SIZE: {
        InvalidateUIRectsCache(); // UX7: force recalculation on resize
        UpdateConsoleEditPosition(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_APP + 1:
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1; // Prevent background from being erased, fixes screen flashing/flicker
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        
        RECT rc; GetClientRect(hWnd, &rc);
        int w = rc.right, h = rc.bottom;
        
        float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
        if (dpiScale <= 0.0f) dpiScale = 1.0f;
        int logicalW = w / dpiScale;
        int logicalH = h / dpiScale;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBmp = SelectObject(memDC, memBitmap);
        
        Graphics g(memDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.ScaleTransform(dpiScale, dpiScale);

        
        SolidBrush bgBrush(g_theme.bg);
        g.FillRectangle(&bgBrush, 0, 0, w, h);
        
        // TD4: GDI+ FontFamily and Font are stack-allocated; their destructors release
        // GDI+ resources properly when they go out of scope at the end of WM_PAINT.
        // No explicit cleanup needed — RAII via stack unwinding.
        FontFamily ff(L"Segoe UI");
        Font fontSec(&ff, 15, FontStyleBold, UnitPixel);
        Font fontBody(&ff, 12, FontStyleRegular, UnitPixel);
        Font smallFont(&ff, 11, FontStyleRegular, UnitPixel); // for small UI elements (UX3, UX6)
        SolidBrush textBrush(g_theme.text);
        SolidBrush mutedBrush(g_theme.textMuted);
        Pen borderPen(g_theme.border);
        SolidBrush panelBrush(g_theme.panel);

        StringFormat sfCenter;
        sfCenter.SetAlignment(StringAlignmentCenter);
        sfCenter.SetLineAlignment(StringAlignmentCenter);

        UIRects ui;
        CalculateUIRects(ui, logicalW, logicalH);
        
        int pad = 15;

        // --- CONNECTION PANEL ---
        DrawRoundedRect(g, ui.rConnPanel, 8, &borderPen, &panelBrush);
        g.DrawString(L"CONNECTION", -1, &fontSec, PointF(pad + 10, ui.rConnPanel.Y + 10), &textBrush);
        // Draw theme buttons
        SolidBrush btnDark(LerpColor(Color(255, 20, 20, 25), Color(255, 50, 50, 55), g_animDarkBtn));
        SolidBrush btnLight(LerpColor(Color(255, 235, 235, 240), Color(255, 255, 255, 255), g_animLightBtn));
        
        Pen activePen(g_theme.accent, 2.0f);
        
        g.FillEllipse(&btnDark, ui.rDarkBtn);
        g.DrawEllipse(g_config.use_light_theme ? &borderPen : &activePen, ui.rDarkBtn);
        
        g.FillEllipse(&btnLight, ui.rLightBtn);
        g.DrawEllipse(g_config.use_light_theme ? &activePen : &borderPen, ui.rLightBtn);

        
        SolidBrush statusBrush(g_targetInjected ? g_theme.success : g_theme.error);
        g.FillEllipse(&statusBrush, ui.rStatusDot); // I3: use cached rect instead of hardcoded coords
        g.DrawString(g_statusText.c_str(), -1, &fontBody, PointF(pad + 120, ui.rConnPanel.Y + 11), &textBrush);
        
        SolidBrush btnInj(g_downInject ? Color(255,50,80,120) : LerpColor(g_theme.accent, Color(255,70,100,150), g_animInject));
        SolidBrush btnReinj(g_downReinject ? Color(255,50,80,120) : LerpColor(g_theme.accent, Color(255,70,100,150), g_animReinject));
        SolidBrush btnRst(g_downReset ? Color(255,50,80,120) : LerpColor(g_theme.accent, Color(255,70,100,150), g_animReset));
        DrawRoundedRect(g, ui.rInj, 4, nullptr, &btnInj);
        DrawRoundedRect(g, ui.rReinj, 4, nullptr, g_targetInjected ? &btnReinj : &mutedBrush);
        DrawRoundedRect(g, ui.rRst, 4, nullptr, g_targetInjected ? &btnRst : &mutedBrush);
        g.DrawString(g_targetInjected ? L"Disconnect" : L"Connect", -1, &fontBody, RectF((REAL)ui.rInj.X, (REAL)ui.rInj.Y, (REAL)ui.rInj.Width, (REAL)ui.rInj.Height), &sfCenter, &textBrush);
        g.DrawString(L"Reinject", -1, &fontBody, RectF((REAL)ui.rReinj.X, (REAL)ui.rReinj.Y, (REAL)ui.rReinj.Width, (REAL)ui.rReinj.Height), &sfCenter, &textBrush);
        g.DrawString(L"Reset", -1, &fontBody, RectF((REAL)ui.rRst.X, (REAL)ui.rRst.Y, (REAL)ui.rRst.Width, (REAL)ui.rRst.Height), &sfCenter, &textBrush);

        SolidBrush btnPath(g_downPath ? Color(255,50,80,120) : LerpColor(g_theme.panel, Color(255,70,100,150), g_animPath));
        DrawRoundedRect(g, ui.rPath, 4, &borderPen, &btnPath);
        g.DrawString(L"Cemu Executable", -1, &fontBody, RectF((REAL)ui.rPath.X, (REAL)ui.rPath.Y, (REAL)ui.rPath.Width, (REAL)ui.rPath.Height), &sfCenter, &textBrush);
        if (!g_config.cemu_path_override.empty()) {
            SolidBrush overrideBrush(g_theme.accent);
            std::wstring exeName = Utf8ToWstr(g_config.cemu_path_override);
            size_t lastSlash = exeName.find_last_of(L"\\/");
            if (lastSlash != std::wstring::npos) exeName = exeName.substr(lastSlash + 1);
            std::wstring txt = L"Target: " + exeName;
            g.DrawString(txt.c_str(), -1, &fontBody, PointF(ui.rPath.X + 150, ui.rPath.Y + 2), &overrideBrush);
            // Reset path button
            SolidBrush resetBg(LerpColor(Color(255, 60, 30, 30), g_theme.error, g_animPathReset));
            DrawRoundedRect(g, ui.rPathReset, 3, nullptr, &resetBg);
            SolidBrush resetText(Color::White);
            Font resetFont(&ff, 12, FontStyleBold, UnitPixel);
            g.DrawString(L"\u2715", -1, &resetFont, RectF((REAL)ui.rPathReset.X, (REAL)ui.rPathReset.Y, (REAL)ui.rPathReset.Width, (REAL)ui.rPathReset.Height), &sfCenter, &resetText);
        }


        // --- MEMORY ADDRESSES ---
        DrawRoundedRect(g, ui.rMemPanel, 8, &borderPen, &panelBrush);
        g.DrawString(L"MEMORY ADDRESSES", -1, &fontSec, PointF(pad + 10, ui.rMemPanel.Y + 10), &textBrush);
        
        auto getAddrStr = [&](uint64_t addr, const wchar_t* foundStr) -> std::wstring {
            if (!g_targetInjected) return L"Not injected yet";
            if (addr == 0) return L"Scanning...";
            return std::wstring(foundStr);
        };
        
        auto drawMemLine = [&](const wchar_t* label, const std::wstring& val, int yOffset) {
            g.DrawString(label, -1, &fontBody, PointF(pad + 10, ui.rMemPanel.Y + yOffset), &textBrush);
            g.DrawString(val.c_str(), -1, &fontBody, PointF(pad + 140, ui.rMemPanel.Y + yOffset), &textBrush);
        };
        
        drawMemLine(L"GameRomCamera:", getAddrStr(g_addrGameRomCamera, L"Found"), 35);
        drawMemLine(L"Magne Target:", getAddrStr(g_addrMagneTarget, g_magneDetourActive ? L"NOP'd" : L"Found"), 50);
        
        wchar_t valBuf1[64], valBuf2[64];
        swprintf_s(valBuf1, L"Value: %d", g_liveShortcutMenu); swprintf_s(valBuf2, L"Value: %d", g_liveMenuState);
        drawMemLine(L"Shortcut Menu:", getAddrStr(g_addrShortcutMenu, valBuf1), 65);
        drawMemLine(L"Menu State:", getAddrStr(g_addrMenuState, valBuf2), 80);
        
        wchar_t wbuf[64];
        if (!g_targetInjected) swprintf_s(wbuf, L"Not injected yet");
        else swprintf_s(wbuf, L"%u", g_writersFound);
        drawMemLine(L"Writers NOP'd:", std::wstring(wbuf), 95);

        // --- VECTORS ---
        DrawRoundedRect(g, ui.rTelePanel, 8, &borderPen, &panelBrush);
        g.DrawString(L"VECTORS", -1, &fontSec, PointF(pad + 10, ui.rTelePanel.Y + 10), &textBrush);
        
        auto drawVecLine = [&](const wchar_t* label, float vx, float vy, float vz, int yOffset) {
            g.DrawString(label, -1, &fontBody, PointF(pad + 10, ui.rTelePanel.Y + yOffset), &textBrush);
            wchar_t xb[32], yb[32], zb[32];
            swprintf_s(xb, L"X: %.2f", vx); swprintf_s(yb, L"Y: %.2f", vy); swprintf_s(zb, L"Z: %.2f", vz);
            g.DrawString(xb, -1, &fontBody, PointF(pad + 100, ui.rTelePanel.Y + yOffset), &textBrush);
            g.DrawString(yb, -1, &fontBody, PointF(pad + 200, ui.rTelePanel.Y + yOffset), &textBrush);
            g.DrawString(zb, -1, &fontBody, PointF(pad + 300, ui.rTelePanel.Y + yOffset), &textBrush);
        };
        
        drawVecLine(L"Position:", g_liveCamPosX, g_liveCamPosY, g_liveCamPosZ, 35);
        drawVecLine(L"Focus:", g_liveCamFocX, g_liveCamFocY, g_liveCamFocZ, 50);
        
        g.DrawString(L"FOV:", -1, &fontBody, PointF(pad + 10, ui.rTelePanel.Y + 65), &textBrush);
        wchar_t fovb[32]; swprintf_s(fovb, L"%.2f\x00B0", g_liveCamFOV);
        g.DrawString(fovb, -1, &fontBody, PointF(pad + 100, ui.rTelePanel.Y + 65), &textBrush);
        
        drawVecLine(L"Pivot:", g_pSharedMemory?g_pSharedMemory->m_telePivotX:0, g_pSharedMemory?g_pSharedMemory->m_telePivotY:0, g_pSharedMemory?g_pSharedMemory->m_telePivotZ:0, 80);
        
        float mX = 0, mY = 0, mZ = 0;
        if (g_magneDetourActive && g_pSharedMemory) { mX = g_pSharedMemory->m_teleMagneTargetX; mY = g_pSharedMemory->m_teleMagneTargetY; mZ = g_pSharedMemory->m_teleMagneTargetZ; }
        drawVecLine(L"MTarget:", mX, mY, mZ, 95);

        // --- CAMERA SETTINGS ---
        DrawRoundedRect(g, ui.rSetPanel, 8, &borderPen, &panelBrush);
        g.DrawString(L"CAMERA SETTINGS", -1, &fontSec, PointF(pad + 10, ui.rSetPanel.Y + 10), &textBrush);
        
        DrawToggle(g, ui.rScrollHelper.X, ui.rScrollHelper.Y, g_animScrollHelper, L"Scroll Wheel Weapon Select", ff);
        DrawToggle(g, ui.rOrbitCam.X, ui.rOrbitCam.Y, g_animOrbitCam, L"Full Orbit Camera", ff);
        DrawToggle(g, ui.rIndepSens.X, ui.rIndepSens.Y, g_animIndepSens, L"Independent Vertical Sensitivity", ff);
        
        DrawSlider(g, ui.rSensH.X, ui.rSensH.Y, ui.rSensH.Width, g_config.sensitivity_x, 0.1f, 5.0f, g_animSensH, g_config.use_independent_sens ? L"Sensitivity (H)" : L"Sensitivity & Speed", ff);
        if (g_config.use_independent_sens) {
            DrawSlider(g, ui.rSensV.X, ui.rSensV.Y, ui.rSensV.Width, g_config.sensitivity_y, 0.1f, 5.0f, g_animSensV, L"Sensitivity (V)", ff);
        }

        // --- MOUSE BINDINGS ---
        DrawRoundedRect(g, ui.rBindPanel, 8, &borderPen, &panelBrush);
        g.DrawString(L"MOUSE BINDINGS", -1, &fontSec, PointF(pad + 10, ui.rBindPanel.Y + 10), &textBrush);
        
        DrawDropdown(g, ui.rDrops[0].X, ui.rDrops[0].Y, ui.rDrops[0].Width, L"Left:", g_config.mouse_bindings[0], 0, g_animDrop[0], ff);
        DrawDropdown(g, ui.rDrops[1].X, ui.rDrops[1].Y, ui.rDrops[1].Width, L"Right:", g_config.mouse_bindings[1], 1, g_animDrop[1], ff);
        DrawDropdown(g, ui.rDrops[2].X, ui.rDrops[2].Y, ui.rDrops[2].Width, L"Middle:", g_config.mouse_bindings[2], 2, g_animDrop[2], ff);
        DrawDropdown(g, ui.rDrops[3].X, ui.rDrops[3].Y, ui.rDrops[3].Width, L"Mouse 4:", g_config.mouse_bindings[3], 3, g_animDrop[3], ff);
        DrawDropdown(g, ui.rDrops[4].X, ui.rDrops[4].Y, ui.rDrops[4].Width, L"Mouse 5:", g_config.mouse_bindings[4], 4, g_animDrop[4], ff);
        
        // --- LOG ---
        Rect rLog = ui.rLog;
        SolidBrush consoleBrush(g_theme.consoleBg);
        DrawRoundedRect(g, rLog, 8, &borderPen, &consoleBrush);
        g.DrawString(L"LOG", -1, &fontSec, PointF(pad + 10, ui.rLog.Y + 10), &textBrush);
        
        // UX3: Shortcuts info text below LOG header
        SolidBrush mutedText(g_theme.textMuted);
#ifdef _DEBUG
        const wchar_t* shortcutsText = L"F2: Toggle Camera | F5: AOB Dump";
#else
        const wchar_t* shortcutsText = L"F2: Toggle Camera";
#endif
        g.DrawString(shortcutsText, -1, &smallFont, PointF(pad + 10, ui.rLog.Y + 26), &mutedText);

        // UX6: [Clear] button on right side of LOG header
        {
            Color clearNormal = g_theme.border;
            Color clearHover = g_config.use_light_theme ? Color(255, 200, 200, 200) : Color(255, 80, 80, 90);
            SolidBrush clearBg(LerpColor(clearNormal, clearHover, g_animClearLog));
            DrawRoundedRect(g, ui.rClearLog, 3, nullptr, &clearBg);
            g.DrawString(L"Clear", -1, &smallFont, RectF((REAL)ui.rClearLog.X, (REAL)ui.rClearLog.Y, (REAL)ui.rClearLog.Width, (REAL)ui.rClearLog.Height), &sfCenter, &textBrush);
        }
        
        g.SetClip(Rect(rLog.X + 5, rLog.Y + 30, rLog.Width - 10, rLog.Height - 35));
        int logY = rLog.Y + 30;
        int maxItems = (rLog.Height - 30) / 16;

        g.ResetClip();

        // Draw active dropdown overlay if any
        if (g_openDropdown != -1) {
            g.FillRectangle(&bgBrush, ui.rDropMenu);
            g.DrawRectangle(&borderPen, ui.rDropMenu);
            
            const ButtonItem* buttons = g_ki.is_gamepad ? GAMEPAD_BUTTONS : PRO_BUTTONS;
            for (int i=0; i<18; ++i) {
                if (g_hoverDropMenuRow == i) {
                    SolidBrush hov(Color(255, 30, 30, 40));
                    g.FillRectangle(&hov, ui.rDropMenu.X, ui.rDropMenu.Y + i * 18, ui.rDropMenu.Width, 18);
                }
                std::wstring s(buttons[i].name, buttons[i].name + strlen(buttons[i].name));
                g.DrawString(s.c_str(), -1, &fontBody, PointF(ui.rDropMenu.X + 5, ui.rDropMenu.Y + i * 18), &textBrush);
            }
        }

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBitmap);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam), y = HIWORD(lParam);
        RECT rc; GetClientRect(hWnd, &rc);
        float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
        if (dpiScale <= 0) dpiScale = 1.0f;
        x /= dpiScale; y /= dpiScale;
        int logicalW = rc.right / dpiScale;
        int logicalH = rc.bottom / dpiScale;
        UIRects ui; CalculateUIRects(ui, logicalW, logicalH);
        
        if (g_openDropdown != -1) {
            if (ui.rDropMenu.Contains(x, y)) {
                int idx = (y - ui.rDropMenu.Y) / 18;
                const ButtonItem* buttons = g_ki.is_gamepad ? GAMEPAD_BUTTONS : PRO_BUTTONS;
                if (idx >= 0 && idx < 18) {
                    g_config.mouse_bindings[g_openDropdown] = buttons[idx].val;
                    g_ki.mouse_bindings[g_openDropdown] = buttons[idx].val;
                    SaveConfig(); WriteConfigToSharedMemory();
                }
            }
            g_openDropdown = -1;
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        
        if (ui.rDarkBtn.Contains(x, y)) { g_config.use_light_theme = false; ApplyTheme(); SaveConfig(); InvalidateUIRectsCache(); InvalidateRect(hWnd, nullptr, FALSE); return 0; }
        if (ui.rLightBtn.Contains(x, y)) { g_config.use_light_theme = true; ApplyTheme(); SaveConfig(); InvalidateUIRectsCache(); InvalidateRect(hWnd, nullptr, FALSE); return 0; }
        if (ui.rClearLog.Contains(x, y)) { ClearConsole(); InvalidateRect(hWnd, nullptr, FALSE); return 0; } // UX6
        if (ui.rInj.Contains(x, y)) { g_downInject = true; InvalidateRect(hWnd, nullptr, FALSE); SetCapture(hWnd); }
        if (ui.rReinj.Contains(x, y) && g_targetInjected) { g_downReinject = true; InvalidateRect(hWnd, nullptr, FALSE); SetCapture(hWnd); }
        if (ui.rRst.Contains(x, y) && g_targetInjected) { g_downReset = true; InvalidateRect(hWnd, nullptr, FALSE); SetCapture(hWnd); }
        
        if (ui.rPath.Contains(x, y)) { g_downPath = true; InvalidateRect(hWnd, nullptr, FALSE); SetCapture(hWnd); }
        if (!g_config.cemu_path_override.empty() && ui.rPathReset.Contains(x, y)) {
            g_config.cemu_path_override.clear();
            InvalidateUIRectsCache();
            SaveConfig();
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        
        if (ui.rScrollHelper.Contains(x, y)) { g_config.scroll_helper = !g_config.scroll_helper; SaveConfig(); InvalidateRect(hWnd, nullptr, FALSE); }
        if (ui.rOrbitCam.Contains(x, y)) { g_config.full_orbit_camera = !g_config.full_orbit_camera; SaveConfig(); InvalidateRect(hWnd, nullptr, FALSE); }
        if (ui.rIndepSens.Contains(x, y)) {
            g_config.use_independent_sens = !g_config.use_independent_sens;
            SaveConfig();
            InvalidateUIRectsCache();
            UpdateConsoleEditPosition(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        
        Rect hBoxH = ui.rSensH; hBoxH.Y += 15; hBoxH.Height = 24;
        Rect hBoxV = ui.rSensV; hBoxV.Y += 15; hBoxV.Height = 24;
        if (hBoxH.Contains(x, y)) { g_dragSlider = 0; SetCapture(hWnd); }
        if (g_config.use_independent_sens && hBoxV.Contains(x, y)) { g_dragSlider = 1; SetCapture(hWnd); }
        
        for (int i=0; i<5; ++i) {
            if (ui.rDrops[i].Contains(x, y)) { g_openDropdown = i; InvalidateRect(hWnd, nullptr, FALSE); return 0; }
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        return 0;
    }
    case WM_MOUSELEAVE: {
        g_trackingMouse = false;
        g_hoverInject = g_hoverReinject = g_hoverReset = false;
        g_hoverDarkBtn = g_hoverLightBtn = g_hoverPath = g_hoverPathReset = false;
        g_hoverScrollHelper = g_hoverOrbitCam = g_hoverIndepSens = false;
        g_hoverSensH = g_hoverSensV = g_hoverClearLog = false;
        g_hoverDrop = g_hoverDropMenuRow = -1;
        if (g_tooltipActive) ShowTooltip(hWnd, nullptr, 0, 0);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = LOWORD(lParam), y = HIWORD(lParam);
        RECT rc; GetClientRect(hWnd, &rc);
        float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
        if (dpiScale <= 0) dpiScale = 1.0f;
        x /= dpiScale; y /= dpiScale;
        int logicalW = rc.right / dpiScale;
        int logicalH = rc.bottom / dpiScale;
        UIRects ui; CalculateUIRects(ui, logicalW, logicalH);
        
        if (!g_trackingMouse) {
            TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            g_trackingMouse = true;
        }
        bool needRedraw = false;
        
        auto checkHov = [&](bool& state, bool cond) {
            if (state != cond) { state = cond; needRedraw = true; }
        };
        auto checkHovI = [&](int& state, int val) {
            if (state != val) { state = val; needRedraw = true; }
        };
        
        if (g_openDropdown != -1) {
            if (ui.rDropMenu.Contains(x, y)) checkHovI(g_hoverDropMenuRow, (y - ui.rDropMenu.Y) / 18);
            else checkHovI(g_hoverDropMenuRow, -1);
        } else {
            checkHovI(g_hoverDropMenuRow, -1);
            
            checkHov(g_hoverInject, ui.rInj.Contains(x, y));
            checkHov(g_hoverReinject, ui.rReinj.Contains(x, y) && g_targetInjected);
            checkHov(g_hoverReset, ui.rRst.Contains(x, y) && g_targetInjected);
            checkHov(g_hoverDarkBtn, ui.rDarkBtn.Contains(x, y));
            checkHov(g_hoverLightBtn, ui.rLightBtn.Contains(x, y));
            checkHov(g_hoverPath, ui.rPath.Contains(x, y));
            checkHov(g_hoverPathReset, !g_config.cemu_path_override.empty() && ui.rPathReset.Contains(x, y));
            
            checkHov(g_hoverScrollHelper, ui.rScrollHelper.Contains(x, y));
            checkHov(g_hoverOrbitCam, ui.rOrbitCam.Contains(x, y));
            checkHov(g_hoverIndepSens, ui.rIndepSens.Contains(x, y));
            Rect hBoxH = ui.rSensH; hBoxH.Y += 15; hBoxH.Height = 24;
            Rect hBoxV = ui.rSensV; hBoxV.Y += 15; hBoxV.Height = 24;
            checkHov(g_hoverSensH, hBoxH.Contains(x, y));
            if (g_config.use_independent_sens) checkHov(g_hoverSensV, hBoxV.Contains(x, y));
            
            int dropHov = -1;
            for (int i=0; i<5; ++i) if (ui.rDrops[i].Contains(x, y)) dropHov = i;
            checkHovI(g_hoverDrop, dropHov);
            
            checkHov(g_hoverClearLog, ui.rClearLog.Contains(x, y)); // UX6
        }
        
        if (g_dragSlider != -1) {
            int pad = 15; int w = ui.rSensH.Width;
            float pct = (float)(x - pad - 10) / w;
            if (pct < 0) pct = 0; if (pct > 1) pct = 1;
            float val = 0.1f + pct * 4.9f;
            if (g_dragSlider == 0) g_config.sensitivity_x = val;
            else g_config.sensitivity_y = val;
            SaveConfig(); WriteConfigToSharedMemory();
            needRedraw = true;
        }
        
        // UX5: Tooltip tracking for owner-drawn UI elements
        if (!g_tooltipActive || g_tooltipText != nullptr) {
            const wchar_t* tip = nullptr;
            if (ui.rInj.Contains(x, y)) tip = g_targetInjected ? L"Eject the DLL from the target process" : L"Inject the DLL into the target process";
            else if (ui.rReinj.Contains(x, y) && g_targetInjected) tip = L"Re-eject and re-inject to reload settings";
            else if (ui.rRst.Contains(x, y) && g_targetInjected) tip = L"Reset AOB scanner to re-scan memory signatures";
            else if (ui.rDarkBtn.Contains(x, y)) tip = L"Dark theme";
            else if (ui.rLightBtn.Contains(x, y)) tip = L"Light theme";
            else if (x > ui.rMemPanel.X + 10 && x < ui.rMemPanel.X + 250 && y > ui.rMemPanel.Y + 30 && y < ui.rMemPanel.Y + 45) tip = L"Address in memory storing Camera XYZ.";
            else if (x > ui.rMemPanel.X + 10 && x < ui.rMemPanel.X + 250 && y > ui.rMemPanel.Y + 45 && y < ui.rMemPanel.Y + 60) tip = L"Used to control Magnesis properly.";
            if (tip) ShowTooltip(hWnd, tip, x, y);
            else if (g_tooltipActive) ShowTooltip(hWnd, nullptr, 0, 0);
        }
        
        if (needRedraw) InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_KILLFOCUS: {
        g_dragSlider = -1;
        g_downInject = g_downReinject = g_downReset = g_downPath = false;
        ReleaseCapture();
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        int x = LOWORD(lParam), y = HIWORD(lParam);
        RECT rc; GetClientRect(hWnd, &rc);
        float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
        if (dpiScale <= 0) dpiScale = 1.0f;
        x /= dpiScale; y /= dpiScale;
        int logicalW = rc.right / dpiScale;
        int logicalH = rc.bottom / dpiScale;
        UIRects ui; CalculateUIRects(ui, logicalW, logicalH);
        
        if (g_downPath) {
            g_downPath = false;
            ReleaseCapture();
            if (ui.rPath.Contains(x, y)) {
                OPENFILENAMEW ofn = { 0 };
                wchar_t szFile[MAX_PATH] = { 0 };
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hWnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
                ofn.lpstrFilter = L"Executable Files\0*.exe\0All Files\0*.*\0";
                ofn.nFilterIndex = 1;
                ofn.lpstrTitle = L"Select Cemu Executable";
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

                if (GetOpenFileNameW(&ofn)) {
                    g_config.cemu_path_override = WstrToUtf8(ofn.lpstrFile);
                    InvalidateUIRectsCache();
                    SaveConfig();
                }
            }
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (g_dragSlider != -1) { g_dragSlider = -1; ReleaseCapture(); InvalidateRect(hWnd, nullptr, FALSE); }
        if (g_downInject) { g_downInject = false; ReleaseCapture(); DoInjectOrEject(); InvalidateRect(hWnd, nullptr, FALSE); }
        if (g_downReinject) { g_downReinject = false; ReleaseCapture(); DoReinject(); InvalidateRect(hWnd, nullptr, FALSE); }
        if (g_downReset) { 
            g_downReset = false; 
            ReleaseCapture(); 
            if (g_pSharedMemory) g_pSharedMemory->m_reqResetScan = true;
            InvalidateRect(hWnd, nullptr, FALSE); 
        }
        return 0;
    }


    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = WND_W;
        mmi->ptMinTrackSize.y = WND_H;
        return 0;
    }
    case WM_DESTROY:
        DoEjectOnClose();
        if (g_hTargetProcess) { CloseHandle(g_hTargetProcess); g_hTargetProcess = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);

    // DPI awareness — must be set before window creation (UX2)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(101));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = L"MousecamClass";

    RegisterClassExW(&wcex);

    // Load config BEFORE window creation so theme colors are known (I2)
    LoadConfig();

    // Initial rect
    RECT rc = { 0, 0, WND_W, WND_H };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

    g_hWnd = CreateWindowExW(0, L"MousecamClass", L"Mousecam Companion", 
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);

    g_hConsoleEdit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 0, 0, g_hWnd, nullptr, hInstance, nullptr);
    
    SendMessageW(g_hConsoleEdit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), MAKELPARAM(TRUE, 0));
    // Theme BG is applied by ApplyTheme() below — no duplicate line needed (I2)


    if (!g_hWnd) return 0;

    // Apply theme now that g_hConsoleEdit exists (sets console BG, etc.)
    ApplyTheme();
    UpdateConsoleEditPosition(g_hWnd);
    g_ki.ReloadSettings();

    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(g_gdiplusToken);
    return (int)msg.wParam;
}
