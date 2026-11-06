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
static size_t g_usedStart = 0;             // 实际搜索词在输入框里的起点

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

static int S(int v) { return theme::Scale(v); }

// Win10 1607+ 的 GetDpiForWindow 动态取；Win7 退回系统 DPI。
static int DpiForWindowCompat(HWND hwnd) {
    typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn fn = nullptr;
    static bool loaded = false;
    if (!loaded) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) fn = (GetDpiForWindowFn)GetProcAddress(u, "GetDpiForWindow");
        loaded = true;
    }
    if (fn && hwnd) {
        UINT d = fn(hwnd);
        if (d) return (int)d;
    }
    HDC dc = GetDC(hwnd);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return dpi > 0 ? dpi : 96;
}

struct SearchResultMsg {
    unsigned seq;
    // 请求在主线程填好再交给工作线程：线程里一律不碰 g.settings
    // （主线程可能正在保存设置/拖入新文件夹，std::wstring 赋值不是原子的）
    SearchRequest req;
    SearchOutcome outcome;
};

// 取词线程 -> 主线程的结果包裹
struct CaptureResultMsg {
    POINT pt = {};
    unsigned seq = 0;               // 取词代次：超时后旧线程的结果作废
    Settings settings;              // 启动线程时的设置快照
    bool   got = false;
    std::wstring raw, src, fail;
    std::vector<double> charX;
    double mouseX = 0;
    bool   haveX = false;
    bool   estimated = false;
    int    alignPct = -1;
    float  confidence = -1.0f;      // OCR 置信度；UIA/控件为 -1
};


// ---------------- 自动取词状态 ----------------
#define TIMER_POLL 1
static POINT    g_lastPt{};
static DWORD    g_stillSince = 0;
static DWORD    g_lastAttempt = 0;
static bool     g_fired = false;
static int      g_attemptsAtPoint = 0;   // 同一个静止点位已经取过几次（失败就放弃）
static unsigned g_captureSeq = 0;        // 取词代次：超时后丢弃旧线程结果
static DWORD    g_captureStart = 0;
static bool     g_userEditing = false;    // 用户正在改输入框：先别自动取词覆盖
static std::wstring g_lastCaptured;
static std::wstring g_lastCapturedRoot;
static bool     g_autoBacktrack = false;
static int      g_backtrackTries = 0;
static const int kMaxBacktrack = 12;

// 搜索忙时取词拿到的新关键词不丢：挂起最后一次，搜完补跑。
static std::wstring g_pendingKeyword;
static bool     g_pendingFromCapture = false;
static bool     g_hasPendingSearch = false;
static bool     g_inSizeMove = false;   // 用户正在拖动窗口边缘：列表重测延后

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

static DWORD WINAPI OcrPrewarmProc(LPVOID) {
    std::wstring err;
    OcrInit(err);
    return 0;
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
    const int shown = (int)g.outcome.items.size();
    const int total = g.outcome.totalHits > 0 ? g.outcome.totalHits : shown;
    // 撞到收集上限时 total 只是「至少这么多」，加个 ≥ 避免被当成精确次数
    g_statusRight = std::wstring(L"命中 ") + (g.outcome.truncated ? L"≥ " : L"") +
                    std::to_wstring(total) + L" 条";
    if (g.outcome.totalFiles > 0) {
        g_statusRight += L" · " + std::to_wstring(g.outcome.totalFiles) + L" 个文件";
    }
    if (shown > 0 && shown < total) {
        g_statusRight += L" · 显示 " + std::to_wstring(shown) + L" 条";
    }
    if (g.outcome.truncated && g.outcome.pending.empty()) g_statusRight += L" · 结果已截断";
    if (total == 0 && g.outcome.scannedFiles > 0) {
        g_statusRight += L" · 扫描 " + std::to_wstring(g.outcome.scannedFiles) + L" 个文件";
    }
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
    SearchOutcome res = RunSearch(msg->req);   // req 是主线程填好的快照
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

    // 灰色 = 没参与搜索的部分。搜索词通常是 OCR 行中间的一段，
    // 所以起点之前、终点之后都要置灰。
    auto dimRange = [&](size_t b, size_t e) {
        if (b >= (size_t)n || e <= b) return;
        if (e > (size_t)n) e = (size_t)n;
        CHARRANGE dim{ (LONG)b, (LONG)e };
        SendMessageW(g_hwndEdit, EM_EXSETSEL, 0, (LPARAM)&dim);
        cf.crTextColor = theme::kTextFaint;
        SendMessageW(g_hwndEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    };
    size_t usedEnd = (g_dimFrom < (size_t)n) ? g_dimFrom : (size_t)n;
    if (g_usedStart > 0) dimRange(0, g_usedStart);
    if (usedEnd < (size_t)n) dimRange(usedEnd, (size_t)n);

    if (keepCaret) {
        SendMessageW(g_hwndEdit, EM_EXSETSEL, 0, (LPARAM)&save);
    } else {
        CHARRANGE end{ n, n };
        SendMessageW(g_hwndEdit, EM_EXSETSEL, 0, (LPARAM)&end);
    }
    g_editSilent = wasSilent;
}

static std::wstring SingleLineForEdit(const std::wstring& s) {
    std::wstring t = s;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == L'\r' || t[i] == L'\n' || t[i] == L'\t') t[i] = L' ';
    }
    return t;
}

