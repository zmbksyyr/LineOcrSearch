// ocr_onnx.cpp - C2 模型识别：投影分行 + 归一化 + ONNX Runtime C API + CTC 解码
#include "common.h"
#include "../vendor/onnxruntime_c_api.h"
#include <cmath>
#include <cstring>

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
        if (!line.empty()) {
            g_dict.push_back(U2W(line));
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return !g_dict.empty();
}

// ---------------- 初始化 ----------------

bool OcrReady() { return g_sess != nullptr; }

std::wstring OcrEngineInfo() { return g_info; }

static bool Fail(std::wstring& err, const wchar_t* msg) {
    err = msg;
    return false;
}

bool OcrInit(std::wstring& err) {
    if (g_sess) return true;

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

    g_ort->DisableTelemetryEvents(g_env);

    st = g_ort->CreateSessionOptions(&g_opts);
    if (st) { err = U2W(g_ort->GetErrorMessage(st)); g_ort->ReleaseStatus(st); return false; }
    g_ort->SetIntraOpNumThreads(g_opts, 2);
    g_ort->SetSessionGraphOptimizationLevel(g_opts, ORT_ENABLE_ALL);

    std::vector<char> model;
    if (!ReadAllBytes(mdlPath, model)) return Fail(err, L"读取模型文件失败");

    st = g_ort->CreateSessionFromArray(g_env, model.data(), model.size(), g_opts, &g_sess);
    if (st) { err = U2W(g_ort->GetErrorMessage(st)); g_ort->ReleaseStatus(st); g_sess = nullptr; return false; }

    if (!LoadDict(dicPath)) return Fail(err, L"字典文件为空或无法解析");

    OrtAllocator* alloc = nullptr;
    g_ort->GetAllocatorWithDefaultOptions(&alloc);

    size_t nIn = 0, nOut = 0;
    g_ort->SessionGetInputCount(g_sess, &nIn);
    g_ort->SessionGetOutputCount(g_sess, &nOut);
    if (nIn < 1 || nOut < 1) return Fail(err, L"模型输入输出异常");

    char* nm = nullptr;
    g_ort->SessionGetInputName(g_sess, 0, alloc, &nm);
    if (nm) { g_inName = nm; alloc->Free(alloc, nm); }
    nm = nullptr;
    g_ort->SessionGetOutputName(g_sess, 0, alloc, &nm);
    if (nm) { g_outName = nm; alloc->Free(alloc, nm); }

    g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &g_mem);

    g_info = L"模型识别就绪 · 字典 " + std::to_wstring(g_dict.size()) + L" 字";
    return true;
}

void OcrShutdown() {
    if (g_ort) {
        if (g_mem) { g_ort->ReleaseMemoryInfo(g_mem); g_mem = nullptr; }
        if (g_sess) { g_ort->ReleaseSession(g_sess); g_sess = nullptr; }
        if (g_opts) { g_ort->ReleaseSessionOptions(g_opts); g_opts = nullptr; }
        if (g_env) { g_ort->ReleaseEnv(g_env); g_env = nullptr; }
    }
    if (g_dll) { FreeLibrary(g_dll); g_dll = nullptr; }
    g_ort = nullptr;
    g_dict.clear();
    g_info.clear();
}

// ---------------- 灰度 + 投影分行 ----------------

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

