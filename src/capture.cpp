// capture.cpp - 只截鼠标附近一条带（绝不截全屏），像素只留在内存
#include "common.h"

void EnablePerMonitorDpi() {
    typedef BOOL(WINAPI * PFN_SetCtx)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    PFN_SetCtx fn = (PFN_SetCtx)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (fn) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        fn((HANDLE)-4);
        return;
    }
    SetProcessDPIAware();
}

bool CaptureBand(int halfW, int halfH, CapturedBgra& out) {
    out = CapturedBgra{};
    POINT p{};
    if (!GetCursorPos(&p)) return false;

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return false;

    int x1 = p.x - halfW; if (x1 < vx) x1 = vx;
    int y1 = p.y - halfH; if (y1 < vy) y1 = vy;
    int x2 = p.x + halfW; if (x2 > vx + vw) x2 = vx + vw;
    int y2 = p.y + halfH; if (y2 > vy + vh) y2 = vy + vh;

    int w = x2 - x1;
    int h = y2 - y1;
    if (w <= 0 || h <= 0) return false;

    HDC screen = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);
    if (!mem) { DeleteDC(screen); return false; }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(mem);
        DeleteDC(screen);
        return false;
    }

    HGDIOBJ old = SelectObject(mem, bmp);

    // 截屏前把鼠标指针藏起来：指针正好压在文字上会污染识别结果
    ShowCursor(FALSE);
    Sleep(6);
    BOOL ok = BitBlt(mem, 0, 0, w, h, screen, x1, y1, SRCCOPY);
    ShowCursor(TRUE);

    if (ok) {
        out.w = w;
        out.h = h;
        out.mouseLocalX = p.x - x1;
        out.mouseLocalY = p.y - y1;
        out.px.assign((const unsigned char*)bits,
                      (const unsigned char*)bits + (size_t)w * (size_t)h * 4);
    }

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    DeleteDC(screen);

    if (!ok) {
        // 失败时清空缓冲，不保留任何像素
        SecureZeroMemory(out.px.data(), out.px.size());
        out.px.clear();
        return false;
    }
    return true;
}

void WipeBytes(std::vector<unsigned char>& buf) {
    if (!buf.empty()) {
        SecureZeroMemory(buf.data(), buf.size());
        buf.clear();
    }
}
