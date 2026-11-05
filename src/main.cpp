// main.cpp - 主窗口、控件、自绘结果列表、后台搜索
#include "common.h"
#include "theme.h"
#include "app_state.h"
#include "flatbutton.h"
#include <shellapi.h>
#include <richedit.h>
#include <commctrl.h>
#include <windowsx.h>
#include <objbase.h>
#include <cwctype>
#include <atomic>

// ---------------- 全局状态 ----------------



AppState g;
HWND g_hwndMain = nullptr;
static HWND g_hwndEdit = nullptr;
HWND g_hwndList = nullptr;
static HWND g_hwndBtnSearch = nullptr;
static HWND g_hwndBtnSet = nullptr;

static bool   g_richOk = false;            // 输入框是否用了 RichEdit（支持局部变色）
static bool   g_editSilent = false;        // 程序自己在改内容时忽略 EN_CHANGE
static size_t g_dimFrom = (size_t)-1;      // 输入框里从第几个字开始是「被缩短掉的」，grey

static std::wstring g_bundleNote;   // 首次启动释放内嵌资源的说明
static std::wstring g_statusLeft;   // 取词状态 / 倒计时
static std::wstring g_statusMid;    // 鼠标位置 · 取词方式
static std::wstring g_statusRight;  // 命中统计
static RECT g_statusRect{};
static RECT g_inputRect{};

int g_lineH = 20;
static const int kTopBarH = 64;
static const int kStatusH = 30;
static const int kCardGap = 8;
static const int kCardPad = 12;
static const int kSideMargin = 14;

struct SearchResultMsg {
    unsigned seq;
    SearchOutcome outcome;
};

// 取词线程 -> 主线程的结果包裹
struct CaptureResultMsg {
    POINT pt = {};
    Settings settings;              // 启动线程时的设置快照
    bool   got = false;
    std::wstring raw, src, fail;
    std::vector<double> charX;
    double mouseX = 0;
    bool   haveX = false;
    bool   estimated = false;
    int    alignPct = -1;
};


// ---------------- 自动取词状态 ----------------
#define TIMER_POLL 1
static POINT    g_lastPt{};
static DWORD    g_stillSince = 0;
static DWORD    g_lastAttempt = 0;
static bool     g_fired = false;
static std::wstring g_lastCaptured;
static bool     g_autoBacktrack = false;
static int      g_backtrackTries = 0;
static const int kMaxBacktrack = 12;

// ---------------- 能力探测 ----------------

Capabilities ProbeCapabilities() {
    Capabilities c;
    c.c1Text = true;

    std::wstring rt  = JoinPath(SubDirPath(cfgpaths::kRuntimeDir), L"onnxruntime.dll");
    std::wstring mdl = JoinPath(SubDirPath(cfgpaths::kModelDir), L"rec.onnx");
    std::wstring dic = JoinPath(SubDirPath(cfgpaths::kModelDir), L"ppocr_keys_v1.txt");
    c.c2Model = FileExists(rt) && FileExists(mdl) && FileExists(dic);

    if (!c.c2Model) c.note = L"未找到模型，图片文字识别不可用";
    return c;
}

// ---------------- 状态栏 ----------------

static std::wstring ModeName() {
    switch (g.settings.captureMode) {
        case 0:  return L"智能";
        case 2:  return L"总是 UIA 元素";
        case 3:  return L"总是识别图片";
        default: return L"自动";
    }
}

static void RefreshStatusBar() {
    if (g_hwndMain && g_statusRect.right > g_statusRect.left) {
        InvalidateRect(g_hwndMain, &g_statusRect, FALSE);
    }
}

void SetLeftStatus(const std::wstring& s) {
    if (g_statusLeft == s) return;
    g_statusLeft = s;
    RefreshStatusBar();
}

// 统计与鼠标位置：只在搜索完成或方式变化时更新
static void SetStatus() {
    g_statusRight = L"命中 " + std::to_wstring(g.outcome.items.size()) + L" 条";
    if (g.outcome.scannedFiles > 0) {
        g_statusRight += L" · 扫描 " + std::to_wstring(g.outcome.scannedFiles) + L" 个文件";
    }
    if (g.outcome.truncated) g_statusRight += L" · 已截断";
    if (!g.statusNote.empty()) g_statusRight += L" · " + g.statusNote;

    g_statusMid = ModeName();
    POINT p{};
    if (GetCursorPos(&p)) {
        wchar_t buf[64];
        swprintf(buf, 64, L"%ld, %ld", p.x, p.y);
        g_statusMid += L" · " + std::wstring(buf);
    }
    RefreshStatusBar();
}

