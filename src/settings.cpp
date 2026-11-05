// settings.cpp - 10 项大白话设置窗口（单页、无滚动、模态）
#include "common.h"
#include "theme.h"
#include "flatbutton.h"
#include <shlobj.h>
#include <objbase.h>
#include <cstdio>
#include <cstdlib>

#define IDC_SET_PATH     2001
#define IDC_SET_BROWSE   2002
#define IDC_SET_LINGER   2003
#define IDC_SET_AUTOCAP  2004
#define IDC_SET_AUTOSRCH 2005
#define IDC_SET_MODE0    2020
#define IDC_SET_MODE1    2021
#define IDC_SET_MODE2    2022
#define IDC_SET_MODE3    2023
#define IDC_SET_BACK     2009
#define IDC_SET_OFFICE   2010
#define IDC_SET_CONTEXT  2011
#define IDC_SET_MAXRES   2012
#define IDC_SET_OK       2013
#define IDC_SET_CANCEL   2014
#define IDC_SET_RESET    2015
#define IDC_SET_LINGER_LBL 2016
#define IDC_SET_CAPINFO  2017
#define IDC_SET_CTX0     2030
#define IDC_SET_CTX1     2031
#define IDC_SET_CTX2     2032
#define IDC_SET_MAX0     2033
#define IDC_SET_MAX1     2034
#define IDC_SET_MAX2     2035
#define IDC_SET_MAX3     2036
#define IDC_SET_CLEARCACHE 2040
#define IDC_SET_CACHE0   2041
#define IDC_SET_CACHE1   2042
#define IDC_SET_CACHE2   2043
#define IDC_SET_CACHE3   2044

static HWND g_dlg = nullptr;
static HWND g_parent = nullptr;
static Settings g_edit;
static bool g_accepted = false;
static RECT g_pathBox{};

static HFONT Font() { return theme::FontUI(); }
static void SetCtlFont(HWND h) { SendMessageW(h, WM_SETFONT, (WPARAM)Font(), TRUE); }

// 所有控件查找都用传入的 h：WM_CREATE 阶段 g_dlg 还没有值
static void UpdateLingerLabel(HWND h) {
    int v = (int)SendMessageW(GetDlgItem(h, IDC_SET_LINGER), TBM_GETPOS, 0, 0);
    wchar_t buf[64];
    swprintf(buf, 64, L"%.1f 秒", v / 1000.0);
    SetWindowTextW(GetDlgItem(h, IDC_SET_LINGER_LBL), buf);
}

