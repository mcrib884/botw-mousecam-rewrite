#pragma once
#include <windows.h>
#include <gdiplus.h>
using namespace Gdiplus;

extern int g_hoverDropdown;
extern int g_openDropdown;
extern float g_dragSlider;

extern bool g_hoverInject, g_hoverReinject, g_hoverReset;
extern bool g_hoverPath, g_hoverPathReset, g_hoverDarkBtn, g_hoverLightBtn;
extern bool g_downPath;
extern bool g_hoverScrollHelper, g_hoverOrbitCam, g_hoverIndepSens, g_hoverCemuExperimental;
extern bool g_hoverSensH, g_hoverSensV;
extern bool g_hoverMagneYDeadzone;
extern bool g_hoverMagneSens;
extern bool g_hoverClearLog;
extern Rect g_clearLogRect;

extern bool g_downInject, g_downReinject, g_downReset;

extern float g_animInject, g_animReinject, g_animReset;
extern float g_animDarkBtn, g_animLightBtn, g_animPath, g_animPathReset;
extern float g_animScrollHelper, g_animOrbitCam, g_animIndepSens, g_animCemuExperimental;
extern float g_animSensH, g_animSensV, g_animClearLog;
extern float g_animMagneYDeadzone;
extern float g_animMagneSens;
extern float g_animDrop[5];

extern bool g_trackingMouse;
extern int g_hoverDrop, g_hoverDropMenuRow;

LRESULT CALLBACK DropdownPopupWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT HandleLButtonDown(HWND hWnd, WPARAM wParam, LPARAM lParam);
LRESULT HandleLButtonUp(HWND hWnd, WPARAM wParam, LPARAM lParam);
LRESULT HandleMouseMove(HWND hWnd, WPARAM wParam, LPARAM lParam);
LRESULT HandleMouseWheel(HWND hWnd, WPARAM wParam, LPARAM lParam);
LRESULT HandleMouseLeave(HWND hWnd, WPARAM wParam, LPARAM lParam);
LRESULT HandleKillFocus(HWND hWnd, WPARAM wParam, LPARAM lParam);
LRESULT HandleSetCursor(HWND hWnd, WPARAM wParam, LPARAM lParam);
LRESULT HandleGetMinMaxInfo(HWND hWnd, WPARAM wParam, LPARAM lParam);