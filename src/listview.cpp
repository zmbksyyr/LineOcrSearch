// listview.cpp - 结果列表：卡片绘制、命中高亮、滚动、键盘/鼠标交互
#include "common.h"
#include "app_state.h"
#include "theme.h"
#include <shellapi.h>
#include <windowsx.h>

static const int kCardGap = 8;
static const int kCardPad = 12;
static const int kSideMargin = 14;
static const int kMoreH = 52;     // 底部「加载更多」条的高度
// ---------------- 结果列表 ----------------

// 卡片里文字可用的宽度（跟随窗口宽度变化）
static int ContentWidth() {
    RECT rc{};
    GetClientRect(g_hwndList, &rc);
    int w = rc.right - 2 * kSideMargin - 2 * kCardPad;
    return w < 40 ? 40 : w;
}

// 把一行文字按可用宽度折成多行，返回每行在 text 中的 [起点, 长度)。
// 中文按字断行；断点落在拉丁字母/数字中间时回退到最近的空格，避免把单词劈开。
static bool isClosingPunct(wchar_t c) {
    return c && wcschr(L"。，、；：！？）】》」』”’.,;:!?", c) != nullptr;
}

static void WrapRanges(HDC dc, const std::wstring& text, int maxW,
                       std::vector<std::pair<int, int>>& out) {
    out.clear();
    const int len = (int)text.size();
    if (len <= 0) { out.push_back({ 0, 0 }); return; }   // 空行也占一行
    if (maxW < 32) maxW = 32;

    std::vector<int> dx((size_t)len);
    int pos = 0;
    while (pos < len) {
        int fit = 0;
        SIZE sz{};
        if (!GetTextExtentExPointW(dc, text.c_str() + pos, len - pos, maxW, &fit,
                                   dx.data(), &sz) || fit <= 0) {
            fit = 1;   // 宽度极窄时至少放一个字，避免原地打转
        }
        int end = pos + fit;
        if (end >= len) { out.push_back({ pos, len - pos }); break; }

        auto isLatin = [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                   (c >= L'0' && c <= L'9');
        };
        if (isLatin(text[end]) && isLatin(text[end - 1])) {
            int back = end - 1;
            while (back > pos + 1 && text[back] != L' ' && text[back] != L'\t') --back;
            if (back > pos + 1) end = back + 1;   // 空格留在行尾
        }

        // 禁则：行首不放收尾标点，把它们挤进上一行（最多多挤 4 个字，防止极端串）。
        // 挤完必须复核宽度仍在 maxW 内，否则放弃——否则行会画出卡片右边界。
        int ext = end;
        while (ext < len && ext - pos < fit + 4 && isClosingPunct(text[ext])) ++ext;
        if (ext > end) {
            int fit2 = 0;
            SIZE sz2{};
            if (GetTextExtentExPointW(dc, text.c_str() + pos, ext - pos, maxW, &fit2,
                                      nullptr, &sz2) && fit2 >= ext - pos) {
                end = ext;
            }
        }

        out.push_back({ pos, end - pos });
        pos = end;
    }
}

// 一行文字的片段：区间内要么命中、要么普通
//
// 注意：这里故意凑成 16 字节（4 个 int）而不是 12 字节（int,int,bool）。
// zig 自带的 clang 在 -O2 下把 vector::size() 里的「(end-begin)/12」
// 编译成了错误的魔数乘法（返回 0xAAAAAAAAAAAAAAAE 这类毒值），
// 12 字节结构体的 size() 会返回垃圾，进而把未初始化的槽位当片段画出来，
// 造成结果文字横向溢出乃至越界崩溃；除以 16 则退化为移位，不会踩这个坑。
// 除 Seg 外，代码里其余 vector 元素都是 8/16/32 字节，天然安全。
struct Seg { int start; int len; int hit; int pad; };

// 每张卡片最多这么多绘制片段（正常一行撑死几十段，超出直接截断，
// 宁可少画几个字的红底，也不能让越界访问把进程带走）。
static const int kMaxDrawSegs = 128;

