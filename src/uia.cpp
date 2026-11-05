// uia.cpp - C1 文字层取词：原生控件 -> UIA -> MSAA，全程不截屏
#include "common.h"
#include <uiautomation.h>
#include <oleacc.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

// ---------------- 小工具 ----------------

static void FreeBstr(BSTR& b) {
    if (b) { SysFreeString(b); b = nullptr; }
}

static std::wstring TakeBstr(BSTR b) {
    if (!b) return std::wstring();
    std::wstring s(b, SysStringLen(b));
    SysFreeString(b);
    return s;
}

static std::wstring TrimW3(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) b++;
    while (e > b && iswspace(s[e-1])) e--;
    return s.substr(b, e - b);
}

static IUIAutomation* GetUia() {
    static IUIAutomation* s_uia = nullptr;
    if (!s_uia) {
        CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                         IID_IUIAutomation, (void**)&s_uia);
    }
    return s_uia;
}

// 取控件全文并按行号切出目标行。RichEdit 的换行是 \r（不是 \r\n），
// 只按 \n 切会把整个控件当成一行。
static std::wstring LineFromText(const std::wstring& all, int wantLine) {
    if (wantLine < 0) wantLine = 0;
    int line = 0;
    size_t start = 0;
    for (size_t i = 0; i <= all.size(); ++i) {
        bool end = (i == all.size()) || all[i] == L'\n' || all[i] == L'\r';
        if (!end) continue;
        if (line == wantLine) return all.substr(start, i - start);
        ++line;
        if (i < all.size() && all[i] == L'\r' &&
            i + 1 < all.size() && all[i + 1] == L'\n') {
            ++i;
        }
        start = i + 1;
    }
    return std::wstring();
}

// 用矩形把一行的每个字符水平位置按等宽铺开（没有更精确信息时的兜底）
static void FillUniformX(const std::wstring& t, const RECT& rc, std::vector<double>& xs) {
    xs.clear();
    if (t.empty()) return;
    double L = (double)rc.left, R = (double)rc.right;
    double step = (R - L) / (double)t.size();
    xs.reserve(t.size());
    for (size_t i = 0; i < t.size(); ++i) xs.push_back(L + step * ((double)i + 0.5));
}

// ---------------- 策略 1.5：TextPattern 逐字矩形 ----------------
//
// UIA 的文字是源文本（不会认错字），但很多程序（Chromium / Word）的
// RangeFromPoint 返回的是整个段落，段落里的软换行又没有 \r\n，
// 直接当“行”用会取到上一行/整段。可靠的做法是拿段落里每个字的
// GetBoundingRectangles，按 y 聚成视觉行，再按鼠标 Y 选行。
// 这样文字来自 UIA、位置也来自 UIA，不需要均匀估算。

struct UiaGlyph {
    std::wstring text;   // 一个 UTF-16 码元
    RECT rc{};           // 屏幕矩形；失败时为 0
};

struct UiaLineData {
    std::wstring text;
    std::vector<double> xs;
    RECT rc{};
};

static bool FirstGlyphRect(IUIAutomationTextRange* r, RECT& rc) {
    SAFEARRAY* sa = nullptr;
    if (FAILED(r->GetBoundingRectangles(&sa)) || !sa) return false;

    LONG lb = 0, ub = -1;
    if (FAILED(SafeArrayGetLBound(sa, 1, &lb)) ||
        FAILED(SafeArrayGetUBound(sa, 1, &ub)) || ub < lb + 3) {
        SafeArrayDestroy(sa);
        return false;
    }
    double* p = nullptr;
    bool ok = false;
    if (SUCCEEDED(SafeArrayAccessData(sa, (void**)&p)) && p) {
        for (LONG i = lb; i + 3 <= ub; i += 4) {
            if (p[i + 2] >= 2.0 && p[i + 3] >= 2.0) {
                rc.left = (LONG)std::lround(p[i]);
                rc.top = (LONG)std::lround(p[i + 1]);
                rc.right = (LONG)std::lround(p[i] + p[i + 2]);
                rc.bottom = (LONG)std::lround(p[i + 1] + p[i + 3]);
                ok = true;
                break;
            }
        }
        SafeArrayUnaccessData(sa);
    }
    SafeArrayDestroy(sa);
    return ok;
}

