// legacy_office.cpp - Word 97-2003 (.doc) 与 Excel 97-2003 (.xls) 的正文抽取
//
// 这两种格式都是 OLE 复合文档（Compound File Binary，CFB）：文件内部像一个小文件系统，
// 有 FAT、目录项和命名流。所以整体分两步：
//   1. Cfb：按 CFB 规范把任意命名的流读出来（与具体格式无关）；
//   2. WordDocument / Workbook 各自的结构解析，产出纯文本。
//
// 不做的事：保留格式、还原图片、解密。加密文档、Word 6/95 及更早、Excel 5.0/95 直接返回失败，
// 让上层退回“只按文件名匹配”，绝不猜内容。
#include "common.h"
#include <map>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace {

const unsigned kEndOfChain = 0xFFFFFFFEu;  // 链尾
const unsigned kFreeSect   = 0xFFFFFFFFu;  // 未使用

unsigned U16(const unsigned char* p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
unsigned U32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
unsigned long long U64(const unsigned char* p) {
    return (unsigned long long)U32(p) | ((unsigned long long)U32(p + 4) << 32);
}

bool ReadWholeFile(const std::wstring& path, unsigned long long maxBytes,
                   std::vector<unsigned char>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 ||
        (unsigned long long)sz.QuadPart > maxBytes) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(got);
    return true;
}

// ======================= CFB（OLE 复合文档）=======================

struct Cfb {
    struct Entry {
        std::wstring name;
        unsigned type = 0;                 // 1 存储 / 2 流 / 5 根
        unsigned start = kEndOfChain;
        unsigned long long size = 0;
    };

    std::vector<unsigned char> data;
    std::vector<unsigned char> miniStream;
    std::vector<unsigned> fat, miniFat;
    std::vector<Entry> entries;

    unsigned sectorSize = 512;
    unsigned miniSectorSize = 64;
    unsigned miniCutoff = 4096;

