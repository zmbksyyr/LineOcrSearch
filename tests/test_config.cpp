// test_config.cpp - 验证取词方式的旧配置迁移（老 0/1/2 -> 新 1/2/3）
#include "../src/common.h"
#include <cstdio>

static int g_fail = 0;

static void Check(bool cond, const char* what) {
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fail;
}

// 递归删除目录（测试要反复把数据目录清空来演改名）
static void RemoveTree(const std::wstring& dir) {
    std::wstring pat = JoinPath(dir, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring p = JoinPath(dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static void WriteIni(const std::wstring& body) {
    CreateDirectoryW(ConfigDirPath().c_str(), nullptr);
    HANDLE f = CreateFileW(ConfigIniPath().c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string s;
    for (size_t i = 0; i < body.size(); ++i) s.push_back((char)body[i]);  // 纯 ASCII 测试内容
    DWORD w = 0;
    WriteFile(f, s.data(), (DWORD)s.size(), &w, nullptr);
    CloseHandle(f);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    printf("-- 旧配置（无 modeVersion）--\n");
    WriteIni(L"captureMode=0\n");
    Settings a = DefaultSettings();
    LoadSettings(a);
    Check(a.captureMode == 1, "old 0 (auto) -> new 1 (auto)");

    WriteIni(L"captureMode=1\n");
    Settings b = DefaultSettings();
    LoadSettings(b);
    Check(b.captureMode == 2, "old 1 (uia only) -> new 2 (uia)");

    WriteIni(L"captureMode=2\n");
    Settings c = DefaultSettings();
    LoadSettings(c);
    Check(c.captureMode == 3, "old 2 (image) -> new 3 (image)");

    printf("-- 新配置（modeVersion=2）--\n");
    WriteIni(L"captureMode=0\nmodeVersion=2\n");
    Settings d = DefaultSettings();
    LoadSettings(d);
    Check(d.captureMode == 0, "new 0 stays 0 (smart)");

    WriteIni(L"captureMode=3\nmodeVersion=2\n");
    Settings e = DefaultSettings();
    LoadSettings(e);
    Check(e.captureMode == 3, "new 3 stays 3");

    printf("-- 保存后应写入 modeVersion --\n");
    Settings f = DefaultSettings();
    f.captureMode = 0;
    SaveSettings(f);
    Settings g = DefaultSettings();
    g.captureMode = 3;
    LoadSettings(g);
    Check(g.captureMode == 0, "saved smart mode reloads as smart");

    printf("-- 默认值 --\n");
    Settings h = DefaultSettings();
    Check(h.captureMode == 0, "default is smart");
    Check(h.lingerMs == 500, "default linger 500ms");
    Check(h.cacheMB == 64, "default cache limit 64MB");

    printf("-- 缓存上限：读写 + 非法值归位 --\n");
    WriteIni(L"cacheMB=128\nmodeVersion=2\n");
    Settings i = DefaultSettings();
    LoadSettings(i);
    Check(i.cacheMB == 128, "cacheMB=128 stays 128");

    WriteIni(L"cacheMB=300\nmodeVersion=2\n");
    Settings j = DefaultSettings();
    LoadSettings(j);
    Check(j.cacheMB == 256, "cacheMB=300 snaps to 256");

    WriteIni(L"cacheMB=5\nmodeVersion=2\n");
    Settings k = DefaultSettings();
    LoadSettings(k);
    Check(k.cacheMB == 64, "cacheMB=5 snaps to 64");

    Settings l = DefaultSettings();
    l.cacheMB = 512;
    SaveSettings(l);
    Settings m = DefaultSettings();
    LoadSettings(m);
    Check(m.cacheMB == 512, "saved 512 reloads as 512");

    printf("-- 旧数据目录改名到 %ls --\n", cfgpaths::kConfigDir);
    {
        std::wstring target = ConfigDirPath();
        const wchar_t* legacyNames[2] = { cfgpaths::kLegacyDir2, cfgpaths::kLegacyDir };

        for (int i = 0; i < 2; ++i) {
            std::wstring legacy = JoinPath(ExeDir(), legacyNames[i]);

            // 目标目录里可能已有前面步骤写下的 config.ini 与 cache\，先清干净再演一遍改名
            RemoveTree(target);

            if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
                printf("[SKIP] %ls：目标目录非空，跳过\n", legacyNames[i]);
                continue;
            }

            CreateDirectoryW(legacy.c_str(), nullptr);
            MigrateLegacyDataDir();
            Check(GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES,
                  i == 0 ? "legacy LineOcrSearch dir renamed" : "legacy config dir renamed");
            Check(GetFileAttributesW(legacy.c_str()) == INVALID_FILE_ATTRIBUTES,
                  i == 0 ? "old LineOcrSearch dir gone" : "old config dir gone");
        }
    }

    printf("----\n%s: %d failure(s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
