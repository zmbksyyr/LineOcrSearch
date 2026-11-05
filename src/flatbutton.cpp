#include "flatbutton.h"
#include "theme.h"

namespace {

struct BtnState {
    bool primary = false;
    bool hover = false;
    bool pressed = false;
    std::wstring text;
};

void DrawBtn(HWND h, BtnState* st) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(h, &ps);
    RECT rc{};
    GetClientRect(h, &rc);

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    SetBkMode(mem, TRANSPARENT);

    // 与父窗口同色打底，保证圆角外没有白边
    HBRUSH bg = CreateSolidBrush(theme::kPageBg);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    RECT r{ 0, 0, rc.right, rc.bottom };
    COLORREF textColor = theme::kTextMain;

    if (st->primary) {
        COLORREF fill = theme::kBtnDark;
        if (st->pressed) fill = RGB(30, 33, 39);
        else if (st->hover) fill = theme::kBtnDarkHi;
        theme::FillRoundRect(mem, r, 6, fill);
        textColor = RGB(255, 255, 255);
    } else {
        if (st->pressed) theme::FillRoundRect(mem, r, 6, RGB(236, 238, 241));
        else if (st->hover) theme::FillRoundRect(mem, r, 6, theme::kBtnLightHi);
        else theme::FillRoundRect(mem, r, 6, theme::kCardBg);
        theme::StrokeRoundRect(mem, r, 6, theme::kBorder);
        textColor = theme::kTextMain;
    }

    HGDIOBJ oldFont = SelectObject(mem, theme::FontUI());
    SIZE sz{};
    GetTextExtentPoint32W(mem, st->text.c_str(), (int)st->text.size(), &sz);
    int tx = (rc.right - sz.cx) / 2;
    int ty = (rc.bottom - sz.cy) / 2;
    SetTextColor(mem, textColor);
    TextOutW(mem, tx, ty, st->text.c_str(), (int)st->text.size());
    SelectObject(mem, oldFont);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

LRESULT CALLBACK BtnProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    BtnState* st = (BtnState*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
            st = (BtnState*)cs->lpCreateParams;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
            return 0;
        }
        case WM_PAINT:
            if (st) DrawBtn(h, st);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE:
            if (st && !st->hover) {
                st->hover = true;
                InvalidateRect(h, nullptr, FALSE);
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
            }
            return 0;
        case WM_MOUSELEAVE:
            if (st) {
                st->hover = false;
                st->pressed = false;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (st) {
                st->pressed = true;
                SetCapture(h);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (st) {
                bool wasPressed = st->pressed;
                st->pressed = false;
                ReleaseCapture();
                InvalidateRect(h, nullptr, FALSE);
                if (wasPressed) {
                    HWND parent = GetParent(h);
                    int id = GetDlgCtrlID(h);
                    SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)h);
                }
            }
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        case WM_ENABLE:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            delete st;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void RegisterFlatButton() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = BtnProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"LineOcrFlatBtn";
    RegisterClassExW(&wc);
    done = true;
}

HWND CreateFlatButton(HWND parent, int id, const wchar_t* text, bool primary) {
    RegisterFlatButton();
    BtnState* st = new BtnState();
    st->primary = primary;
    st->text = text ? text : L"";
    return CreateWindowExW(0, L"LineOcrFlatBtn", L"", WS_CHILD | WS_VISIBLE,
                           0, 0, 80, 30, parent, (HMENU)(INT_PTR)id,
                           GetModuleHandleW(nullptr), st);
}

void SetFlatButtonText(HWND h, const wchar_t* text) {
    BtnState* st = (BtnState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!st) return;
    st->text = text ? text : L"";
    InvalidateRect(h, nullptr, FALSE);
}
