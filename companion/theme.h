#pragma once
// theme.h — Theme palette + dark/light lerp + DWM dark-mode attribute.
// Extracted from companion/main.cpp. Depends on GDI+ (Color), config (g_config.use_light_theme),
// and the main window handle g_hWnd (declared here, defined in main.cpp).

#include <Windows.h>
#include <gdiplus.h>
using namespace Gdiplus;

#include "config.h" // for g_config.use_light_theme used by ApplyTheme

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

// Main window handle. Declared here so ApplyTheme can call DwmSetWindowAttribute.
// Defined in main.cpp (HWND g_hWnd = nullptr;).
extern HWND g_hWnd;

// Rich-edit HWND used by ApplyTheme to recolor the console. Defined in console.cpp.
extern HWND g_hConsoleEdit;

extern ThemeColors g_theme;
extern float g_animTheme; // -1.0f until first ApplyTheme, then 0..1

Color LerpColor(Color a, Color b, float t);
void ApplyTheme();