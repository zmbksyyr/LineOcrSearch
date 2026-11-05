// office.cpp - docx / xlsx 文本抽取：自实现 inflate + ZIP 读取 + 轻量 XML 扫描
#include "common.h"
#include <cstring>
#include <algorithm>

// ======================= deflate 解压 =======================

namespace {

struct BitReader {
    const unsigned char* p = nullptr;
    size_t len = 0;
    size_t pos = 0;
    unsigned buf = 0;
    int cnt = 0;

    int bit() {
        if (cnt == 0) {
            if (pos >= len) return -1;
            buf = p[pos++];
            cnt = 8;
        }
        int b = (int)(buf & 1u);
        buf >>= 1;
        --cnt;
        return b;
    }
    int bits(int n) {
        int v = 0;
        for (int i = 0; i < n; ++i) {
            int b = bit();
            if (b < 0) return -1;
            v |= b << i;
        }
        return v;
    }
    void alignByte() { cnt = 0; }
};

struct Huffman {
    short count[16];
    short symbol[320];
};

static int BuildHuff(Huffman* h, const short* length, int n) {
    for (int i = 0; i < 16; ++i) h->count[i] = 0;
    for (int i = 0; i < n; ++i) h->count[length[i]]++;
    if (h->count[0] == n) return 0;

    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int i = 0; i < n; ++i) {
        if (length[i]) h->symbol[offs[length[i]]++] = (short)i;
    }
    return left;
}

static int DecodeSym(BitReader* br, const Huffman* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        int b = br->bit();
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static const short kLenBase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short kLenExtra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short kDistBase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short kDistExtra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static bool InflateBlock(BitReader* br, const Huffman* lit, const Huffman* dist,
                         std::vector<unsigned char>& out, size_t limit) {
    for (;;) {
        int sym = DecodeSym(br, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out.size() >= limit) return false;
            out.push_back((unsigned char)sym);
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int extra = br->bits(kLenExtra[sym]);
            if (extra < 0) return false;
            int len = kLenBase[sym] + extra;

            int dsym = DecodeSym(br, dist);
            if (dsym < 0 || dsym >= 30) return false;
            int dextra = br->bits(kDistExtra[dsym]);
            if (dextra < 0) return false;
            int d = kDistBase[dsym] + dextra;
            if ((size_t)d > out.size()) return false;

            for (int i = 0; i < len; ++i) {
                if (out.size() >= limit) return false;
                out.push_back(out[out.size() - (size_t)d]);
            }
        }
    }
}

static bool InflateRaw(const unsigned char* src, size_t srclen,
                       std::vector<unsigned char>& out, size_t limit) {
    BitReader br;
    br.p = src;
    br.len = srclen;

    int last = 0;
    while (!last) {
        last = br.bits(1);
        if (last < 0) return false;
        int type = br.bits(2);
        if (type < 0) return false;

        if (type == 0) {
            br.alignByte();
            if (br.pos + 4 > srclen) return false;
            unsigned len = (unsigned)src[br.pos] | ((unsigned)src[br.pos + 1] << 8);
            br.pos += 4;
            if (br.pos + len > srclen) return false;
            if (out.size() + len > limit) return false;
            out.insert(out.end(), src + br.pos, src + br.pos + len);
            br.pos += len;
        } else if (type == 1) {
            Huffman lit{}, dst{};
            short lens[288];
            for (int i = 0; i < 144; ++i) lens[i] = 8;
            for (int i = 144; i < 256; ++i) lens[i] = 9;
            for (int i = 256; i < 280; ++i) lens[i] = 7;
            for (int i = 280; i < 288; ++i) lens[i] = 8;
            if (BuildHuff(&lit, lens, 288) < 0) return false;
            short dl[30];
            for (int i = 0; i < 30; ++i) dl[i] = 5;
            if (BuildHuff(&dst, dl, 30) < 0) return false;
            if (!InflateBlock(&br, &lit, &dst, out, limit)) return false;
        } else if (type == 2) {
            int hlit = br.bits(5);
            int hdist = br.bits(5);
            int hclen = br.bits(4);
            if (hlit < 0 || hdist < 0 || hclen < 0) return false;
            hlit += 257;
            hdist += 1;
            hclen += 4;
            if (hlit > 286 || hdist > 30) return false;

            static const short order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            short clen[19] = {0};
            for (int i = 0; i < hclen; ++i) {
                int v = br.bits(3);
                if (v < 0) return false;
                clen[order[i]] = (short)v;
            }
            Huffman clh{};
            if (BuildHuff(&clh, clen, 19) < 0) return false;

            short lengths[320] = {0};
            int total = hlit + hdist;
            int i = 0;
            while (i < total) {
                int sym = DecodeSym(&br, &clh);
                if (sym < 0) return false;
                if (sym < 16) {
                    lengths[i++] = (short)sym;
                } else if (sym == 16) {
                    if (i == 0) return false;
                    int rep = br.bits(2);
                    if (rep < 0) return false;
                    rep += 3;
                    short prev = lengths[i - 1];
                    while (rep-- > 0 && i < total) lengths[i++] = prev;
                } else if (sym == 17) {
                    int rep = br.bits(3);
                    if (rep < 0) return false;
                    rep += 3;
                    while (rep-- > 0 && i < total) lengths[i++] = 0;
                } else if (sym == 18) {
                    int rep = br.bits(7);
                    if (rep < 0) return false;
                    rep += 11;
                    while (rep-- > 0 && i < total) lengths[i++] = 0;
                } else {
                    return false;
                }
            }

            Huffman lit{}, dst{};
            if (BuildHuff(&lit, lengths, hlit) < 0) return false;
            if (BuildHuff(&dst, lengths + hlit, hdist) < 0) return false;
            if (!InflateBlock(&br, &lit, &dst, out, limit)) return false;
        } else {
            return false;
        }
    }
    return true;
}

