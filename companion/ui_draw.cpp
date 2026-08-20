#include <windows.h>
#include "ui_draw.h"
#include "theme.h"
#include "cemu_key_injector.h"

ButtonItem GAMEPAD_BUTTONS[18] = {
    {"Disabled", 0}, {"A", 1}, {"B", 2}, {"X", 3}, {"Y", 4},
    {"L", 5}, {"R", 6}, {"ZL", 7}, {"ZR", 8}, {"Plus", 9},
    {"Minus", 10}, {"D-Pad Up", 11}, {"D-Pad Down", 12}, {"D-Pad Left", 13}, {"D-Pad Right", 14},
    {"L-Stick Click", 15}, {"R-Stick Click", 16}, {"Home", 19}
};

ButtonItem PRO_BUTTONS[18] = {
    {"Disabled", 0}, {"A", 1}, {"B", 2}, {"X", 3}, {"Y", 4},
    {"L", 5}, {"R", 6}, {"ZL", 7}, {"ZR", 8}, {"Plus", 9},
    {"Minus", 10}, {"Home", 11}, {"D-Pad Up", 12}, {"D-Pad Down", 13}, {"D-Pad Left", 14},
    {"D-Pad Right", 15}, {"L-Stick Click", 16}, {"R-Stick Click", 17}
};

void DrawRoundedRect(Graphics& g, Rect r, int radius, Pen* pen, Brush* brush) {
    GraphicsPath path;
    int d = radius * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
    if (brush) g.FillPath(brush, &path);
    if (pen) g.DrawPath(pen, &path);
}

void DrawToggle(Graphics& g, int x, int y, float stateAnim, const wchar_t* label, FontFamily& ff, bool disabled) {
    Rect r(x, y, 36, 20);
    Color cOff = disabled ? Color(255, 45, 45, 45) : Color(255, 60, 60, 75);
    Color cOn = disabled ? Color(255, 80, 80, 80) : g_theme.accent;
    Color bg = LerpColor(cOff, cOn, stateAnim);
    SolidBrush bgBrush(bg);
    Pen borderPen(g_theme.border);
    DrawRoundedRect(g, r, 10, &borderPen, &bgBrush);

    Color thumbCol = disabled ? Color(255, 120, 120, 120) : Color(255, 255, 255, 255);
    SolidBrush thumbBrush(thumbCol);
    float thumbX = 2 + 16 * stateAnim;
    g.FillEllipse(&thumbBrush, (int)(x + thumbX), y + 2, 16, 16);

    if (label && label[0] != L'\0') {
        Font font(&ff, 12, FontStyleRegular, UnitPixel);
        SolidBrush textBrush(g_theme.text);
        PointF pt((float)(x + 45), (float)(y + 2));
        g.DrawString(label, -1, &font, pt, &textBrush);
    }
}

void DrawSlider(Graphics& g, int x, int y, int width, float value, float min, float max, float hoverAnim, const wchar_t* label, FontFamily& ff) {
    SolidBrush textBrush(g_theme.text);
    Font font(&ff, 12, FontStyleRegular, UnitPixel);
    PointF labelPt((float)x, (float)y);
    g.DrawString(label, -1, &font, labelPt, &textBrush);

    wchar_t valBuf[32];
    swprintf_s(valBuf, L"%.2f", value);
    SolidBrush valBrush(g_theme.textMuted);
    PointF valPt((float)(x + width - 30), (float)y);
    g.DrawString(valBuf, -1, &font, valPt, &valBrush);

    y += 20;
    SolidBrush trackBrush(Color(255, 60, 60, 75));
    g.FillRectangle(&trackBrush, x, y + 4, width, 4);

    float pct = (value - min) / (max - min);
    if (pct < 0) pct = 0;
    if (pct > 1) pct = 1;
    Color fillCol = LerpColor(g_theme.accent, Color(255, 60, 160, 255), hoverAnim);
    SolidBrush fillBrush(fillCol);
    g.FillRectangle(&fillBrush, x, y + 4, (int)(width * pct), 4);
    g.FillEllipse(&fillBrush, x + (int)(width * pct) - 6, y - 2, 12, 12);
}

void DrawDropdown(Graphics& g, int x, int y, int width, const wchar_t* label, uint32_t selectedVal, int dropdownIdx, float hoverAnim, FontFamily& ff) {
    SolidBrush textBrush(g_theme.text);
    Font font(&ff, 12, FontStyleRegular, UnitPixel);
    PointF labelPt((float)x, (float)(y + 4));
    g.DrawString(label, -1, &font, labelPt, &textBrush);

    Rect r(x + 80, y, width - 80, 24);
    Color bg = LerpColor(g_theme.bg, Color(255, 35, 35, 45), hoverAnim);
    SolidBrush bgBrush(bg);
    Pen borderPen(g_theme.border);
    g.FillRectangle(&bgBrush, r);
    g.DrawRectangle(&borderPen, r);

    ButtonItem* buttons = g_ki.is_gamepad ? GAMEPAD_BUTTONS : PRO_BUTTONS;
    const char* selName = "Disabled";
    for (int i = 0; i < 18; i++) {
        if (buttons[i].val == selectedVal) {
            selName = buttons[i].name;
            break;
        }
    }
    wchar_t wselName[32];
    int n = 0;
    for (const char* p = selName; *p && n < 31; ++p, ++n) wselName[n] = (wchar_t)(unsigned char)*p;
    wselName[n] = 0;
    SolidBrush selBrush(g_theme.text);
    PointF selPt((float)(r.X + 5), (float)(r.Y + 4));
    g.DrawString(wselName, -1, &font, selPt, &selBrush);

    Color arrowCol = LerpColor(g_theme.textMuted, Color(255, 200, 200, 200), hoverAnim);
    SolidBrush arrowBrush(arrowCol);
    Point pts[3] = {
        Point(r.X + r.Width - 15, r.Y + 10),
        Point(r.X + r.Width - 5, r.Y + 10),
        Point(r.X + r.Width - 10, r.Y + 15)
    };
    g.FillPolygon(&arrowBrush, pts, 3);
}