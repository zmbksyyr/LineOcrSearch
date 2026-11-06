#include "theme.h"

namespace theme {

static HFONT g_font = nullptr;
static int   g_lineH = 20;
static int   g_dpi = 96;

int Dpi() { return g_dpi; }

int Scale(int v) {
    return MulDiv(v, g_dpi, 96);
}

void SetDpi(int dpi) {
    if (dpi < 72) dpi = 72;
    if (dpi > 384) dpi = 384;
    if (dpi == g_dpi) return;
    g_dpi = dpi;
    if (g_font) {
        DoneFonts();
        InitFonts();
    }
}

void InitFonts() {
    if (g_font) return;
    g_font = CreateFontW(-Scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                         L"Microsoft YaHei UI");
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, g_font);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    g_lineH = tm.tmHeight + Scale(4);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
}

void DoneFonts() {
    if (g_font) { DeleteObject(g_font); g_font = nullptr; }
}

HFONT FontUI() { return g_font; }
int   LineHeight() { return g_lineH; }

void FillRoundRect(HDC dc, const RECT& r, int radius, COLORREF fill) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

void StrokeRoundRect(HDC dc, const RECT& r, int radius, COLORREF border) {
    HBRUSH br = (HBRUSH)GetStockObject(NULL_BRUSH);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

void DrawTextLine(HDC dc, int x, int y, const std::wstring& s, COLORREF color, int maxRight) {
    if (s.empty()) return;
    HGDIOBJ old = SelectObject(dc, g_font);
    SetTextColor(dc, color);
    RECT clip{ x, y, maxRight, y + g_lineH + Scale(4) };
    ExtTextOutW(dc, x, y, ETO_CLIPPED, &clip, s.c_str(), (int)s.size(), nullptr);
    SelectObject(dc, old);
}

}  // namespace theme
