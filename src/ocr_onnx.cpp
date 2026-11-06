// ocr_onnx.cpp - C2 模型识别：只做「鼠标所在行右侧」的投影定位 + 归一化 + CTC 解码
//
// 行定位不依赖检测模型：以鼠标 Y 为锚点，只统计鼠标右侧的墨迹，
// 找到真正包含鼠标的那一行；右侧没有字、或第一个字离鼠标太远，都直接失败。
#include "common.h"
#include "../vendor/onnxruntime_c_api.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <atomic>

// ---------------- ORT 全局状态 ----------------

static HMODULE              g_dll   = nullptr;
static const OrtApi*        g_ort   = nullptr;
static OrtEnv*              g_env   = nullptr;
static OrtSession*          g_sess  = nullptr;
static OrtSessionOptions*   g_opts  = nullptr;
static OrtMemoryInfo*       g_mem   = nullptr;
static std::string          g_inName;
static std::string          g_outName;
static std::vector<std::wstring> g_dict;
static std::wstring         g_info;
static std::mutex           g_initMutex;
static std::atomic<bool>    g_ready{ false };

// ---------------- 小工具 ----------------

static std::wstring U2W(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static bool ReadAllBytes(const std::wstring& path, std::vector<char>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)512 * 1024 * 1024) {
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

// ---------------- 字典 ----------------

static bool LoadDict(const std::wstring& path) {
    std::vector<char> raw;
    if (!ReadAllBytes(path, raw)) return false;

    std::string utf8;
    size_t off = 0;
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) {
        off = 3;
    }
    utf8.assign(raw.begin() + (long)off, raw.end());

    g_dict.clear();
    size_t pos = 0;
    while (pos < utf8.size()) {
        size_t nl = utf8.find('\n', pos);
        std::string line = (nl == std::string::npos) ? utf8.substr(pos) : utf8.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) g_dict.push_back(U2W(line));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return !g_dict.empty();
}

// ---------------- 初始化 ----------------

bool OcrReady() { return g_ready; }

std::wstring OcrEngineInfo() { return g_info; }

static bool Fail(std::wstring& err, const wchar_t* msg) {
    err = msg;
    return false;
}

