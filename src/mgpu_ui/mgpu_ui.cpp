// ============================================================================
//  mgpu_ui.cpp  -  Lossless-Scaling-style front-end for bridge.exe
//
//  Layout and look follow Lossless Scaling 3.2.2: a dark frameless window with its
//  own title bar, a profile sidebar on the left, and two columns of cards on the
//  right. Every control is drawn by hand (no child windows) so the dark theme stays
//  consistent: dropdowns open a popup list drawn by us, toggles are pill switches,
//  numbers are spinners with up/down chevrons.
//
//  What it actually does (the point, regardless of the skin):
//    * enumerate visible top-level windows and pick any of them (-> --hwnd)
//    * start/stop bridge with a global hotkey or the button in the title bar
//    * edit the knobs that matter and see them applied as bridge flags
//    * profile list on the left; each profile keeps its own settings + target window
//    * live stats parsed from bridge's stdout shown in the bottom bar
//
//  Build: src/mgpu_ui/build.bat   (driver: scripts/build_ui.sh)
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <dxgi.h>
#include <cstdarg>
#include <cstring>

#include "mgpu_shared.h"

#include <atomic>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")

// ------------------------------------------------------------- GPU adapters
// The UI used to show a hard-coded "AMD Radeon RX 6750 GRE" and always let bridge
// pick the consumer GPU itself. Now the GPU is a real choice: bridge takes
// `--gpu <index>`, and the index here is the same DXGI enumeration order.
struct AdapterInfo
{
    int          index = 0;
    std::wstring name;
    UINT         vendor = 0;
};

static std::vector<AdapterInfo> g_adapters;

static void enum_adapters()
{
    g_adapters.clear();
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory))) || !factory) {
        return;
    }
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) { break; }
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d)) &&
            !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            wchar_t buf[192]{};
            _snwprintf_s(buf, _TRUNCATE, L"#%u  %.110s", i, d.Description);
            AdapterInfo a;
            a.index = static_cast<int>(i);
            a.name = buf;
            a.vendor = d.VendorId;
            g_adapters.push_back(a);
        }
        adapter->Release();
    }
    factory->Release();
}

// ------------------------------------------------------------------- theme
static const COLORREF C_BG        = RGB(24, 24, 24);   // window background
static const COLORREF C_SIDEBAR   = RGB(20, 20, 20);   // left panel
static const COLORREF C_CARD      = RGB(37, 37, 37);   // card body
static const COLORREF C_CARD_HEAD = RGB(30, 30, 30);   // card header strip
static const COLORREF C_CTL       = RGB(46, 46, 46);   // control fill
static const COLORREF C_TRACK_OFF = RGB(74, 74, 74);   // toggle track when off
static const COLORREF C_CTL_HOVER = RGB(58, 58, 58);
static const COLORREF C_BORDER    = RGB(52, 52, 52);
static const COLORREF C_TEXT      = RGB(232, 232, 232);
static const COLORREF C_LABEL     = RGB(176, 176, 176);
static const COLORREF C_DIM       = RGB(130, 130, 130);
static const COLORREF C_ACCENT    = RGB(90, 160, 240);
static const COLORREF C_ON        = RGB(196, 196, 196);   // light track when on, like the reference
static const COLORREF C_BTN       = RGB(52, 52, 52);
static const COLORREF C_BTN_HOVER = RGB(68, 68, 68);

// ------------------------------------------------------------------- state
struct Profile;

struct State
{
    // 帧生成
    int  fg_mode = 0;          // 0 fixed, 1 dynamic (--dyn-mfg)
    int  interp = 3;           // 2..6  -> --interp N-1
    int  out_mode = 0;         // 0 1:1, 1 scale 0.75, 2 den 2
    bool perf_no_throttle = false;
    // 捕获
    int  pool = 4;             // -> --pool
    int  latency = 1;          // -> --latency
    // XeFG 输入
    // MV source: 0 = off (--no-mv), 1 = the game MV, 2 = the game MV reversed (--mv-flip)
    int  mv_mode = 0;
    bool jitter_zero = true;
    bool proj_rz = true;
    int  fov_sel = 1;          // 55 / 58.7 / 60 / 65
    // Input interval the pacing model assumes: 0 = measured, 1 = 25 ms, 2 = 33 ms.
    // Feeds --input-ms; measured benefit at 3x+MV: slow 17->0, skip 19->13 (docs 5.18).
    int  ivl_sel = 0;
    bool no_depth = false;
    // 0 = let bridge choose (monitor rule / avoid the producer); 1+ = position in
    // the enumerated adapter list -> --gpu <index>.
    int  gpu_sel = 0;
    bool ui_none = true;
    int  rt_sel = 1;           // 0 ema, 1 median
    bool unlock_mfg = true;
    bool slack1 = false;
    // 渲染
    bool tearing = false;
    bool sdr = false;
    // A/B switch for the click-through problem: adds WS_EX_LAYERED + alpha 255 to
    // the overlay. Measured on both UE5 titles (影之刃零 / 伍长): with it ON the
    // in-game menus receive the clicks, with it OFF they do not - so this is now the
    // default. Turning it off (or turning 输入转发 on) falls back to the old path.
    bool overlay_layered = true;
    // A/B for "the menu ignores clicks": the overlay stops being click-through and
    // forwards the mouse to the game window itself.
    bool relay_input = false;
    // Single-line on-screen display drawn by bridge inside the game window
    // (fps / multiplier / input fps / gpu wait / game frame time / lock / drops).
    bool osd = true;
    // 启动
    int  target_mode = 0;      // 0 = the foreground window (LSFG habit), 1 = the selected one
    int  lock_fps = 2;         // hint only: 0 off, 30, 24, 40, 60
    int  hotkey = 0;   // index into g_hotkeys (0 = Ctrl+PageUp)
    std::wstring win_title;    // remembered target, matched by title on load
    HWND win_hwnd = nullptr;
};

struct Profile
{
    std::wstring name = L"默认";
    State        st;
};

struct WinEntry
{
    HWND         hwnd = nullptr;
    DWORD        pid = 0;
    std::wstring title;
    std::wstring cls;
    int          w = 0, h = 0;
};

struct Hotkey { const wchar_t* name; UINT mods; UINT vk; UINT gate; };
static const Hotkey g_hotkeys[] = {
    { L"Alt+PageUp",                     MOD_ALT,               VK_PRIOR, 0          },
    { L"Ctrl+PageUp",                    MOD_CONTROL,           VK_PRIOR, 0          },
    { L"Ctrl+Alt+F9",                    MOD_CONTROL | MOD_ALT, VK_F9,    0          },
    { L"Ctrl+Alt+F10",                   MOD_CONTROL | MOD_ALT, VK_F10,   0          },
    { L"Ctrl+Alt+F11",                   MOD_CONTROL | MOD_ALT, VK_F11,   0          },
    { L"Ctrl+Alt+F12",                   MOD_CONTROL | MOD_ALT, VK_F12,   0          },
    { L"Ctrl+Shift+F11",                 MOD_CONTROL | MOD_SHIFT, VK_F11, 0          },
    // Polled alternatives (no RegisterHotKey can express these; kept as fallbacks).
    { L"CapsLock 开 + PageUp",           0,                     VK_PRIOR, VK_CAPITAL },
    { L"PageUp + PageDown（同时按）",    0,                     VK_PRIOR, VK_NEXT    },
};
static const int kHotkeyCount = (int)(sizeof(g_hotkeys) / sizeof(g_hotkeys[0]));

// ------------------------------------------------------------------- globals
static std::vector<Profile>  g_profiles;
static int                   g_prof = 0;
static std::vector<WinEntry> g_wins;
static int                   g_win_sel = -1;

static HWND    g_hwnd = nullptr;
static HFONT   g_f_title = nullptr, g_f_card = nullptr, g_f_ui = nullptr, g_f_small = nullptr;
static bool    g_running = false;
static HANDLE  g_child = nullptr;
static DWORD   g_child_pid = 0;
// The window we handed to bridge (--hwnd). Kept so stop_bridge() can undo the
// WS_EX_TOPMOST promotion the bridge causes while it runs: assert_topmost() puts the
// game directly below its own topmost overlay, and Windows then promotes the game
// too - without this the game stays glued to the top of the desktop after FG stops.
static HWND    g_last_target = nullptr;
static HANDLE  g_stdout_rd = nullptr;

static std::atomic<int>    g_show{ 0 }, g_xn{ 0 }, g_skip{ 0 };
static std::atomic<double> g_gpu{ 0.0 }, g_game{ 0.0 }, g_present{ 0.0 };
static CRITICAL_SECTION    g_line_cs;
static std::wstring        g_last_line;

static UINT    g_hk_mods = MOD_CONTROL | MOD_ALT;
static UINT    g_hk_vk = VK_F11;
static bool    g_hk_custom = false;
static bool    g_capture_hotkey = false;
// polled hotkeys (everything RegisterHotKey cannot express)
static std::atomic<bool> g_hk_poll{ false };
static UINT    g_hk_poll_vk = 0;      // key whose press fires the toggle
static UINT    g_hk_poll_gate = 0;    // 0 = none, VK_CAPITAL = latched, else held key
static UINT g_refresh = 120;         // panel refresh, read at startup
static std::wstring g_msg;
static DWORD   g_msg_until = 0;

static int     g_scroll = 0;              // main area scroll offset (px)
static int     g_content_h = 0;           // measured at paint time
static int     g_hover_y = -1;            // hovered row (client coords)
static int     g_hover_x = -1;
static int     g_popup_id = 0;            // dropdown id whose list is open (0 = none)
static RECT    g_popup_rect{};            // where the closed control sits
static int     g_popup_hover = -1;
static bool    g_dragging = false;

static State& S() { return g_profiles[g_prof].st; }



