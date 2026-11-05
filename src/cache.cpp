// cache.cpp - 文档提取缓存：内存 LRU 优先，落盘到数据目录下的 cache\ 跨重启复用
// 只缓存用户自己文件夹里解析出的文档文本；屏幕内容、OCR 原文一律不缓存。
#include "common.h"
#include <unordered_map>
#include <list>
#include <cstdio>
#include <algorithm>

namespace {

// 后台搜索线程与设置窗口的“建立缓存”可能同时用缓存，公开入口统一串行化
CRITICAL_SECTION* CacheCs() {
    static CRITICAL_SECTION* cs = [] {
        CRITICAL_SECTION* p = new CRITICAL_SECTION();
        InitializeCriticalSection(p);
        return p;
    }();
    return cs;
}

struct CacheGuard {
    CacheGuard() { EnterCriticalSection(CacheCs()); }
    ~CacheGuard() { LeaveCriticalSection(CacheCs()); }
    CacheGuard(const CacheGuard&) = delete;
    CacheGuard& operator=(const CacheGuard&) = delete;
};

struct MemEntry {
    std::vector<std::wstring> lines;
    size_t chars = 0;
};

std::unordered_map<std::string, MemEntry> g_mem;
std::list<std::string> g_lru;  // 头部最新
const size_t kMaxMemEntries = 120;
// 光按条数限不住内存：预建缓存会把整目录的正文都塞进来，再给一个字符总量上限，
// 超出的只落盘不进内存，下次命中走磁盘。
const size_t kMaxMemChars = 24u * 1024u * 1024u;
size_t g_memChars = 0;
int g_batchDepth = 0;  // >0 表示批量写入，暂不做逐次配额检查

// FNV-1a，用于生成缓存文件名
unsigned long long Fnv1a(const std::string& s) {
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < s.size(); ++i) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// key = 绝对路径 | 大小 | 修改时间
std::string MakeKey(const std::wstring& path, unsigned size, unsigned long long mtime) {
    std::string k = Utf8(path);
    k += '|';
    k += std::to_string(size);
    k += '|';
    k += std::to_string(mtime);
    return k;
}

// 文件名只由「路径」决定，不含大小和 mtime：文档改一次就覆盖同一个缓存文件，
// 不会像以前那样每个修改时间都留一份、旧文件永远没人清。
// 大小和 mtime 写在文件头做校验，读盘时对不上就当失效。
std::wstring CacheFilePath(const std::string& key) {
    size_t cut = key.find('|');
    std::string nameKey = (cut == std::string::npos) ? key : key.substr(0, cut);
    wchar_t name[64];
    swprintf(name, 64, L"%016llx.cache", Fnv1a(nameKey));
    return JoinPath(SubDirPath(cfgpaths::kCacheDir), name);
}

size_t CountChars(const std::vector<std::wstring>& lines) {
    size_t n = 0;
    for (size_t i = 0; i < lines.size(); ++i) n += lines[i].size() + 1;
    return n;
}

void MemErase(const std::string& key) {
    std::unordered_map<std::string, MemEntry>::iterator it = g_mem.find(key);
    if (it == g_mem.end()) return;
    if (g_memChars >= it->second.chars) g_memChars -= it->second.chars;
    else g_memChars = 0;
    g_mem.erase(it);
}

void TouchLru(const std::string& key) {
    for (std::list<std::string>::iterator it = g_lru.begin(); it != g_lru.end(); ++it) {
        if (*it == key) {
            g_lru.erase(it);
            break;
        }
    }
    g_lru.push_front(key);
    while (g_lru.size() > kMaxMemEntries) {
        MemErase(g_lru.back());
        g_lru.pop_back();
    }
}

// 放进内存 LRU；太大就只留磁盘那份，避免预建缓存时把内存撑爆
void MemStore(const std::string& key, const std::vector<std::wstring>& lines) {
    size_t chars = CountChars(lines);
    MemErase(key);
    for (std::list<std::string>::iterator it = g_lru.begin(); it != g_lru.end(); ++it) {
        if (*it == key) {
            g_lru.erase(it);
            break;
        }
    }
    if (chars > kMaxMemChars) return;
    if (g_memChars + chars > kMaxMemChars) return;

    MemEntry e;
    e.lines = lines;
    e.chars = chars;
    g_mem[key] = e;
    g_memChars += chars;
    TouchLru(key);
}

bool ReadDisk(const std::string& key, std::vector<std::wstring>& lines) {
    std::wstring path = CacheFilePath(key);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)32 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    std::string raw((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    BOOL ok = ReadFile(h, &raw[0], (DWORD)raw.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    raw.resize(got);

    size_t nl = raw.find('\n');
    if (nl == std::string::npos) return false;
    if (raw.substr(0, nl) != key) return false;  // 源文件变了，缓存失效

    std::string body = raw.substr(nl + 1);
    lines.clear();
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t e = body.find('\n', pos);
        std::string line = (e == std::string::npos) ? body.substr(pos) : body.substr(pos, e - pos);
        lines.push_back(FromUtf8(line));
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return true;
}

// 只看文件头那一行 key，不把正文读进来
bool ReadDiskHeader(const std::string& key) {
    std::wstring path = CacheFilePath(key);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    char buf[8192];
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)sizeof(buf), &got, nullptr);
    CloseHandle(h);
    if (!ok || got == 0) return false;

    size_t len = 0;
    while (len < got && buf[len] != '\n') ++len;
    if (len >= got) return false;          // 头行没读完，按失效处理
    if (key.size() != len) return false;
    return memcmp(buf, key.data(), len) == 0;
}

void EnforceDiskQuota(const std::wstring& dir);

void WriteDisk(const std::string& key, const std::vector<std::wstring>& lines) {
    std::wstring dir = SubDirPath(cfgpaths::kCacheDir);
    CreateDirectoryW(dir.c_str(), nullptr);

    std::string out = key;
    out += '\n';
    for (size_t i = 0; i < lines.size(); ++i) {
        out += Utf8(lines[i]);
        out += '\n';
    }

    std::wstring path = CacheFilePath(key);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;  // 只读目录等情况：静默放弃
    DWORD wr = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &wr, nullptr);
    CloseHandle(h);

