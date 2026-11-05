// test_cachebuild.cpp - 目录预建缓存：正常建 / 复用 / 无支持文件 / 中途停止
// 用法：exe 放到任意目录运行，自己在临时目录里造样本，不碰别处
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

// 缓存改成了单库（cache\store.bin），不能再数「每文档一个 .cache」；
// 直接问 API：这份文件在缓存里吗
static bool Cached(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return false;
    unsigned size = (unsigned)fad.nFileSizeLow;
    unsigned long long mtime =
        ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) |
        fad.ftLastWriteTime.dwLowDateTime;
    return CacheHas(p, size, mtime);
}

static void WriteBytes(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr);
    CloseHandle(h);
}

static void RemoveTree(const std::wstring& dir) {
    std::wstring pat = JoinPath(dir, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' ||
             (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) continue;
        std::wstring full = JoinPath(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(full);
        else DeleteFileW(full.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir.c_str());
}

static bool CancelsAfterFirst(void*, const CacheBuildStats&) { return false; }

int main() {
    SetConsoleOutputCP(CP_UTF8);
    CreateDirectoryW(ConfigDirPath().c_str(), nullptr);
    CreateDirectoryW(CacheDir().c_str(), nullptr);
    CacheClearAll();

    wchar_t tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring root = JoinPath(tmp, L"los_cachebuild_test");
    RemoveTree(root);
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(JoinPath(root, L"sub").c_str(), nullptr);
    CreateDirectoryW(JoinPath(root, L"empty").c_str(), nullptr);

    WriteBytes(JoinPath(root, L"a.txt"), "订单编号 A-1024\n客户名称 测试\n");
    WriteBytes(JoinPath(root, L"b.md"), "# 标题\n正文内容\n");
    WriteBytes(JoinPath(root, L"sub\\e.csv"), "name,qty\napple,3\n");
    WriteBytes(JoinPath(root, L"c.docx"), "this is not a zip archive at all\n");  // 解析必失败
    WriteBytes(JoinPath(root, L"d.bin"), "unsupported type\n");
    WriteBytes(JoinPath(root, L"big.txt"), std::string(2 * 1024 * 1024 + 8, 'x'));  // 超上限

    printf("cache dir: %ls\n", CacheDir().c_str());
    printf("\n-- 1) 正常建立：3 个可解析、1 个坏 docx、1 个超大、1 个不支持 --\n");
    {
        CacheBuildStats st = CacheBuildForDir(root, nullptr, nullptr);
        printf("  found=%d built=%d reused=%d failed=%d tooBig=%d\n",
               st.found, st.built, st.reused, st.failed, st.tooBig);
        CHECK(st.found == 5);          // a.txt b.md sub/e.csv c.docx big.txt
        CHECK(st.built == 3);
        CHECK(st.reused == 0);
        CHECK(st.failed == 1);         // c.docx
        CHECK(st.tooBig == 1);         // big.txt
        CHECK(!st.canceled && !st.quotaFull);
        CHECK(Cached(JoinPath(root, L"a.txt")));
        CHECK(Cached(JoinPath(root, L"b.md")));
        CHECK(Cached(JoinPath(root, L"sub\\e.csv")));
        CHECK(!Cached(JoinPath(root, L"c.docx")));      // 解析失败的不该有缓存
        CHECK(CacheUsedBytes() > 0);
    }

    printf("\n-- 2) 再跑一次：已有缓存应复用，不重复解析 --\n");
    {
        CacheBuildStats st = CacheBuildForDir(root, nullptr, nullptr);
        printf("  found=%d built=%d reused=%d\n", st.found, st.built, st.reused);
        CHECK(st.reused == 3);
        CHECK(st.built == 0);
        CHECK(Cached(JoinPath(root, L"a.txt")));
        CHECK(Cached(JoinPath(root, L"sub\\e.csv")));
    }

    printf("\n-- 3) 只有不支持的文件：一个缓存都不该产生 --\n");
    {
        std::wstring only = JoinPath(tmp, L"los_cachebuild_none");
        RemoveTree(only);
        CreateDirectoryW(only.c_str(), nullptr);
        WriteBytes(JoinPath(only, L"x.bin"), "nope\n");
        WriteBytes(JoinPath(only, L"y.jpg"), "nope\n");
        CacheBuildStats st = CacheBuildForDir(only, nullptr, nullptr);
        CHECK(st.found == 0);
        CHECK(st.built == 0 && st.reused == 0);
        RemoveTree(only);
    }

    printf("\n-- 4) 用户中途停止：立刻收手，标记 canceled --\n");
    {
        CacheBuildStats st = CacheBuildForDir(root, CancelsAfterFirst, nullptr);
        CHECK(st.canceled);
        CHECK(st.built <= 1);
    }

    printf("\n-- 5) 不存在的目录：安静返回空结果 --\n");
    {
        CacheBuildStats st = CacheBuildForDir(JoinPath(tmp, L"los_no_such_dir_12345"), nullptr, nullptr);
        CHECK(st.found == 0 && st.built == 0 && st.reused == 0 && !st.canceled);
    }

    RemoveTree(root);
    CacheClearAll();
    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
