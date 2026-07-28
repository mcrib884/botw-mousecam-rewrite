// theme.cpp — Theme palette + dark/light lerp + DWM dark-mode attribute.
// Phase 5: Expanded from 2 themes (dark/light) to 7 presets.

#define NOMINMAX
#include <Windows.h>
#include <dwmapi.h>
#include <richedit.h>
#include <cmath>
#include "theme.h"

#pragma comment(lib, "dwmapi.lib")

ThemeColors g_theme;
float g_animTheme = -1.0f;

Color LerpColor(Color a, Color b, float t) {
    return Color(255, a.GetR() + (b.GetR() - a.GetR()) * t,
                        a.GetG() + (b.GetG() - a.GetG()) * t,
                        a.GetB() + (b.GetB() - a.GetB()) * t);
}

// W3C relative luminance: sRGB -> linear, then weighted sum.
double ColorLuminance(BYTE r, BYTE g, BYTE b) {
    auto linear = [](BYTE v) {
        double s = v / 255.0;
        return s <= 0.03928 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(r) + 0.7152 * linear(g) + 0.0722 * linear(b);
}

Color GetContrastTextColor(Color bg) {
    // Threshold 0.5 on relative luminance: > 0.5 -> dark text, else light text.
    // This works for all the preset accents (Dark/Light/Nord/Solarized/Catppuccin/
    // Gruvbox/Tokyo Night) and any custom color the user might add later.
    double lum = ColorLuminance(bg.GetR(), bg.GetG(), bg.GetB());
    if (lum > 0.5) return Color(255, 20, 20, 24);
    return Color(255, 245, 245, 248);
}

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// Phase 5: Pre-defined color palettes for all 7 theme presets.
// Preset 0 = Classic Dark, 1 = Classic Light, 2 = Nord, 3 = Solarized Dark,
// 4 = Catppuccin Mocha, 5 = Gruvbox Dark, 6 = Tokyo Night.
static ThemeColors GetPresetColors(int preset) {
    switch (preset) {
    case 0: // Classic Dark
        return {
            Color(255, 13, 17, 23),   // bg
            Color(255, 22, 27, 34),   // panel
            Color(255, 48, 54, 61),   // border
            Color(255, 47, 129, 247), // accent
            Color(255, 201, 209, 217),// text
            Color(255, 139, 148, 158),// textMuted
            Color(255, 35, 134, 54),  // success
            Color(255, 218, 54, 51),  // error
            Color(255, 10, 12, 16)    // consoleBg
        };
    case 1: // Classic Light
        return {
            Color(255, 245, 245, 247), Color(255, 255, 255, 255), Color(255, 220, 220, 225),
            Color(255, 0, 102, 204), Color(255, 30, 30, 35), Color(255, 120, 120, 125),
            Color(255, 30, 150, 60), Color(255, 220, 40, 40), Color(255, 220, 222, 228)
        };
    case 2: // Nord
        return {
            Color(255, 46, 52, 64),   // bg         (nord0)
            Color(255, 59, 66, 82),   // panel      (nord1)
            Color(255, 76, 86, 106),  // border     (nord2)
            Color(255, 136, 192, 208),// accent     (nord8)
            Color(255, 216, 222, 233),// text       (nord4)
            Color(255, 129, 161, 193),// textMuted  (nord9)
            Color(255, 163, 190, 140),// success    (nord14)
            Color(255, 191, 97, 106), // error      (nord11)
            Color(255, 36, 40, 51)    // consoleBg  (nord0-dark)
        };
    case 3: // Solarized Dark
        return {
            Color(255, 0, 43, 54),    // bg         (base03)
            Color(255, 7, 54, 66),    // panel      (base02)
            Color(255, 88, 110, 117), // border     (base01)
            Color(255, 38, 139, 210), // accent     (blue)
            Color(255, 131, 148, 150),// text       (base0)
            Color(255, 101, 123, 131),// textMuted  (base00)
            Color(255, 133, 153, 0),  // success    (green)
            Color(255, 220, 50, 47),  // error      (red)
            Color(255, 0, 33, 43)     // consoleBg
        };
    case 4: // Catppuccin Mocha
        return {
            Color(255, 30, 30, 46),   // bg         (base)
            Color(255, 49, 50, 68),   // panel      (surface0)
            Color(255, 69, 71, 90),   // border     (surface1)
            Color(255, 137, 180, 250),// accent     (blue)
            Color(255, 205, 214, 244),// text       (text)
            Color(255, 108, 112, 134),// textMuted  (overlay0)
            Color(255, 166, 227, 161),// success    (green)
            Color(255, 243, 139, 168),// error      (red)
            Color(255, 24, 24, 37)    // consoleBg
        };
    case 5: // Gruvbox Dark
        return {
            Color(255, 40, 40, 40),   // bg         (bg)
            Color(255, 50, 48, 47),   // panel      (bg1)
            Color(255, 60, 56, 54),   // border     (bg3)
            Color(255, 131, 165, 152),// accent     (aqua)
            Color(255, 235, 219, 178),// text       (fg)
            Color(255, 146, 131, 116),// textMuted  (fg3)
            Color(255, 184, 187, 38), // success    (green)
            Color(255, 251, 73, 52),  // error      (red)
            Color(255, 29, 32, 33)    // consoleBg  (bg0_h)
        };
    case 6: // Tokyo Night
        return {
            Color(255, 26, 27, 38),   // bg
            Color(255, 36, 40, 59),   // panel
            Color(255, 56, 62, 90),   // border
            Color(255, 122, 162, 247),// accent
            Color(255, 169, 177, 214),// text
            Color(255, 86, 95, 137),  // textMuted
            Color(255, 158, 206, 106),// success
            Color(255, 247, 118, 142),// error
            Color(255, 22, 23, 32)    // consoleBg
        };
    default:
        return GetPresetColors(0);
    }
}

const wchar_t* GetThemePresetName(int preset) {
    switch (preset) {
    case 0: return L"Dark";      case 1: return L"Light";
    case 2: return L"Nord";      case 3: return L"Solarized";
    case 4: return L"Catppuccin";case 5: return L"Gruvbox";
    case 6: return L"Tokyo Night";
    default: return L"Unknown";
    }
}

void ApplyTheme() {
    if (g_hWnd) {
        // All presets except 1 (light) use dark DWM title bar.
        BOOL dark = (g_config.theme_preset != 1);
        DwmSetWindowAttribute(g_hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }

    // Phase 5: use preset colors directly (no lerp for presets 2-6; classic
    // 0/1 still use the lerp for compatibility).
    int p = g_config.theme_preset;
    if (p == 0 || p == 1) {
        // Classic dark/light: lerp between the two using g_animTheme.
        ThemeColors light = GetPresetColors(1);
        ThemeColors dark  = GetPresetColors(0);
        float t = g_animTheme == -1.0f ? (g_config.use_light_theme ? 0.0f : 1.0f) : g_animTheme;
        g_theme.bg        = LerpColor(light.bg,        dark.bg,        t);
        g_theme.panel     = LerpColor(light.panel,     dark.panel,     t);
        g_theme.border    = LerpColor(light.border,    dark.border,    t);
        g_theme.accent    = LerpColor(light.accent,    dark.accent,    t);
        g_theme.text      = LerpColor(light.text,      dark.text,      t);
        g_theme.textMuted = LerpColor(light.textMuted, dark.textMuted, t);
        g_theme.success   = LerpColor(light.success,   dark.success,   t);
        g_theme.error     = LerpColor(light.error,     dark.error,     t);
        g_theme.consoleBg = LerpColor(light.consoleBg, dark.consoleBg, t);
    } else {
        // Themed presets: apply directly, no lerp.
        g_theme = GetPresetColors(p);
    }

    if (g_hConsoleEdit) {
        COLORREF bg = RGB(g_theme.consoleBg.GetR(), g_theme.consoleBg.GetG(), g_theme.consoleBg.GetB());
        SendMessageW(g_hConsoleEdit, EM_SETBKGNDCOLOR, 0, bg);

        CHARFORMATW cf = {};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = RGB(g_theme.text.GetR(), g_theme.text.GetG(), g_theme.text.GetB());
        SendMessageW(g_hConsoleEdit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
        SendMessageW(g_hConsoleEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);

        InvalidateRect(g_hConsoleEdit, nullptr, TRUE);
    }
}