// 按换行逐段绘制，返回绘制结束后的 y。用同一份折行结果算高度和绘制，保证两者一致。
//
// 注意：片段先拷贝到本地定长数组，绘制循环里只用 int 计数 + 本地数组，
// 不再每轮用 vector 的 begin/end 反推循环上界。
// 以前直接 `for (k < segs.size())` 时，编译器把上界优化成了
// 「(end-begin)/sizeof(Seg)」的乘法取整；一旦 end-begin 不是 12 的整数倍，
// 这个取整会算出一个天文数字，循环随即越界读堆内存，窗口一画结果就崩。
static int DrawWrappedLine(HDC dc, int x, int y, const std::vector<Seg>& segs,
                           const std::wstring& line, int maxW) {
    HGDIOBJ oldFont = SelectObject(dc, theme::FontUI());
    std::vector<std::pair<int, int>> rows;
    WrapRanges(dc, line, maxW, rows);

    const int lineLen = (int)line.size();

    Seg segBuf[kMaxDrawSegs];
    int segCount = 0;
    {
        // 全部按「元素个数」算，并且被容量与本地数组上限双重夹住：
        // 即使传进来的 vector 状态不对，索引也走不出它的缓冲区。
        size_t n = segs.size();
        size_t cap = segs.capacity();
        if (n > cap) n = cap;
        if (n > (size_t)kMaxDrawSegs) n = (size_t)kMaxDrawSegs;
        segCount = (int)n;
        for (int i = 0; i < segCount; ++i) segBuf[i] = segs[(size_t)i];
    }

    for (size_t r = 0; r < rows.size(); ++r) {
        int ws = rows[r].first;
        int we = ws + rows[r].second;
        if (ws < 0) ws = 0;
        if (we > lineLen) we = lineLen;

        if (ws < we) {
            int cx = x;
            for (int k = 0; k < segCount; ++k) {
                const Seg& sg = segBuf[k];
                int a = sg.start > ws ? sg.start : ws;
                int b = (sg.start + sg.len) < we ? (sg.start + sg.len) : we;
                if (a < 0) a = 0;
                if (b > lineLen) b = lineLen;
                if (a >= b) continue;

                std::wstring piece = line.substr((size_t)a, (size_t)(b - a));
                SIZE sz{};
                GetTextExtentPoint32W(dc, piece.c_str(), (int)piece.size(), &sz);

                if (sg.hit) {
                    RECT bg{ cx - 2, y, cx + sz.cx + 2, y + g_lineH - 2 };
                    HBRUSH br = CreateSolidBrush(theme::kHitBg);
                    FillRect(dc, &bg, br);
                    DeleteObject(br);
                    SetTextColor(dc, theme::kHitText);
                } else {
                    SetTextColor(dc, theme::kTextMain);
                }
                TextOutW(dc, cx, y, piece.c_str(), (int)piece.size());
                cx += sz.cx;
            }
        }
        y += g_lineH;
    }

    SelectObject(dc, oldFont);
    return y;
}

// 缓存的折行前提：窗口宽度 + 结果条数。任一变化就重算每张卡片的高度。
static int              g_wrapW = -1;
static unsigned         g_wrapSeq = 0xFFFFFFFFu;   // 结果换代也要重算（条数可能刚好相同）
static std::vector<int> g_itemH;

static int MeasureItem(HDC dc, const ResultItem& it, int maxW) {
    HGDIOBJ oldFont = SelectObject(dc, theme::FontUI());
    std::vector<std::pair<int, int>> rows;
    int lineCount = 0;
    for (size_t j = 0; j < it.context.size(); ++j) {
        std::wstring line = L"  " + it.context[j];
        WrapRanges(dc, line, maxW, rows);
        lineCount += (int)rows.size();
    }
    SelectObject(dc, oldFont);
    if (lineCount < 1) lineCount = 1;
    return kCardPad + g_lineH + 6 + lineCount * g_lineH + kCardPad + kCardGap;
}

static int ItemH(size_t i) {
    if (i >= g_itemH.size()) return kCardPad * 2 + g_lineH * 2 + kCardGap;
    return g_itemH[i];
}