// ======================= ZIP 读取 =======================

static unsigned ReadU32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static unsigned ReadU16(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

struct ZipEntry {
    std::string name;
    unsigned method = 0;
    unsigned compSize = 0;
    unsigned uncompSize = 0;
    unsigned localOffset = 0;
};

struct ZipArchive {
    std::vector<unsigned char> data;
    std::vector<ZipEntry> entries;

    bool Open(const std::wstring& path) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)128 * 1024 * 1024) {
            CloseHandle(h);
            return false;
        }
        data.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        BOOL ok = ReadFile(h, data.data(), (DWORD)data.size(), &got, nullptr);
        CloseHandle(h);
        if (!ok) return false;
        data.resize(got);
        return ParseCentral();
    }

    bool ParseCentral() {
        if (data.size() < 22) return false;
        // 从尾部向前找 EOCD
        size_t maxBack = data.size() > 66000 ? 66000 : data.size();
        size_t eocd = (size_t)-1;
        // 注意：不能写成 `for (i = size-22; i + 4 <= size; --i)`，
        // i 减到 0 之后会回绕成 SIZE_MAX，i + 4 又绕回 3，条件仍然成立，
        // 于是拿 SIZE_MAX 当下标去读，遇到不是 zip 的 .docx/.xlsx 就直接崩。
        const size_t first = data.size() - 22;
        for (size_t i = first + 1; i-- > 0; ) {
            if (ReadU32(&data[i]) == 0x06054b50u) { eocd = i; break; }
            if (first - i > maxBack) break;
        }
        if (eocd == (size_t)-1) return false;

        unsigned count = ReadU16(&data[eocd + 10]);
        unsigned cdOffset = ReadU32(&data[eocd + 16]);
        if (cdOffset >= data.size()) return false;

        size_t p = cdOffset;
        for (unsigned i = 0; i < count && p + 46 <= data.size(); ++i) {
            if (ReadU32(&data[p]) != 0x02014b50u) break;
            ZipEntry e;
            e.method = ReadU16(&data[p + 10]);
            e.compSize = ReadU32(&data[p + 20]);
            e.uncompSize = ReadU32(&data[p + 24]);
            unsigned nameLen = ReadU16(&data[p + 28]);
            unsigned extraLen = ReadU16(&data[p + 30]);
            unsigned commentLen = ReadU16(&data[p + 32]);
            e.localOffset = ReadU32(&data[p + 42]);
            if (p + 46 + nameLen > data.size()) break;
            e.name.assign((const char*)&data[p + 46], nameLen);
            entries.push_back(e);
            p += 46 + nameLen + extraLen + commentLen;
        }
        return !entries.empty();
    }

    const ZipEntry* Find(const std::string& name) const {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].name == name) return &entries[i];
        }
        return nullptr;
    }

    bool Read(const ZipEntry* e, std::vector<unsigned char>& out) const {
        if (!e || e->localOffset + 30 > data.size()) return false;
        const unsigned char* p = &data[e->localOffset];
        if (ReadU32(p) != 0x04034b50u) return false;
        unsigned nameLen = ReadU16(p + 26);
        unsigned extraLen = ReadU16(p + 28);
        size_t start = e->localOffset + 30 + nameLen + extraLen;
        if (start + e->compSize > data.size()) return false;

        if (e->method == 0) {
            out.assign(data.begin() + (long)start, data.begin() + (long)(start + e->compSize));
            return true;
        }
        if (e->method != 8) return false;

        size_t limit = e->uncompSize ? (size_t)e->uncompSize + 1024 : (size_t)64 * 1024 * 1024;
        out.clear();
        out.reserve(e->uncompSize ? e->uncompSize : 65536);
        return InflateRaw(&data[start], e->compSize, out, limit);
    }
};

