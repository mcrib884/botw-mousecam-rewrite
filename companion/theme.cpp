// theme.cpp — Theme palette + dark/light lerp + DWM dark-mode attribute.
// Extracted verbatim from companion/main.cpp lines 263-323.

#define NOMINMAX
#include <Windows.h>
#include <dwmapi.h>
#include <richedit.h>
#include "theme.h"

#pragma comment(lib, "dwmapi.lib")

ThemeColors g_theme;
float g_animTheme = -1.0f;

Color LerpColor(Color a, Color b, float t) {
    return Color(255, a.GetR() + (b.GetR() - a.GetR()) * t,
                        a.GetG() + (b.GetG() - a.GetG()) * t,
                        a.GetB() + (b.GetB() - a.GetB()) * t);
}

void ApplyTheme() {
    if (g_hWnd) {
        BOOL dark = !g_config.use_light_theme;
        DwmSetWindowAttribute(g_hWnd, 20, &dark, sizeof(dark));
    }
    ThemeColors lightTheme = {
        Color(255, 245, 245, 247), Color(255, 255, 255, 255), Color(255, 220, 220, 225),
        Color(255, 0, 102, 204), Color(255, 30, 30, 35), Color(255, 120, 120, 125),
        Color(255, 30, 150, 60), Color(255, 220, 40, 40), Color(255, 220, 222, 228)
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