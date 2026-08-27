#pragma once
// console.h — Rich-edit console control, status text, tooltip.
// Extracted from companion/main.cpp. Owns g_hConsoleEdit (richedit HWND).

#include <Windows.h>
#include <string>

// Rich-edit child window. Created in main.cpp wWinMain, repositioned by UpdateConsoleEditPosition.
extern HWND g_hConsoleEdit;

// Status line shown in the CONNECTION panel. Set via SetStatus.
extern std::wstring g_statusText;

// Tooltip control. Created in WndProc WM_CREATE. Used by ShowTooltip.
extern HWND g_hTooltip;
extern const wchar_t* g_tooltipText;
extern bool g_tooltipActive;

void LogToConsole(const wchar_t* format, ...);
void ClearConsole();
void ClearLogFile();
void UpdateConsoleEditPosition(HWND hWnd);
void ShowTooltip(HWND hWnd, const wchar_t* text, int x, int y);
void SetStatus(const wchar_t* msg);