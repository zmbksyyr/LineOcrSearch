#pragma once
#include "common.h"

// 统一视觉规范：一种字体、一个字号、一套中性色。
// 文字不使用粗体、不使用彩色标题；唯一的颜色例外是命中词标红（功能需要）。
namespace theme {

const COLORREF kPageBg    = RGB(246, 247, 249);
const COLORREF kCardBg    = RGB(255, 255, 255);
const COLORREF kBorder    = RGB(228, 230, 235);
const COLORREF kBorderStrong = RGB(206, 210, 217);
const COLORREF kTextMain  = RGB(45, 49, 56);
const COLORREF kTextSub   = RGB(138, 143, 152);
const COLORREF kTextFaint = RGB(178, 182, 190);
const COLORREF kHitText   = RGB(198, 40, 40);
const COLORREF kHitBg     = RGB(255, 242, 242);
const COLORREF kSelBg     = RGB(240, 245, 255);
const COLORREF kHoverBg   = RGB(249, 250, 251);
const COLORREF kBtnDark   = RGB(45, 49, 56);
const COLORREF kBtnDarkHi = RGB(66, 71, 80);
const COLORREF kBtnLightHi = RGB(244, 245, 247);

// 全局唯一字体：14px 常规字重
HFONT FontUI();
int   LineHeight();
void  InitFonts();
void  DoneFonts();

// 圆角填充矩形（不依赖 GDI+）
void FillRoundRect(HDC dc, const RECT& r, int radius, COLORREF fill);
void StrokeRoundRect(HDC dc, const RECT& r, int radius, COLORREF border);

// 绘制一行文字（统一颜色与字体）
void DrawTextLine(HDC dc, int x, int y, const std::wstring& s, COLORREF color, int maxRight);

}  // namespace theme