    bool Open(const std::wstring& path) {
        if (!ReadWholeFile(path, 64ULL * 1024 * 1024, data)) return false;
        if (data.size() < 512) return false;
        static const unsigned char kSig[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
        if (memcmp(data.data(), kSig, 8) != 0) return false;

        unsigned shift = U16(&data[0x1E]);
        unsigned mshift = U16(&data[0x20]);
        if (shift < 7 || shift > 14 || mshift < 4 || mshift > 12) return false;
        sectorSize = 1u << shift;
        miniSectorSize = 1u << mshift;
        miniCutoff = U32(&data[0x38]);
        if (miniCutoff == 0) miniCutoff = 4096;

        unsigned numFatSect   = U32(&data[0x2C]);
        unsigned firstDirSect = U32(&data[0x30]);
        unsigned firstMiniFat = U32(&data[0x3C]);
        unsigned numMiniFat   = U32(&data[0x40]);
        unsigned firstDifat   = U32(&data[0x44]);
        unsigned numDifat     = U32(&data[0x48]);

        // ---- DIFAT：头部 109 项，之后是 DIFAT 扇区链，每扇区最后一个 UINT32 指向下一个 ----
        std::vector<unsigned> difat;
        for (int i = 0; i < 109; ++i) {
            unsigned v = U32(&data[0x4C + i * 4]);
            if (v != kFreeSect) difat.push_back(v);
        }
        unsigned sec = firstDifat;
        for (unsigned guard = 0; sec != kEndOfChain && sec != kFreeSect; ++guard) {
            if (guard > numDifat + 8) break;
            size_t off = SectorOffset(sec);
            if (off + sectorSize > data.size()) break;
            unsigned per = sectorSize / 4 - 1;
            for (unsigned i = 0; i < per; ++i) {
                unsigned v = U32(&data[off + i * 4]);
                if (v != kFreeSect) difat.push_back(v);
            }
            sec = U32(&data[off + per * 4]);
        }
        (void)numFatSect;

        // ---- FAT ----
        fat.clear();
        fat.reserve(difat.size() * (sectorSize / 4));
        for (size_t i = 0; i < difat.size(); ++i) {
            size_t off = SectorOffset(difat[i]);
            if (off + sectorSize > data.size()) continue;
            for (unsigned k = 0; k < sectorSize / 4; ++k) fat.push_back(U32(&data[off + k * 4]));
        }
        if (fat.empty()) return false;

        // ---- 目录 ----
        std::vector<unsigned char> dir;
        if (!ReadChain(firstDirSect, 0, false, dir)) return false;
        bool v3 = U16(&data[0x1A]) == 3;
        for (size_t p = 0; p + 128 <= dir.size(); p += 128) {
            unsigned nameLen = U16(&dir[p + 0x40]);
            if (nameLen > 64) nameLen = 64;
            Entry e;
            if (nameLen >= 2) {
                e.name.assign((const wchar_t*)(&dir[p]), nameLen / 2 - 1);
            }
            e.type  = dir[p + 0x42];
            e.start = U32(&dir[p + 0x74]);
            e.size  = v3 ? (unsigned long long)U32(&dir[p + 0x78]) : U64(&dir[p + 0x78]);
            entries.push_back(e);
        }
        if (entries.empty()) return false;

        // ---- 迷你流（小型流都住在这里，root 项给出位置与长度）----
        if (!entries.empty()) {
            const Entry& root = entries[0];
            if (root.type == 5 && root.size > 0) {
                ReadChain(root.start, (unsigned)root.size, false, miniStream);
            }
        }

        // ---- 迷你 FAT ----
        if (numMiniFat > 0 && firstMiniFat != kEndOfChain) {
            std::vector<unsigned char> mf;
            if (ReadChain(firstMiniFat, 0, false, mf)) {
                miniFat.reserve(mf.size() / 4);
                for (size_t i = 0; i + 4 <= mf.size(); i += 4) miniFat.push_back(U32(&mf[i]));
            }
        }
        return true;
    }

    size_t SectorOffset(unsigned sector) const { return (size_t)(sector + 1) * sectorSize; }

    bool ReadChain(unsigned start, unsigned size, bool mini, std::vector<unsigned char>& out) const {
        out.clear();
        const std::vector<unsigned>& table = mini ? miniFat : fat;
        const std::vector<unsigned char>& src = mini ? miniStream : data;
        const unsigned unit = mini ? miniSectorSize : sectorSize;

        unsigned sec = start;
        unsigned guard = 0;
        while (sec != kEndOfChain && sec != kFreeSect) {
            if (++guard > table.size() + 16) break;      // 环 / 脏数据
            size_t off = mini ? (size_t)sec * miniSectorSize : SectorOffset(sec);
            if (off + unit > src.size()) break;
            out.insert(out.end(), src.begin() + (long)off, src.begin() + (long)(off + unit));
            if (size && out.size() >= size) { out.resize(size); break; }
            if (sec >= table.size()) break;
            sec = table[sec];
        }
        if (size) return out.size() >= size;
        return !out.empty();
    }

    const Entry* Find(const wchar_t* name) const {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].name.size() != wcslen(name)) continue;
            if (_wcsicmp(entries[i].name.c_str(), name) == 0) return &entries[i];
        }
        return nullptr;
    }

    bool ReadStream(const Entry* e, std::vector<unsigned char>& out) const {
        if (!e || e->type != 2) return false;
        if (e->size == 0) { out.clear(); return true; }
        if (e->size < miniCutoff) return ReadChain(e->start, (unsigned)e->size, true, out);
        return ReadChain(e->start, (unsigned)e->size, false, out);
    }
};

// ======================= Word 97-2003（.doc）=======================