// ------------------------------------------------------------------- ids
enum Id
{
    ID_NONE = 0,
    ID_FG_TYPE, ID_FG_MODE, ID_INTERP, ID_OUT, ID_PERF,
    ID_CAP_API, ID_POOL, ID_LAT,
    ID_NOMV, ID_JITZ, ID_PROJRZ, ID_FOV, ID_NODEPTH, ID_UI, ID_RT, ID_UNLOCK, ID_SLACK, ID_IVL,
    ID_MV,
    ID_TEAR, ID_SDR, ID_LAYERED, ID_RELAY, ID_OSD,
    ID_GPU, ID_GPUSEL, ID_DISPLAY,
    ID_TMODE, ID_TARGET, ID_HOTKEY, ID_LOCK, ID_CMD,
    ID_MIN, ID_CLOSE, ID_START, ID_ADD, ID_DEL,
};

enum RowKind { RK_INFO = 0, RK_DD, RK_SPIN, RK_TOG, RK_SLIDER, RK_CMDLINE };

struct RowDef { Id id; const wchar_t* label; RowKind kind; };
struct CardDef { const wchar_t* title; const RowDef* rows; int n; int col; };

static const RowDef kRowsFg[] = {
    { ID_FG_TYPE, L"类型",            RK_INFO },
    { ID_FG_MODE, L"模式",            RK_DD   },
    { ID_INTERP,  L"倍数",            RK_SPIN },
    { ID_OUT,     L"输出",            RK_DD   },
    { ID_MV,      L"运动矢量",        RK_DD   },
};
static const RowDef kRowsCap[] = {
    { ID_CAP_API, L"捕获 API",        RK_INFO },
    { ID_POOL,    L"帧池深度",        RK_SPIN },
    { ID_LAT,     L"最大帧延迟数",    RK_SPIN },
};
static const RowDef kRowsAdv[] = {
    { ID_FOV,     L"FOV（合成投影）", RK_DD },
    { ID_IVL,     L"输入间隔",        RK_DD },
};
static const RowDef kRowsDev[] = {
    { ID_GPUSEL,  L"帧生成用的 GPU",      RK_DD   },
    { ID_DISPLAY, L"输出显示器（只读）", RK_INFO },
};
static const RowDef kRowsLaunch[] = {
    { ID_HOTKEY,  L"开关热键（点击可改）", RK_INFO },
    { ID_LOCK,    L"建议锁帧",             RK_INFO },
    { ID_CMD,     L"启动命令",             RK_CMDLINE },
};

static const RowDef kRowsOverlay[] = {
    { ID_LAYERED, L"分层窗口 (A/B)", RK_TOG },
    { ID_RELAY,   L"输入转发 (A/B)", RK_TOG },
    { ID_OSD,     L"单行 OSD",        RK_TOG },
};

static const CardDef kCards[] = {
    { L"帧生成",       kRowsFg,     5, 0 },
    { L"捕获",         kRowsCap,    3, 1 },
    { L"启动",         kRowsLaunch, 3, 0 },
    { L"GPU / 显示器", kRowsDev,    2, 1 },
    { L"叠加层",       kRowsOverlay, 3, 1 },
    // FOV only feeds the synthetic projection, which the XeFG FG path ignores (docs 5.16),
    // but our own --fg-warp / --fg-of paths do use it. 输入间隔 feeds --input-ms.
    { L"高级",         kRowsAdv,    2, 0 },
};
static const int kCardCount = (int)(sizeof(kCards) / sizeof(kCards[0]));

// ------------------------------------------------------------------ options
static const wchar_t* kOptFgMode[] = { L"固定", L"动态 (实验)" };
static const wchar_t* kOptOut[]    = { L"1:1 原生", L"5K -> 4K (0.75x)", L"半分辨率" };
static const wchar_t* kOptMv[]     = { L"关闭", L"游戏 MV", L"游戏 MV (反向)", L"AMD 光流" };
static const wchar_t* kOptFov[]    = { L"55.0", L"58.7", L"60.0", L"65.0" };
static const wchar_t* kOptIvl[]    = { L"自动（倍数+1 vblank 余量）", L"16.7 ms (60 fps)", L"20 ms (50 fps)",
                                      L"25 ms (40 fps)", L"33 ms (30 fps)", L"41.7 ms (24 fps)" };
static const double   kIvlMs[]     = { 0.0, 16.7, 20.0, 25.0, 33.0, 41.7 };
static const wchar_t* kOptUi[]     = { L"关闭", L"自动" };
static const wchar_t* kOptRt[]     = { L"ema", L"median (16 帧)" };
static const wchar_t* kOptLock[]   = { L"关闭", L"30 fps", L"24 fps", L"40 fps", L"60 fps" };
static const wchar_t* kOptTMode[]  = { L"前景窗口 (鼠标点进游戏再按热键)", L"上一次启动的窗口" };
static const double   kLockFps[]   = { 0.0, 30.0, 24.0, 40.0, 60.0 };

static std::wstring hotkey_name();          // defined with the input handling

static int opt_count(Id id)
{
    switch (id) {
    case ID_FG_MODE: return 2;
    case ID_OUT:     return 3;
    case ID_MV:      return 4;
    case ID_FOV:     return 4;
    case ID_IVL:     return 6;
    case ID_UI:      return 2;
    case ID_RT:      return 2;
    case ID_HOTKEY:  return kHotkeyCount;
    case ID_LOCK:    return 5;
    case ID_TARGET:  return (int)g_wins.size();
    case ID_TMODE:   return 2;
    case ID_GPUSEL: return 1 + (int)g_adapters.size();
    default:         return 0;
    }
}

static const wchar_t* opt_text(Id id, int i)
{
    switch (id) {
    case ID_FG_MODE: return kOptFgMode[i % 2];
    case ID_OUT:     return kOptOut[i % 3];
    case ID_MV:      return kOptMv[i % 4];
    case ID_FOV:     return kOptFov[i % 4];
    case ID_IVL:     return kOptIvl[i % 6];
    case ID_UI:      return kOptUi[i % 2];
    case ID_RT:      return kOptRt[i % 2];
    case ID_HOTKEY:  return g_hk_custom ? L"自定义" : g_hotkeys[i % kHotkeyCount].name;
    case ID_LOCK:    return kOptLock[i % 5];
    case ID_TMODE:   return kOptTMode[i % 2];
    case ID_GPUSEL: {
        if (i <= 0) { return L"自动（窗口所在显示器 / 避开游戏卡）"; }
        const size_t k = (size_t)(i - 1);
        return (k < g_adapters.size()) ? g_adapters[k].name.c_str() : L"(无)";
    }
    case ID_TARGET: {
        static std::wstring s;
        if (i >= 0 && i < (int)g_wins.size()) {
            wchar_t b[400]{};
            _snwprintf_s(b, _TRUNCATE, L"%.40s   %dx%d", g_wins[i].title.c_str(),
                         g_wins[i].w, g_wins[i].h);
            s = b;
            return s.c_str();
        }
        return L"(无窗口)";
    }
    default: return L"";
    }
}

static int opt_get(Id id)
{
    switch (id) {
    case ID_FG_MODE: return S().fg_mode;
    case ID_OUT:     return S().out_mode;
    case ID_MV:      return S().mv_mode;
    case ID_FOV:     return S().fov_sel;
    case ID_IVL:     return S().ivl_sel;
    case ID_UI:      return S().ui_none ? 0 : 1;
    case ID_RT:      return S().rt_sel;
    case ID_HOTKEY:  return S().hotkey;
    case ID_LOCK:    return S().lock_fps;
    case ID_TARGET:  return g_win_sel < 0 ? 0 : g_win_sel;
    case ID_TMODE:   return S().target_mode;
    case ID_GPUSEL: return (S().gpu_sel > 0 && S().gpu_sel <= (int)g_adapters.size())
                            ? S().gpu_sel : 0;
    default: return 0;
    }
}


// Caption shown on the right of a row.
static std::wstring row_value(Id id)
{
    wchar_t buf[128]{};
    switch (id) {
    case ID_FG_TYPE: return L"XeFG 1.3.1 (libxess_fg)";
    case ID_CAP_API: return L"WGC";
    case ID_GPU:     return L"AMD Radeon RX 6750 GRE";
    case ID_DISPLAY: {
        DEVMODEW dm{}; dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
            _snwprintf_s(buf, _TRUNCATE, L"%lux%lu @ %lu Hz",
                         (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight,
                         (unsigned long)dm.dmDisplayFrequency);
            return buf;
        }
        return L"(未知)";
    }
    case ID_INTERP: _snwprintf_s(buf, _TRUNCATE, L"%dx", S().interp); return buf;
    case ID_POOL:   _snwprintf_s(buf, _TRUNCATE, L"%d", S().pool);    return buf;
    case ID_LAT:    _snwprintf_s(buf, _TRUNCATE, L"%d", S().latency); return buf;
    case ID_HOTKEY: return g_capture_hotkey ? std::wstring(L"按下新组合键...") : hotkey_name();
    case ID_LOCK: {
        // Read-only advice: lock the game where refresh / multiplier lands exactly.
        const double lf = (S().interp > 0) ? (g_refresh / (double)S().interp) : 0.0;
        _snwprintf_s(buf, _TRUNCATE, L"%.0f fps  ->  %d x %.0f = %.0f fps 输出",
                     lf, S().interp, lf, lf * S().interp);
        return buf;
    }
    case ID_TARGET:
        if (g_win_sel >= 0 && g_win_sel < (int)g_wins.size()) {
            const WinEntry& w = g_wins[g_win_sel];
            _snwprintf_s(buf, _TRUNCATE, L"%dx%d  pid %lu", w.w, w.h,
                         (unsigned long)w.pid);
            return buf;
        }
        return L"(点击选择)";
    default: return L"";
    }
}

// ------------------------------------------------------------------ helpers
static std::wstring exe_dir();   // defined below

// Tiny append-only log next to the exe. The toggle is hard to debug otherwise: the
// interesting events are "the hotkey fired" and what the stop ladder actually did.
static void uilog(const char* fmt, ...)
{
    char msg[512]{};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
    va_end(ap);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[600]{};
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%02d:%02d:%02d.%03d  %s\r\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);

    const std::wstring path = exe_dir() + L"\\mgpu_ui.log";
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { return; }
    DWORD wrote = 0;
    WriteFile(f, line, static_cast<DWORD>(strlen(line)), &wrote, nullptr);
    CloseHandle(f);
}

