#include "ui_paint.h"
#include <gdiplus.h>
using namespace Gdiplus;
#include <string>
#include <cmath>
#include "theme.h"
#include "ui_layout.h"
#include "ui_draw.h"
#include "config.h"
#include "console.h"
#include "injector_ops.h"
#include "shared_memory_manager.h"
#include "ui_input.h"
#include "string_utils.h"

extern float g_animInject, g_animReinject, g_animReset;
extern float g_animDarkBtn, g_animLightBtn, g_animPath, g_animPathReset;
extern float g_animScrollHelper, g_animOrbitCam, g_animIndepSens, g_animCemuExperimental;
extern float g_animSensH, g_animSensV, g_animClearLog, g_animMagneYDeadzone, g_animMagneSens;
extern float g_animDrop[5];
extern bool g_downInject, g_downReinject, g_downReset, g_downPath;

void PaintWindow(HWND hWnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    RECT rc;
    GetClientRect(hWnd, &rc);
    int w = rc.right;
    int h = rc.bottom;
    float dpiScale = GetDpiForWindow(hWnd) / 96.0f;
    if (dpiScale <= 0) dpiScale = 1.0f;
    int logicalW = (int)(w / dpiScale);
    int logicalH = (int)(h / dpiScale);

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBitmap = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(memDC, memBitmap);

    Graphics g(memDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.ScaleTransform(dpiScale, dpiScale);

    SolidBrush bgBrush(g_theme.bg);
    g.FillRectangle(&bgBrush, 0, 0, w, h);

    FontFamily ff(L"Segoe UI");
    Font fontSec(&ff, 15, FontStyleBold, UnitPixel);
    Font fontBody(&ff, 12, FontStyleRegular, UnitPixel);
    Font smallFont(&ff, 11, FontStyleRegular, UnitPixel);

    SolidBrush textBrush(g_theme.text);
    SolidBrush mutedBrush(g_theme.textMuted);
    Pen borderPen(g_theme.border);
    SolidBrush panelBrush(g_theme.panel);
    StringFormat sfCenter;
    sfCenter.SetAlignment(StringAlignmentCenter);
    sfCenter.SetLineAlignment(StringAlignmentCenter);

    UIRects ui;
    CalculateUIRects(ui, logicalW, logicalH);
    int pad = 15;

    // CONNECTION
    DrawRoundedRect(g, ui.rConnPanel, 8, &borderPen, &panelBrush);
    g.DrawString(L"CONNECTION", -1, &fontSec, PointF((REAL)(pad + 10), (REAL)(ui.rConnPanel.Y + 10)), &textBrush);

    Color btnDark = LerpColor(Color(255, 20, 20, 25), Color(255, 50, 50, 55), g_animDarkBtn);
    Color btnLight = LerpColor(Color(255, 235, 235, 240), Color(255, 255, 255, 255), g_animLightBtn);
    Pen activePen(g_theme.accent, 2.0f);
    SolidBrush darkBrush(btnDark);
    g.FillEllipse(&darkBrush, ui.rDarkBtn);
    g.DrawEllipse(g_config.use_light_theme ? &borderPen : &activePen, ui.rDarkBtn);
    SolidBrush lightBrush(btnLight);
    g.FillEllipse(&lightBrush, ui.rLightBtn);
    g.DrawEllipse(g_config.use_light_theme ? &activePen : &borderPen, ui.rLightBtn);

    SolidBrush statusBrush(g_targetInjected ? g_theme.success : g_theme.error);
    g.FillEllipse(&statusBrush, ui.rStatusDot);
    g.DrawString(g_statusText.c_str(), -1, &fontBody, PointF((REAL)(pad + 120), (REAL)(ui.rConnPanel.Y + 11)), &textBrush);

    Color btnInj = g_downInject ? Color(255, 50, 80, 120) : LerpColor(g_theme.accent, Color(255, 70, 100, 150), g_animInject);
    Color btnReinj = g_downReinject ? Color(255, 50, 80, 120) : LerpColor(g_theme.accent, Color(255, 70, 100, 150), g_animReinject);
    Color btnRst = g_downReset ? Color(255, 50, 80, 120) : LerpColor(g_theme.accent, Color(255, 70, 100, 150), g_animReset);
    SolidBrush injBrush(btnInj);
    SolidBrush reinjBrush(btnReinj);
    SolidBrush rstBrush(btnRst);
    DrawRoundedRect(g, ui.rInj, 4, nullptr, &injBrush);
    DrawRoundedRect(g, ui.rReinj, 4, nullptr, g_targetInjected ? &reinjBrush : &mutedBrush);
    DrawRoundedRect(g, ui.rRst, 4, nullptr, g_targetInjected ? &rstBrush : &mutedBrush);
    g.DrawString(g_targetInjected ? L"Disconnect" : L"Connect", -1, &fontBody, RectF((REAL)ui.rInj.X, (REAL)ui.rInj.Y, (REAL)ui.rInj.Width, (REAL)ui.rInj.Height), &sfCenter, &textBrush);
    g.DrawString(L"Reinject", -1, &fontBody, RectF((REAL)ui.rReinj.X, (REAL)ui.rReinj.Y, (REAL)ui.rReinj.Width, (REAL)ui.rReinj.Height), &sfCenter, &textBrush);
    g.DrawString(L"Reset", -1, &fontBody, RectF((REAL)ui.rRst.X, (REAL)ui.rRst.Y, (REAL)ui.rRst.Width, (REAL)ui.rRst.Height), &sfCenter, &textBrush);

    if (!g_config.cemu_path_override.empty()) {
        SolidBrush overrideBrush(g_theme.accent);
        std::wstring exeName = Utf8ToWstr(g_config.cemu_path_override);
        size_t lastSlash = exeName.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) exeName = exeName.substr(lastSlash + 1);
        std::wstring txt = L"Target: " + exeName;
        g.DrawString(txt.c_str(), -1, &fontBody, PointF((REAL)ui.rPath.X, (REAL)(ui.rPath.Y - 18)), &overrideBrush);
    } else {
        SolidBrush defaultBrush(g_theme.textMuted);
        g.DrawString(L"Target: cemu.exe (Default)", -1, &fontBody, PointF((REAL)ui.rPath.X, (REAL)(ui.rPath.Y - 18)), &defaultBrush);
    }
    Color btnPath = g_downPath ? Color(255, 50, 80, 120) : LerpColor(g_theme.panel, Color(255, 70, 100, 150), g_animPath);
    SolidBrush pathBrush(btnPath);
    DrawRoundedRect(g, ui.rPath, 4, &borderPen, &pathBrush);
    g.DrawString(L"Cemu Executable", -1, &fontBody, RectF((REAL)ui.rPath.X, (REAL)ui.rPath.Y, (REAL)ui.rPath.Width, (REAL)ui.rPath.Height), &sfCenter, &textBrush);

    if (!g_config.cemu_path_override.empty()) {
        Color resetBg = LerpColor(Color(255, 60, 30, 30), g_theme.error, g_animPathReset);
        SolidBrush resetBrush(resetBg);
        DrawRoundedRect(g, ui.rPathReset, 3, nullptr, &resetBrush);
        SolidBrush resetText(Color(255, 255, 255, 255));
        Font resetFont(&ff, 12, FontStyleBold, UnitPixel);
        g.DrawString(L"\u2715", -1, &resetFont, RectF((REAL)ui.rPathReset.X, (REAL)ui.rPathReset.Y, (REAL)ui.rPathReset.Width, (REAL)ui.rPathReset.Height), &sfCenter, &resetText);
    }

    bool isExpDisabled = g_targetInjected;
    DrawToggle(g, ui.rCemuExperimental.X, ui.rCemuExperimental.Y, g_animCemuExperimental, L"Experimental", ff, isExpDisabled);

    // MEMORY ADDRESSES
    DrawRoundedRect(g, ui.rMemPanel, 8, &borderPen, &panelBrush);
    const wchar_t* memTitle = g_collapsedMem ? L"\u25b6  MEMORY ADDRESSES" : L"\u25bc  MEMORY ADDRESSES";
    g.DrawString(memTitle, -1, &fontSec, PointF((REAL)(pad + 10), (REAL)(ui.rMemPanel.Y + 8)), &textBrush);
    if (!g_collapsedMem) {
        auto getAddrStr = [](uintptr_t addr, const wchar_t* foundStr) -> std::wstring {
            if (!g_targetInjected) return L"Not injected yet";
            if (addr == 0) return L"Scanning...";
            return std::wstring(foundStr);
        };
        auto drawMemLine = [&](const wchar_t* label, const std::wstring& val, int yOffset) {
            g.DrawString(label, -1, &fontBody, PointF((REAL)(pad + 10), (REAL)(ui.rMemPanel.Y + yOffset)), &textBrush);
            g.DrawString(val.c_str(), -1, &fontBody, PointF((REAL)(pad + 140), (REAL)(ui.rMemPanel.Y + yOffset)), &textBrush);
        };
        drawMemLine(L"GameRomCamera:", getAddrStr(g_addrGameRomCamera, L"Found"), 35);
        drawMemLine(L"Magne Target:", getAddrStr(g_addrMagneTarget, g_magneDetourActive ? L"NOP'd" : L"Found"), 50);
        wchar_t valBuf1[64], valBuf2[64];
        swprintf_s(valBuf1, L"Value: %d", g_liveShortcutMenu);
        swprintf_s(valBuf2, L"Value: %d", g_liveMenuState);
        drawMemLine(L"Shortcut Menu:", getAddrStr(g_addrShortcutMenu, valBuf1), 65);
        drawMemLine(L"Menu State:", getAddrStr(g_addrMenuState, valBuf2), 80);
        wchar_t wbuf[64];
        if (!g_targetInjected) swprintf_s(wbuf, L"Not injected yet");
        else swprintf_s(wbuf, L"%u", g_writersFound);
        drawMemLine(L"Writers NOP'd:", std::wstring(wbuf), 95);
    }

    // VECTORS
    DrawRoundedRect(g, ui.rTelePanel, 8, &borderPen, &panelBrush);
    const wchar_t* teleTitle = g_collapsedTele ? L"\u25b6  VECTORS" : L"\u25bc  VECTORS";
    g.DrawString(teleTitle, -1, &fontSec, PointF((REAL)(pad + 10), (REAL)(ui.rTelePanel.Y + 8)), &textBrush);
    if (!g_collapsedTele) {
        auto drawVecLine = [&](const wchar_t* label, float vx, float vy, float vz, int yOffset) {
            g.DrawString(label, -1, &fontBody, PointF((REAL)(pad + 10), (REAL)(ui.rTelePanel.Y + yOffset)), &textBrush);
            wchar_t xb[32], yb[32], zb[32];
            swprintf_s(xb, L"X: %.2f", vx);
            swprintf_s(yb, L"Y: %.2f", vy);
            swprintf_s(zb, L"Z: %.2f", vz);
            g.DrawString(xb, -1, &fontBody, PointF((REAL)(pad + 100), (REAL)(ui.rTelePanel.Y + yOffset)), &textBrush);
            g.DrawString(yb, -1, &fontBody, PointF((REAL)(pad + 200), (REAL)(ui.rTelePanel.Y + yOffset)), &textBrush);
            g.DrawString(zb, -1, &fontBody, PointF((REAL)(pad + 300), (REAL)(ui.rTelePanel.Y + yOffset)), &textBrush);
        };
        drawVecLine(L"Position:", g_liveCamPosX, g_liveCamPosY, g_liveCamPosZ, 35);
        drawVecLine(L"Focus:", g_liveCamFocX, g_liveCamFocY, g_liveCamFocZ, 50);
        g.DrawString(L"FOV:", -1, &fontBody, PointF((REAL)(pad + 10), (REAL)(ui.rTelePanel.Y + 65)), &textBrush);
        wchar_t fovb[32];
        swprintf_s(fovb, L"%.2f\x00B0", g_liveCamFOV);
        g.DrawString(fovb, -1, &fontBody, PointF((REAL)(pad + 100), (REAL)(ui.rTelePanel.Y + 65)), &textBrush);
        drawVecLine(L"Pivot:",
                    g_pSharedMemory ? g_pSharedMemory->m_telePivotX : 0,
                    g_pSharedMemory ? g_pSharedMemory->m_telePivotY : 0,
                    g_pSharedMemory ? g_pSharedMemory->m_telePivotZ : 0, 80);
        float mX = 0, mY = 0, mZ = 0;
        if (g_magneDetourActive) {
            mX = g_liveMagneTargetX;
            mY = g_liveMagneTargetY;
            mZ = g_liveMagneTargetZ;
        }
        drawVecLine(L"MTarget:", mX, mY, mZ, 95);

        // Magnesis object speedometers (horizontal / vertical)
        auto drawSpeedLine = [&](const wchar_t* label, float speed, float maxSpeed, int yOffset) {
            g.DrawString(label, -1, &fontBody, PointF((REAL)(pad + 10), (REAL)(ui.rTelePanel.Y + yOffset)), &textBrush);
            wchar_t buf[32];
            swprintf_s(buf, L"%.1f", speed);
            g.DrawString(buf, -1, &fontBody, PointF((REAL)(pad + 100), (REAL)(ui.rTelePanel.Y + yOffset)), &textBrush);

            int barX = pad + 160;
            int barY = ui.rTelePanel.Y + yOffset + 5;
            int barW = ui.rTelePanel.Width - barX - pad - 5;
            int barH = 8;
            float t = 0.0f;
            if (maxSpeed > 0.0f) {
                t = std::fabs(speed) / maxSpeed;
                if (t > 1.0f) t = 1.0f;
            }
            int fillW = static_cast<int>(barW * t);
            SolidBrush barBg(g_theme.border);
            g.FillRectangle(&barBg, barX, barY, barW, barH);
            SolidBrush barFill(g_theme.accent);
            g.FillRectangle(&barFill, barX, barY, fillW, barH);
        };
        float hMax = 500.0f, vMax = 200.0f;
        if (g_config.magnesis_speed_mode == 0) { hMax = 50.0f; vMax = 30.0f; }
        else if (g_config.magnesis_speed_mode == 1) { hMax = 150.0f; vMax = 90.0f; }
        drawSpeedLine(L"MSpd H:", g_magneSpeedH, hMax, 110);
        drawSpeedLine(L"MSpd V:", g_magneSpeedV, vMax, 125);
    }

    // CAMERA SETTINGS
    DrawRoundedRect(g, ui.rSetPanel, 8, &borderPen, &panelBrush);
    const wchar_t* setTitle = g_collapsedSet ? L"\u25b6  CAMERA SETTINGS" : L"\u25bc  CAMERA SETTINGS";
    g.DrawString(setTitle, -1, &fontSec, PointF((REAL)(pad + 10), (REAL)(ui.rSetPanel.Y + 8)), &textBrush);
    if (!g_collapsedSet) {
        DrawToggle(g, ui.rScrollHelper.X, ui.rScrollHelper.Y, g_animScrollHelper, L"Scroll Wheel Weapon Select", ff, false);
        DrawToggle(g, ui.rOrbitCam.X, ui.rOrbitCam.Y, g_animOrbitCam, L"Full Orbit Camera", ff, false);
        DrawToggle(g, ui.rIndepSens.X, ui.rIndepSens.Y, g_animIndepSens, L"Independent Vertical Sensitivity", ff, false);
        DrawSlider(g, ui.rSensH.X, ui.rSensH.Y, ui.rSensH.Width, g_config.sensitivity_x, SENS_MIN, SENS_MAX, g_animSensH, g_config.use_independent_sens ? L"Sensitivity (H)" : L"Sensitivity & Speed", ff);
        if (g_config.use_independent_sens)
            DrawSlider(g, ui.rSensV.X, ui.rSensV.Y, ui.rSensV.Width, g_config.sensitivity_y, SENS_MIN, SENS_MAX, g_animSensV, L"Sensitivity (V)", ff);

        DrawSlider(g, ui.rMagneSens.X, ui.rMagneSens.Y, ui.rMagneSens.Width, g_config.magnesis_sensitivity, SENS_MIN, SENS_MAX, g_animMagneSens, L"Magnesis Sensitivity", ff);

        DrawSlider(g, ui.rMagneYDeadzone.X, ui.rMagneYDeadzone.Y, ui.rMagneYDeadzone.Width, g_config.magnesis_y_deadzone, 0.0f, 10.0f, g_animMagneYDeadzone, L"Magnesis V Deadzone", ff);

        // Magnesis speed mode cycle button
        {
            const wchar_t* modeLabels[] = { L"Vanilla", L"Extended", L"Unlimited" };
            int mode = g_config.magnesis_speed_mode;
            if (mode < 0 || mode > 2) mode = 0;

            // Draw a small colored pill indicator
            Color pillColors[] = { Color(255, 80, 180, 80), Color(255, 220, 160, 40), Color(255, 200, 60, 60) };
            SolidBrush pillBrush(pillColors[mode]);
            g.FillEllipse(&pillBrush, ui.rMagneSpeedMode.X, ui.rMagneSpeedMode.Y + 3, 14, 14);

            // Draw label
            wchar_t buf[64];
            swprintf_s(buf, L"  Magnesis Speed: %s", modeLabels[mode]);
            Font font(&ff, 12, FontStyleRegular, UnitPixel);
            SolidBrush textBrush(g_theme.text);
            g.DrawString(buf, -1, &font, PointF((REAL)(ui.rMagneSpeedMode.X + 14), (REAL)(ui.rMagneSpeedMode.Y + 2)), &textBrush);
        }
    }

    // MOUSE BINDINGS
    DrawRoundedRect(g, ui.rBindPanel, 8, &borderPen, &panelBrush);
    const wchar_t* bindTitle = g_collapsedBind ? L"\u25b6  MOUSE BINDINGS" : L"\u25bc  MOUSE BINDINGS";
    g.DrawString(bindTitle, -1, &fontSec, PointF((REAL)(pad + 10), (REAL)(ui.rBindPanel.Y + 8)), &textBrush);
    if (!g_collapsedBind) {
        DrawDropdown(g, ui.rDrops[0].X, ui.rDrops[0].Y, ui.rDrops[0].Width, L"Left:", g_config.mouse_bindings[0], 0, g_animDrop[0], ff);
        DrawDropdown(g, ui.rDrops[1].X, ui.rDrops[1].Y, ui.rDrops[1].Width, L"Right:", g_config.mouse_bindings[1], 1, g_animDrop[1], ff);
        DrawDropdown(g, ui.rDrops[2].X, ui.rDrops[2].Y, ui.rDrops[2].Width, L"Middle:", g_config.mouse_bindings[2], 2, g_animDrop[2], ff);
        DrawDropdown(g, ui.rDrops[3].X, ui.rDrops[3].Y, ui.rDrops[3].Width, L"Mouse 4:", g_config.mouse_bindings[3], 3, g_animDrop[3], ff);
        DrawDropdown(g, ui.rDrops[4].X, ui.rDrops[4].Y, ui.rDrops[4].Width, L"Mouse 5:", g_config.mouse_bindings[4], 4, g_animDrop[4], ff);
    }

    // LOG
    Rect rLog = ui.rLog;
    SolidBrush consoleBrush(g_theme.consoleBg);
    DrawRoundedRect(g, rLog, 8, &borderPen, &consoleBrush);
    const wchar_t* logTitle = g_collapsedLog ? L"\u25b6  LOG" : L"\u25bc  LOG";
    g.DrawString(logTitle, -1, &fontSec, PointF((REAL)(pad + 10), (REAL)(ui.rLog.Y + 8)), &textBrush);
    if (!g_collapsedLog) {
        SolidBrush mutedText(g_theme.textMuted);
#ifdef _DEBUG
        const wchar_t* shortcutsText = L"F2: Toggle Camera | F5: AOB Dump";
#else
        const wchar_t* shortcutsText = L"F2: Toggle Camera";
#endif
        g.DrawString(shortcutsText, -1, &smallFont, PointF((REAL)(pad + 10), (REAL)(ui.rLog.Y + 26)), &mutedText);
        {
            Color clearNormal = g_theme.border;
            Color clearHover = g_config.use_light_theme ? Color(255, 200, 200, 200) : Color(255, 80, 80, 90);
            SolidBrush clearBg(LerpColor(clearNormal, clearHover, g_animClearLog));
            DrawRoundedRect(g, ui.rClearLog, 3, nullptr, &clearBg);
            g.DrawString(L"Clear", -1, &smallFont, RectF((REAL)ui.rClearLog.X, (REAL)ui.rClearLog.Y, (REAL)ui.rClearLog.Width, (REAL)ui.rClearLog.Height), &sfCenter, &textBrush);
        }
        g.SetClip(Rect(rLog.X + 5, rLog.Y + 30, rLog.Width - 10, rLog.Height - 35));
        g.ResetClip();
    }

    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBitmap);
    DeleteDC(memDC);
    EndPaint(hWnd, &ps);
}