// 片段表里每一片的字符逐个过一遍：控制字符转成换行/制表，域代码丢掉
void AppendDocChars(const wchar_t* p, size_t n, std::wstring& out, int& fieldDepth) {
    for (size_t i = 0; i < n; ++i) {
        wchar_t c = p[i];
        if (c == 0x13) { ++fieldDepth; continue; }        // 域开始：后面是域代码
        if (c == 0x14) { if (fieldDepth > 0) --fieldDepth; continue; }  // 域代码结束，后面是结果
        if (c == 0x15) { if (fieldDepth > 0) --fieldDepth; continue; }  // 域结束
        if (fieldDepth > 0) continue;                      // 域代码本身不要

        switch (c) {
            case 0x0D:                                     // 段落标记
            case 0x0B:                                     // 手动换行
            case 0x0C:                                     // 分页/分节
                out += L'\n';
                break;
            case 0x07:                                     // 单元格 / 行结束
                out += L'\t';
                break;
            case 0x1E: out += L'-'; break;                 // 不间断连字符
            case 0x1F: break;                              // 可选连字符
            case 0xA0: out += L' '; break;                 // 不间断空格
            case 0x01: case 0x02: case 0x05: case 0x08:    // 图片 / 脚注引用 / 批注 / 图形
                break;
            default:
                if (c == L'\r' || c == L'\n' || c == L'\t' || c >= 0x20) out += c;
                break;
        }
    }
}

bool ExtractDocText(Cfb& cfb, std::wstring& out) {
    const Cfb::Entry* wd = cfb.Find(L"WordDocument");
    if (!wd) return false;

    std::vector<unsigned char> doc;
    if (!cfb.ReadStream(wd, doc) || doc.size() < 0x0200) return false;
    if (U16(&doc[0]) != 0xA5EC) return false;

    unsigned nFib = U16(&doc[2]);
    if (nFib < 193) return false;                     // Word 6.0 / 95：结构不同，放弃

    unsigned flags = U16(&doc[0x0A]);
    if (flags & 0x0100) return false;                 // 加密

    // 压缩片段（每字符 1 字节）规范上按 cp1252 解释，但中文版 Word 生成的
    // 文档里若出现双字节（部分第三方工具会这样写），按 cp1252 解会把一个字
    // 拆成两个西欧字符，中文搜索必然落空。按 FIB 的 lid 选代码页更稳。
    UINT cp = 1252;
    {
        unsigned lid = U16(&doc[0x06]);
        switch (lid) {
            case 0x0804: case 0x1004: cp = 936; break;   // 简中（大陆 / 新加坡）
            case 0x0404: case 0x0C04: cp = 950; break;   // 繁中（台湾 / 香港）
            case 0x0411: cp = 932; break;                // 日文
            case 0x0412: cp = 949; break;                // 韩文
            default: break;
        }
    }

    bool use1Table = (flags & 0x0200) != 0;
    unsigned ccpText = U32(&doc[0x4C]);
    if (ccpText == 0 || ccpText > 0x4000000u) return false;   // > 64M 字符：明显不对

    unsigned fcClx  = U32(&doc[0x01A2]);
    unsigned lcbClx = U32(&doc[0x01A6]);
    if (lcbClx == 0) return false;

    const Cfb::Entry* tbl = cfb.Find(use1Table ? L"1Table" : L"0Table");
    if (!tbl) tbl = cfb.Find(L"1Table");
    if (!tbl) tbl = cfb.Find(L"0Table");
    if (!tbl) return false;

    std::vector<unsigned char> table;
    if (!cfb.ReadStream(tbl, table)) return false;
    if ((unsigned long long)fcClx + lcbClx > table.size()) return false;

    // Clx = 若干 Prc（0x01） + 一段 Pcdt（0x02 + 长度 + PlcPcd）
    size_t p = fcClx, end = (size_t)fcClx + lcbClx;
    while (p < end && table[p] == 1) {
        if (p + 3 > end) return false;
        p += 3 + U16(&table[p + 1]);
    }
    if (p + 5 > end || table[p] != 2) return false;
    unsigned lcb = U32(&table[p + 1]);
    p += 5;
    if (p + lcb > table.size() || lcb < 16) return false;

    unsigned pieces = (lcb - 4) / 12;
    if (pieces == 0 || pieces > 200000) return false;
    const unsigned char* cps  = &table[p];                 // pieces+1 个字符位置
    const unsigned char* pcds = cps + (pieces + 1) * 4;    // 每片 8 字节

    out.clear();
    out.reserve(ccpText < 1u << 20 ? ccpText : 1u << 20);
    int fieldDepth = 0;

    for (unsigned i = 0; i < pieces; ++i) {
        unsigned cpStart = U32(cps + i * 4);
        unsigned cpEnd   = U32(cps + (i + 1) * 4);
        if (cpEnd <= cpStart) continue;
        if (cpStart >= ccpText) break;                    // 正文之后是脚注/页眉，不要

        unsigned take = cpEnd - cpStart;
        if (cpStart + take > ccpText) take = ccpText - cpStart;

        const unsigned char* pcd = pcds + i * 8;
        unsigned fcRaw = U32(pcd + 2);
        bool compressed = (fcRaw & 0x40000000u) != 0;
        unsigned fc = fcRaw & 0x3FFFFFFFu;
        if (compressed) fc /= 2;

        if (compressed) {
            // 坏分片只跳过这一段，不整篇判死（末尾若一个字都没有才算失败）
            if ((unsigned long long)fc + take > doc.size()) continue;
            const char* bytes = (const char*)&doc[fc];
            int need = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, bytes, (int)take, nullptr, 0);
            if (need <= 0) need = MultiByteToWideChar(cp, 0, bytes, (int)take, nullptr, 0);
            if (need > 0) {
                std::wstring w((size_t)need, L'\0');
                MultiByteToWideChar(cp, 0, bytes, (int)take, &w[0], need);
                AppendDocChars(w.c_str(), w.size(), out, fieldDepth);
            }
        } else {
            if ((unsigned long long)fc + (unsigned long long)take * 2 > doc.size()) continue;
            const wchar_t* w = (const wchar_t*)(&doc[fc]);
            AppendDocChars(w, take, out, fieldDepth);
        }
    }

    return !out.empty();
}