// 注意：WM_CREATE 阶段 g_hwndMain 还没赋值，必须显式传窗口句柄
static void UpdateTitle(HWND hwnd) {
    if (!hwnd) return;
    std::wstring t = L"行取词搜索";
    if (!g.settings.rootDir.empty()) {
        size_t p = g.settings.rootDir.find_last_of(L"\\/");
        std::wstring name = (p == std::wstring::npos) ? g.settings.rootDir
                                                      : g.settings.rootDir.substr(p + 1);
        if (!name.empty()) t += L" · " + name;
    }
    SetWindowTextW(hwnd, t.c_str());
}

static std::wstring TrimW2(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) b++;
    while (e > b && iswspace(s[e - 1])) e--;
    return s.substr(b, e - b);
}

// ---------------- 后台搜索 ----------------

static DWORD WINAPI SearchThreadProc(LPVOID param) {
    SearchResultMsg* msg = (SearchResultMsg*)param;
    SearchRequest req;
    req.keyword      = msg->outcome.keyword;
    req.rootDir      = g.settings.rootDir;
    req.searchOffice = g.settings.searchOffice;
    req.contextLevel = g.settings.contextLevel;
    req.maxResults   = g.settings.maxResults;

    SearchOutcome res = RunSearch(req);
    msg->outcome = std::move(res);
    PostMessageW(g_hwndMain, WM_APP_RESULTS, 0, (LPARAM)msg);
    return 0;
}

// ---------------- 输入框（RichEdit：可以把「被缩短的部分」置灰）----------------