// ======================= XML 文本抽取 =======================

// 去掉命名空间前缀：w:p -> p，x:c -> c
static std::string StripNs(const std::string& name) {
    size_t c = name.find(':');
    return (c == std::string::npos) ? name : name.substr(c + 1);
}

static void AppendUtf8AsWide(std::wstring& out, const std::string& s) {
    if (s.empty()) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return;
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    out += w;
}

static void AppendXmlTextRaw(std::wstring& out, const std::string& s) {
    // 处理常见实体
    std::string buf;
    buf.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0) { buf += '&'; i += 4; continue; }
            if (s.compare(i, 4, "&lt;") == 0)  { buf += '<'; i += 3; continue; }
            if (s.compare(i, 4, "&gt;") == 0)  { buf += '>'; i += 3; continue; }
            if (s.compare(i, 6, "&quot;") == 0){ buf += '"'; i += 5; continue; }
            if (s.compare(i, 6, "&apos;") == 0){ buf += '\''; i += 5; continue; }
            if (s.compare(i, 5, "&#10;") == 0) { buf += '\n'; i += 4; continue; }
        }
        buf += s[i];
    }
    AppendUtf8AsWide(out, buf);
}

// docx：按 <w:p> 分段，取 <w:t> 文本
static void ExtractWordXml(const std::string& xml, std::wstring& out) {
    size_t p = 0;
    bool inT = false;
    while (p < xml.size()) {
        size_t lt = xml.find('<', p);
        if (lt == std::string::npos) {
            if (inT) AppendXmlTextRaw(out, xml.substr(p));
            break;
        }
        if (inT) AppendXmlTextRaw(out, xml.substr(p, lt - p));

        size_t gt = xml.find('>', lt);
        if (gt == std::string::npos) break;
        std::string tag = xml.substr(lt + 1, gt - lt - 1);

        bool closing = !tag.empty() && tag[0] == '/';
        std::string name = closing ? tag.substr(1) : tag;
        size_t sp = name.find_first_of(" \t\r\n/");
        if (sp != std::string::npos) name = name.substr(0, sp);
        name = StripNs(name);

        if (name == "t") {
            inT = !closing;
        } else if (name == "p") {
            if (!out.empty() && out.back() != L'\n') out += L'\n';
        } else if (name == "tab") {
            out += L'\t';
        } else if (name == "br" || name == "cr") {
            out += L'\n';
        }
        p = gt + 1;
    }
}

// xlsx：sharedStrings 收集 <si> 文本；sheet 按 <row> 换行，按 <c> 取 <v>
static void ExtractSharedStrings(const std::string& xml, std::vector<std::wstring>& out) {
    size_t p = 0;
    std::wstring cur;
    bool inSi = false, inT = false;
    while (p < xml.size()) {
        size_t lt = xml.find('<', p);
        if (lt == std::string::npos) break;
        if (inT) AppendXmlTextRaw(cur, xml.substr(p, lt - p));

        size_t gt = xml.find('>', lt);
        if (gt == std::string::npos) break;
        std::string tag = xml.substr(lt + 1, gt - lt - 1);
        bool closing = !tag.empty() && tag[0] == '/';
        std::string name = closing ? tag.substr(1) : tag;
        size_t sp = name.find_first_of(" \t\r\n/");
        if (sp != std::string::npos) name = name.substr(0, sp);
        name = StripNs(name);

        if (name == "si") {
            if (!closing) { inSi = true; cur.clear(); }
            else if (inSi) { out.push_back(cur); inSi = false; }
        } else if (name == "t") {
            inT = !closing;
        }
        p = gt + 1;
    }
}