// ======================= Excel 97-2003（.xls，BIFF8）=======================

// SST 里的字符串可能跨 CONTINUE 记录，跨段时开头会多一个“选项字节”重新给出编码
struct SstSeg {
    const unsigned char* p;
    unsigned len;
};

struct SstReader {
    std::vector<SstSeg> segs;
    size_t si = 0;
    size_t off = 0;
    bool highByte = false;

    bool AtEnd() const { return si >= segs.size(); }

    bool RawU8(unsigned char& v) {
        while (si < segs.size() && off >= segs[si].len) { ++si; off = 0; }
        if (si >= segs.size()) return false;
        v = segs[si].p[off++];
        return true;
    }
    bool RawU16(unsigned& v) {
        unsigned char a = 0, b = 0;
        if (!RawU8(a) || !RawU8(b)) return false;
        v = (unsigned)a | ((unsigned)b << 8);
        return true;
    }
    bool RawU32(unsigned& v) {
        unsigned char a = 0, b = 0, c = 0, d = 0;
        if (!RawU8(a) || !RawU8(b) || !RawU8(c) || !RawU8(d)) return false;
        v = (unsigned)a | ((unsigned)b << 8) | ((unsigned)c << 16) | ((unsigned)d << 24);
        return true;
    }
    // 读字符前调用：若当前段读完，换段并读掉新的选项字节
    bool NextCharSeg() {
        if (si < segs.size() && off < segs[si].len) return true;
        ++si;
        off = 0;
        if (si >= segs.size()) return false;
        unsigned char flag = 0;
        if (!RawU8(flag)) return false;
        highByte = (flag & 1) != 0;
        return true;
    }
    bool Skip(unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            unsigned char c = 0;
            if (!RawU8(c)) return false;
        }
        return true;
    }
};