// 把段落范围逐字走一遍，拿到每个码元的文字和屏幕矩形。
static bool CollectRangeGlyphs(IUIAutomationTextRange* para, std::vector<UiaGlyph>& out,
                               int maxChars) {
    out.clear();
    IUIAutomationTextRange* r = nullptr;
    if (FAILED(para->Clone(&r)) || !r) return false;
    if (FAILED(r->MoveEndpointByRange(TextPatternRangeEndpoint_End, para,
                                      TextPatternRangeEndpoint_Start))) {
        r->Release();
        return false;
    }

    RECT last{};
    int moved = 0;
    for (int i = 0; i < maxChars; ++i) {
        if (FAILED(r->MoveEndpointByUnit(TextPatternRangeEndpoint_End,
                                         TextUnit_Character, 1, &moved)) || moved == 0) {
            break;
        }
        BSTR b = nullptr;
        std::wstring t;
        if (SUCCEEDED(r->GetText(8, &b)) && b) {
            t.assign(b, SysStringLen(b));
            SysFreeString(b);
        }
        RECT rc{};
        bool have = FirstGlyphRect(r, rc);
        if (!have) rc = last;
        else last = rc;

        for (wchar_t c : t) {
            UiaGlyph g;
            g.text.assign(1, c);
            g.rc = rc;
            out.push_back(g);
        }
        if (FAILED(r->MoveEndpointByUnit(TextPatternRangeEndpoint_Start,
                                         TextUnit_Character, 1, &moved)) || moved == 0) {
            break;
        }
    }
    r->Release();
    return !out.empty();
}

// 按矩形 top 聚视觉行；行内按 left 排序，拼出文字和每字中心 x。
static void BuildVisualLines(const std::vector<UiaGlyph>& glyphs, std::vector<UiaLineData>& lines) {
    lines.clear();
    std::vector<int> order;
    order.reserve(glyphs.size());
    for (int i = 0; i < (int)glyphs.size(); ++i) {
        const RECT& rc = glyphs[i].rc;
        if (rc.right > rc.left && rc.bottom > rc.top && !glyphs[i].text.empty()) order.push_back(i);
    }
    if (order.empty()) return;

    std::sort(order.begin(), order.end(), [&](int a, int b) {
        if (glyphs[a].rc.top != glyphs[b].rc.top) return glyphs[a].rc.top < glyphs[b].rc.top;
        return glyphs[a].rc.left < glyphs[b].rc.left;
    });

    std::vector<int> group;
    auto flush = [&]() {
        if (group.empty()) return;
        std::sort(group.begin(), group.end(), [&](int a, int b) {
            return glyphs[a].rc.left < glyphs[b].rc.left;
        });
        UiaLineData line;
        RECT rc = glyphs[group[0]].rc;
        for (int idx : group) {
            const RECT& g = glyphs[idx].rc;
            line.text += glyphs[idx].text;
            line.xs.push_back(((double)g.left + (double)g.right) * 0.5);
            if (g.left < rc.left) rc.left = g.left;
            if (g.top < rc.top) rc.top = g.top;
            if (g.right > rc.right) rc.right = g.right;
            if (g.bottom > rc.bottom) rc.bottom = g.bottom;
        }
        line.rc = rc;
        lines.push_back(std::move(line));
        group.clear();
    };

    LONG curTop = glyphs[order[0]].rc.top;
    for (int idx : order) {
        LONG top = glyphs[idx].rc.top;
        if (std::labs((long)top - (long)curTop) > 4) {
            flush();
            curTop = top;
        }
        group.push_back(idx);
    }
    flush();
}

static void AddPatternAt(IUIAutomationElement* el, std::vector<IUIAutomationTextPattern*>& out) {
    if (!el) return;
    IUIAutomationTextPattern* tp = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPatternId, IID_IUIAutomationTextPattern,
                                          (void**)&tp)) && tp) {
        out.push_back(tp);
    }
}

// 在鼠标点附近找一个 TextPattern 范围：从点到右侧逐级试探，
// 这样鼠标停在行首左边/页边距时也能命中同一行。
// 注意：Chromium 在文字上返回的 range 可能是退化的（GetText 为空），
// 但 ExpandToEnclosingUnit(Paragraph) 之后就能拿到整段，所以这里不按 GetText 过滤。
static IUIAutomationTextRange* RangeNearPoint(IUIAutomationTextPattern* tp, POINT pt) {
    static const int kOffsets[] = { 0, 8, 16, 24, 32, 48, 64, 96, 128, 192, 256 };
    for (int dx : kOffsets) {
        POINT q{ pt.x + dx, pt.y };
        IUIAutomationTextRange* r = nullptr;
        if (SUCCEEDED(tp->RangeFromPoint(q, &r)) && r) return r;
    }
    return nullptr;
}