static std::wstring EditText() {
    if (!g_hwndEdit) return std::wstring();
    int n = GetWindowTextLengthW(g_hwndEdit);
    if (n <= 0) return std::wstring();
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(g_hwndEdit, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}

// keepCaret=true 时保留用户光标位置（用户自己打字时调用）；false 时把光标放到末尾
static void EditPaintColors(bool keepCaret) {
    if (!g_richOk || !g_hwndEdit) return;
    // 改字符格式也会触发 EN_CHANGE，必须静默，否则刚置的灰会被自己清掉
    bool wasSilent = g_editSilent;
    g_editSilent = true;

    CHARRANGE save{ 0, 0 };
    if (keepCaret) SendMessageW(g_hwndEdit, EM_EXGETSEL, 0, (LPARAM)&save);

    int n = GetWindowTextLengthW(g_hwndEdit);

    CHARRANGE all{ 0, -1 };
    SendMessageW(g_hwndEdit, EM_EXSETSEL, 0, (LPARAM)&all);
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = theme::kTextMain;
    SendMessageW(g_hwndEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

    if (g_dimFrom < (size_t)n) {
        CHARRANGE dim{ (LONG)g_dimFrom, -1 };
        SendMessageW(g_hwndEdit, EM_EXSETSEL, 0, (LPARAM)&dim);
        cf.crTextColor = theme::kTextFaint;
        SendMessageW(g_hwndEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    }

    if (keepCaret) {
        SendMessageW(g_hwndEdit, EM_EXSETSEL, 0, (LPARAM)&save);
    } else {
        CHARRANGE end{ n, n };
        SendMessageW(g_hwndEdit, EM_EXSETSEL, 0, (LPARAM)&end);
    }
    g_editSilent = wasSilent;
}

// 程序写入整段文字：整段都是「实际搜索用到的」
static void EditSetText(const std::wstring& text) {
    if (!g_hwndEdit) return;
    g_dimFrom = (size_t)-1;
    g_editSilent = true;
    SetWindowTextW(g_hwndEdit, text.c_str());
    g_editSilent = false;
    EditPaintColors(false);
    InvalidateRect(g_hwndEdit, nullptr, TRUE);
}

// 文字不动，只把「前 used 个字」当作用中的，其余置灰（自动缩短关键词时用）
static void EditSetUsed(size_t used) {
    if (!g_hwndEdit) return;
    size_t n = (size_t)GetWindowTextLengthW(g_hwndEdit);
    g_dimFrom = (used >= n) ? (size_t)-1 : used;
    if (g_richOk) {
        EditPaintColors(false);
        InvalidateRect(g_hwndEdit, nullptr, TRUE);
    }
}

// 单行输入：回车直接搜索，不让插换行
static LRESULT CALLBACK EditSubclass(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                     UINT_PTR id, DWORD_PTR ref) {
    if (m == WM_KEYDOWN && wp == VK_RETURN) {
        if (g_hwndMain) {
            PostMessageW(g_hwndMain, WM_COMMAND,
                         MAKEWPARAM(IDC_BTN_SEARCH, BN_CLICKED), 0);
        }
        return 0;
    }
    if (m == WM_CHAR && (wp == L'\r' || wp == L'\n')) return 0;
    return DefSubclassProc(h, m, wp, lp);
}

static void StartSearch(const std::wstring& keyword, bool fromCapture = false) {
    std::wstring kw = TrimW2(keyword);
    if (kw.empty()) return;
    if (!fromCapture) { g_autoBacktrack = false; g_backtrackTries = 0; }
    else { g_autoBacktrack = true; }

    if (g.settings.rootDir.empty()) {
        g.statusNote = L"请先拖一个文件夹到窗口";
        SetStatus();
        return;
    }
    if (InterlockedCompareExchange(&g.searching, 1, 0) != 0) {
        g.statusNote = L"上一次搜索还在进行";
        SetStatus();
        return;
    }

    SearchResultMsg* msg = new SearchResultMsg();
    msg->seq = ++g.seq;
    msg->outcome.keyword = kw;

    g.selected = -1;
    g.scrollY  = 0;
    g.statusNote = L"搜索中…";
    SetStatus();
    InvalidateRect(g_hwndList, nullptr, TRUE);

    HANDLE th = CreateThread(nullptr, 0, SearchThreadProc, msg, 0, nullptr);
    if (th) CloseHandle(th);
    else {
        delete msg;
        InterlockedExchange(&g.searching, 0);
    }
}

// 状态栏实时反映鼠标位置与取词方式
static void UpdateMouseStatus(POINT p) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%ld, %ld", p.x, p.y);
    std::wstring mid = ModeName();
    mid += L" · " + std::wstring(buf);
    if (mid != g_statusMid) {
        g_statusMid = mid;
        RefreshStatusBar();
    }
}

// 鼠标静止判定 -> 取词（自动模式先文字层、再图像层，取到的那一路才算数）
// 截一条鼠标附近的带子认出「一行」：先从上下各 80px 找，找不到再扩到 150px
// 输出 text / 每字像素 x（条带内坐标）/ mouseLocalX，像素缓冲用完清零
static bool OcrLineAtCursor(std::wstring& text, std::vector<double>& xs,
                            int& mouseLocalX, std::wstring& fail) {
    text.clear();
    xs.clear();
    mouseLocalX = 0;

    if (!g.caps.c2Model) { fail = L"图片识别不可用（缺模型）"; return false; }
    if (!OcrReady()) {
        std::wstring err;
        if (!OcrInit(err)) { g.caps.c2Model = false; fail = err; return false; }
    }

    const int radii[2] = { 80, 150 };
    for (int ri = 0; ri < 2; ++ri) {
        CapturedBgra cap;
        if (!CaptureBand(400, radii[ri], cap)) { fail = L"截屏失败"; continue; }

        std::wstring t;
        std::vector<double> v;
        int lt = 0, lb = 0;
        bool ok = OcrRecognizeBgra(cap.px.data(), cap.w, cap.h, cap.mouseLocalY,
                                   t, &lt, &lb, &v);
        int mx = cap.mouseLocalX;
        WipeBytes(cap.px);

        if (!ok) { fail = L"图片里没找到文字行"; continue; }
        if (CountTextChars(CleanOcrText(t)) < 2) { fail = L"图片里没认出文字"; continue; }

        text = t;
        xs = v;
        mouseLocalX = mx;
        return true;
    }
    return false;
}

// ---------------- 取词主体（在工作线程里跑）----------------
// 只算不画：不动任何 UI。UIA / OCR 卡住时只影响这一次取词，界面不冻。
static void CaptureFlow(const Settings& st, POINT p, CaptureResultMsg* out) {
    const int mode = st.captureMode;           // 0 智能 / 1 自动 / 2 总是 UIA / 3 总是图片

    std::wstring raw, src, fail;
    bool got = false;
    std::vector<double> charX;      // 每字水平位置（与 mouseX 同一坐标系）
    double mouseX = (double)p.x;
    bool haveX = false;
    bool estimated = false;         // 位置是按元素矩形估算的
    int  alignPct = -1;             // 智能模式的对齐命中率

    // 纯 OCR 的结果先留着：智能模式对齐失败、或文字层没结果时直接复用，不重复截屏
    std::wstring ocrText, ocrFail;
    std::vector<double> ocrX;
    int  ocrMouseX = 0;
    bool ocrTried = false, ocrOk = false;



    // 纯 OCR 的结果先留着：智能模式对齐失败、或文字层没结果时直接复用，不重复截屏

    // ---- 1) 精确路径：原生 EDIT（不截屏，位置精确）----
    if (mode != 3) {
        std::wstring t, s2;
        RECT rc{};
        std::vector<double> xs;
        if (CaptureNativeEditLine(t, s2, &rc, &xs)) {
            std::wstring cand = (xs.size() == t.size())
                ? CleanOcrTextByX(t, xs, (double)p.x)
                : CleanOcrText(t);
            if (CountTextChars(cand) >= 2) {
                raw = t;
                src = s2;
                charX = xs;
                haveX = (xs.size() == t.size());
                got = true;
            }
        }
    }

    // ---- 2) 智能：像素定位置 + UIA 校文字 ----
    if (!got && mode == 0) {
        ocrTried = true;
        ocrOk = OcrLineAtCursor(ocrText, ocrX, ocrMouseX, ocrFail);
        if (!ocrOk) {
            fail = ocrFail;
        } else {
            std::wstring doc;
            AlignResult ar;
            if (GetUiaDocumentText(p, ocrText, doc)) ar = AlignLineToDocument(doc, ocrText, 0.40);
            if (ar.ok) {
                std::wstring wt;
                std::vector<double> wx;
                BuildAlignedLine(doc, ar, ocrX, wt, wx);
                if (!wt.empty() && wx.size() == wt.size()) {
                    raw = wt;
                    charX = wx;
                    mouseX = (double)ocrMouseX;
                    haveX = true;
                    alignPct = (int)(ar.ratio * 100.0 + 0.5);
                    src = L"智能";
                    got = true;
                }
            }
            if (!got) fail = L"对齐失败";
        }
    }

    // ---- 3) 文字层（UIA / MSAA）：自动与纯 UIA 模式的主路径，智能模式的回退 ----
    if (!got && (mode == 1 || mode == 2 || mode == 0)) {
        std::wstring t, s2;
        RECT rc{};
        std::vector<double> xs;
        if (CaptureTextLayer(p, t, s2, &rc, &xs)) {
            std::wstring cand = (xs.size() == t.size())
                ? CleanOcrTextByX(t, xs, (double)p.x)
                : CleanOcrText(t);
            if (CountTextChars(cand) >= 2) {
                raw = t;
                src = s2;
                charX = xs;
                haveX = (xs.size() == t.size());
                estimated = true;
                got = true;
            } else {
                fail = L"控件文字太少";
            }
        } else if (fail.empty()) {
            fail = L"控件读不到文字";
        }
    }

    // ---- 4) 纯 OCR 兜底 ----
    if (!got && (mode == 0 || mode == 1 || mode == 3)) {
        if (!ocrTried) {
            ocrTried = true;
            ocrOk = OcrLineAtCursor(ocrText, ocrX, ocrMouseX, ocrFail);
        }
        if (ocrOk) {
            raw = ocrText;
            charX = ocrX;
            mouseX = (double)ocrMouseX;
            haveX = (ocrX.size() == ocrText.size());
            src = L"图片识别";
            got = true;
        } else {
            fail = ocrFail;
        }
    }

    out->fail = fail;


    out->got = got;
    out->raw = raw;
    out->src = src;
    out->charX = charX;
    out->mouseX = mouseX;
    out->haveX = haveX;
    out->estimated = estimated;
    out->alignPct = alignPct;
}

// 取词工作线程：COM 按 MTA 初始化（同一时刻只有一个取词在跑）
static DWORD WINAPI CaptureThreadProc(LPVOID param) {
    CaptureResultMsg* msg = (CaptureResultMsg*)param;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CaptureFlow(msg->settings, msg->pt, msg);
    if (SUCCEEDED(co)) CoUninitialize();
    if (g_hwndMain) PostMessageW(g_hwndMain, WM_APP_CAPTURE, 0, (LPARAM)msg);
    else delete msg;
    return 0;
}

// 主线程收取词结果：只做 UI 更新与触发搜索
static void OnCaptureDone(CaptureResultMsg* msg) {
    InterlockedExchange(&g.capturing, 0);

    if (!msg->got) {
        SetLeftStatus(L"未取到文字 · " + (msg->fail.empty() ? std::wstring(L"无可用通道") : msg->fail));
        delete msg;
        return;
    }

    g.lastOcrRaw = msg->raw;
    std::wstring clean = (msg->haveX && msg->charX.size() == msg->raw.size())
        ? CleanOcrTextByX(msg->raw, msg->charX, msg->mouseX)
        : CleanOcrText(msg->raw);
    std::wstring note = L"已取词 · " + msg->src;
    if (msg->alignPct >= 0) note += L" · 对齐 " + std::to_wstring(msg->alignPct) + L"%";
    if (msg->estimated) note += L" · 位置为估算";
    SetLeftStatus(note);

    if (clean == g_lastCaptured) { delete msg; return; }  // 内容没变就不重复搜索
    g_lastCaptured = clean;

    EditSetText(clean);
    if (g.settings.autoSearch) StartSearch(clean, true);
    delete msg;
}

static void OnPollTick() {
    POINT p{};
    if (!GetCursorPos(&p)) return;
    UpdateMouseStatus(p);

    if (!g.settings.autoCapture) {
        SetLeftStatus(L"自动取词已关闭");
        return;
    }

    // 鼠标一动就重新计时
    if (p.x != g_lastPt.x || p.y != g_lastPt.y) {
        g_lastPt = p;
        g_stillSince = GetTickCount();
        g_fired = false;
        SetLeftStatus(L"");
        return;
    }

    // 鼠标在本进程的任何窗口里都不取词：主窗口、设置窗口、自绘按钮全部算在内，
    // 否则会读到自己的界面文字（比如把窗口标题「设置」当成取词结果）
    HWND h = WindowFromPoint(p);
    if (h) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) {
            SetLeftStatus(L"鼠标在自己的窗口内，不取词");
            return;
        }
    }

    DWORD elapsed = GetTickCount() - g_stillSince;
    if (elapsed < (DWORD)g.settings.lingerMs) {
        int remain = (int)((DWORD)g.settings.lingerMs - elapsed);
        wchar_t buf[64];
        swprintf(buf, 64, L"等待 %.1f 秒", remain / 1000.0);
        SetLeftStatus(buf);
        return;
    }

    if (g.searching) {
        SetLeftStatus(L"搜索中…");
        return;
    }

    // 同一点位允许重试：上次尝试后冷却 1.2 秒，避免连续截屏
    DWORD now = GetTickCount();
    if (g_fired && (DWORD)(now - g_lastAttempt) < 1200) return;

    g_fired = true;
    g_lastAttempt = now;
    SetLeftStatus(L"取词中…");

    // 取词放进工作线程：UIA 卡住只影响这一次取词，界面不冻
    if (InterlockedExchange(&g.capturing, 1) != 0) return;

    CaptureResultMsg* msg = new CaptureResultMsg();
    msg->pt = p;
    msg->settings = g.settings;   // 快照：线程跑期间改设置不影响这一次
    HANDLE th = CreateThread(nullptr, 0, CaptureThreadProc, msg, 0, nullptr);
    if (th) {
        CloseHandle(th);
    } else {
        InterlockedExchange(&g.capturing, 0);
        delete msg;
        SetLeftStatus(L"取词线程启动失败");
    }
    return;
}

