// pack.cpp - 把依赖文件附加到 exe 末尾，让 LineOcrSearch.exe 自带运行时资源
//
// 用法：pack.exe <目标exe> "内部名=源文件" ["内部名=源文件" ...]
//   例：pack.exe dist\LineOcrSearch.exe "runtime\onnxruntime.dll=config\runtime\onnxruntime.dll"
//
// 布局（LOSRES02，每项可选压缩）：
//   payload{ [u32 nameLen][name UTF-8][u64 rawSize][u64 storedSize][u8 flag][data] ... }
//     flag: 0 = 原样存储；1 = ntdll XPRESS_HUFF 压缩（不可用时退 LZNT1，flag 记实际格式）
//   尾标{ magic[8]="LOSRES02", payloadOffset(u64), count(u32) }
// 兼容：旧 LOSRES01（无压缩字段）仍能被 bundle.cpp 识别；再次打包会先剥旧 payload。
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

static const char kMagic[8] = { 'L','O','S','R','E','S','0','2' };
static const DWORD kTrailerSize = 20;

// ntdll 压缩 API（动态加载，避免链接问题）
typedef long (__stdcall*RtlGetCompressionWorkSpaceSize_t)(unsigned short,
                                                          unsigned long*, unsigned long*);
typedef long (__stdcall*RtlCompressBuffer_t)(unsigned short, unsigned char*, unsigned long,
                                             unsigned char*, unsigned long, unsigned long,
                                             unsigned long*, void*);
typedef long (__stdcall*RtlDecompressBuffer_t)(unsigned short, unsigned char*, unsigned long,
                                               unsigned char*, unsigned long, unsigned long*);

static const unsigned short kFmtXpress     = 3;   // 往返验证通过，压缩率优于 LZNT1
static const unsigned short kFmtLznt1      = 2;   // 兜底

struct CompressApi {
    RtlCompressBuffer_t        compress = nullptr;
    RtlGetCompressionWorkSpaceSize_t wsSize = nullptr;
    unsigned short             fmt = 0;
    std::vector<unsigned char> ws;

    bool Init() {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (!nt) return false;
        compress = (RtlCompressBuffer_t)(void*)GetProcAddress(nt, "RtlCompressBuffer");
        wsSize = (RtlGetCompressionWorkSpaceSize_t)(void*)GetProcAddress(
            nt, "RtlGetCompressionWorkSpaceSize");
        if (!compress || !wsSize) return false;

        // XPRESS_HUFF(4) 压得动但本机 ntdll 解不开（0xC00000E8 / 0xC0000242），不用；
        // XPRESS(3) 与 LZNT1(2) 都往返验证通过，优先 XPRESS
        for (unsigned short f : { kFmtXpress, kFmtLznt1 }) {
            unsigned long b = 0, s = 0;
            if (wsSize(f, &b, &s) == 0) {
                ws.resize(s + b + 64);
                fmt = f;
                return true;
            }
        }
        return false;
    }
};

// 返回 true 表示压缩后更小；out 为压缩数据
static bool TryCompress(CompressApi& api, const std::vector<unsigned char>& in,
                        std::vector<unsigned char>& out) {
    out.clear();
    if (!api.compress) return false;
    out.resize(in.size() + 4096);
    unsigned long finalSize = 0;
    long st = api.compress(api.fmt,
                           (unsigned char*)in.data(), (unsigned long)in.size(),
                           out.data(), (unsigned long)out.size(),
                           4096, &finalSize, api.ws.data());
    if (st != 0 || finalSize == 0 || finalSize >= in.size()) {
        out.clear();
        return false;
    }
    out.resize(finalSize);
    return true;
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static bool SeekTo(HANDLE f, unsigned long long off) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(f, li, nullptr, FILE_BEGIN) != 0;
}

static bool WriteAll(HANDLE f, const void* p, DWORD n) {
    const unsigned char* b = (const unsigned char*)p;
    DWORD done = 0;
    while (done < n) {
        DWORD wrote = 0;
        if (!WriteFile(f, b + done, n - done, &wrote, nullptr) || wrote == 0) return false;
        done += wrote;
    }
    return true;
}