// 自动判断前景极性（深底浅字 / 浅底深字），再按行统计前景像素数找文字行
static void FindTextLines(const std::vector<unsigned char>& gray, int w, int h,
                          std::vector<LineSpan>& spans, bool& darkText) {
    spans.clear();
    if (w <= 0 || h <= 0) return;

    long darkCount = 0, lightCount = 0;
    for (size_t i = 0; i < gray.size(); ++i) {
        if (gray[i] < 100) ++darkCount;
        else if (gray[i] > 160) ++lightCount;
    }
    darkText = (darkCount <= lightCount);

    std::vector<int> rowFg(h, 0);
    for (int y = 0; y < h; ++y) {
        const unsigned char* p = &gray[(size_t)y * w];
        int c = 0;
        for (int x = 0; x < w; ++x) {
            bool fg = darkText ? (p[x] < 128) : (p[x] > 128);
            if (fg) ++c;
        }
        rowFg[y] = c;
    }

    int need = w > 40 ? w / 100 : 1;  // 至少 1% 宽度有前景才算文字行
    if (need < 2) need = 2;

    int start = -1;
    int gap = 0;
    for (int y = 0; y < h; ++y) {
        bool isText = rowFg[y] >= need;
        if (isText) {
            if (start < 0) start = y;
            gap = 0;
        } else if (start >= 0) {
            ++gap;
            if (gap > 2) {  // 允许 2px 空隙，避免把一行切成两段
                LineSpan s; s.top = start; s.bottom = y - gap;
                if (s.bottom - s.top >= 5) spans.push_back(s);
                start = -1;
                gap = 0;
            }
        }
    }
    if (start >= 0) {
        LineSpan s; s.top = start; s.bottom = h - 1;
        if (s.bottom - s.top >= 5) spans.push_back(s);
    }
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
            if (gx1 >= sw) gx1 = sw - 1;
            if (gy1 >= sh) gy1 = sh - 1;

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

// CTC 解码，同时记录每个输出字符的水平位置（条带内像素坐标）。
// 模型输出 T 个时间步对应整幅缩放图的宽度，所以第 t 步落在 x ≈ (t+0.5)*srcW/T 处，
// 用它就能把「鼠标在条带里的 x」映射到「识别结果的第几个字」，从而按鼠标位置选段。
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

bool OcrRecognizeBgra(const unsigned char* bgra, int w, int h, int mouseY,
                      std::wstring& text, int* lineTop, int* lineBottom,
                      std::vector<double>* charX) {
    text.clear();
    if (charX) charX->clear();
    if (lineTop) *lineTop = 0;
    if (lineBottom) *lineBottom = 0;
    if (!g_sess || !bgra || w <= 0 || h <= 0) return false;

    std::vector<unsigned char> gray;
    ToGray(bgra, w, h, gray);

    std::vector<LineSpan> spans;
    bool darkText = true;
    FindTextLines(gray, w, h, spans, darkText);
    if (spans.empty()) return false;

    // 选包含鼠标 Y 的行；落在空白处取最近的一行
    int pick = -1;
    int bestDist = 1 << 30;
    for (size_t i = 0; i < spans.size(); ++i) {
        if (mouseY >= spans[i].top && mouseY <= spans[i].bottom) { pick = (int)i; break; }
        int d = (mouseY < spans[i].top) ? (spans[i].top - mouseY) : (mouseY - spans[i].bottom);
        if (d < bestDist) { bestDist = d; pick = (int)i; }
    }
    if (pick < 0) return false;

    int top = spans[pick].top;
    int bottom = spans[pick].bottom;

    // 行距很密时投影会把相邻几行粘成一块，这种情况按鼠标 Y 再切出一行，
    // 避免把上下好几行一起送进模型导致识别结果串行。
    int spanH = bottom - top + 1;
    if (spanH > 44) {
        int half = 22;
        int t2 = mouseY - half;
        int b2 = mouseY + half;
        if (t2 < top) t2 = top;
        if (b2 > bottom) b2 = bottom;
        if (b2 - t2 + 1 >= 12) {
            top = t2;
            bottom = b2;
        }
    }

    if (lineTop) *lineTop = top;
    if (lineBottom) *lineBottom = bottom;

    int pad = 3;
    int y0 = top - pad; if (y0 < 0) y0 = 0;
    int y1 = bottom + pad; if (y1 > h - 1) y1 = h - 1;
    int srcH = y1 - y0 + 1;
    int srcW = w;
    if (srcH < 4) return false;

    // 目标尺寸：高固定 48，宽按比例
    int dstH = 48;
    int dstW = (int)std::lround(48.0 * srcW / (double)srcH);
    if (dstW < 16) dstW = 16;
    if (dstW > 1600) dstW = 1600;

    std::vector<float> input;
    ResizeGrayToFloat(gray, w, h, 0, y0, srcW, srcH, dstW, dstH, input);

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
            }
        }
        g_ort->ReleaseTensorTypeAndShapeInfo(tsi);
    }
    g_ort->ReleaseValue(outVal);

    text = result;
    if (charX && xs.size() == result.size()) charX->swap(xs);
    return !text.empty();
}