void RecalcTotalHeight() {
    int maxW = ContentWidth();
    const bool widthChanged = (maxW != g_wrapW);
    const bool newSearch    = (g_wrapSeq != g.seq);

    if (widthChanged || newSearch || g_itemH.size() > g.outcome.items.size()) {
        // 宽度变了 / 换了新结果：全部重测
        g_itemH.assign(g.outcome.items.size(), 0);
    } else if (g_itemH.size() < g.outcome.items.size()) {
        // 「加载更多」只是尾巴变长：已测过的保留，只补后面的
        g_itemH.resize(g.outcome.items.size(), 0);
    }
    g_wrapW = maxW;
    g_wrapSeq = g.seq;

    bool needMeasure = false;
    for (size_t i = 0; i < g_itemH.size(); ++i) {
        if (g_itemH[i] <= 0) { needMeasure = true; break; }
    }
    if (needMeasure && !g.outcome.items.empty()) {
        HDC dc = GetDC(g_hwndList);
        for (size_t i = 0; i < g.outcome.items.size(); ++i) {
            if (g_itemH[i] <= 0) g_itemH[i] = MeasureItem(dc, g.outcome.items[i], maxW);
        }
        ReleaseDC(g_hwndList, dc);
    }

    int h = 0;
    for (size_t i = 0; i < g_itemH.size(); ++i) h += g_itemH[i];
    if (!g.outcome.pending.empty()) h += kMoreH;   // 底部「加载更多」条
    g.totalH = h;

    // 窗口拉大 / 追加结果后，原来滚到底的位置可能越界
    RECT rc{};
    GetClientRect(g_hwndList, &rc);
    int maxScroll = g.totalH - rc.bottom;
    if (maxScroll < 0) maxScroll = 0;
    if (g.scrollY > maxScroll) g.scrollY = maxScroll;
}

// 「加载更多」条在客户区里的矩形（没有待加载内容时返回 false）
static bool MoreStripRect(RECT& out) {
    if (g.outcome.pending.empty()) return false;
    RECT rc{};
    GetClientRect(g_hwndList, &rc);
    int top = 0;
    for (size_t i = 0; i < g.outcome.items.size(); ++i) top += ItemH(i);
    int sy = 10 + top - g.scrollY;
    out.left   = kSideMargin;
    out.right  = rc.right - kSideMargin;
    out.top    = sy;
    out.bottom = sy + kMoreH - kCardGap;
    return true;
}

// 命中行：整行普通色，命中区间红字 + 淡红底（多关键词时可能有多个区间）
static void BuildSegs(const ResultItem& it, bool hitLine, int len, std::vector<Seg>& segs) {
    segs.clear();
    if (!hitLine) {
        segs.push_back({ 0, len, false });
        return;
    }
    std::vector<HitSpan> ranges = it.hitRanges;
    if (ranges.empty()) ranges.push_back({ it.hitStart, it.hitLen });

    int cur = 0;
    for (size_t i = 0; i < ranges.size(); ++i) {
        int s = ranges[i].start + 2;   // 前缀两个空格，区间按原文行给的，这里平移
        int e = s + ranges[i].len;
        if (s < 0) s = 0;
        if (e > len) e = len;
        if (s > cur) segs.push_back({ cur, s - cur, 0, 0 });
        if (e > s)   segs.push_back({ s, e - s, 1, 0 });
        if (e > cur) cur = e;
    }
    if (cur < len) segs.push_back({ cur, len - cur, 0, 0 });
}

// ---------------- 自绘滚动条（可拖动）----------------
static const int kScrollHitW = 16;   // 鼠标命中区宽度（比视觉更宽，好抓）
static const int kThumbW     = 8;    // 滑块视觉宽度
static const int kThumbMinH  = 44;   // 滑块最小高度（保证长文档下也好抓）
static const int kTrackInset = 2;    // 轨道上下留白

static bool g_dragScroll = false;    // 正在拖动滑块
static int  g_dragGrabY  = 0;        // 按点相对滑块顶部的偏移

static int ThumbHeight(const RECT& rc) {
    int barH = rc.bottom * rc.bottom / g.totalH;
    if (barH < kThumbMinH) barH = kThumbMinH;
    int maxBar = rc.bottom - kTrackInset * 2;
    if (barH > maxBar) barH = maxBar;
    return barH;
}

// 由当前 scrollY 反推滑块矩形；不可滚动时返回 false
static bool ScrollThumbRect(const RECT& rc, RECT& thumb) {
    if (g.totalH <= 0 || g.totalH <= rc.bottom) return false;
    int barH   = ThumbHeight(rc);
    int trackH = rc.bottom - kTrackInset * 2;
    int maxScroll = g.totalH - rc.bottom;
    int barY = (trackH > barH && maxScroll > 0)
                   ? (trackH - barH) * g.scrollY / maxScroll : 0;
    thumb = { rc.right - kThumbW - 3, kTrackInset + barY,
              rc.right - 3, kTrackInset + barY + barH };
    return true;
}

