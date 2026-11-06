// text_clean.cpp - OCR 结果清洗：只留纯文字，剥除首尾符号
#include "common.h"
#include <algorithm>

struct CharKind {
    bool text;   // 字母 / 汉字 / 数字
    bool space;
    bool ctrl;
};

// 「纯文字」判定：只认汉字与字母，数字、标点、符号一律算噪声。
// 理由：OCR 对数字和符号的准确率明显低于汉字，把它们带进关键词只会搜不到。
static bool IsHardText(wchar_t c) {
    if (c >= L'a' && c <= L'z') return true;
    if (c >= L'A' && c <= L'Z') return true;
    if (c >= 0x00C0 && c <= 0x024F) return true;  // 带音标拉丁
    if (c >= 0x0370 && c <= 0x03FF) return true;  // 希腊
    if (c >= 0x0400 && c <= 0x04FF) return true;  // 西里尔
    if (c >= 0x3040 && c <= 0x30FF) return true;  // 日文假名
    if (c >= 0x3400 && c <= 0x4DBF) return true;  // CJK 扩展 A
    if (c >= 0x4E00 && c <= 0x9FFF) return true;  // CJK 基本区
    if (c >= 0xAC00 && c <= 0xD7AF) return true;  // 韩文
    if (c >= 0xF900 && c <= 0xFAFF) return true;  // 兼容汉字
    if (c >= 0xFF21 && c <= 0xFF3A) return true;  // 全角大写
    if (c >= 0xFF41 && c <= 0xFF5A) return true;  // 全角小写
    return false;  // 数字 / 标点 / 符号 / 空格
}