static bool UiaVisualLineAtPoint(POINT pt, std::wstring& outText, std::wstring& source,
                                 RECT* outRect, std::vector<double>* charX) {
    IUIAutomation* uia = GetUia();
    if (!uia) return false;

    IUIAutomationElement* el = nullptr;
    if (FAILED(uia->ElementFromPoint(pt, &el)) || !el) return false;

    std::vector<IUIAutomationTextPattern*> patterns;
    AddPatternAt(el, patterns);

    IUIAutomationTreeWalker* walker = nullptr;
    uia->get_ControlViewWalker(&walker);
    IUIAutomationElement* cur = el;
    cur->AddRef();
    for (int depth = 0; depth < 8 && patterns.size() < 3; ++depth) {
        IUIAutomationElement* parent = nullptr;
        if (!walker || FAILED(walker->GetParentElement(cur, &parent)) || !parent) break;
        cur->Release();
        cur = parent;
        AddPatternAt(cur, patterns);
    }
    cur->Release();
    if (walker) walker->Release();

    // 有些程序（部分 Electron / 内嵌浏览器）把 TextPattern 放在 Document 后代上，
    // 而不是当前元素或祖先；前面没找到时再向下找一层 Document。
    if (patterns.empty()) {
        IUIAutomationCondition* cond = nullptr;
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = UIA_DocumentControlTypeId;
        if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond)) && cond) {
            IUIAutomationElementArray* arr = nullptr;
            if (SUCCEEDED(el->FindAll(TreeScope_Descendants, cond, &arr)) && arr) {
                int n = 0;
                arr->get_Length(&n);
                if (n > 4) n = 4;
                for (int i = 0; i < n; ++i) {
                    IUIAutomationElement* d = nullptr;
                    if (SUCCEEDED(arr->GetElement(i, &d)) && d) {
                        AddPatternAt(d, patterns);
                        d->Release();
                    }
                }
                arr->Release();
            }
            cond->Release();
        }
    }

    bool ok = false;
    for (IUIAutomationTextPattern* tp : patterns) {
        IUIAutomationTextRange* r = RangeNearPoint(tp, pt);
        if (!r) continue;

        IUIAutomationTextRange* para = nullptr;
        if (FAILED(r->Clone(&para)) || !para) { r->Release(); continue; }
        para->ExpandToEnclosingUnit(TextUnit_Paragraph);

        std::vector<UiaGlyph> glyphs;
        if (!CollectRangeGlyphs(para, glyphs, 3000)) {
            para->Release();
            r->Release();
            continue;
        }
        para->Release();
        r->Release();

        std::vector<UiaLineData> lines;
        BuildVisualLines(glyphs, lines);
        int pick = -1, pickH = 1 << 30;
        for (int i = 0; i < (int)lines.size(); ++i) {
            const RECT& rc = lines[i].rc;
            int hgt = rc.bottom - rc.top + 1;
            int tol = hgt / 4;
            if (tol < 2) tol = 2;
            if (tol > 8) tol = 8;
            if (pt.y >= rc.top - tol && pt.y <= rc.bottom + tol) {
                if (hgt < pickH) { pickH = hgt; pick = i; }
            }
        }
        if (pick < 0) continue;

        const UiaLineData& line = lines[pick];
        if (line.text.empty() || line.xs.size() != line.text.size()) continue;
        outText = line.text;
        if (charX) *charX = line.xs;
        if (outRect) *outRect = line.rc;
        source = L"UIA 文字行";
        ok = true;
        break;
    }

    for (IUIAutomationTextPattern* tp : patterns) tp->Release();
    el->Release();
    return ok;
}

// ---------------- 策略 1：原生 EDIT / RichEdit 精确取行 ----------------

