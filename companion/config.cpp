// config.cpp — AppConfig load/save, manual JSON, theme reg, profile path resolution.
// Extracted verbatim from companion/main.cpp lines 226-577.

#define NOMINMAX
#include <Windows.h>
#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cstdlib>

#include "config.h"
#include "string_utils.h"
#include "theme.h"      // ApplyTheme
#include "console.h"    // LogToConsole

AppConfig g_config;

extern bool g_collapsedMem;
extern bool g_collapsedTele;
extern bool g_collapsedSet;
extern bool g_collapsedBind;
extern bool g_collapsedLog;

std::string extract_tag(const std::string& s, const std::string& open, const std::string& close, size_t start_pos) {
    size_t open_pos = s.find(open, start_pos);
    if (open_pos == std::string::npos) return "";
    open_pos += open.length();
    size_t close_pos = s.find(close, open_pos);
    if (close_pos == std::string::npos) return "";
    return s.substr(open_pos, close_pos - open_pos);
}

bool IsWindowsLightTheme() {
    DWORD data = 0;
    DWORD dataSize = sizeof(DWORD);
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&data, &dataSize);
        RegCloseKey(hKey);
    }
    return data != 0;
}

std::string GetConfigPath() {
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

void SaveConfig() {
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
    f << "  \"use_light_theme\": " << (g_config.use_light_theme ? "true" : "false") << ",\n";
    f << "  \"cemu_experimental\": " << (g_config.cemu_experimental ? "true" : "false") << ",\n";
    f << "  \"magnesis_speed_mode\": " << g_config.magnesis_speed_mode << ",\n";
    f << "  \"fps_magnesis\": " << (g_config.fps_magnesis ? "true" : "false") << ",\n";
    f << "  \"fps_magne_eye_height\": " << g_config.fps_magne_eye_height << ",\n";
    f << "  \"fps_magne_offset_forward\": " << g_config.fps_magne_offset_forward << ",\n";
    f << "  \"fps_magne_offset_side\": " << g_config.fps_magne_offset_side << ",\n";
    f << "  \"use_independent_magne_sens\": " << (g_config.use_independent_magne_sens ? "true" : "false") << ",\n";
    f << "  \"magnesis_sensitivity\": " << g_config.magnesis_sensitivity << ",\n";
    f << "  \"magnesis_sensitivity_y\": " << g_config.magnesis_sensitivity_y << ",\n";
    f << "  \"magnesis_pull_sensitivity\": " << g_config.magnesis_pull_sensitivity << ",\n";
    f << "  \"scan_magne_target\": " << (g_config.scan_magne_target ? "true" : "false") << ",\n";
    f << "  \"scan_shortcut_menu\": " << (g_config.scan_shortcut_menu ? "true" : "false") << ",\n";
    f << "  \"scan_menu_state\": " << (g_config.scan_menu_state ? "true" : "false") << ",\n";
    f << "  \"log_to_file\": " << (g_config.log_to_file ? "true" : "false") << ",\n";
    f << "  \"collapsed_mem\": " << (g_collapsedMem ? "true" : "false") << ",\n";
    f << "  \"collapsed_tele\": " << (g_collapsedTele ? "true" : "false") << ",\n";
    f << "  \"collapsed_set\": " << (g_collapsedSet ? "true" : "false") << ",\n";
    f << "  \"collapsed_bind\": " << (g_collapsedBind ? "true" : "false") << ",\n";
    f << "  \"collapsed_log\": " << (g_collapsedLog ? "true" : "false") << "\n";
    f << "}\n";
    f.close();
}

std::string UnescapeJsonString(const std::string& s) {
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

void LoadConfig() {
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
    g_config.cemu_experimental = extract_json_bool("cemu_experimental", false);
    g_config.magnesis_speed_mode = (int)extract_json_double("magnesis_speed_mode", 0.0);
    if (g_config.magnesis_speed_mode < 0 || g_config.magnesis_speed_mode > 2) g_config.magnesis_speed_mode = 0;
    g_config.fps_magnesis = extract_json_bool("fps_magnesis", false);
    g_config.fps_magne_eye_height = (float)extract_json_double("fps_magne_eye_height", 0.5);
    g_config.fps_magne_offset_forward = (float)extract_json_double("fps_magne_offset_forward", 0.0);
    g_config.fps_magne_offset_side = (float)extract_json_double("fps_magne_offset_side", 0.0);
    g_config.use_independent_magne_sens = extract_json_bool("use_independent_magne_sens", false);
    g_config.magnesis_sensitivity = (float)extract_json_double("magnesis_sensitivity", 1.0);
    if (g_config.magnesis_sensitivity < 0.01f) g_config.magnesis_sensitivity = 0.01f;
    if (g_config.magnesis_sensitivity > 2.0f) g_config.magnesis_sensitivity = 2.0f;
    g_config.magnesis_sensitivity_y = (float)extract_json_double("magnesis_sensitivity_y", 1.0);
    if (g_config.magnesis_sensitivity_y < 0.01f) g_config.magnesis_sensitivity_y = 0.01f;
    if (g_config.magnesis_sensitivity_y > 2.0f) g_config.magnesis_sensitivity_y = 2.0f;
    g_config.magnesis_pull_sensitivity = (float)extract_json_double("magnesis_pull_sensitivity", 1.0);
    if (g_config.magnesis_pull_sensitivity < 1.0f) g_config.magnesis_pull_sensitivity = 1.0f;
    if (g_config.magnesis_pull_sensitivity > 10.0f) g_config.magnesis_pull_sensitivity = 10.0f;
    g_config.scan_magne_target = extract_json_bool("scan_magne_target", true);
    g_config.scan_shortcut_menu = extract_json_bool("scan_shortcut_menu", true);
    g_config.scan_menu_state = extract_json_bool("scan_menu_state", true);
    g_config.log_to_file = extract_json_bool("log_to_file", false);

    g_collapsedMem = extract_json_bool("collapsed_mem", false);
    g_collapsedTele = extract_json_bool("collapsed_tele", false);
    g_collapsedSet = extract_json_bool("collapsed_set", false);
    g_collapsedBind = extract_json_bool("collapsed_bind", false);
    g_collapsedLog = extract_json_bool("collapsed_log", false);

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

std::wstring ExpandEnv(const std::wstring& s) {
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

static std::wstring FindProfileInDir(const std::wstring& dir) {
    if (dir.empty()) return L"";
    std::wstring baseDir = dir;
    // Strip trailing slash
    while (!baseDir.empty() && (baseDir.back() == L'\\' || baseDir.back() == L'/')) {
        baseDir.pop_back();
    }
    // If baseDir points to an .exe or file, strip the filename to get parent dir
    DWORD attr = GetFileAttributesW(baseDir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        size_t last_slash = baseDir.find_last_of(L"\\/");
        if (last_slash != std::wstring::npos) {
            baseDir = baseDir.substr(0, last_slash);
        }
    } else {
        size_t last_slash = baseDir.find_last_of(L"\\/");
        if (last_slash != std::wstring::npos) {
            std::wstring filename = baseDir.substr(last_slash + 1);
            if (filename.size() >= 4 && _wcsicmp(filename.c_str() + filename.size() - 4, L".exe") == 0) {
                baseDir = baseDir.substr(0, last_slash);
            }
        }
    }

    // 1. Try controller0.xml
    std::wstring p0 = baseDir + L"\\controllerProfiles\\controller0.xml";
    if (GetFileAttributesW(p0.c_str()) != INVALID_FILE_ATTRIBUTES) return p0;

    // 2. Try Default.xml
    std::wstring pDef = baseDir + L"\\controllerProfiles\\Default.xml";
    if (GetFileAttributesW(pDef.c_str()) != INVALID_FILE_ATTRIBUTES) return pDef;

    // 3. Try any .xml in controllerProfiles folder
    std::wstring searchPattern = baseDir + L"\\controllerProfiles\\*.xml";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        std::wstring foundPath = baseDir + L"\\controllerProfiles\\" + fd.cFileName;
        FindClose(hFind);
        return foundPath;
    }

    return L"";
}

std::wstring ResolveProfilePath(const std::string& custom_path) {
    // 1. Check custom cemu path override
    if (!g_config.cemu_path_override.empty()) {
        std::wstring woverride = Utf8ToWstr(g_config.cemu_path_override);
        std::wstring found = FindProfileInDir(woverride);
        if (!found.empty()) return found;
    }

    // 2. Check explicit custom_path (direct XML file or folder)
    if (!custom_path.empty()) {
        std::wstring wcustom = ExpandEnv(Utf8ToWstr(custom_path));
        DWORD attr = GetFileAttributesW(wcustom.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) {
            if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                return wcustom; // Direct XML file
            }
            std::wstring found = FindProfileInDir(wcustom);
            if (!found.empty()) return found;
        }
    }

    // 3. Check directory of current executable (companion exe)
    wchar_t exe_path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH)) {
        std::wstring found = FindProfileInDir(exe_path);
        if (!found.empty()) return found;
    }

    // 4. Check %APPDATA%\Cemu
    std::wstring appdata_cemu = ExpandEnv(L"%APPDATA%\\Cemu");
    std::wstring found = FindProfileInDir(appdata_cemu);
    if (!found.empty()) return found;

    return L"";
}