static void OnResults(SearchResultMsg* msg) {
    bool needBacktrack = false;
    std::wstring nextKw;

    if (msg->seq == g.seq) {
        g.outcome = std::move(msg->outcome);
        g.selected = -1;
        g.scrollY  = 0;
        g.statusNote.clear();

        if (g.outcome.items.empty() && g.settings.backtrack && g_autoBacktrack &&
            g_backtrackTries < kMaxBacktrack) {
            const std::wstring& kw = g.outcome.keyword;
            std::wstring cand = kw;
            while (!cand.empty() && !iswalnum(cand.back()) && CountTextChars(cand) > 2) {
                cand.pop_back();
            }
            if (CountTextChars(cand) > 2) {
                nextKw = cand.substr(0, cand.size() - 1);
                if (CountTextChars(nextKw) >= 2) needBacktrack = true;
            }
        }

        RecalcTotalHeight();
        InvalidateRect(g_hwndList, nullptr, TRUE);
        SetStatus();
    }

    InterlockedExchange(&g.searching, 0);
    delete msg;

    if (needBacktrack) {
        ++g_backtrackTries;
        EditSetUsed(nextKw.size());   // 文字保留，只把被缩掉的部分置灰
        g.statusNote = L"自动缩短关键词（第 " + std::to_wstring(g_backtrackTries) + L" 次）";
        StartSearch(nextKw, true);
    } else if (g_autoBacktrack && g.outcome.items.empty() && g_backtrackTries > 0) {
        g.statusNote = L"已缩到最短仍未找到";
        SetStatus();
        g_autoBacktrack = false;
    }
}

