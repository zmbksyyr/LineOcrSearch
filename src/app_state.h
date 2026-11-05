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
void EnsureItemVisible(int index);
void CopyResultToClipboard(const ResultItem& it);
void OpenItem(int index);
LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
