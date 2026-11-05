// test_legacy.cpp - 老格式（.doc/.xls）解析的健壮性：坏输入只能返回失败，绝不许崩
// 想跑真实样本：设环境变量 LOS_LEGACY_SAMPLE=某个装着 .doc/.xls 的目录，再运行
#include "../src/common.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (cond) { printf("  PASS  %s\n", #cond); } \
    else { printf("  FAIL  %s  (line %d)\n", #cond, __LINE__); ++g_fail; } \
} while (0)

static std::wstring TempDir() {
    wchar_t tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"los_legacy_test";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

static void WriteBytes(const std::wstring& path, const void* p, size_t n) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    if (n) WriteFile(h, p, (DWORD)n, &w, nullptr);
    CloseHandle(h);
}

static void PutU16(std::vector<unsigned char>& v, size_t off, unsigned val) {
    v[off] = (unsigned char)(val & 0xFF);
    v[off + 1] = (unsigned char)((val >> 8) & 0xFF);
}
static void PutU32(std::vector<unsigned char>& v, size_t off, unsigned val) {
    v[off] = (unsigned char)(val & 0xFF);
    v[off + 1] = (unsigned char)((val >> 8) & 0xFF);
    v[off + 2] = (unsigned char)((val >> 16) & 0xFF);
    v[off + 3] = (unsigned char)((val >> 24) & 0xFF);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::wstring dir = TempDir();
    std::wstring text;

    printf("-- 1) 空文件 / 纯文本伪装 / 随机字节：都应返回失败且不崩 --\n");
    {
        std::wstring empty = JoinPath(dir, L"empty.doc");
        WriteBytes(empty, "", 0);
        CHECK(!ExtractOfficeText(empty, text));

        std::wstring fake = JoinPath(dir, L"fake.doc");
        const char* txt = "这不是复合文档，只是一段普通文本 0123456789";
        WriteBytes(fake, txt, strlen(txt));
        CHECK(!ExtractOfficeText(fake, text));

        std::vector<unsigned char> rnd(4096);
        for (size_t i = 0; i < rnd.size(); ++i) rnd[i] = (unsigned char)(i * 37 + 11);
        std::wstring r = JoinPath(dir, L"random.xls");
        WriteBytes(r, rnd.data(), rnd.size());
        CHECK(!ExtractOfficeText(r, text));
    }

    printf("\n-- 2) 只有 CFB 头、后面没有扇区：不能越界读 --\n");
    {
        std::vector<unsigned char> hdr(512, 0);
        const unsigned char sig[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
        memcpy(hdr.data(), sig, 8);
        PutU16(hdr, 0x1A, 3);      // major version 3
        PutU16(hdr, 0x1E, 9);      // sector shift 512
        PutU16(hdr, 0x20, 6);      // mini sector shift 64
        PutU32(hdr, 0x30, 0);      // 目录扇区 0（实际不存在）
        PutU32(hdr, 0x2C, 0);
        std::wstring p = JoinPath(dir, L"hdronly.doc");
        WriteBytes(p, hdr.data(), hdr.size());
        CHECK(!ExtractOfficeText(p, text));
    }

    printf("\n-- 3) CFB 头里的链值全是指向界外的脏数据：要有保护、不能死循环 --\n");
    {
        std::vector<unsigned char> buf(512 * 3, 0);
        const unsigned char sig[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
        memcpy(buf.data(), sig, 8);
        PutU16(buf, 0x1A, 3);
        PutU16(buf, 0x1E, 9);
        PutU16(buf, 0x20, 6);
        PutU32(buf, 0x2C, 1);                 // 1 个 FAT 扇区
        PutU32(buf, 0x30, 0x7FFFFFF0u);       // 目录起始扇区：远超文件
        PutU32(buf, 0x44, 0x7FFFFFF1u);       // DIFAT 起始扇区：同样越界
        PutU32(buf, 0x48, 1000000);           // DIFAT 扇区数
        PutU32(buf, 0x4C, 0x7FFFFFF2u);       // 第一个 FAT 扇区也越界
        std::wstring p = JoinPath(dir, L"dirty.xls");
        WriteBytes(p, buf.data(), buf.size());
        CHECK(!ExtractOfficeText(p, text));
    }

    printf("\n-- 4) 真实样本（可选，设 LOS_LEGACY_SAMPLE 目录）--\n");
    {
        wchar_t sample[MAX_PATH * 2] = {0};
        DWORD n = GetEnvironmentVariableW(L"LOS_LEGACY_SAMPLE", sample, MAX_PATH * 2);
        if (n == 0 || n >= MAX_PATH * 2) {
            printf("  SKIP  未设置 LOS_LEGACY_SAMPLE\n");
        } else {
            int ok = 0, bad = 0;
            std::wstring pat = JoinPath(sample, L"*");
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW(pat.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    std::wstring full = JoinPath(sample, fd.cFileName);
                    std::wstring ext = fd.cFileName;
                    size_t dot = ext.find_last_of(L'.');
                    if (dot == std::wstring::npos) continue;
                    ext = ext.substr(dot);
                    for (size_t i = 0; i < ext.size(); ++i) ext[i] = (wchar_t)towlower(ext[i]);
                    if (ext != L".doc" && ext != L".xls") continue;
                    std::wstring t;
                    if (ExtractOfficeText(full, t) && t.size() > 10) ++ok; else ++bad;
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            printf("  样本：OK=%d FAIL=%d\n", ok, bad);
            CHECK(ok > 0);
            CHECK(bad == 0);
        }
    }

    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
