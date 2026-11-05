// listview.cpp - 结果列表：卡片绘制、命中高亮、滚动、键盘/鼠标交互
#include "common.h"
#include "app_state.h"
#include "theme.h"
#include <shellapi.h>
#include <windowsx.h>

static const int kCardGap = 8;
static const int kCardPad = 12;
static const int kSideMargin = 14;
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

        // 禁则：行首不放收尾标点，把它们挤进上一行（最多多挤 4 个字，防止极端串）
        while (end < len && end - pos < fit + 4 && isClosingPunct(text[end])) ++end;

        out.push_back({ pos, end - pos });
        pos = end;
    }
}

// 一行文字的片段：区间内要么命中、要么普通
struct Seg { int start; int len; bool hit; };

// 按换行逐段绘制，返回绘制结束后的 y。用同一份折行结果算高度和绘制，保证两者一致。
static int DrawWrappedLine(HDC dc, int x, int y, const std::vector<Seg>& segs,
                           const std::wstring& line, int maxW) {
    HGDIOBJ oldFont = SelectObject(dc, theme::FontUI());
    std::vector<std::pair<int, int>> rows;
    WrapRanges(dc, line, maxW, rows);

    for (size_t r = 0; r < rows.size(); ++r) {
        int ws = rows[r].first;
        int we = ws + rows[r].second;
        int cx = x;
        for (size_t k = 0; k < segs.size(); ++k) {
            int a = segs[k].start > ws ? segs[k].start : ws;
            int b = segs[k].start + segs[k].len < we ? segs[k].start + segs[k].len : we;
            if (a >= b) continue;

            std::wstring piece = line.substr(a, b - a);
            SIZE sz{};
            GetTextExtentPoint32W(dc, piece.c_str(), (int)piece.size(), &sz);

            if (segs[k].hit) {
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
    if (maxW != g_wrapW || g_itemH.size() != g.outcome.items.size() || g_wrapSeq != g.seq) {
        g_wrapW = maxW;
        g_wrapSeq = g.seq;
        g_itemH.assign(g.outcome.items.size(), 0);
        if (!g.outcome.items.empty()) {
            HDC dc = GetDC(g_hwndList);
            for (size_t i = 0; i < g.outcome.items.size(); ++i)
                g_itemH[i] = MeasureItem(dc, g.outcome.items[i], maxW);
            ReleaseDC(g_hwndList, dc);
        }
    }

    int h = 0;
    for (size_t i = 0; i < g_itemH.size(); ++i) h += g_itemH[i];
    g.totalH = h;

    // 窗口拉大后，原来滚到底的位置可能越界
    RECT rc{};
    GetClientRect(g_hwndList, &rc);
    int maxScroll = g.totalH - rc.bottom;
    if (maxScroll < 0) maxScroll = 0;
    if (g.scrollY > maxScroll) g.scrollY = maxScroll;
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
        if (s > cur) segs.push_back({ cur, s - cur, false });
        if (e > s)   segs.push_back({ s, e - s, true });
        if (e > cur) cur = e;
    }
    if (cur < len) segs.push_back({ cur, len - cur, false });
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

    // 右侧细滚动条
    if (g.totalH > rc.bottom && g.totalH > 0) {
        int barH = rc.bottom * rc.bottom / g.totalH;
        if (barH < 36) barH = 36;
        int maxScroll = g.totalH - rc.bottom;
        int barY = maxScroll > 0 ? (rc.bottom - barH) * g.scrollY / maxScroll : 0;
        RECT bar{ rc.right - 6, barY + 4, rc.right - 3, barY + barH - 4 };
        theme::FillRoundRect(mem, bar, 2, RGB(206, 210, 217));
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

// Ctrl+C：复制命中行文字 + 文件位置
void CopyResultToClipboard(const ResultItem& it) {
    if (it.hitIndex < 0 || it.hitIndex >= (int)it.context.size()) return;
    std::wstring text = it.context[it.hitIndex];
    text += L"\r\n";
    text += it.path;
    text += L" · 第 " + std::to_wstring(it.lineNo) + L" 行";

    BOOL ok = OpenClipboard(g_hwndMain);
    if (ok) {
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
    SetLeftStatus(L"已复制命中行与路径");
}

void OpenItem(int index) {
    if (index < 0 || index >= (int)g.outcome.items.size()) return;
    ShellExecuteW(nullptr, L"open", g.outcome.items[index].path.c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
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
            int my = GET_Y_LPARAM(lp) + g.scrollY - 10;
            int y = 0;
            g.selected = -1;
            for (size_t i = 0; i < g.outcome.items.size(); ++i) {
                int ih = ItemH(i);
                if (my >= y && my < y + ih - kCardGap) { g.selected = (int)i; break; }
                y += ih;
            }
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            int my = GET_Y_LPARAM(lp) + g.scrollY - 10;
            int y = 0;
            for (size_t i = 0; i < g.outcome.items.size(); ++i) {
                int ih = ItemH(i);
                if (my >= y && my < y + ih - kCardGap) { OpenItem((int)i); break; }
                y += ih;
            }
            return 0;
        }
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        case WM_KEYDOWN:
            // 点过列表后焦点在这里，按键转发给主窗口的导航处理
            if (g_hwndMain) PostMessageW(g_hwndMain, WM_KEYDOWN, wp, lp);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
