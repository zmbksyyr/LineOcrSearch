// config.cpp - 路径工具、config.ini 读写（UTF-8 BOM，支持中文路径）
#include "common.h"
#include <shlobj.h>
#include <cstdio>

// ---------------- 路径 ----------------

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    std::wstring p(buf);
    size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring(L".") : p.substr(0, pos);
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    wchar_t last = a[a.size() - 1];
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring ConfigDirPath() { return JoinPath(ExeDir(), cfgpaths::kConfigDir); }
std::wstring ConfigIniPath() { return JoinPath(ConfigDirPath(), cfgpaths::kIniName); }
std::wstring SubDirPath(const wchar_t* sub) { return JoinPath(ConfigDirPath(), sub); }

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void MigrateLegacyDataDir() {
    std::wstring dst = ConfigDirPath();
    if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    // 先从最近的旧名字试起：LineOcrSearch（上一版）→ config（更早）
    const wchar_t* legacy[] = { cfgpaths::kLegacyDir2, cfgpaths::kLegacyDir };
    for (int i = 0; i < 2; ++i) {
        std::wstring old = JoinPath(ExeDir(), legacy[i]);
        DWORD a = GetFileAttributesW(old.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (MoveFileExW(old.c_str(), dst.c_str(), 0)) return;
    }
}

// ---------------- 编码小工具 ----------------

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const char* p, int len) {
    if (len <= 0) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, p, len, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p, len, &out[0], n);
    return out;
}

static std::wstring TrimW(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) b++;
    while (e > b && (s[e-1] == L' ' || s[e-1] == L'\t' || s[e-1] == L'\r' || s[e-1] == L'\n')) e--;
    return s.substr(b, e - b);
}

static int ParseInt(const std::wstring& v, int defVal) {
    if (v.empty()) return defVal;
    int x = _wtoi(v.c_str());
    return x == 0 && v != L"0" ? defVal : x;
}

static bool ParseBool(const std::wstring& v, bool defVal) {
    if (v.empty()) return defVal;
    return v == L"1" || v == L"true" || v == L"yes" || v == L"on";
}

// ---------------- 默认值 ----------------

Settings DefaultSettings() {
    Settings s;
    wchar_t docs[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) {
        s.rootDir = docs;
    }
    return s;
}

// ---------------- 读 ----------------

bool LoadSettings(Settings& s) {
    s = DefaultSettings();

    std::wstring path = ConfigIniPath();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    std::string raw((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    BOOL ok = ReadFile(h, &raw[0], (DWORD)raw.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    raw.resize(got);

    // 去 UTF-8 BOM
    size_t off = 0;
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) {
        off = 3;
    }
    std::wstring text = Utf8ToWide(raw.data() + off, (int)(raw.size() - off));

    bool hasModeVersion = false;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = (nl == std::wstring::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        if (nl == std::wstring::npos) pos = text.size() + 1; else pos = nl + 1;

        line = TrimW(line);
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = TrimW(line.substr(0, eq));
        std::wstring v = TrimW(line.substr(eq + 1));

        if (k == L"rootDir")           s.rootDir = v;
        else if (k == L"lingerMs")     s.lingerMs = ParseInt(v, s.lingerMs);
        else if (k == L"autoCapture")  s.autoCapture = ParseBool(v, s.autoCapture);
        else if (k == L"autoSearch")   s.autoSearch = ParseBool(v, s.autoSearch);
        else if (k == L"captureMode")  s.captureMode = ParseInt(v, s.captureMode);
        else if (k == L"backtrack")    s.backtrack = ParseBool(v, s.backtrack);
        else if (k == L"searchOffice") s.searchOffice = ParseBool(v, s.searchOffice);
        else if (k == L"contextLevel") s.contextLevel = ParseInt(v, s.contextLevel);
        else if (k == L"maxResults")   s.maxResults = ParseInt(v, s.maxResults);
        else if (k == L"cacheMB")      s.cacheMB = ParseInt(v, s.cacheMB);
        else if (k == L"modeVersion")  hasModeVersion = ParseInt(v, 0) >= 2;
    }

    // 收敛到合法范围
    if (s.lingerMs < 200) s.lingerMs = 200;
    if (s.lingerMs > 1000) s.lingerMs = 1000;
    // 旧配置（没有 modeVersion）里 0/1/2 = 自动 / 只读文字 / 总是图片，
    // 新版插入了「智能」到 0 位，需要整体后移一位，否则会悄悄换成别的档
    if (!hasModeVersion) {
        if (s.captureMode < 0) s.captureMode = 1;
        else s.captureMode = (s.captureMode > 2 ? 3 : s.captureMode + 1);
    }
    if (s.captureMode < 0 || s.captureMode > 3) s.captureMode = 1;
    if (s.contextLevel < 0 || s.contextLevel > 2) s.contextLevel = 1;

    // 结果条数只有 10 / 20 / 50 / 100 四档，旧配置里的其它值就近归位
    {
        const int opts[4] = { 10, 20, 50, 100 };
        int best = 50, bestDiff = 1 << 30;
        for (int i = 0; i < 4; ++i) {
            int d = s.maxResults - opts[i];
            if (d < 0) d = -d;
            if (d < bestDiff) { bestDiff = d; best = opts[i]; }
        }
        s.maxResults = best;
    }

    // 缓存上限同样只有 64 / 128 / 256 / 512 四档
    {
        const int opts[4] = { 64, 128, 256, 512 };
        int best = 64, bestDiff = 1 << 30;
        for (int i = 0; i < 4; ++i) {
            int d = s.cacheMB - opts[i];
            if (d < 0) d = -d;
            if (d < bestDiff) { bestDiff = d; best = opts[i]; }
        }
        s.cacheMB = best;
    }
    return true;
}

// ---------------- 写 ----------------

bool SaveSettings(const Settings& s) {
    // 确保数据目录存在（通常已由启动时的资源释放流程建好；万一被删则补建）
    CreateDirectoryW(ConfigDirPath().c_str(), nullptr);

    std::wstring text;
    text += L"# 行取词搜索 · 用户设置\n";
    text += L"# 由程序在点「确定」时写入，其余时间只读\n";
    text += L"rootDir=" + s.rootDir + L"\n";
    text += L"lingerMs=" + std::to_wstring(s.lingerMs) + L"\n";
    text += L"autoCapture=" + std::wstring(s.autoCapture ? L"1" : L"0") + L"\n";
    text += L"autoSearch=" + std::wstring(s.autoSearch ? L"1" : L"0") + L"\n";
    text += L"captureMode=" + std::to_wstring(s.captureMode) + L"\n";
    text += L"modeVersion=2\n";
    text += L"backtrack=" + std::wstring(s.backtrack ? L"1" : L"0") + L"\n";
    text += L"searchOffice=" + std::wstring(s.searchOffice ? L"1" : L"0") + L"\n";
    text += L"contextLevel=" + std::to_wstring(s.contextLevel) + L"\n";
    text += L"maxResults=" + std::to_wstring(s.maxResults) + L"\n";
    text += L"cacheMB=" + std::to_wstring(s.cacheMB) + L"\n";

    std::string utf8 = WideToUtf8(text);
    std::string out;
    out.push_back((char)0xEF);
    out.push_back((char)0xBB);
    out.push_back((char)0xBF);
    out += utf8;

    std::wstring path = ConfigIniPath();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, out.data(), (DWORD)out.size(), &written, nullptr);
    CloseHandle(h);
    return ok && written == out.size();
}
