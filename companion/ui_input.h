#pragma once
#include <windows.h>
#include <gdiplus.h>
using namespace Gdiplus;

extern int g_hoverDropdown;
extern int g_openDropdown;
extern float g_dragSlider;

extern bool g_hoverInject, g_hoverReinject, g_hoverReset, g_hoverToggleCam;
extern bool g_hoverPath, g_hoverPathReset, g_hoverDarkBtn, g_hoverLightBtn;
extern bool g_downPath;
extern bool g_hoverScrollHelper, g_hoverOrbitCam, g_hoverIndepSens, g_hoverIndepMagneSens, g_hoverCemuExperimental;
extern bool g_hoverSensH, g_hoverSensV;
extern bool g_hoverMagneSens, g_hoverMagneSensV;
extern bool g_hoverMagnePullSens;
extern bool g_hoverFpsMagnesis, g_hoverFpsMagneEyeHeight, g_hoverFpsMagneOffsetForward, g_hoverFpsMagneOffsetSide;
extern bool g_hoverClearLog;
extern Rect g_clearLogRect;

extern bool g_downInject, g_downReinject, g_downReset, g_downToggleCam;

extern float g_animInject, g_animReinject, g_animReset, g_animToggleCam;
extern float g_animDarkBtn, g_animLightBtn, g_animPath, g_animPathReset;
extern float g_animScrollHelper, g_animOrbitCam, g_animIndepSens, g_animIndepMagneSens, g_animCemuExperimental;
extern float g_animToggleMagneTarget, g_animToggleShortcutMenu, g_animToggleMenuState;
extern float g_animSensH, g_animSensV, g_animClearLog;
extern float g_animMagneSens, g_animMagneSensV;
extern float g_animMagneSpeedBtn[3];
extern bool  g_hoverMagneSpeedBtn[3];
extern float g_animMagnePullSens;
extern float g_animFpsMagnesis, g_animFpsMagneEyeHeight, g_animFpsMagneOffsetForward, g_animFpsMagneOffsetSide;
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