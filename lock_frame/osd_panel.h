#pragma once
// ===========================================================================
//  osd_panel.h - draw one line of text into a layered window with per-pixel
//  alpha (premultiplied), pill-shaped translucent background.
//
//  Shared by bridge.cpp (the real OSD) and osd_test.cpp (a standalone harness
//  that renders sample lines on a fake "game" window, so the layout can be
//  checked without launching a game).
//
//  Why GDI + UpdateLayeredWindow: the overlay swap chain is flip-model (no
//  IDXGISurface1::GetDC) and sits in the frame-generation path - a separate
//  layered window is independent of it, cannot disturb presentation, and one
//  blit every 400 ms is free.
// ===========================================================================
static void osd_panel_render(HWND wnd, const wchar_t* text, int len,
                             int scr_x, int scr_y, int dpi)
{
    if (!wnd || !text || len <= 0) { return; }
    if (dpi <= 0) { dpi = 96; }

    const int fh = MulDiv(13, dpi, 96);          // ~13 px tall at 100 %
    const int pad_x = MulDiv(10, dpi, 96);
    const int pad_y = MulDiv(4, dpi, 96);

    HDC screen = GetDC(nullptr);
    if (!screen) { return; }
    HDC mdc = CreateCompatibleDC(screen);
    HFONT font = CreateFontW(-fh, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Segoe UI");
    if (!mdc || !font) {
        if (font) { DeleteObject(font); }
        if (mdc) { DeleteDC(mdc); }
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ old_font = SelectObject(mdc, font);
    SIZE sz{};
    GetTextExtentPoint32W(mdc, text, len, &sz);
    const int w = sz.cx + pad_x * 2;
    const int h = sz.cy + pad_y * 2;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                  // top-down rows
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        HGDIOBJ old_bmp = SelectObject(mdc, dib);
        BYTE* px = static_cast<BYTE*>(bits);
        memset(px, 0, static_cast<size_t>(w) * h * 4);

        // Pill-shaped translucent panel first: alpha only, colour stays black, so
        // the result composites as a dark scrim whatever is behind it.
        const int rr = h / 2;
        for (int y = 0; y < h; ++y) {
            const int dy = (y < rr) ? (rr - y) : ((y >= h - rr) ? (y - (h - 1 - rr)) : 0);
            for (int x = 0; x < w; ++x) {
                const int dx = (x < rr) ? (rr - x) : ((x >= w - rr) ? (x - (w - 1 - rr)) : 0);
                if (dx && dy && dx * dx + dy * dy > rr * rr) { continue; }
                px[(static_cast<size_t>(y) * w + x) * 4 + 3] = 150;
            }
        }

        // Text on top. GDI writes its anti-aliased coverage into RGB and never
        // touches the alpha byte, so one pass afterwards turns coverage into
        // premultiplied white.
        SetBkMode(mdc, TRANSPARENT);
        SetTextColor(mdc, RGB(255, 255, 255));
        RECT tr{ pad_x, pad_y, w - pad_x, h - pad_y };
        DrawTextW(mdc, text, len, &tr,
                  DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX);

        for (size_t i = 0, cnt = static_cast<size_t>(w) * h; i < cnt; ++i) {
            BYTE* p = px + i * 4;
            const BYTE v = (p[0] > p[1]) ? ((p[0] > p[2]) ? p[0] : p[2])
                                        : ((p[1] > p[2]) ? p[1] : p[2]);
            const int a = v + p[3] * (255 - v) / 255;
            p[0] = p[1] = p[2] = v;
            p[3] = static_cast<BYTE>(a > 255 ? 255 : a);
        }

        POINT dst{ scr_x, scr_y };
        SIZE size{ w, h };
        POINT src{ 0, 0 };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(wnd, screen, &dst, &size, mdc, &src, 0, &bf, ULW_ALPHA);
        // Show and re-assert the topmost/z-order bits only on the FIRST render.
        // UpdateLayeredWindow() already positions the window (dst above), so doing
        // ShowWindow + SetWindowPos(HWND_TOPMOST) on every update only churned the
        // window stack 2.5 times a second: the overlay re-asserts its own topmost
        // position in the same 400 ms beat, so the OSD kept getting covered and
        // uncovered (a visible slow flicker) and every recompose kicked the overlay
        // out of its independent flip (present went from 5.3 ms to 7.2 ms, which
        // cost input frames).
        static HWND primed = nullptr;
        if (primed != wnd) {
            primed = wnd;
            ShowWindow(wnd, SW_SHOWNOACTIVATE);
            SetWindowPos(wnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        SelectObject(mdc, old_bmp);
        DeleteObject(dib);
    }
    SelectObject(mdc, old_font);
    DeleteObject(font);
    DeleteDC(mdc);
    ReleaseDC(nullptr, screen);
}
