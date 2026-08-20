// cemu_key_injector.cpp — Cemu profile parser + mouse-button -> ScanCode event sender.
// Extracted verbatim from companion/main.cpp lines 579-864.

#define NOMINMAX
#include <Windows.h>
#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <chrono>

#include "cemu_key_injector.h"
#include "config.h"     // LoadConfig, AppConfig, extract_tag, ResolveProfilePath
#include "console.h"    // LogToConsole

void CemuKeyInjector::ReloadSettings() {
    ReleaseAll();
    LoadConfig();

    for (int i = 0; i < 5; ++i) {
        mouse_bindings[i] = g_config.mouse_bindings[i];
    }

    LoadCemuProfile(g_config.controller_profile_path);
}

void CemuKeyInjector::LoadCemuProfile(const std::string& custom_path) {
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

    // Strip UTF-8 BOM if present.
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

    LogToConsole(L"[INFO] Loaded %d keyboard mappings from profile: %s", found, profile_path.c_str());
}

int CemuKeyInjector::MouseVk(int idx) {
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

void CemuKeyInjector::Update(HWND hCemuWnd) {
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

void CemuKeyInjector::ReleaseKey(int idx) {
    uint32_t gpid = mouse_bindings[idx];
    if (gpid == 0) return;
    auto it = gamepad_to_key.find(gpid);
    if (it != gamepad_to_key.end()) {
        SendKey(static_cast<uint16_t>(it->second), true);
    }
}

void CemuKeyInjector::ReleaseAll() {
    for (int i = 0; i < 5; ++i) {
        if (prev_pressed[i]) {
            ReleaseKey(i);
            prev_pressed[i] = false;
            press_time_valid[i] = false;
        }
    }
}

void CemuKeyInjector::SendKey(uint16_t keycode, bool up) {
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

uint16_t CemuKeyInjector::GetKeyForGamepadId(uint32_t id) const {
    auto it = gamepad_to_key.find(id);
    if (it != gamepad_to_key.end()) return static_cast<uint16_t>(it->second);
    return 0;
}

uint16_t CemuKeyInjector::GetDpadUpKey() const {
    uint32_t id = is_gamepad ? 11 : 12;
    return GetKeyForGamepadId(id);
}

uint16_t CemuKeyInjector::GetDpadDownKey() const {
    uint32_t id = is_gamepad ? 12 : 13;
    return GetKeyForGamepadId(id);
}

uint16_t CemuKeyInjector::GetDpadLeftKey() const {
    uint32_t id = is_gamepad ? 13 : 14;
    return GetKeyForGamepadId(id);
}

uint16_t CemuKeyInjector::GetDpadRightKey() const {
    uint32_t id = is_gamepad ? 14 : 15;
    return GetKeyForGamepadId(id);
}

uint16_t CemuKeyInjector::GetRstickLeftKey() const {
    return GetKeyForGamepadId(24);
}

uint16_t CemuKeyInjector::GetRstickRightKey() const {
    return GetKeyForGamepadId(25);
}

// Singleton instance.
CemuKeyInjector g_ki;