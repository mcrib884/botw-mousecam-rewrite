#pragma once
#include <gdiplus.h>
using namespace Gdiplus;

#include "config.h"

extern const int WND_W;
extern const int WND_H;
extern const float SENS_MIN;
extern const float SENS_MAX;

struct UIRects {
    Rect rConnPanel;
    Rect rMemPanel;
    Rect rTelePanel;
    Rect rSetPanel;
    Rect rBindPanel;

    Rect rInj;
    Rect rReinj;
    Rect rRst;

    Rect rScrollHelper;
    Rect rOrbitCam;
    Rect rIndepSens;
    Rect rCemuExperimental;
    Rect rSensH;
    Rect rSensV;
    Rect rMagneSpeedMode;
    Rect rMagneYDeadzone;
    Rect rMagneSens;

    Rect rDrops[5];
    Rect rDropMenu;

    Rect rPath;
    Rect rPathReset;

    Rect rDarkBtn;
    Rect rLightBtn;

    Rect rStatusDot; // I3: status indicator dot

    Rect rClearLog; // UX6: [Clear] button
    Rect rLog;
};

extern bool g_collapsedMem;
extern bool g_collapsedTele;
extern bool g_collapsedSet;
extern bool g_collapsedBind;
extern bool g_collapsedLog;

void InvalidateUIRectsCache();
void CalculateUIRects(UIRects& r, int w, int h);