bool SstReadString(SstReader& r, std::wstring& out) {
    unsigned cch = 0;
    unsigned char grbit = 0;
    if (!r.RawU16(cch)) return false;
    if (!r.RawU8(grbit)) return false;
    if (cch > 32767) return false;

    bool high = (grbit & 1) != 0;
    unsigned cRun = 0, cbExt = 0;
    if (grbit & 0x08) { if (!r.RawU16(cRun)) return false; }
    if (grbit & 0x04) { if (!r.RawU32(cbExt)) return false; }

    r.highByte = high;
    out.clear();
    out.reserve(cch < 8192 ? cch : 8192);

    unsigned left = cch;
    while (left > 0) {
        if (!r.NextCharSeg()) return false;
        size_t avail = r.segs[r.si].len - r.off;
        size_t per = r.highByte ? 2 : 1;
        size_t can = avail / per;
        if (can == 0) {                     // 段里剩下的不够一个字符：当作脏数据，换段
            r.off = r.segs[r.si].len;
            continue;
        }
        if (can > left) can = left;
        const unsigned char* p = r.segs[r.si].p + r.off;
        for (size_t i = 0; i < can; ++i) {
            if (r.highByte) out.push_back((wchar_t)((unsigned)p[i * 2] | ((unsigned)p[i * 2 + 1] << 8)));
            else out.push_back((wchar_t)p[i]);
        }
        r.off += can * per;
        left -= (unsigned)can;
    }

    unsigned long long skip = (unsigned long long)cRun * 4 + cbExt;
    while (skip > 0) {                      // 富文本格式串 / 注音数据：跳过
        unsigned chunk = skip > 4096 ? 4096 : (unsigned)skip;
        if (!r.Skip(chunk)) return false;
        skip -= chunk;
    }
    return true;
}

// 单元格里自带的 Unicode 字符串：2 字节长度 + 1 字节选项 + 字符
std::wstring ReadXlString(const unsigned char* p, unsigned len) {
    if (len < 3) return std::wstring();
    unsigned cch = U16(p);
    bool high = (p[2] & 1) != 0;
    size_t need = 3 + (size_t)cch * (high ? 2 : 1);
    if (need > len) cch = (unsigned)((len - 3) / (high ? 2 : 1));
    std::wstring s;
    s.reserve(cch);
    const unsigned char* q = p + 3;
    for (unsigned i = 0; i < cch; ++i) {
        if (high) s.push_back((wchar_t)((unsigned)q[i * 2] | ((unsigned)q[i * 2 + 1] << 8)));
        else s.push_back((wchar_t)q[i]);
    }
    return s;
}

double RkToDouble(unsigned rk) {
    bool x100 = (rk & 1) != 0;
    bool isInt = (rk & 2) != 0;
    double v;
    if (isInt) {
        int i = (int)rk >> 2;               // 30 位有符号整数
        v = (double)i;
    } else {
        unsigned long long bits = ((unsigned long long)(rk & 0xFFFFFFFCu)) << 32;
        double d = 0;
        memcpy(&d, &bits, sizeof(d));
        v = d;
    }
    return x100 ? v / 100.0 : v;
}

std::wstring NumberToText(double v) {
    if (!(v == v) || v > 1e300 || v < -1e300) return std::wstring();
    wchar_t buf[64];
    swprintf(buf, 64, L"%.10g", v);
    return std::wstring(buf);
}

struct XlsSheetText {
    std::map<unsigned, std::wstring> cells;   // key = row * 1024 + col
    bool full = false;
};

