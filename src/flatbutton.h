#pragma once
#include "common.h"

// 扁平自绘按钮：统一字体、圆角、悬停与按下反馈
void RegisterFlatButton();
HWND CreateFlatButton(HWND parent, int id, const wchar_t* text, bool primary);
void SetFlatButtonText(HWND h, const wchar_t* text);
