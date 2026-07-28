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
extern float g_animSensH, g_animSensV, g_animClearLog, g_animCopyLog, g_animMagneSens, g_animMagnePullSens;
extern float g_animFpsMagnesis, g_animFpsMagneEyeHeight, g_animFpsMagneOffsetForward, g_animFpsMagneOffsetSide;
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
    g.FillRectangle(&bgBrush, 0, 0, logicalW, logicalH);

    // P2-3: Cache the three primary fonts across paints. Creating Font objects
    // per paint was 360-750 Font alloc/dealloc cycles per second (3 in
    // PaintWindow plus extra in DrawToggle / DrawSlider / DrawDropdown). GDI+
    // caches the underlying font handle internally, but the C++ Font wrapper
    // still constructs/destructs the object and walks the family list each
    // time. Move them to thread-local statics that live for the app lifetime.
    // The FontFamily is also static (no DPI dependency); Font sizes are in
    // UnitPixel so they don't auto-scale with DPI, but the produced glyphs
    // still render at the correct physical size due to g.ScaleTransform.
    static FontFamily s_ff(L"Segoe UI");
    static Font s_fontSec(&s_ff, 15, FontStyleBold, UnitPixel);
    static Font s_fontBody(&s_ff, 12, FontStyleRegular, UnitPixel);
    static Font s_smallFont(&s_ff, 11, FontStyleRegular, UnitPixel);
    FontFamily& ff = s_ff;
    Font& fontSec = s_fontSec;
    Font& fontBody = s_fontBody;
    Font& smallFont = s_smallFont;

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
    g.DrawString(g_statusText.c_str(), -1, &fontBody, PointF((REAL)(pad + 28), (REAL)(ui.rConnPanel.Y + 38)), &textBrush);

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
        // P3-8: Cache the UTF-8 -> wide conversion + last-slash slice so we
        // don't redo it on every paint (this fires 30-125 times/sec when idle
        // and is a measurable chunk of per-frame CPU). The cache invalidates
        // when the override string mutates.
        static std::string s_cachedOverrideUtf8;
        static std::wstring s_cachedOverrideExeName;
        if (s_cachedOverrideUtf8 != g_config.cemu_path_override) {
            s_cachedOverrideUtf8 = g_config.cemu_path_override;
            std::wstring full = Utf8ToWstr(s_cachedOverrideUtf8);
            size_t lastSlash = full.find_last_of(L"\\/");
            s_cachedOverrideExeName = (lastSlash != std::wstring::npos) ? full.substr(lastSlash + 1) : full;
        }
        std::wstring txt = L"Target: " + s_cachedOverrideExeName;
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
        // P2-3: cache the bold 12px font used for the ✕ glyph.
        static Font s_resetFont(&ff, 12, FontStyleBold, UnitPixel);
        g.DrawString(L"\u2715", -1, &s_resetFont, RectF((REAL)ui.rPathReset.X, (REAL)ui.rPathReset.Y, (REAL)ui.rPathReset.Width, (REAL)ui.rPathReset.Height), &sfCenter, &resetText);
    }

    bool isExpDisabled = g_targetInjected;
    DrawToggle(g, ui.rCemuExperimental.X, ui.rCemuExperimental.Y, g_animCemuExperimental, L"Experimental", ff, isExpDisabled);

    // MEMORY ADDRESSES
    DrawRoundedRect(g, ui.rMemPanel, 8, &borderPen, &panelBrush);
    const wchar_t* memTitle = g_collapsedMem ? L"\u25b6  MEMORY ADDRESSES" : L"\u25bc  MEMORY ADDRESSES";
    g.DrawString(memTitle, -1, &fontSec, PointF((REAL)(pad + 10), (REAL)(ui.rMemPanel.Y + 8)), &textBrush);
    if (!g_collapsedMem) {
        // IM-6: Show the actual hex address of each found signature so the user
        // can confirm the scanner's hit location, compare across runs, or paste
        // into a debugger. Previously only "Found" / "Scanning..." was shown,
        // obscuring whether the same address was reused across reinstalls.
        auto getAddrStr = [](uintptr_t addr, const wchar_t* foundStr) -> std::wstring {
            if (!g_targetInjected) return L"Not injected yet";
            if (addr == 0) return L"Scanning...";
            wchar_t buf[64];
            swprintf_s(buf, L"%s (0x%016llX)", foundStr, (unsigned long long)addr);
            return std::wstring(buf);
        };
        auto drawMemLine = [&](const wchar_t* label, const std::wstring& val, int yOffset) {
            g.DrawString(label, -1, &fontBody, PointF((REAL)(pad + 10), (REAL)(ui.rMemPanel.Y + yOffset)), &textBrush);
            g.DrawString(val.c_str(), -1, &fontBody, PointF((REAL)(pad + 140), (REAL)(ui.rMemPanel.Y + yOffset)), &textBrush);
        };
        drawMemLine(L"GameRomCamera:", getAddrStr(g_addrGameRomCamera, L"Found"), 35);
        if (g_targetInjected) {
            wchar_t wbuf[64];
            swprintf_s(wbuf, L"Writers found: %u", g_writersFound);
            g.DrawString(wbuf, -1, &fontBody, PointF((REAL)(pad + 250), (REAL)(ui.rMemPanel.Y + 35)), &textBrush);
        }
        drawMemLine(L"Magne Target:", getAddrStr(g_addrMagneTarget, g_magneDetourActive ? L"NOP'd" : L"Found"), 50);
        wchar_t valBuf1[64], valBuf2[64];
        swprintf_s(valBuf1, L"Value: %d", g_liveShortcutMenu);
        if (g_liveMenuState == 3 || g_liveMenuState == 5) {
            swprintf_s(valBuf2, L"Value: %d (In World)", g_liveMenuState);
        } else if (g_liveMenuState == 6 || g_liveMenuState == 10) {
            swprintf_s(valBuf2, L"Value: %d (In Menu)", g_liveMenuState);
        } else {
            swprintf_s(valBuf2, L"Value: %d", g_liveMenuState);
        }
        drawMemLine(L"Shortcut Menu:", getAddrStr(g_addrShortcutMenu, valBuf1), 65);
        drawMemLine(L"Menu State:", getAddrStr(g_addrMenuState, valBuf2), 80);
    }

    // VECTORS
    DrawRoundedRect(g, ui.rTelePanel, 8, &borderPen, &panelBrush);
    const wchar_t* teleTitle = g_collapsedTele ? L"\u25b6  VECTORS" : L"\u25bc  VECTORS";
    g.DrawString(teleTitle, -1, &fontSec, PointF((REAL)(pad + 10), (REAL)(ui.rTelePanel.Y + 8)), &textBrush);
    if (!g_collapsedTele) {
        auto drawVecLine = [&](const wchar_t* label, float vx, float vy, float vz, int yOffset, int numXOffset = 115) {
            g.DrawString(label, -1, &fontBody, PointF((REAL)(pad + 10), (REAL)(ui.rTelePanel.Y + yOffset)), &textBrush);
            wchar_t xb[32], yb[32], zb[32];
            swprintf_s(xb, L"X: %.2f", vx);
            swprintf_s(yb, L"Y: %.2f", vy);
            swprintf_s(zb, L"Z: %.2f", vz);
            g.DrawString(xb, -1, &fontBody, PointF((REAL)(pad + numXOffset), (REAL)(ui.rTelePanel.Y + yOffset)), &textBrush);
            g.DrawString(yb, -1, &fontBody, PointF((REAL)(pad + numXOffset + 95), (REAL)(ui.rTelePanel.Y + yOffset)), &textBrush);
            g.DrawString(zb, -1, &fontBody, PointF((REAL)(pad + numXOffset + 190), (REAL)(ui.rTelePanel.Y + yOffset)), &textBrush);
        };
        float pX = g_pSharedMemory ? g_pSharedMemory->m_telePivotX : 0.0f;
        float pY = g_pSharedMemory ? g_pSharedMemory->m_telePivotY : 0.0f;
        float pZ = g_pSharedMemory ? g_pSharedMemory->m_telePivotZ : 0.0f;
        drawVecLine(L"Player Position:", pX, pY, pZ, 35, 115);
        drawVecLine(L"Focus:", g_liveCamFocX, g_liveCamFocY, g_liveCamFocZ, 50, 115);
        g.DrawString(L"FOV:", -1, &fontBody, PointF((REAL)(pad + 10), (REAL)(ui.rTelePanel.Y + 65)), &textBrush);
        wchar_t fovb[32];
        swprintf_s(fovb, L"%.2f\x00B0", g_liveCamFOV);
        g.DrawString(fovb, -1, &fontBody, PointF((REAL)(pad + 115), (REAL)(ui.rTelePanel.Y + 65)), &textBrush);

        // P3-9: Show "(released)" instead of snapping to 0,0,0 when magnesis
        // detour deactivates. The DLL already exposes g_hasDeactSpeed but the
        // target XYZ is lost on release — keep last known values for display
        // until detour resumes, so the user doesn't see a glitch-snap to zero.
        static float s_dispMagneX = 0.0f, s_dispMagneY = 0.0f, s_dispMagneZ = 0.0f;
        static bool s_dispMagneValid = false;
        float mX = 0, mY = 0, mZ = 0;
        bool showReleased = false;
        if (g_magneDetourActive) {
            mX = g_liveMagneTargetX;
            mY = g_liveMagneTargetY;
            mZ = g_liveMagneTargetZ;
            s_dispMagneX = mX;
            s_dispMagneY = mY;
            s_dispMagneZ = mZ;
            s_dispMagneValid = true;
        } else if (s_dispMagneValid) {
            // Detour released — keep the last known values visible so the user
            // sees a stable "where the object was" rather than a flicker to zero.
            mX = s_dispMagneX;
            mY = s_dispMagneY;
            mZ = s_dispMagneZ;
            showReleased = true;
        }
        if (showReleased) {
            SolidBrush releasedBrush(g_theme.textMuted);
            g.DrawString(L"MTarget (released):", -1, &fontBody, PointF((REAL)(pad + 10), (REAL)(ui.rTelePanel.Y + 80)), &releasedBrush);
        } else {
            drawVecLine(L"MTarget:", mX, mY, mZ, 80, 115);
        }
    }

    // CAMERA SETTINGS
    DrawRoundedRect(g, ui.rSetPanel, 8, &borderPen, &panelBrush);
    const wchar_t* setTitle = g_collapsedSet ? L"\u25b6  CAMERA SETTINGS" : L"\u25bc  CAMERA SETTINGS";
    g.DrawString(setTitle, -1, &fontSec, PointF((REAL)(pad + 10), (REAL)(ui.rSetPanel.Y + 8)), &textBrush);
    if (!g_collapsedSet) {
        DrawToggle(g, ui.rScrollHelper.X, ui.rScrollHelper.Y, g_animScrollHelper, L"Scroll Wheel Weapon Select", ff, false);
        DrawToggle(g, ui.rOrbitCam.X, ui.rOrbitCam.Y, g_animOrbitCam, L"Full Orbit Camera", ff, false);
        DrawToggle(g, ui.rIndepSens.X, ui.rIndepSens.Y, g_animIndepSens, L"Independent Vertical Sensitivity", ff, false);
        DrawToggle(g, ui.rIndepMagneSens.X, ui.rIndepMagneSens.Y, g_animIndepMagneSens, L"Independent Vertical Sensitivity (Magnesis)", ff, false);
        DrawSlider(g, ui.rSensH.X, ui.rSensH.Y, ui.rSensH.Width, g_config.sensitivity_x, SENS_MIN, SENS_MAX, g_animSensH, g_config.use_independent_sens ? L"Sensitivity (H)" : L"Sensitivity & Speed", ff);
        if (g_config.use_independent_sens)
            DrawSlider(g, ui.rSensV.X, ui.rSensV.Y, ui.rSensV.Width, g_config.sensitivity_y, SENS_MIN, SENS_MAX, g_animSensV, L"Sensitivity (V)", ff);

        // IM-10: Segmented control for magnesis speed. Three button-like segments
        // (Vanilla | Extended | Unlimited) with the active one highlighted. The
        // previous single-pill click-cycle forced the user to experiment to
        // discover modes and never showed the full set at once.
        {
            const wchar_t* modeLabels[] = { L"Vanilla", L"Extended", L"Unlimited" };
            int mode = g_config.magnesis_speed_mode;
            if (mode < 0 || mode > 2) mode = 0;

            Color segColors[] = { Color(255, 80, 180, 80), Color(255, 220, 160, 40), Color(255, 200, 60, 60) };
            int segW = (ui.rMagneSpeedMode.Width - 4) / 3;  // 2 px gap between segments
            static Font s_segFont(&ff, 12, FontStyleRegular, UnitPixel);
            Font segFontBold(&ff, 12, FontStyleBold, UnitPixel);
            for (int s = 0; s < 3; ++s) {
                Rect seg(ui.rMagneSpeedMode.X + 2 + s * (segW + 2), ui.rMagneSpeedMode.Y + 3, segW, ui.rMagneSpeedMode.Height - 6);
                const wchar_t* label = modeLabels[s];
                Color fill = (s == mode) ? segColors[s] : g_theme.bg;
                SolidBrush segBrush(fill);
                Pen segPen(g_theme.border);
                int r = (s == mode) ? 5 : 4;
                DrawRoundedRect(g, seg, r, &segPen, &segBrush);
                SolidBrush labelBrush(s == mode ? Color(255, 255, 255, 255) : g_theme.text);
                StringFormat sfSeg;
                sfSeg.SetAlignment(StringAlignmentCenter);
                sfSeg.SetLineAlignment(StringAlignmentCenter);
                if (s == mode) {
                    g.DrawString(label, -1, &segFontBold, RectF((REAL)seg.X, (REAL)seg.Y, (REAL)seg.Width, (REAL)seg.Height), &sfSeg, &labelBrush);
                } else {
                    g.DrawString(label, -1, &s_segFont, RectF((REAL)seg.X, (REAL)seg.Y, (REAL)seg.Width, (REAL)seg.Height), &sfSeg, &labelBrush);
                }
            }
        }

        DrawSlider(g, ui.rMagneSens.X, ui.rMagneSens.Y, ui.rMagneSens.Width, g_config.magnesis_sensitivity, MAGNE_SENS_MIN, MAGNE_SENS_MAX, g_animMagneSens, g_config.use_independent_magne_sens ? L"Magnesis Sensitivity (H)" : L"Magnesis Sensitivity", ff);

        if (g_config.use_independent_magne_sens)
            DrawSlider(g, ui.rMagneSensV.X, ui.rMagneSensV.Y, ui.rMagneSensV.Width, g_config.magnesis_sensitivity_y, MAGNE_SENS_MIN, MAGNE_SENS_MAX, g_animMagneSensV, L"Magnesis Sensitivity (V)", ff);

        DrawSlider(g, ui.rMagnePullSens.X, ui.rMagnePullSens.Y, ui.rMagnePullSens.Width, g_config.magnesis_pull_sensitivity, 1.0f, 10.0f, g_animMagnePullSens, L"Magnesis Pull Sensitivity", ff);

        DrawToggle(g, ui.rFpsMagnesis.X, ui.rFpsMagnesis.Y, g_animFpsMagnesis, L"FPS Magnesis", ff, false);

        if (g_config.fps_magnesis) {
            wchar_t eyeLabel[64], fwdLabel[64], sideLabel[64];
            swprintf_s(eyeLabel, L"FPS Eye Height: %.2fm", g_config.fps_magne_eye_height);
            swprintf_s(fwdLabel, L"FPS Forward Offset: %.2fm", g_config.fps_magne_offset_forward);
            swprintf_s(sideLabel, L"FPS Side Offset: %.2fm", g_config.fps_magne_offset_side);

            DrawSlider(g, ui.rFpsMagneEyeHeight.X, ui.rFpsMagneEyeHeight.Y, ui.rFpsMagneEyeHeight.Width, g_config.fps_magne_eye_height, -2.0f, 5.0f, g_animFpsMagneEyeHeight, eyeLabel, ff);
            DrawSlider(g, ui.rFpsMagneOffsetForward.X, ui.rFpsMagneOffsetForward.Y, ui.rFpsMagneOffsetForward.Width, g_config.fps_magne_offset_forward, -5.0f, 5.0f, g_animFpsMagneOffsetForward, fwdLabel, ff);
            DrawSlider(g, ui.rFpsMagneOffsetSide.X, ui.rFpsMagneOffsetSide.Y, ui.rFpsMagneOffsetSide.Width, g_config.fps_magne_offset_side, -5.0f, 5.0f, g_animFpsMagneOffsetSide, sideLabel, ff);
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
        // AD-1: Copy log to clipboard button.
        {
            Color copyNormal = g_theme.accent;
            Color copyHover = Color(255, 70, 100, 150);
            SolidBrush copyBg(LerpColor(copyNormal, copyHover, g_animCopyLog));
            DrawRoundedRect(g, ui.rCopyLog, 3, nullptr, &copyBg);
            SolidBrush copyText(Color(255, 255, 255, 255));
            g.DrawString(L"Copy", -1, &smallFont, RectF((REAL)ui.rCopyLog.X, (REAL)ui.rCopyLog.Y, (REAL)ui.rCopyLog.Width, (REAL)ui.rCopyLog.Height), &sfCenter, &copyText);
        }
        // P3-1: Removed dead SetClip/ResetClip pair — the rich-edit child window
        // already clips its own painting, and these calls were back-to-back no-ops.
    }

    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBitmap);
    DeleteDC(memDC);
    EndPaint(hWnd, &ps);
}