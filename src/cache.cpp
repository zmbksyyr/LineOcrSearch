// cache.cpp - 文档提取缓存：内存 LRU + 单文件存储库
//
// 磁盘侧只有一个 append-only 的 cache\store.bin：
//   文件头 8 字节 "LOSC0001"，之后是连续记录：
//     [u32 keyLen][u32 bodyLen][u32 seq] + key + body（UTF-8，行间 \n）
//   启动后第一次用到缓存时顺序扫一遍建索引 key -> (offset, len)，
//   之后每次命中只是一次 pread，不再「每个文件一次 open」。
//
// 为什么改：旧版每文档一个 .cache，2000 个文件 = 2000 次 CreateFileW，
// 实测约 1.2s/次搜索，按 kMaxScanned 推算上限 12s；单库后同样规模降到几十毫秒。
//
// 只缓存用户自己文件夹里解析出的文档文本；屏幕内容、OCR 原文一律不缓存。
#include "common.h"
#include <unordered_map>
#include <list>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <mutex>

namespace {

// 内存/索引用 CacheCs 保护；磁盘读写用 g_storeIo 串行化。
// 两者不嵌套持有太久：磁盘 I/O 期间只拿 g_storeIo，不拿 CacheCs，
// 这样建缓存写盘时，搜索线程仍能命中内存 LRU。
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

std::mutex g_storeIo;   // 磁盘 store 的读/写/压缩都串行化，避免共用文件指针

// ---------------- 内存 LRU ----------------

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

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const char* p, size_t len) {
    if (!p || len == 0) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, p, (int)len, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p, (int)len, &w[0], n);
    return w;
}

// 解析器版本：文档提取逻辑一改就 +1。
// key 里带着版本，解析器修好后旧缓存自动失效重建 —— 否则用户会一直读修好之前的旧文本。
constexpr const char* kParserVersion = "p3";

// key = 绝对路径 | 解析器版本 | 大小 | 修改时间
std::string MakeKey(const std::wstring& path, unsigned size, unsigned long long mtime) {
    std::string k = Utf8(path);
    k += '|';
    k += kParserVersion;
    k += '|';
    k += std::to_string(size);
    k += '|';
    k += std::to_string(mtime);
    return k;
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

// ---------------- 单文件存储库 ----------------

const char     kStoreMagic[8] = { 'L', 'O', 'S', 'C', '0', '0', '0', '1' };
const unsigned kStoreHdr      = 8;    // 文件头
const unsigned kRecHdr        = 12;   // 每条记录的头：keyLen / bodyLen / seq

struct StoredEntry {
    unsigned long long off;      // 记录起点（含 12 字节头）
    unsigned bodyLen = 0;
    unsigned keyLen  = 0;
    unsigned seq     = 0;        // 写入序号：压缩时按它从旧到新丢
};

HANDLE g_store = INVALID_HANDLE_VALUE;
unsigned long long g_storeEnd = 0;    // 有效数据末尾（也是下一条的写入位置）
unsigned g_storeSeq = 0;
std::unordered_map<std::string, StoredEntry> g_index;
// 同一路径被改写后，旧记录还留在文件里（key 里带 size/mtime）。这里记「每个路径
// 最新的写入序号」，读取时旧的一律当未命中；压缩时也只保留最新的那条。
std::unordered_map<std::string, unsigned> g_latest;

std::string PathOfKey(const std::string& key) {
    size_t cut = key.find('|');
    return (cut == std::string::npos) ? key : key.substr(0, cut);
}

int g_limitMB = 64;  // 默认 64MB

unsigned long long MaxDiskBytes() { return (unsigned long long)g_limitMB * 1024ULL * 1024ULL; }
unsigned long long LowWaterBytes() { return MaxDiskBytes() / 10ULL * 8ULL; }  // 降到 80% 停手

std::wstring StoreDir()  { return SubDirPath(cfgpaths::kCacheDir); }
std::wstring StorePath() { return JoinPath(StoreDir(), L"store.bin"); }

bool ReadAt(unsigned long long off, void* dst, size_t n) {
    if (g_store == INVALID_HANDLE_VALUE || n == 0) return false;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(g_store, li, nullptr, FILE_BEGIN)) return false;
    char* p = (char*)dst;
    size_t got = 0;
    while (got < n) {
        DWORD rd = 0;
        DWORD ask = (DWORD)((n - got) > 0x1000000 ? 0x1000000 : (n - got));
        if (!ReadFile(g_store, p + got, ask, &rd, nullptr) || rd == 0) return false;
        got += rd;
    }
    return true;
}