bool OcrInit(std::wstring& err) {
    std::lock_guard<std::mutex> lock(g_initMutex);
    if (g_ready) return true;

    std::wstring dllPath = JoinPath(SubDirPath(cfgpaths::kRuntimeDir), L"onnxruntime.dll");
    std::wstring mdlPath = JoinPath(SubDirPath(cfgpaths::kModelDir), L"rec.onnx");
    std::wstring dicPath = JoinPath(SubDirPath(cfgpaths::kModelDir), L"ppocr_keys_v1.txt");

    if (!FileExists(dllPath)) return Fail(err, (std::wstring(L"缺少 ") + cfgpaths::kConfigDir + L"\\runtime\\onnxruntime.dll").c_str());
    if (!FileExists(mdlPath)) return Fail(err, (std::wstring(L"缺少 ") + cfgpaths::kConfigDir + L"\\model\\rec.onnx").c_str());
    if (!FileExists(dicPath)) return Fail(err, (std::wstring(L"缺少 ") + cfgpaths::kConfigDir + L"\\model\\ppocr_keys_v1.txt").c_str());

    g_dll = LoadLibraryExW(dllPath.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_dll) return Fail(err, L"onnxruntime.dll 加载失败（可能缺 VC++ 运行库或被策略拦截）");

    typedef const OrtApiBase*(ORT_API_CALL* GetApiBaseFn)(void);
    GetApiBaseFn getBase = (GetApiBaseFn)GetProcAddress(g_dll, "OrtGetApiBase");
    if (!getBase) return Fail(err, L"onnxruntime.dll 缺少 OrtGetApiBase");

    const OrtApiBase* base = getBase();
    if (!base) return Fail(err, L"OrtGetApiBase 返回空");
    g_ort = base->GetApi(ORT_API_VERSION);
    if (!g_ort) return Fail(err, L"ONNX Runtime 版本不匹配");

    OrtStatus* st = g_ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "lineocr", &g_env);
    if (st) { err = U2W(g_ort->GetErrorMessage(st)); g_ort->ReleaseStatus(st); return false; }

    (void)g_ort->DisableTelemetryEvents(g_env);

    st = g_ort->CreateSessionOptions(&g_opts);
    if (st) { err = U2W(g_ort->GetErrorMessage(st)); g_ort->ReleaseStatus(st); return false; }
    (void)g_ort->SetIntraOpNumThreads(g_opts, 2);
    (void)g_ort->SetSessionGraphOptimizationLevel(g_opts, ORT_ENABLE_ALL);

    std::vector<char> model;
    if (!ReadAllBytes(mdlPath, model)) return Fail(err, L"读取模型文件失败");

    st = g_ort->CreateSessionFromArray(g_env, model.data(), model.size(), g_opts, &g_sess);
    if (st) { err = U2W(g_ort->GetErrorMessage(st)); g_ort->ReleaseStatus(st); g_sess = nullptr; return false; }

    if (!LoadDict(dicPath)) return Fail(err, L"字典文件为空或无法解析");

    OrtAllocator* alloc = nullptr;
    (void)g_ort->GetAllocatorWithDefaultOptions(&alloc);

    size_t nIn = 0, nOut = 0;
    (void)g_ort->SessionGetInputCount(g_sess, &nIn);
    (void)g_ort->SessionGetOutputCount(g_sess, &nOut);
    if (nIn < 1 || nOut < 1) return Fail(err, L"模型输入输出异常");

    char* nm = nullptr;
    (void)g_ort->SessionGetInputName(g_sess, 0, alloc, &nm);
    if (nm) { g_inName = nm; alloc->Free(alloc, nm); }
    nm = nullptr;
    (void)g_ort->SessionGetOutputName(g_sess, 0, alloc, &nm);
    if (nm) { g_outName = nm; alloc->Free(alloc, nm); }

    g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &g_mem);

    g_info = L"模型识别就绪 · 字典 " + std::to_wstring(g_dict.size()) + L" 字";
    g_ready = true;
    return true;
}

void OcrShutdown() {
    std::lock_guard<std::mutex> lock(g_initMutex);
    if (g_ort) {
        if (g_mem) { g_ort->ReleaseMemoryInfo(g_mem); g_mem = nullptr; }
        if (g_sess) { g_ort->ReleaseSession(g_sess); g_sess = nullptr; }
        if (g_opts) { g_ort->ReleaseSessionOptions(g_opts); g_opts = nullptr; }
        if (g_env) { g_ort->ReleaseEnv(g_env); g_env = nullptr; }
    }
    if (g_dll) { FreeLibrary(g_dll); g_dll = nullptr; }
    g_ready = false;
    g_ort = nullptr;
    g_dict.clear();
    g_info.clear();
}

// ---------------- 灰度 + 右侧行定位 ----------------

static void ToGray(const unsigned char* bgra, int w, int h, std::vector<unsigned char>& gray) {
    gray.resize((size_t)w * h);
    for (int y = 0; y < h; ++y) {
        const unsigned char* p = bgra + (size_t)y * w * 4;
        unsigned char* q = &gray[(size_t)y * w];
        for (int x = 0; x < w; ++x) {
            int b = p[x * 4 + 0];
            int g = p[x * 4 + 1];
            int r = p[x * 4 + 2];
            q[x] = (unsigned char)((r * 299 + g * 587 + b * 114) / 1000);
        }
    }
}

struct LineSpan { int top = 0; int bottom = 0; };