static bool TryNativeEditLine(POINT pt, std::wstring& out, std::wstring& source,
                              RECT* outRect, std::vector<double>* charX) {
    HWND h = WindowFromPoint(pt);
    if (!h) return false;

    // 向下钻到最内层子窗口
    for (int depth = 0; depth < 8; ++depth) {
        POINT c = pt;
        ScreenToClient(h, &c);
        HWND child = ChildWindowFromPointEx(h, c,
                        CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (!child || child == h) break;
        h = child;
    }

    wchar_t cls[64] = {0};
    GetClassNameW(h, cls, 64);

    bool isEdit = (wcscmp(cls, L"Edit") == 0);
    bool isRich = (wcsncmp(cls, L"RichEdit", 8) == 0);
    if (!isEdit && !isRich) return false;

    POINT c = pt;
    ScreenToClient(h, &c);

    int charIdx = -1;
    if (isRich) {
        LRESULT r = SendMessageW(h, EM_CHARFROMPOS, 0, MAKELPARAM(c.x, c.y));
        charIdx = (int)LOWORD(r);
    } else {
        LRESULT r = SendMessageW(h, EM_CHARFROMPOS, 0, MAKELPARAM(c.x, c.y));
        charIdx = (int)LOWORD(r);
    }
    if (charIdx < 0) return false;

    int lineIdx = (int)SendMessageW(h, EM_LINEFROMCHAR, (WPARAM)charIdx, 0);
    if (lineIdx < 0) return false;

    int len = GetWindowTextLengthW(h);
    if (len <= 0 || len > 200000) return false;
    std::wstring all((size_t)len + 1, L'\0');
    int got = GetWindowTextW(h, &all[0], len + 1);
    if (got <= 0) return false;
    all.resize((size_t)got);

    std::wstring line = LineFromText(all, lineIdx);
    if (TrimW3(line).empty()) return false;

    // 行矩形：单行 Edit 用 EM_POSFROMCHAR 拿这一行的真实左右边界，
    // 比「整个控件矩形」准得多（否则短行会被当成整宽，鼠标落点算错）
    RECT lineRect{};
    bool haveLineRect = false;
    if (isEdit) {
        int lineStart = (int)SendMessageW(h, EM_LINEINDEX, (WPARAM)lineIdx, 0);
        int lineLen   = (int)SendMessageW(h, EM_LINELENGTH, (WPARAM)lineStart, 0);
        if (lineStart >= 0 && lineLen > 0) {
            // 只能问「行内真实存在的字符」的坐标，问行尾后一个字符会返回 -1
            int first = lineStart;
            int lastCh = lineStart + lineLen - 1;
            if (lastCh > got - 1) lastCh = got - 1;
            LRESULT p0 = SendMessageW(h, EM_POSFROMCHAR, (WPARAM)first, 0);
            LRESULT p1 = SendMessageW(h, EM_POSFROMCHAR, (WPARAM)lastCh, 0);
            if (p0 != -1 && p1 != -1) {
                POINT a{ (short)LOWORD(p0), (short)HIWORD(p0) };
                POINT b{ (short)LOWORD(p1), (short)HIWORD(p1) };
                ClientToScreen(h, &a);
                ClientToScreen(h, &b);
                int n = lastCh - first;                     // 字符间隔数
                int adv = (n > 0) ? (b.x - a.x) / n : 14;   // 平均字宽
                if (adv <= 0) adv = 14;
                lineRect.left = a.x;
                lineRect.right = b.x + adv;                 // 最后一个字的右侧
                RECT wr{};
                GetWindowRect(h, &wr);
                lineRect.top = wr.top;
                lineRect.bottom = wr.bottom;
                if (lineRect.right <= lineRect.left) lineRect.right = lineRect.left + adv;
                haveLineRect = true;
            }
        }
    }

    if (outRect) {
        if (haveLineRect) *outRect = lineRect;
        else GetWindowRect(h, outRect);
    }
    if (charX) {
        RECT use = haveLineRect ? lineRect : RECT{};
        if (!haveLineRect) GetWindowRect(h, &use);
        FillUniformX(line, use, *charX);
    }

    out = line;
    source = L"控件文字";
    return true;
}

// ---------------- 策略 2：UIA ----------------

struct Cand {
    long top;
    long left;
    long right;
    long bottom;
    std::wstring text;
};

static bool TryUiaTextPattern(IUIAutomation* uia, POINT pt, std::wstring& out,
                              std::wstring& source, RECT* outRect,
                              std::vector<double>* charX) {
    IUIAutomationElement* el = nullptr;
    if (FAILED(uia->ElementFromPoint(pt, &el)) || !el) return false;

    RECT elRect{};
    el->get_CurrentBoundingRectangle(&elRect);

    bool ok = false;

    // 2a) 不再用 RangeFromPoint 返回的段落文字直接当行：
    //     Chromium 的软换行没有 \r\n，整段会被误当成一行。
    //     逐字矩形路径（UiaVisualLineAtPoint）已经能给出真正的视觉行。

    // 2b) 几何聚行：把 Descendants 中 Text 类型且纵向覆盖鼠标点的元素按 left 拼接
    if (!ok) {
        IUIAutomationCondition* cond = nullptr;
        if (SUCCEEDED(uia->CreateTrueCondition(&cond)) && cond) {
            IUIAutomationElementArray* arr = nullptr;
            if (SUCCEEDED(el->FindAll(TreeScope_Descendants, cond, &arr)) && arr) {
                int n = 0;
                arr->get_Length(&n);
                if (n > 4000) n = 4000;

                std::vector<Cand> cands;
                cands.reserve(64);
                for (int i = 0; i < n; ++i) {
                    IUIAutomationElement* c = nullptr;
                    if (FAILED(arr->GetElement(i, &c)) || !c) continue;

                    CONTROLTYPEID ct = 0;
                    c->get_CurrentControlType(&ct);
                    if (ct == UIA_TextControlTypeId || ct == UIA_ListItemControlTypeId) {
                        RECT rc{};
                        if (SUCCEEDED(c->get_CurrentBoundingRectangle(&rc))) {
                            long hgt = rc.bottom - rc.top;
                            long wid = rc.right - rc.left;
                            // 笔尖是左锚点：元素必须落在笔尖右侧（或跨过笔尖）。
                            // 只看 Y 时会收进同一高度上左边那一栏的元素——鼠标在正文里，
                            // 返回的却是侧栏「空间 (0)」「暂无任务」；任务管理器的「图标 / CPU」同理。
                            bool coversY = (pt.y >= rc.top && pt.y <= rc.bottom);
                            bool atRight = (rc.right >= pt.x - 24);
                            // 左边也不能太远：整块空白那头的元素（别的栏、别的窗口）不要
                            bool notFarRight = (rc.left <= pt.x + MaxRightGap(20));
                            if (coversY && atRight && notFarRight && hgt > 0 && hgt <= 120 && wid <= 2000) {
                                BSTR nm = nullptr;
                                if (SUCCEEDED(c->get_CurrentName(&nm)) && nm) {
                                    std::wstring s = TakeBstr(nm);
                                    if (!TrimW3(s).empty() && s.size() <= 200) {
                                        cands.push_back(Cand{ rc.top, rc.left, rc.right, rc.bottom, s });
                                    }
                                }
                            }
                        }
                    }
                    c->Release();
                }
                arr->Release();

                // 按 top 聚成行，选包含鼠标点且行高最小（最贴近该行）的一组
                std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
                    if (a.top != b.top) return a.top < b.top;
                    return a.left < b.left;
                });

                std::vector<Cand> best;
                long bestDist = -1;
                for (size_t i = 0; i < cands.size(); ) {
                    size_t j = i + 1;
                    while (j < cands.size() && labs(cands[j].top - cands[i].top) <= 6) ++j;
                    // 这一组的纵向范围，用「离笔尖最近」而不是「元素最多」来选：
                    // 元素多的一组往往是表格行或整块面板，未必是笔尖那一行
                    long top = cands[i].top, bottom = cands[i].bottom;
                    for (size_t k = i; k < j; ++k) {
                        if (cands[k].top < top) top = cands[k].top;
                        if (cands[k].bottom > bottom) bottom = cands[k].bottom;
                    }
                    long dist = (pt.y < top) ? (top - pt.y)
                                             : (pt.y > bottom ? pt.y - bottom : 0);
                    bool better = (bestDist < 0) || (dist < bestDist) ||
                                  (dist == bestDist && (j - i) > best.size());
                    if (better) {
                        bestDist = dist;
                        best.assign(cands.begin() + (long)i, cands.begin() + (long)j);
                    }
                    i = j;
                }

                if (!best.empty()) {
                    std::sort(best.begin(), best.end(),
                              [](const Cand& a, const Cand& b) { return a.left < b.left; });
                    std::wstring s;
                    std::vector<double> xs;   // 每个字在屏幕上的水平中心
                    long prevRight = 0;
                    bool first = true;
                    for (size_t i = 0; i < best.size(); ++i) {
                        if (!first && best[i].left > prevRight + 3) {
                            s.push_back(L' ');
                            xs.push_back(((double)prevRight + (double)best[i].left) / 2.0);
                        }
                        const std::wstring& piece = best[i].text;
                        double L = (double)best[i].left, R = (double)best[i].right;
                        double step = (R - L) / (double)(piece.empty() ? 1 : piece.size());
                        for (size_t k = 0; k < piece.size(); ++k) {
                            xs.push_back(L + step * ((double)k + 0.5));
                        }
                        s += piece;
                        prevRight = best[i].right;
                        first = false;
                    }

                    // 去首尾空白，同时把字符位置表一起对齐裁掉
                    size_t tb = 0, te = s.size();
                    while (tb < te && iswspace(s[tb])) ++tb;
                    while (te > tb && iswspace(s[te - 1])) --te;
                    if (tb > 0 || te < s.size()) {
                        s = s.substr(tb, te - tb);
                        if (xs.size() >= te) xs = std::vector<double>(xs.begin() + (long)tb,
                                                                     xs.begin() + (long)te);
                        else xs.clear();
                    }

                    if (!s.empty()) {
                        out = s;
                        source = L"UIA 文字元素";
                        ok = true;
                        if (charX && xs.size() == s.size()) *charX = xs;
                        if (outRect) {
                            long l = best[0].left, r = best[0].right;
                            long t = best[0].top, b = best[0].bottom;
                            for (size_t i = 1; i < best.size(); ++i) {
                                if (best[i].left < l) l = best[i].left;
                                if (best[i].right > r) r = best[i].right;
                                if (best[i].top < t) t = best[i].top;
                                if (best[i].bottom > b) b = best[i].bottom;
                            }
                            outRect->left = l;
                            outRect->right = r;
                            outRect->top = t;
                            outRect->bottom = b;
                        }
                    }
                }
            }
            cond->Release();
        }
    }

    // 2c) 元素自身 Name（按钮 / 标签 / 单行输入框）
    //     只在「内容型且像一行」的控件上采信：整段/多行文本的 Name 不能当行用。
    if (!ok) {
        CONTROLTYPEID ct = 0;
        el->get_CurrentControlType(&ct);
        long elH = elRect.bottom - elRect.top;
        long elW = elRect.right - elRect.left;
        bool textish = (ct == UIA_TextControlTypeId ||
                        ct == UIA_ListItemControlTypeId ||
                        ct == UIA_DataItemControlTypeId);
        // 文本类控件只有“明显是一行”才用 Name；整段/多行的 Name 会取到上一行。
        bool lineLike = (elH > 0 && elH <= (textish ? 32 : 48) && elW > 0 && elW <= 2000);
        bool contentLike = (ct == UIA_TextControlTypeId ||
                            ct == UIA_ButtonControlTypeId ||
                            ct == UIA_EditControlTypeId ||
                            ct == UIA_HyperlinkControlTypeId ||
                            ct == UIA_ListItemControlTypeId ||
                            ct == UIA_DataItemControlTypeId ||
                            ct == UIA_MenuItemControlTypeId ||
                            ct == UIA_CheckBoxControlTypeId ||
                            ct == UIA_RadioButtonControlTypeId);
        if (contentLike && lineLike) {
            BSTR nm = nullptr;
            if (SUCCEEDED(el->get_CurrentName(&nm)) && nm) {
                std::wstring s = TrimW3(TakeBstr(nm));
                if (!s.empty() && s.size() <= 300) {
                    out = s;
                    source = L"UIA 控件名称";
                    ok = true;
                    if (outRect) *outRect = elRect;
                }
            }
        }
    }

    el->Release();
    return ok;
}

