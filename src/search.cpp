// search.cpp - 目录遍历、编码探测、全文匹配、上下文提取
#include "common.h"
#include <algorithm>

// ---- 内置常量（不进设置界面）----
static const wchar_t* kTextExts[] = {
    L".txt", L".md", L".log", L".csv", L".tsv", L".json", L".xml", L".yaml", L".yml",
    L".ini", L".cfg", L".conf", L".toml", L".properties",
    L".c", L".h", L".hpp", L".cpp", L".cc", L".cxx", L".cs", L".java", L".py",
    L".js", L".ts", L".jsx", L".tsx", L".css", L".html", L".htm", L".sql",
    L".bat", L".cmd", L".ps1", L".sh", L".go", L".rs", L".php", L".rb", L".lua",
    L".srt", L".vtt", L".tex",
};

static const wchar_t* kSkipDirs[] = {
    L".git", L".svn", L".hg", L"node_modules", L"__pycache__", L".idea", L".vs",
    L"bin", L"obj", L"target", L"dist", L"build", L".venv", L"venv", L".cache",
};

static const int   kMaxFileBytes  = 2 * 1024 * 1024;  // 单文件上限 2 MB
static const int   kMaxDepth      = 32;               // 递归深度上限，防目录环
static const int   kHitsPerFile   = 3;                // 同一文件最多列 3 处命中
static const int   kMaxScanned    = 20000;            // 单次搜索最多检查的文件数

// ---------------- 小工具 ----------------

static std::wstring UpperW(const std::wstring& s) {
    std::wstring t = s;
    if (!t.empty()) CharUpperBuffW(&t[0], (DWORD)t.size());
    return t;
}

// OCR 常把半角逗号识别成全角「，」，这里把全角 ASCII 区逐字符折成半角再比对。
// 映射是 1:1 的，字符位置不变，所以命中位置仍然可以直接用于高亮。
static wchar_t NormWidthChar(wchar_t c) {
    if (c >= 0xFF01 && c <= 0xFF5E) return (wchar_t)(c - 0xFEE0);
    if (c == 0x3000) return L' ';
    return c;
}

static std::wstring NormalizeWidth(const std::wstring& s) {
    std::wstring t = s;
    for (size_t i = 0; i < t.size(); ++i) t[i] = NormWidthChar(t[i]);
    return t;
}

static std::wstring LowerExt(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return std::wstring();
    std::wstring e = name.substr(dot);
    if (!e.empty()) CharLowerBuffW(&e[0], (DWORD)e.size());
    return e;
}

static bool IsTextExt(const std::wstring& lowerExt) {
    for (size_t i = 0; i < sizeof(kTextExts) / sizeof(kTextExts[0]); ++i) {
        if (lowerExt == kTextExts[i]) return true;
    }
    return false;
}

static bool IsSkippedDir(const std::wstring& name) {
    std::wstring lower = name;
    if (!lower.empty()) CharLowerBuffW(&lower[0], (DWORD)lower.size());
    for (size_t i = 0; i < sizeof(kSkipDirs) / sizeof(kSkipDirs[0]); ++i) {
        if (lower == kSkipDirs[i]) return true;
    }
    return false;
}