static void LoadToControls(HWND h) {
    SetWindowTextW(GetDlgItem(h, IDC_SET_PATH), g_edit.rootDir.c_str());
    SendMessageW(GetDlgItem(h, IDC_SET_LINGER), TBM_SETPOS, TRUE, g_edit.lingerMs);
    UpdateLingerLabel(h);

    CheckDlgButton(h, IDC_SET_AUTOCAP,  g_edit.autoCapture ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(h, IDC_SET_AUTOSRCH, g_edit.autoSearch ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(h, IDC_SET_BACK,     g_edit.backtrack ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(h, IDC_SET_OFFICE,   g_edit.searchOffice ? BST_CHECKED : BST_UNCHECKED);

    int mode = g_edit.captureMode;
    if (mode < 0 || mode > 3) mode = 0;
    CheckRadioButton(h, IDC_SET_MODE0, IDC_SET_MODE3, IDC_SET_MODE0 + mode);

    int ctx = g_edit.contextLevel;
    if (ctx < 0 || ctx > 2) ctx = 1;
    CheckRadioButton(h, IDC_SET_CTX0, IDC_SET_CTX2, IDC_SET_CTX0 + ctx);

    int mi = 2;  // 50 条
    if (g_edit.maxResults <= 10) mi = 0;
    else if (g_edit.maxResults <= 20) mi = 1;
    else if (g_edit.maxResults >= 100) mi = 3;
    CheckRadioButton(h, IDC_SET_MAX0, IDC_SET_MAX3, IDC_SET_MAX0 + mi);

    int ci = 0;  // 64MB
    if (g_edit.cacheMB >= 512) ci = 3;
    else if (g_edit.cacheMB >= 256) ci = 2;
    else if (g_edit.cacheMB >= 128) ci = 1;
    CheckRadioButton(h, IDC_SET_CACHE0, IDC_SET_CACHE3, IDC_SET_CACHE0 + ci);

    InvalidateRect(h, nullptr, FALSE);
}

static void SaveFromControls() {
    wchar_t buf[1024] = {0};
    GetWindowTextW(GetDlgItem(g_dlg, IDC_SET_PATH), buf, 1024);
    g_edit.rootDir = buf;

    g_edit.lingerMs = (int)SendMessageW(GetDlgItem(g_dlg, IDC_SET_LINGER), TBM_GETPOS, 0, 0);
    g_edit.autoCapture  = IsDlgButtonChecked(g_dlg, IDC_SET_AUTOCAP) == BST_CHECKED;
    g_edit.autoSearch   = IsDlgButtonChecked(g_dlg, IDC_SET_AUTOSRCH) == BST_CHECKED;
    g_edit.backtrack    = IsDlgButtonChecked(g_dlg, IDC_SET_BACK) == BST_CHECKED;
    g_edit.searchOffice = IsDlgButtonChecked(g_dlg, IDC_SET_OFFICE) == BST_CHECKED;

    if (IsDlgButtonChecked(g_dlg, IDC_SET_MODE1) == BST_CHECKED) g_edit.captureMode = 1;
    else if (IsDlgButtonChecked(g_dlg, IDC_SET_MODE2) == BST_CHECKED) g_edit.captureMode = 2;
    else if (IsDlgButtonChecked(g_dlg, IDC_SET_MODE3) == BST_CHECKED) g_edit.captureMode = 3;
    else g_edit.captureMode = 0;

    if (IsDlgButtonChecked(g_dlg, IDC_SET_CTX0) == BST_CHECKED) g_edit.contextLevel = 0;
    else if (IsDlgButtonChecked(g_dlg, IDC_SET_CTX2) == BST_CHECKED) g_edit.contextLevel = 2;
    else g_edit.contextLevel = 1;

    if (IsDlgButtonChecked(g_dlg, IDC_SET_MAX0) == BST_CHECKED) g_edit.maxResults = 10;
    else if (IsDlgButtonChecked(g_dlg, IDC_SET_MAX1) == BST_CHECKED) g_edit.maxResults = 20;
    else if (IsDlgButtonChecked(g_dlg, IDC_SET_MAX3) == BST_CHECKED) g_edit.maxResults = 100;
    else g_edit.maxResults = 50;

    if (IsDlgButtonChecked(g_dlg, IDC_SET_CACHE1) == BST_CHECKED) g_edit.cacheMB = 128;
    else if (IsDlgButtonChecked(g_dlg, IDC_SET_CACHE2) == BST_CHECKED) g_edit.cacheMB = 256;
    else if (IsDlgButtonChecked(g_dlg, IDC_SET_CACHE3) == BST_CHECKED) g_edit.cacheMB = 512;
    else g_edit.cacheMB = 64;
}

static void PickFolder() {
    wchar_t cur[1024] = {0};
    GetWindowTextW(GetDlgItem(g_dlg, IDC_SET_PATH), cur, 1024);

    BROWSEINFOW bi{};
    bi.hwndOwner = g_dlg;
    bi.lpszTitle = L"选择要搜索的文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;

    wchar_t path[MAX_PATH] = {0};
    if (SHGetPathFromIDListW(pidl, path)) {
        SetWindowTextW(GetDlgItem(g_dlg, IDC_SET_PATH), path);
    }
    CoTaskMemFree(pidl);
}

static void CreateControls(HWND h) {
    const int x = 28;
    const int labelW = 500;
    int y = 22;

    // 1 搜索文件夹
    CreateWindowExW(0, L"STATIC", L"搜索文件夹", WS_CHILD | WS_VISIBLE,
                    x, y, labelW, 22, h, nullptr, nullptr, nullptr);
    y += 26;
    g_pathBox = RECT{ x, y, x + 340, y + 34 };
    HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             x + 12, y + 7, 316, 20, h, (HMENU)IDC_SET_PATH, nullptr, nullptr);
    SetCtlFont(e);
    CreateFlatButton(h, IDC_SET_BROWSE, L"浏览", false);
    MoveWindow(GetDlgItem(h, IDC_SET_BROWSE), x + 348, y, 76, 34, TRUE);
    CreateFlatButton(h, IDC_SET_CLEARCACHE, L"重建缓存", false);
    MoveWindow(GetDlgItem(h, IDC_SET_CLEARCACHE), x + 432, y, 104, 34, TRUE);

    y += 54;
    // 2 静止时长
    CreateWindowExW(0, L"STATIC", L"鼠标停多久自动取词", WS_CHILD | WS_VISIBLE,
                    x, y + 7, 160, 22, h, nullptr, nullptr, nullptr);
    HWND tb = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                              WS_CHILD | WS_VISIBLE | TBS_HORZ,
                              x + 168, y, 216, 32, h, (HMENU)IDC_SET_LINGER, nullptr, nullptr);
    SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELPARAM(200, 1000));
    SendMessageW(tb, TBM_SETPAGESIZE, 0, 100);

    HWND lbl = CreateWindowExW(0, L"STATIC", L"0.5 秒", WS_CHILD | WS_VISIBLE,
                               x + 392, y + 7, 64, 22, h, (HMENU)IDC_SET_LINGER_LBL, nullptr, nullptr);
    SetCtlFont(lbl);

    y += 48;
    // 3 自动取词
    HWND c1 = CreateWindowExW(0, L"BUTTON", L"自动取词（鼠标停住就取）",
                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                              x, y, 340, 24, h, (HMENU)IDC_SET_AUTOCAP, nullptr, nullptr);
    SetCtlFont(c1);

    y += 32;
    // 4 取词后自动搜索
    HWND c2 = CreateWindowExW(0, L"BUTTON", L"取词后自动搜索",
                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                              x, y, 340, 24, h, (HMENU)IDC_SET_AUTOSRCH, nullptr, nullptr);
    SetCtlFont(c2);

    y += 40;
    // 5 取词方式
    CreateWindowExW(0, L"STATIC", L"取词方式", WS_CHILD | WS_VISIBLE,
                    x, y, 200, 22, h, nullptr, nullptr, nullptr);
    y += 26;
    HWND r0 = CreateWindowExW(0, L"BUTTON", L"智能（两者结合）",
                              WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                              x + 4, y, 460, 24, h, (HMENU)IDC_SET_MODE0, nullptr, nullptr);
    SetCtlFont(r0);
    HWND r1 = CreateWindowExW(0, L"BUTTON", L"自动（两者之一）",
                              WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                              x + 4, y + 28, 460, 24, h, (HMENU)IDC_SET_MODE1, nullptr, nullptr);
    SetCtlFont(r1);
    HWND r2 = CreateWindowExW(0, L"BUTTON", L"总是 UIA 元素",
                              WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                              x + 4, y + 56, 460, 24, h, (HMENU)IDC_SET_MODE2, nullptr, nullptr);
    SetCtlFont(r2);
    HWND r3 = CreateWindowExW(0, L"BUTTON", L"总是识别图片",
                              WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                              x + 4, y + 84, 460, 24, h, (HMENU)IDC_SET_MODE3, nullptr, nullptr);
    SetCtlFont(r3);
    y += 84;

    y += 40;
    // 6 回溯
    HWND c3 = CreateWindowExW(0, L"BUTTON", L"搜不到时自动缩短关键词",
                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                              x, y, 340, 24, h, (HMENU)IDC_SET_BACK, nullptr, nullptr);
    SetCtlFont(c3);

    y += 32;
    // 7 Office
    HWND c4 = CreateWindowExW(0, L"BUTTON", L"同时搜索 Word / Excel",
                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                              x, y, 340, 24, h, (HMENU)IDC_SET_OFFICE, nullptr, nullptr);
    SetCtlFont(c4);

    y += 40;
    // 8 上下文（单选）
    CreateWindowExW(0, L"STATIC", L"结果里显示多少前后文", WS_CHILD | WS_VISIBLE,
                    x, y, 260, 22, h, nullptr, nullptr, nullptr);
    y += 26;
    HWND ct0 = CreateWindowExW(0, L"BUTTON", L"简洁",
                               WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                               x, y, 96, 24, h, (HMENU)IDC_SET_CTX0, nullptr, nullptr);
    SetCtlFont(ct0);
    HWND ct1 = CreateWindowExW(0, L"BUTTON", L"标准",
                               WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                               x + 96, y, 96, 24, h, (HMENU)IDC_SET_CTX1, nullptr, nullptr);
    SetCtlFont(ct1);
    HWND ct2 = CreateWindowExW(0, L"BUTTON", L"详细",
                               WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                               x + 192, y, 96, 24, h, (HMENU)IDC_SET_CTX2, nullptr, nullptr);
    SetCtlFont(ct2);

    y += 40;
    // 9 结果数（单选）
    CreateWindowExW(0, L"STATIC", L"最多显示多少条结果", WS_CHILD | WS_VISIBLE,
                    x, y, 260, 22, h, nullptr, nullptr, nullptr);
    y += 26;
    HWND mx0 = CreateWindowExW(0, L"BUTTON", L"10 条",
                               WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                               x, y, 96, 24, h, (HMENU)IDC_SET_MAX0, nullptr, nullptr);
    SetCtlFont(mx0);
    HWND mx1 = CreateWindowExW(0, L"BUTTON", L"20 条",
                               WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                               x + 96, y, 96, 24, h, (HMENU)IDC_SET_MAX1, nullptr, nullptr);
    SetCtlFont(mx1);
    HWND mx2 = CreateWindowExW(0, L"BUTTON", L"50 条",
                               WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                               x + 192, y, 96, 24, h, (HMENU)IDC_SET_MAX2, nullptr, nullptr);
    SetCtlFont(mx2);
    HWND mx3 = CreateWindowExW(0, L"BUTTON", L"100 条",
                               WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                               x + 288, y, 100, 24, h, (HMENU)IDC_SET_MAX3, nullptr, nullptr);
    SetCtlFont(mx3);

    y += 40;
    // 10 缓存上限（单选）
    CreateWindowExW(0, L"STATIC", L"缓存最多占多少磁盘", WS_CHILD | WS_VISIBLE,
                    x, y, 260, 22, h, nullptr, nullptr, nullptr);
    y += 26;
    const wchar_t* cacheLabels[4] = { L"64 MB", L"128 MB", L"256 MB", L"512 MB" };
    for (int i = 0; i < 4; ++i) {
        HWND cb = CreateWindowExW(0, L"BUTTON", cacheLabels[i],
                                  WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON |
                                      (i == 0 ? WS_GROUP : 0),
                                  x + i * 96, y, 96, 24, h,
                                  (HMENU)(INT_PTR)(IDC_SET_CACHE0 + i), nullptr, nullptr);
        SetCtlFont(cb);
    }

    // 能力说明（只读）
    y += 40;
    HWND info = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                x, y, 500, 22, h, (HMENU)IDC_SET_CAPINFO, nullptr, nullptr);
    SetCtlFont(info);

    // 底部按钮：整组在客户区水平居中，两侧留白对称
    int by = y + 44;
    RECT crc{};
    GetClientRect(h, &crc);
    const int gapBtn = 10;
    const int wOk = 84, wCancel = 84, wReset = 92;
    const int totalW = wOk + gapBtn + wCancel + gapBtn + wReset;
    int bx = (crc.right - totalW) / 2;
    if (bx < x) bx = x;

    CreateFlatButton(h, IDC_SET_OK, L"确定", true);
    MoveWindow(GetDlgItem(h, IDC_SET_OK), bx, by, wOk, 32, TRUE);
    CreateFlatButton(h, IDC_SET_CANCEL, L"取消", false);
    MoveWindow(GetDlgItem(h, IDC_SET_CANCEL), bx + wOk + gapBtn, by, wCancel, 32, TRUE);
    CreateFlatButton(h, IDC_SET_RESET, L"恢复默认", false);
    MoveWindow(GetDlgItem(h, IDC_SET_RESET), bx + wOk + gapBtn + wCancel + gapBtn, by, wReset, 32, TRUE);

    // 统一字体
    for (HWND child = GetWindow(h, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t cls[64] = {0};
        GetClassNameW(child, cls, 64);
        if (wcscmp(cls, L"LineOcrFlatBtn") != 0) SetCtlFont(child);
    }
}

