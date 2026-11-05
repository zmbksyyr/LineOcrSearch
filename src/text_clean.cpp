// text_clean.cpp - OCR 结果清洗：只留纯文字，剥除首尾符号
#include "common.h"

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

// 段内有效文字数（不含空格），不足 2 个的段不能当关键词
static int SegTextCount(const std::wstring& raw, const Seg& s) {
    int n = 0;
    for (size_t i = s.b; i < s.e; ++i) if (IsHardText(raw[i])) ++n;
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

// 没有鼠标位置：取最长的一段
std::wstring CleanOcrText(const std::wstring& raw) {
    if (raw.empty()) return std::wstring();
    std::vector<Seg> segs;
    SplitSegments(raw, segs);

    size_t pick = 0;
    size_t bestLen = 0;
    bool found = false;
    for (size_t k = 0; k < segs.size(); ++k) {
        if (SegTextCount(raw, segs[k]) < 2) continue;
        size_t len = segs[k].e - segs[k].b;
        if (!found || len > bestLen) { bestLen = len; pick = k; found = true; }
    }
    if (!found) return std::wstring();
    return SegText(raw, segs[pick]);
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

int CountTextChars(const std::wstring& s) {
    if (s.empty()) return 0;
    std::vector<CharKind> kind;
    Classify(s, kind);
    int n = 0;
    for (size_t i = 0; i < s.size(); ++i) if (kind[i].text) ++n;
    return n;
}