// 编码探测：BOM → UTF-8 严格 → 系统 ANSI（中文系统为 GBK）
static bool DecodeBytes(const std::string& raw, std::wstring& out) {
    out.clear();
    if (raw.empty()) return true;
    const unsigned char* p = (const unsigned char*)raw.data();
    size_t n = raw.size();

    // UTF-8 BOM
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, (const char*)p + 3, (int)(n - 3), nullptr, 0);
        if (wlen > 0) {
            out.resize((size_t)wlen);
            MultiByteToWideChar(CP_UTF8, 0, (const char*)p + 3, (int)(n - 3), &out[0], wlen);
            return true;
        }
        return false;
    }
    // UTF-16 LE / BE
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        size_t chars = (n - 2) / 2;
        out.assign((const wchar_t*)(p + 2), chars);
        return true;
    }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        size_t chars = (n - 2) / 2;
        out.resize(chars);
        const unsigned char* q = p + 2;
        for (size_t i = 0; i < chars; ++i) {
            out[i] = (wchar_t)((q[i * 2] << 8) | q[i * 2 + 1]);
        }
        return true;
    }

    // 无 BOM：先按 UTF-8 严格校验
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)n, nullptr, 0);
    if (wlen > 0) {
        out.resize((size_t)wlen);
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)n, &out[0], wlen);
        return true;
    }
    // 退回系统 ANSI 代码页（不假定区域：用 CP_ACP 而不是硬编码 936）
    wlen = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)n, nullptr, 0);
    if (wlen > 0) {
        out.resize((size_t)wlen);
        MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)n, &out[0], wlen);
        return true;
    }
    return false;
}

static void SplitLines(const std::wstring& text, std::vector<std::wstring>& lines) {
    lines.clear();
    lines.reserve(text.size() / 32 + 8);
    std::wstring cur;
    cur.reserve(128);
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t c = text[i];
        if (c == L'\n') {
            lines.push_back(cur);
            cur.clear();
        } else if (c == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) lines.push_back(cur);
}

static bool ReadWholeFile(const std::wstring& path, int maxBytes, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > maxBytes) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    BOOL ok = ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(got);
    return true;
}

// 按上下文等级决定前后行数
static void ContextSpan(int level, int& before, int& after) {
    switch (level) {
        case 0: before = 0; after = 2; break;   // 简洁
        case 2: before = 3; after = 8; break;   // 详细
        default: before = 1; after = 4; break;  // 标准（前 1 后 4）
    }
}

// ---------------- Office 接口（office.cpp 实现）----------------
bool ExtractOfficeText(const std::wstring& path, std::wstring& outText);

// ---------------- 主搜索 ----------------

struct Walker {
    const SearchRequest* req = nullptr;
    SearchOutcome* out = nullptr;
    std::vector<std::wstring> kws;      // 拆分后的关键词（已归一化 + 大写）；多词 = AND
    int before = 1, after = 4;

    void HandleFile(const std::wstring& full, const std::wstring& rel, const std::wstring& name,
                    unsigned size, unsigned long long mtime);
    void Walk(const std::wstring& dir, const std::wstring& rel, int depth);
};

