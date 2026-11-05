// shot.cpp - 把指定窗口截成 BMP，用于检查界面外观
#include "../src/common.h"
#include <cstdio>

static bool SaveBmp(const wchar_t* path, int w, int h, const void* bits) {
    BITMAPFILEHEADER bf{};
    BITMAPINFOHEADER bi{};
    int lineBytes = ((w * 24 + 31) / 32) * 4;
    DWORD imgSize = (DWORD)(lineBytes * h);

    bf.bfType = 0x4D42;
    bf.bfOffBits = sizeof(bf) + sizeof(bi);
    bf.bfSize = bf.bfOffBits + imgSize;

    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = h;  // bottom-up
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = imgSize;

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    WriteFile(f, &bf, sizeof(bf), &wr, nullptr);
    WriteFile(f, &bi, sizeof(bi), &wr, nullptr);
    WriteFile(f, bits, imgSize, &wr, nullptr);
    CloseHandle(f);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: shot.exe out.bmp [hwndclass]\n"); return 1; }

    HWND target = nullptr;
    if (argc >= 3) {
        target = FindWindowW(argv[2], nullptr);
        if (target) {
            ShowWindow(target, SW_RESTORE);
            SetWindowPos(target, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetWindowPos(target, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(target);
            SetActiveWindow(target);
            Sleep(600);
        }
    }

    RECT rc{};
    if (target) {
        GetWindowRect(target, &rc);
    } else {
        rc.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        rc.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        rc.right = rc.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        rc.bottom = rc.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) { wprintf(L"bad rect\n"); return 2; }

    HDC screen = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
    HDC mem = CreateCompatibleDC(screen);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY);

    // BGRA -> BGR 24bit bottom-up
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
    DeleteDC(screen);

    wprintf(L"saved %ls  %dx%d  %s\n", argv[1], w, h, ok ? L"ok" : L"fail");
    return ok ? 0 : 3;
}