// ---------------- 策略 3：MSAA ----------------

static bool TryMsaa(POINT pt, std::wstring& out, std::wstring& source, RECT* outRect) {
    IAccessible* acc = nullptr;
    VARIANT varChild;
    VariantInit(&varChild);
    if (FAILED(AccessibleObjectFromPoint(pt, &acc, &varChild)) || !acc) return false;

    RECT accRect{};
    bool haveRect = false;
    {
        long l = 0, t = 0, w = 0, h = 0;
        if (SUCCEEDED(acc->accLocation(&l, &t, &w, &h, varChild))) {
            accRect.left = l; accRect.top = t;
            accRect.right = l + w; accRect.bottom = t + h;
            haveRect = true;
        }
    }

    // 几何校验：AccessibleObjectFromPoint 经常返回「容器」而不是文字本身——
    // 桌面空白处返回整个桌面对象（名字就叫「桌面」，rect 是全屏），
    // Electron 程序返回 Chrome Legacy Window（rect 是整个窗口）。
    // 只有 rect 像一行文字、且落在笔尖邻域时才采信。
    if (haveRect) {
        long wid = accRect.right - accRect.left;
        long hgt = accRect.bottom - accRect.top;
        bool lineLike = (wid > 0 && hgt > 0 && wid <= 2000 && hgt <= 200);
        bool coversY = (pt.y >= accRect.top - 8 && pt.y <= accRect.bottom + 8);
        bool nearX = (accRect.right >= pt.x - 24 &&
                      accRect.left <= pt.x + MaxRightGap(20));
        if (!lineLike || !coversY || !nearX) {
            acc->Release();
            return false;
        }
    }

    bool ok = false;

    BSTR nm = nullptr;
    if (SUCCEEDED(acc->get_accName(varChild, &nm)) && nm) {
        std::wstring s = TrimW3(TakeBstr(nm));
        if (!s.empty() && s.size() <= 300) {
            out = s;
            source = L"MSAA 名称";
            ok = true;
            if (outRect) *outRect = accRect;
        }
    }

    if (!ok) {
        BSTR val = nullptr;
        if (SUCCEEDED(acc->get_accValue(varChild, &val)) && val) {
            std::wstring s = TrimW3(TakeBstr(val));
            size_t nl = s.find_first_of(L"\r\n");
            if (nl != std::wstring::npos) s = s.substr(0, nl);
            s = TrimW3(s);
            if (!s.empty() && s.size() <= 300) {
                out = s;
                source = L"MSAA 值";
                ok = true;
                if (outRect) *outRect = accRect;
            }
        }
    }

    acc->Release();
    return ok;
}

