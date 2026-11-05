// editprobe.cpp - 验证输入框行为：读文字 / 模拟打字 / 触发搜索
// 用法：editprobe.exe show | type:<ascii> | search
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

struct Ctx { HWND edit; };

static BOOL CALLBACK EnumChildProc(HWND c, LPARAM lp) {
    wchar_t cls[64] = {0};
    GetClassNameW(c, cls, 64);
    if (wcsncmp(cls, L"RICHEDIT", 8) == 0 || wcscmp(cls, L"Edit") == 0) {
        ((Ctx*)lp)->edit = c;
        return FALSE;
    }
    return TRUE;
}

static std::wstring EditText(HWND e) {
    LRESULT n = SendMessageW(e, WM_GETTEXTLENGTH, 0, 0);
    if (n <= 0) return std::wstring();
    std::wstring s((size_t)n + 1, L'\0');
    SendMessageW(e, WM_GETTEXT, (WPARAM)(n + 1), (LPARAM)&s[0]);
    s.resize((size_t)n);
    return s;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) { printf("usage: editprobe.exe show|type:<ascii>|search\n"); return 2; }

    HWND main = FindWindowW(L"LineOcrMain", nullptr);
    if (!main) { printf("no app window\n"); return 3; }
    Ctx ctx{ nullptr };
    EnumChildWindows(main, EnumChildProc, (LPARAM)&ctx);
    if (!ctx.edit) { printf("no edit control\n"); return 4; }

    wchar_t cls[64] = {0};
    GetClassNameW(ctx.edit, cls, 64);
    printf("edit class = %ls\n", cls);

    std::wstring arg = argv[1];
    if (arg == L"show") {
        printf("text = [%s]\n", U8(EditText(ctx.edit)).c_str());
        return 0;
    }

    if (arg.rfind(L"type:", 0) == 0) {
        std::wstring before = EditText(ctx.edit);
        printf("before = [%s]\n", U8(before).c_str());
        for (size_t i = 5; i < arg.size(); ++i) {
            SendMessageW(ctx.edit, WM_CHAR, (WPARAM)arg[i], 1);
        }
        printf("after  = [%s]\n", U8(EditText(ctx.edit)).c_str());
        return 0;
    }

    if (arg.rfind(L"set:", 0) == 0) {
        std::wstring text = arg.substr(4);
        SendMessageW(ctx.edit, WM_SETTEXT, 0, (LPARAM)text.c_str());
        printf("set -> [%s]\n", U8(EditText(ctx.edit)).c_str());
        return 0;
    }

    if (arg == L"search") {
        PostMessageW(main, WM_COMMAND, MAKEWPARAM(IDC_BTN_SEARCH, BN_CLICKED), 0);
        printf("search posted\n");
        return 0;
    }

    printf("unknown command\n");
    return 2;
}