void FlushSheet(const std::wstring& name, XlsSheetText& sheet, std::wstring& out) {
    if (sheet.cells.empty()) return;
    if (!out.empty() && out.back() != L'\n') out += L'\n';
    if (!name.empty()) {
        out += L"【工作表：";
        out += name;
        out += L"】\n";
    }
    unsigned lastRow = 0xFFFFFFFFu;
    for (std::map<unsigned, std::wstring>::const_iterator it = sheet.cells.begin();
         it != sheet.cells.end(); ++it) {
        unsigned row = it->first / 1024;
        if (row != lastRow) {
            if (lastRow != 0xFFFFFFFFu) out += L'\n';
            lastRow = row;
        } else {
            out += L'\t';
        }
        out += it->second;
    }
    out += L'\n';
    // 超限不能静默：用户会以为「这个大表搜不到后面几万行」是程序坏了
    if (sheet.full) {
        out += L"【表格过大，已截断：仅前 20 万个单元格参与搜索】\n";
    }
}

void PutCell(XlsSheetText& sheet, unsigned row, unsigned col, const std::wstring& text) {
    if (text.empty() || col >= 1024) return;
    if (sheet.cells.size() >= 200000) { sheet.full = true; return; }
    sheet.cells[row * 1024 + col] = text;
}