bool WriteAt(unsigned long long off, const void* src, size_t n) {
    if (g_store == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(g_store, li, nullptr, FILE_BEGIN)) return false;
    const char* p = (const char*)src;
    size_t put = 0;
    while (put < n) {
        DWORD wr = 0;
        DWORD ask = (DWORD)((n - put) > 0x1000000 ? 0x1000000 : (n - put));
        if (!WriteFile(g_store, p + put, ask, &wr, nullptr) || wr == 0) return false;
        put += wr;
    }
    return true;
}

void TruncateAt(unsigned long long off) {
    if (g_store == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    SetFilePointerEx(g_store, li, nullptr, FILE_BEGIN);
    SetEndOfFile(g_store);
}

// 重建为只有一个文件头的空库
void ResetStore() {
    g_index.clear();
    g_latest.clear();
    g_storeSeq = 0;
    TruncateAt(0);
    WriteAt(0, kStoreMagic, kStoreHdr);
    g_storeEnd = kStoreHdr;
}

// 删掉旧版「每文档一个 .cache」留下的文件（只在新建库时做一次）
void DeleteLegacyCacheFiles() {
    std::wstring pattern = JoinPath(StoreDir(), L"*.cache");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        DeleteFileW(JoinPath(StoreDir(), fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// 顺序扫一遍记录建索引。只读头与 key，正文靠偏移跳过；
// 扫到损坏处就截断，保证后续追加不会写进垃圾里。
void LoadIndex() {
    g_index.clear();
    g_latest.clear();
    g_storeSeq = 0;
    g_storeEnd = 0;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(g_store, &sz) || sz.QuadPart <= 0) {
        ResetStore();
        return;
    }
    const unsigned long long total = (unsigned long long)sz.QuadPart;
    if (total < kStoreHdr) {
        ResetStore();
        return;
    }

    char magic[kStoreHdr] = {0};
    if (!ReadAt(0, magic, kStoreHdr) || memcmp(magic, kStoreMagic, kStoreHdr) != 0) {
        ResetStore();
        return;
    }

    unsigned long long pos = kStoreHdr;
    char hdrBuf[1024];
    while (pos + kRecHdr <= total) {
        size_t avail = (size_t)(total - pos);
        if (avail > sizeof(hdrBuf)) avail = sizeof(hdrBuf);
        if (!ReadAt(pos, hdrBuf, avail)) break;

        unsigned keyLen = 0, bodyLen = 0, seq = 0;
        memcpy(&keyLen, hdrBuf, 4);
        memcpy(&bodyLen, hdrBuf + 4, 4);
        memcpy(&seq, hdrBuf + 8, 4);

        unsigned long long recEnd = pos + kRecHdr + (unsigned long long)keyLen + bodyLen;
        if (keyLen > (1u << 20) || recEnd > total) break;   // 头损坏：当作到此为止

        std::string key;
        if (kRecHdr + keyLen <= avail) {
            key.assign(hdrBuf + kRecHdr, keyLen);
        } else {
            key.resize(keyLen);
            if (!ReadAt(pos + kRecHdr, &key[0], keyLen)) break;
        }

        StoredEntry e;
        e.off = pos;
        e.bodyLen = bodyLen;
        e.keyLen = keyLen;
        e.seq = seq;
        g_index[key] = e;
        std::string path = PathOfKey(key);
        std::unordered_map<std::string, unsigned>::iterator lit = g_latest.find(path);
        if (lit == g_latest.end() || lit->second < seq) g_latest[path] = seq;
        if (seq > g_storeSeq) g_storeSeq = seq;
        pos = recEnd;
    }

    g_storeEnd = pos;
    if (g_storeEnd < total) TruncateAt(g_storeEnd);   // 去掉尾部垃圾
    if (g_storeEnd == 0) {
        // 只有头或头都不完整
        WriteAt(0, kStoreMagic, kStoreHdr);
        g_storeEnd = kStoreHdr;
    }
}

bool StoreReady() {
    if (g_store != INVALID_HANDLE_VALUE) return true;
    std::wstring dir = StoreDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = StorePath();
    bool existed = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;

    g_store = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_store == INVALID_HANDLE_VALUE) return false;

    LoadIndex();
    if (!existed) DeleteLegacyCacheFiles();   // 一次性迁移：清掉旧版每文件缓存
    return true;
}

static void ParseBody(const std::string& body, std::vector<std::wstring>& lines) {
    lines.clear();
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t e2 = body.find('\n', pos);
        if (e2 == std::string::npos) {
            if (pos < body.size()) lines.push_back(FromUtf8(body.data() + pos, body.size() - pos));
            break;
        }
        lines.push_back(FromUtf8(body.data() + pos, e2 - pos));
        pos = e2 + 1;
    }
}

void Compact(unsigned long long target);

unsigned long long StoreBytesOnDisk() {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(StorePath().c_str(), GetFileExInfoStandard, &fad)) return 0;
    return ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

// 压缩：按写入序号从旧到新丢，直到估算总量降到 target 以内；活着的记录按原顺序重写
void Compact(unsigned long long target) {
    if (g_store == INVALID_HANDLE_VALUE) return;
    CacheGuard g;

    // 只保留每条路径最新的那条：旧版本本来就读不到，顺手清掉
    std::vector<std::pair<unsigned, std::string> > order;
    order.reserve(g_index.size());
    unsigned long long total = kStoreHdr;
    for (std::unordered_map<std::string, StoredEntry>::iterator it = g_index.begin();
         it != g_index.end(); ++it) {
        std::unordered_map<std::string, unsigned>::iterator lit = g_latest.find(PathOfKey(it->first));
        if (lit != g_latest.end() && lit->second != it->second.seq) continue;
        order.push_back(std::make_pair(it->second.seq, it->first));
        total += (unsigned long long)kRecHdr + it->second.keyLen + it->second.bodyLen;
    }
    std::sort(order.begin(), order.end());

    size_t drop = 0;
    while (total > target && drop < order.size()) {
        const StoredEntry& e = g_index[order[drop].second];
        total -= (unsigned long long)kRecHdr + e.keyLen + e.bodyLen;
        ++drop;
    }

    std::wstring tmpPath = JoinPath(StoreDir(), L"store.tmp");
    HANDLE hOut = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) return;

    DWORD wr = 0;
    WriteFile(hOut, kStoreMagic, kStoreHdr, &wr, nullptr);

    std::unordered_map<std::string, StoredEntry> newIndex;
    unsigned long long outPos = kStoreHdr;
    std::string body;
    for (size_t i = drop; i < order.size(); ++i) {
        const std::string& key = order[i].second;
        const StoredEntry e = g_index[key];

        body.resize(e.bodyLen);
        if (e.bodyLen && !ReadAt(e.off + kRecHdr + e.keyLen, &body[0], e.bodyLen)) continue;
        if (!e.bodyLen) body.clear();

        unsigned hdr[3] = { e.keyLen, e.bodyLen, e.seq };
        if (!WriteFile(hOut, hdr, kRecHdr, &wr, nullptr)) break;
        if (!WriteFile(hOut, key.data(), e.keyLen, &wr, nullptr)) break;
        if (e.bodyLen && !WriteFile(hOut, body.data(), e.bodyLen, &wr, nullptr)) break;

        StoredEntry ne;
        ne.off = outPos;
        ne.keyLen = e.keyLen;
        ne.bodyLen = e.bodyLen;
        ne.seq = e.seq;
        newIndex[key] = ne;
        outPos += (unsigned long long)kRecHdr + e.keyLen + e.bodyLen;
    }
    CloseHandle(hOut);

    std::wstring path = StorePath();
    CloseHandle(g_store);
    g_store = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmpPath.c_str());
        StoreReady();   // 换不了就继续用旧库（索引重新扫一遍）
        return;
    }

    g_store = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_index.swap(newIndex);
    g_latest.clear();
    for (std::unordered_map<std::string, StoredEntry>::iterator it = g_index.begin();
         it != g_index.end(); ++it) {
        std::string pth = PathOfKey(it->first);
        std::unordered_map<std::string, unsigned>::iterator lit = g_latest.find(pth);
        if (lit == g_latest.end() || lit->second < it->second.seq) g_latest[pth] = it->second.seq;
    }
    g_storeEnd = outPos;
    g_mem.clear();          // 内存里的副本仍然有效，但内容一定在库里，清掉更省内存
    g_lru.clear();
    g_memChars = 0;
}

}  // namespace

