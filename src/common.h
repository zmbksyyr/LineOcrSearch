// common.h - 公共定义、设置结构、常量
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <memory>

// ---- 窗口消息 ----
#define WM_APP_RESULTS (WM_APP + 1)   // 后台搜索完成
#define WM_APP_HINT    (WM_APP + 2)   // 状态栏提示更新
#define WM_APP_CAPTURE (WM_APP + 3)   // 取词完成（后台线程）

// ---- 控件 ID ----
#define IDC_EDIT      1001
#define IDC_BTN_SEARCH 1002
#define IDC_BTN_SET   1003
#define IDC_LIST      1004
#define IDC_STATUSBAR 1005

// ---- 设置项（界面上 10 项）----
struct Settings {
    std::wstring rootDir;      // 1 搜索文件夹
    int  lingerMs     = 500;   // 2 鼠标停多久自动取词（200-1000ms）
    bool autoCapture  = true;  // 3 自动取词
    bool autoSearch   = true;  // 4 取词后自动搜索
    int  captureMode  = 0;     // 5 取词方式 0 智能 / 1 自动 / 2 总是 UIA / 3 总是图片
    bool backtrack    = true;  // 6 搜不到时自动缩短关键词
    bool searchOffice = true;  // 7 同时搜索 Word / Excel
    int  contextLevel = 1;     // 8 前后文 0 简洁 / 1 标准 / 2 详细
    int  maxResults   = 50;    // 9 最多显示多少条：10 / 20 / 50 / 100
    int  cacheMB      = 64;    // 10 缓存上限（MB）：64 / 128 / 256 / 512
};

// ---- 结果项 ----
struct HitSpan {
    int start = 0;                      // 命中词在行内的起始位置（字符）
    int len   = 0;                      // 命中词长度
};

struct ResultItem {
    std::wstring path;                  // 完整路径
    std::wstring relPath;               // 相对根目录
    std::wstring fileName;
    int lineNo = 0;                     // 1 基
    std::vector<std::wstring> context;  // 上下文行
    int hitIndex = 0;                   // 命中行在 context 中的下标
    int hitStart = 0;                   // 第一个命中词的起始位置（兼容旧字段）
    int hitLen   = 0;                   // 第一个命中词长度
    std::vector<HitSpan> hitRanges;     // 全部命中区间（按 start 排序、互不重叠）；
                                        // 单关键词时只有一段，多关键词 AND 时每词一段
};

struct SearchRequest {
    std::wstring keyword;
    std::wstring rootDir;
    bool searchOffice  = true;
    int  contextLevel  = 1;
    int  maxResults    = 200;
};

struct SearchOutcome {
    std::wstring keyword;
    std::vector<ResultItem> items;    // 当前显示的（按文件分组、组内按行号）
    std::vector<ResultItem> pending;  // 已命中但受「最多显示」限制暂存的，供「加载更多」
    int  scannedFiles = 0;
    int  totalHits    = 0;            // 命中总条数（跨所有文件，受收集上限约束）
    int  totalFiles   = 0;            // 有命中的文件数
    bool truncated    = false;        // 收集阶段撞到上限，还有内容没进来
};

// ---- 数据目录布局（目录名集中在这里，改组织方式只动这一处）----
// 数据目录只在 exe 首次运行释放资源时创建，源码树里不存在；
// 源码树里的 res\ 只是打包输入，两者不是一回事。
namespace cfgpaths {
    constexpr const wchar_t* kConfigDir  = L"LineOcrSearchData";  // exe 同级的数据文件夹
    constexpr const wchar_t* kLegacyDir  = L"config";             // 更早的目录名
    constexpr const wchar_t* kLegacyDir2 = L"LineOcrSearch";      // 上一版的目录名
    constexpr const wchar_t* kIniName    = L"config.ini";
    constexpr const wchar_t* kRuntimeDir = L"runtime";
    constexpr const wchar_t* kModelDir   = L"model";
    constexpr const wchar_t* kCacheDir   = L"cache";
}

// ---- 路径工具 ----
std::wstring ExeDir();
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
std::wstring ConfigDirPath();
std::wstring ConfigIniPath();
std::wstring SubDirPath(const wchar_t* sub);
bool         FileExists(const std::wstring& p);
// 旧版本的数据目录（config、LineOcrSearch）在新目录不存在时整目录改名过来，
// 避免重新释放 21MB 依赖、也不丢已有缓存。改名失败就当没这一步。
void         MigrateLegacyDataDir();

