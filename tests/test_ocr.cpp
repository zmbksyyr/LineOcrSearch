// test_ocr.cpp - 端到端识别测试：自建窗口画大字 -> 鼠标移上去 -> 截屏 -> C2 识别
#include "../src/common.h"
#include <objbase.h>
#include <cstdio>

static const wchar_t* kText = L"改为系统控件吧，这个也是丑陋，还不如用原生的好了。";

static std::string U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        RECT rc{};
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(20, 20, 20));
        HFONT f = CreateFontW(-32, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        HGDIOBJ old = SelectObject(dc, f);
        TextOutW(dc, 24, 60, kText, (int)wcslen(kText));
        SelectObject(dc, old);
        DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static void Pump(int ms) {
    DWORD end = GetTickCount() + (DWORD)ms;
    MSG msg{};
    while (GetTickCount() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    EnablePerMonitorDpi();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OcrTestWnd";
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    RegisterClassExW(&wc);

    HWND h = CreateWindowExW(0, L"OcrTestWnd", L"OCR 测试",
                             WS_OVERLAPPEDWINDOW, 120, 120, 1320, 240,
                             nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(h, SW_SHOW);
    SetWindowPos(h, HWND_TOPMOST, 120, 120, 1320, 240, SWP_SHOWWINDOW);
    SetForegroundWindow(h);
    UpdateWindow(h);
    Pump(400);

    std::wstring err;
    if (!OcrInit(err)) {
        printf("OcrInit FAILED: %s\n", U8(err).c_str());
        printf("exe dir = %s\n", U8(ExeDir()).c_str());
        printf("config  = %s\n", U8(ConfigDirPath()).c_str());
        return 2;
    }
    printf("ocr = %s\n", U8(OcrEngineInfo()).c_str());

    // 同一行文字上换几个鼠标位置，验证「以鼠标右侧为准」选段
    // 原文：改为系统控件吧 | ， | 这个也是丑陋 | ， | 还不如用原生的好了 | 。
    const int probes[4] = { 40, 300, 620, 980 };
    int fails = 0;
    for (int pi = 0; pi < 4; ++pi) {
        POINT pt{ probes[pi], 76 };
        ClientToScreen(h, &pt);
        SetCursorPos(pt.x, pt.y);
        Pump(200);

        CapturedBgra cap;
        if (!CaptureBand(400, 60, cap)) { printf("CaptureBand FAILED\n"); ++fails; continue; }

        std::wstring text;
        std::vector<double> xs;
        int lt = 0, lb = 0;
        DWORD t0 = GetTickCount();
        bool ok = OcrRecognizeBgra(cap.px.data(), cap.w, cap.h, cap.mouseLocalY,
                                   text, &lt, &lb, &xs);
        DWORD dt = GetTickCount() - t0;

        printf("\n[probe %d] clientX=%d mouseLocalX=%d  ok=%d  %lums\n",
               pi, probes[pi], cap.mouseLocalX, ok ? 1 : 0, (unsigned long)dt);
        printf("  raw     = %s\n", U8(text).c_str());
        if (xs.size() == text.size() && !text.empty()) {
            std::wstring byX = CleanOcrTextByX(text, xs, (double)cap.mouseLocalX);
            printf("  by right= %s\n", U8(byX).c_str());
            printf("  pos[0..5]=");
            for (size_t k = 0; k < xs.size() && k < 6; ++k) printf(" %.0f", xs[k]);
            printf("   last=%.0f\n", xs.back());
            if (byX.empty()) ++fails;
        } else {
            printf("  by right= (位置表缺失 %d vs %d)\n", (int)xs.size(), (int)text.size());
            ++fails;
        }
        printf("  longest = %s\n", U8(CleanOcrText(text)).c_str());

        WipeBytes(cap.px);
    }

    OcrShutdown();
    DestroyWindow(h);
    CoUninitialize();
    printf("\n=== probes failed: %d ===\n", fails);
    return fails == 0 ? 0 : 3;
}
