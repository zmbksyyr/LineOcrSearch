// wraptest.cpp - 用真实的 listview.cpp 渲一张结果列表，检查长行是否完整折行显示。
// 用法：wraptest.exe out.bmp <宽> [高]
#include "../src/common.h"
#include "../src/app_state.h"
#include "../src/theme.h"
#include <cstdio>

AppState g;
HWND g_hwndMain = nullptr;
HWND g_hwndList = nullptr;
int  g_lineH = 18;

void SetLeftStatus(const std::wstring&) {}

static bool SaveBmp(const wchar_t* path, int w, int h, const void* bits) {
    BITMAPFILEHEADER bf{};
    BITMAPINFOHEADER bi{};
    int lineBytes = ((w * 24 + 31) / 32) * 4;
    DWORD imgSize = (DWORD)(lineBytes * h);
    bf.bfType = 0x4D42;
    bf.bfOffBits = sizeof(bf) + sizeof(bi);
    bf.bfSize = bf.bfOffBits + imgSize;
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = h;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = imgSize;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    WriteFile(f, &bf, sizeof(bf), &wr, nullptr);
    WriteFile(f, &bi, sizeof(bi), &wr, nullptr);
    WriteFile(f, bits, imgSize, &wr, nullptr);
    CloseHandle(f);
    return true;
}

static bool Snap(HWND hwnd, const wchar_t* path) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return false;

    HDC wdc = GetWindowDC(hwnd);
    HDC mem = CreateCompatibleDC(wdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    if (!PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)) {
        wprintf(L"PrintWindow failed\n");
        return false;
    }

    int lineBytes = ((w * 24 + 31) / 32) * 4;
    std::vector<unsigned char> out((size_t)lineBytes * h, 0);
    const unsigned char* src = (const unsigned char*)bits;
    for (int y = 0; y < h; ++y) {
        const unsigned char* s = src + (size_t)y * w * 4;
        unsigned char* d = &out[(size_t)(h - 1 - y) * lineBytes];
        for (int x = 0; x < w; ++x) {
            d[x * 3 + 0] = s[x * 4 + 0];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + 2];
        }
    }
    bool ok = SaveBmp(path, w, h, out.data());
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, wdc);
    return ok;
}

static void AddItem(const wchar_t* name, int lineNo,
                    std::vector<std::wstring> ctx, int hitIndex,
                    std::vector<HitSpan> hits) {
    ResultItem it;
    it.fileName = name;
    it.lineNo = lineNo;
    it.context = ctx;
    it.hitIndex = hitIndex;
    it.hitRanges = hits;
    if (!hits.empty()) { it.hitStart = hits[0].start; it.hitLen = hits[0].len; }
    g.outcome.items.push_back(it);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: wraptest.exe out.bmp <width> [height]\n"); return 1; }
    int w = _wtoi(argv[2]);
    int h = argc > 3 ? _wtoi(argv[3]) : 620;

    theme::InitFonts();
    g_lineH = theme::LineHeight();

    WNDCLASSEXW lc{ sizeof(lc) };
    lc.lpfnWndProc = ListProc;
    lc.hInstance = GetModuleHandleW(nullptr);
    lc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    lc.lpszClassName = L"LineOcrList";
    RegisterClassExW(&lc);

    HWND hwnd = CreateWindowExW(0, L"LineOcrList", L"", WS_POPUP | WS_VISIBLE,
                                0, 0, w, h, nullptr, nullptr, lc.hInstance, nullptr);
    if (!hwnd) { wprintf(L"create failed\n"); return 2; }
    g_hwndList = hwnd;

    g.outcome.keyword = L"取词";
    g.seq = 1;

    AddItem(L"季度总结报告.docx", 42,
            { L"这个季度的核心问题在于取词准确率不稳定，尤其是浏览器里长段落的折行位置与视觉行不一致，导致选段经常偏到前一句，需要逐字回溯才能命中。",
              L"上句：把鼠标停在任意文字上就能取词。",
              L"下句：取词之后自动在指定文件夹里搜索。" },
            0, { { 9, 2 } });

    AddItem(L"notes.txt", 7,
            { L"The quick brown fox jumps over the lazy dog while the OCR engine keeps recognizing the same long English sentence over and over again without any clipping.",
              L"second line of the same file, also fairly long but shorter than the first one." },
            0, { { 0, 2 } });

    AddItem(L"说明.md", 3,
            { L"多关键词同时命中的情况：取词、搜索两个词都在这一行里。" },
            0, { { 11, 2 }, { 14, 2 } });

    RecalcTotalHeight();
    g.selected = 0;
    wprintf(L"totalH=%d  item heights:", g.totalH);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    bool ok = Snap(hwnd, argv[1]);
    wprintf(L"saved %ls  %dx%d  %s\n", argv[1], w, h, ok ? L"ok" : L"fail");
    theme::DoneFonts();
    return ok ? 0 : 3;
}
