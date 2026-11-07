// ===========================================================================
//  osd_test.cpp - render sample OSD lines on a fake "game" window so the layout
//  can be checked without launching a game.
//
//  build (on the Windows side, from <STAGE_DIR>\osd):
//    cl /nologo /std:c++17 /EHsc /O2 /MD /utf-8 /permissive- ^
//       /I<repo>\src\bridge osd_test.cpp /Fe:osd_test.exe /link user32.lib gdi32.lib
//
//  It creates a 1280x200 window painted like a dark game frame, then draws three
//  variants of the single OSD line (short / with lock / with lock+drops) so the
//  pill, the font and the spacing can be eyeballed at once. Closes after 20 s or
//  on Esc.
// ===========================================================================
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "osd_panel.h"

static const wchar_t* kClass = L"osd_test_wnd";
static HWND g_fake = nullptr;

static LRESULT CALLBACK fake_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        RECT r{};
        GetClientRect(h, &r);
        // a mid-dark "game frame" so the translucent pill is visible
        HBRUSH b = CreateSolidBrush(RGB(28, 30, 34));
        FillRect(dc, &r, b);
        DeleteObject(b);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_KEYDOWN && wp == VK_ESCAPE) { DestroyWindow(h); return 0; }
    return DefWindowProcW(h, m, wp, lp);
}

static HWND make_fake_window()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = fake_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kClass;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);
    return CreateWindowExW(WS_EX_TOPMOST, kClass, L"mgpu OSD test",
                           WS_POPUP | WS_VISIBLE, 100, 100, 1280, 220,
                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// One layered window per sample line, each 40 px lower than the last.
static HWND make_osd_window()
{
    static bool once = false;
    if (!once) {
        once = true;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"osd_test_layer";
        RegisterClassExW(&wc);
    }
    HWND h = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST,
        L"osd_test_layer", L"", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    return h;
}

int main()
{
    const UINT dpi = GetDpiForWindow(GetDesktopWindow()) ? 96 : 96;   // 100 % here
    g_fake = make_fake_window();
    if (!g_fake) { printf("no fake window\n"); return 1; }
    UpdateWindow(g_fake);

    struct Sample { const wchar_t* text; };
    const Sample samples[] = {
        { L"120 fps   x5   in 24.0 fps   gpu 7.9 ms   game 41.2 ms" },
        { L"119 fps   x5   in 23.8 fps   gpu 7.9 ms   game 41.9 ms   lock 24" },
        { L"118 fps   x5   in 23.6 fps   gpu 8.4 ms   game 42.3 ms   lock 24   drop 3" },
        { L"60 fps   x2   in 30.0 fps   gpu 12.3 ms   game 33.3 ms   drop 118" },
    };

    RECT cr{};
    GetClientRect(g_fake, &cr);
    POINT org{ cr.left, cr.top };
    ClientToScreen(g_fake, &org);

    int y = org.y + 12;
    for (const Sample& s : samples) {
        HWND w = make_osd_window();
        if (!w) { continue; }
        const int len = (int)wcslen(s.text);
        osd_panel_render(w, s.text, len, org.x + 12, y, (int)dpi);
        y += 34;
    }
    printf("rendered %d lines at %d,%d dpi=%u; window closes in 30s\n",
           (int)(sizeof(samples) / sizeof(samples[0])), org.x + 12, org.y + 12, dpi);

    const ULONGLONG t0 = GetTickCount64();
    MSG msg{};
    while (GetTickCount64() - t0 < 30000) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { return 0; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(50);
        if (!IsWindow(g_fake)) { break; }
    }
    return 0;
}
