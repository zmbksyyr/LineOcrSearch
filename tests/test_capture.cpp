// test_capture.cpp - C1 取词冒烟测试：在当前鼠标位置取一行文字
#include "../src/common.h"
#include <objbase.h>
#include <cstdio>

int main() {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    POINT p{};
    GetCursorPos(&p);
    printf("cursor = (%ld, %ld)\n", (long)p.x, (long)p.y);

    std::wstring text, source;
    bool ok = CaptureLineAtCursor(text, source);
    printf("captured = %s\n", ok ? "yes" : "no");
    if (ok) {
        printf("source   = %ls\n", source.c_str());
        printf("raw      = %ls\n", text.c_str());
        std::wstring cleaned = CleanOcrText(text);
        printf("cleaned  = %ls\n", cleaned.c_str());
        printf("chars    = %d\n", CountTextChars(cleaned));
    }

    CoUninitialize();
    return ok ? 0 : 2;
}
