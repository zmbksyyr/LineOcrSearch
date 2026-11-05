// uia.cpp - C1 文字层取词：原生控件 -> UIA -> MSAA，全程不截屏
#include "common.h"
#include <uiautomation.h>
#include <oleacc.h>
#include <algorithm>
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

// 取控件全文并按行号切出目标行
static std::wstring LineFromText(const std::wstring& all, int wantLine) {
    if (wantLine < 0) wantLine = 0;
    int line = 0;
    size_t start = 0;
    for (size_t i = 0; i <= all.size(); ++i) {
        if (i == all.size() || all[i] == L'\n') {
            if (line == wantLine) {
                size_t e = i;
                if (e > start && all[e-1] == L'\r') --e;
                return all.substr(start, e - start);
            }
            ++line;
            start = i + 1;
        }
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
        charIdx = (int)(short)LOWORD(r);
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

    // 2a) TextPattern.RangeFromPoint（部分程序可用）
    IUIAutomationTextPattern* tp = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPatternId, IID_IUIAutomationTextPattern,
                                          (void**)&tp)) && tp) {
        IUIAutomationTextRange* r = nullptr;
        if (SUCCEEDED(tp->RangeFromPoint(pt, &r)) && r) {
            BSTR t = nullptr;
            if (SUCCEEDED(r->GetText(512, &t))) {
                std::wstring s = TakeBstr(t);
                s = TrimW3(s);
                if (s.size() >= 1) {
                    // RangeFromPoint 可能返回整段；取第一行
                    size_t nl = s.find_first_of(L"\r\n");
                    if (nl != std::wstring::npos) s = s.substr(0, nl);
                    s = TrimW3(s);
                    if (!s.empty()) {
                        out = s;
                        source = L"UIA 文本范围";
                        ok = true;
                        if (outRect) *outRect = elRect;
                    }
                }
            }
            r->Release();
        }
        tp->Release();
    }

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
                            if (pt.y >= rc.top && pt.y <= rc.bottom && hgt > 0 && hgt <= 120) {
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
                for (size_t i = 0; i < cands.size(); ) {
                    size_t j = i + 1;
                    while (j < cands.size() && labs(cands[j].top - cands[i].top) <= 6) ++j;
                    if (j - i > best.size()) {
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
    //     只在「内容型」控件上采信，否则鼠标停在窗口空白处会取到窗口标题这种没用的文字
    if (!ok) {
        CONTROLTYPEID ct = 0;
        el->get_CurrentControlType(&ct);
        bool contentLike = (ct == UIA_TextControlTypeId ||
                            ct == UIA_ButtonControlTypeId ||
                            ct == UIA_EditControlTypeId ||
                            ct == UIA_HyperlinkControlTypeId ||
                            ct == UIA_ListItemControlTypeId ||
                            ct == UIA_DataItemControlTypeId ||
                            ct == UIA_MenuItemControlTypeId ||
                            ct == UIA_CheckBoxControlTypeId ||
                            ct == UIA_RadioButtonControlTypeId);
        if (contentLike) {
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
    {
        long l = 0, t = 0, w = 0, h = 0;
        if (SUCCEEDED(acc->accLocation(&l, &t, &w, &h, varChild))) {
            accRect.left = l; accRect.top = t;
            accRect.right = l + w; accRect.bottom = t + h;
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