// 在灰度图的一个矩形区域里用 Otsu 找阈值，并判断是深底浅字还是浅底深字。
// 只在鼠标右侧取样：左侧有没有字与我们无关。
static bool OtsuInRect(const std::vector<unsigned char>& gray, int w,
                       int x0, int x1, int y0, int y1,
                       int& thr, bool& darkText) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > (int)(gray.size() / (size_t)w)) y1 = (int)(gray.size() / (size_t)w);
    if (x1 - x0 < 8 || y1 - y0 < 4) return false;

    int hist[256] = {0};
    long long total = 0;
    int lo = 255, hi = 0;
    for (int y = y0; y < y1; ++y) {
        const unsigned char* row = &gray[(size_t)y * w];
        for (int x = x0; x < x1; ++x) {
            int v = row[x];
            ++hist[v];
            ++total;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    if (total < 64 || hi - lo < 45) return false;

    double sum = 0.0;
    for (int i = 0; i < 256; ++i) sum += (double)i * hist[i];

    double sumB = 0.0;
    long long wB = 0;
    double bestVar = -1.0;
    int bestT = lo;
    for (int t = lo; t <= hi; ++t) {
        wB += hist[t];
        if (wB == 0) continue;
        long long wF = total - wB;
        if (wF == 0) break;
        sumB += (double)t * hist[t];
        double mB = sumB / (double)wB;
        double mF = (sum - sumB) / (double)wF;
        double var = (double)wB * (double)wF * (mB - mF) * (mB - mF);
        if (var > bestVar) { bestVar = var; bestT = t; }
    }

    long long below = 0;
    for (int i = 0; i <= bestT; ++i) below += hist[i];
    long long above = total - below;
    if (below < total / 100 || above < total / 100) return false;

    thr = bestT;
    darkText = below <= above;   // 文字是少数派
    return true;
}

// 把 (top,bottom) 高度超过 maxH 的块在前景最少的谷底行切开；切不动就整块保留。
static void SplitTallSpan(const std::vector<int>& rowFg, int top, int bottom, int maxH,
                          std::vector<LineSpan>& out) {
    int hgt = bottom - top + 1;
    if (hgt <= maxH || bottom >= (int)rowFg.size()) {
        if (hgt >= 5) { LineSpan s; s.top = top; s.bottom = bottom; out.push_back(s); }
        return;
    }
    int best = -1, bestV = 1 << 30;
    int lo = top + 6, hi = bottom - 6;
    for (int y = lo; y <= hi; ++y) {
        if (rowFg[y] < bestV) { bestV = rowFg[y]; best = y; }
    }
    if (best < 0 || best - top < 6 || bottom - best < 6) {
        LineSpan s; s.top = top; s.bottom = bottom; out.push_back(s);
        return;
    }
    SplitTallSpan(rowFg, top, best - 1, maxH, out);
    SplitTallSpan(rowFg, best + 1, bottom, maxH, out);
}

// 右侧行定位结果
struct RightLine {
    bool ok = false;
    bool darkText = true;
    int top = 0;
    int bottom = 0;
    int firstX = 0;     // 锚点右侧第一段墨迹的左边界（条带坐标）
    int gap = 0;        // 这段墨迹离鼠标的距离（<=0 表示鼠标压在字上）
    int lineH = 0;
};

// 只找「包含鼠标 Y、且在鼠标右侧有字」的那一行。
// 找不到 / 右侧没字 / 右侧字太远，都返回 false；不换行、不看左侧。
static RightLine FindRightLine(const std::vector<unsigned char>& gray, int w, int h,
                               int mouseX, int mouseY, int* reject) {
    RightLine out;
    if (w <= 0 || h <= 0 || mouseX >= w) { if (reject) *reject = 0; return out; }

    // 取样区域：鼠标右侧、鼠标上下各 80px；找不到高对比再退回整条带。
    int thr = 128;
    bool darkText = true;
    bool havePolarity = OtsuInRect(gray, w, mouseX, std::min(w, mouseX + 480),
                                   mouseY - 80, mouseY + 80, thr, darkText);
    if (!havePolarity) {
        havePolarity = OtsuInRect(gray, w, 0, w, 0, h, thr, darkText);
    }
    if (!havePolarity) { if (reject) *reject = 0; return out; }
    out.darkText = darkText;

    // 行投影只统计鼠标右侧，左侧文字不参与“有没有行”的判断。
    std::vector<int> rowFg((size_t)h, 0);
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = &gray[(size_t)y * w];
        int c = 0;
        for (int x = mouseX; x < w; ++x) {
            bool fg = darkText ? (row[x] < thr) : (row[x] > thr);
            if (fg) ++c;
        }
        rowFg[y] = c;
    }

    int need = (w - mouseX) / 100;
    if (need < 2) need = 2;

    std::vector<LineSpan> spans;
    int start = -1, gapRows = 0;
    for (int y = 0; y < h; ++y) {
        bool isText = rowFg[y] >= need;
        if (isText) {
            if (start < 0) start = y;
            gapRows = 0;
        } else if (start >= 0) {
            ++gapRows;
            if (gapRows > 2) {
                LineSpan s; s.top = start; s.bottom = y - gapRows;
                if (s.bottom - s.top >= 5) spans.push_back(s);
                start = -1;
                gapRows = 0;
            }
        }
    }
    if (start >= 0) {
        LineSpan s; s.top = start; s.bottom = h - 1;
        if (s.bottom - s.top >= 5) spans.push_back(s);
    }

    // 行距很密时投影会把相邻几行粘成一块，在谷底递归切开。
    std::vector<LineSpan> split;
    split.reserve(spans.size() + 4);
    for (const LineSpan& s : spans) SplitTallSpan(rowFg, s.top, s.bottom, 56, split);

    int pick = -1;
    int pickH = 1 << 30;
    for (int i = 0; i < (int)split.size(); ++i) {
        const LineSpan& s = split[i];
        int hgt = s.bottom - s.top + 1;
        int tol = hgt / 4;
        if (tol < 2) tol = 2;
        if (tol > 8) tol = 8;
        if (mouseY >= s.top - tol && mouseY <= s.bottom + tol) {
            if (hgt < pickH) { pickH = hgt; pick = i; }
        }
    }
    if (pick < 0) { if (reject) *reject = 0; return out; }

    const LineSpan& line = split[pick];
    int lineH = line.bottom - line.top + 1;
    out.top = line.top;
    out.bottom = line.bottom;
    out.lineH = lineH;

    // 列投影：在选中行里找鼠标右侧第一段连续墨迹。
    std::vector<int> colFg((size_t)w, 0);
    for (int x = 0; x < w; ++x) {
        int c = 0;
        const unsigned char* row = nullptr;
        (void)row;
        for (int y = line.top; y <= line.bottom; ++y) {
            const unsigned char* r = &gray[(size_t)y * w];
            bool fg = darkText ? (r[x] < thr) : (r[x] > thr);
            if (fg) ++c;
        }
        colFg[x] = c;
    }

    int x = mouseX;
    int anchorL = -1, anchorR = -1;
    while (x < w) {
        if (colFg[x] <= 0) { ++x; continue; }
        int l = x;
        int last = x;
        int r = x;
        while (r + 1 < w) {
            if (colFg[r + 1] > 0) { ++r; last = r; continue; }
            // 字内允许 1~2 列抗锯齿空隙
            if (r + 2 < w && colFg[r + 2] > 0) { r += 2; last = r; continue; }
            break;
        }
        if (last - l + 1 >= 3) {
            anchorL = l;
            anchorR = last + 1;
            break;
        }
        x = last + 1;
    }
    (void)anchorR;
    if (anchorL < 0) { if (reject) *reject = 1; return out; }

    out.firstX = anchorL;
    out.gap = anchorL - mouseX;
    if (out.gap < 0) out.gap = 0;
    if (out.gap > MaxRightGap(lineH)) { if (reject) *reject = 1; return out; }

    out.ok = true;
    return out;
}

