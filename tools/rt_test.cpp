// rt_test.cpp - ntdll 压缩/解压往返自测：pack 同款调用方式，验证问题在哪一环
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>

typedef long (__stdcall*RtlGetCompWsSize_t)(unsigned short, unsigned long*, unsigned long*);
typedef long (__stdcall*RtlCompress_t)(unsigned short, unsigned char*, unsigned long,
                                       unsigned char*, unsigned long, unsigned long,
                                       unsigned long*, void*);
typedef long (__stdcall*RtlDecompress_t)(unsigned short, unsigned char*, unsigned long,
                                         unsigned char*, unsigned long, unsigned long*);

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) { printf("usage: rt_test <file>\n"); return 2; }

    HANDLE f = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { printf("open failed\n"); return 1; }
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    std::vector<unsigned char> plain((size_t)sz.QuadPart);
    DWORD rd = 0;
    ReadFile(f, plain.data(), (DWORD)plain.size(), &rd, nullptr);
    CloseHandle(f);
    printf("input %lu bytes\n", rd);

    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto getWs = (RtlGetCompWsSize_t)(void*)GetProcAddress(nt, "RtlGetCompressionWorkSpaceSize");
    auto comp  = (RtlCompress_t)(void*)GetProcAddress(nt, "RtlCompressBuffer");
    auto decomp= (RtlDecompress_t)(void*)GetProcAddress(nt, "RtlDecompressBuffer");
    if (!getWs || !comp || !decomp) { printf("api missing\n"); return 1; }

    for (unsigned short fmt : { (unsigned short)3, (unsigned short)4, (unsigned short)2 }) {
        unsigned long b = 0, s = 0;
        long st = getWs(fmt, &b, &s);
        printf("fmt %u: GetWsSize=0x%lX buf=%lu frag=%lu\n", fmt, (unsigned long)st, b, s);
        if (st != 0) continue;
        std::vector<unsigned char> ws(s + b + 64);
        std::vector<unsigned char> packed(plain.size() + 4096);
        unsigned long packedLen = 0;
        st = comp(fmt, plain.data(), (unsigned long)plain.size(),
                  packed.data(), (unsigned long)packed.size(), 4096, &packedLen, ws.data());
        printf("  compress st=0x%lX len=%lu\n", (unsigned long)st, packedLen);
        if (st != 0) continue;

        std::vector<unsigned char> out(plain.size() + 4096);
        unsigned long outLen = 0;
        st = decomp(fmt, out.data(), (unsigned long)out.size(),
                    packed.data(), packedLen, &outLen);
        printf("  decompress st=0x%lX len=%lu (expect %lu) match=%d\n",
               (unsigned long)st, outLen, rd,
               (st == 0 && outLen == rd && memcmp(out.data(), plain.data(), rd) == 0));
typedef long (__stdcall*RtlDecompressEx_t)(unsigned short, unsigned char*, unsigned long,
                                           unsigned char*, unsigned long, unsigned long*, void*);
        // 再试 Ex 版（带 workspace，XPRESS_HUFF 可能需要）
        auto decompEx = (RtlDecompressEx_t)(void*)GetProcAddress(nt, "RtlDecompressBufferEx");
        if (decompEx) {
            unsigned long outLen = 0;
            st = decompEx(fmt, out.data(), (unsigned long)out.size(),
                          packed.data(), packedLen, &outLen, ws.data());
            printf("  decompressEx st=0x%lX len=%lu match=%d\n",
                   (unsigned long)st, outLen,
                   (st == 0 && outLen == rd && memcmp(out.data(), plain.data(), rd) == 0));
        }
    }
    return 0;
}