// 程序写入一段文字：默认整段都是「实际搜索用到的」
static void EditSetText(const std::wstring& text) {
    if (!g_hwndEdit) return;
    g_usedStart = 0;
    g_dimFrom = (size_t)-1;
    g_editSilent = true;
    SetWindowTextW(g_hwndEdit, text.c_str());
    g_editSilent = false;
    EditPaintColors(false);
    InvalidateRect(g_hwndEdit, nullptr, TRUE);
}

static void EditSetCaret(size_t pos) {
    if (!g_richOk || !g_hwndEdit) return;
    int n = GetWindowTextLengthW(g_hwndEdit);
    if (pos > (size_t)n) pos = (size_t)n;
    CHARRANGE cr{ (LONG)pos, (LONG)pos };
    SendMessageW(g_hwndEdit, EM_EXSETSEL, 0, (LPARAM)&cr);
    SendMessageW(g_hwndEdit, EM_SCROLLCARET, 0, 0);
}

// 整段文字保留，只把 [start, start+len) 当作用中的，前后置灰。
// 这样能同时看到 OCR 原文、纯文字段、实际搜索词。
static void EditSetUsedRange(size_t start, size_t len) {
    if (!g_hwndEdit) return;
    size_t n = (size_t)GetWindowTextLengthW(g_hwndEdit);
    if (start > n) start = n;
    size_t end = start + len;
    if (end > n) end = n;
    g_usedStart = start;
    g_dimFrom = (end >= n) ? (size_t)-1 : end;
    if (g_richOk) {
        EditPaintColors(false);
        EditSetCaret(end);   // 输入框很窄时，把视图滚到搜索词附近
        InvalidateRect(g_hwndEdit, nullptr, TRUE);
    }
}

// 文字不动，只把「从搜索起点开始的 used 个字」当作用中的（自动缩短关键词时用）
static void EditSetUsed(size_t used) {
    if (!g_hwndEdit) return;
    size_t n = (size_t)GetWindowTextLengthW(g_hwndEdit);
    size_t start = (g_usedStart <= n) ? g_usedStart : 0;
    size_t end = start + used;
    if (end > n) end = n;
    g_usedStart = start;
    g_dimFrom = (end >= n) ? (size_t)-1 : end;
    if (g_richOk) {
        EditPaintColors(false);
        EditSetCaret(end);
        InvalidateRect(g_hwndEdit, nullptr, TRUE);
    }
}

static void CycleFocus(bool back);   // 定义在下面：Tab 键共用同一条焦点链

// 单行输入：回车直接搜索，不让插换行；↓/↑ 跳到结果列表
static LRESULT CALLBACK EditSubclass(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                     UINT_PTR id, DWORD_PTR ref) {
    // RichEdit 是 ES_MULTILINE，Tab 会被它当成制表符插进关键词（还会把关键词
    // 拆成两个词做 AND）。这里直接接管，转给主窗口的焦点链。
    if (m == WM_KEYDOWN && wp == VK_TAB) {
        CycleFocus((GetKeyState(VK_SHIFT) & 0x8000) != 0);
        return 0;
    }
    if (m == WM_KEYDOWN && wp == VK_RETURN) {
        // Ctrl+Enter = 加载更多，交给主窗口处理；单独 Enter 才是搜索
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            if (g_hwndMain) PostMessageW(g_hwndMain, WM_KEYDOWN, VK_RETURN, lp);
            return 0;
        }
        if (g_hwndMain) {
            PostMessageW(g_hwndMain, WM_COMMAND,
                         MAKEWPARAM(IDC_BTN_SEARCH, BN_CLICKED), 0);
        }
        return 0;
    }
    // ↓/↑：单行输入框里没有别的含义，直接进结果列表
    if (m == WM_KEYDOWN && (wp == VK_DOWN || wp == VK_UP)) {
        if (g_hwndList && !g.outcome.items.empty()) {
            int last = (int)g.outcome.items.size() - 1;
            g.selected = (wp == VK_DOWN) ? 0 : last;
            EnsureItemVisible(g.selected);
            InvalidateRect(g_hwndList, nullptr, TRUE);
            SetFocus(g_hwndList);
            return 0;
        }
    }
    if (m == WM_KEYDOWN && wp == L'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(h, EM_SETSEL, 0, -1);   // 已经在这里：全选，方便重打
        return 0;
    }
    if (m == WM_CHAR && (wp == L'\r' || wp == L'\n' || wp == L'\t')) return 0;
    return DefSubclassProc(h, m, wp, lp);
}

// 把焦点送回搜索框并全选（Ctrl+F）
static void FocusSearchBox() {
    if (!g_hwndEdit) return;
    SetFocus(g_hwndEdit);
    SendMessageW(g_hwndEdit, EM_SETSEL, 0, -1);
}

// Tab 在「输入框 → 搜索 → 设置 → 结果列表」之间循环
static HWND TabStopAt(int i) {
    switch (i) {
        case 0: return g_hwndEdit;
        case 1: return g_hwndBtnSearch;
        case 2: return g_hwndBtnSet;
        default: return g_hwndList;
    }
}
static const int kTabStops = 4;

