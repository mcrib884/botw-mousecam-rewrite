#pragma once
// cemu_key_injector.h — Translates mouse-button presses into keyboard ScanCode events
// sent to the focused Cemu window so Breath of the Wild thinks the player is using a
// gamepad. Extracted verbatim from companion/main.cpp lines 579-862.
//
// TD2: The injector class is DUPLICATED in mod_dll (different process), with separate
// input state. The companion instance owns the foreground-key-sending path; the DLL
// owns the in-process hook. They cannot share an instance across processes.

#include <Windows.h>
#include <string>
#include <cstdint>
#include <unordered_map>
#include <chrono>

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

    void ReloadSettings();
    void LoadCemuProfile(const std::string& custom_path);

    // Maps our logical mouse-button index (0..4) to a Win32 virtual-key code.
    static int MouseVk(int idx);

    // Polls GetAsyncKeyState for the 5 mapped mouse buttons and sends press/release
    // events for the corresponding Cemu gamepad id. Only runs while Cemu is foreground.
    void Update(HWND hCemuWnd);

    void ReleaseKey(int idx);
    void ReleaseAll();

    static void SendKey(uint16_t keycode, bool up);

    uint16_t GetKeyForGamepadId(uint32_t id) const;
    uint16_t GetDpadUpKey() const;
    uint16_t GetDpadDownKey() const;
    uint16_t GetDpadLeftKey() const;
    uint16_t GetDpadRightKey() const;
    uint16_t GetRstickLeftKey() const;
    uint16_t GetRstickRightKey() const;
};

// Singleton instance. Defined in cemu_key_injector.cpp.
extern CemuKeyInjector g_ki;