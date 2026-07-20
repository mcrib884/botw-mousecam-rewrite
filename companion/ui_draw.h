#pragma once
#include <gdiplus.h>
using namespace Gdiplus;
#include <cstdint>

struct ButtonItem {
    const char* name;
    uint32_t val;
};

extern ButtonItem GAMEPAD_BUTTONS[18];
extern ButtonItem PRO_BUTTONS[18];

void DrawRoundedRect(Graphics& g, Rect r, int radius, Pen* pen, Brush* brush);
void DrawToggle(Graphics& g, int x, int y, float stateAnim, const wchar_t* label, FontFamily& ff, bool disabled);
void DrawSlider(Graphics& g, int x, int y, int width, float value, float min, float max, float hoverAnim, const wchar_t* label, FontFamily& ff);
void DrawDropdown(Graphics& g, int x, int y, int width, const wchar_t* label, uint32_t selectedVal, int dropdownIdx, float hoverAnim, FontFamily& ff);