static LRESULT CALLBACK SettingsProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CREATE:
            CreateControls(h);
            LoadToControls(h);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(h, &ps);
            RECT rc{};
            GetClientRect(h, &rc);

            HBRUSH bg = CreateSolidBrush(theme::kPageBg);
            FillRect(dc, &rc, bg);
            DeleteObject(bg);

            RECT box = g_pathBox;
            theme::FillRoundRect(dc, box, 7, theme::kCardBg);
            theme::StrokeRoundRect(dc, box, 7, theme::kBorder);

            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            SetBkColor(dc, theme::kPageBg);
            SetTextColor(dc, theme::kTextMain);
            static HBRUSH br = CreateSolidBrush(theme::kPageBg);
            return (LRESULT)br;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetBkColor(dc, theme::kCardBg);
            SetTextColor(dc, theme::kTextMain);
            static HBRUSH br = CreateSolidBrush(theme::kCardBg);
            return (LRESULT)br;
        }
        case WM_HSCROLL:
            if ((HWND)lp == GetDlgItem(h, IDC_SET_LINGER)) UpdateLingerLabel(h);
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDC_SET_BROWSE) { PickFolder(); return 0; }
            if (id == IDC_SET_CLEARCACHE) {
                CacheClearAll();
                SetWindowTextW(GetDlgItem(h, IDC_SET_CAPINFO),
                               L"缓存已清空，下次搜索会重新建立");
                return 0;
            }
            if (id == IDC_SET_OK) {
                SaveFromControls();
                g_accepted = true;
                DestroyWindow(h);
                return 0;
            }
            if (id == IDC_SET_CANCEL) { DestroyWindow(h); return 0; }
            if (id == IDC_SET_RESET) {
                Settings def = DefaultSettings();
                def.rootDir = g_edit.rootDir;
                g_edit = def;
                LoadToControls(h);
                return 0;
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            EnableWindow(g_parent, TRUE);
            SetActiveWindow(g_parent);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

bool ShowSettingsDialog(HWND parent, Settings& s, const std::wstring& capNote) {
    g_parent = parent;
    g_edit = s;
    g_accepted = false;

    static bool clsDone = false;
    if (!clsDone) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"LineOcrSettings";
        wc.hIcon = MakeAppIcon(32);
        wc.hIconSm = MakeAppIcon(16);
        RegisterClassExW(&wc);
        clsDone = true;
    }

    RECT pr{};
    GetWindowRect(parent, &pr);
    int w = 600, h = 774;
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

    g_dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"LineOcrSettings", L"设置",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            x, y, w, h, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_dlg) return false;

    EnableWindow(parent, FALSE);
    ShowWindow(g_dlg, SW_SHOW);
    UpdateWindow(g_dlg);

    SetWindowTextW(GetDlgItem(g_dlg, IDC_SET_CAPINFO), capNote.c_str());

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_QUIT) break;
        if (!IsDialogMessageW(g_dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_accepted) s = g_edit;
    g_dlg = nullptr;
    return g_accepted;
}
