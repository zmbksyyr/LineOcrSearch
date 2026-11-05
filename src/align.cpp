// align.cpp - 把 OCR 认出来的「一行」对齐到 UIA 给出的「整篇文本」
//
// 目的：位置必须来自真实像素（UIA 在 Chromium 上给的矩形是块级/窗口级的，没法定位），
//       但文字要用 UIA 原文（OCR 会认错字，认错的字拿去搜根本搜不到）。
// 做法：OCR 行（短）+ doc 全文（长）做滑窗比相似度，找到对应片段后逐字对应，
//       把 OCR 每个字的像素 x 搬到 doc 的字符上，得到「精确文字 + 真实位置」。
#include "common.h"

namespace {

bool IsBlank(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' ||
           c == 0x3000 || c == 0x00A0;
}

void StripBlanks(const std::wstring& s, std::wstring& out, std::vector<int>& back) {
    out.clear();
    back.clear();
    out.reserve(s.size());
    back.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (IsBlank(s[i])) continue;
        out.push_back(s[i]);
        back.push_back((int)i);
    }
}

int CountMatch(const std::wstring& hay, const std::wstring& needle, size_t pos, size_t len) {
    int m = 0;
    for (size_t i = 0; i < len; ++i) {
        if (hay[pos + i] == needle[i]) ++m;
    }
    return m;
}

// 长文档时的种子定位：拿 needle 里几段 8 字原文去 doc 里找，只在命中处附近评估
void SeedSearch(const std::wstring& hay, const std::wstring& needle,
                size_t& bestP, int& bestM) {
    const size_t K = 8;
    const size_t M = needle.size();
    if (M < K) return;

    size_t probes[5];
    probes[0] = 0;
    probes[1] = M / 3;
    probes[2] = M / 2;
    probes[3] = (M * 2) / 3;
    probes[4] = M - K;

    for (int pi = 0; pi < 5; ++pi) {
        std::wstring seed = needle.substr(probes[pi], K);
        size_t q = hay.find(seed);
        int tries = 0;
        while (q != std::wstring::npos && tries < 24) {
            long long start = (long long)q - (long long)probes[pi];
            if (start >= 0 && (size_t)start + M <= hay.size()) {
                int m = CountMatch(hay, needle, (size_t)start, M);
                if (m > bestM) { bestM = m; bestP = (size_t)start; }
            }
            q = hay.find(seed, q + 1);
            ++tries;
        }
    }
}

// 把 -1 的空洞按已知点线性补齐（两端外推），让每个字都有个合理的像素位置
void FillGaps(std::vector<double>& xs, double defaultStep) {
    size_t n = xs.size();
    if (n == 0) return;

    size_t first = n;
    for (size_t i = 0; i < n; ++i) {
        if (xs[i] >= 0.0) { first = i; break; }
    }
    if (first == n) {
        for (size_t i = 0; i < n; ++i) xs[i] = defaultStep * (double)i;
        return;
    }

    // 估一个典型字距
    double step = defaultStep;
    {
        int pairs = 0;
        double sum = 0.0;
        size_t prev = first;
        for (size_t i = first + 1; i < n; ++i) {
            if (xs[i] < 0.0) continue;
            double d = (xs[i] - xs[prev]) / (double)(i - prev);
            if (d > 1.0 && d < 200.0) { sum += d; ++pairs; }
            prev = i;
        }
        if (pairs > 0) step = sum / (double)pairs;
    }

    for (size_t i = first; i-- > 0;) xs[i] = xs[i + 1] - step;

    size_t prev = first;
    for (size_t i = first + 1; i < n; ++i) {
        if (xs[i] < 0.0) continue;
        if (i > prev + 1) {
            double s = (xs[i] - xs[prev]) / (double)(i - prev);
            for (size_t k = prev + 1; k < i; ++k) xs[k] = xs[prev] + s * (double)(k - prev);
        }
        prev = i;
    }
    for (size_t i = prev + 1; i < n; ++i) xs[i] = xs[prev] + step * (double)(i - prev);
}

}  // namespace