// ---------------- 布局 ----------------

static void Layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);

    int margin = 16;
    int btnW = 78;
    int btnH = 32;
    int gap = 8;
    int inputH = 32;
    int inputRight = rc.right - margin - btnW * 2 - gap * 2;
    int inputW = inputRight - margin;
    if (inputW < 80) inputW = 80;

    g_inputRect.left = margin;
    g_inputRect.top = (kTopBarH - inputH) / 2;
    g_inputRect.right = margin + inputW;
    g_inputRect.bottom = g_inputRect.top + inputH;

    int editInset = 11;
    MoveWindow(g_hwndEdit,
               g_inputRect.left + editInset,
               g_inputRect.top + (inputH - 22) / 2,
               inputW - editInset * 2, 22, TRUE);

    MoveWindow(g_hwndBtnSearch, inputRight + gap, (kTopBarH - btnH) / 2, btnW, btnH, TRUE);
    MoveWindow(g_hwndBtnSet, inputRight + gap + btnW + gap, (kTopBarH - btnH) / 2, btnW, btnH, TRUE);

    int listH = rc.bottom - kTopBarH - kStatusH;
    if (listH < 0) listH = 0;
    MoveWindow(g_hwndList, 0, kTopBarH, rc.right, listH, TRUE);

    g_statusRect.left = 0;
    g_statusRect.right = rc.right;
    g_statusRect.bottom = rc.bottom;
    g_statusRect.top = rc.bottom - kStatusH;
}