static void ExtractSheetXml(const std::string& xml, const std::vector<std::wstring>& shared,
                            std::wstring& out) {
    size_t p = 0;
    bool inV = false, inIsT = false;
    int cellTypeShared = 0;
    std::string cellText;
    std::wstring inlineText;
    std::wstring rowLine;
    int colCount = 0;

    auto flushCell = [&]() {
        std::wstring val;
        if (cellTypeShared == 1) {
            int idx = atoi(cellText.c_str());
            if (idx >= 0 && idx < (int)shared.size()) val = shared[(size_t)idx];
        } else if (!inlineText.empty()) {
            val = inlineText;
        } else if (!cellText.empty()) {
            AppendUtf8AsWide(val, cellText);  // 数字 / 布尔 / 公式结果
        }
        if (!val.empty()) {
            if (!rowLine.empty()) rowLine += L'\t';
            rowLine += val;
        }
        cellTypeShared = 0;
        cellText.clear();
        inlineText.clear();
    };

    while (p < xml.size()) {
        size_t lt = xml.find('<', p);
        if (lt == std::string::npos) break;
        size_t gt = xml.find('>', lt);
        if (gt == std::string::npos) break;

        std::string tag = xml.substr(lt + 1, gt - lt - 1);
        bool closing = !tag.empty() && tag[0] == '/';
        std::string body = closing ? tag.substr(1) : tag;
        size_t sp = body.find_first_of(" \t\r\n/");
        std::string name = (sp == std::string::npos) ? body : body.substr(0, sp);
        name = StripNs(name);

        if (name == "c") {
            if (!closing) {
                cellTypeShared = 0;
                size_t tpos = tag.find("t=\"s\"");
                if (tpos != std::string::npos) cellTypeShared = 1;
                cellText.clear();
                inlineText.clear();
            } else {
                flushCell();
                ++colCount;
            }
        } else if (name == "v") {
            inV = !closing;
            if (!closing) cellText.clear();
        } else if (name == "t" && closing) {
            inIsT = false;
        } else if (name == "row") {
            if (closing) {
                if (!rowLine.empty()) {
                    out += rowLine;
                    out += L"\n";
                }
                rowLine.clear();
                colCount = 0;
            }
        }

        // 捕获节点文字
        size_t nextLt = xml.find('<', gt + 1);
        std::string inner = (nextLt == std::string::npos)
                            ? xml.substr(gt + 1)
                            : xml.substr(gt + 1, nextLt - gt - 1);
        if (name == "v") {
            if (!closing) cellText += inner;
        } else if (name == "t") {
            if (!closing) {
                AppendXmlTextRaw(inlineText, inner);
                inIsT = true;
            }
        }
        if (nextLt == std::string::npos) break;
        p = nextLt;
    }
    if (!rowLine.empty()) { out += rowLine; out += L"\n"; }
}

}  // namespace

// ======================= 对外入口 =======================

// 去掉过多空行：正文里段落之间的空行保留，连续的只留两行
static void CleanupText(std::wstring& text) {
    std::wstring cleaned;
    cleaned.reserve(text.size());
    int blank = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t c = text[i];
        if (c == L'\n') {
            if (++blank > 2) continue;
        } else if (c != L'\r') {
            blank = 0;
        }
        cleaned += c;
    }
    text.swap(cleaned);
}

bool ExtractOfficeText(const std::wstring& path, std::wstring& outText) {
    outText.clear();

    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    for (size_t i = 0; i < ext.size(); ++i) ext[i] = (wchar_t)towlower(ext[i]);

    // Word 97-2003 / Excel 97-2003：不是 zip，走 OLE 复合文档解析
    if (ext == L".doc" || ext == L".xls") {
        if (!ExtractLegacyOfficeText(path, outText)) return false;
        CleanupText(outText);
        return !outText.empty();
    }

    ZipArchive zip;
    if (!zip.Open(path)) {
        // 后缀像新格式、内容其实是老格式（改过后缀 / 另存出错），再按老的试一次
        if (!ExtractLegacyOfficeText(path, outText)) return false;
        CleanupText(outText);
        return !outText.empty();
    }

    std::vector<unsigned char> buf;
    std::wstring text;

    if (ext == L".docx" || ext == L".docm") {
        const ZipEntry* e = zip.Find("word/document.xml");
        if (!e) return false;
        if (!zip.Read(e, buf)) return false;
        std::string xml((const char*)buf.data(), buf.size());
        ExtractWordXml(xml, text);
    } else if (ext == L".xlsx" || ext == L".xlsm") {
        // 共享字符串
        std::vector<std::wstring> shared;
        if (const ZipEntry* e = zip.Find("xl/sharedStrings.xml")) {
            if (zip.Read(e, buf)) {
                std::string xml((const char*)buf.data(), buf.size());
                ExtractSharedStrings(xml, shared);
            }
        }
        // 依次拼接工作表
        for (size_t i = 0; i < zip.entries.size(); ++i) {
            const std::string& nm = zip.entries[i].name;
            if (nm.compare(0, 14, "xl/worksheets/") != 0) continue;
            if (nm.size() < 4 || nm.compare(nm.size() - 4, 4, ".xml") != 0) continue;
            if (!zip.Read(&zip.entries[i], buf)) continue;
            std::string xml((const char*)buf.data(), buf.size());
            ExtractSheetXml(xml, shared, text);
            text += L"\n";
        }
    } else {
        return false;
    }

    CleanupText(text);
    outText = text;
    return !outText.empty();
}