AlignResult AlignLineToDocument(const std::wstring& doc, const std::wstring& line,
                                double minRatio) {
    AlignResult res;

    std::wstring needle, hay;
    std::vector<int> nBack, hBack;
    StripBlanks(line, needle, nBack);
    StripBlanks(doc, hay, hBack);

    const size_t M = needle.size();
    const size_t N = hay.size();
    if (M < 3 || N < 6) return res;

    // 在偏移 o（needle[0] 对应 hay[o]，可为负）下，统计重叠区的相同字数。
    // 允许两边长度不等：OCR 多认一个符号（比如行首的「」）时不会整体判失败。
    auto score = [&](long long o, int& overlap) -> int {
        long long s = o < 0 ? 0 : o;
        long long e = (long long)N;
        if (o + (long long)M < e) e = o + (long long)M;
        if (e <= s) { overlap = 0; return 0; }
        overlap = (int)(e - s);
        int m = 0;
        for (long long i = s; i < e; ++i) {
            if (hay[(size_t)i] == needle[(size_t)(i - o)]) ++m;
        }
        return m;
    };

    double bestScore = -1.0;
    long long bestOff = 0;
    int bestMatches = 0;

    const int need = (M < 6) ? (int)M : 6;   // 至少要比对这么多字才算数

    if (N <= 60000) {
        for (long long o = -(long long)(M - (size_t)need); o <= (long long)N - need; ++o) {
            int ov = 0;
            int m = score(o, ov);
            if (ov < need) continue;
            double denom = (double)((long long)M > ov ? (long long)M : ov);
            double r = (double)m / denom;
            if (r > bestScore) {
                bestScore = r;
                bestOff = o;
                bestMatches = m;
                if (m == (int)M && ov == (int)M) break;
            }
        }
    } else {
        size_t sp = 0;
        int sm = -1;
        SeedSearch(hay, needle, sp, sm);
        if (sm > 0) {
            for (int d = -2; d <= 2; ++d) {
                long long o = (long long)sp + d;
                int ov = 0;
                int m = score(o, ov);
                if (ov < need) continue;
                double denom = (double)((long long)M > ov ? (long long)M : ov);
                double r = (double)m / denom;
                if (r > bestScore) { bestScore = r; bestOff = o; bestMatches = m; }
            }
        }
    }
    if (bestScore <= 0.0 || bestMatches <= 0) return res;
    if (bestScore < minRatio) return res;

    // 对应的 hay 区间（夹在有效范围内）
    long long hs = bestOff < 0 ? 0 : bestOff;
    long long he = bestOff + (long long)M;
    if (he > (long long)N) he = (long long)N;
    if (hs >= he) return res;

    res.ok = true;
    res.ratio = bestScore;
    res.needleToDoc.assign(line.size(), -1);

    for (size_t i = 0; i < M && i < needle.size(); ++i) {
        long long hi = bestOff + (long long)i;
        if (hi < 0 || hi >= (long long)N) continue;
        int docIndex = hBack[(size_t)hi];
        if ((size_t)nBack[i] < res.needleToDoc.size()) {
            res.needleToDoc[(size_t)nBack[i]] = docIndex;
        }
    }

    res.hayStart = (size_t)hBack[(size_t)hs];
    res.hayEnd = (size_t)hBack[(size_t)(he - 1)] + 1;
    return res;
}

void BuildAlignedLine(const std::wstring& doc, const AlignResult& ar,
                      const std::vector<double>& lineCharX,
                      std::wstring& outText, std::vector<double>& outCharX) {
    outText.clear();
    outCharX.clear();
    if (!ar.ok || ar.hayStart >= ar.hayEnd || ar.hayEnd > doc.size()) return;

    outText = doc.substr(ar.hayStart, ar.hayEnd - ar.hayStart);
    outCharX.assign(outText.size(), -1.0);

    for (size_t i = 0; i < ar.needleToDoc.size() && i < lineCharX.size(); ++i) {
        int j = ar.needleToDoc[i];
        if (j < 0) continue;
        if ((size_t)j < ar.hayStart || (size_t)j >= ar.hayEnd) continue;
        outCharX[(size_t)j - ar.hayStart] = lineCharX[i];
    }

    FillGaps(outCharX, 8.0);
}