// ======================= 对外入口 =======================

void CacheSetLimitMB(int mb) {
    if (mb < 16) mb = 16;
    if (mb > 2048) mb = 2048;
    CacheGuard g;
    g_limitMB = mb;
}

unsigned long long CacheLimitBytes() {
    CacheGuard g;
    return MaxDiskBytes();
}

unsigned long long CacheUsedBytes() {
    CacheGuard g;
    return StoreBytesOnDisk();
}

void CacheBeginBatch() {
    CacheGuard g;
    ++g_batchDepth;
}

void CacheEndBatch() {
    std::lock_guard<std::mutex> io(g_storeIo);
    bool needCompact = false;
    {
        CacheGuard g;
        if (g_batchDepth > 0) --g_batchDepth;
        needCompact = (g_batchDepth == 0 && g_storeEnd > MaxDiskBytes());
    }
    if (needCompact) Compact(LowWaterBytes());
}

bool CacheGet(const std::wstring& path, unsigned size, unsigned long long mtime,
              std::vector<std::wstring>& lines) {
    std::string key = MakeKey(path, size, mtime);

    {
        CacheGuard g;
        std::unordered_map<std::string, MemEntry>::iterator it = g_mem.find(key);
        if (it != g_mem.end()) {
            lines = it->second.lines;
            TouchLru(key);
            return true;
        }
    }

    // 索引/元数据在 CacheCs 内取；正文读取只拿 g_storeIo，不阻塞内存命中。
    std::unique_lock<std::mutex> io(g_storeIo);
    StoredEntry e;
    {
        CacheGuard g;
        if (!StoreReady()) return false;
        std::unordered_map<std::string, StoredEntry>::iterator it = g_index.find(key);
        if (it == g_index.end() || it->second.bodyLen == 0) return false;
        std::unordered_map<std::string, unsigned>::iterator lit = g_latest.find(PathOfKey(key));
        if (lit != g_latest.end() && lit->second != it->second.seq) return false;
        e = it->second;
    }

    std::string body;
    body.resize(e.bodyLen);
    if (!ReadAt(e.off + kRecHdr + e.keyLen, &body[0], body.size())) {
        CacheGuard g;
        std::unordered_map<std::string, StoredEntry>::iterator it = g_index.find(key);
        if (it != g_index.end() && it->second.seq == e.seq) g_index.erase(it);
        return false;
    }
    io.unlock();

    ParseBody(body, lines);
    CacheGuard g;
    MemStore(key, lines);
    return true;
}