// Kill any bridge.exe we may have lost track of (stale handle, crashed UI state, ...).
static int kill_stray_bridge()
{
    int killed = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { return 0; }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"bridge.exe") == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
                if (h) {
                    TerminateProcess(h, 0);
                    WaitForSingleObject(h, 1500);
                    CloseHandle(h);
                    ++killed;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return killed;
}

static std::wstring exe_dir()
{
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    const size_t slash = s.find_last_of(L'\\');
    return (slash == std::wstring::npos) ? std::wstring(L".") : s.substr(0, slash);
}

static std::wstring ini_path() { return exe_dir() + L"\\mgpu_ui.ini"; }

static void ini_w(const std::wstring& sec, const wchar_t* key, int v)
{
    wchar_t buf[32]{};
    _snwprintf_s(buf, _TRUNCATE, L"%d", v);
    WritePrivateProfileStringW(sec.c_str(), key, buf, ini_path().c_str());
}

static void ini_ws(const std::wstring& sec, const wchar_t* key, const std::wstring& v)
{
    WritePrivateProfileStringW(sec.c_str(), key, v.c_str(), ini_path().c_str());
}

static int ini_r(const std::wstring& sec, const wchar_t* key, int def)
{
    return (int)GetPrivateProfileIntW(sec.c_str(), key, def, ini_path().c_str());
}

static std::wstring ini_rs(const std::wstring& sec, const wchar_t* key, const wchar_t* def)
{
    wchar_t buf[512]{};
    GetPrivateProfileStringW(sec.c_str(), key, def, buf, 512, ini_path().c_str());
    return buf;
}

static void set_msg(const wchar_t* m)
{
    g_msg = m;
    g_msg_until = GetTickCount() + 4000;
    if (g_hwnd) { InvalidateRect(g_hwnd, nullptr, FALSE); }
}


// ------------------------------------------------------------------ profiles
static std::wstring prof_sec(int i)
{
    wchar_t b[32]{};
    _snwprintf_s(b, _TRUNCATE, L"p%d", i);
    return b;
}

static void save_profile(int i)
{
    const std::wstring sec = prof_sec(i);
    const State& s = g_profiles[i].st;
    ini_ws(sec, L"name", g_profiles[i].name);
    ini_w(sec, L"fg_mode", s.fg_mode);
    ini_w(sec, L"interp", s.interp);
    ini_w(sec, L"out_mode", s.out_mode);
    ini_w(sec, L"pool", s.pool);
    ini_w(sec, L"latency", s.latency);
    ini_w(sec, L"fov", s.fov_sel);
    ini_w(sec, L"ivl", s.ivl_sel);
    ini_w(sec, L"rt", s.rt_sel);
    ini_w(sec, L"lock", s.lock_fps);
    ini_w(sec, L"tmode", s.target_mode);
    ini_w(sec, L"gpu_sel", s.gpu_sel);
    ini_w(sec, L"hotkey3", s.hotkey);
    ini_w(sec, L"mv_mode", s.mv_mode);
    ini_w(sec, L"no_mv", s.mv_mode == 0 ? 1 : 0);   // legacy key
    ini_w(sec, L"jitter_zero", s.jitter_zero ? 1 : 0);
    ini_w(sec, L"proj_rz", s.proj_rz ? 1 : 0);
    ini_w(sec, L"no_depth", s.no_depth ? 1 : 0);
    ini_w(sec, L"ui_none", s.ui_none ? 1 : 0);
    ini_w(sec, L"unlock", s.unlock_mfg ? 1 : 0);
    ini_w(sec, L"slack1", s.slack1 ? 1 : 0);
    ini_w(sec, L"tearing", s.tearing ? 1 : 0);
    ini_w(sec, L"sdr", s.sdr ? 1 : 0);
    ini_w(sec, L"overlay_layered", s.overlay_layered ? 1 : 0);
    ini_w(sec, L"relay_input", s.relay_input ? 1 : 0);
    ini_w(sec, L"osd", s.osd ? 1 : 0);
    ini_w(sec, L"perf", s.perf_no_throttle ? 1 : 0);
    ini_ws(sec, L"win_title", s.win_title);
}

static void load_profile(int i)
{
    const std::wstring sec = prof_sec(i);
    Profile& p = g_profiles[i];
    State& s = p.st;
    p.name        = ini_rs(sec, L"name", L"默认");
    s.fg_mode     = ini_r(sec, L"fg_mode", s.fg_mode);
    s.interp      = ini_r(sec, L"interp", s.interp);
    s.out_mode    = ini_r(sec, L"out_mode", s.out_mode);
    s.pool        = ini_r(sec, L"pool", s.pool);
    s.latency     = ini_r(sec, L"latency", s.latency);
    s.fov_sel     = ini_r(sec, L"fov", s.fov_sel);
    s.ivl_sel     = ini_r(sec, L"ivl", s.ivl_sel);
    if (s.ivl_sel < 0 || s.ivl_sel > 5) { s.ivl_sel = 0; }
    s.rt_sel      = ini_r(sec, L"rt", s.rt_sel);
    s.lock_fps    = ini_r(sec, L"lock", s.lock_fps);
    s.target_mode = ini_r(sec, L"tmode", s.target_mode);
    s.gpu_sel     = ini_r(sec, L"gpu_sel", s.gpu_sel);
    s.hotkey      = ini_r(sec, L"hotkey3", s.hotkey);
    if (s.hotkey < 0 || s.hotkey >= kHotkeyCount) { s.hotkey = 0; }
    s.mv_mode     = ini_r(sec, L"mv_mode", ini_r(sec, L"no_mv", 1) != 0 ? 0 : 1);
    if (s.mv_mode < 0 || s.mv_mode > 2) { s.mv_mode = 0; }
    s.jitter_zero = ini_r(sec, L"jitter_zero", s.jitter_zero ? 1 : 0) != 0;
    s.proj_rz     = ini_r(sec, L"proj_rz", s.proj_rz ? 1 : 0) != 0;
    s.no_depth    = ini_r(sec, L"no_depth", s.no_depth ? 1 : 0) != 0;
    s.ui_none     = ini_r(sec, L"ui_none", s.ui_none ? 1 : 0) != 0;
    s.unlock_mfg  = ini_r(sec, L"unlock", s.unlock_mfg ? 1 : 0) != 0;
    s.slack1      = ini_r(sec, L"slack1", s.slack1 ? 1 : 0) != 0;
    s.tearing     = ini_r(sec, L"tearing", s.tearing ? 1 : 0) != 0;
    s.sdr         = ini_r(sec, L"sdr", s.sdr ? 1 : 0) != 0;
    s.overlay_layered = ini_r(sec, L"overlay_layered", s.overlay_layered ? 1 : 0) != 0;
    s.relay_input = ini_r(sec, L"relay_input", s.relay_input ? 1 : 0) != 0;
    s.osd = ini_r(sec, L"osd", s.osd ? 1 : 0) != 0;
    s.perf_no_throttle = ini_r(sec, L"perf", s.perf_no_throttle ? 1 : 0) != 0;
    s.win_title   = ini_rs(sec, L"win_title", L"");
    if (s.interp < 2) { s.interp = 2; }
    if (s.interp > 6) { s.interp = 6; }
    if (s.pool < 2) { s.pool = 2; }
    if (s.pool > 6) { s.pool = 6; }
    if (s.latency < 1) { s.latency = 1; }
    if (s.latency > 3) { s.latency = 3; }
}

static void save_all()
{
    ini_w(std::wstring(L"ui"), L"count", (int)g_profiles.size());
    ini_w(std::wstring(L"ui"), L"active", g_prof);
    ini_w(std::wstring(L"ui"), L"hk_custom", g_hk_custom ? 1 : 0);
    ini_w(std::wstring(L"ui"), L"hk_mods", (int)g_hk_mods);
    ini_w(std::wstring(L"ui"), L"hk_vk", (int)g_hk_vk);
    for (int i = 0; i < (int)g_profiles.size(); ++i) { save_profile(i); }
}

static void load_all()
{
    g_hk_custom = ini_r(L"ui", L"hk_custom", 0) != 0;
    g_hk_mods = (UINT)ini_r(L"ui", L"hk_mods", MOD_CONTROL | MOD_ALT);
    g_hk_vk = (UINT)ini_r(L"ui", L"hk_vk", VK_F11);

    int n = ini_r(L"ui", L"count", 0);
    if (n < 1 || n > 16) { n = 1; }
    g_profiles.clear();
    for (int i = 0; i < n; ++i) { g_profiles.push_back(Profile{}); }
    for (int i = 0; i < n; ++i) { load_profile(i); }
    g_prof = ini_r(L"ui", L"active", 0);
    if (g_prof < 0 || g_prof >= (int)g_profiles.size()) { g_prof = 0; }
}

// ------------------------------------------------------ window enumeration
static BOOL CALLBACK enum_win_proc(HWND h, LPARAM lp)
{
    auto* out = reinterpret_cast<std::vector<WinEntry>*>(lp);
    if (h == g_hwnd) { return TRUE; }
    if (!IsWindowVisible(h)) { return TRUE; }
    if (GetWindow(h, GW_OWNER) != nullptr) { return TRUE; }
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) { return TRUE; }
    RECT cr{};
    if (!GetClientRect(h, &cr)) { return TRUE; }
    const int w = cr.right - cr.left, hh = cr.bottom - cr.top;
    // Games are big: a 720p client is the floor, and UWP/system shells are skipped so
    // the list does not fill up with 设置 / 照片 / ApplicationFrameHost windows.
    if (w < 1280 || hh < 720) { return TRUE; }
    wchar_t title[256]{};
    GetWindowTextW(h, title, 256);
    wchar_t cls[128]{};
    GetClassNameW(h, cls, 128);
    if (cls[0] == 0) { return TRUE; }
    if (wcscmp(cls, L"ApplicationFrameWindow") == 0 ||
        wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0 ||
        wcscmp(cls, L"ConsoleWindowClass") == 0 || wcscmp(cls, L"Progman") == 0 ||
        wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"Shell_TrayWnd") == 0) {
        return TRUE;
    }
    WinEntry e{};
    e.hwnd = h;
    e.w = w;
    e.h = hh;
    e.title = title;
    e.cls = cls;
    GetWindowThreadProcessId(h, &e.pid);
    out->push_back(e);
    return TRUE;
}