static bool ReadAllAt(HANDLE f, unsigned long long off, void* p, DWORD n) {
    if (!SeekTo(f, off)) return false;
    DWORD got = 0;
    return ReadFile(f, p, n, &got, nullptr) && got == n;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) {
        wprintf(L"usage: pack.exe <target.exe> \"innerName=sourceFile\" [...]\n");
        return 2;
    }

    CompressApi api;
    bool canCompress = api.Init();
    wprintf(L"[pack] compression: %ls\n",
            canCompress ? (api.fmt == kFmtXpress ? L"XPRESS" : L"LZNT1") : L"unavailable");

    const wchar_t* exePath = argv[1];
    HANDLE f = CreateFileW(exePath, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        wprintf(L"[pack] cannot open %ls\n", exePath);
        return 1;
    }

    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    unsigned long long total = (unsigned long long)sz.QuadPart;

    // 已经打过包：截掉旧的 payload + 尾标，从干净状态重新写。
    // 旧 LOSRES01 与新 LOSRES02 的尾标前 8 字节不同，但布局同构，直接按通用办法剥：
    // 读两种 magic，谁匹配就按谁的 payloadOffset 截。
    if (total > kTrailerSize) {
        unsigned char tr[kTrailerSize] = {0};
        if (ReadAllAt(f, total - kTrailerSize, tr, kTrailerSize) &&
            (memcmp(tr, "LOSRES01", 8) == 0 || memcmp(tr, kMagic, 8) == 0)) {
            unsigned long long off = 0;
            for (int i = 7; i >= 0; --i) off = (off << 8) | (unsigned long long)tr[8 + i];
            if (off > 0 && off < total) {
                SeekTo(f, off);
                SetEndOfFile(f);
                total = off;
                wprintf(L"[pack] stripped previous bundle (back to %llu bytes)\n", off);
            }
        }
    }

    unsigned long long payloadOff = total;
    if (!SeekTo(f, payloadOff)) {
        CloseHandle(f);
        return 1;
    }

    unsigned int count = 0;
    unsigned long long cursor = payloadOff;
    std::vector<unsigned char> raw(1 * 1024 * 1024);

    for (int i = 2; i < argc; ++i) {
        std::wstring arg = argv[i];
        size_t eq = arg.find(L'=');
        if (eq == std::wstring::npos) {
            wprintf(L"[pack] bad argument (need name=path): %ls\n", arg.c_str());
            CloseHandle(f);
            return 1;
        }
        std::wstring inner = arg.substr(0, eq);
        std::wstring srcPath = arg.substr(eq + 1);
        for (size_t k = 0; k < inner.size(); ++k) if (inner[k] == L'/') inner[k] = L'\\';

        HANDLE in = CreateFileW(srcPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (in == INVALID_HANDLE_VALUE) {
            wprintf(L"[pack] missing source file: %ls\n", srcPath.c_str());
            CloseHandle(f);
            return 1;
        }
        LARGE_INTEGER fsz{};
        GetFileSizeEx(in, &fsz);
        unsigned long long size = (unsigned long long)fsz.QuadPart;

        std::vector<unsigned char> data((size_t)size);
        unsigned long long got = 0;
        while (got < size) {
            DWORD want = (DWORD)((size - got) > raw.size() ? raw.size() : (size - got));
            DWORD rd = 0;
            if (!ReadFile(in, raw.data(), want, &rd, nullptr) || rd == 0) break;
            memcpy(data.data() + got, raw.data(), rd);
            got += rd;
        }
        CloseHandle(in);
        if (got != size) {
            wprintf(L"[pack] read failed: %ls\n", srcPath.c_str());
            CloseHandle(f);
            return 1;
        }

        // 尝试压缩；压不小就原样存
        std::vector<unsigned char> packed;
        unsigned char flag = 0;
        if (canCompress && TryCompress(api, data, packed)) {
            data.swap(packed);
            flag = (unsigned char)api.fmt;
        }

        std::string name = ToUtf8(inner);
        unsigned char hdr[4] = {
            (unsigned char)(name.size() & 0xFF),
            (unsigned char)((name.size() >> 8) & 0xFF),
            (unsigned char)((name.size() >> 16) & 0xFF),
            (unsigned char)((name.size() >> 24) & 0xFF)
        };
        unsigned char rawb[8] = {0};
        unsigned long long rawSize = size;
        for (int k = 0; k < 8; ++k) rawb[k] = (unsigned char)((rawSize >> (8 * k)) & 0xFF);
        unsigned char stb[8] = {0};
        unsigned long long stored = data.size();
        for (int k = 0; k < 8; ++k) stb[k] = (unsigned char)((stored >> (8 * k)) & 0xFF);

        bool ok = WriteAll(f, hdr, 4) && WriteAll(f, name.data(), (DWORD)name.size()) &&
                  WriteAll(f, rawb, 8) && WriteAll(f, stb, 8) && WriteAll(f, &flag, 1) &&
                  WriteAll(f, data.data(), (DWORD)data.size());
        if (!ok) {
            wprintf(L"[pack] write failed\n");
            CloseHandle(f);
            return 1;
        }

        wprintf(L"[pack] + %-28ls %9llu -> %9llu bytes (%ls)\n", inner.c_str(), rawSize, stored,
                flag ? L"compressed" : L"raw");
        cursor += 4 + name.size() + 8 + 8 + 1 + stored;
        ++count;
    }

    // 尾标
    unsigned char tr[kTrailerSize] = {0};
    memcpy(tr, kMagic, 8);
    for (int k = 0; k < 8; ++k) tr[8 + k] = (unsigned char)((payloadOff >> (8 * k)) & 0xFF);
    for (int k = 0; k < 4; ++k) tr[16 + k] = (unsigned char)((count >> (8 * k)) & 0xFF);

    SeekTo(f, cursor);
    bool ok = WriteAll(f, tr, kTrailerSize);
    CloseHandle(f);

    if (!ok) {
        wprintf(L"[pack] trailer write failed\n");
        return 1;
    }
    wprintf(L"[pack] done: %u files, total %llu bytes\n", count, cursor + kTrailerSize);
    return 0;
}