void Walker::HandleFile(const std::wstring& full, const std::wstring& rel, const std::wstring& name,
                        unsigned size, unsigned long long mtime) {
    if ((int)out->items.size() >= req->maxResults) { out->truncated = true; return; }

    std::wstring ext = LowerExt(name);
    bool isText = IsTextExt(ext);
    bool isOffice = (ext == L".docx" || ext == L".xlsx" || ext == L".xlsm");
    if (!isText && !isOffice) return;
    if (isOffice && !req->searchOffice) return;

    out->scannedFiles++;

    std::vector<std::wstring> lines;
    if (!CacheGet(full, size, mtime, lines)) {
        std::wstring text;
        if (isText) {
            std::string raw;
            if (!ReadWholeFile(full, kMaxFileBytes, raw)) return;
            if (!DecodeBytes(raw, text)) return;
            SplitLines(text, lines);
        } else {
            if (!ExtractOfficeText(full, text)) return;
            SplitLines(text, lines);
        }
        // 超大文档不缓存，避免内存与缓存目录膨胀
        if (lines.size() <= 50000) CachePut(full, size, mtime, lines);
    }
    if (lines.empty()) return;

    int hits = 0;
    for (size_t i = 0; i < lines.size() && hits < kHitsPerFile; ++i) {
        if ((int)out->items.size() >= req->maxResults) { out->truncated = true; break; }

        std::wstring lineUpper = UpperW(NormalizeWidth(lines[i]));

        // 每个关键词都要在这一行出现（多关键词 AND）；任何一个找不到就跳过
        std::vector<HitSpan> found;
        bool all = true;
        for (size_t k = 0; k < kws.size(); ++k) {
            size_t p = lineUpper.find(kws[k]);
            if (p == std::wstring::npos) { all = false; break; }
            HitSpan hs;
            hs.start = (int)p;
            hs.len = (int)kws[k].size();
            found.push_back(hs);
        }
        if (!all) continue;

        // 按 start 排序后合并重叠区间（比如「应收」「账款」相邻或交叠）
        std::sort(found.begin(), found.end(),
                  [](const HitSpan& a, const HitSpan& b) { return a.start < b.start; });
        std::vector<HitSpan> merged;
        for (size_t k = 0; k < found.size(); ++k) {
            if (!merged.empty() && found[k].start <= merged.back().start + merged.back().len) {
                int end = found[k].start + found[k].len;
                int oldEnd = merged.back().start + merged.back().len;
                if (end > oldEnd) merged.back().len = end - merged.back().start;
            } else {
                merged.push_back(found[k]);
            }
        }

        ResultItem it;
        it.path = full;
        it.relPath = rel;
        it.fileName = name;
        it.lineNo = (int)i + 1;
        it.hitIndex = -1;
        it.hitStart = merged[0].start;
        it.hitLen = merged[0].len;
        it.hitRanges = merged;

        int from = (int)i - before; if (from < 0) from = 0;
        int to = (int)i + after;   if (to > (int)lines.size() - 1) to = (int)lines.size() - 1;
        for (int k = from; k <= to; ++k) {
            if (k == (int)i) it.hitIndex = (int)it.context.size();
            it.context.push_back(lines[k]);
        }
        out->items.push_back(std::move(it));
        hits++;
    }
}

void Walker::Walk(const std::wstring& dir, const std::wstring& rel, int depth) {
    if (depth > kMaxDepth) return;
    if ((int)out->items.size() >= req->maxResults) { out->truncated = true; return; }
    if (out->scannedFiles > kMaxScanned) return;

    std::wstring pattern = JoinPath(dir, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        const wchar_t* nm = fd.cFileName;
        if (nm[0] == L'.' && (nm[1] == L'\0' || (nm[1] == L'.' && nm[2] == L'\0'))) continue;

        std::wstring name(nm);
        std::wstring full = JoinPath(dir, name);
        std::wstring childRel = rel.empty() ? name : JoinPath(rel, name);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (IsSkippedDir(name)) continue;
            Walk(full, childRel, depth + 1);
        } else {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            if (fd.nFileSizeHigh != 0 || fd.nFileSizeLow > (DWORD)kMaxFileBytes) {
                // 超限文件跳过；Office 也一并跳过，避免卡顿
                continue;
            }
            unsigned long long mtime =
                ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) |
                (unsigned long long)fd.ftLastWriteTime.dwLowDateTime;
            HandleFile(full, childRel, name, (unsigned)fd.nFileSizeLow, mtime);
        }
        if ((int)out->items.size() >= req->maxResults) { out->truncated = true; break; }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

SearchOutcome RunSearch(const SearchRequest& req) {
    SearchOutcome out;
    out.keyword = req.keyword;
    if (req.keyword.empty() || req.rootDir.empty()) return out;

    // 关键词按空白拆分：单词 = 普通子串匹配（行为与旧版一致）；
    // 多词 = AND，要求所有词出现在同一行里
    Walker w;
    w.req = &req;
    w.out = &out;
    std::wstring cur;
    for (size_t i = 0; i <= req.keyword.size(); ++i) {
        wchar_t c = (i < req.keyword.size()) ? req.keyword[i] : L' ';
        if (c == L' ' || c == L'\t' || c == 0x3000) {
            if (!cur.empty()) {
                w.kws.push_back(UpperW(NormalizeWidth(cur)));
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (w.kws.empty()) return out;
    ContextSpan(req.contextLevel, w.before, w.after);
    w.Walk(req.rootDir, std::wstring(), 0);
    return out;
}