// ---- config ----
Settings DefaultSettings();
bool     LoadSettings(Settings& s);
bool     SaveSettings(const Settings& s);

// ---- 文本清洗 ----
// 规则：整行按「非纯文字」（数字 / 标点 / 符号）切成若干纯文字段，再挑一段当关键词。
// CleanOcrTextByX：以鼠标右侧为准 —— 取「包含鼠标、或位于鼠标右侧」的第一段。
//   charX 为每个字符的水平中心，与 mouseX 同一坐标系，长度必须与 raw 相同。
// CleanOcrText：没有坐标信息时退化为「取最长的一段」。
std::wstring CleanOcrText(const std::wstring& raw);
std::wstring CleanOcrTextByX(const std::wstring& raw,
                             const std::vector<double>& charX, double mouseX);
// 笔尖左锚点取词（主链路）：笔尖只是「从这里开始」的标记，不必压在字上。
//   1) 找笔尖右侧第一个文字字符（笔尖落在某个字内部时，从该字算起）
//   2) 从它截到行尾
//   3) 两端去噪（空白 / 标点 / 符号 / 开头编号），中间原样保留
// 认不出有效文字时返回空串，由调用方决定是否回退。
// outBegin/outEnd（可选）返回这段在 raw 里的下标区间，便于就地替换文字而保留坐标。
std::wstring TakeRightOfCursor(const std::wstring& raw,
                               const std::vector<double>& charX, double mouseX,
                               size_t* outBegin = nullptr, size_t* outEnd = nullptr);
// 两段文字的字符重合度 0..1（字符频次交集 / 较长者长度）。
// 用来判断 UIA 与图片认的是不是同一句：差距大就以图片为准。
double TextSimilarity(const std::wstring& a, const std::wstring& b);
int          CountTextChars(const std::wstring& s);

// ---- 搜索（阻塞，调用方放后台线程）----
SearchOutcome RunSearch(const SearchRequest& req);

// ---- Office 文本抽取（docx / xlsx / xlsm；老 .doc/.xls 走 legacy_office.cpp）----
bool ExtractOfficeText(const std::wstring& path, std::wstring& outText);
// Word 97-2003 / Excel 97-2003（OLE 复合文档）正文抽取；不支持加密文档与 95 及更早格式
bool ExtractLegacyOfficeText(const std::wstring& path, std::wstring& outText);

// ---- 文档提取缓存（内存 LRU + 数据目录下的 cache\）----
bool CacheGet(const std::wstring& path, unsigned size, unsigned long long mtime,
              std::vector<std::wstring>& lines);
void CachePut(const std::wstring& path, unsigned size, unsigned long long mtime,
              const std::vector<std::wstring>& lines);
// 只判断这份文件是否已有可用缓存，不把正文读进内存（预建缓存时用）
bool CacheHas(const std::wstring& path, unsigned size, unsigned long long mtime);
void CacheClearAll();          // 清空内存与 cache\ 下的缓存文件
void CacheSetLimitMB(int mb);  // 磁盘上限（MB），启动与改设置时调用；降到 80% 为止
unsigned long long CacheLimitBytes();  // 当前上限（字节）
unsigned long long CacheUsedBytes();   // cache\ 当前占用（字节）
// 批量写入：期间不做逐次配额检查（几千个文件时避免反复扫目录），结束时统一查一次
void CacheBeginBatch();
void CacheEndBatch();

// ---- 为整个目录预建缓存（浏览目录后 / 点“生成缓存”时调用）----
struct CacheBuildStats {
    int  found  = 0;        // 支持的类型（文本 / docx / xlsx / xlsm）的文件数
    int  built  = 0;        // 本次写入缓存
    int  reused = 0;        // 已有可用缓存，直接复用
    int  failed = 0;        // 读取或解析失败，未缓存
    int  tooBig = 0;        // 超过单文件上限，跳过
    bool quotaFull = false; // 达到缓存上限，提前收工
    bool canceled  = false; // 用户中途停止
};
// progress 返回 false 表示停止；返回后 stats.canceled 为 true
typedef bool (*CacheBuildProgressFn)(void* user, const CacheBuildStats& st);
CacheBuildStats CacheBuildForDir(const std::wstring& dir,
                                 CacheBuildProgressFn progress, void* user);

