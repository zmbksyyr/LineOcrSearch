// icon.cpp - 用代码绘制应用图标（不依赖 .ico 资源与 windres）
#include "common.h"

namespace {

bool InRoundRect(int x, int y, int s, int r) {
    int cx = -1, cy = -1;
    if (x < r && y < r) { cx = r; cy = r; }
    else if (x >= s - r && y < r) { cx = s - r - 1; cy = r; }
    else if (x < r && y >= s - r) { cx = r; cy = s - r - 1; }
    else if (x >= s - r && y >= s - r) { cx = s - r - 1; cy = s - r - 1; }
    if (cx < 0) return true;
    int dx = x - cx;
    int dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

}  // namespace

// 深色圆角底 + 两条白线（一行文字）+ 一个强调点（取词位置）
HICON MakeAppIcon(int size) {
    if (size < 8) size = 8;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    HGDIOBJ oldBmp = SelectObject(mem, color);
    memset(bits, 0, (size_t)size * size * 4);

    int radius = size * 22 / 100;
    if (radius < 2) radius = 2;

    // 底色
    HBRUSH bg = CreateSolidBrush(RGB(45, 49, 56));
    HPEN bgPen = CreatePen(PS_SOLID, 1, RGB(45, 49, 56));
    HGDIOBJ ob = SelectObject(mem, bg);
    HGDIOBJ op = SelectObject(mem, bgPen);
    RoundRect(mem, 0, 0, size, size, radius * 2, radius * 2);
    SelectObject(mem, ob);
    SelectObject(mem, op);
    DeleteObject(bg);
    DeleteObject(bgPen);

    // 两条白线
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ op2 = SelectObject(mem, GetStockObject(NULL_PEN));
    int lh = size * 9 / 100;
    if (lh < 2) lh = 2;

    RECT l1{ size * 22 / 100, size * 32 / 100, size * 78 / 100, size * 32 / 100 + lh };
    FillRect(mem, &l1, white);
    RECT l2{ size * 22 / 100, size * 56 / 100, size * 58 / 100, size * 56 / 100 + lh };
    FillRect(mem, &l2, white);

    // 强调点
    HBRUSH dot = CreateSolidBrush(RGB(110, 168, 255));
    HGDIOBJ ob2 = SelectObject(mem, dot);
    int dr = size * 10 / 100;
    if (dr < 2) dr = 2;
    int cx = size * 70 / 100;
    int cy = size * 56 / 100 + lh / 2;
    Ellipse(mem, cx - dr, cy - dr, cx + dr, cy + dr);
    SelectObject(mem, ob2);
    SelectObject(mem, op2);
    DeleteObject(white);
    DeleteObject(dot);

    // GDI 不写 alpha，这里按圆角形状补上
    unsigned char* px = (unsigned char*)bits;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            unsigned char* p = px + ((size_t)y * size + x) * 4;
            if (InRoundRect(x, y, size, radius)) {
                p[3] = 255;
            } else {
                p[0] = p[1] = p[2] = 0;
                p[3] = 0;
            }
        }
    }

    SelectObject(mem, oldBmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}
