// test_cache.cpp - 缓存去重 + 磁盘配额淘汰
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

static int CountCacheFiles() {
    std::wstring pat = JoinPath(CacheDir(), L"*.cache");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++n; }
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static unsigned long long TotalCacheBytes() {
    std::wstring pat = JoinPath(CacheDir(), L"*.cache");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    unsigned long long t = 0;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
             t += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; }
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return t;
}

// 造一个大 cache 文件并设定写入时间（FILETIME 每 100ns 一格）
static void MakeFake(const wchar_t* name, unsigned long long bytes, unsigned long long ft) {
    std::wstring p = JoinPath(CacheDir(), name);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { printf("  cannot create fake %ls\n", name); return; }
    // 头部写个假 key 行，让真实读盘时校验失败即可
    const char* hdr = "fake|key|line\n";
    DWORD w = 0;
    WriteFile(h, hdr, (DWORD)strlen(hdr), &w, nullptr);
    if (bytes > 16) SetFilePointer(h, (LONG)(bytes - 16), nullptr, FILE_BEGIN);
    WriteFile(h, "x", 1, &w, nullptr);
    FILETIME f{ (DWORD)(ft & 0xffffffffULL), (DWORD)(ft >> 32) };
    SetFileTime(h, &f, &f, &f);
    CloseHandle(h);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    printf("cache dir: %ls\n", CacheDir().c_str());
    // 生产环境由 bundle 负责建目录，测试里自己建
    CreateDirectoryW(ConfigDirPath().c_str(), nullptr);
    CreateDirectoryW(CacheDir().c_str(), nullptr);
    CacheClearAll();
    printf("\n-- 1) 同一文档改 mtime：应覆盖同一份缓存文件，不产生第二份 --\n");
    {
        std::wstring doc = L"C:\\tmp\\测试文档.docx";
        std::vector<std::wstring> v1{ L"第一版内容" };
        std::vector<std::wstring> v2{ L"第二版内容", L"多了一行" };
        CachePut(doc, 100, 111111, v1);
        int n1 = CountCacheFiles();
        CachePut(doc, 101, 222222, v2);          // 大小和 mtime 都变了
        int n2 = CountCacheFiles();
        CHECK(n1 == 1);
        CHECK(n2 == 1);                           // 同名覆盖，不是新增

        std::vector<std::wstring> got;
        CHECK(CacheGet(doc, 101, 222222, got));   // 新版本命中
        CHECK(got.size() == 2 && got[0] == L"第二版内容");

        // 把内存 LRU（120 条）挤空，强制下一查走磁盘：旧版本应失效
        for (int i = 0; i < 130; ++i) {
            wchar_t p[64];
            swprintf(p, 64, L"C:\\tmp\\fill\\f%d.txt", i);
            CachePut(p, (unsigned)i, (unsigned long long)i, { L"fill" });
        }
        CHECK(!CacheGet(doc, 100, 111111, got));  // 磁盘上已只剩新版，旧版本失效
        CHECK( CacheGet(doc, 101, 222222, got));  // 新版从磁盘仍可读回
    }

    printf("\n-- 2) 不同文档：各自一份 --\n");
    {
        CachePut(L"C:\\tmp\\a.txt", 1, 1, { L"aaa" });
        CachePut(L"C:\\tmp\\b.txt", 2, 2, { L"bbb" });
        CHECK(CountCacheFiles() == 133);          // 130 条填充 + 文档 + a + b
        std::vector<std::wstring> got;
        CHECK(CacheGet(L"C:\\tmp\\b.txt", 2, 2, got) && got[0] == L"bbb");
    }

    printf("\n-- 3) 超过 64MB：按写入时间从旧到新淘汰 --\n");
    {
        // 造三份假旧文件：写入时间 1000 < 2000 < 3000，共 70MB
        MakeFake(L"old1.cache", 40ULL * 1024 * 1024, 1000);
        MakeFake(L"old2.cache", 20ULL * 1024 * 1024, 2000);
        MakeFake(L"old3.cache", 10ULL * 1024 * 1024, 3000);
        unsigned long long before = TotalCacheBytes();
        printf("  before = %.1f MB\n", before / 1048576.0);
        CHECK(before > 64ULL * 1024 * 1024);

        CachePut(L"C:\\tmp\\c.txt", 3, 3, { L"ccc" });   // 触发配额检查

        unsigned long long after = TotalCacheBytes();
        printf("  after  = %.1f MB\n", after / 1048576.0);
        CHECK(after <= 51ULL * 1024 * 1024);
        CHECK(!FileExists(JoinPath(CacheDir(), L"old1.cache")));  // 最旧先删
        CHECK( FileExists(JoinPath(CacheDir(), L"old2.cache")));  // 降到低水位就停
        CHECK( FileExists(JoinPath(CacheDir(), L"old3.cache")));
        std::vector<std::wstring> got;
        CHECK(CacheGet(L"C:\\tmp\\c.txt", 3, 3, got));            // 新缓存还在
    }

    printf("\n-- 4) 上限可调：调到 128MB 后 70MB 不再淘汰 --\n");
    {
        CacheClearAll();
        MakeFake(L"q1.cache", 40ULL * 1024 * 1024, 4000);
        MakeFake(L"q2.cache", 20ULL * 1024 * 1024, 5000);
        MakeFake(L"q3.cache", 10ULL * 1024 * 1024, 6000);
        CacheSetLimitMB(128);
        CachePut(L"C:\\tmp\\d.txt", 4, 4, { L"ddd" });            // 触发配额检查
        printf("  at 128MB limit = %.1f MB\n", TotalCacheBytes() / 1048576.0);
        CHECK(TotalCacheBytes() > 64ULL * 1024 * 1024);           // 上限放宽，不该淘汰
        CHECK(FileExists(JoinPath(CacheDir(), L"q1.cache")));

        CacheSetLimitMB(64);                                      // 收回到 64MB
        CachePut(L"C:\\tmp\\e.txt", 5, 5, { L"eee" });
        printf("  back at 64MB  = %.1f MB\n", TotalCacheBytes() / 1048576.0);
        CHECK(TotalCacheBytes() <= 64ULL * 1024 * 1024);
        CHECK(!FileExists(JoinPath(CacheDir(), L"q1.cache")));     // 最旧的先走
    }

    CacheClearAll();
    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