static void CycleFocus(bool back) {
    HWND focus = GetFocus();
    int cur = -1;
    for (int i = 0; i < kTabStops; ++i) {
        if (TabStopAt(i) == focus) { cur = i; break; }
    }
    if (cur < 0) cur = back ? 0 : kTabStops - 1;
    int next = (cur + (back ? kTabStops - 1 : 1)) % kTabStops;
    if (HWND t = TabStopAt(next)) SetFocus(t);
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
        // 上一次还在搜：挂起最新一次，等 OnResults 结束后补跑，不静默丢弃。
        g_pendingKeyword = kw;
        g_pendingFromCapture = fromCapture;
        g_hasPendingSearch = true;
        g.statusNote = L"上一次搜索还在进行，完成后自动重搜";
        SetStatus();
        return;
    }

    // 手动搜索是更强的用户意图：取消之前挂起的取词搜索，避免搜完又被覆盖。
    if (!fromCapture) {
        g_hasPendingSearch = false;
        g_pendingKeyword.clear();
    }

    SearchResultMsg* msg = new SearchResultMsg();
    msg->seq = ++g.seq;
    // 设置快照在主线程取：工作线程只读 msg->req
    msg->req.keyword      = kw;
    msg->req.rootDir      = g.settings.rootDir;
    msg->req.searchOffice = g.settings.searchOffice;
    msg->req.contextLevel = g.settings.contextLevel;
    msg->req.maxResults   = g.settings.maxResults;
    msg->outcome.keyword  = kw;

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
// 截一条鼠标附近的带子认出「一行」：先从上下各 80px 找，找不到扩到 150px、300px
// 输出 text / 每字像素 x（条带内坐标）/ mouseLocalX，像素缓冲用完清零

// 把条带里本程序自己的窗口区域涂成背景色（条带灰度众数）：
// 大条带会把旁边自己的状态栏框进来，离鼠标很近、总被当成"最近的一行"，
// 识别结果还干扰真实目标。涂掉后投影和模型都看不到它。
static void MaskOwnWindow(CapturedBgra& cap) {
    if (!g_hwndMain) return;
    RECT wr{};
    if (!GetWindowRect(g_hwndMain, &wr)) return;
    POINT cp{};
    if (!GetCursorPos(&cp)) return;

    // 条带在屏幕上的原点 = 光标 - 光标在条带内的位置
    int bx = cp.x - cap.mouseLocalX;
    int by = cp.y - cap.mouseLocalY;
    int x0 = wr.left - bx;   if (x0 < 0) x0 = 0;
    int y0 = wr.top - by;    if (y0 < 0) y0 = 0;
    int x1 = wr.right - bx;  if (x1 > cap.w) x1 = cap.w;
    int y1 = wr.bottom - by; if (y1 > cap.h) y1 = cap.h;
    if (x1 <= x0 || y1 <= y0) return;

    // 背景色 = 全条带灰度众数（深色桌面上涂深色、浅色窗口上涂浅色，不破坏极性判定）
    int hist[256] = {0};
    for (size_t i = 0; i < cap.px.size(); i += 16) {        // 4 像素一采样，够用
        unsigned b = cap.px[i], g = cap.px[i + 1], r = cap.px[i + 2];
        hist[(r * 299 + g * 587 + b * 114) / 1000]++;
    }
    int mode = 0, best = 0;
    for (int v = 0; v < 256; ++v) if (hist[v] > best) { best = hist[v]; mode = v; }

    // 只涂“这一块像素确实属于本程序窗口”的小格。
    // 如果本程序窗口在目标窗口后面，条带里看到的是目标文字，涂了就把目标擦掉了。
    const int kCell = S(24);
    DWORD myPid = GetCurrentProcessId();
    for (int yy = y0; yy < y1; yy += kCell) {
        int ey = yy + kCell; if (ey > y1) ey = y1;
        for (int xx = x0; xx < x1; xx += kCell) {
            int ex = xx + kCell; if (ex > x1) ex = x1;
            POINT q{ bx + (xx + ex) / 2, by + (yy + ey) / 2 };
            HWND top = WindowFromPoint(q);
            if (!top) continue;
            DWORD pid = 0;
            GetWindowThreadProcessId(top, &pid);
            if (pid != myPid) continue;
            for (int y = yy; y < ey; ++y) {
                unsigned char* row = &cap.px[(size_t)y * cap.w * 4];
                for (int x = xx; x < ex; ++x) {
                    row[x * 4 + 0] = (unsigned char)mode;
                    row[x * 4 + 1] = (unsigned char)mode;
                    row[x * 4 + 2] = (unsigned char)mode;
                    row[x * 4 + 3] = 255;
                }
            }
        }
    }
}

// ---------------- 取词候选 ----------------
//
// 四条通道最终都归一到同一个结构：整行文字 + 每字 x + 鼠标位置，
// 再统一做「鼠标右侧、最长纯文字段」的判定。模式只决定用哪条/哪几条。