static void rescan_windows()
{
    g_wins.clear();
    EnumWindows(enum_win_proc, reinterpret_cast<LPARAM>(&g_wins));
    for (size_t i = 1; i < g_wins.size(); ++i) {          // biggest first
        for (size_t j = i; j > 0 &&
             (g_wins[j - 1].w * g_wins[j - 1].h) < (g_wins[j].w * g_wins[j].h); --j) {
            std::swap(g_wins[j - 1], g_wins[j]);
        }
    }
    // Re-bind the remembered window of the active profile by title.
    g_win_sel = g_wins.empty() ? -1 : 0;
    const std::wstring want = S().win_title;
    if (!want.empty()) {
        for (size_t i = 0; i < g_wins.size(); ++i) {
            if (g_wins[i].title == want) { g_win_sel = (int)i; break; }
        }
    }
    if (g_win_sel >= 0) { S().win_title = g_wins[g_win_sel].title; }
}


// ------------------------------------------------------------ bridge process
static std::wstring build_cmdline(HWND target, bool display)
{
    std::wstring cmd = display ? std::wstring(L"bridge.exe")
                               : (L"\"" + exe_dir() + L"\\bridge.exe\"");
    if (target) {
        wchar_t b[64]{};
        _snwprintf_s(b, _TRUNCATE, L" --hwnd 0x%llX",
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target)));
        cmd += b;
    } else if (display) {
        cmd += L" --hwnd {当前前台窗口}";
    }

    static const wchar_t* kOut[] = { L" --den 1", L" --scale 0.75", L" --den 2" };
    cmd += kOut[(S().out_mode >= 0 && S().out_mode <= 2) ? S().out_mode : 0];

    wchar_t b[256]{};
    _snwprintf_s(b, _TRUNCATE, L" --pool %d --latency %d --interp %d",
                 S().pool, S().latency, S().interp - 1);
    cmd += b;
    if (S().fg_mode == 1) { cmd += L" --dyn-mfg"; }
    // Which GPU runs the frame generation (A card rendering + N card output, or the
    // other way round). 0 = let bridge decide from the window monitor / producer LUID.
    if (S().gpu_sel > 0 && S().gpu_sel <= (int)g_adapters.size()) {
        wchar_t g[64]{};
        _snwprintf_s(g, _TRUNCATE, L" --gpu %d",
                     g_adapters[(size_t)S().gpu_sel - 1].index);
        cmd += g;
    }

    // Fixed part: reversed-Z projection, no UI texture, first 6x unlock, median
    // frame-time feed. jitter is NOT in the fixed part any more - see below.
    if (S().overlay_layered) { cmd += L" --overlay-layered"; }
    if (S().relay_input) { cmd += L" --relay-input"; }
    if (!S().osd) { cmd += L" --no-osd"; }
    cmd += L" --unlock-mfg --fg-ui none --fg-proj-rz --fg-rt-mode median";
    // The FOV dropdown used to be decorative: the cmdline hard-coded 58.7. Emit the
    // selected value instead (`--fg-fov` only feeds the synthetic projection, which is
    // used when the input source has no camera - FSR exports a real one and wins).
    {
        static const wchar_t* kFov[] = { L"55.0", L"58.7", L"60.0", L"65.0" };
        const int sel = (S().fov_sel >= 0 && S().fov_sel < 4) ? S().fov_sel : 1;
        cmd += L" --fg-fov ";
        cmd += kFov[sel];
    }
    if (S().ivl_sel > 0 && S().ivl_sel < 6) {
        wchar_t iv[32];
        _snwprintf_s(iv, _TRUNCATE, L" --input-ms %.1f", kIvlMs[S().ivl_sel]);
        cmd += iv;
    } else if (S().ivl_sel == 0 && g_refresh > 1.0) {
        // Smart default: one whole vblank of slack over the theoretical (interp+1) vblanks.
        // Measured 2026-11-08: the exact theoretical value makes XeFG drop generated frames
        // whenever throttle+present overshoots by a hair; +1 vblank removes the drops with
        // no latency cost (docs FINAL_CONFIG 5.18.1).
        const double vblank = 1000.0 / g_refresh;
        const double ms = (S().interp + 2) * vblank;
        wchar_t iv[32];
        _snwprintf_s(iv, _TRUNCATE, L" --input-ms %.1f", ms);
        cmd += iv;
    }
    // MV: 0 = off, 1 = the game vectors (bridge asks the add-on for the MV stream;
    // the add-on has to run with SkipMv=0), 2 = reversed (--mv-flip).
    if (S().mv_mode == 0) {
        cmd += L" --no-mv";
    } else if (S().mv_mode == 2) {
        cmd += L" --mv-flip";   // game vectors, reversed
    } else if (S().mv_mode == 3) {
        // AMD optical flow from the captured colour (FidelityFX). Uses only the colour plus
        // the game's native depth as an edge-guided upsample weight - no reprojection
        // matrix, no synthetic camera (docs FINAL_CONFIG 5.19).
        cmd += L" --fg-of";
    }
    // --fg-jitter-zero is deprecated and no longer emitted at all: the game's jitter is
    // always passed. Its motion vectors are computed on the jittered render, so XeFG needs
    // the real jitter to align history - measured 2026-11-08 (PBZ, 2x@40, real MV): that
    // removed both the flicker and most of the model-edge ghosting jitter=0 showed.
    return cmd;
}

static std::wstring display_cmdline()
{
    HWND t = nullptr;
    if (S().target_mode == 1 && S().win_hwnd && IsWindow(S().win_hwnd)) { t = S().win_hwnd; }
    return build_cmdline(t, true);
}

static void parse_line(const std::string& s)
{
    auto num = [&](const char* key) -> double {
        const char* p = strstr(s.c_str(), key);
        return p ? atof(p + strlen(key)) : 0.0;
    };
    if (strstr(s.c_str(), "show=")) { g_show = (int)num("show="); }
    if (strstr(s.c_str(), "breakdown:")) {
        g_gpu = num("gpu=");
        g_present = num("present=");
        g_game = num("game=");
        g_xn = (int)(num("xN=") * 100.0);
        g_skip = (int)num("skip=");
    }
    if (strstr(s.c_str(), "dyn-mfg:") || strstr(s.c_str(), "SetNumInterpolatedFrames(") ||
        strstr(s.c_str(), "WARNING") || strstr(s.c_str(), "ERR")) {
        wchar_t w[512]{};
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w, 512);
        EnterCriticalSection(&g_line_cs);
        g_last_line = w;
        LeaveCriticalSection(&g_line_cs);
    }
}

static DWORD WINAPI reader_thread(LPVOID)
{
    char buf[4096];
    std::string acc;

    // Bridge logs to stdout, which is this pipe, so the diagnostics never reach a
    // file of their own. Keep a copy next to the exe.
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[64]{};
    _snwprintf_s(name, _TRUNCATE, L"\\bridge_ui_%02u%02u%02u.txt",
                 st.wHour, st.wMinute, st.wSecond);
    const std::wstring log_path = exe_dir() + name;
    uilog("bridge stdout -> %ls", name + 1);
    HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(g_stdout_rd, buf, sizeof(buf) - 1, &got, nullptr) || got == 0) { break; }
        if (log != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            WriteFile(log, buf, got, &wrote, nullptr);
        }
        for (DWORD i = 0; i < got; ++i) {
            if (buf[i] == '\n') { parse_line(acc); acc.clear(); }
            else if (buf[i] != '\r') { acc.push_back(buf[i]); }
        }
        if (g_hwnd) { PostMessageW(g_hwnd, WM_APP + 1, 0, 0); }
    }
    if (log != INVALID_HANDLE_VALUE) { CloseHandle(log); }
    return 0;
}

// bridge clears the game's WS_EX_TOPMOST itself on every exit path (hard_exit); this
// is the safety net for the paths it cannot cover - the TerminateProcess fallback
// above, a crash, or the user killing bridge.exe from Task Manager.
static void clear_target_topmost()
{
    HWND h = g_last_target;
    if (!h || !IsWindow(h)) { return; }
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOPMOST) {
        SetWindowLongPtrW(h, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_TOPMOST));
        uilog("stop: cleared WS_EX_TOPMOST on target %p", (void*)h);
    }
    SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static void stop_bridge()
{
    DWORD pid = g_child_pid;
    if (!pid && g_child) { pid = GetProcessId(g_child); }
    uilog("stop: running=%d pid=%lu handle=%p", (int)g_running, (unsigned long)pid,
          (void*)g_child);

    if (pid) {
        // 1) polite: WM_CLOSE to every top-level window of the bridge process
        struct Ctx { DWORD pid; int n; } ctx{ pid, 0 };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD p = 0;
            GetWindowThreadProcessId(h, &p);
            if (p == c->pid) { ++c->n; PostMessageW(h, WM_CLOSE, 0, 0); }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        uilog("stop: WM_CLOSE -> %d window(s)", ctx.n);

        bool gone = (g_child && WaitForSingleObject(g_child, 500) == WAIT_OBJECT_0);
        uilog("stop: WM_CLOSE handled, exited=%d", (int)gone);

        // 2) the overlay ignores WM_CLOSE when the game has focus: quit its message loop
        if (!gone) {
            // PostThreadMessage needs a THREAD id (a pid silently fails).
            struct TCtx { DWORD pid; DWORD tid; } tctx{ pid, 0 };
            EnumWindows([](HWND h, LPARAM lp) -> BOOL {
                auto* c = reinterpret_cast<TCtx*>(lp);
                DWORD p = 0;
                const DWORD t = GetWindowThreadProcessId(h, &p);
                if (p == c->pid) { c->tid = t; return FALSE; }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&tctx));
            if (tctx.tid) { PostThreadMessageW(tctx.tid, WM_QUIT, 0, 0); }
            gone = (g_child && WaitForSingleObject(g_child, 300) == WAIT_OBJECT_0);
            uilog("stop: WM_QUIT posted, exited=%d", (int)gone);
        }

        // 3) last resort
        if (!gone) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
            if (h) {
                TerminateProcess(h, 0);
                WaitForSingleObject(h, 1500);
                CloseHandle(h);
                uilog("stop: TerminateProcess");
            } else {
                uilog("stop: OpenProcess failed (err=%lu)", (unsigned long)GetLastError());
            }
        }
    } else {
        // We lost track of the process (stale state): kill whatever bridge.exe is left.
        const int k = kill_stray_bridge();
        uilog("stop: no pid, killed %d stray bridge.exe", k);
    }

    if (g_stdout_rd) { CloseHandle(g_stdout_rd); g_stdout_rd = nullptr; }
    if (g_child) { CloseHandle(g_child); g_child = nullptr; }
    clear_target_topmost();
    g_child_pid = 0;
    g_running = false;
    g_show = 0; g_xn = 0; g_skip = 0; g_gpu = 0; g_game = 0; g_present = 0;
}

