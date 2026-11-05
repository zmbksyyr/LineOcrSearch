// click.cpp - 向主窗口发一条 WM_COMMAND，用于自动化检查设置窗口
#include "../src/common.h"
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    int id = (argc >= 2) ? _wtoi(argv[1]) : IDC_BTN_SET;
    HWND h = FindWindowW(L"LineOcrMain", nullptr);
    if (!h) { wprintf(L"main window not found\n"); return 1; }
    PostMessageW(h, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
    wprintf(L"posted WM_COMMAND %d\n", id);
    return 0;
}
