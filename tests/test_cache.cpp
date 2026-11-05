// test_cache.cpp - 缓存：单库去重 / 版本失效 / 配额压缩
// 用法：把 exe 放到任意目录运行，它会在旁边的数据目录里建 cache\ 做实验，不动别处。
#include "../src/common.h"
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (cond) { printf("  PASS  %s\n", #cond); } \
    else { printf("  FAIL  %s  (line %d)\n", #cond, __LINE__); ++g_fail; } \
} while (0)

static std::wstring CacheDir() { return SubDirPath(cfgpaths::kCacheDir); }

static int CountFiles() {
    std::wstring pat = JoinPath(CacheDir(), L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++n; }
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

// 造一份大约 rows * chars 字节的正文
static std::vector<std::wstring> BigText(int rows, int chars) {
    std::wstring line((size_t)chars, L'x');
    std::vector<std::wstring> v;
    v.reserve((size_t)rows);
    for (int i = 0; i < rows; ++i) v.push_back(line);
    return v;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    printf("cache dir: %ls\n", CacheDir().c_str());
    // 生产环境由 bundle 负责建目录，测试里自己建
    CreateDirectoryW(ConfigDirPath().c_str(), nullptr);
    CreateDirectoryW(CacheDir().c_str(), nullptr);
    CacheClearAll();
    printf("  after clear: %d file(s), %llu bytes\n", CountFiles(), CacheUsedBytes());

    printf("\n-- 1) 同一文档改 mtime：只保留最新一条，旧版本失效 --\n");
    {
        std::wstring doc = L"C:\\tmp\\测试文档.docx";
        std::vector<std::wstring> v1{ L"第一版内容" };
        std::vector<std::wstring> v2{ L"第二版内容", L"多了一行" };
        CachePut(doc, 100, 111111, v1);
        CachePut(doc, 101, 222222, v2);

        std::vector<std::wstring> got;
        CHECK(CacheGet(doc, 101, 222222, got));
        CHECK(got.size() == 2 && got[0] == L"第二版内容");

        // 把内存 LRU（120 条）挤空，强制下一查走存储库
        for (int i = 0; i < 130; ++i) {
            wchar_t p[64];
            swprintf(p, 64, L"C:\\tmp\\fill\\f%d.txt", i);
            CachePut(p, (unsigned)i, (unsigned long long)i, { L"fill" });
        }
        CHECK(!CacheGet(doc, 100, 111111, got));   // 旧 key 已经不在索引里
        CHECK( CacheGet(doc, 101, 222222, got));   // 新版仍可读回
    }

    printf("\n-- 2) 所有缓存共用一个存储文件 --\n");
    {
        CachePut(L"C:\\tmp\\a.txt", 1, 1, { L"aaa" });
        CachePut(L"C:\\tmp\\b.txt", 2, 2, { L"bbb" });
        CHECK(CountFiles() == 1);                  // 只剩 store.bin
        CHECK(FileExists(JoinPath(CacheDir(), L"store.bin")));
        std::vector<std::wstring> got;
        CHECK(CacheGet(L"C:\\tmp\\b.txt", 2, 2, got) && got[0] == L"bbb");
    }

    printf("\n-- 3) 超过上限：按写入顺序从旧到新淘汰 --\n");
    {
        CacheClearAll();
        CacheSetLimitMB(16);
        std::vector<std::wstring> big1 = BigText(3000, 2000);   // 约 6MB
        std::vector<std::wstring> big2 = BigText(3000, 2000);
        std::vector<std::wstring> big3 = BigText(3000, 2000);
        CachePut(L"C:\\tmp\\big1.txt", 10, 1000, big1);
        CachePut(L"C:\\tmp\\big2.txt", 11, 2000, big2);
        unsigned long long beforeThird = CacheUsedBytes();
        CachePut(L"C:\\tmp\\big3.txt", 12, 3000, big3);          // 写这条时超限，触发压缩
        unsigned long long after = CacheUsedBytes();
        printf("  used: %.1f MB -> %.1f MB (limit 16MB)\n",
               beforeThird / 1048576.0, after / 1048576.0);
        CHECK(beforeThird > 10ULL * 1024 * 1024);
        CHECK(after <= 16ULL * 1024 * 1024);

        std::vector<std::wstring> got;
        CHECK(!CacheGet(L"C:\\tmp\\big1.txt", 10, 1000, got));   // 最旧的被丢掉
        CHECK( CacheGet(L"C:\\tmp\\big3.txt", 12, 3000, got));   // 新写的还在
    }

    printf("\n-- 4) 上限调大后不再淘汰 --\n");
    {
        CacheSetLimitMB(128);
        std::vector<std::wstring> big4 = BigText(3000, 2000);
        CachePut(L"C:\\tmp\\big4.txt", 13, 4000, big4);
        unsigned long long used = CacheUsedBytes();
        printf("  used at 128MB limit: %.1f MB\n", used / 1048576.0);
        CHECK(used > 16ULL * 1024 * 1024);
        std::vector<std::wstring> got;
        CHECK(CacheGet(L"C:\\tmp\\big3.txt", 12, 3000, got));
        CHECK(CacheGet(L"C:\\tmp\\big4.txt", 13, 4000, got));

        CacheSetLimitMB(64);
    }

    CacheClearAll();
    printf("  after final clear: %d file(s)\n", CountFiles());
    CHECK(CountFiles() == 0);

    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