struct PickCandidate {
    bool ok = false;
    std::wstring raw;       // 整行文字
    std::wstring block;     // 右侧最长纯文字段（最终关键词）
    std::wstring src;       // 来源说明
    std::wstring fail;      // 失败原因
    std::vector<double> charX;
    double mouseX = 0.0;
    bool haveX = false;
    bool exact = false;     // 逐字坐标是否精确（UIA 逐字 / 原生控件）
    bool estimated = false; // 是否按元素矩形估算
    int lineH = 20;
    int alignPct = -1;
    float confidence = -1.0f;
    RECT rect{};            // 屏幕行框（用于 UIA/OCR 是否同一行的校验）
    bool hasRect = false;
};

static bool MakeCandidate(const std::wstring& raw, const std::wstring& src,
                          const std::vector<double>& charX, double mouseX,
                          int lineH, bool exact, bool estimated, PickCandidate& out) {
    out = PickCandidate();
    out.raw = raw;
    out.src = src;
    out.mouseX = mouseX;
    out.charX = charX;
    out.haveX = (!raw.empty() && charX.size() == raw.size());
    out.exact = exact;
    out.estimated = estimated;
    out.lineH = (lineH > 0 && lineH < 400) ? lineH : 20;

    if (raw.empty()) { out.fail = L"没取到文字"; return false; }

    double firstLeft = -1.0;
    std::wstring block = out.haveX
        ? TakeRightOfCursor(raw, charX, mouseX, nullptr, nullptr, &firstLeft)
        : TakeRightOfCursor(raw, std::vector<double>(), 0.0);

    if (CountTextChars(block) < 2) {
        out.fail = L"右侧没有足够的文字";
        return false;
    }
    if (firstLeft >= 0.0 && firstLeft - mouseX > (double)MaxRightGap(out.lineH)) {
        out.fail = L"右侧文字太远";
        return false;
    }
    out.block = block;
    out.ok = true;
    return true;
}

static bool CaptureNativeCandidate(POINT p, PickCandidate& out) {
    std::wstring t, s;
    RECT rc{};
    std::vector<double> xs;
    if (!CaptureNativeEditLine(t, s, &rc, &xs)) {
        out = PickCandidate();
        out.fail = L"控件读不到文字";
        return false;
    }
    int h = rc.bottom - rc.top;
    if (h <= 0 || h > 60) h = 20;   // 多行控件的高度不是行高
    if (!MakeCandidate(t, s, xs, (double)p.x, h, true, false, out)) return false;
    out.rect = rc;
    out.hasRect = (rc.right > rc.left && rc.bottom > rc.top);
    return true;
}

static bool CaptureUiaCandidate(POINT p, PickCandidate& out) {
    std::wstring t, s;
    RECT rc{};
    std::vector<double> xs;
    if (!CaptureTextLayer(p, t, s, &rc, &xs)) {
        out = PickCandidate();
        out.fail = L"控件读不到文字";
        return false;
    }
    int h = rc.bottom - rc.top;
    if (h <= 0 || h > 200) h = 20;
    bool exact = (s == L"UIA 文字行");
    if (!MakeCandidate(t, s, xs, (double)p.x, h, exact, !exact, out)) return false;
    out.rect = rc;
    out.hasRect = (rc.right > rc.left && rc.bottom > rc.top);
    return true;
}

static bool CaptureOcrCandidate(POINT p, PickCandidate& out, std::wstring& fail) {
    out = PickCandidate();
    if (!g.caps.c2Model) { fail = L"图片识别不可用（缺模型）"; return false; }
    if (!OcrReady()) {
        std::wstring err;
        if (!OcrInit(err)) { g.caps.c2Model = false; fail = err; return false; }
    }

    CapturedBgra cap;
    // 一条带就够：只看鼠标所在行，不试多档半径、不换行。
    if (!CaptureBandRight(24, 800, 120, cap)) { fail = L"截屏失败"; return false; }
    MaskOwnWindow(cap);

    std::wstring t;
    std::vector<double> v;
    int lt = 0, lb = 0, reject = 0;
    float confidence = 0.0f;
    bool ok = OcrRecognizeBgra(cap.px.data(), cap.w, cap.h,
                               cap.mouseLocalX, cap.mouseLocalY,
                               t, &lt, &lb, &v, &reject, &confidence);
    int mx = cap.mouseLocalX;
    int lineH = (lb - lt + 1);
    WipeBytes(cap.px);

    if (!ok) {
        fail = (reject == 1) ? L"右侧没有文字，或离文字太远" : L"鼠标不在文字行上";
        return false;
    }
    if (!MakeCandidate(t, L"图片识别", v, (double)mx, lineH, false, false, out)) {
        fail = out.fail;
        return false;
    }
    out.confidence = confidence;
    out.rect.left = p.x;
    out.rect.right = p.x + 1;
    out.rect.top = p.y - cap.mouseLocalY + lt;
    out.rect.bottom = p.y - cap.mouseLocalY + lb;
    out.hasRect = (lb > lt);

    int cjk = CountCjkChars(out.block);
    if (confidence < 0.18f || (confidence < 0.32f && cjk < 4)) {
        fail = L"图片识别置信度太低（可能是图标或噪声）";
        out = PickCandidate();
        return false;
    }
    return true;
}

