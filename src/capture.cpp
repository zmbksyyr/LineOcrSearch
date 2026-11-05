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

// 截一块矩形（不越出虚拟屏），并把光标在其中的局部坐标一并返回
static bool CaptureRect(int x1, int y1, int x2, int y2, CapturedBgra& out) {
    out = CapturedBgra{};
    POINT p{};
    if (!GetCursorPos(&p)) return false;

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return false;

    if (x1 < vx) x1 = vx;
    if (y1 < vy) y1 = vy;
    if (x2 > vx + vw) x2 = vx + vw;
    if (y2 > vy + vh) y2 = vy + vh;

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

// 以光标为中心的对称条带（老接口，测试用）
bool CaptureBand(int halfW, int halfH, CapturedBgra& out) {
    POINT p{};
    if (!GetCursorPos(&p)) { out = CapturedBgra{}; return false; }
    if (halfW < 1) halfW = 1;
    if (halfH < 1) halfH = 1;
    return CaptureRect(p.x - halfW, p.y - halfH, p.x + halfW, p.y + halfH, out);
}

// 以光标为左锚点的右偏条带：左边只留 leftPad（够判断字有没有跨过笔尖），
// 右边取 width。取词只认「笔尖往右」的文字，左边那半截除了干扰没别的作用。
bool CaptureBandRight(int leftPad, int width, int halfH, CapturedBgra& out) {
    POINT p{};
    if (!GetCursorPos(&p)) { out = CapturedBgra{}; return false; }
    if (leftPad < 0) leftPad = 0;
    if (width < 1) width = 1;
    if (halfH < 1) halfH = 1;
    return CaptureRect(p.x - leftPad, p.y - halfH, p.x + width, p.y + halfH, out);
}

