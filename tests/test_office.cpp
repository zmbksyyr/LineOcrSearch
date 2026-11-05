// test_office.cpp - docx / xlsx 文本抽取测试
#include "../src/common.h"
#include <cstdio>

static std::string U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    const wchar_t* files[] = { L".deps\\test.docx", L".deps\\test.xlsx" };
    int fail = 0;
    for (int i = 0; i < 2; ++i) {
        std::wstring text;
        bool ok = ExtractOfficeText(files[i], text);
        printf("== %s : %s (%d chars)\n", U8(files[i]).c_str(), ok ? "OK" : "FAIL", (int)text.size());
        printf("%s\n", U8(text).c_str());
        if (!ok) ++fail;
    }
    return fail ? 1 : 0;
}
