// target.cpp - 真机验证：外部窗口里摆一行长文字，把鼠标依次放到三个位置，
// 然后直接读主程序输入框里的内容，确认「以鼠标右侧为准」选段生效。
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
                                 WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
                                 20, 20, 900, 100, h, (HMENU)1, nullptr, nullptr);
        HFONT f = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
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

struct FindCtx { HWND main; HWND edit; };

static BOOL CALLBACK EnumChild(HWND c, LPARAM lp) {
    FindCtx* f = (FindCtx*)lp;
    wchar_t cls[64] = {0};
    GetClassNameW(c, cls, 64);
    if (wcscmp(cls, L"Edit") == 0 && f->edit == nullptr) f->edit = c;
    return TRUE;
}

// 跨进程读控件文本必须发 WM_GETTEXT（GetWindowText 只对窗口标题有效）
static std::wstring ReadMainEdit() {
    HWND main = FindWindowW(L"LineOcrMain", nullptr);
    if (!main) return L"<no-main-window>";
    FindCtx ctx{ main, nullptr };
    EnumChildWindows(main, EnumChild, (LPARAM)&ctx);
    if (!ctx.edit) return L"<no-edit>";
    LRESULT n = SendMessageW(ctx.edit, WM_GETTEXTLENGTH, 0, 0);
    if (n <= 0) return std::wstring();
    std::wstring s((size_t)n + 1, L'\0');
    SendMessageW(ctx.edit, WM_GETTEXT, (WPARAM)(n + 1), (LPARAM)&s[0]);
    s.resize((size_t)n);
    return s;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    EnablePerMonitorDpi();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TargetLineWnd";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    HWND h = CreateWindowExW(0, L"TargetLineWnd", L"取词目标行",
                             WS_OVERLAPPEDWINDOW, 260, 260, 960, 200,
                             nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(h, SW_SHOW);
    SetWindowPos(h, HWND_TOPMOST, 260, 260, 960, 200, SWP_SHOWWINDOW);
    SetForegroundWindow(h);
    UpdateWindow(h);
    Pump(500);

    HWND e = GetDlgItem(h, 1);
    // 三个探针：段1中间 / 段2中间 / 段3中间
    const int idx[3] = { 3, 10, 20 };
    const char* expect[3] = { "改为系统控件吧", "这个也是丑陋", "还不如用原生的好了" };

    int fails = 0;
    for (int i = 0; i < 3; ++i) {
        LRESULT p = SendMessageW(e, EM_POSFROMCHAR, (WPARAM)idx[i], 0);
        POINT pt{ (short)LOWORD(p), (short)HIWORD(p) };
        pt.y += 4;
        ClientToScreen(e, &pt);
        SetCursorPos(pt.x, pt.y);
        Pump(3000);   // 等主程序判定静止 + 取词 + 搜索

        std::wstring got;
        for (int k = 0; k < 18; ++k) {          // 最多等 3.6 秒，取到就记下
            Pump(200);
            std::wstring t = ReadMainEdit();
            if (!t.empty()) got = t;
        }
        std::string g = U8(got);
        printf("[probe %d] 鼠标X=%ld 就指向「%s」 -> 输入框=「%s」\n",
               i, (long)pt.x, expect[i], g.c_str());
        if (g != expect[i]) ++fails;
    }

    DestroyWindow(h);
    CoUninitialize();
    printf("\n=== 不一致 %d 项 ===\n", fails);
    return 0;
}
