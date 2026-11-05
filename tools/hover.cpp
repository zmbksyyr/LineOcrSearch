// hover.cpp - 真机验证工具：把鼠标放到指定窗口的客户区坐标，等主程序取词，再读它的输入框
// 用法：hover.exe <hwnd十进制|标题关键字> <dx> <dy> [等待毫秒]
#include "../src/common.h"
#include <objbase.h>
#include <cstdio>
#include <string>

static std::string U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

struct FindCtx { const wchar_t* needle; HWND hit; };

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    FindCtx* c = (FindCtx*)lp;
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t title[512] = {0};
    GetWindowTextW(h, title, 511);
    if (!title[0]) return TRUE;
    if (wcsstr(title, c->needle)) {
        RECT rc{};
        GetWindowRect(h, &rc);
        if (rc.right - rc.left < 300) return TRUE;
        c->hit = h;
        return FALSE;
    }
    return TRUE;
}

struct EditCtx { HWND edit; };

static BOOL CALLBACK EnumChildProc(HWND c, LPARAM lp) {
    wchar_t cls[64] = {0};
    GetClassNameW(c, cls, 64);
    if (wcsncmp(cls, L"RICHEDIT", 8) == 0 || wcscmp(cls, L"Edit") == 0) { ((EditCtx*)lp)->edit = c; return FALSE; }
    return TRUE;
}

static std::wstring ReadAppEdit() {
    HWND main = FindWindowW(L"LineOcrMain", nullptr);
    if (!main) return L"<no app window>";
    EditCtx ctx{ nullptr };
    EnumChildWindows(main, EnumChildProc, (LPARAM)&ctx);
    if (!ctx.edit) return L"<no edit>";
    LRESULT n = SendMessageW(ctx.edit, WM_GETTEXTLENGTH, 0, 0);
    if (n <= 0) return std::wstring();
    std::wstring s((size_t)n + 1, L'\0');
    SendMessageW(ctx.edit, WM_GETTEXT, (WPARAM)(n + 1), (LPARAM)&s[0]);
    s.resize((size_t)n);
    return s;
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

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 4) {
        printf("usage: hover.exe <hwnd|title> <dx> <dy> [waitMs]\n");
        return 2;
    }

    // 直接给 HWND（十进制）最稳，避免命令行中文编码问题
    HWND target = (HWND)(ULONG_PTR)wcstoull(argv[1], nullptr, 10);
    if (!target || !IsWindow(target)) {
        FindCtx fc{ argv[1], nullptr };
        EnumWindows(EnumProc, (LPARAM)&fc);
        target = fc.hit;
    }
    if (!target) { printf("window not found\n"); return 3; }

    POINT origin{ 0, 0 };
    ClientToScreen(target, &origin);
    RECT cr{};
    GetClientRect(target, &cr);
    POINT pt{ origin.x + _wtoi(argv[2]), origin.y + _wtoi(argv[3]) };
    int waitMs = (argc >= 5) ? _wtoi(argv[4]) : 2200;

    SetCursorPos(pt.x, pt.y);
    Pump(waitMs);

    std::wstring got = ReadAppEdit();
    wchar_t title[256] = {0};
    GetWindowTextW(target, title, 255);
    printf("hwnd=%p client=%ldx%ld point=(%ld,%ld)\n",
           (void*)target, (long)cr.right, (long)cr.bottom, (long)pt.x, (long)pt.y);
    printf("  title  = %ls\n", title);
    printf("  picked = [%s]\n", U8(got).c_str());
    return 0;
}