// A target only makes sense if it is the process that created the shared mailbox,
// i.e. the one with the game_capture add-on loaded. Without this check, pressing the
// hotkey over a browser starts bridge, which then waits forever for the mapping -
// "selecting a browser does nothing".
static bool target_is_producer(HWND wnd, DWORD* out_pid, DWORD* out_producer,
                               unsigned* out_version)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(wnd, &pid);
    if (out_pid) { *out_pid = pid; }
    if (out_producer) { *out_producer = 0; }
    if (out_version) { *out_version = 0; }

    HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\mgpu_mailbox_v1");
    if (!m) { return false; }
    auto* v = static_cast<const mgpu::SharedControl*>(
        MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(mgpu::SharedControl)));
    bool ok = false;
    if (v) {
        if (out_producer) { *out_producer = v->producer_pid; }
        if (out_version) { *out_version = v->version; }
        ok = (v->magic == mgpu::kSharedMagic) && (v->version == mgpu::kSharedVersion) &&
             (v->producer_pid == pid);
        UnmapViewOfFile(v);
    }
    CloseHandle(m);
    return ok;
}

static void start_bridge()
{
    // Which window do we capture? In "foreground" mode it is whatever has focus when
    // the hotkey fires - Lossless Scaling's own muscle memory - which also means the
    // user picks the window with the mouse simply by clicking into it. The list
    // selection is the fallback (and the only option when the UI itself has focus).
    HWND target = nullptr;
    if (S().target_mode == 0) {
        const HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) { GetWindowThreadProcessId(fg, &pid); }
        if (fg && pid != GetCurrentProcessId()) { target = fg; }
    } else if (S().win_hwnd && IsWindow(S().win_hwnd)) {
        target = S().win_hwnd;
    }
    if (!target && g_win_sel >= 0 && g_win_sel < (int)g_wins.size()) {
        target = g_wins[g_win_sel].hwnd;
    }
    if (!target) {
        set_msg(L"没有目标窗口：把鼠标点到游戏里再按热键");
        uilog("start: no target window (target_mode=%d, win_sel=%d)", (int)S().target_mode,
              g_win_sel);
        return;
    }

    DWORD tpid = 0, ppid = 0;
    unsigned pver = 0;
    if (!target_is_producer(target, &tpid, &ppid, &pver)) {
        uilog("start: target %p is not the producer (pid=%lu producer_pid=%lu version=%u "
              "expect=%u)", (void*)target, (unsigned long)tpid, (unsigned long)ppid,
              pver, mgpu::kSharedVersion);
        wchar_t m[300]{};
        if (ppid != 0 && pver != 0 && pver != mgpu::kSharedVersion) {
            _snwprintf_s(m, _TRUNCATE,
                         L"协议版本不符：add-on 说 v%u，本 UI 说 v%u —— 三件套要一起重装",
                         pver, mgpu::kSharedVersion);
        } else if (ppid != 0) {
            _snwprintf_s(m, _TRUNCATE,
                         L"这个窗口 (pid %lu) 不是游戏进程；共享内存由 pid %lu 创建 —— 请点进游戏再按热键",
                         (unsigned long)tpid, (unsigned long)ppid);
        } else {
            _snwprintf_s(m, _TRUNCATE,
                         L"目标进程 (pid %lu) 里没有 game_capture add-on，无法抓帧",
                         (unsigned long)tpid);
        }
        set_msg(m);
        return;
    }
    const std::wstring bridge = exe_dir() + L"\\bridge.exe";
    if (GetFileAttributesW(bridge.c_str()) == INVALID_FILE_ATTRIBUTES) {
        set_msg(L"mgpu_ui.exe 旁边找不到 bridge.exe");
        uilog("start: bridge.exe missing next to %ls", exe_dir().c_str());
        return;
    }
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        set_msg(L"CreatePipe 失败");
        uilog("start: CreatePipe FAILED err=%lu", GetLastError());
        return;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = build_cmdline(target, false);
    std::vector<wchar_t> mc(cmd.begin(), cmd.end());
    mc.push_back(0);

    const BOOL ok = CreateProcessW(bridge.c_str(), mc.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, exe_dir().c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        set_msg(L"CreateProcess 失败");
        uilog("start: CreateProcess FAILED err=%lu cmd=%ls", GetLastError(), cmd.c_str());
        return;
    }

    CloseHandle(pi.hThread);
    g_child = pi.hProcess;
    g_child_pid = pi.dwProcessId;
    g_last_target = target;
    g_stdout_rd = rd;
    g_running = true;
    CloseHandle(CreateThread(nullptr, 0, reader_thread, nullptr, 0, nullptr));
    S().win_hwnd = target;
    {
        wchar_t t[256]{};
        GetWindowTextW(target, t, 256);
        S().win_title = t;
    }
    save_all();
    uilog("start: pid=%lu cmd=%ls", (unsigned long)g_child_pid, cmd.c_str());
    // Point out the one combination that pays for MV without seeing the benefit: at 3x the
    // generated frames come from the patched 2x model applied twice, and that model-edge
    // ghosting dominates whatever the motion vectors fix (measured 2026-11-08).
    if ((S().mv_mode == 1 || S().mv_mode == 2) && S().interp >= 2) {
        // Only the GAME's vectors flicker at 3x (jitter-relative, misaligned with the
        // resolved colour). --fg-of estimates them from that colour, so it is fine at 3x
        // and is in fact the recommended 3x choice (docs 5.19).
        set_msg(L"已启动 · 提示：3x + 游戏 MV 会闪烁，建议改 2x 或把运动矢量换成「AMD 光流」");
    } else {
        set_msg(L"bridge 已启动");
    }
}

static void toggle_bridge()
{
    uilog("hotkey/button: running=%d child_pid=%lu", (int)g_running, (unsigned long)g_child_pid);
    if (g_running) { stop_bridge(); set_msg(L"bridge 已停止"); }
    else           { start_bridge(); }
    if (g_hwnd) { InvalidateRect(g_hwnd, nullptr, FALSE); }
}


// ------------------------------------------------------------------- layout
static const int kWinW = 1200, kWinH = 620;
static const int kTitleH = 38;
static const int kSideW = 250;
static const int kMargin = 22;
static const int kGap = 16;
static const int kCardHead = 44;
static const int kRowH = 33;
static const int kCardTail = 12;
static const int kBottomH = 34;

struct Item
{
    Id      id = ID_NONE;
    RowKind kind = RK_INFO;
    RECT    rc{};
    bool    is_row = false;
    bool    on = false;
};

static int row_h(RowKind k) { return (k == RK_CMDLINE) ? 106 : kRowH; }

static int ctl_w(RowKind k)
{
    switch (k) {
    case RK_DD:   return 210;
    case RK_SPIN: return 116;
    case RK_TOG:  return 46;
    default:      return 0;
    }
}

static void layout_items(std::vector<Item>& out, int* content_h)
{
    out.clear();
    const int main_x = kSideW;
    const int content_w = kWinW - kSideW - 2 * kMargin;
    const int colw = (content_w - kGap) / 2;
    int y[2] = { kTitleH + 66, kTitleH + 66 };

    for (int ci = 0; ci < kCardCount; ++ci) {
        const CardDef& cd = kCards[ci];
        const int col = cd.col;
        const int x = main_x + kMargin + col * (colw + kGap);
        const int top = y[col];
        int body = 0;
        for (int r = 0; r < cd.n; ++r) { body += row_h(cd.rows[r].kind); }
        const int bottom = top + kCardHead + body + kCardTail;

        Item head{};
        head.rc = RECT{ x, top, x + colw, bottom };
        head.id = (Id)(1000 + ci);
        out.push_back(head);

        int ry = top + kCardHead;
        for (int r = 0; r < cd.n; ++r) {
            Item it{};
            it.id = cd.rows[r].id;
            it.kind = cd.rows[r].kind;
            it.is_row = true;
            const int hh = row_h(it.kind);
            it.rc = RECT{ x + 16, ry, x + colw - 16, ry + hh };
            ry += hh;
            it.on = (it.id == ID_JITZ) ? S().jitter_zero
                  : (it.id == ID_PROJRZ) ? S().proj_rz
                  : (it.id == ID_NODEPTH) ? S().no_depth
                  : (it.id == ID_UNLOCK) ? S().unlock_mfg
                  : (it.id == ID_SLACK) ? S().slack1
                  : (it.id == ID_TEAR) ? S().tearing
                  : (it.id == ID_SDR) ? S().sdr
                  : (it.id == ID_LAYERED) ? S().overlay_layered
                  : (it.id == ID_RELAY) ? S().relay_input
                  : (it.id == ID_OSD) ? S().osd
                  : (it.id == ID_PERF) ? S().perf_no_throttle
                  : false;
            out.push_back(it);
        }
        y[col] = bottom + kGap;
    }
    *content_h = (y[0] > y[1] ? y[0] : y[1]) - (kTitleH + 66) + kMargin;
}

