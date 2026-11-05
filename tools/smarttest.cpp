// smarttest.cpp - 诊断「智能」链路：截屏识别 -> 取 UIA 文档 -> 对齐 -> 选段
// 用法：smarttest.exe <hwnd> <dx> <dy>
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

static bool SaveBandBmp(const CapturedBgra& cap, const wchar_t* path) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    int stride = ((cap.w * 3 + 3) / 4) * 4;
    ih.biSize = sizeof(ih);
    ih.biWidth = cap.w;
    ih.biHeight = cap.h;          // 正值 = 自下而上
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = (DWORD)(stride * cap.h);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;

    std::vector<unsigned char> px((size_t)stride * cap.h, 0);
    for (int y = 0; y < cap.h; ++y) {
        const unsigned char* src = cap.px.data() + (size_t)(cap.h - 1 - y) * cap.w * 4;
        unsigned char* dst = &px[(size_t)y * stride];
        for (int x = 0; x < cap.w; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, &fh, sizeof(fh), &w, nullptr) &&
              WriteFile(f, &ih, sizeof(ih), &w, nullptr) &&
              WriteFile(f, px.data(), (DWORD)px.size(), &w, nullptr);
    CloseHandle(f);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    EnablePerMonitorDpi();
    if (argc < 4) { printf("usage: smarttest.exe <hwnd> <dx> <dy>\n"); return 2; }

    HWND target = (HWND)(ULONG_PTR)wcstoull(argv[1], nullptr, 10);
    if (!target || !IsWindow(target)) { printf("bad hwnd\n"); return 3; }

    POINT origin{ 0, 0 };
    ClientToScreen(target, &origin);
    POINT pt{ origin.x + _wtoi(argv[2]), origin.y + _wtoi(argv[3]) };
    SetCursorPos(pt.x, pt.y);
    Sleep(300);
    printf("cursor = (%ld,%ld)\n", (long)pt.x, (long)pt.y);

    std::wstring err;
    if (!OcrInit(err)) { printf("OcrInit failed: %s\n", U8(err).c_str()); return 4; }

    CapturedBgra cap;
    if (!CaptureBand(400, 80, cap)) { printf("CaptureBand failed\n"); return 5; }
    printf("band = %dx%d mouseLocal=(%d,%d)\n", cap.w, cap.h, cap.mouseLocalX, cap.mouseLocalY);
    SaveBandBmp(cap, L"build\band.bmp");


    std::wstring t;
    std::vector<double> xs;
    int lt = 0, lb = 0;
    DWORD t0 = GetTickCount();
    bool ok = OcrRecognizeBgra(cap.px.data(), cap.w, cap.h, cap.mouseLocalY, t, &lt, &lb, &xs);
    DWORD ms = GetTickCount() - t0;
    WipeBytes(cap.px);
    printf("ocr ok=%d %lums line=%d..%d\n", ok ? 1 : 0, (unsigned long)ms, lt, lb);
    printf("  ocr text  = %s\n", U8(t).c_str());
    printf("  ocr chars = %d, xs = %d\n", (int)t.size(), (int)xs.size());
    if (!ok) return 6;

    std::wstring doc;
    DWORD t1 = GetTickCount();
    bool gotDoc = GetUiaDocumentText(pt, t, doc);
    printf("uia doc ok=%d %lums len=%d\n", gotDoc ? 1 : 0,
           (unsigned long)(GetTickCount() - t1), (int)doc.size());
    if (gotDoc) {
        std::wstring head = doc.substr(0, doc.size() < 120 ? doc.size() : 120);
        printf("  doc head  = %s\n", U8(head).c_str());
    }
    if (!gotDoc) return 7;

    AlignResult ar = AlignLineToDocument(doc, t, 0.10);
    printf("align ok=%d ratio=%.2f hay=[%d..%d]\n", ar.ok ? 1 : 0, ar.ratio,
           (int)ar.hayStart, (int)ar.hayEnd);
    if (!ar.ok) return 8;

    std::wstring wt;
    std::vector<double> wx;
    BuildAlignedLine(doc, ar, xs, wt, wx);
    printf("  window    = %s\n", U8(wt).c_str());
    if (wx.size() >= 2) {
        printf("  win x     = %.0f .. %.0f (mouseLocal=%d)\n", wx.front(), wx.back(), cap.mouseLocalX);
    }
    std::wstring picked = CleanOcrTextByX(wt, wx, (double)cap.mouseLocalX);
    printf("  picked    = %s\n", U8(picked).c_str());

    OcrShutdown();
    CoUninitialize();
    return 0;
}
