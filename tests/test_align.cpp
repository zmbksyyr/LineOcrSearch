// test_align.cpp - 对齐模块单测：OCR 的一行 -> UIA 全文里的对应片段
#include "../src/common.h"
#include <cstdio>
#include <string>

static int g_fail = 0;

static void Check(bool cond, const char* what) {
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fail;
}

static std::string U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// 构造一段"文档"：前面几段干扰 + 目标段落
static std::wstring MakeDoc() {
    std::wstring d;
    d += L"行取词搜索 · 使用说明\n";
    d += L"把文件夹拖进窗口就能设定搜索范围。\n";
    d += L"设置里可以调鼠标停留多久自动取词。\n";
    // 目标段落（两行折行的长段落）
    d += L"如果 exe 放在不可写目录（比如 Program Files），释放失败会在状态栏写明"
         L"「config 目录不可写」，此时模型识别自动降级，控件取词和手动搜索照常可用，不会崩。\n";
    d += L"下面是另一段，用来验证不会串到后面去。\n";
    d += L"这段里也出现了「自动降级」这个词，但位置不同。\n";
    return d;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    const std::wstring doc = MakeDoc();

    printf("-- 完全一致 --\n");
    {
        // 取段落中的一小段（模拟 OCR 只截到这一行的一部分）
        std::wstring line = L"此时模型识别自动降级，控件取词和手动搜索照常可用";
        AlignResult ar = AlignLineToDocument(doc, line, 0.40);
        Check(ar.ok, "aligned");
        Check(ar.ratio > 0.99, "ratio ~ 1.0");
        std::wstring got = doc.substr(ar.hayStart, ar.hayEnd - ar.hayStart);
        Check(got == line, "window text equals the line");
    }

    printf("-- OCR 认错几个字 --\n");
    {
        // 「释放失败会在状态栏写明」里错了 3 个字（模型、控、降）
        std::wstring line = L"释放失误会在状态栏写明「config 目录不可写」，此时模形识别自动降极";
        AlignResult ar = AlignLineToDocument(doc, line, 0.40);
        Check(ar.ok, "aligned despite OCR errors");
        Check(ar.ratio > 0.80 && ar.ratio < 1.0, "ratio between 0.8 and 1.0");
        std::wstring got = doc.substr(ar.hayStart, ar.hayEnd - ar.hayStart);
        // 对齐后拿到的应是文档里的正确文字（不是 OCR 的错字）
        Check(got.find(L"释放失败") != std::wstring::npos, "window uses document text, not OCR text");
        Check(got.find(L"释放失误") == std::wstring::npos, "OCR typo not present in window");
    }

    printf("-- 重复内容取第一处 --\n");
    {
        std::wstring line = L"自动降级";
        AlignResult ar = AlignLineToDocument(doc, line, 0.40);
        Check(ar.ok, "aligned short repeated phrase");
        std::wstring got = doc.substr(ar.hayStart, ar.hayEnd - ar.hayStart);
        Check(got == line, "window text correct");
    }

    printf("-- 完全对不上 --\n");
    {
        std::wstring line = L"这是一句文档里根本不存在的话";
        AlignResult ar = AlignLineToDocument(doc, line, 0.40);
        Check(!ar.ok, "rejected when nothing matches");
    }

    printf("-- 位置映射 --\n");
    {
        std::wstring line = L"此时模型识别自动降级，控件取词和手动搜索照常可用";
        AlignResult ar = AlignLineToDocument(doc, line, 0.40);
        Check(ar.ok, "aligned for mapping test");

        // 假装 OCR 给的每个字位置：起点 100，字距 14
        std::vector<double> lineX;
        for (size_t i = 0; i < line.size(); ++i) lineX.push_back(100.0 + 14.0 * (double)i);

        std::wstring wt;
        std::vector<double> wx;
        BuildAlignedLine(doc, ar, lineX, wt, wx);
        Check(wt == line, "window text");
        Check(wx.size() == wt.size(), "one x per char");
        Check(wx.front() >= 99.0 && wx.front() <= 101.0, "first x at 100");

        bool mono = true;
        for (size_t i = 1; i < wx.size(); ++i) if (wx[i] <= wx[i - 1]) mono = false;
        Check(mono, "x is monotonically increasing");

        // 鼠标落在「自动降级」的「自」上：取到的关键词应是含「自动降级」的一段
        size_t pos = wt.find(L"自动降级");
        Check(pos != std::wstring::npos, "keyword exists in window");
        double mx = wx[pos] + 1.0;
        std::wstring picked = CleanOcrTextByX(wt, wx, mx);
        printf("       picked = %s\n", U8(picked).c_str());
        Check(picked.find(L"自动降级") != std::wstring::npos, "picked segment contains keyword");
    }

    printf("-- 长文档种子定位 --\n");
    {
        std::wstring big;
        for (int i = 0; i < 900; ++i) big += L"填充行内容用来把文档撑长，第若干行。\n";  // ~900*20 = 18000 字
        std::wstring target = L"目标句子里包含这些字，用来验证长文定位";
        big += target;
        for (int i = 0; i < 900; ++i) big += L"再来一些填充内容把尾巴也撑长。\n";
        AlignResult ar = AlignLineToDocument(big, target, 0.40);
        Check(ar.ok, "aligned in long document");
        std::wstring got = big.substr(ar.hayStart, ar.hayEnd - ar.hayStart);
        Check(got == target, "window text correct in long doc");
    }

    printf("----\n%s: %d failure(s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