bool CacheHas(const std::wstring& path, unsigned size, unsigned long long mtime) {
    std::string key = MakeKey(path, size, mtime);
    {
        CacheGuard g;
        if (g_mem.find(key) != g_mem.end()) return true;
    }
    std::lock_guard<std::mutex> io(g_storeIo);
    CacheGuard g;
    if (!StoreReady()) return false;
    return g_index.find(key) != g_index.end();   // 纯内存判断，不再碰磁盘
}

// 清空内存缓存 + 删掉存储库与旧版遗留的缓存文件
void CacheClearAll() {
    std::lock_guard<std::mutex> io(g_storeIo);
    CacheGuard g;
    g_mem.clear();
    g_lru.clear();
    g_memChars = 0;

    if (g_store != INVALID_HANDLE_VALUE) {
        CloseHandle(g_store);
        g_store = INVALID_HANDLE_VALUE;
    }
    g_index.clear();
    g_latest.clear();
    g_storeEnd = 0;
    g_storeSeq = 0;

    DeleteFileW(StorePath().c_str());
    DeleteFileW(JoinPath(StoreDir(), L"store.tmp").c_str());
    DeleteLegacyCacheFiles();
}

void CachePut(const std::wstring& path, unsigned size, unsigned long long mtime,
              const std::vector<std::wstring>& lines) {
    if (lines.empty()) return;
    std::string key = MakeKey(path, size, mtime);

    std::string body;
    size_t approx = 0;
    for (size_t i = 0; i < lines.size(); ++i) approx += lines[i].size() * 3 + 1;
    body.reserve(approx);
    for (size_t i = 0; i < lines.size(); ++i) {
        body += Utf8(lines[i]);
        body += '\n';
    }

    {
        CacheGuard g;
        MemStore(key, lines);
    }

    unsigned keyLen = (unsigned)key.size();
    unsigned bodyLen = (unsigned)body.size();
    std::string rec;
    rec.resize((size_t)kRecHdr + key.size() + body.size());
    memcpy(&rec[0], &keyLen, 4);
    memcpy(&rec[4], &bodyLen, 4);
    memcpy(&rec[kRecHdr], key.data(), key.size());
    memcpy(&rec[kRecHdr + key.size()], body.data(), body.size());

    std::unique_lock<std::mutex> io(g_storeIo);
    unsigned seq = 0;
    unsigned long long off = 0;
    bool needCompact = false;
    {
        CacheGuard g;
        if (!StoreReady()) return;
        seq = ++g_storeSeq;
        memcpy(&rec[8], &seq, 4);
        off = g_storeEnd;

        StoredEntry e;
        e.off = off;
        e.bodyLen = bodyLen;
        e.keyLen = keyLen;
        e.seq = seq;
        g_index[key] = e;
        g_latest[PathOfKey(key)] = seq;
        g_storeEnd += rec.size();
        needCompact = (g_batchDepth == 0 && g_storeEnd > MaxDiskBytes());
    }

    if (!WriteAt(off, rec.data(), rec.size())) {
        CacheGuard g;
        std::unordered_map<std::string, StoredEntry>::iterator it = g_index.find(key);
        if (it != g_index.end() && it->second.seq == seq) g_index.erase(it);
        if (g_storeEnd == off + rec.size()) {
            TruncateAt(off);
            g_storeEnd = off;
        }
        return;
    }

    if (needCompact) Compact(LowWaterBytes());
}