// OCR 只给了位置，UIA 给正确文字：把 OCR 那一行对齐到 UIA 全文，
// 用对齐后的原文替换 OCR 的错字，坐标仍保留 OCR 的像素位置。
static bool AlignOcrToUia(POINT p, const PickCandidate& ocr, PickCandidate& out) {
    if (!ocr.ok || !ocr.haveX) return false;
    std::wstring doc;
    if (!GetUiaDocumentText(p, ocr.raw, doc)) return false;
    if (doc.empty()) return false;

    AlignResult ar = AlignLineToDocument(doc, ocr.raw, 0.45);
    if (!ar.ok || ar.ratio < 0.45) return false;

    std::wstring wt;
    std::vector<double> wx;
    BuildAlignedLine(doc, ar, ocr.charX, wt, wx);
    if (wt.empty() || wx.size() != wt.size()) return false;

    PickCandidate c;
    if (!MakeCandidate(wt, L"智能 · 对齐", wx, ocr.mouseX, ocr.lineH, false, false, c)) return false;
    c.alignPct = (int)(ar.ratio * 100.0 + 0.5);
    c.confidence = ocr.confidence;
    c.rect = ocr.rect;
    c.hasRect = ocr.hasRect;
    out = c;
    return true;
}

static int CandidateScore(const PickCandidate& c) {
    if (!c.ok) return -1000;
    int n = CountTextChars(c.block);
    int score = n;
    if (c.exact) score += 4;
    if (c.estimated) score -= 2;
    if (c.src.find(L"UIA") != std::wstring::npos) score += 2;
    if (c.confidence >= 0.0f) score += (int)(c.confidence * 6.0f + 0.5f);
    return score;
}