bool ExtractXlsText(Cfb& cfb, std::wstring& out) {
    const Cfb::Entry* wb = cfb.Find(L"Workbook");
    if (!wb) wb = cfb.Find(L"Book");
    if (!wb) return false;

    std::vector<unsigned char> st;
    if (!cfb.ReadStream(wb, st) || st.size() < 16) return false;

    std::vector<std::wstring> sst;
    std::map<unsigned, std::wstring> sheetNames;   // BOF 偏移 -> 工作表名
    XlsSheetText sheet;
    std::wstring sheetName;
    unsigned pendingRow = 0, pendingCol = 0;
    bool pendingFormulaString = false;
    bool biff8 = true;
    bool sawSheet = false;
    out.clear();

    size_t pos = 0;
    while (pos + 4 <= st.size()) {
        unsigned id  = U16(&st[pos]);
        unsigned len = U16(&st[pos + 2]);
        if (pos + 4 + len > st.size()) break;
        const unsigned char* d = &st[pos + 4];
        size_t recStart = pos;
        pos += 4 + len;

        switch (id) {
            case 0x0809: {   // BOF
                unsigned ver = len >= 2 ? U16(d) : 0;
                unsigned type = len >= 4 ? U16(d + 2) : 0;
                if (ver == 0x0600) biff8 = true;
                else if (ver == 0x0500) biff8 = false;
                if (type == 0x0010) {               // 工作表
                    FlushSheet(sheetName, sheet, out);
                    sheet.cells.clear();
                    sheet.full = false;
                    std::map<unsigned, std::wstring>::const_iterator it = sheetNames.find((unsigned)recStart);
                    sheetName = (it == sheetNames.end()) ? std::wstring() : it->second;
                    sawSheet = true;
                } else if (type == 0x0005) {        // 工作簿全局
                    FlushSheet(sheetName, sheet, out);
                    sheet.cells.clear();
                    sheetName.clear();
                }
                pendingFormulaString = false;
                break;
            }
            case 0x0085: {   // BOUNDSHEET
                if (len < 8) break;
                unsigned lbPlyPos = U32(d);
                unsigned cch = d[6];
                unsigned char grbit = d[7];
                std::wstring nm;
                bool high = (grbit & 1) != 0;
                size_t p = 8;
                for (unsigned i = 0; i < cch; ++i) {
                    if (high) {
                        if (p + 1 >= len) break;
                        nm.push_back((wchar_t)((unsigned)d[p] | ((unsigned)d[p + 1] << 8)));
                        p += 2;
                    } else {
                        if (p >= len) break;
                        nm.push_back((wchar_t)d[p]);
                        p += 1;
                    }
                }
                sheetNames[lbPlyPos] = nm;
                break;
            }
            case 0x00FC: {   // SST（共享字符串表）
                SstReader r;
                r.segs.push_back(SstSeg{ d, len });
                size_t q = pos;
                while (q + 4 <= st.size() && U16(&st[q]) == 0x003C) {   // CONTINUE
                    unsigned clen = U16(&st[q + 2]);
                    if (q + 4 + clen > st.size()) break;
                    r.segs.push_back(SstSeg{ &st[q + 4], clen });
                    q += 4 + clen;
                }
                pos = q;
                unsigned total = 0, unique = 0;
                if (!r.RawU32(total) || !r.RawU32(unique)) break;
                (void)total;
                if (unique > 1000000) unique = 1000000;
                sst.reserve(unique);
                for (unsigned i = 0; i < unique && !r.AtEnd(); ++i) {
                    std::wstring s;
                    if (!SstReadString(r, s)) break;
                    sst.push_back(s);
                }
                break;
            }
            case 0x00FD: {   // LABELSST
                if (len < 10) break;
                unsigned isst = U32(d + 6);
                unsigned row = U16(d), col = U16(d + 2);
                if (isst < sst.size()) PutCell(sheet, row, col, sst[isst]);
                break;
            }
            case 0x0204:     // LABEL（BIFF8 里是 Unicode 串）
            case 0x00D6: {   // RSTRING
                if (len < 6) break;
                PutCell(sheet, U16(d), U16(d + 2), ReadXlString(d + 6, len - 6));
                break;
            }
            case 0x0203: {   // NUMBER
                if (len < 14) break;
                double v = 0;
                memcpy(&v, d + 6, 8);
                PutCell(sheet, U16(d), U16(d + 2), NumberToText(v));
                break;
            }
            case 0x027E: {   // RK
                if (len < 10) break;
                PutCell(sheet, U16(d), U16(d + 2), NumberToText(RkToDouble(U32(d + 6))));
                break;
            }
            case 0x00BD: {   // MULRK
                if (len < 6) break;
                unsigned row = U16(d);
                unsigned colFirst = U16(d + 2);
                unsigned n = (len - 6) / 6;
                for (unsigned i = 0; i < n; ++i) {
                    unsigned rk = U32(d + 4 + i * 6 + 2);
                    PutCell(sheet, row, colFirst + i, NumberToText(RkToDouble(rk)));
                }
                break;
            }
            case 0x0006: {   // FORMULA（只取缓存结果）
                if (len < 20) break;
                unsigned row = U16(d), col = U16(d + 2);
                const unsigned char* res = d + 6;
                if (res[6] == 0xFF && res[7] == 0xFF) {
                    pendingFormulaString = true;    // 字符串结果在紧随其后的 STRING 记录里
                    pendingRow = row;
                    pendingCol = col;
                } else if (res[6] == 0 && res[7] == 0) {
                    PutCell(sheet, row, col, res[2] ? L"TRUE" : L"FALSE");
                } else {
                    double v = 0;
                    memcpy(&v, res, 8);
                    PutCell(sheet, row, col, NumberToText(v));
                }
                break;
            }
            case 0x0207: {   // STRING（FORMULA 的字符串结果）
                if (!pendingFormulaString || len < 3) break;
                pendingFormulaString = false;
                PutCell(sheet, pendingRow, pendingCol, ReadXlString(d, len));
                break;
            }
            case 0x0205: {   // BOOLERR
                if (len < 8) break;
                if (d[7] == 0) PutCell(sheet, U16(d), U16(d + 2), d[6] ? L"TRUE" : L"FALSE");
                break;
            }
            default:
                break;
        }
    }
    FlushSheet(sheetName, sheet, out);

    if (!biff8) {           // Excel 5.0/95：字符串编码规则不同，宁可不给错内容
        out.clear();
        return false;
    }
    if (!sawSheet && sst.empty()) return false;
    return !out.empty();
}

}  // namespace

bool ExtractLegacyOfficeText(const std::wstring& path, std::wstring& outText) {
    outText.clear();
    Cfb cfb;
    if (!cfb.Open(path)) return false;

    std::wstring text;
    if (cfb.Find(L"WordDocument")) {
        if (ExtractDocText(cfb, text)) {
            outText = text;
            return true;
        }
    }
    text.clear();
    if (ExtractXlsText(cfb, text)) {
        outText = text;
        return true;
    }
    return false;
}
