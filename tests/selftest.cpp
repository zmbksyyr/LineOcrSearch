// selftest.cpp - 命令行自测：文本清洗、目录搜索、上下文、编码探测
#include "../src/common.h"
#include <cstdio>

static int g_fail = 0;

static void Check(bool cond, const char* name) {
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) ++g_fail;
}

static std::wstring W(const char* utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring s((size_t)(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &s[0], n);
    return s;
}

static bool WriteFileBytes(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
    return ok;
}

static void WriteUtf8(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], n, nullptr, nullptr);
    WriteFileBytes(path, out);
}

static void WriteGbk(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(936, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(936, 0, text.c_str(), (int)text.size(), &out[0], n, nullptr, nullptr);
    WriteFileBytes(path, out);
}

static void TestClean() {
    printf("-- CleanOcrText --\n");
    Check(CleanOcrText(L"  ▸【应收账款】·  ") == L"应收账款", "strip surrounding marks");
    Check(CleanOcrText(L"1.我的142个") == L"我的", "digits are cut too");
    Check(CleanOcrText(L"  1,284,600 元 ").empty(), "pure number+unit becomes empty");
    Check(CleanOcrText(L"·2026年10月账单…") == L"月账单", "digits ignored at head");
    Check(CleanOcrText(L"  【】||  ").empty(), "empty when no text char");
    Check(CountTextChars(L"1.我的142个") == 3, "count text chars ignores digits");
    Check(CleanOcrText(L"hello world") == L"hello world", "keeps inner space");
    // 空格只在两侧都是拉丁字母时才不断段：OCR 前缀噪声不会粘进关键词
    Check(CleanOcrText(L"Dekap 改为系统控件吧") == L"改为系统控件吧",
          "latin noise before CJK is split off");
    Check(CleanOcrText(L"本期 应收账款") == L"应收账款",
          "space between CJK splits segments");
    Check(CleanOcrText(L"hello world 你好") == L"hello world",
          "latin run and CJK run are separate");

    // 以鼠标右侧为准：给出每个字的水平位置，取「包含鼠标或位于鼠标右侧」的第一段
    // 排版假设：第 i 个字中心在 x = 10 + 20*i；句子共 26 字
    const std::wstring line = L"改为系统控件吧，这个也是丑陋，还不如用原生的好了。";
    std::vector<double> xs;
    for (size_t i = 0; i < line.size(); ++i) xs.push_back(10.0 + 20.0 * (double)i);

    // 0-6 改为系统控件吧 | 7 ， | 8-13 这个也是丑陋 | 14 ， | 15-24 还不如用原生的好了
    Check(CleanOcrTextByX(line, xs, 70.0) == L"改为系统控件吧",
          "mouse on segment 1 -> segment 1");
    Check(CleanOcrTextByX(line, xs, 150.0) == L"这个也是丑陋",
          "mouse on comma -> segment to the right, not the longest one");
    Check(CleanOcrTextByX(line, xs, 410.0) == L"还不如用原生的好了",
          "mouse on segment 3 -> segment 3");
    Check(CleanOcrTextByX(line, xs, 0.0) == L"改为系统控件吧",
          "mouse left of text -> first segment");
    Check(CleanOcrTextByX(line, xs, 9000.0) == L"还不如用原生的好了",
          "mouse right of text -> last segment");

    // 位置表长度不匹配时退回「取最长段」，不许崩
    std::vector<double> bad(3, 0.0);
    Check(CleanOcrTextByX(line, bad, 70.0) == L"还不如用原生的好了",
          "bad positions -> falls back to longest segment");
}

// 笔尖左锚点取词：笔尖只表示「从这里开始」，左边一律不要，右边取到行尾
static void TestTakeRight() {
    printf("-- TakeRightOfCursor --\n");
    const std::wstring line = L"改为系统控件吧，这个也是丑陋，还不如用原生的好了。";
    std::vector<double> xs;
    for (size_t i = 0; i < line.size(); ++i) xs.push_back(10.0 + 20.0 * (double)i);

    Check(TakeRightOfCursor(line, xs, 0.0) ==
              L"改为系统控件吧，这个也是丑陋，还不如用原生的好了",
          "cursor left of line -> whole line, tail punctuation trimmed");
    Check(TakeRightOfCursor(line, xs, 50.0) == L"系统控件吧，这个也是丑陋，还不如用原生的好了",
          "cursor inside a char -> starts at that char");
    Check(TakeRightOfCursor(line, xs, 150.0) == L"这个也是丑陋，还不如用原生的好了",
          "cursor on comma -> left side dropped, right side kept in one piece");
    Check(TakeRightOfCursor(line, xs, 9000.0).empty(),
          "cursor right of all text -> empty, no guessing");

    // 行首编号 / 装饰符被剥掉，正文里的标点原样保留（搜索要按原句命中）
    const std::wstring numbered = L"（3）总则：本办法适用于全线。";
    std::vector<double> nx;
    for (size_t i = 0; i < numbered.size(); ++i) nx.push_back(10.0 + 20.0 * (double)i);
    Check(TakeRightOfCursor(numbered, nx, 0.0) == L"总则：本办法适用于全线",
          "leading number + brackets cut, inner punctuation kept");

    // 任务管理器那种「名字后面跟计数」的写法：两端剥掉，英文词间空格保留
    const std::wstring proc = L"Microsoft Edge (8)";
    std::vector<double> px;
    for (size_t i = 0; i < proc.size(); ++i) px.push_back(10.0 + 9.0 * (double)i);
    Check(TakeRightOfCursor(proc, px, 0.0) == L"Microsoft Edge",
          "trailing counter cut, english space kept");

    // 没有坐标表（UIA 只给了文字）：整段两端去噪
    Check(TakeRightOfCursor(L"·【应收账款】·", std::vector<double>(), 0.0) == L"应收账款",
          "no positions -> trim both ends only");

    // firstLeft：第一个被采纳字的左边缘，供「右侧空白门槛」使用
    {
        double fl = -1.0;
        TakeRightOfCursor(line, xs, 50.0, nullptr, nullptr, &fl);
        Check(fl >= 30.0 && fl <= 50.0, "firstLeft near the char under cursor");
        TakeRightOfCursor(line, xs, 150.0, nullptr, nullptr, &fl);
        Check(fl >= 150.0 && fl <= 170.0, "firstLeft near next char after comma");
        TakeRightOfCursor(L"纯文字", std::vector<double>(), 0.0, nullptr, nullptr, &fl);
        Check(fl < 0.0, "no positions -> firstLeft unset");
        TakeRightOfCursor(line, xs, 9000.0, nullptr, nullptr, &fl);
        Check(fl < 0.0, "nothing right of cursor -> firstLeft unset");
    }

    printf("-- TextSimilarity --\n");
    Check(TextSimilarity(L"状态栏", L"状态栏") > 0.999, "identical -> 1.0");
    Check(TextSimilarity(L"图标", L"状态栏") < 0.1, "unrelated -> near 0");
    Check(TextSimilarity(L"Microsoft Edge", L"Microsoft Edge (8)") > 0.8,
          "same words with a counter -> high");
    Check(TextSimilarity(L"闭塞设备", L"闭塞设备") == 1.0, "cjk identical");
}

static void TestSearch() {
    printf("-- RunSearch --\n");
    // 固定相对当前工作目录，避免测试因 exe 位置不同而失效。
    // 用独立子目录：tests\tmp 下还会放临时探针/截图，混在一起会污染命中数。
    std::wstring dir = L"tests\\tmp\\selftest_search";
    CreateDirectoryW(L"tests", nullptr);
    CreateDirectoryW(L"tests\\tmp", nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);

    WriteUtf8(JoinPath(dir, L"a.txt"),
              L"第一行\n本期应收账款余额为 1,284,600 元\n第三行\n第四行\n第五行\n");
    WriteGbk(JoinPath(dir, L"b.txt"),
             L"标题\n这里含有测试关键词ABC\n结尾\n");
    WriteUtf8(JoinPath(dir, L"skip.log"), L"no match here\n");

    SearchRequest req;
    req.keyword = L"余额";
    req.rootDir = dir;
    req.contextLevel = 1;
    req.maxResults = 200;
    req.searchOffice = false;

    SearchOutcome o = RunSearch(req);
    Check(o.items.size() == 1, "utf8 file hit count = 1");
    if (!o.items.empty()) {
        const ResultItem& it = o.items[0];
        Check(it.lineNo == 2, "hit line number = 2");
        Check(it.hitStart == 6, "hit start = 6");
        Check(it.hitLen == 2, "hit length = 2");
        Check(it.context.size() == 5, "context lines = 5 (L1..L5 clamp)");
        Check(it.hitIndex == 1, "hit index inside context = 1");
    }

    SearchRequest req2;
    req2.keyword = L"测试关键词ABC";
    req2.rootDir = dir;
    req2.contextLevel = 0;
    req2.maxResults = 200;
    req2.searchOffice = false;
    SearchOutcome o2 = RunSearch(req2);
    Check(o2.items.size() == 1, "gbk file hit count = 1");
    if (!o2.items.empty()) {
        Check(o2.items[0].lineNo == 2, "gbk hit line = 2");
        Check(o2.items[0].hitStart == 4, "gbk hit start = 4");
    }

    // 大小写不敏感
    SearchRequest req3;
    req3.keyword = L"abc";
    req3.rootDir = dir;
    req3.maxResults = 200;
    SearchOutcome o3 = RunSearch(req3);
    Check(o3.items.size() == 1, "case-insensitive hit = 1");

    // 结果上限
    SearchRequest req4;
    req4.keyword = L"行";
    req4.rootDir = dir;
    req4.maxResults = 1;
    SearchOutcome o4 = RunSearch(req4);
    Check((int)o4.items.size() <= 1, "maxResults respected");

    // ---- 多关键词 AND ----
    WriteUtf8(JoinPath(dir, L"multi.txt"),
              L"应收账款和预付账款都增加\n应收账款减少了\n预付账款没有变化\n什么都不含\n");
    SearchRequest m1;
    m1.keyword = L"应收账款 预付账款";
    m1.rootDir = dir;
    m1.contextLevel = 0;
    m1.maxResults = 200;
    m1.searchOffice = false;
    SearchOutcome m1o = RunSearch(m1);
    Check(m1o.items.size() == 1, "AND: only line with both words hits");
    if (!m1o.items.empty()) {
        const ResultItem& it = m1o.items[0];
        Check(it.hitRanges.size() == 2, "AND: two highlight ranges");
        Check(it.hitRanges[0].start == 0 && it.hitRanges[0].len == 4, "AND: first range = 应收账款");
        Check(it.hitRanges[1].start == 5 && it.hitRanges[1].len == 4, "AND: second range = 预付账款");
        Check(it.hitStart == 0 && it.hitLen == 4, "AND: legacy fields = first range");
    }

    // 词顺序无关
    SearchRequest m2;
    m2.keyword = L"本期 应收账款";
    m2.rootDir = dir;
    m2.contextLevel = 0;
    m2.maxResults = 200;
    m2.searchOffice = false;
    Check(RunSearch(m2).items.size() == 1, "AND: word order irrelevant");

    // 任一词缺失就不命中
    SearchRequest m3;
    m3.keyword = L"应收账款 利润";
    m3.rootDir = dir;
    m3.contextLevel = 0;
    m3.maxResults = 200;
    m3.searchOffice = false;
    Check(RunSearch(m3).items.size() == 0, "AND: missing word = no hit");

    // 连续空格/全角空格照常拆分
    SearchRequest m4;
    m4.keyword = L"应收账款　　本期";
    m4.rootDir = dir;
    m4.contextLevel = 0;
    m4.maxResults = 200;
    m4.searchOffice = false;
    Check(RunSearch(m4).items.size() == 1, "AND: fullwidth/multiple spaces split too");
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    TestClean();
    TestTakeRight();
    TestSearch();
    printf("----\n%s: %d failure(s)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
