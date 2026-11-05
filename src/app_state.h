#pragma once
// app_state.h - main.cpp 持有实体、各 UI 模块引用的共享状态与接口
#include "common.h"

struct AppState {
    Settings settings;
    SearchOutcome outcome;
    std::wstring lastOcrRaw;
    std::wstring statusNote;
    int  selected = -1;
    int  scrollY  = 0;
    int  totalH   = 0;
    unsigned seq  = 0;
    volatile LONG searching  = 0;
    volatile LONG capturing  = 0;
    Capabilities caps;
};

extern AppState g;
extern HWND g_hwndMain;
extern HWND g_hwndList;
extern int  g_lineH;

// 状态栏左段（main.cpp 实现）
void SetLeftStatus(const std::wstring& s);

// 结果列表（listview.cpp 实现）
void RecalcTotalHeight();   // 结果变化或窗口宽度变化后重算折行与总高度
void ShowMoreResults();     // 把暂存的命中追加一批到列表（「加载更多」）
void EnsureItemVisible(int index);
void CopyResultToClipboard(const ResultItem& it);
void OpenItem(int index);
void OpenItemFolder(int index);        // 在资源管理器里定位文件
void CopyItemPath(int index);          // 只复制文件路径
void ScrollListByPixels(int dy);       // 按像素滚动（自动夹紧），翻页键用
void ScrollListToEdge(bool bottom);    // 滚到顶/底
bool ListMenuOpen();                   // 右键菜单是否正在弹出（此期间列表不吃键盘）
LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