static void Classify(const std::wstring& s, std::vector<CharKind>& out) {
    out.assign(s.size(), CharKind{false, false, false});
    if (s.empty()) return;

    std::vector<WORD> types(s.size(), 0);
    if (!GetStringTypeW(CT_CTYPE1, s.c_str(), (int)s.size(), &types[0])) {
        for (size_t i = 0; i < s.size(); ++i) {
            wchar_t c = s[i];
            out[i].space = (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n');
            out[i].text  = IsHardText(c);
        }
        return;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        WORD t = types[i];
        // 文字判定以码位为准；GetStringTypeW 只用来辅助判断空白与控制字符
        out[i].text  = IsHardText(s[i]);
        out[i].space = (t & C1_SPACE) != 0;
        out[i].ctrl  = (t & C1_CNTRL) != 0;
    }
}

// 把整行切成若干「纯文字段」：
//   数字、标点、符号都算分段符（OCR 对它们不准，带进关键词只会搜不到）；
//   段内空格保留，避免把英文词组劈开。
struct Seg { size_t b; size_t e; };

// 拉丁字母：只有空格两侧都是拉丁字母时才允许空格留在段内，
// 这样 `hello world` 不会被劈开，而 OCR 前缀噪声 `Dekap 改为系统控件吧` 会断开成两段。
static bool IsLatinLetter(wchar_t c) {
    if (c >= L'a' && c <= L'z') return true;
    if (c >= L'A' && c <= L'Z') return true;
    if (c >= 0x00C0 && c <= 0x024F) return true;
    return false;
}

static void SplitSegments(const std::wstring& raw, std::vector<Seg>& segs) {
    segs.clear();
    size_t i = 0;
    while (i < raw.size()) {
        if (!IsHardText(raw[i])) { ++i; continue; }
        size_t b = i;
        while (i < raw.size() && IsHardText(raw[i])) ++i;
        // 段内空格：两侧都是拉丁字母才跨过去，否则空格就是分段符
        while (i < raw.size() && (raw[i] == L' ' || raw[i] == L'\t')) {
            size_t j = i;
            while (j < raw.size() && (raw[j] == L' ' || raw[j] == L'\t')) ++j;
            if (j >= raw.size() || !IsHardText(raw[j])) break;
            if (i == 0 || !IsLatinLetter(raw[i - 1]) || !IsLatinLetter(raw[j])) break;
            i = j;
            while (i < raw.size() && IsHardText(raw[i])) ++i;
        }
        segs.push_back(Seg{ b, i });
    }
}

// 汉字：关键词只把汉字当作“文字”。英文字母/数字/标点都不算，
// 否则一串英文会把纯文字段的字符数撑大，选出来全是英文。
static bool IsCjkChar(wchar_t c) {
    return (c >= 0x3400 && c <= 0x4DBF) ||   // 扩展 A
           (c >= 0x4E00 && c <= 0x9FFF) ||   // 基本区
           (c >= 0xF900 && c <= 0xFAFF);     // 兼容汉字
}

// 段内有效文字数（不含空格），不足 2 个的段不能当关键词
static int SegTextCount(const std::wstring& raw, const Seg& s) {
    int n = 0;
    for (size_t i = s.b; i < s.e; ++i) if (IsHardText(raw[i])) ++n;
    return n;
}

static int SegCjkCount(const std::wstring& raw, const Seg& s) {
    int n = 0;
    for (size_t i = s.b; i < s.e; ++i) if (IsCjkChar(raw[i])) ++n;
    return n;
}

// 取出段文本（去掉首尾空格），有效文字不足 2 个返回空
static std::wstring SegText(const std::wstring& raw, const Seg& s) {
    size_t b = s.b, e = s.e;
    while (b < e && (raw[b] == L' ' || raw[b] == L'\t')) ++b;
    while (e > b && (raw[e - 1] == L' ' || raw[e - 1] == L'\t')) --e;
    if (e <= b) return std::wstring();
    std::wstring seg = raw.substr(b, e - b);
    if (SegTextCount(raw, s) < 2) return std::wstring();
    return seg;
}

// 没有鼠标位置：优先选汉字最多的一段；整行没有汉字时才退回英文/数字段。
std::wstring CleanOcrText(const std::wstring& raw) {
    if (raw.empty()) return std::wstring();
    std::vector<Seg> segs;
    SplitSegments(raw, segs);

    size_t pickCjk = 0, pickAny = 0;
    int bestCjk = 0, bestAny = 0;
    size_t lenCjk = 0, lenAny = 0;
    bool foundCjk = false, foundAny = false, sawCjk = false;

    for (size_t k = 0; k < segs.size(); ++k) {
        int cjk = SegCjkCount(raw, segs[k]);
        if (cjk > 0) sawCjk = true;
        int cnt = SegTextCount(raw, segs[k]);
        if (cnt < 2) continue;
        size_t len = segs[k].e - segs[k].b;
        if (cjk >= 2) {
            if (!foundCjk || cjk > bestCjk || (cjk == bestCjk && len > lenCjk)) {
                foundCjk = true;
                pickCjk = k;
                bestCjk = cjk;
                lenCjk = len;
            }
        } else if (!foundAny || cnt > bestAny || (cnt == bestAny && len > lenAny)) {
            foundAny = true;
            pickAny = k;
            bestAny = cnt;
            lenAny = len;
        }
    }
    if (foundCjk) return SegText(raw, segs[pickCjk]);
    if (sawCjk) return std::wstring();   // 有汉字但不足 2 个：不退回英文
    if (foundAny) return SegText(raw, segs[pickAny]);
    return std::wstring();
}

// 以鼠标右侧为准：取「包含鼠标、或位于鼠标右侧」的第一段；都在左侧时取最后一段。
// 这样鼠标放在一行中间时，拿到的永远是右半边的文字，而不是整行里最长的那段。
std::wstring CleanOcrTextByX(const std::wstring& raw,
                             const std::vector<double>& charX, double mouseX) {
    if (raw.empty()) return std::wstring();
    std::vector<Seg> segs;
    SplitSegments(raw, segs);
    if (segs.empty()) return std::wstring();

    bool haveX = (charX.size() == raw.size());
    if (!haveX) return CleanOcrText(raw);

    size_t pick = 0;
    bool found = false;
    size_t last = 0;
    bool hasLast = false;

    for (size_t k = 0; k < segs.size(); ++k) {
        if (SegTextCount(raw, segs[k]) < 2) continue;
        last = k;
        hasLast = true;
        double right = charX[segs[k].e - 1];
        if (!found && right >= mouseX) { pick = k; found = true; }
    }
    if (!found) {
        if (!hasLast) return std::wstring();
        pick = last;   // 鼠标在所有文字右侧：取最靠近的（最后）一段
    }
    return SegText(raw, segs[pick]);
}

// ---------------- 笔尖左锚点取词 ----------------

// 笔尖只是「从这里开始」的标记，不要求压在字上：
//   1) 找笔尖右侧第一个文字字符（笔尖落在某个字内部时从该字算起）
//   2) 从它所在的纯文字段取到段尾（标点/括号/数字切段，英文词间空格不切）
//   3) 段两端再去空白；中间原样保留
std::wstring TakeRightOfCursor(const std::wstring& raw,
                               const std::vector<double>& charX, double mouseX,
                               size_t* outBegin, size_t* outEnd,
                               double* firstLeft) {
    if (outBegin) *outBegin = 0;
    if (outEnd) *outEnd = 0;
    if (firstLeft) *firstLeft = -1.0;
    if (raw.empty()) return std::wstring();

    size_t i0 = 0;
    bool haveX = (charX.size() == raw.size());
    if (haveX) {
        i0 = raw.size();
        for (size_t i = 0; i < raw.size(); ++i) {
            if (!IsHardText(raw[i])) continue;      // 标点不参与「起点」判定
            // 这个字的右边缘：与下一个字符中心的中点（最后一个字按平均字宽外推）
            double right;
            if (i + 1 < raw.size() && charX[i + 1] > charX[i]) {
                right = (charX[i] + charX[i + 1]) * 0.5;
            } else if (i > 0 && charX[i] > charX[i - 1]) {
                right = charX[i] + (charX[i] - charX[i - 1]) * 0.5;
            } else {
                right = charX[i] + 8.0;
            }
            if (right >= mouseX) { i0 = i; break; }
        }
        if (i0 >= raw.size()) return std::wstring();  // 笔尖右侧没有字，不猜
    }

    std::wstring s = raw.substr(i0);
    if (s.empty()) return std::wstring();

    // 笔尖右侧的区域按「非纯文字」切段（标点 / 括号 / 数字都是分隔符，
    // 英文词间空格除外），优先取汉字最多的一段；没有汉字才退回英文段：
    //   「关于你问的「两端处理是不是误伤了」」-> 「两端处理是不是误伤了」；
    //   「已处理 7m59s」-> 「已处理」；「Microsoft Edge (8)」-> 「Microsoft Edge」。
    // 用户明确要求「取一大块纯文字去搜索」：数字和标点 OCR 不准，干脆不要；
    // 短段（如「总则」「第」）搜出来命中太泛，优先更长的段。
    std::vector<Seg> segs;
    SplitSegments(s, segs);
    if (segs.empty()) return std::wstring();

    size_t pickCjk = 0, pickAny = 0;
    int bestCjk = 0, bestAny = 0;
    size_t lenCjk = 0, lenAny = 0;
    bool foundCjk = false, foundAny = false, sawCjk = false;
    for (size_t k = 0; k < segs.size(); ++k) {
        int cjk = SegCjkCount(s, segs[k]);
        if (cjk > 0) sawCjk = true;
        int cnt = SegTextCount(s, segs[k]);
        if (cnt < 2) continue;
        size_t len = segs[k].e - segs[k].b;
        if (cjk >= 2) {
            if (!foundCjk || cjk > bestCjk || (cjk == bestCjk && len > lenCjk)) {
                foundCjk = true;
                pickCjk = k;
                bestCjk = cjk;
                lenCjk = len;
            }
        } else if (!foundAny || cnt > bestAny || (cnt == bestAny && len > lenAny)) {
            foundAny = true;
            pickAny = k;
            bestAny = cnt;
            lenAny = len;
        }
    }
    if (!foundCjk && sawCjk) return std::wstring();   // 有汉字但不足 2 个：不退回英文
    if (!foundCjk && !foundAny) return std::wstring();
    size_t best = foundCjk ? pickCjk : pickAny;

    size_t b = segs[best].b, e = segs[best].e;

    while (b < e && (s[b] == L' ' || s[b] == L'\t')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t')) --e;

    if (e <= b) return std::wstring();
    std::wstring out = s.substr(b, e - b);
    if (CountTextChars(out) < 1) return std::wstring();
    if (outBegin) *outBegin = i0 + b;
    if (outEnd) *outEnd = i0 + e;
    if (haveX && firstLeft) {
        // 距离门槛看“鼠标右侧第一个字”，不是最后选中的长段：
        // 只要这一行在鼠标右侧确实有文字，就可以在这行里挑最长的一段。
        size_t i = i0;
        double half;
        if (i + 1 < raw.size() && charX[i + 1] > charX[i]) {
            half = (charX[i + 1] - charX[i]) * 0.5;
        } else if (i > 0 && charX[i] > charX[i - 1]) {
            half = (charX[i] - charX[i - 1]) * 0.5;
        } else {
            half = 8.0;
        }
        if (half < 2.0) half = 8.0;
        *firstLeft = charX[i] - half;
    }
    return out;
}

// 字符重合度：只看「有哪些字、各出现几次」，不看顺序。
// UIA 给的是源文本（字对、位置准），图片给的是字形（位置准、可能认错字），
// 用它判断两者说的是不是同一句。
double TextSimilarity(const std::wstring& a, const std::wstring& b) {
    std::vector<wchar_t> ka, kb;
    ka.reserve(a.size());
    kb.reserve(b.size());
    for (size_t i = 0; i < a.size(); ++i) if (!iswspace(a[i])) ka.push_back(a[i]);
    for (size_t i = 0; i < b.size(); ++i) if (!iswspace(b[i])) kb.push_back(b[i]);
    if (ka.empty() || kb.empty()) return 0.0;
    std::sort(ka.begin(), ka.end());
    std::sort(kb.begin(), kb.end());

    size_t i = 0, j = 0, common = 0;
    while (i < ka.size() && j < kb.size()) {
        if (ka[i] == kb[j]) { ++common; ++i; ++j; }
        else if (ka[i] < kb[j]) ++i;
        else ++j;
    }
    size_t m = ka.size() > kb.size() ? ka.size() : kb.size();
    return (double)common / (double)m;
}

// 顺序敏感的重合度：LCS / 较长者长度。用于智能模式判断 UIA 与 OCR
// 是不是同一句——只用字符频次时，“闭塞设备”和“设备闭塞”会被判成一样。
double TextSimilarityOrdered(const std::wstring& a, const std::wstring& b) {
    std::wstring x, y;
    x.reserve(a.size());
    y.reserve(b.size());
    for (wchar_t c : a) if (!iswspace(c)) x.push_back(c);
    for (wchar_t c : b) if (!iswspace(c)) y.push_back(c);
    if (x.empty() || y.empty()) return 0.0;
    if (x.size() > 600 || y.size() > 600 || x.size() * y.size() > 250000) {
        return TextSimilarity(a, b);
    }
    if (!x.empty()) CharUpperBuffW(&x[0], (DWORD)x.size());
    if (!y.empty()) CharUpperBuffW(&y[0], (DWORD)y.size());

    const size_t n = x.size(), m = y.size();
    std::vector<int> prev(m + 1, 0), cur(m + 1, 0);
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = 0;
        for (size_t j = 1; j <= m; ++j) {
            if (x[i - 1] == y[j - 1]) cur[j] = prev[j - 1] + 1;
            else cur[j] = (prev[j] > cur[j - 1]) ? prev[j] : cur[j - 1];
        }
        prev.swap(cur);
    }
    size_t lcs = (size_t)prev[m];
    size_t big = (n > m) ? n : m;
    return (double)lcs / (double)big;
}

int CountTextChars(const std::wstring& s) {
    if (s.empty()) return 0;
    std::vector<CharKind> kind;
    Classify(s, kind);
    int n = 0;
    for (size_t i = 0; i < s.size(); ++i) if (kind[i].text) ++n;
    return n;
}