// ---------------- 取词主体（在工作线程里跑）----------------
// 只算不画：不动任何 UI。UIA / OCR 卡住时只影响这一次取词，界面不冻。
static void CaptureFlow(const Settings& st, POINT p, CaptureResultMsg* out) {
    const int mode = st.captureMode;           // 0 智能 / 1 自动 / 2 总是 UIA / 3 总是图片

    PickCandidate native, uia, ocr, chosen;
    std::wstring fail;

    // 原生 EDIT：位置精确、不截屏；图片模式不用它。
    if (mode != 3) CaptureNativeCandidate(p, native);

    if (mode == 2 || mode == 1 || mode == 0) CaptureUiaCandidate(p, uia);
    if (mode == 3 || mode == 1 || mode == 0) CaptureOcrCandidate(p, ocr, fail);

    if (mode == 3) {
        chosen = ocr;
    } else if (mode == 2) {
        chosen = native.ok ? native : uia;
    } else if (mode == 1) {
        // 自动：几条都拿到，按“纯文字段长度 + 坐标是否精确”选一条整体更好的。
        if (native.ok) chosen = native;
        else {
            int sU = CandidateScore(uia);
            int sO = CandidateScore(ocr);
            if (uia.ok && ocr.ok) chosen = (sO > sU + 2) ? ocr : uia;
            else chosen = uia.ok ? uia : ocr;
        }
    } else {
        // 智能：UIA 有逐字行时以 UIA 的文字和坐标为准，OCR 只做“是否同一行”的校验；
        // UIA 只有整段/估算坐标时，用 OCR 定位 + UIA 原文校正。
        if (native.ok) {
            chosen = native;
        } else if (uia.ok && ocr.ok) {
            bool sameLine = true;
            if (uia.hasRect && ocr.hasRect) {
                int top = (uia.rect.top > ocr.rect.top) ? uia.rect.top : ocr.rect.top;
                int bottom = (uia.rect.bottom < ocr.rect.bottom) ? uia.rect.bottom : ocr.rect.bottom;
                int ov = bottom - top;
                int h1 = uia.rect.bottom - uia.rect.top;
                int h2 = ocr.rect.bottom - ocr.rect.top;
                int minH = (h1 < h2) ? h1 : h2;
                sameLine = (ov > 0 && minH > 0 && ov * 2 >= minH);
            }
            if (!sameLine) {
                // UIA 和 OCR 选中的不是同一视觉行：OCR 是像素行框，通常更可信；
                // 只有 OCR 置信度很低时才退回 UIA。
                chosen = (ocr.confidence >= 0.0f && ocr.confidence < 0.45f) ? uia : ocr;
            } else if (uia.exact) {
                double sim = TextSimilarityOrdered(uia.block, ocr.block);
                bool ocrWeak = (ocr.confidence >= 0.0f && ocr.confidence < 0.50f);
                chosen = (ocrWeak || sim >= 0.35 || CountTextChars(ocr.block) < 4) ? uia : ocr;
                if (chosen.ok) chosen.alignPct = (int)(sim * 100.0 + 0.5);
            } else {
                PickCandidate aligned;
                if (AlignOcrToUia(p, ocr, aligned)) chosen = aligned;
                else chosen = ocr;
            }
        } else if (uia.ok && uia.exact) {
            chosen = uia;
        } else if (ocr.ok) {
            chosen = ocr;
        } else if (uia.ok) {
            chosen = uia;
        }
    }

    if (!chosen.ok) {
        if (fail.empty()) fail = (!uia.fail.empty() ? uia.fail : ocr.fail);
        if (fail.empty()) fail = L"没取到文字";
        out->fail = fail;
        out->got = false;
        return;
    }

    out->got = true;
    out->raw = chosen.raw;
    out->src = chosen.src;
    out->charX = chosen.charX;
    out->mouseX = chosen.mouseX;
    out->haveX = chosen.haveX;
    out->estimated = chosen.estimated;
    out->alignPct = chosen.alignPct;
    out->confidence = chosen.confidence;
    out->fail.clear();
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
    if (msg->seq != g_captureSeq) { delete msg; return; }   // 超时后旧线程的结果
    InterlockedExchange(&g.capturing, 0);

    if (!msg->got) {
        SetLeftStatus(L"未取到文字 · " + (msg->fail.empty() ? std::wstring(L"无可用通道") : msg->fail));
        delete msg;
        return;
    }

    g_attemptsAtPoint = 0;   // 取到了，重点位的重试计数归零

    g.lastOcrRaw = msg->raw;
    size_t blockBegin = 0, blockEnd = 0;
    std::wstring clean = (msg->haveX && msg->charX.size() == msg->raw.size())
        ? TakeRightOfCursor(msg->raw, msg->charX, msg->mouseX, &blockBegin, &blockEnd)
        : TakeRightOfCursor(msg->raw, std::vector<double>(), 0.0, &blockBegin, &blockEnd);
    if (clean.empty() && !msg->raw.empty()) {
        clean = CleanOcrText(msg->raw);   // 兜底：至少别空手
        size_t at = msg->raw.find(clean);
        blockBegin = (at == std::wstring::npos) ? 0 : at;
        blockEnd = blockBegin + clean.size();
    }
    std::wstring note = L"已取词 · " + msg->src;
    if (msg->alignPct >= 0) note += L" · 与 UIA 重合 " + std::to_wstring(msg->alignPct) + L"%";
    if (msg->estimated) note += L" · 位置为估算";
    if (msg->confidence >= 0.0f && msg->confidence < 0.65f) {
        note += L" · 识别置信度 " +
                std::to_wstring((int)(msg->confidence * 100.0f + 0.5f)) + L"%";
    }
    SetLeftStatus(note);

    if (clean == g_lastCaptured && g.settings.rootDir == g_lastCapturedRoot) {
        delete msg;
        return;   // 同一行、同一搜索目录：内容没变就不重复搜索
    }
    g_lastCaptured = clean;
    g_lastCapturedRoot = g.settings.rootDir;

    // 输入框显示 OCR/UIA 取到的整行原文；只有实际参与搜索的那一段是深色，
    // 前后不参与的部分用原有置灰逻辑标出来。
    EditSetText(SingleLineForEdit(msg->raw));
    if (blockEnd > blockBegin && blockBegin < msg->raw.size()) {
        EditSetUsedRange(blockBegin, blockEnd - blockBegin);
    }
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

    // 按住鼠标（点击/拖选）时不取词：否则选到一半输入框就被改写。
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        SetLeftStatus(L"松开鼠标后再取词");
        return;
    }

    // 鼠标一动就重新计时
    if (p.x != g_lastPt.x || p.y != g_lastPt.y) {
        g_lastPt = p;
        g_stillSince = GetTickCount();
        g_fired = false;
        g_attemptsAtPoint = 0;
        g_userEditing = false;
        SetLeftStatus(L"");
        return;
    }

    // 用户正在改关键词：不要用下一次自动取词把输入框覆盖掉。
    if (g_userEditing) {
        SetLeftStatus(L"正在编辑关键词 · 移动鼠标后可继续取词");
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

    // 某个程序的 UIA 调用可能永久卡住。超过 6 秒就作废这一次，
    // 让后续取词还能继续；旧线程若之后返回，按代次丢弃。
    if (g.capturing && (DWORD)(now - g_captureStart) > 6000) {
        ++g_captureSeq;
        InterlockedExchange(&g.capturing, 0);
        g_fired = false;
        g_attemptsAtPoint = 0;
        SetLeftStatus(L"取词超时，已恢复 · 请重新移动鼠标");
        return;
    }

    if (g_fired && (DWORD)(now - g_lastAttempt) < 1200) return;

    // 同一个点连续 3 次都没取到文字就停下来：鼠标停在桌面/图片/按钮上时，
    // 否则会每 1.2 秒截两次屏 + 跑一遍 OCR，白烧 CPU 且状态栏一直闪
    if (g_attemptsAtPoint >= 3) {
        SetLeftStatus(L"此处没取到文字 · 移动鼠标后重试");
        return;
    }

    g_fired = true;
    g_lastAttempt = now;
    ++g_attemptsAtPoint;
    SetLeftStatus(L"取词中…");

    // 取词放进工作线程：UIA 卡住只影响这一次取词，界面不冻
    if (InterlockedExchange(&g.capturing, 1) != 0) return;

    CaptureResultMsg* msg = new CaptureResultMsg();
    msg->pt = p;
    msg->seq = ++g_captureSeq;
    g_captureStart = now;
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

// 「加载更多」：把暂存的命中按批追加到列表末尾（不重新搜索，纯内存搬运）
void ShowMoreResults() {
    if (g.outcome.pending.empty()) return;
    const size_t kBatch = 50;
    size_t take = g.outcome.pending.size();
    if (take > kBatch) take = kBatch;

    g.outcome.items.reserve(g.outcome.items.size() + take);
    for (size_t i = 0; i < take; ++i) {
        g.outcome.items.push_back(std::move(g.outcome.pending[i]));
    }
    g.outcome.pending.erase(g.outcome.pending.begin(),
                            g.outcome.pending.begin() + (std::ptrdiff_t)take);

    RecalcTotalHeight();
    InvalidateRect(g_hwndList, nullptr, TRUE);
    SetStatus();
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
            g_backtrackTries < kMaxBacktrack && !g_hasPendingSearch) {
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

    // 搜索期间取词拿到的新关键词：现在补跑最后一次。
    if (!needBacktrack && g_hasPendingSearch) {
        std::wstring kw = g_pendingKeyword;
        bool cap = g_pendingFromCapture;
        g_hasPendingSearch = false;
        g_pendingKeyword.clear();
        StartSearch(kw, cap);
    }
}

// ---------------- 布局 ----------------

static void Layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);

    int margin = S(16);
    int btnW = S(78);
    int btnH = S(32);
    int gap = S(8);
    int inputH = S(32);
    int inputRight = rc.right - margin - btnW * 2 - gap * 2;
    int inputW = inputRight - margin;
    if (inputW < S(80)) inputW = S(80);

    g_inputRect.left = margin;
    g_inputRect.top = (S(kTopBarH) - inputH) / 2;
    g_inputRect.right = margin + inputW;
    g_inputRect.bottom = g_inputRect.top + inputH;

    int editInset = S(11);
    int editH = S(22);
    MoveWindow(g_hwndEdit,
               g_inputRect.left + editInset,
               g_inputRect.top + (inputH - editH) / 2,
               inputW - editInset * 2, editH, TRUE);

    MoveWindow(g_hwndBtnSearch, inputRight + gap, (S(kTopBarH) - btnH) / 2, btnW, btnH, TRUE);
    MoveWindow(g_hwndBtnSet, inputRight + gap + btnW + gap, (S(kTopBarH) - btnH) / 2, btnW, btnH, TRUE);

    int listH = rc.bottom - S(kTopBarH) - S(kStatusH);
    if (listH < 0) listH = 0;
    MoveWindow(g_hwndList, 0, S(kTopBarH), rc.right, listH, TRUE);

    g_statusRect.left = 0;
    g_statusRect.right = rc.right;
    g_statusRect.bottom = rc.bottom;
    g_statusRect.top = rc.bottom - S(kStatusH);
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
    theme::FillRoundRect(dc, box, S(7), theme::kCardBg);
    theme::StrokeRoundRect(dc, box, S(7), theme::kBorder);

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
    int textY = sr.top + (S(kStatusH) - g_lineH) / 2;

    // 右段：命中统计
    GetTextExtentPoint32W(dc, g_statusRight.c_str(), (int)g_statusRight.size(), &sz);
    int rx = sr.right - S(16) - sz.cx;

    // 左段：取词状态 / 倒计时 / 未设置提示
    std::wstring leftText = g_statusLeft;
    if (leftText.empty() && g.settings.rootDir.empty()) {
        leftText = L"未设置搜索文件夹（拖一个文件夹进来）";
    }
    SetTextColor(dc, theme::kTextMain);
    RECT lc{ S(16), sr.top, rx - S(24), sr.bottom };
    ExtTextOutW(dc, S(16), textY, ETO_CLIPPED, &lc,
                leftText.c_str(), (int)leftText.size(), nullptr);
    GetTextExtentPoint32W(dc, leftText.c_str(), (int)leftText.size(), &sz);
    int leftW = sz.cx;

    // 中段：取词方式 · 鼠标位置
    int mx = S(16) + leftW + S(24);
    if (mx < rx - S(140)) {
        SetTextColor(dc, theme::kTextFaint);
        RECT mc{ mx, sr.top, rx - S(12), sr.bottom };
        ExtTextOutW(dc, mx, textY, ETO_CLIPPED, &mc,
                    g_statusMid.c_str(), (int)g_statusMid.size(), nullptr);
    }

    // 右段
    SetTextColor(dc, theme::kTextSub);
    RECT rcl{ rx, sr.top, sr.right - S(16), sr.bottom };
    ExtTextOutW(dc, rx, textY, ETO_CLIPPED, &rcl,
                g_statusRight.c_str(), (int)g_statusRight.size(), nullptr);

    SelectObject(dc, oldFont);
    EndPaint(hwnd, &ps);
}