// ---- C1 文字层取词（主线程调用；内部走 COM / Win32，不截屏）----
// 成功返回 true，text 为鼠标所在那一行的整行文字，source 为命中通道名；
// outRect（可选）返回这一行在屏幕上的矩形；
// charX（可选）返回每个字符在屏幕上的水平中心，用于判断鼠标指向行内哪一段
bool CaptureLineAtCursor(std::wstring& text, std::wstring& source,
                         RECT* outRect = nullptr,
                         std::vector<double>* charX = nullptr);

// ---- 局部条带截屏（只取鼠标附近，绝不截全屏，像素只在内存）----
struct CapturedBgra {
    int w = 0, h = 0;
    int mouseLocalX = 0, mouseLocalY = 0;
    std::vector<unsigned char> px;  // BGRA，自上而下
};
bool CaptureBand(int halfW, int halfH, CapturedBgra& out);
// 右偏条带：光标左侧只留 leftPad，右侧取 width（取词主链路用这个）
bool CaptureBandRight(int leftPad, int width, int halfH, CapturedBgra& out);
void WipeBytes(std::vector<unsigned char>& buf);
void EnablePerMonitorDpi();

// ---- C2 模型识别 ----
bool        OcrInit(std::wstring& err);
void        OcrShutdown();
bool        OcrReady();
// mouseX / mouseY 为光标在条带内的坐标：
//   Y 决定「第几行」，X 决定「这一行在笔尖右侧有没有字」——右侧整段空白的行不参与候选。
bool        OcrRecognizeBgra(const unsigned char* bgra, int w, int h,
                             int mouseX, int mouseY,
                             std::wstring& text, int* lineTop, int* lineBottom,
                             std::vector<double>* charX = nullptr);
std::wstring OcrEngineInfo();

// ---- 对齐：把 OCR 认出来的一行对到 UIA 全文的对应片段 ----
// 位置必须来自真实像素（UIA 在 Chromium 上只给块级矩形），文字要用 UIA 原文，
// 所以两边做对齐：doc 给字、OCR 给位置。
struct AlignResult {
    bool   ok = false;
    double ratio = 0.0;            // 对齐命中率 0..1
    size_t hayStart = 0;           // 在 doc 里的起始下标（含）
    size_t hayEnd = 0;             // 结束下标（不含）
    std::vector<int> needleToDoc;  // 长度 = line.size()，-1 表示未对上
};
AlignResult AlignLineToDocument(const std::wstring& doc, const std::wstring& line,
                                double minRatio);

// 用对齐结果把 OCR 的每字像素位置搬到 doc 的对应片段上（空洞线性补齐）
void BuildAlignedLine(const std::wstring& doc, const AlignResult& ar,
                      const std::vector<double>& lineCharX,
                      std::wstring& outText, std::vector<double>& outCharX);

// ---- 文字层取词（拆成两个入口，便于「智能」模式按需要选路）----
// 原生 EDIT / RichEdit：位置精确（EM_POSFROMCHAR），全程不截屏
bool CaptureNativeEditLine(std::wstring& text, std::wstring& source,
                           RECT* outRect = nullptr,
                           std::vector<double>* charX = nullptr);
// UIA / MSAA：能拿到文字，但位置只能按元素矩形估算
bool CaptureTextLayer(POINT pt, std::wstring& text, std::wstring& source,
                      RECT* outRect = nullptr,
                      std::vector<double>* charX = nullptr);
// 取鼠标所在窗口的 UIA 文档全文（Chromium/Electron 可用，带 2 秒缓存）
bool GetUiaDocumentText(POINT pt, const std::wstring& hint, std::wstring& text);

// ---- 内嵌资源释放 ----
// 依赖（推理库 / 模型 / 字典）打包进 exe 末尾；启动时逐项比对数据目录下的文件，
// 缺失或大小不符才写盘。返回本次释放的文件个数，note 为可显示给用户的说明。
int  ExtractBundledResources(std::wstring& note);

// ---- 设置窗口 ----
bool ShowSettingsDialog(HWND parent, Settings& s, const std::wstring& capNote);

// ---- 应用图标（代码绘制，返回后由调用方负责 DestroyIcon）----
HICON MakeAppIcon(int size);

// ---- 能力探测 ----
struct Capabilities {
    bool c1Text      = false;  // 文字层
    bool c2Model     = false;  // 随包模型
    std::wstring note;         // 状态栏说明
};
Capabilities ProbeCapabilities();
