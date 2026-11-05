// test_edit.cpp - 验证 C1 文字层「以鼠标右侧为准」选段（原生 EDIT 控件路径）
// 建一个带 EDIT 的窗口，把鼠标放到这一行的不同位置，看取到哪一段。
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
    if (m == WM_CREATE) {
        HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", kText,
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
                                 20, 20, 900, 120, h, (HMENU)1, nullptr, nullptr);
        HFONT f = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        SendMessageW(e, WM_SETFONT, (WPARAM)f, TRUE);
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
    wc.lpszClassName = L"EditTestWnd";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    HWND h = CreateWindowExW(0, L"EditTestWnd", L"EDIT 取词测试",
                             WS_OVERLAPPEDWINDOW, 160, 160, 970, 220,
                             nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(h, SW_SHOW);
    SetWindowPos(h, HWND_TOPMOST, 160, 160, 970, 220, SWP_SHOWWINDOW);
    SetForegroundWindow(h);
    UpdateWindow(h);
    Pump(500);

    // 文字从控件左上角开始排，每字约 14px，一行 26 字约 364px
    const int probes[4] = { 6, 90, 190, 330 };   // 控件内相对 x
    int fails = 0;
    for (int i = 0; i < 4; ++i) {
        HWND e = GetDlgItem(h, 1);
        POINT pt{ probes[i], 10 };
        ClientToScreen(e, &pt);
        SetCursorPos(pt.x, pt.y);
        Pump(150);

        std::wstring text, src;
        RECT rc{};
        std::vector<double> xs;
        if (!CaptureLineAtCursor(text, src, &rc, &xs)) {
            printf("[probe %d] 取词失败\n", i);
            ++fails;
            continue;
        }
        printf("\n[probe %d] 鼠标屏幕X=%ld  行矩形=[%ld..%ld]  来源=%s\n",
               i, (long)pt.x, (long)rc.left, (long)rc.right, U8(src).c_str());
        printf("  line    = %s\n", U8(text).c_str());
        bool sized = (xs.size() == text.size());
        printf("  pos     = %s\n", sized ? "ok" : "缺失");
        if (sized) {
            printf("  pos[0..4]=");
            for (size_t k = 0; k < xs.size() && k < 5; ++k) printf(" %.0f", xs[k]);
            printf("   last=%.0f\n", xs.back());
            std::wstring byX = CleanOcrTextByX(text, xs, (double)pt.x);
            printf("  by right= %s\n", U8(byX).c_str());
            if (byX.empty()) ++fails;
        } else {
            ++fails;
        }
        printf("  longest = %s\n", U8(CleanOcrText(text)).c_str());
    }

    DestroyWindow(h);
    CoUninitialize();
    printf("\n=== probes failed: %d ===\n", fails);
    return fails == 0 ? 0 : 3;
}
