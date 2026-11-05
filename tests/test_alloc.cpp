// test_alloc.cpp - 多文件命中的「显示名额分配」：
// 旧逻辑按遍历顺序收集、凑满 maxResults 就停，一个大文件能把名额吃光，
// 其它文件的命中完全看不到。这里锁住新行为：保底 1 条 + 命中多的多分 + 按文件分组。
#include "../src/common.h"
#include <cstdio>

static int g_fail = 0;

static void Check(bool cond, const char* name) {
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) ++g_fail;
}

static void WriteUtf8(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], n, nullptr, nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &wr, nullptr);
    CloseHandle(h);
}

// 造一个含 hits 条命中的文本文件（每行一条，行号 = 1..hits）
static void MakeFile(const std::wstring& dir, const wchar_t* name, int hits) {
    std::wstring text;
    for (int i = 1; i <= hits; ++i) {
        text += L"第 ";
        text += std::to_wstring(i);
        text += L" 行 锚点词 出现\n";
    }
    WriteUtf8(JoinPath(dir, name), text);
}

static int CountOf(const std::vector<ResultItem>& v, const wchar_t* name) {
    int n = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].fileName == name) ++n;
    }
    return n;
}

int main() {
    std::wstring root = L"tests";
    std::wstring dir  = L"tests\\tmp\\alloc";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(L"tests\\tmp", nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);

    MakeFile(dir, L"a.txt", 30);
    MakeFile(dir, L"b.txt", 3);
    MakeFile(dir, L"c.txt", 1);

    SearchRequest req;
    req.keyword = L"锚点词";
    req.rootDir = dir;
    req.contextLevel = 0;      // 只要命中行，测分配逻辑
    req.searchOffice = false;

    // ---- 名额 10，命中 34 ----
    req.maxResults = 10;
    SearchOutcome o = RunSearch(req);

    printf("-- 名额 10 / 命中 34 --\n");
    Check(o.totalFiles == 3, "totalFiles = 3");
    Check(o.totalHits == 34, "totalHits = 34");
    Check(o.items.size() == 10, "items = maxResults = 10");
    Check(o.pending.size() == 24, "pending = 34 - 10");
    Check(!o.truncated, "not truncated");

    // 水位分配：a 30、b 3、c 1，10 个名额 -> 6 / 3 / 1
    Check(CountOf(o.items, L"a.txt") == 6, "big file gets 6");
    Check(CountOf(o.items, L"b.txt") == 3, "mid file gets all 3");
    Check(CountOf(o.items, L"c.txt") == 1, "small file still gets its 1 (保底)");

    // 组间按命中数降序，组内按行号升序
    Check(!o.items.empty() && o.items[0].fileName == L"a.txt", "biggest file group first");
    Check(o.items.size() >= 10 && o.items[6].fileName == L"b.txt", "b group starts at 6");
    Check(o.items.size() >= 10 && o.items[9].fileName == L"c.txt", "c group starts at 9");
    Check(o.items[0].lineNo == 1 && o.items[1].lineNo == 2, "lines ascending inside group");
    Check(!o.pending.empty() && o.pending[0].fileName == L"a.txt" && o.pending[0].lineNo == 7,
          "pending continues where the group stopped");

    // ---- 名额 4（比文件数还紧张时也不能只喂一个文件）----
    req.maxResults = 4;
    SearchOutcome o2 = RunSearch(req);
    printf("-- 名额 4 / 命中 34 --\n");
    Check(o2.items.size() == 4, "items = 4");
    Check(CountOf(o2.items, L"a.txt") == 2 && CountOf(o2.items, L"b.txt") == 1 &&
          CountOf(o2.items, L"c.txt") == 1, "water-filling: 2 / 1 / 1");
    Check(o2.pending.size() == 30, "pending = 30");

    // ---- 名额足够：全部显示，无 pending ----
    req.maxResults = 1000;
    SearchOutcome o3 = RunSearch(req);
    printf("-- 名额 1000 / 命中 34 --\n");
    Check(o3.items.size() == 34, "all hits shown");
    Check(o3.pending.empty(), "no pending");
    Check(!o3.truncated, "not truncated");

    printf(g_fail ? "\nFAILED (%d)\n" : "\nALL PASSED (0 failed)\n", g_fail);
    return g_fail ? 1 : 0;
}
