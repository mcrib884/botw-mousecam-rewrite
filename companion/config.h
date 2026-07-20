#pragma once
// config.h — AppConfig struct + manual JSON load/save + theme reg query + Cemu profile path resolution.
// Extracted from companion/main.cpp. No global state beyond g_config.

#include <string>
#include <cstdint>

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
    bool cemu_experimental = false;
    int  magnesis_speed_mode = 0; // 0=Vanilla, 1=Extended, 2=Unlimited
};

extern AppConfig g_config;

// Manual JSON helpers (kept dependency-free for trivial distribution).
std::string extract_tag(const std::string& s, const std::string& open, const std::string& close, size_t start_pos = 0);
bool IsWindowsLightTheme();
std::string GetConfigPath();
void SaveConfig();
std::string UnescapeJsonString(const std::string& s);
void LoadConfig();

// Environment-variable expansion for Cemu profile path resolution.
std::wstring ExpandEnv(const std::wstring& s);
std::wstring ResolveProfilePath(const std::string& custom_path);