// ---------------- 缩放 + 归一化 ----------------

static void ResizeGrayToFloat(const std::vector<unsigned char>& gray, int sw, int sh,
                              int sx, int sy, int srcW, int srcH,
                              int dstW, int dstH, std::vector<float>& out) {
    // 双线性缩放，输出 CHW（3 通道相同）
    size_t plane = (size_t)dstW * dstH;
    out.assign(plane * 3, 0.0f);

    for (int dy = 0; dy < dstH; ++dy) {
        float fy = (dstH > 1) ? (float)dy * (srcH - 1) / (dstH - 1) : 0.0f;
        int y0 = (int)fy;
        int y1 = (y0 + 1 < srcH) ? y0 + 1 : y0;
        float wy = fy - y0;

        for (int dx = 0; dx < dstW; ++dx) {
            float fx = (dstW > 1) ? (float)dx * (srcW - 1) / (dstW - 1) : 0.0f;
            int x0 = (int)fx;
            int x1 = (x0 + 1 < srcW) ? x0 + 1 : x0;
            float wx = fx - x0;

            int gx0 = sx + x0, gx1 = sx + x1;
            int gy0 = sy + y0, gy1 = sy + y1;
            if (gx0 >= sw) gx0 = sw - 1;
            if (gx1 >= sw) gx1 = sw - 1;
            if (gy0 >= sh) gy0 = sh - 1;
            if (gy1 >= sh) gy1 = sh - 1;
            if (gx0 < 0) gx0 = 0;
            if (gx1 < 0) gx1 = 0;
            if (gy0 < 0) gy0 = 0;
            if (gy1 < 0) gy1 = 0;

            float v00 = gray[(size_t)gy0 * sw + gx0];
            float v01 = gray[(size_t)gy0 * sw + gx1];
            float v10 = gray[(size_t)gy1 * sw + gx0];
            float v11 = gray[(size_t)gy1 * sw + gx1];

            float v = (v00 * (1 - wx) + v01 * wx) * (1 - wy) +
                      (v10 * (1 - wx) + v11 * wx) * wy;

            float nv = v / 127.5f - 1.0f;  // (v/255 - 0.5)/0.5
            size_t idx = (size_t)dy * dstW + dx;
            out[idx] = nv;
            out[idx + plane] = nv;
            out[idx + plane * 2] = nv;
        }
    }
}