    if (g_batchDepth == 0) EnforceDiskQuota(dir);
}

// ---- 磁盘总量上限：可在设置里改，超过上限就按最后写入时间从旧到新删 ----
static int g_limitMB = 64;  // 默认 64MB

unsigned long long MaxDiskBytes() { return (unsigned long long)g_limitMB * 1024ULL * 1024ULL; }
unsigned long long LowWaterBytes() { return MaxDiskBytes() / 10ULL * 8ULL; }  // 降到 80% 停手

unsigned long long UsedBytes(const std::wstring& dir) {
    std::wstring pattern = JoinPath(dir, L"*.cache");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    unsigned long long total = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        total += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

void EnforceDiskQuota(const std::wstring& dir) {
    std::wstring pattern = JoinPath(dir, L"*.cache");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    unsigned long long total = 0;
    struct Item { unsigned long long write; unsigned long long size; std::wstring name; };
    std::vector<Item> items;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        unsigned long long wt = ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32)
                              | fd.ftLastWriteTime.dwLowDateTime;
        Item it;
        it.write = wt;
        it.size = sz;
        it.name = JoinPath(dir, fd.cFileName);
        items.push_back(it);
        total += sz;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (total <= MaxDiskBytes()) return;

    // 从最旧开始删，降到低水位为止
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.write < b.write; });
    for (size_t i = 0; i < items.size() && total > LowWaterBytes(); ++i) {
        if (DeleteFileW(items[i].name.c_str())) total -= items[i].size;
    }
}

}  // namespace

void CacheSetLimitMB(int mb) {
    if (mb < 16) mb = 16;
    if (mb > 2048) mb = 2048;
    CacheGuard g;
    g_limitMB = mb;
}

unsigned long long CacheLimitBytes() { return MaxDiskBytes(); }

unsigned long long CacheUsedBytes() {
    CacheGuard g;
    return UsedBytes(SubDirPath(cfgpaths::kCacheDir));
}

void CacheBeginBatch() {
    CacheGuard g;
    ++g_batchDepth;
}

void CacheEndBatch() {
    CacheGuard g;
    if (g_batchDepth > 0) --g_batchDepth;
    if (g_batchDepth == 0) EnforceDiskQuota(SubDirPath(cfgpaths::kCacheDir));
}

bool CacheGet(const std::wstring& path, unsigned size, unsigned long long mtime,
              std::vector<std::wstring>& lines) {
    std::string key = MakeKey(path, size, mtime);
    CacheGuard g;

    std::unordered_map<std::string, MemEntry>::iterator it = g_mem.find(key);
    if (it != g_mem.end()) {
        lines = it->second.lines;
        TouchLru(key);
        return true;
    }
    if (ReadDisk(key, lines)) {
        MemStore(key, lines);
        return true;
    }
    return false;
}

bool CacheHas(const std::wstring& path, unsigned size, unsigned long long mtime) {
    std::string key = MakeKey(path, size, mtime);
    CacheGuard g;
    if (g_mem.find(key) != g_mem.end()) return true;
    return ReadDiskHeader(key);
}

// 清空内存缓存 + 删除 cache\ 下的缓存文件（只动自己的缓存，不碰别处）
void CacheClearAll() {
    CacheGuard g;
    g_mem.clear();
    g_lru.clear();
    g_memChars = 0;

    std::wstring dir = SubDirPath(cfgpaths::kCacheDir);
    std::wstring pattern = JoinPath(dir, L"*.cache");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring f = JoinPath(dir, fd.cFileName);
        DeleteFileW(f.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void CachePut(const std::wstring& path, unsigned size, unsigned long long mtime,
              const std::vector<std::wstring>& lines) {
    if (lines.empty()) return;
    std::string key = MakeKey(path, size, mtime);

    CacheGuard g;
    MemStore(key, lines);
    WriteDisk(key, lines);
}