// ------------------------------------------------------------------ drawing
static void fill_round(HDC dc, const RECT& r, int rad, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void stroke_round(HDC dc, const RECT& r, int rad, COLORREF fill, COLORREF border)
{
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void txt(HDC dc, HFONT f, const RECT& r, const std::wstring& s, COLORREF c, UINT fmt)
{
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    // Without TRANSPARENT every DrawText call paints its bounding box with the DC's
    // background colour (white by default) - the text then sits light-on-white and the
    // whole UI looks like it has no labels at all.
    SetBkMode(dc, TRANSPARENT);
    RECT rc = r;
    DrawTextW(dc, s.c_str(), -1, &rc, fmt | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void draw_chevron(HDC dc, int cx, int cy, int dir, COLORREF c)
{
    POINT p[3] = { { cx - 5, cy - 2 * dir }, { cx + 5, cy - 2 * dir }, { cx, cy + 3 * dir } };
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    Polygon(dc, p, 3);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}


static void draw_toggle(HDC dc, const RECT& r, bool on, bool hover)
{
    RECT t = r;
    t.left = r.right - 40;
    t.right = r.right;
    t.top = r.top + 8;
    t.bottom = r.bottom - 8;
    fill_round(dc, t, (t.bottom - t.top), on ? C_ON : (hover ? RGB(92, 92, 92) : C_TRACK_OFF));
    const int d = t.bottom - t.top - 4;
    RECT knob = t;
    knob.top += 2;
    knob.bottom -= 2;
    if (on) { knob.right -= 2; knob.left = knob.right - d; }
    else    { knob.left = t.left + 2; knob.right = knob.left + d; }
    fill_round(dc, knob, d, RGB(232, 232, 232));
}

static void draw_spin(HDC dc, const RECT& r, const std::wstring& value,
                      bool hoverup, bool hoverdn)
{
    RECT b{ r.right - 116, r.top + 4, r.right, r.bottom - 4 };
    stroke_round(dc, b, 6, C_CTL, C_BORDER);
    RECT vt{ b.left, b.top, b.right - 26, b.bottom };
    txt(dc, g_f_ui, vt, value, C_TEXT, DT_CENTER | DT_VCENTER);
    const int cx = b.right - 13;
    draw_chevron(dc, cx, b.top + 9, -1, hoverup ? C_TEXT : C_DIM);
    draw_chevron(dc, cx, b.bottom - 9, +1, hoverdn ? C_TEXT : C_DIM);
}

static void draw_dd(HDC dc, const RECT& r, const std::wstring& value, bool hover, bool open)
{
    RECT b{ r.right - 210, r.top + 4, r.right, r.bottom - 4 };
    stroke_round(dc, b, 6, (hover || open) ? C_CTL_HOVER : C_CTL, C_BORDER);
    RECT vt{ b.left + 10, b.top, b.right - 26, b.bottom };
    txt(dc, g_f_ui, vt, value, C_TEXT, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    draw_chevron(dc, b.right - 14, (b.top + b.bottom) / 2 + (open ? -1 : 1), open ? -1 : +1, C_DIM);
}

static void draw_info(HDC dc, const RECT& r, const std::wstring& value)
{
    RECT vt{ r.left + 150, r.top, r.right, r.bottom };
    txt(dc, g_f_ui, vt, value, C_LABEL, DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS);
}

// Caption of a dropdown row. The lock row also shows what that lock would produce.
static std::wstring dd_caption(Id id)
{
    if (id == ID_LOCK) {
        const double lf = kLockFps[opt_get(id) % 5];
        if (lf > 0.0) {
            wchar_t b[64]{};
            _snwprintf_s(b, _TRUNCATE, L"%.0f fps  ->  %.0f fps 输出", lf, lf * S().interp);
            return b;
        }
    }
    return opt_text(id, opt_get(id));
}

static void draw_popup(HDC dc)
{
    if (!g_popup_id) { return; }
    const Id id = (Id)g_popup_id;
    const int n = opt_count(id);
    if (n <= 0) { return; }
    const int rowH = 30;
    const int w = (int)(g_popup_rect.right - g_popup_rect.left);
    int x = g_popup_rect.left;
    int y = g_popup_rect.bottom + 2;
    const int h = n * rowH + 8;
    if (y + h > kWinH - kBottomH) { y = g_popup_rect.top - h - 2; }   // open upwards

    RECT box{ x, y, x + w, y + h };
    stroke_round(dc, box, 8, RGB(40, 40, 40), C_BORDER);
    for (int i = 0; i < n; ++i) {
        RECT rr{ x + 4, y + 4 + i * rowH, x + w - 4, y + 4 + (i + 1) * rowH };
        if (i == g_popup_hover) { fill_round(dc, rr, 6, C_CTL_HOVER); }
        RECT tt{ rr.left + 10, rr.top, rr.right - 8, rr.bottom };
        txt(dc, g_f_ui, tt, opt_text(id, i),
            (i == opt_get(id)) ? C_ACCENT : C_TEXT, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }
}

static void draw_titlebar(HDC dc, int W)
{
    fill_round(dc, RECT{ 0, 0, W, kTitleH }, 0, RGB(30, 30, 30));
    RECT t{ 14, 0, W - 200, kTitleH };
    txt(dc, g_f_small, t, L"MGPU  -  双显卡帧生成", C_TEXT, DT_LEFT | DT_VCENTER);

    // START / STOP button (like Lossless Scaling's action button, top right)
    RECT btn{ W - 190, 7, W - 100, kTitleH - 7 };
    stroke_round(dc, btn, 6, g_running ? RGB(140, 60, 60) : RGB(56, 96, 70), C_BORDER);
    txt(dc, g_f_ui, btn, g_running ? L"停止" : L"启动", C_TEXT, DT_CENTER | DT_VCENTER);

    // minimise and close glyphs
    RECT mn{ W - 88, 0, W - 52, kTitleH };
    RECT cl{ W - 44, 0, W - 8, kTitleH };
    HBRUSH nb = CreateSolidBrush(C_TEXT);
    RECT bar{ mn.left + 8, mn.top + 18, mn.right - 8, mn.top + 20 };
    FillRect(dc, &bar, nb);
    DeleteObject(nb);
    RECT x1{ cl.left + 9, cl.top + 13, cl.right - 9, cl.top + 15 };
    RECT x2{ cl.left + 9, cl.top + 23, cl.right - 9, cl.top + 25 };
    (void)x1; (void)x2;
    txt(dc, g_f_ui, cl, L"\u2715", C_TEXT, DT_CENTER | DT_VCENTER);
    txt(dc, g_f_ui, mn, L"\u2013", C_TEXT, DT_CENTER | DT_VCENTER);
}


static void draw_sidebar(HDC dc)
{
    fill_round(dc, RECT{ 0, kTitleH, kSideW, kWinH }, 0, C_SIDEBAR);
    txt(dc, g_f_small, RECT{ 16, kTitleH + 14, kSideW - 16, kTitleH + 40 },
        L"游戏配置文件", C_DIM, DT_LEFT | DT_VCENTER);

    int y = kTitleH + 48;
    for (int i = 0; i < (int)g_profiles.size(); ++i) {
        RECT rc{ 10, y, kSideW - 10, y + 34 };
        if (i == g_prof) {
            fill_round(dc, rc, 6, RGB(42, 42, 42));
            fill_round(dc, RECT{ rc.left, rc.top, rc.left + 3, rc.bottom }, 2, C_ACCENT);
        }
        txt(dc, g_f_ui, RECT{ rc.left + 14, rc.top, rc.right - 10, rc.bottom },
            g_profiles[i].name, (i == g_prof) ? C_TEXT : C_LABEL,
            DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        y += 38;
    }

    // + / - buttons under the list
    const int by = y + 6;
    RECT add{ 10, by, 40, by + 30 };
    RECT del{ 46, by, 76, by + 30 };
    stroke_round(dc, add, 6, C_BTN, C_BORDER);
    txt(dc, g_f_ui, add, L"+", C_TEXT, DT_CENTER | DT_VCENTER);
    stroke_round(dc, del, 6, C_BTN, C_BORDER);
    txt(dc, g_f_ui, del, L"\u2212", C_TEXT, DT_CENTER | DT_VCENTER);

    // bottom entries
    RECT man{ 10, kWinH - kBottomH - 40, kSideW - 10, kWinH - kBottomH - 8 };
    txt(dc, g_f_ui, RECT{ man.left + 12, man.top, man.right, man.bottom },
        L"手册", C_LABEL, DT_LEFT | DT_VCENTER);
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps{};
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT cl{};
    GetClientRect(hwnd, &cl);

    // Double buffer: everything is composed into an offscreen bitmap and blitted in
    // one go. Painting straight to the window DC made the whole UI flicker whenever
    // the 500 ms status timer or a mouse move triggered a repaint.
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, cl.right, cl.bottom);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);

    fill_round(dc, cl, 0, C_BG);
    draw_titlebar(dc, cl.right);
    draw_sidebar(dc);

    // profile title
    {
        wchar_t b[128]{};
        _snwprintf_s(b, _TRUNCATE, L"配置：\"%s\"", g_profiles[g_prof].name.c_str());
        txt(dc, g_f_title, RECT{ kSideW + kMargin, kTitleH + 14, cl.right - kMargin,
                                 kTitleH + 54 }, b, C_TEXT, DT_LEFT | DT_VCENTER);
    }

    std::vector<Item> items;
    int content_h = 0;
    layout_items(items, &content_h);
    g_content_h = content_h;

    // clip the scrollable area so cards cannot paint over the title/bottom bars
    HRGN clip = CreateRectRgn(kSideW, kTitleH + 58, cl.right, kWinH - kBottomH);
    SelectClipRgn(dc, clip);

    for (const Item& it : items) {
        RECT rc = it.rc;
        OffsetRect(&rc, 0, -g_scroll);
        if (rc.bottom < kTitleH + 58 || rc.top > kWinH - kBottomH) { continue; }

        if (!it.is_row) {                       // card
            fill_round(dc, rc, 8, C_CARD);
            const CardDef& cd = kCards[it.id - 1000];
            RECT head{ rc.left, rc.top, rc.right, rc.top + kCardHead };
            fill_round(dc, head, 8, C_CARD_HEAD);
            fill_round(dc, RECT{ head.left, head.bottom - 10, head.right, head.bottom },
                       8, C_CARD_HEAD);
            txt(dc, g_f_card, RECT{ head.left + 16, head.top, head.right - 16, head.bottom },
                cd.title, C_TEXT, DT_LEFT | DT_VCENTER);
            continue;
        }

        const RowDef* def = nullptr;
        for (int ci = 0; ci < kCardCount && !def; ++ci) {
            for (int r = 0; r < kCards[ci].n; ++r) {
                if (kCards[ci].rows[r].id == it.id) { def = &kCards[ci].rows[r]; break; }
            }
        }
        if (!def) { continue; }

        const bool hover = (g_hover_y >= rc.top && g_hover_y < rc.bottom &&
                            g_hover_x >= rc.left && g_hover_x < rc.right &&
                            (it.kind == RK_DD || it.kind == RK_TOG || it.kind == RK_SPIN));
        if (hover && it.kind == RK_TOG) { draw_toggle(dc, rc, it.on, true); }
        else if (it.kind == RK_TOG)     { draw_toggle(dc, rc, it.on, false); }

        if (it.kind != RK_CMDLINE) {
            txt(dc, g_f_ui, RECT{ rc.left, rc.top, rc.left + 200, rc.bottom },
                def->label, hover ? C_TEXT : C_LABEL, DT_LEFT | DT_VCENTER);
        }

        switch (it.kind) {
        case RK_INFO: draw_info(dc, rc, row_value(it.id)); break;
        case RK_CMDLINE: {
            // The label sits on the first line, the wrapped command below it.
            RECT lr{ rc.left, rc.top, rc.right, rc.top + 24 };
            txt(dc, g_f_ui, lr, def->label, C_LABEL, DT_LEFT | DT_VCENTER);
            RECT tr{ rc.left, rc.top + 26, rc.right, rc.bottom - 2 };
            HGDIOBJ old = SelectObject(dc, g_f_small);
            SetTextColor(dc, C_DIM);
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, display_cmdline().c_str(), -1, &tr,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
            SelectObject(dc, old);
            break;
        }
        case RK_DD:   draw_dd(dc, rc, dd_caption(it.id), hover,
                              g_popup_id == (int)it.id); break;
        case RK_SPIN: {
            const bool up = hover && (g_hover_x > rc.right - 26) && (g_hover_y < rc.top + 18);
            const bool dn = hover && (g_hover_x > rc.right - 26) && (g_hover_y >= rc.top + 18);
            draw_spin(dc, rc, row_value(it.id), up, dn);
            break;
        }
        default: break;
        }
    }

    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    // bottom bar: live stats
    fill_round(dc, RECT{ kSideW, kWinH - kBottomH, cl.right, kWinH }, 0, RGB(28, 28, 28));
    wchar_t s1[256]{};
    _snwprintf_s(s1, _TRUNCATE,
                 L"%s    show %d fps    xN %.2f    skip %d    gpu %.1f ms    game %.1f ms",
                 g_running ? L"运行中" : L"已停止", (int)g_show, (double)g_xn / 100.0,
                 (int)g_skip, (double)g_gpu, (double)g_game);
    txt(dc, g_f_small, RECT{ kSideW + 16, kWinH - kBottomH, cl.right - 300, kWinH },
        s1, C_LABEL, DT_LEFT | DT_VCENTER);

    std::wstring right = (!g_msg.empty() && GetTickCount() < g_msg_until) ? g_msg : g_last_line;
    if (!right.empty()) {
        txt(dc, g_f_small, RECT{ cl.right - 640, kWinH - kBottomH, cl.right - 16, kWinH },
            right, C_DIM, DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS);
    }

    draw_popup(dc);

    BitBlt(wdc, 0, 0, cl.right, cl.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}


// ------------------------------------------------------------ input handling
static std::wstring hotkey_name()
{
    if (!g_hk_custom) { return g_hotkeys[S().hotkey % kHotkeyCount].name; }
    std::wstring s;
    if (g_hk_mods & MOD_CONTROL) { s += L"Ctrl+"; }
    if (g_hk_mods & MOD_ALT)     { s += L"Alt+"; }
    if (g_hk_mods & MOD_SHIFT)   { s += L"Shift+"; }
    if (g_hk_mods & MOD_WIN)     { s += L"Win+"; }
    static const struct { UINT vk; const wchar_t* name; } kNames[] = {
        { VK_PRIOR, L"PageUp" }, { VK_NEXT, L"PageDown" }, { VK_HOME, L"Home" },
        { VK_END, L"End" }, { VK_INSERT, L"Insert" }, { VK_DELETE, L"Delete" },
        { VK_LEFT, L"Left" }, { VK_RIGHT, L"Right" }, { VK_UP, L"Up" },
        { VK_DOWN, L"Down" }, { VK_SPACE, L"Space" }, { VK_TAB, L"Tab" },
        { VK_PAUSE, L"Pause" },
    };
    for (const auto& e : kNames) {
        if (e.vk == g_hk_vk) { return s + e.name; }
    }
    wchar_t key[32]{};
    if (g_hk_vk >= VK_F1 && g_hk_vk <= VK_F24) {
        _snwprintf_s(key, _TRUNCATE, L"F%d", (int)(g_hk_vk - VK_F1 + 1));
    } else {
        const UINT ch = MapVirtualKeyW(g_hk_vk, MAPVK_VK_TO_CHAR);
        _snwprintf_s(key, _TRUNCATE, L"%c", ch ? (wchar_t)ch : (wchar_t)L'?');
    }
    return s + key;
}

// 8 ms poll: reads the global key state only (no hook, no swallowing), so the keys
// keep working everywhere else. Fires once per press (edge triggered).
static DWORD WINAPI hotkey_poll_thread(LPVOID)
{
    bool was_down = false;
    for (;;) {
        Sleep(8);
        if (!g_hk_poll.load()) { was_down = false; continue; }
        bool gate_ok = true;
        if (g_hk_poll_gate == VK_CAPITAL) {
            gate_ok = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;      // latch: CapsLock on
        } else if (g_hk_poll_gate != 0) {
            gate_ok = (GetAsyncKeyState((int)g_hk_poll_gate) & 0x8000) != 0;
        }
        const bool down = gate_ok &&
            (GetAsyncKeyState((int)g_hk_poll_vk) & 0x8000) != 0;
        if (down && !was_down && g_hwnd) {
            PostMessageW(g_hwnd, WM_HOTKEY, 1, 0);                  // same path as before
        }
        was_down = down;
    }
    return 0;
}

static void apply_hotkey()
{
    if (!g_hwnd) { return; }
    UnregisterHotKey(g_hwnd, 1);

    const Hotkey& preset = g_hotkeys[S().hotkey % kHotkeyCount];
    if (!g_hk_custom && preset.gate != 0) {
        // polled hotkey: the thread owns it now
        g_hk_poll_vk = preset.vk;
        g_hk_poll_gate = preset.gate;
        g_hk_poll = true;
        return;
    }
    g_hk_poll = false;

    UINT mods = g_hk_mods, vk = g_hk_vk;
    if (!g_hk_custom) { mods = preset.mods; vk = preset.vk; }
    if (!RegisterHotKey(g_hwnd, 1, mods | MOD_NOREPEAT, vk)) { set_msg(L"热键被占用，换一个"); }
}

static void toggle_field(Id id)
{
    switch (id) {
    case ID_JITZ:    S().jitter_zero = !S().jitter_zero; break;
    case ID_PROJRZ:  S().proj_rz = !S().proj_rz; break;
    case ID_NODEPTH: S().no_depth = !S().no_depth; break;
    case ID_UNLOCK:  S().unlock_mfg = !S().unlock_mfg; break;
    case ID_SLACK:   S().slack1 = !S().slack1; break;
    case ID_TEAR:    S().tearing = !S().tearing; break;
    case ID_SDR:     S().sdr = !S().sdr; break;
    case ID_LAYERED:
        S().overlay_layered = !S().overlay_layered;
        // A/B 两种叠加层模式互斥：开一个就关掉另一个
        if (S().overlay_layered && S().relay_input) { S().relay_input = false; }
        break;
    case ID_RELAY:
        S().relay_input = !S().relay_input;
        if (S().relay_input && S().overlay_layered) { S().overlay_layered = false; }
        break;
    case ID_OSD:     S().osd = !S().osd; break;
    case ID_PERF:    S().perf_no_throttle = !S().perf_no_throttle; break;
    default: break;
    }
}

static void bump_spin(Id id, int dir)
{
    if (id == ID_INTERP) {
        S().interp += dir;
        if (S().interp < 2) { S().interp = 2; }
        if (S().interp > 6) { S().interp = 6; }
        if (S().interp > 4) { S().unlock_mfg = true; }
    } else if (id == ID_POOL) {
        S().pool += dir;
        if (S().pool < 2) { S().pool = 2; }
        if (S().pool > 6) { S().pool = 6; }
    } else if (id == ID_LAT) {
        S().latency += dir;
        if (S().latency < 1) { S().latency = 1; }
        if (S().latency > 3) { S().latency = 3; }
    }
}

static void set_opt(Id id, int idx)
{
    switch (id) {
    case ID_FG_MODE: S().fg_mode = idx % 2; break;
    case ID_OUT:     S().out_mode = idx % 3; break;
    case ID_MV:      S().mv_mode = idx % 4; break;
    case ID_FOV:     S().fov_sel = idx % 4; break;
    case ID_IVL:     S().ivl_sel = idx % 6; break;
    case ID_UI:      S().ui_none = (idx % 2) == 0; break;
    case ID_RT:      S().rt_sel = idx % 2; break;
    case ID_LOCK:    S().lock_fps = idx % 5; break;
    case ID_TMODE:   S().target_mode = idx % 2; break;
    case ID_GPUSEL:  S().gpu_sel = idx % (1 + (int)g_adapters.size()); break;
    case ID_HOTKEY:
        S().hotkey = idx % kHotkeyCount;
        g_hk_custom = false;
        apply_hotkey();
        break;
    case ID_TARGET:
        if (idx >= 0 && idx < (int)g_wins.size()) {
            g_win_sel = idx;
            S().win_title = g_wins[idx].title;
            S().win_hwnd = g_wins[idx].hwnd;
        }
        break;
    default: break;
    }
    save_all();
}

static int popup_hit(int mx, int my)      // -> option index or -1
{
    if (!g_popup_id) { return -1; }
    const Id id = (Id)g_popup_id;
    const int n = opt_count(id);
    const int rowH = 30;
    const int w = (int)(g_popup_rect.right - g_popup_rect.left);
    int x = g_popup_rect.left;
    int y = g_popup_rect.bottom + 2;
    const int h = n * rowH + 8;
    if (y + h > kWinH - kBottomH) { y = g_popup_rect.top - h - 2; }
    if (mx < x || mx >= x + w || my < y + 4 || my >= y + 4 + n * rowH) { return -1; }
    return (my - (y + 4)) / rowH;
}


// ------------------------------------------------------------------- actions
static void on_click(int mx, int my)
{
    // 1) an open dropdown eats the click first
    if (g_popup_id) {
        const int idx = popup_hit(mx, my);
        if (idx >= 0) { set_opt((Id)g_popup_id, idx); }
        g_popup_id = 0;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }

    // 2) title bar
    if (my < kTitleH) {
        RECT cl{};
        GetClientRect(g_hwnd, &cl);
        const int W = cl.right;
        if (mx >= W - 190 && mx < W - 100) { toggle_bridge(); return; }
        if (mx >= W - 88 && mx < W - 52) { ShowWindow(g_hwnd, SW_MINIMIZE); return; }
        if (mx >= W - 44) { SendMessageW(g_hwnd, WM_CLOSE, 0, 0); return; }
        return;
    }

    // 3) sidebar
    if (mx < kSideW) {
        int y = kTitleH + 48;
        for (int i = 0; i < (int)g_profiles.size(); ++i) {
            if (my >= y && my < y + 34) {
                g_prof = i;
                rescan_windows();
                apply_hotkey();
                save_all();
                InvalidateRect(g_hwnd, nullptr, FALSE);
                return;
            }
            y += 38;
        }
        const int by = y + 6;
        if (my >= by && my < by + 30) {
            if (mx < 40) {
                if (g_profiles.size() < 8) {
                    Profile p{};
                    wchar_t b[32]{};
                    _snwprintf_s(b, _TRUNCATE, L"配置 %d", (int)g_profiles.size() + 1);
                    p.name = b;
                    g_profiles.push_back(p);
                    g_prof = (int)g_profiles.size() - 1;
                    rescan_windows();
                    save_all();
                }
            } else if (mx < 76) {
                if (g_profiles.size() > 1) {
                    g_profiles.erase(g_profiles.begin() + g_prof);
                    if (g_prof >= (int)g_profiles.size()) { g_prof = (int)g_profiles.size() - 1; }
                    rescan_windows();
                    save_all();
                }
            }
        }
        if (my >= kWinH - kBottomH - 40 && my < kWinH - kBottomH - 8) {
            const std::wstring help = exe_dir() + L"\\docs\\UI.md";
            if (GetFileAttributesW(help.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ShellExecuteW(nullptr, L"open", help.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else {
                set_msg(L"docs\\UI.md 不在 exe 旁边");
            }
        }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }

    // 4) main area: only the control itself reacts, never the whole row
    std::vector<Item> items;
    int content_h = 0;
    layout_items(items, &content_h);
    for (const Item& it : items) {
        if (!it.is_row) { continue; }
        RECT rc = it.rc;
        OffsetRect(&rc, 0, -g_scroll);
        // 命中必须同时看 x：只比 y 的话，左列那条 106px 高的"启动命令"行会把
        // 右列同一 y 区间的点击全吃掉（叠加层两个开关点不动就是这个原因）。
        if (my < rc.top || my >= rc.bottom) { continue; }
        if (mx < rc.left || mx >= rc.right) { continue; }

        switch (it.kind) {
        case RK_TOG: {
            // 开关图形只有 40x17，可点区域放宽到 68x25 更好按
            RECT sw{ rc.right - 68, rc.top + 4, rc.right, rc.bottom - 4 };
            if (mx < sw.left || mx > sw.right || my < sw.top || my > sw.bottom) { return; }
            toggle_field(it.id);
            break;
        }
        case RK_DD: {
            RECT box{ rc.right - 210, rc.top + 4, rc.right, rc.bottom - 4 };
            if (mx < box.left || mx > box.right || my < box.top || my > box.bottom) { return; }
            g_popup_id = (int)it.id;
            g_popup_rect = box;
            g_popup_hover = -1;
            break;
        }
        case RK_SPIN: {
            RECT box{ rc.right - 116, rc.top + 4, rc.right, rc.bottom - 4 };
            if (mx < box.left || mx > box.right || my < box.top || my > box.bottom) { return; }
            const int mid = (box.top + box.bottom) / 2;
            bump_spin(it.id, (my < mid) ? +1 : -1);
            break;
        }
        case RK_INFO:
            if (it.id == ID_HOTKEY) {
                g_capture_hotkey = true;
                set_msg(L"按下新的组合键（Esc 取消）");
                InvalidateRect(g_hwnd, nullptr, FALSE);
            }
            return;
        default:
            return;          // info rows do nothing
        }
        save_all();
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
}


// ------------------------------------------------------------ window procedure
static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = h;
        InitializeCriticalSection(&g_line_cs);
        // Rounded corners + a hairline border, like the reference UI (Win11 only; the
        // call fails harmlessly on older builds).
        {
            const DWORD pref = 2 /* DWMWCP_ROUND */;
            DwmSetWindowAttribute(h, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
                                  &pref, sizeof(pref));
            const COLORREF border = RGB(48, 48, 48);
            DwmSetWindowAttribute(h, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
        }
        g_f_title = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_f_card = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_f_ui = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_f_small = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Microsoft YaHei UI");
        {
            DEVMODEW dm{}; dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
                g_refresh = dm.dmDisplayFrequency;
            }
        }
        CloseHandle(CreateThread(nullptr, 0, hotkey_poll_thread, nullptr, 0, nullptr));
        uilog("ui started, hotkey index=%d", S().hotkey);
        rescan_windows();
        apply_hotkey();
        SetTimer(h, 1, 500, nullptr);
        return 0;

    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(h); return 0;

    case WM_TIMER: {
        if (g_running && g_child) {
            DWORD code = 0;
            if (GetExitCodeProcess(g_child, &code) && code != STILL_ACTIVE) {
                stop_bridge();
                set_msg(L"bridge 已退出");
            }
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_APP + 1: InvalidateRect(h, nullptr, FALSE); return 0;

    case WM_HOTKEY:
        uilog("WM_HOTKEY id=%u", (unsigned)wp);
        if (wp == 1) { toggle_bridge(); }
        return 0;

    case WM_MOUSEMOVE: {
        g_hover_x = GET_X_LPARAM(lp);
        g_hover_y = GET_Y_LPARAM(lp);
        if (g_popup_id) { g_popup_hover = popup_hit(g_hover_x, g_hover_y); }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSELEAVE:
        g_hover_x = g_hover_y = -1;
        g_popup_hover = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
        if (g_capture_hotkey) { return 0; }
        on_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        const int avail = kWinH - kBottomH - (kTitleH + 58);
        const int maxs = (g_content_h > avail) ? (g_content_h - avail) : 0;
        g_scroll -= (delta / WHEEL_DELTA) * 60;
        if (g_scroll < 0) { g_scroll = 0; }
        if (g_scroll > maxs) { g_scroll = maxs; }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_NCHITTEST: {
        const LRESULT r = DefWindowProcW(h, msg, wp, lp);
        if (r == HTCLIENT) {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(h, &p);
            if (p.y < kTitleH && p.x < kWinW - 200) { return HTCAPTION; }
        }
        return r;
    }

    case WM_KEYDOWN:
        if (g_capture_hotkey) {
            if (wp == VK_ESCAPE) { g_capture_hotkey = false; InvalidateRect(h, nullptr, FALSE); return 0; }
            if (wp == VK_CONTROL || wp == VK_MENU || wp == VK_SHIFT ||
                wp == VK_LWIN || wp == VK_RWIN) { return 0; }
            UINT mods = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) { mods |= MOD_CONTROL; }
            if (GetKeyState(VK_MENU) & 0x8000)    { mods |= MOD_ALT; }
            if (GetKeyState(VK_SHIFT) & 0x8000)   { mods |= MOD_SHIFT; }
            if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) { mods |= MOD_WIN; }
            if (mods == 0) { return 0; }
            g_hk_mods = mods;
            g_hk_vk = (UINT)wp;
            g_hk_custom = true;
            g_capture_hotkey = false;
            apply_hotkey();
            save_all();
            set_msg(L"热键已更新");
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;

    case WM_CLOSE:
        if (g_running) { stop_bridge(); }
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        KillTimer(h, 1);
        UnregisterHotKey(h, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    enum_adapters();      // must precede load_all(): the GPU row is index-based
    load_all();
    if (g_profiles.empty()) { g_profiles.push_back(Profile{}); }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"mgpu_ui3";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"mgpu_ui3", L"MGPU", WS_POPUP | WS_MINIMIZEBOX,
                                (GetSystemMetrics(SM_CXSCREEN) - kWinW) / 2,
                                (GetSystemMetrics(SM_CYSCREEN) - kWinH) / 2,
                                kWinW, kWinH, nullptr, nullptr, inst, nullptr);
    if (!hwnd) { return 1; }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