// 拖动中：把滑块顶部挪到指定 y，同步更新 scrollY
static void ScrollToThumbY(HWND hwnd, int thumbTop) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (g.totalH <= 0 || g.totalH <= rc.bottom) return;
    int barH   = ThumbHeight(rc);
    int trackH = rc.bottom - kTrackInset * 2;
    int maxScroll = g.totalH - rc.bottom;
    int lo = kTrackInset, hi = kTrackInset + trackH - barH;
    int t = thumbTop - g_dragGrabY;
    if (t < lo) t = lo;
    if (t > hi) t = hi;
    g.scrollY = (hi > lo) ? (t - lo) * maxScroll / (hi - lo) : 0;
    if (g.scrollY < 0) g.scrollY = 0;
    if (g.scrollY > maxScroll) g.scrollY = maxScroll;
    InvalidateRect(hwnd, nullptr, TRUE);
}

static void PaintList(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    SetBkMode(mem, TRANSPARENT);

    HBRUSH pageBg = CreateSolidBrush(theme::kPageBg);
    FillRect(mem, &rc, pageBg);
    DeleteObject(pageBg);

    int cardLeft = kSideMargin;
    int cardRight = rc.right - kSideMargin;
    int maxW = cardRight - cardLeft - kCardPad * 2;
    if (maxW < 40) maxW = 40;
    int y = 10 - g.scrollY;

    for (size_t i = 0; i < g.outcome.items.size(); ++i) {
        const ResultItem& it = g.outcome.items[i];
        int ih = ItemH(i);
        if (y + ih < 0) { y += ih; continue; }
        if (y > rc.bottom) break;

        RECT card{ cardLeft, y, cardRight, y + ih - kCardGap };
        bool sel = ((int)i == g.selected);

        theme::FillRoundRect(mem, card, 8, sel ? theme::kSelBg : theme::kCardBg);
        theme::StrokeRoundRect(mem, card, 8, sel ? theme::kBorderStrong : theme::kBorder);

        int tx = card.left + kCardPad;
        int maxRight = card.right - kCardPad;
        int ty = card.top + kCardPad;

        // 标题：文件名超长时省略，行号只在右侧还有空间时显示
        HGDIOBJ oldFont = SelectObject(mem, theme::FontUI());
        std::wstring ln = L"   第 " + std::to_wstring(it.lineNo) + L" 行";
        SIZE lnSz{};
        GetTextExtentPoint32W(mem, ln.c_str(), (int)ln.size(), &lnSz);

        int nameLimit = maxRight - lnSz.cx - 8;
        if (nameLimit > tx) {
            SetTextColor(mem, theme::kTextMain);
            RECT nameRect{ tx, ty, nameLimit, ty + g_lineH + 4 };
            DrawTextW(mem, it.fileName.c_str(), (int)it.fileName.size(), &nameRect,
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            SIZE nameSz{};
            GetTextExtentPoint32W(mem, it.fileName.c_str(), (int)it.fileName.size(), &nameSz);
            int nameW = nameSz.cx;
            if (nameW > nameLimit - tx) nameW = nameLimit - tx;

            if (tx + nameW + lnSz.cx <= maxRight) {
                SetTextColor(mem, theme::kTextFaint);
                TextOutW(mem, tx + nameW, ty, ln.c_str(), (int)ln.size());
            }
        } else {
            SetTextColor(mem, theme::kTextMain);
            RECT nameRect{ tx, ty, maxRight, ty + g_lineH + 4 };
            DrawTextW(mem, it.fileName.c_str(), (int)it.fileName.size(), &nameRect,
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        SelectObject(mem, oldFont);

        ty += g_lineH + 6;

        for (size_t j = 0; j < it.context.size(); ++j) {
            std::wstring line = L"  " + it.context[j];
            std::vector<Seg> segs;
            BuildSegs(it, (int)j == it.hitIndex, (int)line.size(), segs);
            ty = DrawWrappedLine(mem, tx, ty, segs, line, maxRight - tx);
        }

        y += ih;
    }

    // 底部「加载更多」：还有命中没显示时给一个明显的入口
    if (!g.outcome.pending.empty()) {
        int top = 0;
        for (size_t i = 0; i < g.outcome.items.size(); ++i) top += ItemH(i);
        int sy = 10 + top - g.scrollY;
        RECT strip{ cardLeft, sy, cardRight, sy + kMoreH - kCardGap };
        if (strip.bottom > 0 && strip.top < rc.bottom) {
            theme::FillRoundRect(mem, strip, 8, theme::kCardBg);
            theme::StrokeRoundRect(mem, strip, 8, theme::kBorder);
            HGDIOBJ of = SelectObject(mem, theme::FontUI());
            std::wstring tip = L"还有 " + std::to_wstring(g.outcome.pending.size()) +
                               L" 条命中 · 点击加载更多（Ctrl+Enter）";
            SIZE sz{};
            GetTextExtentPoint32W(mem, tip.c_str(), (int)tip.size(), &sz);
            int cx = strip.left + (strip.right - strip.left - sz.cx) / 2;
            if (cx < strip.left + kCardPad) cx = strip.left + kCardPad;
            SetTextColor(mem, theme::kTextSub);
            TextOutW(mem, cx, strip.top + (strip.bottom - strip.top - g_lineH) / 2,
                     tip.c_str(), (int)tip.size());
            SelectObject(mem, of);
        }
    }

    if (g.outcome.items.empty()) {
        std::wstring tip = g.outcome.keyword.empty()
            ? L"把鼠标停在任意文字上，或在上方输入关键词"
            : L"没有找到包含该关键词的内容";
        HGDIOBJ oldFont = SelectObject(mem, theme::FontUI());
        SIZE sz{};
        GetTextExtentPoint32W(mem, tip.c_str(), (int)tip.size(), &sz);
        int cx = (rc.right - sz.cx) / 2;
        if (cx < 0) cx = 0;
        SetTextColor(mem, theme::kTextFaint);
        TextOutW(mem, cx, rc.bottom / 2 - g_lineH, tip.c_str(), (int)tip.size());
        SelectObject(mem, oldFont);
    }

    // 右侧滚动条：命中区 16px，视觉 8px，拖动时加深
    if (g.totalH > rc.bottom && g.totalH > 0) {
        RECT thumb{};
        if (ScrollThumbRect(rc, thumb)) {
            COLORREF c = g_dragScroll ? RGB(148, 154, 165) : RGB(206, 210, 217);
            theme::FillRoundRect(mem, thumb, 3, c);
        }
    }

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// 让第 index 张卡片进入可视范围（键盘 ↑↓ 时跟随）
void EnsureItemVisible(int index) {
    if (index < 0 || index >= (int)g.outcome.items.size()) return;
    RECT rc{};
    GetClientRect(g_hwndList, &rc);

    int y = 0;
    for (int i = 0; i < index; ++i) y += ItemH((size_t)i);
    int ih = ItemH((size_t)index);

    int maxScroll = g.totalH - rc.bottom;
    if (maxScroll < 0) maxScroll = 0;

    if (y - 10 < g.scrollY) {
        g.scrollY = y - 10;                       // 顶在上面，留出 10px 边距
    } else if (y + ih - 10 > g.scrollY + rc.bottom) {
        g.scrollY = y + ih - 10 - rc.bottom;      // 底部露出整卡
    }
    if (g.scrollY < 0) g.scrollY = 0;
    if (g.scrollY > maxScroll) g.scrollY = maxScroll;
}

// 统一剪贴板写入（复制命中行 / 路径都用它）
static void CopyPlainText(const std::wstring& text, const wchar_t* note) {
    if (text.empty()) return;
    if (OpenClipboard(g_hwndMain)) {
        EmptyClipboard();
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem) {
            void* p = GlobalLock(mem);
            if (p) {
                memcpy(p, text.c_str(), bytes);
                GlobalUnlock(mem);
                if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
            } else {
                GlobalFree(mem);
            }
        }
        CloseClipboard();
    }
    if (note) SetLeftStatus(note);
}

// Ctrl+C：复制命中行文字 + 文件位置
void CopyResultToClipboard(const ResultItem& it) {
    if (it.hitIndex >= 0 && it.hitIndex < (int)it.context.size()) {
        std::wstring text = it.context[it.hitIndex];
        text += L"\r\n";
        text += it.path;
        text += L" · 第 " + std::to_wstring(it.lineNo) + L" 行";
        CopyPlainText(text, L"已复制命中行与路径");
    }
}

// 客户区 y 命中的卡片下标；-1 表示空白处
static int HitTestItem(int clientY) {
    int y = clientY + g.scrollY - 10;
    if (y < 0) return -1;
    int top = 0;
    for (size_t i = 0; i < g.outcome.items.size(); ++i) {
        int ih = ItemH(i);
        if (y >= top && y < top + ih - kCardGap) return (int)i;
        top += ih;
    }
    return -1;
}

void OpenItem(int index) {
    if (index < 0 || index >= (int)g.outcome.items.size()) return;
    ShellExecuteW(nullptr, L"open", g.outcome.items[index].path.c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenItemFolder(int index) {
    if (index < 0 || index >= (int)g.outcome.items.size()) return;
    std::wstring args = L"/select,\"";
    args += g.outcome.items[index].path;
    args += L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    SetLeftStatus(L"已在资源管理器中定位");
}

void CopyItemPath(int index) {
    if (index < 0 || index >= (int)g.outcome.items.size()) return;
    CopyPlainText(g.outcome.items[index].path, L"已复制文件路径");
}

void ScrollListByPixels(int dy) {
    if (!g_hwndList) return;
    RECT rc{};
    GetClientRect(g_hwndList, &rc);
    int maxScroll = g.totalH - rc.bottom;
    if (maxScroll < 0) maxScroll = 0;
    g.scrollY += dy;
    if (g.scrollY < 0) g.scrollY = 0;
    if (g.scrollY > maxScroll) g.scrollY = maxScroll;
    InvalidateRect(g_hwndList, nullptr, TRUE);
}

void ScrollListToEdge(bool bottom) {
    if (!g_hwndList) return;
    RECT rc{};
    GetClientRect(g_hwndList, &rc);
    int maxScroll = g.totalH - rc.bottom;
    if (maxScroll < 0) maxScroll = 0;
    g.scrollY = bottom ? maxScroll : 0;
    InvalidateRect(g_hwndList, nullptr, TRUE);
}

// ---------------- 右键菜单 ----------------
enum { kMenuOpen = 1, kMenuFolder, kMenuCopyLine, kMenuCopyPath, kMenuCopyRef };

static bool g_menuOpen = false;   // 菜单弹出期间，列表不吃键盘（交给菜单自己处理）

bool ListMenuOpen() { return g_menuOpen; }

static void ShowItemMenu(HWND hwnd, int idx, POINT screen) {
    if (idx < 0 || idx >= (int)g.outcome.items.size()) return;
    const ResultItem& it = g.outcome.items[idx];

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"打开文件");
    AppendMenuW(menu, MF_STRING, kMenuFolder, L"打开所在文件夹");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuCopyLine, L"复制命中行");
    AppendMenuW(menu, MF_STRING, kMenuCopyPath, L"复制文件路径");
    AppendMenuW(menu, MF_STRING, kMenuCopyRef,
                (L"复制「路径 · 第 " + std::to_wstring(it.lineNo) + L" 行」").c_str());
    SetMenuDefaultItem(menu, kMenuOpen, FALSE);

    // 菜单要能被“点外面就关掉”，需要先把自己提到前台；
    // 所有者用顶层窗口，键盘（↑↓/Enter/Esc）才会被菜单接住
    HWND owner = g_hwndMain ? g_hwndMain : hwnd;
    SetForegroundWindow(owner);
    g_menuOpen = true;
    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  screen.x, screen.y, 0, owner, nullptr);
    g_menuOpen = false;
    DestroyMenu(menu);
    PostMessageW(owner, WM_NULL, 0, 0);

    switch (cmd) {
        case kMenuOpen:      OpenItem(idx); break;
        case kMenuFolder:    OpenItemFolder(idx); break;
        case kMenuCopyLine:
            if (it.hitIndex >= 0 && it.hitIndex < (int)it.context.size())
                CopyPlainText(it.context[it.hitIndex], L"已复制命中行");
            break;
        case kMenuCopyPath:  CopyItemPath(idx); break;
        case kMenuCopyRef:
            CopyPlainText(it.path + L" · 第 " + std::to_wstring(it.lineNo) + L" 行",
                          L"已复制路径与行号");
            break;
        default: break;
    }
}


LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            PaintList(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            RecalcTotalHeight();
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int maxScroll = g.totalH - rc.bottom;
            if (maxScroll < 0) maxScroll = 0;
            g.scrollY -= delta / WHEEL_DELTA * g_lineH * 3;
            if (g.scrollY < 0) g.scrollY = 0;
            if (g.scrollY > maxScroll) g.scrollY = maxScroll;
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            RECT rc{};
            GetClientRect(hwnd, &rc);

            // 滚动条区：按下滑块开始拖；按在轨道上则把滑块中心跟过去再拖
            if (mx >= rc.right - kScrollHitW) {
                RECT thumb{};
                if (ScrollThumbRect(rc, thumb)) {
                    SetCapture(hwnd);
                    g_dragScroll = true;
                    if (my >= thumb.top && my <= thumb.bottom) {
                        g_dragGrabY = my - thumb.top;
                    } else {
                        g_dragGrabY = (thumb.bottom - thumb.top) / 2;
                        ScrollToThumbY(hwnd, my);
                    }
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
                return 0;
            }

            int myInDoc = my + g.scrollY - 10;
            int y = 0;

            // 底部「加载更多」条
            RECT strip{};
            if (MoreStripRect(strip) && my >= strip.top && my <= strip.bottom) {
                SetFocus(hwnd);
                ShowMoreResults();
                return 0;
            }

            g.selected = -1;
            for (size_t i = 0; i < g.outcome.items.size(); ++i) {
                int ih = ItemH(i);
                if (myInDoc >= y && myInDoc < y + ih - kCardGap) { g.selected = (int)i; break; }
                y += ih;
            }
            SetFocus(hwnd);          // 点卡片即接管焦点，↑↓/Enter/Ctrl+C 才能用
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_RBUTTONDOWN: {
            // 右键先选中（并接管焦点），真正的菜单在 WM_CONTEXTMENU 里弹
            SetFocus(hwnd);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            if (mx < rc.right - kScrollHitW) {
                int idx = HitTestItem(my);
                if (idx >= 0) {
                    g.selected = idx;
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
            }
            return 0;
        }
        case WM_CONTEXTMENU: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int idx = -1;
            if (pt.x == -1 && pt.y == -1) {          // 键盘菜单键：用当前选中项
                idx = g.selected;
                if (idx < 0) return 0;
                POINT origin{ rc.left + 40, rc.top + 40 };
                ClientToScreen(hwnd, &origin);
                pt = origin;
            } else {
                POINT cp = pt;
                ScreenToClient(hwnd, &cp);
                if (cp.x >= rc.right - kScrollHitW) return 0;   // 滚动条上不弹卡片菜单
                idx = HitTestItem(cp.y);
            }
            if (idx < 0) return 0;
            g.selected = idx;
            InvalidateRect(hwnd, nullptr, TRUE);
            ShowItemMenu(hwnd, idx, pt);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (g_dragScroll) { ScrollToThumbY(hwnd, GET_Y_LPARAM(lp)); return 0; }
            break;
        case WM_LBUTTONUP:
            if (g_dragScroll) {
                g_dragScroll = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            // SetCapture 被系统抢走（如弹窗）时也要结束拖动
            if (g_dragScroll) { g_dragScroll = false; InvalidateRect(hwnd, nullptr, TRUE); }
            return 0;
        case WM_LBUTTONDBLCLK: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            if (GET_X_LPARAM(lp) >= rc.right - kScrollHitW) return 0;   // 滚动条区域不当作卡片
            int my = GET_Y_LPARAM(lp) + g.scrollY - 10;
            int y = 0;
            for (size_t i = 0; i < g.outcome.items.size(); ++i) {
                int ih = ItemH(i);
                if (my >= y && my < y + ih - kCardGap) { OpenItem((int)i); break; }
                y += ih;
            }
            return 0;
        }
        case WM_SETCURSOR: {
            // 「加载更多」条上用小手，提示可点
            POINT p{};
            RECT sr{};
            if (GetCursorPos(&p) && ScreenToClient(hwnd, &p) &&
                MoreStripRect(sr) && p.y >= sr.top && p.y <= sr.bottom) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
            } else {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            }
            return TRUE;
        }
        case WM_KEYDOWN:
            // 点过列表后焦点在这里，按键转发给主窗口的导航处理
            if (g_menuOpen) break;              // 菜单弹出期间不抢键盘
            if (g_hwndMain) PostMessageW(g_hwndMain, WM_KEYDOWN, wp, lp);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