// ---------------- CTC 解码 ----------------

// 第 t 步对应整幅缩放图宽度上的 x ≈ (t+0.5)*srcW/T，再用于按鼠标位置取段。
static std::wstring CtcDecode(const float* data, int T, int C, int srcW,
                              std::vector<double>& charX) {
    std::wstring res;
    charX.clear();
    int prev = -1;
    for (int t = 0; t < T; ++t) {
        const float* p = data + (size_t)t * C;
        int best = 0;
        float bv = p[0];
        for (int c = 1; c < C; ++c) {
            if (p[c] > bv) { bv = p[c]; best = c; }
        }
        if (best != 0 && best != prev) {
            int idx = best - 1;
            const std::wstring* ch = nullptr;
            static const std::wstring kSpace = L" ";
            if (idx >= 0 && idx < (int)g_dict.size()) ch = &g_dict[(size_t)idx];
            else if (idx == (int)g_dict.size()) ch = &kSpace;
            if (ch && !ch->empty()) {
                res += *ch;
                charX.push_back((double)(t + 0.5) * (double)srcW / (double)T);
            }
        }
        prev = best;
    }
    return res;
}

// ---------------- 对外识别 ----------------

// 对灰度图的 [top..bottom] 行、[x0..x1) 列跑一次模型，输出文字与每字 x（条带坐标）。
static bool RecognizeSpan(const std::vector<unsigned char>& gray, int w, int h,
                          int top, int bottom, int x0, int x1,
                          std::wstring& text, std::vector<double>* charX) {
    text.clear();
    if (charX) charX->clear();

    if (x0 < 0) x0 = 0;
    if (x1 > w) x1 = w;
    if (x1 - x0 < 8) return false;

    int pad = 3;
    int y0 = top - pad; if (y0 < 0) y0 = 0;
    int y1 = bottom + pad; if (y1 > h - 1) y1 = h - 1;
    int srcH = y1 - y0 + 1;
    int srcW = x1 - x0;
    if (srcH < 4) return false;

    // 目标尺寸：高固定 48，宽按比例
    int dstH = 48;
    int dstW = (int)std::lround(48.0 * srcW / (double)srcH);
    if (dstW < 16) dstW = 16;
    if (dstW > 1600) dstW = 1600;

    std::vector<float> input;
    ResizeGrayToFloat(gray, w, h, x0, y0, srcW, srcH, dstW, dstH, input);

    int64_t shape[4] = { 1, 3, dstH, dstW };
    OrtValue* inVal = nullptr;
    OrtStatus* st = g_ort->CreateTensorWithDataAsOrtValue(
        g_mem, input.data(), input.size() * sizeof(float), shape, 4,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVal);
    if (st) { g_ort->ReleaseStatus(st); return false; }

    const char* inNames[1]  = { g_inName.c_str() };
    const char* outNames[1] = { g_outName.c_str() };
    OrtValue* outVal = nullptr;
    st = g_ort->Run(g_sess, nullptr, inNames, &inVal, 1, outNames, 1, &outVal);
    g_ort->ReleaseValue(inVal);
    if (st) { g_ort->ReleaseStatus(st); return false; }

    std::wstring result;
    std::vector<double> xs;
    OrtTensorTypeAndShapeInfo* tsi = nullptr;
    OrtStatus* stShape = g_ort->GetTensorTypeAndShape(outVal, &tsi);
    if (stShape) {
        g_ort->ReleaseStatus(stShape);
    } else if (tsi) {
        size_t nd = 0;
        g_ort->GetDimensionsCount(tsi, &nd);
        int64_t dims[8] = {0};
        if (nd >= 3 && nd <= 8) {
            g_ort->GetDimensions(tsi, dims, nd);
            int T = (int)dims[1];
            int C = (int)dims[2];
            void* raw = nullptr;
            OrtStatus* stData = g_ort->GetTensorMutableData(outVal, &raw);
            if (stData) {
                g_ort->ReleaseStatus(stData);
            } else if (raw && T > 0 && C > 0) {
                result = CtcDecode((const float*)raw, T, C, srcW, xs);
                for (double& v : xs) v += (double)x0;
            }
        }
        g_ort->ReleaseTensorTypeAndShapeInfo(tsi);
    }
    g_ort->ReleaseValue(outVal);

    text = result;
    if (charX && xs.size() == result.size()) charX->swap(xs);
    return !text.empty();
}

bool OcrRecognizeBgra(const unsigned char* bgra, int w, int h,
                      int mouseX, int mouseY,
                      std::wstring& text, int* lineTop, int* lineBottom,
                      std::vector<double>* charX, int* reject) {
    text.clear();
    if (charX) charX->clear();
    if (lineTop) *lineTop = 0;
    if (lineBottom) *lineBottom = 0;
    if (reject) *reject = 0;
    if (!g_sess || !bgra || w <= 0 || h <= 0) return false;

    std::vector<unsigned char> gray;
    ToGray(bgra, w, h, gray);

    RightLine line = FindRightLine(gray, w, h, mouseX, mouseY, reject);
    if (!line.ok) return false;

    std::wstring t;
    std::vector<double> xs;
    if (!RecognizeSpan(gray, w, h, line.top, line.bottom, line.firstX, w, t, &xs)) return false;
    if (CountTextChars(t) < 1) return false;

    text = t;
    if (charX) {
        if (xs.size() == t.size()) charX->swap(xs);
        else charX->clear();
    }
    if (lineTop) *lineTop = line.top;
    if (lineBottom) *lineBottom = line.bottom;
    return true;
}
