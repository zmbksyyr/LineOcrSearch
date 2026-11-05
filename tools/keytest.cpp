// keytest.cpp - 键盘导航验证：click(点列表) / down / up / copy / esc
// 用 SendInput 注入真实输入事件，走系统输入流，焦点行为和真人操作一致
#include <windows.h>
#include <cstdio>
#include <string>

static void U8Print(const wchar_t* tag, const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string u((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u[0], n, nullptr, nullptr);
    printf("%s = [%s]\n", tag, u.c_str());
}

static HWND MainWnd() { return FindWindowW(L"LineOcrMain", nullptr); }

static void ClickAt(int x, int y) {
    SetCursorPos(x, y);
    Sleep(150);
    INPUT in[2]{};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

static void SendKey(WORD vk, bool ctrl) {
    INPUT in[4]{};
    int n = 0;
    if (ctrl) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_CONTROL; ++n; }
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; ++n;
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; in[n].ki.dwFlags = KEYEVENTF_KEYUP; ++n;
    if (ctrl) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_CONTROL; in[n].ki.dwFlags = KEYEVENTF_KEYUP; ++n; }
    SendInput((UINT)n, in, sizeof(INPUT));
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) { printf("usage: keytest click|down|up|enter|esc|copy|read|rect\n"); return 2; }
    std::wstring cmd = argv[1];

    if (cmd == L"rect") {
        RECT r{};
        if (!GetWindowRect(MainWnd(), &r)) { printf("no window\n"); return 1; }
        printf("%ld %ld %ld %ld\n", (long)r.left, (long)r.top, (long)r.right, (long)r.bottom);
        return 0;
    }

    if (cmd == L"click") {   // 点结果列表第一张卡片的标题附近
        RECT r{};
        if (!GetWindowRect(MainWnd(), &r)) { printf("no window\n"); return 1; }
        ClickAt(r.left + 400, r.top + 170);
        printf("clicked\n");
        return 0;
    }

    if (cmd == L"down")  { SendKey(VK_DOWN, false);  printf("down\n");  return 0; }
    if (cmd == L"up")    { SendKey(VK_UP, false);    printf("up\n");    return 0; }
    if (cmd == L"enter") { SendKey(VK_RETURN, false);printf("enter\n"); return 0; }
    if (cmd == L"esc")   { SendKey(VK_ESCAPE, false);printf("esc\n");   return 0; }
    if (cmd == L"copy")  { SendKey(L'C', true);      printf("copy\n");  return 0; }

    if (cmd == L"read") {
        if (!OpenClipboard(nullptr)) { printf("clipboard busy\n"); return 1; }
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const wchar_t* p = (const wchar_t*)GlobalLock(h);
            if (p) { U8Print(L"clipboard", p); GlobalUnlock(h); }
        } else {
            printf("clipboard = (empty)\n");
        }
        CloseClipboard();
        return 0;
    }
    printf("unknown\n");
    return 2;
}
