// bundle.cpp - 运行时资源随 exe 一起分发
//
// 打包方式：把文件顺序附加到 exe 末尾，最后追加 20 字节尾标：
//   LOSRES01（旧）：magic[8] + payloadOffset(u64) + count(u32)，条目原样存储
//   LOSRES02（新）：条目为 [u32 nameLen][name][u64 rawSize][u64 storedSize][u8 flag][data]
//     flag: 0 = 原样；2/3 = ntdll LZNT1 / XPRESS 压缩（4 = XPRESS_HUFF，本机解不开，打包器不会产生）
// PE 加载器不看最后一个节之后的数据，所以 exe 仍是正常程序。
//
// 启动时读自己的文件：尾标存在 -> 逐个比对数据目录下的文件，缺失或大小不符才写盘。
// 依赖齐全时这一步只读 20 字节，不复制任何东西。
#include "common.h"

namespace {

const char  kMagicV1[8]   = { 'L','O','S','R','E','S','0','1' };
const char  kMagicV2[8]   = { 'L','O','S','R','E','S','0','2' };
const DWORD kTrailerSize  = 20;                  // magic(8) + offset(8) + count(4)
const DWORD kChunk        = 64 * 1024;

typedef long (__stdcall*RtlDecompressBuffer_t)(unsigned short, unsigned char*, unsigned long,
                                               unsigned char*, unsigned long, unsigned long*);

unsigned int Rd32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

unsigned long long Rd64(const unsigned char* p) {
    unsigned long long v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | (unsigned long long)p[i];
    return v;
}

bool ReadAt(HANDLE f, unsigned long long off, void* buf, DWORD n) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(f, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(f, buf, n, &got, nullptr) && got == n;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// 逐级创建目录（路径里可能带多级子目录）
bool EnsureDirTree(const std::wstring& dir) {
    if (dir.empty()) return false;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;

    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos && pos > 2) {
        EnsureDirTree(dir.substr(0, pos));
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ExistingSizeMatches(const std::wstring& path, unsigned long long size) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    unsigned long long have = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return have == size;
}

}  // namespace

int ExtractBundledResources(std::wstring& note) {
    note.clear();

    wchar_t selfPath[1024] = {0};
    DWORD n = GetModuleFileNameW(nullptr, selfPath, (DWORD)(sizeof(selfPath) / sizeof(wchar_t)));
    if (n == 0 || n >= sizeof(selfPath) / sizeof(wchar_t)) return 0;

    HANDLE f = CreateFileW(selfPath, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= (LONGLONG)kTrailerSize) {
        CloseHandle(f);
        return 0;
    }
    unsigned long long total = (unsigned long long)sz.QuadPart;

    unsigned char tr[kTrailerSize] = {0};
    if (!ReadAt(f, total - kTrailerSize, tr, kTrailerSize) ||
        memcmp(tr, kMagicV1, 8) != 0 && memcmp(tr, kMagicV2, 8) != 0) {
        CloseHandle(f);
        return 0;   // 未打包（开发机直接跑未打包的 exe）：正常情形，不算失败
    }

    unsigned long long payloadOff = Rd64(tr + 8);
    unsigned int count = Rd32(tr + 16);
    if (count == 0 || count > 64 || payloadOff + kTrailerSize > total) {
        CloseHandle(f);
        return 0;
    }

    // 识别尾标版本：01 = 旧无压缩条目，02 = 新可压缩条目
    bool v2 = (memcmp(tr, kMagicV2, 8) == 0);
    if (!v2 && memcmp(tr, kMagicV1, 8) != 0) {
        CloseHandle(f);
        return 0;
    }

    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlDecompressBuffer_t decompress = nullptr;
    if (v2 && nt) {
        decompress = (RtlDecompressBuffer_t)(void*)GetProcAddress(nt, "RtlDecompressBuffer");
    }

    std::vector<unsigned char> buf;
    std::wstring fail;
    int extracted = 0;
    unsigned long long cur = payloadOff;

    for (unsigned int i = 0; i < count; ++i) {
        unsigned char hdr[4] = {0};
        if (!ReadAt(f, cur, hdr, 4)) break;
        cur += 4;
        unsigned int nameLen = Rd32(hdr);
        if (nameLen == 0 || nameLen > 512) break;

        std::string nameBytes(nameLen, '\0');
        if (!ReadAt(f, cur, &nameBytes[0], nameLen)) break;
        cur += nameLen;

        unsigned char szb[8] = {0};
        if (!ReadAt(f, cur, szb, 8)) break;
        cur += 8;
        unsigned long long fsize = Rd64(szb);

        unsigned long long stored = fsize;   // v1：存储大小就是文件大小
        unsigned char flag = 0;              // v1：永远原样
        if (v2) {
            unsigned char stb[8] = {0};
            if (!ReadAt(f, cur, stb, 8)) break;
            cur += 8;
            stored = Rd64(stb);
            if (!ReadAt(f, cur, &flag, 1)) break;
            cur += 1;
            if (flag != 0 && flag != 2 && flag != 3) break;   // 未知格式，别瞎解
        }

        for (size_t k = 0; k < nameBytes.size(); ++k) {
            if (nameBytes[k] == '/') nameBytes[k] = '\\';
        }
        std::wstring rel = Utf8ToWide(nameBytes);
        std::wstring dest = rel.empty() ? std::wstring() : JoinPath(ConfigDirPath(), rel);

        if (!dest.empty() && !ExistingSizeMatches(dest, fsize)) {
            size_t slash = dest.find_last_of(L'\\');
            if (slash != std::wstring::npos) EnsureDirTree(dest.substr(0, slash));

            HANDLE out = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (out == INVALID_HANDLE_VALUE) {
                fail = L"数据目录不可写";
            } else {
                bool ok = true;
                if (flag == 0) {
                    if (buf.size() < kChunk) buf.resize(kChunk);
                    unsigned long long left = stored;
                    unsigned long long rd = cur;
                    while (left > 0) {
                        DWORD want = (DWORD)(left > kChunk ? kChunk : left);
                        if (!ReadAt(f, rd, buf.data(), want)) { ok = false; break; }
                        DWORD wrote = 0;
                        if (!WriteFile(out, buf.data(), want, &wrote, nullptr) || wrote != want) {
                            ok = false;
                            break;
                        }
                        rd += want;
                        left -= want;
                    }
                } else if (decompress) {
                    // 压缩条目一次性读入内存再解压（最大也就 11MB 量级）
                    std::vector<unsigned char> packed((size_t)stored);
                    if (!ReadAt(f, cur, packed.data(), (DWORD)packed.size())) {
                        ok = false;
                    } else {
                        std::vector<unsigned char> plain((size_t)fsize + 4096);
                        unsigned long finalSize = 0;
                        long st = decompress((unsigned short)flag, plain.data(),
                                             (unsigned long)plain.size(),
                                             packed.data(), (unsigned long)packed.size(),
                                             &finalSize);
                        if (st != 0 || (unsigned long long)finalSize != fsize) {
                            ok = false;
                        } else {
                            // 分块写出，避免一次 WriteFile 超过 DWORD 范围
                            unsigned long long left = fsize, off = 0;
                            while (left > 0) {
                                DWORD want = (DWORD)(left > kChunk ? kChunk : left);
                                DWORD wrote = 0;
                                if (!WriteFile(out, plain.data() + off, want, &wrote, nullptr) ||
                                    wrote != want) { ok = false; break; }
                                off += want;
                                left -= want;
                            }
                        }
                    }
                } else {
                    ok = false;   // v2 条目但拿不到解压函数：不写半截文件
                }
                CloseHandle(out);
                if (ok) ++extracted;
                else DeleteFileW(dest.c_str());   // 半截文件不留在磁盘上
            }
        }
        cur += stored;
    }

    CloseHandle(f);

    if (extracted > 0) note = L"已释放 " + std::to_wstring(extracted) + L" 个资源文件";
    else if (!fail.empty()) note = fail;
    return extracted;
}