// ---------------- 主窗口绘制 ----------------

static void PaintMain(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);

    HBRUSH bg = CreateSolidBrush(theme::kPageBg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);

    // 输入框外框（EDIT 本身无边框，画在它下面）
    RECT box = g_inputRect;
    theme::FillRoundRect(dc, box, 7, theme::kCardBg);
    theme::StrokeRoundRect(dc, box, 7, theme::kBorder);

    // 状态栏分隔线
    RECT sr = g_statusRect;
    HPEN pen = CreatePen(PS_SOLID, 1, theme::kBorder);
    HGDIOBJ op = SelectObject(dc, pen);
    MoveToEx(dc, 0, sr.top, nullptr);
    LineTo(dc, sr.right, sr.top);
    SelectObject(dc, op);
    DeleteObject(pen);

    HGDIOBJ oldFont = SelectObject(dc, theme::FontUI());
    SIZE sz{};
    int textY = sr.top + (kStatusH - g_lineH) / 2;

    // 右段：命中统计
    GetTextExtentPoint32W(dc, g_statusRight.c_str(), (int)g_statusRight.size(), &sz);
    int rx = sr.right - 16 - sz.cx;

    // 左段：取词状态 / 倒计时 / 未设置提示
    std::wstring leftText = g_statusLeft;
    if (leftText.empty() && g.settings.rootDir.empty()) {
        leftText = L"未设置搜索文件夹（拖一个文件夹进来）";
    }
    SetTextColor(dc, theme::kTextMain);
    RECT lc{ 16, sr.top, rx - 24, sr.bottom };
    ExtTextOutW(dc, 16, textY, ETO_CLIPPED, &lc,
                leftText.c_str(), (int)leftText.size(), nullptr);
    GetTextExtentPoint32W(dc, leftText.c_str(), (int)leftText.size(), &sz);
    int leftW = sz.cx;

    // 中段：取词方式 · 鼠标位置
    int mx = 16 + leftW + 24;
    if (mx < rx - 140) {
        SetTextColor(dc, theme::kTextFaint);
        RECT mc{ mx, sr.top, rx - 12, sr.bottom };
        ExtTextOutW(dc, mx, textY, ETO_CLIPPED, &mc,
                    g_statusMid.c_str(), (int)g_statusMid.size(), nullptr);
    }

    // 右段
    SetTextColor(dc, theme::kTextSub);
    RECT rcl{ rx, sr.top, sr.right - 16, sr.bottom };
    ExtTextOutW(dc, rx, textY, ETO_CLIPPED, &rcl,
                g_statusRight.c_str(), (int)g_statusRight.size(), nullptr);

    SelectObject(dc, oldFont);
    EndPaint(hwnd, &ps);
}