// ---------------- 主窗口过程 ----------------

static void CreateControls(HWND hwnd) {
    const wchar_t* editCls = g_richOk ? MSFTEDIT_CLASS : L"EDIT";
    DWORD editStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NOHIDESEL;
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
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 100, 100, hwnd, (HMENU)IDC_LIST, nullptr, nullptr);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            theme::SetDpi(DpiForWindowCompat(hwnd));
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
            // 模型首次加载要 1~3 秒；后台预热，避免用户第一次停鼠标时以为没取到。
            if (g.caps.c2Model) {
                HANDLE pre = CreateThread(nullptr, 0, OcrPrewarmProc, nullptr, 0, nullptr);
                if (pre) CloseHandle(pre);
            }
            g.statusNote = g.caps.note;
            if (!g_bundleNote.empty()) {
                g.statusNote = g.statusNote.empty()
                    ? g_bundleNote
                    : (g_bundleNote + L" · " + g.statusNote);
            }
            SetStatus();
            return 0;
        }
        case WM_DPICHANGED: {
            int dpi = HIWORD(wp);
            if (dpi <= 0) dpi = DpiForWindowCompat(hwnd);
            theme::SetDpi(dpi);
            g_lineH = theme::LineHeight();
            RECT* nr = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, nr->left, nr->top,
                         nr->right - nr->left, nr->bottom - nr->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_hwndEdit) SendMessageW(g_hwndEdit, WM_SETFONT, (WPARAM)theme::FontUI(), TRUE);
            ListResetMetrics();
            Layout(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_ENTERSIZEMOVE:
            g_inSizeMove = true;
            return 0;
        case WM_EXITSIZEMOVE:
            g_inSizeMove = false;
            ListFlushPendingResize();
            return 0;
        case WM_SIZE:
            Layout(hwnd);
            if (!g_inSizeMove) ListFlushPendingResize();
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
            if (ListMenuOpen()) break;          // 右键菜单开着时，键盘交给菜单
            HWND focus = GetFocus();
            const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

            // Tab 循环（输入框 / 搜索 / 设置 / 结果列表）
            if (wp == VK_TAB) { CycleFocus(shift); return 0; }

            // 不依赖焦点的几个：Ctrl+F 回搜索框、F5 重搜
            if (ctrl && (wp == L'F' || wp == 'F')) { FocusSearchBox(); return 0; }
            // Ctrl+Enter：加载更多（不用先把焦点挪到列表底部的「加载更多」条）
            if (ctrl && wp == VK_RETURN) { ShowMoreResults(); return 0; }
            if (wp == VK_F5) {
                if (g.settings.rootDir.empty()) { g.statusNote = L"请先拖一个文件夹到窗口"; SetStatus(); }
                else StartSearch(EditText());
                return 0;
            }

            // 键盘导航只在焦点不在输入框时生效（在输入框里打字/光标不受影响）
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
            // 翻页 / 首尾
            if (wp == VK_PRIOR || wp == VK_NEXT) {
                RECT lrc{};
                GetClientRect(g_hwndList, &lrc);
                int page = lrc.bottom - g_lineH * 2;
                if (page < g_lineH * 2) page = g_lineH * 2;
                ScrollListByPixels(wp == VK_PRIOR ? -page : page);
                return 0;
            }
            if (wp == VK_HOME) { ScrollListToEdge(false); return 0; }
            if (wp == VK_END)  { ScrollListToEdge(true);  return 0; }

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
            if (ctrl && (wp == L'C' || wp == 'C')) {
                if (g.selected >= 0 && g.selected < (int)g.outcome.items.size()) {
                    if (shift) CopyItemPath(g.selected);        // Ctrl+Shift+C：只复制路径
                    else       CopyResultToClipboard(g.outcome.items[g.selected]);
                }
                return 0;
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDC_EDIT && HIWORD(wp) == EN_CHANGE) {
                // 用户自己改了内容：整段都算「实际在用」，取消置灰；
                // 同时先停掉自动取词，免得用户打字打到一半被下一次取词覆盖。
                if (!g_editSilent) {
                    g_userEditing = true;
                    if (g_dimFrom != (size_t)-1 || g_usedStart != 0) {
                        g_usedStart = 0;
                        g_dimFrom = (size_t)-1;
                        EditPaintColors(true);
                    }
                }
                return 0;
            }
            if (id == IDC_BTN_SEARCH) {
                // 输入框现在显示 OCR 原文，只有中间一段是实际搜索词；
                // 手动点搜索沿用这段，用户改过内容后范围会恢复成整段。
                std::wstring all = EditText();
                size_t n = all.size();
                size_t b = (g_usedStart <= n) ? g_usedStart : 0;
                size_t e = (g_dimFrom < n) ? g_dimFrom : n;
                if (e < b) e = n;
                std::wstring kw = all.substr(b, e - b);
                if (kw.empty()) kw = all;
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
        case WM_APP_DPI:
            // 设置窗口里换了 DPI：字体已由 theme 重建，这里补主窗口的行高/布局。
            g_lineH = theme::LineHeight();
            if (g_hwndEdit) SendMessageW(g_hwndEdit, WM_SETFONT, (WPARAM)theme::FontUI(), TRUE);
            ListResetMetrics();
            Layout(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
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

    // 旧版本释放过检测模型；新版只做投影定位，顺手清掉，省 4.7MB。
    DeleteFileW(JoinPath(SubDirPath(cfgpaths::kModelDir), L"det.onnx").c_str());

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

    // 每次启动都用固定默认尺寸并居中，不记忆上次的窗口大小（按 DPI 缩放）
    HDC sdc = GetDC(nullptr);
    if (sdc) {
        theme::SetDpi(GetDeviceCaps(sdc, LOGPIXELSY));
        ReleaseDC(nullptr, sdc);
    }
    const int kDefW = S(920), kDefH = S(660);
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
    if (g_hwndEdit) SetFocus(g_hwndEdit);   // 启动即可直接打字，↓ 进结果列表

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return (int)msg.wParam;
}
