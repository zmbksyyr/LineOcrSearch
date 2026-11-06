// test_ocr.cpp - 端到端识别：自绘 24px 文字 -> 鼠标停在行左 -> 投影取行 -> 右侧纯文字块
//
// 不依赖 det 模型；只要数据目录里有 rec.onnx / 字典 / onnxruntime.dll 就跑。
// 模型缺失时打印 SKIP 并返回 0，避免没准备资源的机器误报失败。
#include "../src/common.h"
#include <objbase.h>
#include <cstdio>
#include <string>

static const wchar_t* kLines[] = {
    L"3.2.1 闭塞区段办理接发列车作业标准（试行）",
    L"第4.1条 车站值班员应确认区间空闲后方可办理闭塞。",
    L"本规则适用于全线各车站、区间及联络线的行车组织工作。",
    L"遇有施工、天窗或设备故障时，应按有关规定办理。",
    L"接发列车进路必须一次排通，禁止中途变更。",
};

static std::string U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        RECT rc{};
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(20, 20, 20));
        HFONT f = CreateFontW(-24, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        HGDIOBJ old = SelectObject(dc, f);
        for (int i = 0; i < 5; ++i) TextOutW(dc, 100, 30 + i * 34, kLines[i], (int)wcslen(kLines[i]));
        SelectObject(dc, old);
        DeleteObject(f);
        EndPaint(h, &ps);
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
        Sleep(5);
    }
}

// 数据目录在 build\LineOcrSearchData；测试从 ..\res 复制一份，避免依赖主程序先跑过。
static bool PrepareData() {
    std::wstring exeDir = ExeDir();
    std::wstring data = JoinPath(exeDir, cfgpaths::kConfigDir);
    std::wstring rt = JoinPath(data, cfgpaths::kRuntimeDir);
    std::wstring md = JoinPath(data, cfgpaths::kModelDir);
    CreateDirectoryW(data.c_str(), nullptr);
    CreateDirectoryW(rt.c_str(), nullptr);
    CreateDirectoryW(md.c_str(), nullptr);

    const wchar_t* files[][2] = {
        { L"runtime\\onnxruntime.dll", L"runtime\\onnxruntime.dll" },
        { L"model\\rec.onnx", L"model\\rec.onnx" },
        { L"model\\ppocr_keys_v1.txt", L"model\\ppocr_keys_v1.txt" },
    };
    for (auto& f : files) {
        std::wstring src = JoinPath(JoinPath(exeDir, L"..\\res"), f[0]);
        std::wstring dst = JoinPath(data, f[1]);
        if (!FileExists(dst)) CopyFileW(src.c_str(), dst.c_str(), FALSE);
    }
    return FileExists(JoinPath(md, L"rec.onnx")) &&
           FileExists(JoinPath(md, L"ppocr_keys_v1.txt")) &&
           FileExists(JoinPath(rt, L"onnxruntime.dll"));
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    EnablePerMonitorDpi();

    // 锁屏/安全桌面下 BitBlt 截不到屏幕，测试应跳过而不是判失败。
    HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
    if (!desk) { printf("SKIP: 当前桌面不可截屏（锁屏/安全桌面）\n"); return 0; }
    CloseDesktop(desk);

    if (!PrepareData()) {
        printf("SKIP: 数据目录缺少模型（res 下也没有）\n");
        return 0;
    }

    std::wstring err;
    if (!OcrInit(err)) {
        printf("SKIP: OcrInit 失败: %s\n", U8(err).c_str());
        return 0;
    }
    printf("ocr = %s\n", U8(OcrEngineInfo()).c_str());

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OcrTestWnd";
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    RegisterClassExW(&wc);

    HWND h = CreateWindowExW(WS_EX_TOPMOST, L"OcrTestWnd", L"OCR 测试",
                             WS_POPUP | WS_VISIBLE, 120, 120, 1100, 320,
                             nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    Pump(400);

    POINT org{ 0, 0 };
    ClientToScreen(h, &org);

    int fails = 0;
    int capFails = 0;
    const int probes[] = { 1, 2, 3 };   // 行 2/3/4
    const wchar_t* need[] = { L"车站值班员", L"行车组织工作", L"天窗或设备故障时" };
    for (int k = 0; k < 3; ++k) {
        int line = probes[k];
        POINT pt{ org.x + 60, org.y + 30 + line * 34 + 12 };
        SetCursorPos(pt.x, pt.y);
        Pump(150);

        CapturedBgra cap;
        if (!CaptureBandRight(24, 800, 120, cap)) {
            printf("[probe %d] capture failed\n", k);
            ++fails;
            ++capFails;
            continue;
        }
        std::wstring t;
        std::vector<double> xs;
        int lt = 0, lb = 0, rej = 0;
        float conf = 0.0f;
        bool ok = OcrRecognizeBgra(cap.px.data(), cap.w, cap.h, cap.mouseLocalX,
                                   cap.mouseLocalY, t, &lt, &lb, &xs, &rej, &conf);
        int mx = cap.mouseLocalX;
        WipeBytes(cap.px);
        if (!ok) {
            printf("[probe %d] ocr failed reject=%d\n", k, rej);
            ++fails;
            continue;
        }
        std::wstring seg = (xs.size() == t.size())
            ? TakeRightOfCursor(t, xs, (double)mx)
            : TakeRightOfCursor(t, std::vector<double>(), 0.0);
        printf("[probe %d] conf=%.3f raw=[%s]\n           seg=[%s]\n", k, conf, U8(t).c_str(), U8(seg).c_str());
        if (seg.find(need[k]) == std::wstring::npos) ++fails;
        if (conf < 0.70f) ++fails;
    }

    if (capFails == 3) {
        printf("SKIP: 屏幕不可截（锁屏/安全桌面）\n");
        OcrShutdown();
        DestroyWindow(h);
        CoUninitialize();
        return 0;
    }
    OcrShutdown();
    DestroyWindow(h);
    CoUninitialize();
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
