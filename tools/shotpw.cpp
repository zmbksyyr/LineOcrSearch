// shotpw.cpp - 用 PrintWindow(PW_RENDERFULLCONTENT) 把指定窗口渲染成 BMP，
// 不依赖屏幕上是否真的可见（会话锁定/被遮挡也能用）。
// 用法：shotpw.exe out.bmp <windowclass>
#include "../src/common.h"
#include <cstdio>
#include <dwmapi.h>

static bool SaveBmp(const wchar_t* path, int w, int h, const void* bits) {
    BITMAPFILEHEADER bf{};
    BITMAPINFOHEADER bi{};
    int lineBytes = ((w * 24 + 31) / 32) * 4;
    DWORD imgSize = (DWORD)(lineBytes * h);
    bf.bfType = 0x4D42;
    bf.bfOffBits = sizeof(bf) + sizeof(bi);
    bf.bfSize = bf.bfOffBits + imgSize;
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = h;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = imgSize;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    WriteFile(f, &bf, sizeof(bf), &wr, nullptr);
    WriteFile(f, &bi, sizeof(bi), &wr, nullptr);
    WriteFile(f, bits, imgSize, &wr, nullptr);
    CloseHandle(f);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: shotpw.exe out.bmp <windowclass>\n"); return 1; }
    HWND target = FindWindowW(argv[2], nullptr);
    if (!target) { wprintf(L"window not found: %ls\n", argv[2]); return 1; }

    RECT rc{};
    GetWindowRect(target, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) { wprintf(L"bad rect\n"); return 2; }

    HDC wdc = GetWindowDC(target);
    HDC mem = CreateCompatibleDC(wdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    if (!PrintWindow(target, mem, PW_RENDERFULLCONTENT)) {
        wprintf(L"PrintWindow failed\n");
        return 3;
    }

    int lineBytes = ((w * 24 + 31) / 32) * 4;
    std::vector<unsigned char> out((size_t)lineBytes * h, 0);
    const unsigned char* src = (const unsigned char*)bits;
    for (int y = 0; y < h; ++y) {
        const unsigned char* s = src + (size_t)y * w * 4;
        unsigned char* d = &out[(size_t)(h - 1 - y) * lineBytes];
        for (int x = 0; x < w; ++x) {
            d[x * 3 + 0] = s[x * 4 + 0];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + 2];
        }
    }
    bool ok = SaveBmp(argv[1], w, h, out.data());
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(target, wdc);
    wprintf(L"saved %ls  %dx%d  %s\n", argv[1], w, h, ok ? L"ok" : L"fail");
    return ok ? 0 : 4;
}
