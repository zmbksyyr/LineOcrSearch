// test_office_fmt.cpp - 新格式文档抽取的结构性回归
//   docx：表格单元格之间必须有分隔（否则跨列误命中）、修订删除的文字要收进正文
//   xlsx：共享字符串类型不能只认字面量 t="s"（单引号 / 等号两侧空格也要认）
// 样本在 tests/data/ 下，避免运行时依赖 Office 环境
#include "../src/common.h"
#include <cstdio>

static int g_fail = 0;

static void Check(bool cond, const char* name) {
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) ++g_fail;
}

// 逐行看：某一行是否正好等于 expected
static bool HasLine(const std::wstring& text, const std::wstring& expected) {
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find(L'\n', p);
        std::wstring line = (e == std::wstring::npos) ? text.substr(p) : text.substr(p, e - p);
        if (line == expected) return true;
        if (e == std::wstring::npos) break;
        p = e + 1;
    }
    return false;
}

static bool Has(const std::wstring& text, const std::wstring& sub) {
    return text.find(sub) != std::wstring::npos;
}

int main() {
    std::wstring text;

    printf("-- docx table / revision --\n");
    bool ok = ExtractOfficeText(L"tests\\data\\fmt_table.docx", text);
    Check(ok, "docx extracted");
    Check(HasLine(text, L"闭塞\t区段\t办理"), "table row kept as one line with tab separators");
    Check(!Has(text, L"闭塞区段"), "cells are not glued together");
    Check(Has(text, L"已删除的说法甲"), "deleted (revision) text included");
    Check(Has(text, L"保留的说法乙"), "normal text still included");
    Check(HasLine(text, L"第一段 闭塞"), "paragraph before table intact");

    printf("-- xlsx shared string attributes --\n");
    std::wstring sx;
    ok = ExtractOfficeText(L"tests\\data\\fmt_quote.xlsx", sx);
    Check(ok, "xlsx extracted");
    Check(HasLine(sx, L"闭塞\t区段办理\t42"), "single-quoted and spaced t= both resolve to shared strings");
    Check(!Has(sx, L"0\t1"), "shared string index is not emitted as a number");

    if (g_fail == 0) printf("\nALL PASSED (0 failed)\n");
    else printf("\nFAILED: %d failure(s)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