// ---------------- 对外入口 ----------------

bool CaptureLineAtCursor(std::wstring& text, std::wstring& source, RECT* outRect,
                         std::vector<double>* charX) {
    text.clear();
    source.clear();
    if (charX) charX->clear();
    if (outRect) { outRect->left = outRect->top = outRect->right = outRect->bottom = 0; }

    POINT pt{};
    if (!GetCursorPos(&pt)) return false;

    if (CaptureNativeEditLine(text, source, outRect, charX)) return true;
    return CaptureTextLayer(pt, text, source, outRect, charX);
}

// 原生 EDIT / RichEdit：位置精确，不截屏
bool CaptureNativeEditLine(std::wstring& text, std::wstring& source, RECT* outRect,
                           std::vector<double>* charX) {
    text.clear();
    source.clear();
    if (charX) charX->clear();
    if (outRect) { outRect->left = outRect->top = outRect->right = outRect->bottom = 0; }

    POINT pt{};
    if (!GetCursorPos(&pt)) return false;
    return TryNativeEditLine(pt, text, source, outRect, charX);
}

// UIA → MSAA：能拿到文字，位置只能按元素矩形估算
bool CaptureTextLayer(POINT pt, std::wstring& text, std::wstring& source,
                      RECT* outRect, std::vector<double>* charX) {
    text.clear();
    source.clear();
    if (charX) charX->clear();
    if (outRect) { outRect->left = outRect->top = outRect->right = outRect->bottom = 0; }

    if (IUIAutomation* uia = GetUia()) {
        // 首选：逐字矩形聚视觉行（文字和坐标都来自 UIA，最准）
        if (UiaVisualLineAtPoint(pt, text, source, outRect, charX)) return true;
        // 兜底：没有 TextPattern 的程序走几何聚行 / 控件名称
        if (TryUiaTextPattern(uia, pt, text, source, outRect, charX)) {
            if (charX && charX->size() != text.size() && outRect && outRect->right > outRect->left) {
                FillUniformX(text, *outRect, *charX);
            }
            return true;
        }
    }
    if (TryMsaa(pt, text, source, outRect)) {
        if (charX && outRect && outRect->right > outRect->left) FillUniformX(text, *outRect, *charX);
        return true;
    }
    return false;
}

