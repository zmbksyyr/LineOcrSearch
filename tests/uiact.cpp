// uiact.cpp - 自动化小动作：移动鼠标到窗口中心 / 关闭设置窗口
#include "../src/common.h"
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: uiact center|close\n"); return 1; }

    if (wcscmp(argv[1], L"center") == 0) {
        HWND h = FindWindowW(L"LineOcrMain", nullptr);
        if (!h) { wprintf(L"no main window\n"); return 2; }
        RECT rc{};
        GetWindowRect(h, &rc);
        int x = (rc.left + rc.right) / 2;
        int y = (rc.top + rc.bottom) / 2;
        SetCursorPos(x, y);
        wprintf(L"cursor -> %d, %d\n", x, y);
        return 0;
    }
    if (wcscmp(argv[1], L"move") == 0 && argc >= 4) {
        int x = _wtoi(argv[2]);
        int y = _wtoi(argv[3]);
        SetCursorPos(x, y);
        wprintf(L"cursor -> %d, %d\n", x, y);
        return 0;
    }
    if (wcscmp(argv[1], L"resize") == 0 && argc >= 4) {
        HWND h = FindWindowW(L"LineOcrMain", nullptr);
        if (!h) { wprintf(L"no main window\n"); return 2; }
        SetWindowPos(h, nullptr, 0, 0, _wtoi(argv[2]), _wtoi(argv[3]),
                     SWP_NOMOVE | SWP_NOZORDER);
        wprintf(L"resized\n");
        return 0;
    }
    if (wcscmp(argv[1], L"search") == 0 && argc >= 3) {
        HWND h = FindWindowW(L"LineOcrMain", nullptr);
        if (!h) { wprintf(L"no main window\n"); return 2; }
        HWND e = GetDlgItem(h, 1001);
        if (e) SetWindowTextW(e, argv[2]);
        PostMessageW(h, WM_COMMAND, MAKEWPARAM(1002, BN_CLICKED), 0);
        wprintf(L"search posted\n");
        return 0;
    }
    if (wcscmp(argv[1], L"close") == 0) {
        HWND h = FindWindowW(L"LineOcrSettings", nullptr);
        if (!h) { wprintf(L"no settings window\n"); return 2; }
        PostMessageW(h, WM_CLOSE, 0, 0);
        wprintf(L"closed settings\n");
        return 0;
    }
    return 1;
}
