#include <windows.h>
#include "ui_layout.h"
#include "console.h"

extern int g_openDropdown;

const int WND_W = 580;
const int WND_H = 910;
const float SENS_MIN = 0.1f;
const float SENS_MAX = 10.0f;
const float MAGNE_SENS_MIN = 0.01f;
const float MAGNE_SENS_MAX = 2.0f;

bool g_collapsedMem = false;
bool g_collapsedTele = false;
bool g_collapsedSet = false;
bool g_collapsedBind = false;
bool g_collapsedLog = false;

static UIRects g_cachedUIRects;
static int g_cachedWidth = 0;
static int g_cachedHeight = 0;
static bool g_cachedIndepSens = false;
static bool g_cachedIndepMagneSens = false;
static bool g_cachedFpsMagne = false;
static bool g_cachedCollapsedMem = false;
static bool g_cachedCollapsedTele = false;
static bool g_cachedCollapsedSet = false;
static bool g_cachedCollapsedBind = false;
static bool g_cachedCollapsedLog = false;

void InvalidateUIRectsCache() {
    g_cachedWidth = 0;
}

void CalculateUIRects(UIRects& r, int w, int h) {
    bool indepSens = g_config.use_independent_sens;
    bool indepMagneSens = g_config.use_independent_magne_sens;
    bool fpsMagne = g_config.fps_magnesis;
    if (w == g_cachedWidth && h == g_cachedHeight &&
        indepSens == g_cachedIndepSens &&
        indepMagneSens == g_cachedIndepMagneSens &&
        fpsMagne == g_cachedFpsMagne &&
        g_openDropdown == -1 &&
        g_collapsedMem == g_cachedCollapsedMem &&
        g_collapsedTele == g_cachedCollapsedTele &&
        g_collapsedSet == g_cachedCollapsedSet &&
        g_collapsedBind == g_cachedCollapsedBind &&
        g_collapsedLog == g_cachedCollapsedLog) {
        r = g_cachedUIRects;
        return;
    }

    int pad = 15;
    int panelW = w - pad * 2;
    int spacing = 10;
    int curY = 10;

    r.rDarkBtn = Rect(w / 2 - 20, 15, 14, 14);
    r.rLightBtn = Rect(w / 2 + 5, 15, 14, 14);

    int connH = 110;
    r.rConnPanel = Rect(pad, curY, panelW, connH);
    r.rInj = Rect(pad + panelW - 260, curY + 70, 80, 26);
    r.rReinj = Rect(pad + panelW - 175, curY + 70, 80, 26);
    r.rRst = Rect(pad + panelW - 90, curY + 70, 80, 26);
    r.rToggleCam = Rect(pad + panelW - 90, curY + 33, 80, 26);
    r.rPath = Rect(pad + 10, curY + 75, 115, 20);
    r.rPathReset = g_config.cemu_path_override.empty() ? Rect(0, 0, 0, 0) : Rect(pad + 130, curY + 75, 18, 20);
    r.rCemuExperimental = Rect(pad + 155, curY + 75, 125, 20);
    r.rStatusDot = Rect(pad + 10, curY + 40, 12, 12);
    curY += connH + spacing;

    int memH = g_collapsedMem ? 30 : 132;
    r.rMemPanel = Rect(pad, curY, panelW, memH);
    if (g_collapsedMem) {
        r.rToggleMagneTarget = Rect(0, 0, 0, 0);
        r.rToggleShortcutMenu = Rect(0, 0, 0, 0);
        r.rToggleMenuState = Rect(0, 0, 0, 0);
    } else {
        r.rToggleMagneTarget = Rect(pad + panelW - 50, curY + 56, 36, 20);
        r.rToggleShortcutMenu = Rect(pad + panelW - 50, curY + 79, 36, 20);
        r.rToggleMenuState = Rect(pad + panelW - 50, curY + 102, 36, 20);
    }
    curY += memH + spacing;

    int teleH = g_collapsedTele ? 30 : 105;
    r.rTelePanel = Rect(pad, curY, panelW, teleH);
    curY += teleH + spacing;

    int nextSetY = curY + 35;
    if (g_collapsedSet) {
        r.rSensH = Rect(0, 0, 0, 0);
        r.rSensV = Rect(0, 0, 0, 0);
        r.rIndepSens = Rect(0, 0, 0, 0);
        r.rScrollHelper = Rect(0, 0, 0, 0);
        r.rOrbitCam = Rect(0, 0, 0, 0);
        r.rMagneSens = Rect(0, 0, 0, 0);
        r.rMagneSensV = Rect(0, 0, 0, 0);
        r.rIndepMagneSens = Rect(0, 0, 0, 0);
        for (int i = 0; i < 3; ++i) r.rMagneSpeedBtn[i] = Rect(0, 0, 0, 0);
        r.rMagnePullSens = Rect(0, 0, 0, 0);
        r.rFpsMagnesis = Rect(0, 0, 0, 0);
        r.rFpsMagneEyeHeight = Rect(0, 0, 0, 0);
        r.rFpsMagneOffsetForward = Rect(0, 0, 0, 0);
        r.rFpsMagneOffsetSide = Rect(0, 0, 0, 0);
    } else {
        // 1. Camera Sensitivity at the very top of Camera Settings
        r.rSensH = Rect(pad + 10, nextSetY, panelW - 40, 24); nextSetY += 30;
        if (indepSens) {
            r.rSensV = Rect(pad + 10, nextSetY, panelW - 40, 24); nextSetY += 30;
        } else {
            r.rSensV = Rect(0, 0, 0, 0);
        }
        r.rIndepSens = Rect(pad + 10, nextSetY, (std::min)(290, panelW - 20), 20); nextSetY += 25;
        r.rScrollHelper = Rect(pad + 10, nextSetY, (std::min)(250, panelW - 20), 20); nextSetY += 25;
        r.rOrbitCam = Rect(pad + 10, nextSetY, (std::min)(175, panelW - 20), 20); nextSetY += 30;

        // 2. Magnesis Sensitivity and controls
        r.rMagneSens = Rect(pad + 10, nextSetY, panelW - 40, 24); nextSetY += 30;
        if (indepMagneSens) {
            r.rMagneSensV = Rect(pad + 10, nextSetY, panelW - 40, 24); nextSetY += 30;
        } else {
            r.rMagneSensV = Rect(0, 0, 0, 0);
        }
        r.rIndepMagneSens = Rect(pad + 10, nextSetY, (std::min)(340, panelW - 20), 20); nextSetY += 30;

        // 3. Three Magnesis Speed buttons (Vanilla, Extended, Unlimited) under Magnesis Sensitivity
        nextSetY += 18;
        int totalBtnW = panelW - 20;
        int btnW = (totalBtnW - 12) / 3;
        r.rMagneSpeedBtn[0] = Rect(pad + 10, nextSetY, btnW, 24);
        r.rMagneSpeedBtn[1] = Rect(pad + 10 + btnW + 6, nextSetY, btnW, 24);
        r.rMagneSpeedBtn[2] = Rect(pad + 10 + (btnW + 6) * 2, nextSetY, totalBtnW - (btnW + 6) * 2, 24);
        nextSetY += 32;

        r.rMagnePullSens = Rect(pad + 10, nextSetY, panelW - 40, 24); nextSetY += 30;

        // 4. FPS Magnesis
        r.rFpsMagnesis = Rect(pad + 10, nextSetY, (std::min)(175, panelW - 20), 20); nextSetY += 30;
        if (fpsMagne) {
            r.rFpsMagneEyeHeight = Rect(pad + 10, nextSetY, panelW - 40, 24); nextSetY += 30;
            r.rFpsMagneOffsetForward = Rect(pad + 10, nextSetY, panelW - 40, 24); nextSetY += 30;
            r.rFpsMagneOffsetSide = Rect(pad + 10, nextSetY, panelW - 40, 24); nextSetY += 30;
        } else {
            r.rFpsMagneEyeHeight = Rect(0, 0, 0, 0);
            r.rFpsMagneOffsetForward = Rect(0, 0, 0, 0);
            r.rFpsMagneOffsetSide = Rect(0, 0, 0, 0);
        }
    }
    int setH = g_collapsedSet ? 30 : (nextSetY - curY + 5);
    r.rSetPanel = Rect(pad, curY, panelW, setH);
    curY += setH + spacing;

    int bindH = g_collapsedBind ? 30 : 120;
    r.rBindPanel = Rect(pad, curY, panelW, bindH);
    if (g_collapsedBind) {
        for (int i = 0; i < 5; i++) r.rDrops[i] = Rect(0, 0, 0, 0);
        r.rDropMenu = Rect(0, 0, 0, 0);
    } else {
        int bw = (panelW - 30) / 2;
        r.rDrops[0] = Rect(pad + 10, curY + 35, bw, 24);
        r.rDrops[1] = Rect(pad + 20 + bw, curY + 35, bw, 24);
        r.rDrops[2] = Rect(pad + 10, curY + 60, bw, 24);
        r.rDrops[3] = Rect(pad + 20 + bw, curY + 60, bw, 24);
        r.rDrops[4] = Rect(pad + 10, curY + 85, bw, 24);
        if (g_openDropdown != -1) {
            int cx = pad + (g_openDropdown == 1 || g_openDropdown == 3 ? 20 + bw : 10) + 80;
            int cy = (g_openDropdown < 2 ? 35 : (g_openDropdown < 4 ? 60 : 85)) + curY;
            r.rDropMenu = Rect(cx, cy + 24, bw - 80, 18 * 18);
        } else {
            r.rDropMenu = Rect(0, 0, 0, 0);
        }
    }
    curY += bindH + spacing;

    int logH = g_collapsedLog ? 30 : (std::max)(10, h - curY - 10);
    r.rLog = Rect(pad, curY, panelW, logH);
    if (g_collapsedLog)
        r.rClearLog = Rect(0, 0, 0, 0);
    else
        r.rClearLog = Rect(pad + panelW - 60, curY + 10, 45, 18);

    g_cachedWidth = w;
    g_cachedHeight = h;
    g_cachedIndepSens = indepSens;
    g_cachedIndepMagneSens = indepMagneSens;
    g_cachedFpsMagne = fpsMagne;
    g_cachedCollapsedMem = g_collapsedMem;
    g_cachedCollapsedTele = g_collapsedTele;
    g_cachedCollapsedSet = g_collapsedSet;
    g_cachedCollapsedBind = g_collapsedBind;
    g_cachedCollapsedLog = g_collapsedLog;
    g_cachedUIRects = r;
}