// ---------------- 主窗口过程 ----------------

static void CreateControls(HWND hwnd) {
    const wchar_t* editCls = g_richOk ? MSFTEDIT_CLASS : L"EDIT";
    DWORD editStyle = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NOHIDESEL;
    if (g_richOk) editStyle |= ES_MULTILINE;   // RichEdit 即使是单行也必须带

    g_hwndEdit = CreateWindowExW(0, editCls, L"", editStyle,
        0, 0, 100, 22, hwnd, (HMENU)IDC_EDIT, nullptr, nullptr);
    SendMessageW(g_hwndEdit, WM_SETFONT, (WPARAM)theme::FontUI(), TRUE);
    SendMessageW(g_hwndEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    if (g_richOk) {
        SendMessageW(g_hwndEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)theme::kCardBg);
        // RichEdit 默认不通知内容变化，必须显式打开，否则用户打字时我们收不到 EN_CHANGE
        SendMessageW(g_hwndEdit, EM_SETEVENTMASK, 0, ENM_CHANGE);
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = theme::kTextMain;
        SendMessageW(g_hwndEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    }
    SetWindowSubclass(g_hwndEdit, EditSubclass, 1, 0);

    g_hwndBtnSearch = CreateFlatButton(hwnd, IDC_BTN_SEARCH, L"搜索", true);
    g_hwndBtnSet = CreateFlatButton(hwnd, IDC_BTN_SET, L"设置", false);

    g_hwndList = CreateWindowExW(0, L"LineOcrList", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 100, 100, hwnd, (HMENU)IDC_LIST, nullptr, nullptr);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            theme::InitFonts();
            g_lineH = theme::LineHeight();

            g.settings = DefaultSettings();
            LoadSettings(g.settings);
            CacheSetLimitMB(g.settings.cacheMB);

            CreateControls(hwnd);

            // 标题栏 / 任务栏图标（代码绘制，不依赖 ico 资源）
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)MakeAppIcon(32));
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)MakeAppIcon(16));

            DragAcceptFiles(hwnd, TRUE);
            SetTimer(hwnd, TIMER_POLL, 60, nullptr);

            Layout(hwnd);
            UpdateTitle(hwnd);
            g.caps = ProbeCapabilities();
            g.statusNote = g.caps.note;
            if (!g_bundleNote.empty()) {
                g.statusNote = g.statusNote.empty()
                    ? g_bundleNote
                    : (g_bundleNote + L" · " + g.statusNote);
            }
            SetStatus();
            return 0;
        }
        case WM_SIZE:
            Layout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            PaintMain(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, theme::kTextMain);
            SetBkColor(dc, theme::kCardBg);
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize.x = 560;
            mm->ptMinTrackSize.y = 400;
            return 0;
        }
        case WM_DROPFILES: {
            HDROP hd = (HDROP)wp;
            wchar_t path[MAX_PATH * 2] = {0};
            if (DragQueryFileW(hd, 0, path, (UINT)(sizeof(path) / sizeof(path[0])))) {
                DWORD attr = GetFileAttributesW(path);
                if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                    g.settings.rootDir = path;
                    SaveSettings(g.settings);
                    UpdateTitle(hwnd);
                    g.statusNote = L"已切换文件夹";
                    SetStatus();
                }
            }
            DragFinish(hd);
            return 0;
        }
        case WM_KEYDOWN: {
            // 键盘导航只在焦点不在输入框时生效（在输入框里打字/光标不受影响）
            HWND focus = GetFocus();
            if (focus != hwnd && focus != g_hwndList) break;

            if (wp == VK_DOWN || wp == VK_UP) {
                if (g.outcome.items.empty()) return 0;
                int n = (int)g.outcome.items.size();
                int sel = g.selected;
                if (sel < 0) sel = (wp == VK_DOWN) ? 0 : n - 1;
                else sel += (wp == VK_DOWN) ? 1 : -1;
                if (sel < 0) sel = 0;
                if (sel >= n) sel = n - 1;
                g.selected = sel;
                EnsureItemVisible(sel);
                InvalidateRect(g_hwndList, nullptr, TRUE);
                return 0;
            }
            if (wp == VK_RETURN) {
                if (g.selected >= 0 && g.selected < (int)g.outcome.items.size()) {
                    OpenItem(g.selected);
                }
                return 0;
            }
            if (wp == VK_ESCAPE) {
                g.selected = -1;
                InvalidateRect(g_hwndList, nullptr, TRUE);
                return 0;
            }
            if (wp == L'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                if (g.selected >= 0 && g.selected < (int)g.outcome.items.size()) {
                    CopyResultToClipboard(g.outcome.items[g.selected]);
                }
                return 0;
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDC_EDIT && HIWORD(wp) == EN_CHANGE) {
                // 用户自己改了内容：整段都算「实际在用」，取消置灰
                if (!g_editSilent && g_dimFrom != (size_t)-1) {
                    g_dimFrom = (size_t)-1;
                    EditPaintColors(true);
                }
                return 0;
            }
            if (id == IDC_BTN_SEARCH) {
                std::wstring kw = EditText();
                if (g_dimFrom != (size_t)-1) {   // 手动搜索 = 搜输入框里看到的整段
                    g_dimFrom = (size_t)-1;
                    EditPaintColors(false);
                }
                StartSearch(kw);
            } else if (id == IDC_BTN_SET) {
                std::wstring note = L"当前可用：控件文字";
                if (g.caps.c2Model) note += L"、模型识别";
                else note += L"（未找到模型，图片文字识别不可用）";
                if (ShowSettingsDialog(hwnd, g.settings, note)) {
                    SaveSettings(g.settings);
                    CacheSetLimitMB(g.settings.cacheMB);
                    UpdateTitle(hwnd);
                    g.statusNote = L"设置已保存";
                    SetStatus();
                }
            }
            return 0;
        }
        case WM_APP_CAPTURE:
            OnCaptureDone((CaptureResultMsg*)lp);
            return 0;
        case WM_APP_RESULTS:
            OnResults((SearchResultMsg*)lp);
            return 0;
        case WM_TIMER:
            if (wp == TIMER_POLL) { OnPollTick(); return 0; }
            break;
        case WM_DESTROY:
            KillTimer(hwnd, TIMER_POLL);
            theme::DoneFonts();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- 入口 ----------------

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    // 单实例：已经有一个在跑时，把那个窗口拉到前面然后退出，
    // 避免两个进程同时盯鼠标、互相抢取词结果
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\LineOcrSearch.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(L"LineOcrMain", nullptr);
        if (prev) {
            ShowWindow(prev, SW_RESTORE);
            SetForegroundWindow(prev);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    EnablePerMonitorDpi();

    // RichEdit：只有它能把输入框里「被缩短的部分」置灰；缺失时退回原生 EDIT
    g_richOk = (LoadLibraryW(L"msftedit.dll") != nullptr);

    // 旧版数据目录叫 config\，新版叫 LineOcrSearch\：能改名就改名，省得重放 21MB。
    MigrateLegacyDataDir();

    // 依赖内嵌在 exe 里：先检查数据目录，缺失或大小不符才释放。
    // 必须在创建窗口之前做完，否则能力探测会把「还没释放」误判成「缺依赖」。
    ExtractBundledResources(g_bundleNote);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HICON iconBig = MakeAppIcon(32);
    HICON iconSmall = MakeAppIcon(16);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"LineOcrMain";
    wc.hIcon = iconBig;
    wc.hIconSm = iconSmall;
    RegisterClassExW(&wc);

    WNDCLASSEXW lc{ sizeof(lc) };
    lc.lpfnWndProc = ListProc;
    lc.hInstance = hInst;
    lc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    lc.hbrBackground = nullptr;
    lc.lpszClassName = L"LineOcrList";
    RegisterClassExW(&lc);

    // 每次启动都用固定默认尺寸并居中，不记忆上次的窗口大小
    const int kDefW = 920, kDefH = 660;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (work.right <= work.left || work.bottom <= work.top) {
        work.left = 0;
        work.top = 0;
        work.right = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int winW = kDefW, winH = kDefH;
    if (winW > work.right - work.left) winW = work.right - work.left;
    if (winH > work.bottom - work.top) winH = work.bottom - work.top;
    int winX = work.left + (work.right - work.left - winW) / 2;
    int winY = work.top + (work.bottom - work.top - winH) / 2;

    g_hwndMain = CreateWindowExW(0, L"LineOcrMain", L"行取词搜索",
        WS_OVERLAPPEDWINDOW,
        winX, winY, winW, winH,
        nullptr, nullptr, hInst, nullptr);
    if (!g_hwndMain) return 1;

    ShowWindow(g_hwndMain, nCmdShow);
    UpdateWindow(g_hwndMain);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return (int)msg.wParam;
}