// 从 TextPattern 读出整篇文本
static bool TextOfPattern(IUIAutomationTextPattern* tp, std::wstring& out) {
    out.clear();
    IUIAutomationTextRange* doc = nullptr;
    if (FAILED(tp->get_DocumentRange(&doc)) || !doc) return false;
    bool ok = false;
    BSTR t = nullptr;
    if (SUCCEEDED(doc->GetText(200000, &t)) && t) {
        std::wstring s(t, SysStringLen(t));
        SysFreeString(t);
        if (s.size() >= 4) { out = s; ok = true; }
    }
    doc->Release();
    return ok;
}

static void AddCandidate(std::vector<std::wstring>& v, IUIAutomationTextPattern* tp) {
    if (!tp) return;
    std::wstring t;
    if (TextOfPattern(tp, t)) v.push_back(std::move(t));
    tp->Release();
}

// 取鼠标所在位置的「网页/文档」全文。
// 实测：Chromium 的 DocumentRange 文本可用，但几何是块级的；
// 而 ElementFromPoint 拿到的可能是浏览器外壳，往上找 TextPattern 会拿到标签栏文字，
// 所以这里收集多个候选（自身 / 最深的 Document 后代 / 祖先），用 OCR 那一行挑最匹配的。
bool GetUiaDocumentText(POINT pt, const std::wstring& hint, std::wstring& text) {
    text.clear();

    HWND h = WindowFromPoint(pt);
    if (!h) return false;

    unsigned long long hintHash = 1469598103934665603ULL;
    for (size_t i = 0; i < hint.size(); ++i) {
        hintHash ^= (unsigned long long)hint[i];
        hintHash *= 1099511628211ULL;
    }

    static std::wstring s_cache;
    static HWND s_cacheHwnd = nullptr;
    static unsigned long long s_cacheHint = 0;
    static DWORD s_cacheTick = 0;
    DWORD now = GetTickCount();
    if (h == s_cacheHwnd && hintHash == s_cacheHint && !s_cache.empty() &&
        (DWORD)(now - s_cacheTick) < 2000) {
        text = s_cache;
        return true;
    }

    IUIAutomation* uia = GetUia();
    if (!uia) return false;

    IUIAutomationElement* el = nullptr;
    if (FAILED(uia->ElementFromPoint(pt, &el)) || !el) return false;

    std::vector<std::wstring> cands;

    // 1) 元素自身
    {
        IUIAutomationTextPattern* tp = nullptr;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPatternId, IID_IUIAutomationTextPattern,
                                             (void**)&tp)) && tp) {
            AddCandidate(cands, tp);
        }
    }

    // 2) 子孙里最靠上的一批 Document 元素（Chromium 的网页正文就在这里）
    {
        IUIAutomationCondition* cond = nullptr;
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = UIA_DocumentControlTypeId;
        if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond)) && cond) {
            IUIAutomationElementArray* arr = nullptr;
            if (SUCCEEDED(el->FindAll(TreeScope_Descendants, cond, &arr)) && arr) {
                int n = 0;
                arr->get_Length(&n);
                if (n > 4) n = 4;
                for (int i = 0; i < n; ++i) {
                    IUIAutomationElement* d = nullptr;
                    if (FAILED(arr->GetElement(i, &d)) || !d) continue;
                    IUIAutomationTextPattern* tp = nullptr;
                    if (SUCCEEDED(d->GetCurrentPatternAs(UIA_TextPatternId,
                                                        IID_IUIAutomationTextPattern,
                                                        (void**)&tp)) && tp) {
                        AddCandidate(cands, tp);
                    }
                    d->Release();
                }
                arr->Release();
            }
            cond->Release();
        }
    }

    // 3) 往上找：兼容 Electron 之类把 TextPattern 挂在祖先上的实现
    {
        IUIAutomationElement* cur = el;
        cur->AddRef();
        IUIAutomationTreeWalker* walker = nullptr;
        uia->get_ControlViewWalker(&walker);
        for (int depth = 0; depth < 6; ++depth) {
            IUIAutomationTextPattern* tp = nullptr;
            if (SUCCEEDED(cur->GetCurrentPatternAs(UIA_TextPatternId, IID_IUIAutomationTextPattern,
                                                  (void**)&tp)) && tp) {
                AddCandidate(cands, tp);
                break;
            }
            IUIAutomationElement* parent = nullptr;
            bool moved = false;
            if (walker && SUCCEEDED(walker->GetParentElement(cur, &parent)) && parent) {
                cur->Release();
                cur = parent;
                moved = true;
            }
            if (!moved) break;
        }
        if (walker) walker->Release();
        cur->Release();
    }
    el->Release();

    if (cands.empty()) return false;

    // 有 OCR 提示行时：选与它最匹配的那份文档；否则取最长的一份
    size_t bestIdx = 0;
    double bestRatio = -1.0;
    if (!hint.empty()) {
        for (size_t i = 0; i < cands.size(); ++i) {
            AlignResult ar = AlignLineToDocument(cands[i], hint, 0.0);
            double r = ar.ok ? ar.ratio : 0.0;
            if (r > bestRatio) {
                bestRatio = r;
                bestIdx = i;
            }
        }
    } else {
        for (size_t i = 1; i < cands.size(); ++i) {
            if (cands[i].size() > cands[bestIdx].size()) bestIdx = i;
        }
    }

    text = cands[bestIdx];
    s_cache = text;
    s_cacheHwnd = h;
    s_cacheHint = hintHash;
    s_cacheTick = now;
    return true;
}
