// ============================================================================
//  lock_frame_ui.cpp - 固定倍率 2x / 3x / 4x + fakerFG 输入源前端
//
//  只做四件事：选游戏窗口、选固定倍率、选 fakerFG 输入源、启动/停止 lock_frame.exe。
//  其他旋钮（GPU/输出/叠加层/HDR）保留，是因为它们与锁帧无关、且已实测。
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
#include <commdlg.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <dxgi.h>
#include <bcrypt.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <UIAutomationClient.h>
#include <cstdarg>
#include <cstring>

#include "mgpu_shared.h"
#include "version.h"

#include <atomic>
#include <ctime>
#include <deque>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uiautomationcore.lib")

// ------------------------------------------------------------------- language
// 集中式翻译：静态文案在绘制时查表（txt / 提示框），带参数的动态文案在构造处
// 用 T(zh,en) 选择。查不到就回退中文，保证不会出现空白。
static int g_lang = 0;      // 0 = 中文，1 = English

struct TrPair { const wchar_t* zh; const wchar_t* en; };
static const TrPair kTr[] = {
    // 标题 / 侧栏
    { L"MGPU  双显卡帧生成", L"MGPU  Dual-GPU FG" },
    { L"游戏配置", L"Game profiles" },
    { L"当前游戏", L"Current game" },
    { L"（还没有选择）", L"(none selected)" },
    { L"用[游戏库]扫描或手动选 exe", L"Scan the library or pick an exe" },
    { L"帮助 / 快捷键 (F1)", L"Help / shortcuts (F1)" },
    { L"高级设置", L"Advanced settings" },
    { L"收起高级设置", L"Hide advanced settings" },
    // 三步卡
    { L"把锁帧组件装进游戏", L"Install lock_frame into the game" },
    { L"进入游戏画面", L"Get into the game" },
    { L"选择倍率并启动", L"Pick a multiplier and start" },
    { L"选择游戏 exe 并安装", L"Pick game exe and install" },
    { L"还原游戏目录", L"Restore folder" },
    { L"游戏库 / 扫描", L"Game library / scan" },
    { L"（点这里选择游戏窗口）", L"(click to choose the game window)" },
    { L"刷新", L"Refresh" },
    { L"建议游戏内锁 %.0f fps（%dx → 输出 %u fps）", L"Lock %.0f fps in game (%dx -> %u fps output)" },
    { L"建议锁 %.1f fps（%u Hz 不能被 %dx 整除，节奏不会完美）", L"Lock %.1f fps (%u Hz not divisible by %dx; pacing will not be perfect)" },
    { L"启动", L"Start" },
    { L"停止", L"Stop" },
    { L"输入 %.1f fps", L"input %.1f fps" },
    // 状态卡
    { L"运行状态", L"Status" },
    { L"游戏侧安装", L"Game install" },
    { L"Intel 运行时", L"Intel runtime" },
    { L"查看日志", L"View log" },
    { L"一键诊断", L"Diagnostics" },
    { L"需要细调（GPU / 色彩 / 热键 / 帧池）？点[高级设置]；快捷键与常见问题按 F1。",
      L"Need tuning (GPU / colour / hotkey / frame pool)? Click [Advanced]; press F1 for shortcuts and FAQ." },
    // 帮助面板
    { L"帮助与快捷键", L"Help and shortcuts" },
    { L"关闭", L"Close" },
    { L"三步开始", L"Three steps" },
    { L"1. 装进游戏：点[选择游戏 exe 并安装]，或用[游戏库 / 扫描]自动找 Steam 游戏。",
      L"1. Install: click [Pick game exe and install], or use [Game library / scan] to find Steam games." },
    { L"2. 进游戏：无边框 / 窗口化全屏，开启 DLSS 帧生成；游戏起来后窗口会自动选中。",
      L"2. In game: borderless fullscreen + DLSS Frame Generation; the window is auto-selected." },
    { L"3. 倍率 + 启动：选 2x / 3x / 4x，按建议锁帧，点[启动]。",
      L"3. Multiplier + start: pick 2x / 3x / 4x, apply the suggested in-game lock, click [Start]." },
    { L"快捷键", L"Shortcuts" },
    { L"Tab / Shift+Tab 切换焦点　Enter / Space 触发　Esc 关闭面板",
      L"Tab / Shift+Tab move focus    Enter / Space activate    Esc close panel" },
    { L"PageUp / PageDown / Home / End：列表翻页　F1：打开本帮助",
      L"PageUp / PageDown / Home / End: page lists    F1: open this help" },
    { L"常见问题", L"FAQ" },
    { L"看不到游戏窗口：先进实际画面再点[刷新]；确认游戏不是独占全屏。",
      L"No game window: reach the in-game scene, click [Refresh]; avoid exclusive fullscreen." },
    { L"状态是“部分插帧”：HUD 可能挡住画面四角，或窗口被缩放。",
      L"Status shows partial frame generation: a HUD may cover the screen corners, or the window is scaled." },
    { L"只有 1 块 GPU：能跑，但插帧会和游戏抢性能；建议双卡。",
      L"Only one GPU: it runs, but frame generation competes with the game; two GPUs are recommended." },
    // 游戏库 / 日志
    { L"游戏库（%d）", L"Game library (%d)" },
    { L"扫描本机", L"Scan this PC" },
    { L"手动 exe", L"Manual exe" },
    { L"使用此游戏", L"Use this game" },
    { L"安装 / 更新", L"Install / update" },
    { L"还原", L"Restore" },
    { L"删除", L"Remove" },
    { L"还没有游戏。点右上角[扫描本机]（Steam / Epic / GOG / 注册表），或[手动 exe]。",
      L"No games yet. Click [Scan this PC] (Steam / Epic / GOG / registry), or [Manual exe]." },
    { L"运行日志（最近 400 行）", L"Runtime log (last 400 lines)" },
    { L"复制全部", L"Copy all" },
    { L"打开目录", L"Open folder" },
    { L"（还没有日志：点「启动」跑一次消费端，这里会显示它的输出）",
      L"(no log yet: click [Start] once and the consumer output will show here)" },
    // 抽屉：卡片与行
    { L"帧生成", L"Frame generation" },
    { L"环境自检", L"Self-check" },
    { L"游戏侧配置", L"Game-side config" },
    { L"捕获", L"Capture" },
    { L"启动", L"Launch" },
    { L"GPU / 显示器", L"GPU / display" },
    { L"叠加层", L"Overlay" },
    { L"类型", L"Type" },
    { L"固定倍率", L"Fixed multiplier" },
    { L"输出", L"Output" },
    { L"MV/深度重建", L"MV/depth rebuild" },
    { L"fakerFG 输入源", L"fakerFG input" },
    { L"MV/深度传输比例", L"MV/depth transfer" },
    { L"游戏侧日志 (Log)", L"Game-side log (Log)" },
    { L"100%（1:1，最清晰）", L"100% (1:1, sharpest)" },
    { L"150%（默认，每维 2/3）", L"150% (default, 2/3 each)" },
    { L"200%（每维 1/2）", L"200% (1/2 each)" },
    { L"400%（每维 1/4，最省）", L"400% (1/4 each, lightest)" },
    { L"游戏配置", L"Game config" },
    { L"版本", L"Version" },
    { L"GPU", L"GPU" },
    { L"XeSS 运行时", L"XeSS runtime" },
    { L"游戏配置检查", L"Game config check" },
    { L"安装包完整性", L"Package integrity" },
    { L"捕获 API", L"Capture API" },
    { L"MV/深度传输", L"MV/depth transfer" },
    { L"色彩空间", L"Colour space" },
    { L"帧池深度", L"Frame pool" },
    { L"最大帧延迟数", L"Max frame latency" },
    { L"帧生成用的 GPU", L"Frame-generation GPU" },
    { L"输出显示器（只读）", L"Output display (read-only)" },
    { L"界面缩放", L"UI scale" },
    { L"界面语言 / Language", L"Language" },
    { L"游戏窗口", L"Game window" },
    { L"开关热键（点击可改）", L"Toggle hotkey (click to change)" },
    { L"建议锁帧", L"Suggested lock" },
    { L"启动命令", L"Command line" },
    { L"分层窗口 (A/B)", L"Layered window (A/B)" },
    { L"输入转发 (A/B)", L"Input relay (A/B)" },
    { L"单行 OSD", L"Single-line OSD" },
    // 抽屉：选项
    { L"1:1 原生", L"1:1 native" },
    { L"5K -> 4K (0.75x)", L"5K -> 4K (0.75x)" },
    { L"半分辨率", L"Half resolution" },
    { L"HDR10（默认）", L"HDR10 (default)" },
    { L"SDR（BT.709）", L"SDR (BT.709)" },
    { L"DLSSG（推荐）", L"DLSSG (recommended)" },
    { L"自动（跟随系统）", L"Auto (system DPI)" },
    { L"自动（窗口所在显示器 / 避开游戏卡）", L"Auto (window monitor / avoid game GPU)" },
    { L"30 fps", L"30 fps" },
    { L"24 fps", L"24 fps" },
    { L"40 fps", L"40 fps" },
    { L"60 fps", L"60 fps" },
    { L"前景窗口 (鼠标点进游戏再按热键)", L"Foreground window (click into the game, then hotkey)" },
    { L"上一次启动的窗口", L"Last launched window" },
    // 自检 / 信息行（静态）
    { L"按 fakerFG.ini 的 StreamPercent", L"from fakerFG.ini StreamPercent" },
    { L"→ 输入画面尺寸（默认开）", L"-> input frame size (on by default)" },
    { L"(未知)", L"(unknown)" },
    { L"(先选择游戏窗口)", L"(select the game window first)" },
    { L"未选择游戏或缺少 fakerFG.ini", L"No game selected or fakerFG.ini missing" },
    { L"缺少 fakerFG.ini", L"fakerFG.ini missing" },
    { L"FgOutput 不是 Capture", L"FgOutput is not Capture" },
    { L"Passthrough=1（应为 0）", L"Passthrough=1 (should be 0)" },
    { L"FgInput 为空", L"FgInput is empty" },
    { L"面板刷新", L"Panel refresh" },
    { L"按下新组合键...", L"Press a new hotkey..." },
    { L"(无窗口)", L"(no window)" },
    { L"(无)", L"(none)" },
    { L"不锁帧", L"No lock" },
    { L"缺少 libxess_fg.dll / libxell.dll", L"Missing libxess_fg.dll / libxell.dll" },
    { L"缺少 libxell.dll", L"Missing libxell.dll" },
    { L"缺少 libxess_fg.dll", L"Missing libxess_fg.dll" },
    { L"游戏目录 fakerFG 里缺少 libxess_fg.dll（点[重新应用配置]自动写入）",
      L"libxess_fg.dll missing in <game>\\fakerFG (Re-apply config writes it)" },
    { L"未提供 SHA256SUMS.txt（开发构建）", L"SHA256SUMS.txt not provided (dev build)" },
    { L"无匹配文件（开发构建）", L"No matching files (dev build)" },
    { L"未校验", L"Not checked" },
    { L"游戏库 · ", L"Library · " },
    { L"v%s · 协议 v%u", L"v%s · protocol v%u" },
    { L"完成", L"Done" },
    { L"滚轮翻页 · Tab/上下切换 · Enter 激活 · Esc 收起",
      L"Wheel to scroll · Tab/Up/Down to move · Enter to activate · Esc to close" },
    { L"fakerFG.ini 路径", L"fakerFG.ini path" },
    { L"自选 exe…", L"Pick exe…" },
    { L"主程序…", L"Main exe…" },
    { L"选择主程序", L"Choose the main executable" },
    { L"按匹配分数排序；点击一行即设为主程序并保存", L"Sorted by match score; click a row to set it" },
    { L"已设置主程序：", L"Main exe set: " },
    { L"Esc 关闭 · 想换别的 exe 用[自选 exe…]", L"Esc to close · use [Pick exe...] for anything else" },
    { L"主程序待解析", L"exe not resolved" },
    { L"主程序：", L"exe: " },
    { L"自选 exe 安装", L"Install selected exe" },
    { L"安装到游戏", L"Install to game" },
    { L"安装完成", L"Installed" },
    { L"安装失败", L"Install failed" },
    { L"已选择：", L"Selected: " },
};
static const wchar_t* tr_lookup(const wchar_t* zh)
{
    if (g_lang == 0 || zh == nullptr) { return nullptr; }
    for (const TrPair& p : kTr) {
        if (wcscmp(p.zh, zh) == 0) { return p.en; }
    }
    return nullptr;
}

static const wchar_t* T(const wchar_t* zh, const wchar_t* en)
{
    return (g_lang == 0) ? zh : en;
}





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
// 整个界面按"逻辑像素"布局（1200x620 这套坐标保持不变），绘制时用
// MM_ANISOTROPIC 把逻辑坐标映射到实际客户区。这样 125% / 150% / 200% 缩放下
// 字号、卡片、点击区域一起变大，而不是缩成一小块看不清的窗口。
static double g_scale = 1.0;
static BOOL   g_dpi_aware = FALSE;
static int    g_scale_pct = 0;     // 0 = 跟随系统；>=80 = 手动百分比（高级设置）

static double dpi_scale_for(HWND h)
{
    UINT dpi = 96;
    if (h != nullptr) { dpi = GetDpiForWindow(h); }
    else              { dpi = GetDpiForSystem(); }
    if (dpi < 96) { dpi = 96; }
    return static_cast<double>(dpi) / 96.0;
}

static double effective_scale(HWND h)
{
    if (g_scale_pct >= 80 && g_scale_pct <= 300) {
        return static_cast<double>(g_scale_pct) / 100.0;
    }
    return dpi_scale_for(h);
}

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
static const COLORREF C_OK        = RGB(96, 190, 120);   // 就绪 / 正在插帧
static const COLORREF C_WARN      = RGB(226, 180, 80);   // 可继续，但要注意
static const COLORREF C_BAD       = RGB(220, 96, 96);    // 阻断 / 几乎没插帧

// ------------------------------------------------------------------- state
struct Profile;

struct State
{
    // 帧生成：固定倍率 2x / 3x / 4x
    int  fg_mode = 2;          // 0 = 2x, 1 = 3x, 2 = 4x（默认 4x，沿用旧版行为）
    int  interp = 4;           // 由 fg_mode 推导的倍率（只用于显示）
    // fakerFG 侧输入源：0 DLSSG（推荐）, 1 NVNGX, 2 Upscaler, 3 FSR
    int  fg_input = 0;
    // fakerFG.ini：MV/深度传输比例（100=1:1，150=每维 2/3，200=1/2，400=1/4）
    int  stream_pct = 150;
    bool fg_log = true;        // fakerFG.ini 的 Log
    // v30: the old per-frame multiplier (--interp-cadence, "low-fps adaptation") was
    // removed from the UI. It disabled the slot gate by design and changed the count every
    // batch, which flickers at 3x/4x; the continuous feed keeps slow scenes smooth instead.
    // 输出帧率上限 (Hz)，直接手输：0 = 面板刷新（填满每个 vblank，不发 --fg-out-hz）；
    // 5..1000 = 上限（--fg-out-hz N）。只在"动态倍率"开启时有意义：它决定每帧要几张，
    // 所以"120Hz 面板但只要 60fps"就是填 60（输入 30 时正好 2x）。最低仍是 2x，
    // 做不到时 bridge 会打一行 warning 说明实际会跑到多少 Hz。
    int  out_fps = 0;
    int  out_mode = 0;         // 0 1:1, 1 scale 0.75, 2 den 2
    bool perf_no_throttle = false;
    // 捕获
    int  pool = 4;             // -> --pool
    int  latency = 1;          // -> --latency
    // WGC 交付节流（GraphicsCaptureSession5::MinUpdateInterval）**不再暴露为选项**：
    // 2026-11-10 实测关掉它（0 ms）后输入延迟与流畅度立刻明显改善，而默认的 ~16.6 ms 会把
    // 捕获钉在 ~60 fps。现在由 bridge 默认 = 0（`--wgc-minint 16` 可恢复旧行为，仅命令行）。
    // XeFG 输入
    // 运动矢量不再由 UI 选择：bridge 按 producer 实际发布的记录自动决定（见 bridge 的
    // "mv: auto -> ..." 日志）。--no-mv / --fg-of 仍是命令行实验开关。
    bool jitter_zero = true;
    int  fov_sel = 1;          // 55 / 58.7 / 60 / 65
    // Input interval the pacing model assumes: 0 = measured, 1 = 25 ms, 2 = 33 ms.
    // Feeds --input-ms; measured benefit at 3x+MV: slow 17->0, skip 19->13 (docs 5.18).
    int  ivl_sel = 0;
    bool no_marker = false;     // --no-marker: no in-frame pairing key (falls back to time/FIFO)
    // --upsample (A/B): rebuild MV+depth from the StreamPercent size to the game's render
    // size on the consumer GPU. XeFG requires both to be the same size, so this is one
    // all-or-nothing switch. Off by default until a real run decides whether it stays
    // (docs/design/UPSAMPLE_QUALITY.md).
    bool upsample = false;
    // v30 lossless transport (fakerFG.ini MvCodec/DepthCodec): 0 = off, 1 = forced on,
    // 2 = auto (default: use the codec when this machine can run it, otherwise the old
    // cross-adapter copy path). Written into the game-side ini by apply_fg_input().
    int  codec = 2;
    // 1.2.9: depth codec resolution. MV stays 1:1 unconditionally (it drives the warp and
    // must stay bit-exact); depth only feeds the disocclusion clamp, so it can travel at
    // 150% (2560x1440) - measured on CP2077 4K: depth encode 2.05 -> 0.79 ms (-61%) and
    // 10.8 -> 5.56 MB per frame with MV untouched (docs/design/PERF_OPT_2026-11-23.md 7.3).
    // 100 = per-pixel lossless, 150 = default, 200 = smaller still (higher edge risk).
    int  depth_pct = 150;
    // 1.3.0: UI composition (producer HUDless patch + consumer reconstruction + XeFG
    // mode 4) is a supported default. ON = interpolated frames no longer smear the HUD.
    bool ui_compose = true;
    // 游戏侧配置版本：false = fakerFG.ini（发布精简版），true = fakerFG.full.ini
    // （测试全参数版）。切换时会备份并覆盖游戏目录的 fakerFG.ini，重启游戏生效。
    bool ini_full = false;
    // --stream-auto: the bridge picks the StreamPercent (both streams) from the measured
    // WGC hop. --stream-asym: MV stays 1:1, depth is scaled (implies upsample so the
    // tagged pair is one size). Both are A/B and default off.
    bool stream_auto = false;
    bool stream_asym = false;
    // Input prefetch (default on): the consumer copies the producer's next MV/depth slot
    // into its own VRAM during the WGC wait; the colour arrival only does a local lookup.
    // Off = the old "copy after WGC" order (A/B).
    bool input_prefetch = true;
    // 0 = let bridge choose (monitor rule / avoid the producer); 1+ = position in
    // the enumerated adapter list -> --gpu <index>.
    int  gpu_sel = 0;
    bool ui_none = true;
    int  rt_sel = 1;           // 0 ema, 1 median
    bool unlock_mfg = true;
    // --game-quirks apply|off: OptiScaler's per-game FG-input table (default on).
    bool game_quirks = true;
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
    // 启动目标：0 = 前台窗口（旧习惯），1 = 下拉里选中的窗口（正式版默认）
    int  target_mode = 1;
    int  lock_fps = 3;         // hint only: 0 off, 30, 24, 40, 60 (40 = 3 vblanks @120Hz)
    int  hotkey = 0;           // index into g_hotkeys (0 = Ctrl+PageUp)
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

// 游戏库：按游戏目录（exe 所在目录）记住名称 / 倍率 / 输入源 / 锁帧。
// 这样换游戏、标题变化、窗口重开都不用重新配一遍。
struct GameEntry
{
    std::wstring root;      // 安装根目录（扫描得到，仅用于展示/继续查找）
    std::wstring dir;       // 实际部署目录（游戏 exe 所在），可能为空
    std::wstring exe;       // 解析出的主程序完整路径（可能为空 = 待解析）
    std::wstring name;      // 显示名
    std::wstring source;    // Steam / Epic / GOG / 注册表 / 手动
    std::wstring profile;   // profiles\xxx.ini（相对 exe 目录；空=旧档案，待迁移）
    int          mult = 2;
    int          fg_input = 0;
    int          lock = 0;          // 旧字段：= st.lock_fps，仅兼容旧档案
    unsigned long long last_use = 0;
    State        st;                // 这个游戏自己的完整配置（权威来源）
};
static std::vector<GameEntry> g_games;
static size_t g_resolve_cursor = 0;   // 后台初始化：轮询到哪个条目
static bool   g_pick_view = false;     // 主程序候选面板

// 当前游戏名字的行内重命名（点击名字进入，Enter 保存 / Esc 取消）。
static bool         g_name_edit = false;
static bool         g_name_fresh = false;   // 刚进入编辑：首字符覆盖原名字
static std::wstring g_name_buf;
static RECT         g_name_hit{};
static void name_edit_commit(bool save);
static void name_edit_start();
static std::vector<WinEntry> g_wins;
static int                   g_win_sel = -1;
static bool                  g_user_picked = false;   // 用户手动选过窗口就不再自动切换
static HWND                  g_last_auto_hwnd = nullptr;

static HWND    g_hwnd = nullptr;
static HFONT   g_f_title = nullptr, g_f_card = nullptr, g_f_ui = nullptr, g_f_small = nullptr;
static HFONT   g_f_mono = nullptr;
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
static int     g_tip_id = 0;              // 当前悬停的控件（0 = 无）
static DWORD   g_tip_since = 0;           // 悬停开始时间（>450ms 才显示说明）
static int     g_popup_id = 0;            // dropdown id whose list is open (0 = none)
static RECT    g_popup_rect{};            // where the closed control sits
static int     g_popup_hover = -1;
static bool    g_dragging = false;

// ------------------------------------------------------------- 内嵌日志查看器
// 保留最近 400 行 consumer 输出 + UI 消息，供"看日志"面板显示。写文件仍然是
// lock_frame_ui_HHMMSS.txt（完整日志），这里只是让用户不用离开界面。
static std::deque<std::wstring>  g_log_lines;
static std::vector<std::wstring> g_log_snap;
static bool                      g_log_view = false;
static int                       g_log_scroll = 0;
static bool                      g_line_cs_ready = false;
static bool                      g_show_cmd = false;   // 高级模式：启动命令默认折叠
static bool                      g_welcome = false;    // 首次运行的三件事提示
static bool                      g_help_view = false;  // 帮助 / 快捷键面板
static std::wstring              g_sha_text = L"未校验";  // 安装包完整性（启动时一次）

// ------------------------------------------------------- 简易模式 / 就绪状态
// Default to the 3-step view. The old card view stays available as "高级设置"
// and keeps its own state; switching only flips this flag.
static bool                g_advanced = false;
static std::wstring        g_game_dir;              // 游戏 exe 所在目录（部署目标）
static std::atomic<int>    g_pair_exact{ 0 };       // 最近一个 report 窗口的 exact
static std::atomic<int>    g_pair_total{ 0 };       // 最近一个 report 窗口的 total

struct Ready
{
    bool deployed = false;      // 游戏目录里已有 fakerFG.dll + dxgi.dll
    bool runtime  = false;      // libxess_fg.dll（3x/4x 还需要 libxell.dll）
    bool window   = false;      // 下拉选中的窗口仍然存在
    bool producer = false;      // 该进程已加载 xess_mfg_producer.dll
    bool ini      = false;      // fakerFG.ini 是消费者链路配置
};
static Ready g_ready{};
static bool  g_ini_drift = false;   // fakerFG.ini 与保存的配置不一致（被外部改过）


// 有选中游戏时，配置属于这个游戏；没有选游戏时才落到"通用配置"（p0）。
static void drawer_focus_step(int dir);                      // 抽屉键盘焦点
static void close_advanced_silent();                         // 打开其它子页时收起抽屉
static GameEntry* find_game_mut(const std::wstring& dir);   // 定义见 find_game
static std::wstring norm_path(const std::wstring& p);        // 定义见 find_game 之前
static void save_all();                                      // 定义见 remember_game 之后
static bool game_entry_installed(const GameEntry& e);        // 定义见游戏库部分
static void draw_dot(HDC dc, int cx, int cy, COLORREF c);    // 定义见绘制部分
static void rescan_windows();                                // 定义见窗口枚举部分
static State& S()
{
    if (!g_game_dir.empty()) {
        GameEntry* e = find_game_mut(g_game_dir);
        if (e != nullptr) { return e->st; }
    }
    if (g_profiles.empty()) { g_profiles.push_back(Profile{}); }
    if (g_prof < 0 || g_prof >= (int)g_profiles.size()) { g_prof = 0; }
    return g_profiles[(size_t)g_prof].st;
}



// ------------------------------------------------------------------- ids
enum Id
{
    ID_NONE = 0,
    ID_FG_TYPE, ID_FG_MODE, ID_INTERP, ID_OUT, ID_PERF, ID_UPSAMPLE_INFO,
    ID_FG_INPUT, ID_FG_INI, ID_STREAM_PCT, ID_CODEC, ID_DEPTH_PCT, ID_UI_COMPOSE, ID_INI_PROFILE, ID_FG_LOG,
    ID_VERSION, ID_CHECK_GPU, ID_CHECK_RUNTIME, ID_CHECK_GAME,
    ID_CAP_API, ID_POOL, ID_LAT, ID_STREAM_INFO,
    ID_JITZ, ID_FOV, ID_NOMARKER, ID_UPSAMPLE, ID_STRAUTO, ID_STRASYM, ID_PREFETCH, ID_UI, ID_RT, ID_UNLOCK, ID_GAME_QUIRKS, ID_SLACK, ID_IVL,
    ID_OUTFPS, ID_HEAD_FIXED, ID_HEAD_DYN,
    ID_TEAR, ID_SDR, ID_LAYERED, ID_RELAY, ID_OSD,
    ID_GPU, ID_GPUSEL, ID_DISPLAY, ID_UISCALE, ID_CHECK_SHA, ID_LANG,
    ID_TMODE, ID_TARGET, ID_HOTKEY, ID_LOCK, ID_CMD,
    ID_MIN, ID_CLOSE, ID_START, ID_ADD, ID_DEL,
    // 简易模式（P0）：一键安装 / 还原、窗口、倍率、启动、诊断
    ID_S_INSTALL, ID_S_UNDEPLOY, ID_S_WINDOW, ID_S_REFRESH, ID_S_START,
    ID_S_2X, ID_S_3X, ID_S_4X, ID_S_DIAG, ID_S_LOGVIEW, ID_S_GAMELIB, ID_S_PICKEXE,
    ID_S_HELP, ID_S_ADV,
    // 内嵌日志查看器
    ID_LOG_REFRESH, ID_LOG_CLOSE,
    // 游戏库（下拉）
    ID_GAMELIB,
};

enum RowKind { RK_INFO = 0, RK_DD, RK_SPIN, RK_TOG, RK_SLIDER, RK_CMDLINE, RK_EDIT, RK_HEAD };

struct RowDef { Id id; const wchar_t* label; RowKind kind; };
struct CardDef { const wchar_t* title; const RowDef* rows; int n; int col; };

// v30: only the FIXED multiplier path is exposed (2x/3x/4x). The old per-frame multiplier
// ("low-fps adaptation") was removed: it fought the slot gate and flickered at 3x/4x.
// RK_HEAD is a section title (not clickable); rows outside the mode are greyed out.
static const RowDef kRowsFg[] = {
    { ID_FG_TYPE, L"类型",            RK_INFO },
    { ID_FG_MODE, L"固定倍率",        RK_DD   },
    { ID_OUT,     L"输出",            RK_DD   },
    { ID_UPSAMPLE_INFO, L"MV/深度重建", RK_INFO },
    // 颜色走 WGC；MV/depth 的 StreamPercent 由 fakerFG.ini 决定（默认 150，Wuchang 实测用 100），
    // 消费端把收到的 MV/depth 联合重建到输入画面尺寸。
};
static const RowDef kRowsGame[] = {
    { ID_FG_INPUT,   L"fakerFG 输入源",     RK_INFO },
    { ID_STREAM_PCT, L"MV/深度传输比例",    RK_DD   },
    { ID_CODEC,      L"无损传输（码流）",   RK_DD   },
    { ID_DEPTH_PCT,  L"深度码流（无损）",   RK_DD   },
    { ID_UI_COMPOSE, L"UI 合成（HUDless）", RK_DD   },
    { ID_INI_PROFILE, L"配置版本（游戏侧）", RK_DD   },
    { ID_GAME_QUIRKS, L"游戏适配（OptiScaler 表）", RK_TOG },
    { ID_FG_LOG,     L"游戏侧日志 (Log)",   RK_TOG  },
    { ID_FG_INI,     L"fakerFG.ini 路径",   RK_INFO },
};
static const RowDef kRowsCheck[] = {
    { ID_VERSION,       L"版本",          RK_INFO },
    { ID_CHECK_GPU,     L"GPU",           RK_INFO },
    { ID_CHECK_RUNTIME, L"XeSS 运行时",   RK_INFO },
    { ID_CHECK_GAME,    L"游戏配置检查",  RK_INFO },
    { ID_CHECK_SHA,     L"安装包完整性",  RK_INFO },
};
static const RowDef kRowsCap[] = {
    { ID_CAP_API, L"捕获 API",        RK_INFO },
    { ID_STREAM_INFO, L"MV/深度传输", RK_INFO },
    { ID_SDR,     L"色彩空间",        RK_DD   },
    { ID_POOL,    L"帧池深度",        RK_SPIN },
    { ID_LAT,     L"最大帧延迟数",    RK_SPIN },
    // 输入预拷与"关闭标记配对"是 bridge 时代的 A/B 开关；锁帧版强制关闭，
    // 留在这里只会让人误以为可以点，所以从界面移除（命令行仍可解析）。
};
static const RowDef kRowsDev[] = {
    { ID_GPUSEL,  L"帧生成用的 GPU",      RK_DD   },
    { ID_DISPLAY, L"输出显示器（只读）", RK_INFO },
    { ID_UISCALE, L"界面缩放",           RK_DD   },
    { ID_LANG,    L"界面语言 / Language", RK_DD   },
};
static const RowDef kRowsLaunch[] = {
    { ID_TARGET,  L"游戏窗口",             RK_DD   },
    { ID_HOTKEY,  L"开关热键（点击可改）", RK_INFO },
    { ID_LOCK,    L"建议锁帧",             RK_INFO },
    { ID_CMD,     L"启动命令",             RK_CMDLINE },
};

static const RowDef kRowsOverlay[] = {
    { ID_LAYERED, L"分层窗口 (A/B)", RK_TOG },
    { ID_RELAY,   L"输入转发 (A/B)", RK_TOG },
    { ID_OSD,     L"单行 OSD",        RK_TOG },
};

// Row counts are derived from the arrays, never hard-coded: a stale count reads one
// RowDef past the end and crashes the paint pass (found 2026-11-13 after removing a row).
template <int N> constexpr int rows_of(const RowDef (&)[N]) { return N; }

static const CardDef kCards[] = {
    { L"帧生成",       kRowsFg,     rows_of(kRowsFg), 0 },
    { L"环境自检",     kRowsCheck,  rows_of(kRowsCheck), 0 },
    { L"游戏侧配置",   kRowsGame,   rows_of(kRowsGame), 1 },
    { L"捕获",         kRowsCap,    rows_of(kRowsCap), 1 },
    { L"启动",         kRowsLaunch, rows_of(kRowsLaunch), 0 },
    { L"GPU / 显示器", kRowsDev,    rows_of(kRowsDev), 1 },
    { L"叠加层",       kRowsOverlay, rows_of(kRowsOverlay), 1 },
    // The "高级" card (FOV / 输入间隔) was removed on 2026-11-08: XeFG ignores the
    // synthetic projection, and frameRenderTime is now vblank-snapped (--fg-rt-mode
    // quant), which subsumes what --input-ms was used for. Both stay CLI-only.
};
static const int kCardCount = (int)(sizeof(kCards) / sizeof(kCards[0]));

// ------------------------------------------------------------------ options
// 固定倍率：2x / 3x / 4x。显示文本按当前面板刷新率动态计算输入帧率。
// 本版只实现 DLSSG 输入（Streamline tag）。UI 不再暴露输入源选择。
static const wchar_t* kOptIniProfile[] = { L"精简（发布）", L"完整（测试）" };
// StreamPercent：100 = 1:1，150 = 每维 2/3，200 = 每维 1/2，400 = 每维 1/4
static const wchar_t* kOptStream[] = {
    L"100%（1:1，最清晰）", L"150%（默认，每维 2/3）", L"200%（每维 1/2）", L"400%（每维 1/4，最省）"
};
static const int kStreamPct[] = { 100, 150, 200, 400 };
// 无损传输（Protocol v30 codec）：写进游戏侧 fakerFG.ini 的 MvCodec / DepthCodec。
static const wchar_t* kOptCodec[] = {
    L"关闭（老路径：跨卡直接拷贝 + 升降采样）",
    L"开启（逐位无损，强制 1:1）",
    L"自动（推荐：能跑就用，跑不了自动退回老路径）"
};
static const wchar_t* kCodecIni[] = { L"0", L"1", L"auto" };
// 1.2.9: depth 码流分辨率（MV 永远是 100%）。
static const wchar_t* kOptDepthPct[] = {
    L"100%（逐像素无损，depth 编码 +1.3 ms）",
    L"150%（默认，每维 2/3；depth 编码 -61%）",
    L"200%（每维 1/2，更省）"
};
static const int kDepthPct[] = { 100, 150, 200 };
// 1.3.0: UI composition（producer HUDless patch + mode 4），默认开启。
static const wchar_t* kOptUiCompose[] = {
    L"关闭（最大帧率，HUD 会在插值帧里残影）",
    L"开启（默认；producer patch + XeFG mode 4，HUD 不残影）"
};
static const wchar_t* kFgInputIni[]  = { L"DLSSG", L"NVNGX", L"Upscaler", L"FSR" };
static const wchar_t* kOptOut[]    = { L"1:1 原生", L"5K -> 4K (0.75x)", L"半分辨率" };
static const wchar_t* kOptFov[]    = { L"55.0", L"58.7", L"60.0", L"65.0" };
static const wchar_t* kOptIvl[]    = { L"自动（倍数+1 vblank 余量）", L"16.7 ms (60 fps)", L"20 ms (50 fps)",
                                      L"25 ms (40 fps)", L"33 ms (30 fps)", L"41.7 ms (24 fps)" };
static const wchar_t* kOptUi[]     = { L"关闭", L"自动" };
static const wchar_t* kOptRt[]     = { L"ema", L"median (16 帧)" };
static const wchar_t* kOptLock[]   = { L"不锁帧", L"30 fps", L"24 fps", L"40 fps", L"60 fps" };
static const wchar_t* kOptTMode[]  = { L"前景窗口 (鼠标点进游戏再按热键)", L"上一次启动的窗口" };
// 色彩空间：我们捕获到的是 scRGB(FP16) 的 HDR 画面，默认按 HDR10（BT.2020 primaries + PQ）
// 编码输出；--sdr 走 BT.709 + gamma 2.2 且告诉交换链这是 SDR。
// 在 HDR 显示器上选 SDR 会看起来发灰/发闷（这是 SDR 在 HDR 屏上的正常表现，不是 bug），
// 反之在 SDR 显示器上保持 HDR10 会发灰发亮。所以做成一个显式开关。
static const wchar_t* kOptColor[]  = { L"HDR10（默认）", L"SDR（BT.709）" };
static const wchar_t* kOptUiScale[] = { L"自动（跟随系统）", L"100%", L"110%", L"125%",
                                       L"150%", L"175%", L"200%" };
static const int      kUiScalePct[] = { 0, 100, 110, 125, 150, 175, 200 };
static const double   kLockFps[]   = { 0.0, 30.0, 24.0, 40.0, 60.0 };

static std::wstring hotkey_name();          // defined with the input handling

static int opt_count(Id id)
{
    switch (id) {
    case ID_FG_MODE: return 3;
    case ID_FG_INPUT: return 1;
    case ID_INI_PROFILE: return 2;
    case ID_STREAM_PCT: return 4;
    case ID_CODEC:   return 3;
    case ID_DEPTH_PCT: return 3;
    case ID_UI_COMPOSE: return 2;
    case ID_OUT:     return 3;
    case ID_SDR:     return 2;
    case ID_FOV:     return 4;
    case ID_IVL:     return 6;
    case ID_UI:      return 2;
    case ID_RT:      return 2;
    case ID_HOTKEY:  return kHotkeyCount;
    case ID_LOCK:    return 5;
    case ID_TARGET:  return (int)g_wins.size();
    case ID_TMODE:   return 2;
    case ID_GPUSEL: return 1 + (int)g_adapters.size();
    case ID_UISCALE: return 7;
    case ID_LANG:    return 2;
    default:         return 0;
    }
}

static const wchar_t* opt_text(Id id, int i)
{
    switch (id) {
    case ID_FG_MODE: {
        static std::wstring s;
        const int mul = (i < 0 || i > 2) ? 4 : (i + 2);      // 2x / 3x / 4x
        const unsigned hz = (g_refresh > 1) ? g_refresh : 120;
        wchar_t b[96]{};
        if (hz % static_cast<unsigned>(mul) == 0) {
            _snwprintf_s(b, _TRUNCATE, T(L"%dx（%u→%u fps）", L"%dx (%u->%u fps)"), mul,
                         hz / static_cast<unsigned>(mul), hz);
        } else {
            _snwprintf_s(b, _TRUNCATE, T(L"%dx（%.1f→%u fps）", L"%dx (%.1f->%u fps)"), mul,
                         static_cast<double>(hz) / mul, hz);
        }
        s = b;
        return s.c_str();
    }
    case ID_FG_INPUT: return L"DLSSG（固定）";
    case ID_INI_PROFILE: return kOptIniProfile[(i % 2) == 0 ? 0 : 1];
    case ID_STREAM_PCT: return kOptStream[(i < 0 || i > 3) ? 1 : i];
    case ID_CODEC:   return kOptCodec[(i < 0 || i > 2) ? 2 : i];
    case ID_DEPTH_PCT:  return kOptDepthPct[(i < 0 || i > 2) ? 1 : i];
    case ID_UI_COMPOSE: return kOptUiCompose[(i % 2) == 0 ? 0 : 1];
    case ID_OUT:     return kOptOut[i % 3];
    case ID_SDR:     return kOptColor[i % 2];
    case ID_FOV:     return kOptFov[i % 4];
    case ID_IVL:     return kOptIvl[i % 6];
    case ID_UI:      return kOptUi[i % 2];
    case ID_RT:      return kOptRt[i % 2];
    case ID_HOTKEY:  return g_hk_custom ? L"自定义" : g_hotkeys[i % kHotkeyCount].name;
    case ID_LOCK:    return kOptLock[i % 5];
    case ID_UISCALE: return kOptUiScale[i % 7];
    case ID_LANG:    return (i % 2) ? L"English" : L"中文";
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

// 固定倍率没有互斥控件：2x/3x/4x 三选一，其余行始终可用。
static bool row_enabled(Id) { return true; }

static int opt_get(Id id)
{
    switch (id) {
    case ID_FG_MODE: return S().fg_mode;
    case ID_FG_INPUT: return 0;
    case ID_INI_PROFILE: return S().ini_full ? 1 : 0;
    case ID_CODEC:   return (S().codec < 0 || S().codec > 2) ? 2 : S().codec;
    case ID_DEPTH_PCT: {
        for (int i = 0; i < 3; ++i) {
            if (kDepthPct[i] == S().depth_pct) { return i; }
        }
        return 1;
    }
    case ID_UI_COMPOSE: return S().ui_compose ? 1 : 0;
    case ID_STREAM_PCT: {
        for (int i = 0; i < 4; ++i) {
            if (kStreamPct[i] == S().stream_pct) { return i; }
        }
        return 1;
    }
    case ID_OUT:     return S().out_mode;
    case ID_SDR:     return S().sdr ? 1 : 0;
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
    case ID_UISCALE: {
        for (int i = 0; i < 7; ++i) {
            if (kUiScalePct[i] == g_scale_pct) { return i; }
        }
        return 0;
    }
    case ID_LANG: return (g_lang != 0) ? 1 : 0;
    default: return 0;
    }
}


// Caption shown on the right of a row.
static bool target_game_ini(std::wstring* out);
static std::wstring exe_dir();
static bool exe_file_exists(const wchar_t* name);
static bool game_runtime_ok(const std::wstring& dir);
static bool file_in_dir(const std::wstring& dir, const wchar_t* name);
static std::wstring row_value(Id id)
{
    wchar_t buf[128]{};
    switch (id) {
    case ID_FG_TYPE: return L"XeFG (libxess_fg)";
    case ID_CAP_API: return L"WGC";
    case ID_STREAM_INFO:   return L"按 fakerFG.ini 的 StreamPercent";
    case ID_UPSAMPLE_INFO: return L"→ 输入画面尺寸（默认开）";
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
    case ID_FG_MODE: return opt_text(ID_FG_MODE, S().fg_mode);
    case ID_FG_INPUT: return opt_text(ID_FG_INPUT, S().fg_input);
    case ID_FG_INI: {
        // 优先用游戏库选中的目录（它才是部署目标），没有再看前台窗口。
        if (!g_game_dir.empty()) { return g_game_dir + L"\\fakerFG.ini"; }
        std::wstring ini;
        return target_game_ini(&ini) ? ini : std::wstring(L"(先选择游戏窗口)");
    }
    case ID_VERSION:
        _snwprintf_s(buf, _TRUNCATE, T(L"%s（协议 v%u）", L"%s (protocol v%u)"),
                     LOCK_FRAME_VERSION_WSTRING, mgpu::kSharedVersion);
        return buf;
    case ID_CHECK_GPU:
        if (g_adapters.size() >= 2) {
            _snwprintf_s(buf, _TRUNCATE, T(L"%d 块（OK）", L"%d GPUs (OK)"),
                         (int)g_adapters.size());
        } else {
            _snwprintf_s(buf, _TRUNCATE, T(L"%d 块（需要双卡）", L"%d GPU (two recommended)"),
                         (int)g_adapters.size());
        }
        return buf;
    case ID_CHECK_RUNTIME: {
        const bool fg = exe_file_exists(L"libxess_fg.dll");
        const bool xell = exe_file_exists(L"libxell.dll");
        if (!fg && !xell) { return L"缺少 libxess_fg.dll / libxell.dll"; }
        if (!fg) { return L"缺少 libxess_fg.dll"; }
        if (!xell) { return L"缺少 libxell.dll"; }
        // 运行时还得在游戏目录的 fakerFG\ 里（XeFG 是在游戏进程里加载的，
        // 游戏自带那份可能缺导出）。
        if (!g_game_dir.empty() && !game_runtime_ok(g_game_dir)) {
            return L"游戏目录 fakerFG 里缺少 libxess_fg.dll（点[重新应用配置]自动写入）";
        }
        return L"libxess_fg + libxell OK";
    }
    case ID_CHECK_GAME: {
        std::wstring ini;
        if (!g_game_dir.empty()) {
            ini = g_game_dir + L"\\fakerFG.ini";
        } else if (!target_game_ini(&ini)) {
            return L"未选择游戏或缺少 fakerFG.ini";
        }
        if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return L"缺少 fakerFG.ini";
        }
        wchar_t out[32]{};
        GetPrivateProfileStringW(L"fakerFG", L"FgOutput", L"", out, 32, ini.c_str());
        if (_wcsicmp(out, L"Capture") != 0) { return L"FgOutput 不是 Capture"; }
        const int pass = (int)GetPrivateProfileIntW(L"fakerFG", L"Passthrough", 0, ini.c_str());
        if (pass != 0) { return L"Passthrough=1（应为 0）"; }
        wchar_t in[32]{};
        GetPrivateProfileStringW(L"fakerFG", L"FgInput", L"", in, 32, ini.c_str());
        if (in[0] == 0) { return L"FgInput 为空"; }
        if (_wcsicmp(in, L"DLSSG") != 0) { return L"FgInput 不是 DLSSG（本版只支持 DLSSG）"; }
        return L"fakerFG.ini OK";
    }
    case ID_CHECK_SHA: return g_sha_text;
    case ID_OUTFPS:
        // 手输的数字框：0 表示"面板刷新"（不限制），其余是 Hz 上限。
        if (S().out_fps == 0) { return L"面板刷新"; }
        _snwprintf_s(buf, _TRUNCATE, L"%d Hz", S().out_fps);
        return buf;
    case ID_POOL:   _snwprintf_s(buf, _TRUNCATE, L"%d", S().pool);    return buf;
    case ID_LAT:    _snwprintf_s(buf, _TRUNCATE, L"%d", S().latency); return buf;
    case ID_HOTKEY: return g_capture_hotkey ? std::wstring(L"按下新组合键...") : hotkey_name();
    case ID_LOCK: {
        const unsigned hz = (g_refresh > 1) ? g_refresh : 120;
        const int mul = S().fg_mode + 2;
        const double in = static_cast<double>(hz) / static_cast<double>(mul);
        if (hz % static_cast<unsigned>(mul) != 0) {
            _snwprintf_s(buf, _TRUNCATE, T(L"锁 %.1f fps（%dx，不能整除）",
                                           L"lock %.1f fps (%dx, not divisible)"), in, mul);
        } else {
            _snwprintf_s(buf, _TRUNCATE, T(L"锁 %.1f fps（%dx -> %u fps）",
                                           L"lock %.1f fps (%dx -> %u fps)"), in, mul, hz);
        }
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

    const std::wstring path = exe_dir() + L"\\lock_frame_ui.log";
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { return; }
    DWORD wrote = 0;
    WriteFile(f, line, static_cast<DWORD>(strlen(line)), &wrote, nullptr);
    CloseHandle(f);
}

// Kill any lock_frame.exe we may have lost track of (stale handle, crashed UI state, ...).
static int kill_stray_bridge()
{
    int killed = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { return 0; }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"lock_frame.exe") == 0) {
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

// File next to lock_frame_ui.exe (the consumer also loads libxess/libxell from there).
static bool exe_file_exists(const wchar_t* name)
{
    if (name == nullptr) { return false; }
    const std::wstring p = exe_dir() + L"\\" + name;
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 目标进程里是否加载了某个模块。用来区分"装了一半没进游戏"和"游戏已经在跑但
// producer 还没挂上" —— 后者几乎总是因为游戏里没开 DLSS 帧生成。
static bool proc_has_module(DWORD pid, const wchar_t* name)
{
    if (pid == 0 || name == nullptr) { return false; }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) { return false; }
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, name) == 0) { found = true; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}


// Keep the folder bounded: the consumer prints a line per report window and the UI
// writes every run to lock_frame_ui_HHMMSS.txt. Keep the newest 8 run logs and drop
// the rest; also drop lock_frame_ui.log when it grows past 4 MiB.
static void prune_old_logs()
{
    struct Ent { ULONGLONG t; std::wstring path; };
    std::vector<Ent> logs;
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = exe_dir() + L"\\lock_frame_ui_*.txt";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { continue; }
            ULARGE_INTEGER u{};
            u.LowPart = fd.ftLastWriteTime.dwLowDateTime;
            u.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            logs.push_back(Ent{ u.QuadPart, exe_dir() + L"\\" + fd.cFileName });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(logs.begin(), logs.end(),
              [](const Ent& a, const Ent& b) { return a.t > b.t; });
    for (size_t i = 8; i < logs.size(); ++i) { DeleteFileW(logs[i].path.c_str()); }

    const std::wstring ui_log = exe_dir() + L"\\lock_frame_ui.log";
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(ui_log.c_str(), GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER u{};
        u.LowPart = fad.nFileSizeLow;
        u.HighPart = fad.nFileSizeHigh;
        if (u.QuadPart > (4ull << 20)) { DeleteFileW(ui_log.c_str()); }
    }
}

static std::wstring ini_path() { return exe_dir() + L"\\lock_frame_ui.ini"; }

// 新文件先写 UTF-16LE BOM：这样 WritePrivateProfileStringW 会保持 Unicode，
// 中文游戏名在非中文系统上也不会变成乱码。
static void ensure_utf16_bom(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) { return; }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return; }
    const unsigned char bom[2] = { 0xFF, 0xFE };
    DWORD w = 0;
    WriteFile(h, bom, 2, &w, nullptr);
    CloseHandle(h);
}

// save_state/load_state 期间临时指向某个游戏配置文件；结束后恢复。
static const wchar_t* g_ini_file_override = nullptr;

// file 为空 = 主配置文件；否则读写指定的游戏配置文件（profiles\*.ini）。
static void ini_w(const std::wstring& sec, const wchar_t* key, int v,
                  const wchar_t* file = nullptr)
{
    wchar_t buf[32]{};
    _snwprintf_s(buf, _TRUNCATE, L"%d", v);
    WritePrivateProfileStringW(sec.c_str(), key, buf,
        file ? file : (g_ini_file_override ? g_ini_file_override : ini_path().c_str()));
}

static void ini_ws(const std::wstring& sec, const wchar_t* key, const std::wstring& v,
                   const wchar_t* file = nullptr)
{
    WritePrivateProfileStringW(sec.c_str(), key, v.c_str(),
        file ? file : (g_ini_file_override ? g_ini_file_override : ini_path().c_str()));
}

static int ini_r(const std::wstring& sec, const wchar_t* key, int def,
                 const wchar_t* file = nullptr)
{
    return (int)GetPrivateProfileIntW(sec.c_str(), key, def,
        file ? file : (g_ini_file_override ? g_ini_file_override : ini_path().c_str()));
}

static std::wstring ini_rs(const std::wstring& sec, const wchar_t* key, const wchar_t* def,
                           const wchar_t* file = nullptr)
{
    wchar_t buf[512]{};
    GetPrivateProfileStringW(sec.c_str(), key, def, buf, 512,
        file ? file : (g_ini_file_override ? g_ini_file_override : ini_path().c_str()));
    return buf;
}

// 日志缓冲：写文件仍由 reader_thread 负责；这里保留最近 400 行给查看器。
static void log_line_wide(const std::wstring& w)
{
    if (!g_line_cs_ready) { return; }
    EnterCriticalSection(&g_line_cs);
    g_log_lines.push_back(w);
    while (g_log_lines.size() > 400) { g_log_lines.pop_front(); }
    LeaveCriticalSection(&g_line_cs);
}

static void log_line_utf8(const std::string& s)
{
    wchar_t w[1024]{};
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w, 1023);
    log_line_wide(w);
}

static void set_msg(const wchar_t* m)
{
    g_msg = m;
    g_msg_until = GetTickCount() + 4000;
    log_line_wide(std::wstring(L"[ui] ") + m);
    if (g_hwnd) { InvalidateRect(g_hwnd, nullptr, FALSE); }
}


// ------------------------------------------------- fakerFG.ini（游戏侧配置）
// 输入源是 fakerFG 的配置，不是 consumer 参数。UI 只改游戏目录里现有的
// fakerFG.ini，并在写之前备份；配置文件不存在时不去猜路径/造一份半配置。
static HWND config_target_window()
{
    if (S().target_mode == 1 && S().win_hwnd && IsWindow(S().win_hwnd)) {
        return S().win_hwnd;
    }
    if (g_win_sel >= 0 && g_win_sel < (int)g_wins.size()) {
        return g_wins[g_win_sel].hwnd;
    }
    HWND fg = GetForegroundWindow();
    return (fg != nullptr && fg != g_hwnd) ? fg : nullptr;
}

// The refresh rate of the monitor the selected window is on, not the primary
// monitor. Multi-monitor (game on a 144 Hz screen, desktop on 60 Hz) otherwise
// shows the wrong "lock input" number.
static UINT refresh_for_window(HWND h)
{
    if (h == nullptr) { return g_refresh; }
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
            dm.dmDisplayFrequency > 1) {
            return dm.dmDisplayFrequency;
        }
    }
    return g_refresh;
}

static bool target_game_ini(std::wstring* out)
{
    if (out == nullptr) { return false; }
    HWND h = config_target_window();
    if (h == nullptr) { return false; }
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == 0) { return false; }
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p == nullptr) { return false; }
    wchar_t exe[MAX_PATH]{};
    DWORD n = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(p, 0, exe, &n);
    CloseHandle(p);
    if (!ok || n == 0) { return false; }
    std::wstring path(exe, n);
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) { return false; }
    *out = path.substr(0, slash) + L"\\fakerFG.ini";
    return true;
}

// --------------------------------------- 游戏侧安装：一键部署 / 一键还原（P0）
// 原来只有 deploy_game.bat。小白不会开命令行、也不会手打游戏目录，所以把同一套
// 文件操作搬进 UI：选 exe -> 自动备份 -> 复制 -> 报告每一步。幂等，重复点安全。
static bool file_in_dir(const std::wstring& dir, const wchar_t* name)
{
    if (dir.empty() || name == nullptr) { return false; }
    return GetFileAttributesW((dir + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// XeFG runs inside the game process and loads its runtime from
//   <game>\fakerFG\libxess_fg.dll  ->  <game>\libxess_fg.dll  ->  search path   (xefg_dyn.h)
// Only OUR copy counts here. A title's own libxess_fg.dll is "found" but can be a build
// that lacks xefgSwapChainD3D12GetProperties (Cyberpunk 2077, 2026-11-20) - then XeFG
// never takes the swap chain, producer_present() never runs and the MV/depth transport
// dies with it. Existence of <game>\libxess_fg.dll therefore proves nothing; existence of
// fakerFG\libxess_fg.dll proves the packaged runtime is the one that will be loaded.
static bool game_runtime_ok(const std::wstring& dir)
{
    if (dir.empty()) { return false; }
    return file_in_dir(dir, L"fakerFG\\libxess_fg.dll");
}

static std::wstring game_dir_for_window(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) { return L""; }
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p == nullptr) { return L""; }
    wchar_t exe[MAX_PATH]{};
    DWORD n = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(p, 0, exe, &n);
    CloseHandle(p);
    if (!ok || n == 0) { return L""; }
    std::wstring path(exe, n);
    const size_t slash = path.find_last_of(L'\\');
    return (slash == std::wstring::npos) ? std::wstring(L"") : path.substr(0, slash);
}

static bool faker_ini_ok(const std::wstring& dir)
{
    const std::wstring ini = dir + L"\\fakerFG.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) { return false; }
    wchar_t out[32]{};
    GetPrivateProfileStringW(L"fakerFG", L"FgOutput", L"", out, 32, ini.c_str());
    if (_wcsicmp(out, L"Capture") != 0) { return false; }
    if ((int)GetPrivateProfileIntW(L"fakerFG", L"Passthrough", 0, ini.c_str()) != 0) {
        return false;
    }
    wchar_t in[32]{};
    GetPrivateProfileStringW(L"fakerFG", L"FgInput", L"", in, 32, ini.c_str());
    return in[0] != 0;
}

static bool pick_game_file(std::wstring* exe_out)
{
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"游戏程序 (*.exe)\0*.exe\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择游戏的 exe";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) { return false; }
    *exe_out = file;
    return exe_out->find(L'\\') != std::wstring::npos;
}

static bool pick_game_exe(std::wstring* dir_out)
{
    std::wstring path;
    if (!pick_game_file(&path)) { return false; }
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) { return false; }
    *dir_out = path.substr(0, slash);
    return true;
}

static const wchar_t* fg_input_ini_value(int i)
{
    return kFgInputIni[(i < 0 || i > 3) ? 0 : i];
}

static void read_game_ini_state(const std::wstring& ini)
{
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) { return; }
    wchar_t v[32]{};
    GetPrivateProfileStringW(L"fakerFG", L"FgInput", L"", v, 32, ini.c_str());
    for (int i = 0; i < 4; ++i) {
        if (_wcsicmp(v, fg_input_ini_value(i)) == 0) { S().fg_input = i; break; }
    }
    // This build only implements the DLSSG lane; the UI never offers another source.
    S().fg_input = 0;

    int pct = (int)GetPrivateProfileIntW(L"fakerFG", L"StreamPercent", 150, ini.c_str());
    wchar_t mv[16]{};
    GetPrivateProfileStringW(L"fakerFG", L"StreamPercentMv", L"", mv, 16, ini.c_str());
    if (mv[0] != 0) { pct = _wtoi(mv); }
    if (pct < 100) { pct = 100; }
    if (pct > 400) { pct = 400; }
    int best = 150;
    int best_d = 0x7fffffff;
    for (int i = 0; i < 4; ++i) {
        const int d = (kStreamPct[i] > pct) ? (kStreamPct[i] - pct) : (pct - kStreamPct[i]);
        if (d < best_d) { best_d = d; best = kStreamPct[i]; }
    }
    S().stream_pct = best;
    S().fg_log = (GetPrivateProfileIntW(L"fakerFG", L"Log", 1, ini.c_str()) != 0);
    // 1.2.9: reflect the file's codec resolution / UI composition so the dropdowns show what
    // the game will actually load. Missing keys mean "not configured yet": depth defaults to
    // 150 (the 1.2.8+ default) and UI composition to ON (the 1.3.0+ default). An older ini
    // that still carries UiStream=0 stays off.
    {
        const int dpct = (int)GetPrivateProfileIntW(L"fakerFG", L"DepthCodecPercent", 150,
                                                    ini.c_str());
        S().depth_pct = (dpct == 100 || dpct == 200) ? dpct : 150;
        S().ui_compose = GetPrivateProfileIntW(L"fakerFG", L"UiPatch", 0, ini.c_str()) != 0;
    }
    // The full test template carries keys the release one does not; use one of them to
    // show the right "配置版本" entry instead of trusting the remembered state.
    {
        wchar_t probe[16]{};
        GetPrivateProfileStringW(L"fakerFG", L"UiSynthDump", L"", probe, 16, ini.c_str());
        S().ini_full = probe[0] != 0;
    }
}

// 目录里是否有进程正在运行（覆盖最小化到托盘、窗口被隐藏的游戏）。
// 只看可执行文件路径：dir 本身或它的子目录都算，避免把同名前缀目录误判。
static bool dir_has_process(const std::wstring& dir)
{
    if (dir.empty()) { return false; }
    const std::wstring k = norm_path(dir);
    if (k.empty()) { return false; }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { return false; }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    const DWORD self = GetCurrentProcessId();
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self) { continue; }
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (p == nullptr) { continue; }
            wchar_t buf[1024]{};
            DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
            if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
                const std::wstring exe = norm_path(buf);
                if (exe.size() > k.size() + 1 && exe.compare(0, k.size(), k) == 0 &&
                    exe[k.size()] == L'\\') {
                    found = true;
                }
            }
            CloseHandle(p);
            if (found) { break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// 这个目录的游戏是否正在运行：先看窗口，再看进程路径（托盘/隐藏窗口）。
static bool game_dir_running(const std::wstring& dir)
{
    if (dir.empty()) { return false; }
    rescan_windows();
    GameEntry* e = find_game_mut(dir);
    const std::wstring k = norm_path(dir);
    for (const WinEntry& w : g_wins) {
        const std::wstring d = game_dir_for_window(w.hwnd);
        if (!d.empty() && norm_path(d) == k) { return true; }
    }
    // 用安装根目录判断，子目录里的 exe 也能覆盖到（Steam 的 Binaries/Win64 之类）。
    if (e != nullptr && !e->root.empty()) { return dir_has_process(e->root); }
    return dir_has_process(dir);
}

static std::wstring game_ini_path()
{
    if (!g_game_dir.empty()) { return g_game_dir + L"\\fakerFG.ini"; }
    std::wstring ini;
    return target_game_ini(&ini) ? ini : std::wstring();
}

// 只读取关键三项，用来判断"文件被人改过、和保存的配置不一致"。
struct IniCfg { bool valid = false; int fg_input = 0; int pct = 150; bool log = true; };
static IniCfg read_ini_cfg(const std::wstring& ini)
{
    IniCfg c;
    if (ini.empty() || GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) { return c; }
    c.valid = true;
    wchar_t v[32]{};
    GetPrivateProfileStringW(L"fakerFG", L"FgInput", L"", v, 32, ini.c_str());
    for (int i = 0; i < 4; ++i) {
        if (_wcsicmp(v, fg_input_ini_value(i)) == 0) { c.fg_input = i; break; }
    }
    int pct = (int)GetPrivateProfileIntW(L"fakerFG", L"StreamPercent", 150, ini.c_str());
    wchar_t mv[16]{};
    GetPrivateProfileStringW(L"fakerFG", L"StreamPercentMv", L"", mv, 16, ini.c_str());
    if (mv[0] != 0) { pct = _wtoi(mv); }
    if (pct < 100) { pct = 100; }
    if (pct > 400) { pct = 400; }
    int best = 150, best_d = 0x7fffffff;
    for (int i = 0; i < 4; ++i) {
        const int d = (kStreamPct[i] > pct) ? (kStreamPct[i] - pct) : (pct - kStreamPct[i]);
        if (d < best_d) { best_d = d; best = kStreamPct[i]; }
    }
    c.pct = best;
    c.log = (GetPrivateProfileIntW(L"fakerFG", L"Log", 1, ini.c_str()) != 0);
    return c;
}

// 每个游戏目录单独备份 fakerFG.ini，只保留最新 8 份。
static void prune_ini_backups(const std::wstring& ini)
{
    const size_t slash = ini.find_last_of(L'\\');
    const std::wstring prefix = (slash == std::wstring::npos) ? std::wstring()
                                                             : ini.substr(0, slash + 1);
    struct Ent { std::wstring path; unsigned long long tick; };
    std::vector<Ent> v;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((ini + L".bak_*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { continue; }
            unsigned long long t = 0;
            const wchar_t* p = wcsstr(fd.cFileName, L".bak_");
            if (p != nullptr) { t = _wcstoui64(p + 5, nullptr, 10); }
            v.push_back(Ent{ prefix + fd.cFileName, t });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(v.begin(), v.end(),
              [](const Ent& a, const Ent& b) { return a.tick > b.tick; });
    for (size_t i = 8; i < v.size(); ++i) { DeleteFileW(v[i].path.c_str()); }
}

static void apply_fg_input()
{
    const std::wstring ini = game_ini_path();
    if (ini.empty()) {
        set_msg(T(L"先选择游戏窗口或游戏库条目，才能写入 fakerFG.ini",
                  L"Select a game window or library entry first"));
        return;
    }
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        set_msg(T(L"游戏目录没有 fakerFG.ini：请先部署游戏侧文件",
                  L"No fakerFG.ini in the game folder: deploy the game-side files first"));
        uilog("fakerfg: %ls missing", ini.c_str());
        return;
    }

    wchar_t bak[512]{};
    _snwprintf_s(bak, _TRUNCATE, L"%s.bak_%lu", ini.c_str(),
                 (unsigned long)GetTickCount());
    if (!CopyFileW(ini.c_str(), bak, FALSE)) {
        uilog("fakerfg: backup failed err=%lu", (unsigned long)GetLastError());
    } else {
        prune_ini_backups(ini);
    }

    wchar_t pct[16]{};
    _snwprintf_s(pct, _TRUNCATE, L"%d", S().stream_pct);
    bool ok = true;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"FgInput",
                                          L"DLSSG", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"FgOutput", L"Capture", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"Passthrough", L"0", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"RealSwapchainObject", L"1", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"StreamPercent", pct, ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"StreamPercentMv", pct, ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"StreamPercentDepth", pct, ini.c_str()) != 0;
    // Protocol v30 lossless transport: 0 / 1 / "auto". Producer reads it once per session.
    const wchar_t* codec_ini = kCodecIni[(S().codec < 0 || S().codec > 2) ? 2 : S().codec];
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"MvCodec", codec_ini, ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"DepthCodec", codec_ini, ini.c_str()) != 0;
    // 1.2.9: per-stream codec resolution + UI composition. MV stays 1:1 (bit-exact); only
    // depth is allowed to travel smaller, because it is the stream that costs the most
    // (2.05 of the 3.5 ms render-card codec time at 4K) and depth resolution is documented
    // as insensitive (FINAL_CONFIG 2.1.2).
    wchar_t dpct[16]{};
    _snwprintf_s(dpct, _TRUNCATE, L"%d", S().depth_pct);
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"CodecStreamPercent", L"100", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"MvCodecPercent", L"100", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"DepthCodecPercent", dpct, ini.c_str()) != 0;
    // UI composition (default ON since 1.3.0): producer HUDless patch -> consumer
    // reconstruction -> XeFG mode 4. Stream 3 / mode 5 is sealed off to avoid the extra
    // UI encode/decode; the producer reads these once per session.
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"UiStream", L"0", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"UiSource", L"Off", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"UiPatch",
                                          S().ui_compose ? L"1" : L"0", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"UiPatchWhiteNits", L"80", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"UiFullRes", L"1", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"UiCodec",
                                          S().ui_compose ? L"1" : L"0", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"UiSparseCodec", L"1", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"UiAlpha8", L"0", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"UiDilate", L"0", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"UiSynthDump", L"0", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"UiSlots", L"2", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"HudlessStream",
                                          S().ui_compose ? L"1" : L"0", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"StreamPercentUi", L"100", ini.c_str()) != 0;
    ok = ok && WritePrivateProfileStringW(L"fakerFG", L"Log", S().fg_log ? L"1" : L"0",
                                          ini.c_str()) != 0;
    if (!ok) {
        set_msg(T(L"写入 fakerFG.ini 失败（只读或权限不足）",
                  L"Failed to write fakerFG.ini (read-only or permission denied)"));
        uilog("fakerfg: write failed ini=%ls", ini.c_str());
        return;
    }

    set_msg(T(L"已写入 fakerFG.ini（DLSSG / 码流分辨率 / UI 合成 / Log），重启游戏生效",
              L"fakerFG.ini written (DLSSG / codec resolution / UI composition / Log); restart to apply"));
    uilog("fakerfg: wrote %ls input=DLSSG pct=%d codec=%ls depthpct=%d ui=%d log=%d", ini.c_str(),
          S().stream_pct,
          kCodecIni[(S().codec < 0 || S().codec > 2) ? 2 : S().codec],
          S().depth_pct, S().ui_compose ? 1 : 0, S().fg_log ? 1 : 0);
}

// 切换游戏侧配置版本：备份现有 fakerFG.ini，再写入精简发布模板或完整测试模板。
// producer 每个会话只读一次 ini，所以写完必须重启游戏才生效。
static void apply_ini_profile()
{
    const std::wstring ini = game_ini_path();
    if (ini.empty()) {
        set_msg(T(L"先选择游戏窗口或游戏库条目，才能写入 fakerFG.ini",
                  L"Select a game window or library entry first"));
        return;
    }
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        set_msg(T(L"游戏目录没有 fakerFG.ini：请先部署游戏侧文件",
                  L"No fakerFG.ini in the game folder: deploy the game-side files first"));
        uilog("ini: %ls missing", ini.c_str());
        return;
    }
    const wchar_t* tmpl = S().ini_full ? L"fakerFG.full.ini" : L"fakerFG.ini";
    if (!exe_file_exists(tmpl)) {
        set_msg(T(L"发布包缺少游戏侧配置模板（fakerFG.ini / fakerFG.full.ini）",
                  L"Game-side ini template is missing from the package (fakerFG.ini / fakerFG.full.ini)"));
        uilog("ini: %ls missing in the package", tmpl);
        return;
    }

    wchar_t bak[512]{};
    _snwprintf_s(bak, _TRUNCATE, L"%s.bak_%lu", ini.c_str(),
                 (unsigned long)GetTickCount());
    if (!CopyFileW(ini.c_str(), bak, FALSE)) {
        uilog("ini: backup failed err=%lu", (unsigned long)GetLastError());
    } else {
        prune_ini_backups(ini);
    }

    const std::wstring src = exe_dir() + L"\\" + tmpl;
    if (!CopyFileW(src.c_str(), ini.c_str(), FALSE)) {
        set_msg(T(L"写入 fakerFG.ini 失败（游戏是否在运行？目录是否只读？）",
                  L"Failed to write fakerFG.ini (game running? read-only?)"));
        uilog("ini: copy %ls failed err=%lu", tmpl, (unsigned long)GetLastError());
        return;
    }
    read_game_ini_state(ini);
    set_msg(S().ini_full
                ? T(L"已写入完整测试配置（fakerFG.full.ini）；重启游戏生效",
                    L"Full test config written (fakerFG.full.ini); restart the game")
                : T(L"已写入精简发布配置（fakerFG.ini）；重启游戏生效",
                    L"Simple release config written (fakerFG.ini); restart the game"));
    uilog("ini: profile=%ls -> %ls", tmpl, ini.c_str());
}

// 从指定目录的 fakerFG.ini 读回 FgInput（不依赖游戏是否在跑）。
static void sync_fg_input_from_dir(const std::wstring& dir)
{
    read_game_ini_state(dir + L"\\fakerFG.ini");
}

// Reflect the file on disk when the user picks another game window; the dropdown
// then shows what the game will actually load, not just the last UI selection.
static void sync_fg_input_from_target()
{
    std::wstring ini;
    if (target_game_ini(&ini)) { read_game_ini_state(ini); }
}


// 部署是“权威覆盖”：先删目标再复制。这样一次失败的复制不会留下旧版本的 DLL，
// 也就不会出现 producer / consumer / fakerFG 协议版本混用（“协议不同”）。
static bool force_delete_file(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) { return true; }
    if (attr & FILE_ATTRIBUTE_READONLY) {
        SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
    }
    return DeleteFileW(path.c_str()) != 0;
}

static bool force_copy_file(const std::wstring& src, const std::wstring& dst)
{
    if (!force_delete_file(dst)) { return false; }
    return CopyFileW(src.c_str(), dst.c_str(), FALSE) != 0;
}

// 安装到 <dir>：与 deploy_game.bat 等价，但把每一步结果返回给 UI。
static bool deploy_game_dir(const std::wstring& dir, std::wstring* out)
{
    std::wstring log;
    auto line = [&](const wchar_t* s) { log += L"  "; log += s; log += L"\r\n"; };
    auto fail = [&](const wchar_t* s) -> bool {
        log += L"  [失败] "; log += s; log += L"\r\n";
        if (out) { *out = log; }
        return false;
    };

    if (dir.empty() || GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return fail(L"游戏目录不存在");
    }
    if (!exe_file_exists(L"fakerFG.dll")) {
        return fail(L"发布包目录里没有 fakerFG.dll（请使用完整的发布压缩包）");
    }

    const std::wstring src = exe_dir();
    const std::wstring dll = src + L"\\fakerFG.dll";
    const std::wstring dxgi = dir + L"\\dxgi.dll";
    const std::wstring dxgi_bak = dir + L"\\dxgi.dll.bak";
    const std::wstring dlssg = dir + L"\\nvngx_dlssg.dll";
    const std::wstring dlssg_real = dir + L"\\nvngx_dlssg_real.dll";

    // 第一次部署才备份游戏自己的 dxgi.dll。已经装过（fakerFG.dll 存在）时不能再把
    // 我们的 dxgi.dll 备份成 .bak，否则还原就还原成我们自己的文件了。
    if (!file_in_dir(dir, L"fakerFG.dll") && file_in_dir(dir, L"dxgi.dll") &&
        !file_in_dir(dir, L"dxgi.dll.bak")) {
        if (!MoveFileW(dxgi.c_str(), dxgi_bak.c_str())) {
            return fail(L"备份原 dxgi.dll 失败（游戏是否正在运行？目录是否只读？）");
        }
        line(L"已备份游戏原 dxgi.dll -> dxgi.dll.bak");
    }
    // 第一次安装才移动游戏自己的 nvngx_dlssg.dll；重装时 nvngx_dlssg.dll 已经是我们的
    // 别名，不能再把它当成“游戏原文件”存成 _real（那会在还原时换回旧组件）。
    if (!file_in_dir(dir, L"fakerFG.dll") && file_in_dir(dir, L"nvngx_dlssg.dll") &&
        !file_in_dir(dir, L"nvngx_dlssg_real.dll")) {
        if (MoveFileW(dlssg.c_str(), dlssg_real.c_str())) {
            line(L"已保留游戏原 DLSS-G -> nvngx_dlssg_real.dll");
        }
    }

    // dlssg_sm86 用 version.dll 抢 nvngx_dlssg.dll；它和我们的别名/协议不兼容。
    // 禁用一次（改名），还原时再换回去。
    if (file_in_dir(dir, L"version.dll") && !file_in_dir(dir, L"version.dll.bak_mgpu")) {
        if (MoveFileW((dir + L"\\version.dll").c_str(),
                      (dir + L"\\version.dll.bak_mgpu").c_str())) {
            line(L"已禁用 version.dll（dlssg_sm86）-> version.dll.bak_mgpu");
        } else {
            line(L"[注意] version.dll 禁用失败（游戏是否在运行？）");
        }
    }

    // 全部权威覆盖：先删后拷，避免旧版本 DLL 残留导致协议/加载器状态不一致。
    const wchar_t* aliases[] = { L"dxgi.dll", L"nvngx_dlssg.dll", L"fakerFG.dll" };
    for (const wchar_t* name : aliases) {
        if (!force_copy_file(dll, dir + L"\\" + name)) {
            return fail(L"写入 fakerFG.dll 失败（游戏是否正在运行？目录是否只读？）");
        }
    }
    line(L"已覆盖 fakerFG.dll（dxgi.dll / nvngx_dlssg.dll 两个别名）");

    if (exe_file_exists(L"xess_mfg_producer.dll")) {
        if (force_copy_file(src + L"\\xess_mfg_producer.dll",
                            dir + L"\\xess_mfg_producer.dll")) {
            line(L"已覆盖 xess_mfg_producer.dll");
        } else {
            line(L"[注意] xess_mfg_producer.dll 写入失败");
        }
    } else {
        line(L"[注意] 发布包里没有 xess_mfg_producer.dll");
    }

    // Intel 运行时必须进**游戏目录**：XeFG 在游戏进程里加载它（先找 <游戏>\fakerFG\，
    // 再找 <游戏>\ 本身 —— 见 src/xefg_dyn.h）。只放在 UI 旁边的 1.2.0 打包方式会让
    // 游戏加载到它自己那份旧 DLL（LoadLibrary 成功、但缺 xefgSwapChainD3D12GetProperties），
    // XeFG 拿不到 swap chain ⇒ producer_present() 从不执行 ⇒ 共享内存不创建，
    // UI 报 “target ... is not the producer (producer_pid=0)”。放在我们自己的子目录，
    // 不覆盖游戏自带文件。
    {
        const std::wstring rt_dir = dir + L"\\fakerFG";
        CreateDirectoryW(rt_dir.c_str(), nullptr);   // 已存在时失败无妨
        const wchar_t* runtime[] = { L"libxess_fg.dll", L"libxell.dll" };
        int want = 0, copied = 0;
        for (const wchar_t* name : runtime) {
            if (!exe_file_exists(name)) { continue; }
            ++want;
            if (force_copy_file(src + L"\\" + name, rt_dir + L"\\" + name)) {
                ++copied;
            }
        }
        wchar_t b[360]{};
        if (copied > 0) {
            _snwprintf_s(b, _TRUNCATE,
                         L"已写入 Intel 运行时 %d/%d 个到 %s（XeFG 需要，producer 靠它启动）",
                         copied, want, rt_dir.c_str());
            line(b);
            if (copied < want) {
                line(L"[注意] 有运行时文件没复制成功（游戏是否正在运行？）");
            }
        } else if (want > 0) {
            line(L"[注意] Intel 运行时复制失败：XeFG 无法接管，producer 不会启动");
        } else {
            line(L"[注意] 发布包里没有 libxess_fg.dll / libxell.dll：游戏侧 XeFG 无法接管");
        }
    }

    // fakerFG.ini 也权威覆盖：旧版本的键（尤其已改名的写入项）留着会让下一次读取
    // 得到混合配置。先备份一份用户文件，再用所选模板覆盖；安装完成后
    // apply_config_after_deploy() 会把当前 UI 配置重新写进去。
    {
        const wchar_t* tmpl = S().ini_full ? L"fakerFG.full.ini" : L"fakerFG.ini";
        const std::wstring ini = dir + L"\\fakerFG.ini";
        if (!exe_file_exists(tmpl)) {
            line(L"[注意] 发布包里没有 fakerFG.ini 模板，请手动放入");
        } else {
            if (file_in_dir(dir, L"fakerFG.ini")) {
                wchar_t bak[512]{};
                _snwprintf_s(bak, _TRUNCATE, L"%s.bak_%lu", ini.c_str(),
                             (unsigned long)GetTickCount());
                if (CopyFileW(ini.c_str(), bak, FALSE)) { prune_ini_backups(ini); }
            }
            if (force_copy_file(src + L"\\" + tmpl, ini)) {
                line(S().ini_full
                         ? L"已覆盖 fakerFG.ini（完整测试模板，旧文件已备份）"
                         : L"已覆盖 fakerFG.ini（精简发布模板，旧文件已备份）");
            } else {
                line(L"[注意] fakerFG.ini 写入失败（游戏是否在运行？目录是否只读？）");
            }
        }
    }

    line(L"完成。请重启游戏，进入实际画面后点[刷新窗口列表]。");
    if (out) { *out = log; }
    return true;
}

static bool undeploy_game_dir(const std::wstring& dir, std::wstring* out)
{
    std::wstring log;
    auto line = [&](const wchar_t* s) { log += L"  "; log += s; log += L"\r\n"; };

    if (dir.empty()) {
        if (out) { *out = L"还没有选择游戏目录"; }
        return false;
    }
    if (!file_in_dir(dir, L"fakerFG.dll")) {
        if (out) { *out = L"这个目录里没有 fakerFG.dll，看起来没装过（无需还原）"; }
        return false;
    }

    const wchar_t* ours[] = { L"nvngx_dlssg.dll", L"fakerFG.dll", L"xess_mfg_producer.dll" };
    for (const wchar_t* name : ours) {
        DeleteFileW((dir + L"\\" + name).c_str());
    }

    // 部署时写进 <游戏>\fakerFG\ 的 Intel 运行时也要一起清掉（那是我们自己的子目录，
    // 游戏原文不会在里面）。只删与发布包同尺寸的那份，避免把用户自己放进去的
    // 其他版本运行时误删；目录非空时 RemoveDirectory 也会失败，作为第二道保护。
    {
        const std::wstring rt_dir = dir + L"\\fakerFG";
        int removed = 0;
        const wchar_t* runtime[] = { L"libxess_fg.dll", L"libxell.dll" };
        for (const wchar_t* name : runtime) {
            const std::wstring dst = rt_dir + L"\\" + name;
            const std::wstring ours_path = exe_dir() + L"\\" + name;
            WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
            if (GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &a) &&
                GetFileAttributesExW(ours_path.c_str(), GetFileExInfoStandard, &b) &&
                a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow) {
                if (DeleteFileW(dst.c_str())) { ++removed; }
            }
        }
        if (removed > 0) {
            RemoveDirectoryW(rt_dir.c_str());
            line(L"已移除 fakerFG 子目录里的 Intel 运行时");
        }
    }

    // dxgi.dll 单独特判：有 .bak 才能安全删（随后会还原）；没有 .bak 时只有确认它
    // 就是我们放进去的别名（与发布包 fakerFG.dll 同尺寸）才删，避免把游戏的原始
    // dxgi.dll 当成我们的文件删掉。
    const bool have_bak = file_in_dir(dir, L"dxgi.dll.bak");
    bool dxgi_is_ours = false;
    {
        WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
        const std::wstring dxgi = dir + L"\\dxgi.dll";
        const std::wstring dll = exe_dir() + L"\\fakerFG.dll";
        if (GetFileAttributesExW(dxgi.c_str(), GetFileExInfoStandard, &a) &&
            GetFileAttributesExW(dll.c_str(), GetFileExInfoStandard, &b)) {
            dxgi_is_ours = (a.nFileSizeHigh == b.nFileSizeHigh) &&
                           (a.nFileSizeLow == b.nFileSizeLow);
        }
    }
    if (have_bak || dxgi_is_ours) {
        DeleteFileW((dir + L"\\dxgi.dll").c_str());
        line(L"已移除 fakerFG.dll / dxgi.dll / nvngx_dlssg.dll / xess_mfg_producer.dll");
    } else {
        line(L"已移除 fakerFG.dll / nvngx_dlssg.dll / xess_mfg_producer.dll");
        line(L"[保留] dxgi.dll 不是本组件写入的（没有 dxgi.dll.bak），未删除");
    }

    if (file_in_dir(dir, L"dxgi.dll.bak")) {
        if (MoveFileW((dir + L"\\dxgi.dll.bak").c_str(), (dir + L"\\dxgi.dll").c_str())) {
            line(L"已还原游戏原 dxgi.dll");
        } else {
            line(L"[注意] dxgi.dll.bak 还原失败，请手动把 .bak 改回 dxgi.dll");
        }
    }
    if (file_in_dir(dir, L"nvngx_dlssg_real.dll")) {
        if (MoveFileW((dir + L"\\nvngx_dlssg_real.dll").c_str(),
                      (dir + L"\\nvngx_dlssg.dll").c_str())) {
            line(L"已还原游戏原 nvngx_dlssg.dll");
        }
    }
    if (file_in_dir(dir, L"version.dll.bak_mgpu")) {
        if (MoveFileW((dir + L"\\version.dll.bak_mgpu").c_str(),
                      (dir + L"\\version.dll").c_str())) {
            line(L"已还原 version.dll");
        } else {
            line(L"[注意] version.dll.bak_mgpu 还原失败");
        }
    }
    line(L"fakerFG.ini 已保留（里面可能改过 FgInput）。");
    if (out) { *out = log; }
    return true;
}

static void run_diagnostics()
{
    const std::wstring bat = exe_dir() + L"\\collect_diagnostics.bat";
    if (GetFileAttributesW(bat.c_str()) == INVALID_FILE_ATTRIBUTES) {
        set_msg(L"collect_diagnostics.bat 不在 exe 旁边，无法一键诊断");
        return;
    }
    ShellExecuteW(nullptr, L"open", bat.c_str(), nullptr, exe_dir().c_str(), SW_SHOWNORMAL);
    set_msg(L"正在生成诊断包（完成后在 exe 旁边，可整包发给人看）");
}

static void open_log_folder()
{
    ShellExecuteW(nullptr, L"open", exe_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    set_msg(L"已打开日志所在目录");
}

// 每 2 s 重算一次；简易模式和启动前检查共用同一份结论，避免两处说法不一致。
static void refresh_readiness()
{
    Ready r{};
    // 先按"选中的游戏"找它的窗口；找不到再退回手动选中的窗口。
    int wi = -1;
    if (!g_game_dir.empty()) {
        const std::wstring k = norm_path(g_game_dir);
        for (int i = 0; i < (int)g_wins.size(); ++i) {
            const std::wstring d = game_dir_for_window(g_wins[(size_t)i].hwnd);
            if (!d.empty() && norm_path(d) == k) { wi = i; break; }
        }
    }
    // 新模型里"窗口就绪"必须属于当前游戏；没选游戏时不再拿任意窗口充数。
    if (wi < 0) { r.window = false; }

    if (wi >= 0) {
        const WinEntry& we = g_wins[(size_t)wi];
        r.window = true;
        r.producer = proc_has_module(we.pid, L"xess_mfg_producer.dll");
        if (!g_user_picked && g_win_sel != wi) {
            g_win_sel = wi;
            S().win_hwnd = we.hwnd;
            S().win_title = we.title;
            S().target_mode = 1;
        }
        // 没有任何游戏被选中时，才从窗口反推目录；已选游戏不能被子窗口改掉。
        if (g_game_dir.empty()) {
            const std::wstring dir = game_dir_for_window(we.hwnd);
            if (!dir.empty() && (r.producer || file_in_dir(dir, L"fakerFG.dll"))) {
                g_game_dir = dir;
            }
        }
    }

    r.deployed = !g_game_dir.empty() && file_in_dir(g_game_dir, L"fakerFG.dll") &&
                 file_in_dir(g_game_dir, L"dxgi.dll");
    r.runtime = exe_file_exists(L"libxess_fg.dll") &&
                (S().fg_mode < 1 || exe_file_exists(L"libxell.dll")) &&
                (g_game_dir.empty() || game_runtime_ok(g_game_dir));
    r.ini = !g_game_dir.empty() && faker_ini_ok(g_game_dir);
    g_ini_drift = false;
    if (r.deployed && !g_game_dir.empty()) {
        const IniCfg c = read_ini_cfg(g_game_dir + L"\\fakerFG.ini");
        if (c.valid) {
            g_ini_drift = (c.fg_input != S().fg_input) || (c.pct != S().stream_pct) ||
                          (c.log != S().fg_log);
        }
    }
    g_ready = r;
}

// ------------------------------------------------------------------ profiles
static std::wstring prof_sec(int i)
{
    wchar_t b[32]{};
    _snwprintf_s(b, _TRUNCATE, L"p%d", i);
    return b;
}

// 一份完整配置的读写：profile 与游戏条目共用同一套键（cfg=1 表示是完整配置）。
static void save_state(const std::wstring& sec, const State& s, const wchar_t* file = nullptr)
{
    const wchar_t* prev = g_ini_file_override;
    g_ini_file_override = file;
    ini_w(sec, L"cfg", 1);
    ini_w(sec, L"fg_mode", s.fg_mode);
    ini_w(sec, L"mult", s.fg_mode + 2);
    ini_w(sec, L"fg_input", s.fg_input);
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
    ini_w(sec, L"jitter_zero", s.jitter_zero ? 1 : 0);
    ini_w(sec, L"no_marker", s.no_marker ? 1 : 0);
    ini_w(sec, L"upsample", s.upsample ? 1 : 0);
    ini_w(sec, L"codec", s.codec);
    ini_w(sec, L"depth_pct", s.depth_pct);
    ini_w(sec, L"ui_compose", s.ui_compose ? 1 : 0);
    ini_w(sec, L"ini_full", s.ini_full ? 1 : 0);
    ini_w(sec, L"stream_auto", s.stream_auto ? 1 : 0);
    ini_w(sec, L"stream_asym", s.stream_asym ? 1 : 0);
    ini_w(sec, L"input_prefetch", s.input_prefetch ? 1 : 0);
    ini_w(sec, L"ui_none", s.ui_none ? 1 : 0);
    ini_w(sec, L"unlock", s.unlock_mfg ? 1 : 0);
    ini_w(sec, L"game_quirks", s.game_quirks ? 1 : 0);
    ini_w(sec, L"slack1", s.slack1 ? 1 : 0);
    ini_w(sec, L"tearing", s.tearing ? 1 : 0);
    ini_w(sec, L"sdr", s.sdr ? 1 : 0);
    ini_w(sec, L"overlay_layered", s.overlay_layered ? 1 : 0);
    ini_w(sec, L"relay_input", s.relay_input ? 1 : 0);
    ini_w(sec, L"osd", s.osd ? 1 : 0);
    ini_w(sec, L"perf", s.perf_no_throttle ? 1 : 0);
    // 只保存模式本身；outfps 是旧版本的字段，不再写。
    ini_ws(sec, L"win_title", s.win_title);
    g_ini_file_override = prev;
}

static void save_profile(int i)
{
    const std::wstring sec = prof_sec(i);
    ini_ws(sec, L"name", g_profiles[(size_t)i].name);
    save_state(sec, g_profiles[(size_t)i].st);
}

static void load_state(const std::wstring& sec, State& s, const wchar_t* file = nullptr)
{
    const wchar_t* prev = g_ini_file_override;
    g_ini_file_override = file;
    s = State{};
    // 新版本存 mult=2/3/4；旧档案只有 fg_mode（0 = 30x4, 1 = 40x3）。
    {
        int mult = ini_r(sec, L"mult", -1);
        if (mult < 2 || mult > 4) {
            const int old = ini_r(sec, L"fg_mode", 2);
            mult = (old == 1) ? 3 : 4;
        }
        s.fg_mode = mult - 2;
        s.interp  = mult;
    }
    s.fg_input    = ini_r(sec, L"fg_input", s.fg_input);
    if (s.fg_input < 0 || s.fg_input > 3) { s.fg_input = 0; }
    s.fg_input = 0;
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
    s.jitter_zero = ini_r(sec, L"jitter_zero", s.jitter_zero ? 1 : 0) != 0;
    s.no_marker   = ini_r(sec, L"no_marker", s.no_marker ? 1 : 0) != 0;
    s.upsample    = ini_r(sec, L"upsample", s.upsample ? 1 : 0) != 0;
    s.codec       = ini_r(sec, L"codec", s.codec);
    s.depth_pct   = ini_r(sec, L"depth_pct", s.depth_pct);
    if (s.depth_pct != 100 && s.depth_pct != 150 && s.depth_pct != 200) {
        s.depth_pct = 150;
    }
    s.ui_compose  = ini_r(sec, L"ui_compose", s.ui_compose ? 1 : 0) != 0;
    s.ini_full    = ini_r(sec, L"ini_full", s.ini_full ? 1 : 0) != 0;
    s.stream_auto = ini_r(sec, L"stream_auto", s.stream_auto ? 1 : 0) != 0;
    s.stream_asym = ini_r(sec, L"stream_asym", s.stream_asym ? 1 : 0) != 0;
    s.input_prefetch = ini_r(sec, L"input_prefetch", s.input_prefetch ? 1 : 0) != 0;
    s.ui_none     = ini_r(sec, L"ui_none", s.ui_none ? 1 : 0) != 0;
    s.unlock_mfg  = ini_r(sec, L"unlock", s.unlock_mfg ? 1 : 0) != 0;
    s.game_quirks = ini_r(sec, L"game_quirks", s.game_quirks ? 1 : 0) != 0;
    s.slack1      = ini_r(sec, L"slack1", s.slack1 ? 1 : 0) != 0;
    s.tearing     = ini_r(sec, L"tearing", s.tearing ? 1 : 0) != 0;
    s.sdr         = ini_r(sec, L"sdr", s.sdr ? 1 : 0) != 0;
    s.overlay_layered = ini_r(sec, L"overlay_layered", s.overlay_layered ? 1 : 0) != 0;
    s.relay_input = ini_r(sec, L"relay_input", s.relay_input ? 1 : 0) != 0;
    s.osd = ini_r(sec, L"osd", s.osd ? 1 : 0) != 0;
    s.perf_no_throttle = ini_r(sec, L"perf", s.perf_no_throttle ? 1 : 0) != 0;
    // 固定倍率 2x/3x/4x。
    if (s.fg_mode < 0 || s.fg_mode > 2) { s.fg_mode = 2; }
    s.out_fps     = 0;
    s.win_title   = ini_rs(sec, L"win_title", L"");
    s.interp      = s.fg_mode + 2;
    if (s.pool < 2) { s.pool = 2; }
    if (s.pool > 6) { s.pool = 6; }
    if (s.latency < 1) { s.latency = 1; }
    if (s.latency > 3) { s.latency = 3; }
    g_ini_file_override = prev;
}

static void load_profile(int i)
{
    const std::wstring sec = prof_sec(i);
    load_state(sec, g_profiles[(size_t)i].st);
    g_profiles[(size_t)i].name = ini_rs(sec, L"name", L"默认");
}

static std::wstring last_component(const std::wstring& path)
{
    std::wstring p = path;
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) { p.pop_back(); }
    const size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? p : p.substr(s + 1);
}

// 对话框里只显示末尾两级路径，长路径不会把 MessageBox 撑爆/裁字。
static std::wstring short_path(const std::wstring& p, int parts = 2)
{
    std::vector<std::wstring> v;
    std::wstring cur;
    for (wchar_t c : p) {
        if (c == L'\\' || c == L'/') { if (!cur.empty()) { v.push_back(cur); cur.clear(); } }
        else { cur.push_back(c); }
    }
    if (!cur.empty()) { v.push_back(cur); }
    std::wstring out;
    for (int i = (int)v.size() - parts; i < (int)v.size(); ++i) {
        if (i < 0) { continue; }
        if (!out.empty()) { out += L"\\"; }
        out += v[(size_t)i];
    }
    return out.empty() ? p : out;
}

// MessageBox 不会自动折行：按固定列数硬折（优先在分隔符后），避免右侧被裁掉。
static std::wstring wrap_path(const std::wstring& s, size_t width = 26)
{
    std::wstring out;
    size_t col = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        out.push_back(s[i]);
        ++col;
        if (col >= width) {
            out += L"\r\n";
            col = 0;
        }
    }
    return out;
}
static bool same_name(const std::wstring& a, const std::wstring& b)
{
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// exe 文件名去掉扩展名：新加游戏默认用它作为配置名。
static std::wstring file_stem(const std::wstring& path)
{
    std::wstring f = last_component(path);
    const size_t dot = f.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) { f = f.substr(0, dot); }
    return f;
}

static std::wstring norm_path(const std::wstring& p)
{
    std::wstring s = p;
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) { s.pop_back(); }
    for (wchar_t& c : s) { c = (wchar_t)towlower(c); }
    return s;
}

// parent 与 child 是否是同一个目录，或 parent 是 child 的祖先（按 \ 边界）。
static bool path_within(const std::wstring& parent, const std::wstring& child)
{
    const std::wstring p = norm_path(parent);
    const std::wstring c = norm_path(child);
    if (p.empty() || c.empty()) { return false; }
    if (p == c) { return true; }
    return c.size() > p.size() && c.compare(0, p.size(), p) == 0 && c[p.size()] == L'\\';
}

static GameEntry* find_game_mut(const std::wstring& dir)
{
    if (dir.empty()) { return nullptr; }
    for (GameEntry& g : g_games) {
        if ((!g.dir.empty() && (path_within(g.dir, dir) || path_within(dir, g.dir))) ||
            (!g.root.empty() && (path_within(g.root, dir) || path_within(dir, g.root)))) {
            return &g;
        }
    }
    return nullptr;
}

static const GameEntry* find_game(const std::wstring& dir)
{
    return find_game_mut(dir);
}

// 新加的游戏从"通用配置"复制一份，避免继承上一个游戏的窗口等会话信息。
static State baseline_state()
{
    if (g_profiles.empty()) { g_profiles.push_back(Profile{}); }
    const int i = (g_prof >= 0 && g_prof < (int)g_profiles.size()) ? g_prof : 0;
    State s = g_profiles[(size_t)i].st;
    s.win_hwnd = nullptr;
    s.win_title.clear();
    return s;
}

// ------------------------------------------------------- 每游戏一个配置文件
// 目录：<exe>\profiles\，主 ini 只存 [games] count + 每项 path。
static std::wstring profile_dir_abs() { return exe_dir() + L"\\profiles"; }
static std::wstring profile_abs(const std::wstring& rel) { return exe_dir() + L"\\" + rel; }

static void ensure_profile_dir()
{
    CreateDirectoryW(profile_dir_abs().c_str(), nullptr);
}

// 文件名里不能出现的字符换成下划线；保留中文/字母/数字。
static std::wstring sanitize_file_name(const std::wstring& in)
{
    std::wstring out;
    for (wchar_t c : in) {
        if (c < 32 || c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
            c == L'"' || c == L'<' || c == L'>' || c == L'|') {
            out.push_back(L'_');
        } else if (c == L' ') {
            out.push_back(L'_');
        } else {
            out.push_back(c);
        }
        if (out.size() >= 48) { break; }
    }
    while (!out.empty() && (out.back() == L'.' || out.back() == L'_')) { out.pop_back(); }
    return out;
}

static bool profile_used_by_other(const std::wstring& rel, const GameEntry* self)
{
    for (const GameEntry& g : g_games) {
        if (&g == self) { continue; }
        if (!g.profile.empty() && _wcsicmp(g.profile.c_str(), rel.c_str()) == 0) { return true; }
    }
    return false;
}

static std::wstring new_profile_rel(const std::wstring& base_name, const GameEntry* self)
{
    std::wstring base = sanitize_file_name(base_name);
    if (base.empty()) { base = L"game"; }
    for (int k = 1; k < 1000; ++k) {
        wchar_t name[96]{};
        if (k == 1) { _snwprintf_s(name, _TRUNCATE, L"profiles\\%s.ini", base.c_str()); }
        else { _snwprintf_s(name, _TRUNCATE, L"profiles\\%s_%d.ini", base.c_str(), k); }
        const std::wstring rel = name;
        if (GetFileAttributesW(profile_abs(rel).c_str()) == INVALID_FILE_ATTRIBUTES &&
            !profile_used_by_other(rel, self)) {
            return rel;
        }
    }
    return L"profiles\\game.ini";
}

static void save_game_file(const GameEntry& e)
{
    if (e.profile.empty()) { return; }
    const std::wstring f = profile_abs(e.profile);
    ensure_utf16_bom(f);
    ini_ws(L"game", L"root", e.root, f.c_str());
    ini_ws(L"game", L"dir", e.dir, f.c_str());
    ini_ws(L"game", L"exe", e.exe, f.c_str());
    ini_ws(L"game", L"name", e.name, f.c_str());
    ini_ws(L"game", L"source", e.source, f.c_str());
    ini_w(L"game", L"last_use", (int)(e.last_use & 0x7FFFFFFF), f.c_str());
    save_state(L"game", e.st, f.c_str());
}

static void load_game_file(GameEntry& e, const std::wstring& path)
{
    e.root = ini_rs(L"game", L"root", L"", path.c_str());
    e.dir = ini_rs(L"game", L"dir", L"", path.c_str());
    e.exe = ini_rs(L"game", L"exe", L"", path.c_str());
    e.name = ini_rs(L"game", L"name", L"", path.c_str());
    e.source = ini_rs(L"game", L"source", L"", path.c_str());
    load_state(L"game", e.st, path.c_str());
    e.last_use = (unsigned long long)ini_r(L"game", L"last_use", 0, path.c_str());
    e.mult = e.st.fg_mode + 2;
    e.fg_input = e.st.fg_input;
    e.lock = e.st.lock_fps;
}

// 迁移后清掉主 ini 里遗留的内联字段（新格式只保留 path/name）。
static void clear_legacy_game_keys(const wchar_t* sec)
{
    static const wchar_t* keys[] = {
        L"root", L"dir", L"exe", L"source", L"mult", L"fg_input", L"lock", L"last_use",
        L"cfg", L"fg_mode", L"interp", L"out_mode", L"pool", L"latency", L"fov", L"ivl",
        L"rt", L"tmode", L"gpu_sel", L"hotkey3", L"jitter_zero", L"no_marker",
        L"upsample", L"stream_auto", L"stream_asym", L"input_prefetch", L"ui_none",
        L"unlock", L"slack1", L"tearing", L"sdr", L"overlay_layered", L"relay_input",
        L"osd", L"perf", L"win_title", L"codec", L"depth_pct", L"ui_compose",
    };
    for (const wchar_t* k : keys) {
        WritePrivateProfileStringW(sec, k, nullptr, ini_path().c_str());
    }
}

static void save_games()
{
    ensure_profile_dir();
    ini_w(L"games", L"count", (int)g_games.size());
    for (int i = 0; i < (int)g_games.size(); ++i) {
        wchar_t sec[24]{};
        _snwprintf_s(sec, _TRUNCATE, L"game%d", i);
        GameEntry& e = g_games[(size_t)i];
        if (e.profile.empty()) {
            const std::wstring base = !e.name.empty() ? e.name
                                     : (!e.exe.empty() ? file_stem(e.exe)
                                                       : last_component(e.dir.empty() ? e.root : e.dir));
            e.profile = new_profile_rel(base, &e);
        }
        e.mult = e.st.fg_mode + 2;          // 旧字段跟配置保持一致
        e.fg_input = e.st.fg_input;
        e.lock = e.st.lock_fps;
        // 主 ini 只留索引；名字也镜像一份，方便手工查看/坏文件时兜底。
        ini_ws(sec, L"path", e.profile);
        ini_ws(sec, L"name", e.name);
        if (!ini_rs(sec, L"dir", L"").empty() || ini_r(sec, L"cfg", 0) != 0) {
            clear_legacy_game_keys(sec);    // 旧格式内联字段：迁移完成后清掉
        }
        save_game_file(e);
    }
}

static void load_games()
{
    g_games.clear();
    int n = ini_r(L"games", L"count", 0);
    if (n < 0) { n = 0; }
    if (n > 400) { n = 400; }
    bool migrated = false;
    for (int i = 0; i < n; ++i) {
        wchar_t sec[24]{};
        _snwprintf_s(sec, _TRUNCATE, L"game%d", i);
        GameEntry e;
        e.profile = ini_rs(sec, L"path", L"");
        if (!e.profile.empty()) {
            load_game_file(e, profile_abs(e.profile));
            if (e.dir.empty() && e.root.empty() && e.exe.empty()) { continue; }  // 文件缺失/损坏
        } else {
            // 旧档案：字段直接写在 [gameN] 里，先读进内存，最后迁移成单独文件。
            e.dir = ini_rs(sec, L"dir", L"");
            e.root = ini_rs(sec, L"root", L"");
            e.exe = ini_rs(sec, L"exe", L"");
            if (e.dir.empty() && e.root.empty()) { continue; }
            e.name = ini_rs(sec, L"name", L"");
            e.source = ini_rs(sec, L"source", L"");
            load_state(sec, e.st);
            e.mult = e.st.fg_mode + 2;
            e.fg_input = e.st.fg_input;
            e.lock = e.st.lock_fps;
            e.last_use = (unsigned long long)ini_r(sec, L"last_use", 0);
            migrated = true;
        }
        if (e.name.empty()) {
            e.name = !e.exe.empty() ? file_stem(e.exe)
                                    : last_component(e.dir.empty() ? e.root : e.dir);
        }
        g_games.push_back(e);
    }
    if (migrated) { save_games(); }   // 生成 profiles\*.ini 并重写索引
}

// =========================================================== 本机游戏扫描
// 用户不一定知道 exe 在哪。这里从 Steam / Epic / GOG / 卸载注册表把
// "游戏名 + 安装位置" 收进游戏库；索引优先级：已解析的 exe 目录 > 安装根目录。
static bool game_like_name(const std::wstring& n);

static std::string read_file_bytes(const std::wstring& path)
{
    std::string out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return out; }
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
        out.append(buf, got);
    }
    CloseHandle(h);
    return out;
}

static std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty()) { return L""; }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) { return L""; }
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// 取一行里所有 "..." 的内容（VDF / JSON 都是这种形状，够用就行）。
static void quoted_values(const std::string& line, std::vector<std::string>* out)
{
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] != '"') { continue; }
        const size_t j = line.find('"', i + 1);
        if (j == std::string::npos) { break; }
        out->push_back(line.substr(i + 1, j - i - 1));
        i = j;
    }
}

static void split_lines(const std::string& data, std::vector<std::string>* out)
{
    size_t start = 0;
    while (start <= data.size()) {
        const size_t nl = data.find('\n', start);
        const size_t end = (nl == std::string::npos) ? data.size() : nl;
        out->push_back(data.substr(start, end - start));
        if (nl == std::string::npos) { break; }
        start = nl + 1;
    }
}

static bool g_sha_checked = false;
static bool g_sha_ok = false;

static bool sha256_file_hex(const std::wstring& path, std::wstring* out)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }
    DWORD obj_len = 0, cb = 0, hash_len = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_len),
                      sizeof(obj_len), &cb, 0);
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len),
                      sizeof(hash_len), &cb, 0);
    std::vector<UCHAR> obj(obj_len > 0 ? obj_len : 1);
    std::vector<UCHAR> hash(hash_len > 0 ? hash_len : 32);

    bool ok = false;
    BCRYPT_HASH_HANDLE h = nullptr;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f != INVALID_HANDLE_VALUE &&
        BCryptCreateHash(alg, &h, obj.data(), obj_len, nullptr, 0, 0) == 0) {
        ok = true;
        UCHAR buf[64 * 1024];
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(f, buf, sizeof(buf), &got, nullptr)) {
                if (GetLastError() != ERROR_HANDLE_EOF) { ok = false; }
                break;
            }
            if (got == 0) { break; }
            if (BCryptHashData(h, buf, got, 0) != 0) { ok = false; break; }
        }
        if (ok && BCryptFinishHash(h, hash.data(), hash_len, 0) == 0) {
            static const wchar_t* H = L"0123456789abcdef";
            out->clear();
            for (DWORD i = 0; i < hash_len; ++i) {
                out->push_back(H[hash[i] >> 4]);
                out->push_back(H[hash[i] & 15]);
            }
        } else {
            ok = false;
        }
    }
    if (h != nullptr) { BCryptDestroyHash(h); }
    if (f != INVALID_HANDLE_VALUE) { CloseHandle(f); }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// 启动时校验一次安装包完整性（不周期采样，不占运行性能）。
static void verify_package()
{
    if (g_sha_checked) { return; }
    g_sha_checked = true;

    const std::string data = read_file_bytes(exe_dir() + L"\\SHA256SUMS.txt");
    if (data.empty()) {
        g_sha_text = T(L"未提供 SHA256SUMS.txt（开发构建）",
                       L"SHA256SUMS.txt not provided (dev build)");
        uilog("sha256: no SHA256SUMS.txt");
        return;
    }

    std::vector<std::string> lines;
    split_lines(data, &lines);
    int checked = 0, bad = 0;
    for (const std::string& l : lines) {
        if (l.size() < 66) { continue; }
        const std::string want = l.substr(0, 64);
        size_t p = 64;
        while (p < l.size() && (l[p] == ' ' || l[p] == '\t')) { ++p; }
        if (p >= l.size()) { continue; }
        std::string rel = l.substr(p);
        if (rel.rfind("./", 0) == 0) { rel = rel.substr(2); }

        std::wstring w = utf8_to_wide(rel);
        for (wchar_t& c : w) { if (c == L'/') { c = L'\\'; } }
        const std::wstring full = exe_dir() + L"\\" + w;
        if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;                      // 本机没有这个文件（如 Intel 运行时）
        }
        std::wstring got;
        if (!sha256_file_hex(full, &got)) { ++bad; continue; }
        ++checked;
        std::wstring want_w = utf8_to_wide(want);
        for (wchar_t& c : want_w) { c = (wchar_t)towlower(c); }
        if (got != want_w) { ++bad; }
    }

    wchar_t b[128]{};
    if (checked == 0 && bad == 0) {
        g_sha_text = T(L"无匹配文件（开发构建）", L"No matching files (dev build)");
    } else if (bad == 0) {
        g_sha_ok = true;
        _snwprintf_s(b, _TRUNCATE, T(L"OK（%d 个文件）", L"OK (%d files)"), checked);
        g_sha_text = b;
    } else {
        _snwprintf_s(b, _TRUNCATE, T(L"%d/%d 个文件校验失败", L"%d/%d files failed"),
                     bad, checked + bad);
        g_sha_text = b;
    }
    uilog("sha256: checked=%d bad=%d", checked, bad);
}


static std::wstring reg_str(HKEY root, const wchar_t* sub, const wchar_t* value)
{
    HKEY k = nullptr;
    std::wstring out;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t buf[1024]{};
        DWORD sz = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &sz) ==
                ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            out = buf;
        }
        RegCloseKey(k);
    }
    return out;
}

static bool dir_exists(const std::wstring& p)
{
    if (p.empty()) { return false; }
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static void scan_steam(std::vector<GameEntry>* out)
{
    std::wstring steam = reg_str(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) {
        steam = reg_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam",
                        L"InstallPath");
    }
    if (steam.empty()) { return; }
    for (wchar_t& c : steam) { if (c == L'/') { c = L'\\'; } }

    std::vector<std::wstring> libs{ steam };
    std::vector<std::string> lines;
    split_lines(read_file_bytes(steam + L"\\steamapps\\libraryfolders.vdf"), &lines);
    for (const std::string& line : lines) {
        std::vector<std::string> q;
        quoted_values(line, &q);
        if (q.size() < 2) { continue; }
        const bool is_path = _stricmp(q[0].c_str(), "path") == 0;
        const bool is_index = !q[0].empty() &&
            q[0].find_first_not_of("0123456789") == std::string::npos;
        if (!is_path && !is_index) { continue; }
        std::wstring p = utf8_to_wide(q[1]);
        for (wchar_t& c : p) { if (c == L'/') { c = L'\\'; } }
        if (dir_exists(p)) { libs.push_back(p); }
    }

    for (const std::wstring& lib : libs) {
        const std::wstring common = lib + L"\\steamapps\\common";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((lib + L"\\steamapps\\appmanifest_*.acf").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) { continue; }
        do {
            const std::string acf = read_file_bytes(lib + L"\\steamapps\\" + fd.cFileName);
            std::wstring name, idir;
            std::vector<std::string> alines;
            split_lines(acf, &alines);
            for (const std::string& l : alines) {
                std::vector<std::string> q;
                quoted_values(l, &q);
                if (q.size() < 2) { continue; }
                if (_stricmp(q[0].c_str(), "name") == 0 && name.empty()) {
                    name = utf8_to_wide(q[1]);
                } else if (_stricmp(q[0].c_str(), "installdir") == 0 && idir.empty()) {
                    idir = utf8_to_wide(q[1]);
                }
            }
            if (!idir.empty()) {
                GameEntry e;
                e.root = common + L"\\" + idir;
                if (dir_exists(e.root) && game_like_name(idir) &&
                    game_like_name(e.name.empty() ? idir : e.name)) {
                    e.name = name.empty() ? idir : name;
                    e.source = L"Steam";
                    out->push_back(e);
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static void scan_epic(std::vector<GameEntry>* out)
{
    wchar_t programdata[512]{};
    if (GetEnvironmentVariableW(L"ProgramData", programdata, 512) == 0) { return; }
    const std::wstring dat =
        std::wstring(programdata) + L"\\Epic\\UnrealEngineLauncher\\LauncherInstalled.dat";
    const std::string data = read_file_bytes(dat);
    if (data.empty()) { return; }

    const std::string key = "\"InstallLocation\"";
    size_t pos = 0;
    while ((pos = data.find(key, pos)) != std::string::npos) {
        const size_t colon = data.find(':', pos + key.size());
        const size_t q1 = (colon == std::string::npos) ? std::string::npos
                                                       : data.find('"', colon);
        const size_t q2 = (q1 == std::string::npos) ? std::string::npos
                                                    : data.find('"', q1 + 1);
        if (q2 == std::string::npos) { break; }
        std::wstring p = utf8_to_wide(data.substr(q1 + 1, q2 - q1 - 1));
        for (wchar_t& c : p) { if (c == L'/') { c = L'\\'; } }
        if (dir_exists(p)) {
            GameEntry e;
            e.root = p;
            e.name = last_component(p);
            e.source = L"Epic";
            out->push_back(e);
        }
        pos = q2 + 1;
    }
}

static void scan_gog(std::vector<GameEntry>* out)
{
    const wchar_t* paths[] = {
        L"SOFTWARE\\WOW6432Node\\GOG.com\\Games",
        L"SOFTWARE\\GOG.com\\Games",
    };
    for (const wchar_t* sub : paths) {
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) {
            continue;
        }
        wchar_t name[128]{};
        for (DWORD i = 0;; ++i) {
            DWORD len = 128;
            if (RegEnumKeyExW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) !=
                ERROR_SUCCESS) {
                break;
            }
            const std::wstring p = reg_str(HKEY_LOCAL_MACHINE,
                                           (std::wstring(sub) + L"\\" + name).c_str(),
                                           L"path");
            if (!dir_exists(p)) { continue; }
            GameEntry e;
            e.root = p;
            const std::wstring gn = reg_str(HKEY_LOCAL_MACHINE,
                                            (std::wstring(sub) + L"\\" + name).c_str(),
                                            L"gameName");
            e.name = gn.empty() ? last_component(p) : gn;
            e.source = L"GOG";
            out->push_back(e);
        }
        RegCloseKey(k);
    }
}

static bool game_like_name(const std::wstring& n)
{
    static const wchar_t* bad[] = {
        L"update for", L"hotfix", L"redistributable", L"runtime", L"sdk",
        L"directx", L"visual c++", L".net ", L"driver", L"language pack",
        L"运行库", L"支持组件",
        L"steamworks", L"proton", L"steam linux runtime", L"steamvr",
        L"easy anti-cheat", L"battleye", L"dedicated server",
    };
    std::wstring s = n;
    for (wchar_t& c : s) { c = (wchar_t)towlower(c); }
    for (const wchar_t* b : bad) {
        if (s.find(b) != std::wstring::npos) { return false; }
    }
    return true;
}

static bool bad_install_dir(const std::wstring& p)
{
    std::wstring s = p;
    for (wchar_t& c : s) { c = (wchar_t)towlower(c); }
    if (s.find(L"\\windowsapps") != std::wstring::npos) { return true; }
    wchar_t windir[260]{};
    if (GetWindowsDirectoryW(windir, 260) > 0) {
        std::wstring w = windir;
        for (wchar_t& c : w) { c = (wchar_t)towlower(c); }
        if (!w.empty() && s.rfind(w, 0) == 0) { return true; }
    }
    return false;
}

static void scan_uninstall(std::vector<GameEntry>* out)
{
    struct Key { HKEY root; const wchar_t* sub; };
    const Key keys[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
    };
    for (const Key& key : keys) {
        HKEY k = nullptr;
        if (RegOpenKeyExW(key.root, key.sub, 0, KEY_READ, &k) != ERROR_SUCCESS) { continue; }
        wchar_t sub[256]{};
        for (DWORD i = 0;; ++i) {
            DWORD len = 256;
            if (RegEnumKeyExW(k, i, sub, &len, nullptr, nullptr, nullptr, nullptr) !=
                ERROR_SUCCESS) {
                break;
            }
            HKEY sk = nullptr;
            if (RegOpenKeyExW(k, sub, 0, KEY_READ, &sk) != ERROR_SUCCESS) { continue; }
            wchar_t dn[512]{}, loc[1024]{};
            DWORD sz = sizeof(dn), type = 0;
            RegQueryValueExW(sk, L"DisplayName", nullptr, &type,
                             reinterpret_cast<LPBYTE>(dn), &sz);
            sz = sizeof(loc);
            type = 0;
            RegQueryValueExW(sk, L"InstallLocation", nullptr, &type,
                             reinterpret_cast<LPBYTE>(loc), &sz);
            RegCloseKey(sk);

            const std::wstring name = dn;
            std::wstring dir = loc;
            while (!dir.empty() && dir.back() == L' ') { dir.pop_back(); }
            if (name.empty() || dir.empty()) { continue; }
            if (!game_like_name(name) || bad_install_dir(dir) || !dir_exists(dir)) { continue; }
            GameEntry e;
            e.root = dir;
            e.name = name;
            e.source = L"注册表";
            out->push_back(e);
        }
        RegCloseKey(k);
    }
}

// 在一个安装根目录里找"最像主程序"的 exe，返回它所在目录（部署目标）。
static bool bad_exe_name(const std::wstring& n)
{
    static const wchar_t* bad[] = {
        L"unins", L"setup", L"redist", L"vcredist", L"dxsetup", L"crash",
        L"report", L"launcher", L"helper", L"ue4prereq", L"unitycrashhandler",
    };
    std::wstring s = n;
    for (wchar_t& c : s) { c = (wchar_t)towlower(c); }
    for (const wchar_t* b : bad) {
        if (s.find(b) != std::wstring::npos) { return true; }
    }
    return false;
}

// ------------------------------------------------------- 主程序解析（PE 探测）
// 包体内可能有多个 exe：启动器、崩溃报告、反作弊、真正的游戏进程。这里按
//   * 是否导入 d3d11/d3d12/dxgi（图形程序的最强信号）
//   * 名字是否像当前游戏目录 / UE 的 -Win64-Shipping
//   * 体积、目录深度、明确黑名单
// 打分选主程序；分数太低时保留为空，交给"自选 exe 安装"。
static bool pe_file_imports_dx(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { return false; }
    auto read_at = [&](DWORD off, void* buf, DWORD n) -> bool {
        LARGE_INTEGER li{};
        li.QuadPart = off;
        if (!SetFilePointerEx(f, li, nullptr, FILE_BEGIN)) { return false; }
        DWORD got = 0;
        return ReadFile(f, buf, n, &got, nullptr) && got == n;
    };

    bool dx = false;
    IMAGE_DOS_HEADER dos{};
    if (read_at(0, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE) {
        IMAGE_FILE_HEADER fh{};
        const DWORD nt = (DWORD)dos.e_lfanew;
        if (read_at(nt + 4, &fh, sizeof(fh)) && fh.SizeOfOptionalHeader >= 96) {
            const bool pe64 = (fh.Machine == IMAGE_FILE_MACHINE_AMD64 ||
                               fh.Machine == IMAGE_FILE_MACHINE_ARM64);
            BYTE opt[240]{};
            const DWORD opt_size = fh.SizeOfOptionalHeader > sizeof(opt)
                                       ? (DWORD)sizeof(opt) : fh.SizeOfOptionalHeader;
            if (read_at(nt + 4 + sizeof(fh), opt, opt_size)) {
                const DWORD dd = pe64 ? 112u : 96u;
                DWORD import_rva = 0;
                if (dd + 8 <= opt_size) {
                    DWORD v = 0;
                    memcpy(&v, opt + dd, 4);
                    import_rva = v;
                }
                if (import_rva != 0 && fh.NumberOfSections > 0 && fh.NumberOfSections <= 96) {
                    std::vector<IMAGE_SECTION_HEADER> secs(fh.NumberOfSections);
                    const DWORD sec_off = nt + 4 + sizeof(fh) + fh.SizeOfOptionalHeader;
                    if (read_at(sec_off, secs.data(),
                                (DWORD)(secs.size() * sizeof(IMAGE_SECTION_HEADER)))) {
                        auto rva2off = [&](DWORD rva) -> DWORD {
                            for (const auto& sc : secs) {
                                const DWORD vs = sc.Misc.VirtualSize ? sc.Misc.VirtualSize
                                                                    : sc.SizeOfRawData;
                                if (rva >= sc.VirtualAddress && rva < sc.VirtualAddress + vs) {
                                    return sc.PointerToRawData + (rva - sc.VirtualAddress);
                                }
                            }
                            return 0;
                        };
                        const DWORD desc = rva2off(import_rva);
                        if (desc != 0) {
                            for (int i = 0; i < 48; ++i) {
                                IMAGE_IMPORT_DESCRIPTOR id{};
                                if (!read_at(desc + (DWORD)i * sizeof(id), &id, sizeof(id))) {
                                    break;
                                }
                                if (id.Name == 0) { break; }
                                const DWORD name_off = rva2off(id.Name);
                                if (name_off == 0) { continue; }
                                char name[64]{};
                                if (!read_at(name_off, name, sizeof(name) - 1)) { continue; }
                                for (char& c : name) { c = (char)tolower((unsigned char)c); }
                                if (strstr(name, "d3d12.dll") || strstr(name, "d3d11.dll") ||
                                    strstr(name, "dxgi.dll")) {
                                    dx = true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    CloseHandle(f);
    return dx;
}

struct ExePick
{
    std::wstring path;
    std::wstring dir;
    int          score = -100000;
};

static std::wstring norm_alnum(const std::wstring& in)
{
    std::wstring out;
    for (wchar_t c : in) {
        if (iswalnum(c)) { out.push_back((wchar_t)towlower(c)); }
    }
    return out;
}

static int name_similarity(const std::wstring& stem, const std::wstring& folder)
{
    const std::wstring a = norm_alnum(stem), b = norm_alnum(folder);
    if (a.empty() || b.empty()) { return 0; }
    if (a == b) { return 30; }
    if (a.find(b) != std::wstring::npos || b.find(a) != std::wstring::npos) { return 20; }
    int n = 0;
    while (n < (int)a.size() && n < (int)b.size() && a[(size_t)n] == b[(size_t)n]) { ++n; }
    return (n >= 4) ? 10 : 0;
}

static std::wstring exe_stem(const std::wstring& name)
{
    const size_t dot = name.find_last_of(L'.');
    return (dot == std::wstring::npos) ? name : name.substr(0, dot);
}

static void resolve_game_exes(const std::wstring& root, std::vector<ExePick>* out,
                              int max_items)
{
    if (out == nullptr) { return; }
    out->clear();
    std::vector<ExePick> found;
    struct Node { std::wstring dir; int depth; };
    std::vector<Node> q{ Node{ root, 0 } };
    const std::wstring folder = last_component(root);

    for (size_t qi = 0; qi < q.size() && qi < 260; ++qi) {
        const Node c = q[qi];
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((c.dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) { continue; }
        do {
            if (fd.cFileName[0] == L'.') { continue; }
            const std::wstring full = c.dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                std::wstring dn = fd.cFileName;
                for (wchar_t& ch : dn) { ch = (wchar_t)towlower(ch); }
                if (c.depth >= 3 || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    continue;
                }
                if (dn == L"redist" || dn == L"_commonredist" || dn == L"directx" ||
                    dn == L"dotnet" || dn == L"support" || dn == L"engine" ||
                    dn == L"__installer" || dn == L"easyanticheat" || dn == L"battleye") {
                    continue;
                }
                q.push_back(Node{ full, c.depth + 1 });
                continue;
            }
            const size_t n = wcslen(fd.cFileName);
            if (n < 5 || _wcsicmp(fd.cFileName + n - 4, L".exe") != 0) { continue; }

            std::wstring lower = fd.cFileName;
            for (wchar_t& ch : lower) { ch = (wchar_t)towlower(ch); }
            const unsigned long long size =
                ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (size < (1ull << 20)) { continue; }        // < 1 MB 基本不是主程序
            if (bad_exe_name(lower)) { continue; }

            int score = 0;
            if (pe_file_imports_dx(full)) { score += 60; }
            if (lower.find(L"shipping") != std::wstring::npos) { score += 25; }
            if (lower.find(L"game") != std::wstring::npos) { score += 15; }
            score += name_similarity(exe_stem(lower), folder);
            if (size >= (20ull << 20))     { score += 25; }
            else if (size >= (8ull << 20)) { score += 15; }
            else                           { score += 5; }
            if (c.depth == 0)      { score += 10; }
            else if (c.depth == 1) { score += 6; }
            else                   { score += 3; }

            ExePick p;
            p.path = full;
            p.dir = c.dir;
            p.score = score;
            found.push_back(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::sort(found.begin(), found.end(),
              [](const ExePick& a, const ExePick& b) { return a.score > b.score; });
    const int total = (int)found.size();
    const int n = (max_items > 0 && total > max_items) ? max_items : total;
    out->assign(found.begin(), found.begin() + n);
}

static void resolve_game_exe(const std::wstring& root, ExePick* best, ExePick* second)
{
    std::vector<ExePick> v;
    resolve_game_exes(root, &v, 2);
    *best = (v.size() > 0) ? v[0] : ExePick{};
    *second = (v.size() > 1) ? v[1] : ExePick{};
}

// 兼容旧调用：只取目录（分数太低视为解析失败）。
static std::wstring find_game_exe_dir(const std::wstring& root)
{
    ExePick best, second;
    resolve_game_exe(root, &best, &second);
    return (best.score >= 40) ? best.dir : std::wstring();
}

// 已经部署过的游戏：目录里存在 fakerFG.dll，直接认它（不需要再猜 exe）。
static std::wstring find_deployed_dir(const std::wstring& root)
{
    struct Node { std::wstring dir; int depth; };
    std::vector<Node> q{ Node{ root, 0 } };
    for (size_t qi = 0; qi < q.size() && qi < 120; ++qi) {
        const Node c = q[qi];
        if (GetFileAttributesW((c.dir + L"\\fakerFG.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
            return c.dir;
        }
        if (c.depth >= 3) { continue; }
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((c.dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) { continue; }
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { continue; }
            if (fd.cFileName[0] == L'.') { continue; }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { continue; }
            if (_wcsicmp(fd.cFileName, L"redist") == 0 ||
                _wcsicmp(fd.cFileName, L"_CommonRedist") == 0 ||
                _wcsicmp(fd.cFileName, L"DirectX") == 0) { continue; }
            q.push_back(Node{ c.dir + L"\\" + fd.cFileName, c.depth + 1 });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return std::wstring();
}

// 选中 / 安装某个游戏时调用：优先已安装目录，否则解析主程序并写回。
static bool ensure_entry_resolved(GameEntry& e)
{
    const std::wstring old_dir = e.dir, old_root = e.root;
    const bool auto_name = e.name.empty() ||
        same_name(e.name, last_component(old_dir.empty() ? old_root : old_dir));
    auto fix_name = [&]() {
        if (auto_name && !e.exe.empty()) { e.name = file_stem(e.exe); }
    };
    if (!e.exe.empty() &&
        GetFileAttributesW(e.exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const size_t s = e.exe.find_last_of(L'\\');
        if (s != std::wstring::npos) { e.dir = e.exe.substr(0, s); }
        fix_name();
        return true;
    }
    if (e.root.empty()) { return false; }

    if (e.dir.empty()) {
        const std::wstring dep = find_deployed_dir(e.root);
        if (!dep.empty()) { e.dir = dep; }
    }
    const std::wstring base = e.dir.empty() ? e.root : e.dir;
    ExePick best, second;
    resolve_game_exe(base, &best, &second);
    if (!best.path.empty() && best.score >= 40) {
        e.exe = best.path;
        e.dir = best.dir;
        fix_name();
        return true;
    }
    return !e.dir.empty();
}

static void dedupe_games(std::vector<GameEntry>* v)
{
    std::vector<GameEntry> out;
    for (const GameEntry& e : *v) {
        bool dup = false;
        for (const GameEntry& o : out) {
            if ((!e.root.empty() && !o.root.empty() && norm_path(e.root) == norm_path(o.root)) ||
                (!e.dir.empty() && !o.dir.empty() && norm_path(e.dir) == norm_path(o.dir)) ||
                (!e.dir.empty() && !o.root.empty() && norm_path(e.dir) == norm_path(o.root))) {
                dup = true;
                break;
            }
        }
        if (!dup) { out.push_back(e); }
    }
    v->swap(out);
}

// 扫描并合并进游戏库（不覆盖已有条目）。返回新增数量。
static int scan_games_merge()
{
    std::vector<GameEntry> found;
    scan_steam(&found);
    scan_epic(&found);
    scan_gog(&found);
    scan_uninstall(&found);
    dedupe_games(&found);

    int added = 0;
    for (const GameEntry& f : found) {
        if (find_game(f.root) != nullptr) { continue; }
        if (!f.dir.empty() && find_game(f.dir) != nullptr) { continue; }
        if (g_games.size() >= 200) { break; }
        GameEntry n = f;
        n.st = baseline_state();       // 每个游戏一份自己的配置
        g_games.push_back(n);
        ++added;
    }
    save_games();
    return added;
}

// 记住当前这套设置属于哪个游戏。安装完成 / 成功启动 / 改输入源时调用。
static void remember_game(const std::wstring& dir)
{
    if (dir.empty()) { return; }
    GameEntry* e = find_game_mut(dir);
    if (e == nullptr) {
        if (g_games.size() >= 200) { g_games.erase(g_games.begin()); }
        GameEntry n;
        n.dir = dir;
        n.root = dir;
        n.name = last_component(dir);
        n.source = L"手动";
        n.st = baseline_state();
        if (g_game_dir.empty() || norm_path(g_game_dir) != norm_path(dir)) { n.st = S(); }
        g_games.push_back(n);
        e = &g_games.back();
    }
    if (e->dir.empty()) { e->dir = dir; }
    if (e->root.empty()) { e->root = dir; }
    if (e->name.empty()) { e->name = last_component(dir); }
    if (e->source.empty()) { e->source = L"手动"; }
    e->mult = e->st.fg_mode + 2;
    e->fg_input = e->st.fg_input;
    e->lock = e->st.lock_fps;
    e->last_use = (unsigned long long)std::time(nullptr);
    save_games();
}

// 切换到这个游戏时恢复它自己的倍率 / 输入源 / 锁帧提示。
static void apply_game(const std::wstring& dir)
{
    GameEntry* e = find_game_mut(dir);
    if (e == nullptr) { return; }
    if (g_game_dir.empty() || norm_path(g_game_dir) != norm_path(dir)) { g_game_dir = dir; }
    // 配置已经是这个条目自己的（S() 按 g_game_dir 路由），这里只做字段同步。
    e->st.interp = e->st.fg_mode + 2;
    e->mult = e->st.fg_mode + 2;
    e->fg_input = e->st.fg_input;
    e->lock = e->st.lock_fps;
}

// 选中一个游戏：后续所有设置都写进它自己的配置；FgInput 以它的 fakerFG.ini 为准。
static bool select_game(int idx)
{
    name_edit_commit(false);
    if (idx < 0 || idx >= (int)g_games.size()) { return false; }
    GameEntry& e = g_games[(size_t)idx];
    ensure_entry_resolved(e);
    std::wstring d = e.dir;
    if (d.empty()) { d = e.root; }
    if (d.empty()) {
        set_msg(T(L"这个条目还没有路径：用[主程序…]指定游戏 exe",
                  L"No path for this entry: pick the exe with [Main exe...]"));
        return false;
    }
    e.dir = d;
    g_game_dir = d;
    apply_game(d);
    sync_fg_input_from_dir(d);
    e.last_use = (unsigned long long)std::time(nullptr);
    g_user_picked = false;
    g_last_auto_hwnd = nullptr;
    ini_ws(L"ui", L"game_dir", g_game_dir);
    save_all();
    refresh_readiness();
    return true;
}

// 安装完成 = 应用这个游戏保存的配置（写入它的 fakerFG.ini）。
static void apply_config_after_deploy(const std::wstring& dir)
{
    if (dir.empty()) { return; }
    g_game_dir = dir;
    apply_fg_input();
    refresh_readiness();
    save_all();
}

static void name_edit_start()
{
    GameEntry* e = find_game_mut(g_game_dir);
    if (e == nullptr) { return; }
    g_name_edit = true;
    g_name_fresh = true;
    g_name_buf = e->name;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void name_edit_commit(bool save)
{
    if (!g_name_edit) { return; }
    if (save) {
        std::wstring t = g_name_buf;
        while (!t.empty() && iswspace(t.front())) { t.erase(t.begin()); }
        while (!t.empty() && iswspace(t.back())) { t.pop_back(); }
        if (!t.empty() && t.size() <= 64) {
            GameEntry* e = find_game_mut(g_game_dir);
            if (e != nullptr) {
                const std::wstring old_name = e->name;
                e->name = t;
                // 配置文件如果是按旧名字自动命名的，跟着改名（失败就保持原路径）。
                if (!e->profile.empty()) {
                    const std::wstring want =
                        L"profiles\\" + sanitize_file_name(old_name) + L".ini";
                    if (_wcsicmp(e->profile.c_str(), want.c_str()) == 0) {
                        const std::wstring rel = new_profile_rel(t, e);
                        if (_wcsicmp(rel.c_str(), e->profile.c_str()) != 0 &&
                            MoveFileW(profile_abs(e->profile).c_str(),
                                      profile_abs(rel).c_str())) {
                            e->profile = rel;
                        }
                    }
                }
                save_games();
                set_msg(T(L"已重命名游戏配置", L"Game renamed"));
            }
        }
    }
    g_name_edit = false;
    g_name_fresh = false;
    g_name_buf.clear();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void save_all()
{
    ini_w(std::wstring(L"ui"), L"count", (int)g_profiles.size());
    ini_w(std::wstring(L"ui"), L"active", g_prof);
    ini_w(std::wstring(L"ui"), L"advanced", g_advanced ? 1 : 0);
    ini_w(std::wstring(L"ui"), L"scale_pct", g_scale_pct);
    ini_w(std::wstring(L"ui"), L"lang", g_lang);
    ini_w(std::wstring(L"ui"), L"show_cmd", g_show_cmd ? 1 : 0);
    ini_ws(std::wstring(L"ui"), L"game_dir", g_game_dir);
    ini_w(std::wstring(L"ui"), L"hk_custom", g_hk_custom ? 1 : 0);
    ini_w(std::wstring(L"ui"), L"hk_mods", (int)g_hk_mods);
    ini_w(std::wstring(L"ui"), L"hk_vk", (int)g_hk_vk);
    for (int i = 0; i < (int)g_profiles.size(); ++i) { save_profile(i); }
    save_games();
}

static void load_all()
{
    ensure_utf16_bom(ini_path());
    g_advanced = ini_r(L"ui", L"advanced", 0) != 0;
    g_scale_pct = ini_r(L"ui", L"scale_pct", 0);
    if (g_scale_pct != 0 && (g_scale_pct < 80 || g_scale_pct > 300)) { g_scale_pct = 0; }
    g_lang = ini_r(L"ui", L"lang", 0);
    if (g_lang < 0 || g_lang > 1) { g_lang = 0; }
    g_show_cmd = ini_r(L"ui", L"show_cmd", 0) != 0;
    g_welcome = ini_r(L"ui", L"welcomed", 0) == 0;
    load_games();
    g_game_dir = ini_rs(L"ui", L"game_dir", L"");
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

    // 游戏库要等 profile 和 game_dir 都读完之后再覆盖倍率/输入源/锁帧。
    if (!g_game_dir.empty()) { apply_game(g_game_dir); }

    // 启动时落一次盘：生成/刷新 profiles\*.ini，并清掉旧格式的内联字段。
    save_games();
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
    bool matched = false;
    if (!want.empty()) {
        for (size_t i = 0; i < g_wins.size(); ++i) {
            if (g_wins[i].title == want) { g_win_sel = (int)i; matched = true; break; }
        }
    }
    // 没有记忆命中时，优先选"已经加载 producer"的那个窗口：它就是我们要的游戏，
    // 而不是列表里正好最大的浏览器。
    if (!matched) {
        for (size_t i = 0; i < g_wins.size(); ++i) {
            if (proc_has_module(g_wins[i].pid, L"xess_mfg_producer.dll")) {
                g_win_sel = (int)i;
                break;
            }
        }
    }
    if (g_win_sel >= 0) {
        S().win_title = g_wins[g_win_sel].title;
        S().win_hwnd = g_wins[g_win_sel].hwnd;
    }
}


// ------------------------------------------------------ lock_frame process
static std::wstring build_cmdline(HWND target, bool display)
{
    std::wstring cmd = display ? std::wstring(L"lock_frame.exe")
                               : (L"\"" + exe_dir() + L"\\lock_frame.exe\"");
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

    // 固定倍率：2x / 3x / 4x。--mode 直接决定插帧数；输入锁仍由游戏侧完成。
    // 这一段与定稿配方 lock_frame/run_3x.bat（2026-11-21 实测）逐项对齐：
    //   --mode Nx --interp N-1   两个都给，脚本里就是这么写的，日志能一眼对上；
    //   --streams=mv,depth       不再请求 stream 3；patch 走 --hudless-stream；
    //   --ui-source off          关闭 UI 平面合成；patch 仍由 stream 4 驱动；
    //   --wgc-minint 0           显式关掉 WGC 的 ~16 ms 交付节流（默认也是 0，写出来便于核对）。
    const int mul = S().fg_mode + 2;
    wchar_t b[256]{};
    // mode 4 patch path: stream 4 carries the support patch; stream 3 / mode 5 is not
    // requested. --ui-source off disables UI unblend/synthesis but --hudless-stream keeps
    // the patch stream and the consumer's H' reconstruction.
    _snwprintf_s(b, _TRUNCATE,
                 L" --pool %d --latency %d --mode %dx --interp %d"
                 L" --streams=mv,depth --wgc-minint 0",
                 S().pool, S().latency, mul, mul - 1);
    cmd += b;
    // 1.2.9: the consumer's UI composition is OFF unless --ui-unblend / --ui-source is
    // given (g_ui_unblend is CLI-only). Patch mode is driven by --hudless-stream.
    if (S().ui_compose) {
        cmd += L" --hudless-stream --ui-source off";
    }
    // v30: no --interp-cadence here. The continuous frameRenderTime feed (--fg-rt-mode auto)
    // keeps slow scenes smooth while the slot gate stays the single pacing owner.
    // Which GPU runs the frame generation (A card rendering + N card output, or the
    // other way round). 0 = let bridge decide from the window monitor / producer LUID.
    if (S().gpu_sel > 0 && S().gpu_sel <= (int)g_adapters.size()) {
        wchar_t g[64]{};
        _snwprintf_s(g, _TRUNCATE, L" --gpu %d",
                     g_adapters[(size_t)S().gpu_sel - 1].index);
        cmd += g;
    }

    // Fixed part: reversed-Z projection, no UI texture, 3x..6x unlock, and the
    // vblank-snapped frame-time feed. jitter is NOT in the fixed part - see below.
    //
    // --fg-rt-mode auto (v30): a continuous, slew-limited feed, not a mode switch.
    //   target = max(m*vblank, game-clock median), 2% deadband, +0.5 ms per batch,
    //   instant recovery. 40 fps -> 25.0 ms (1 output/vblank); a 33 fps scene ramps to
    //   ~31 ms over 0.35 s instead of flipping quant<->raw. See
    //   docs/design/LOCK_FRAME_IMPROVEMENTS.md 12.5d/12.5e.
    if (S().overlay_layered) { cmd += L" --overlay-layered"; }
    if (S().relay_input) { cmd += L" --relay-input"; }
    if (!S().osd) { cmd += L" --no-osd"; }
    // 色彩空间：默认 HDR10（BT.2020 + PQ）。选 SDR 时发 --sdr，由 bridge 切到 BT.709 +
    // gamma 2.2 并把交换链色彩空间声明成 SDR（g_colour_mode = 2）。
    // 我们捕获的是 scRGB(FP16)，所以 HDR 源永远不丢；SDR 只是"编码成 SDR 再上屏"，
    // 在 HDR 显示器上会明显发灰 —— 那是模式本身，不是链路出错。
    if (S().sdr) { cmd += L" --sdr"; }
    // 抗丢帧（--slack-auto）已从 UI 移除（2026-11-10）：对插帧它是负优化，见 kRowsFg 的注释。
    // 参数仍保留在 lock_frame.exe 上供 A/B。
    // --fg-proj-rz is NOT emitted any more: XeFG 1.3.1 ignores projectionMatrix (it only
    // reprojects with the motion vectors - docs/findings/FINAL_CONFIG.md 5.16), and the flag
    // only affects the synthetic matrix our own --fg-warp/--fg-of paths would use.
    // Current pipeline: publications are consumed in order and the in-frame marker is the
    // pairing key (see docs/findings/PERF_ROUND_HANDOVER.md). --report 40 keeps the log
    // readable while still showing every phase of a slow frame.
    // B mode 4: do NOT force --fg-ui none. UI composition is gated by the producer's
    // UI stream; with no stream XeFG stays on its default (NONE), with one the consumer
    // unblends a HUDless and falls back to UI-only mode 2 if that fails.
    //
    // --upsample is NOT emitted: consumer 160_main.inc sets g_upsample = !--no-upsample
    // (ON by default, reconstructed to the input picture size before tagging), and the
    // verified recipe run_3x.bat does not pass it either. The old hardcoded flag made the
    // UI command look different from the measured one while changing nothing.
    cmd += L" --fg-rt-mode auto --report 40";
    // 2x 不需要解锁：原版库就支持 1 张插值帧。3x/4x 继续沿用现有的字节补丁，
    // 保证非 Intel 卡上开箱即用；补丁失败时 consumer 会回退并打日志。
    if (mul > 2) { cmd += L" --unlock-mfg"; }
    // keep2x is the consumer default (150_probe_mfg.inc: g_mfg_model = 2), but the verified
    // recipe states it explicitly; keep the UI command identical so logs line up.
    cmd += L" --mfg-model=keep2x";
    // OptiScaler 的逐游戏输入表：默认 apply（只套用安全项，CLI 优先）。
    cmd += S().game_quirks ? L" --game-quirks apply" : L" --game-quirks off";
    // FOV and 输入间隔 are no longer exposed in the UI (2026-11-08): FOV only feeds the
    // synthetic projection, which the XeFG frame-generation path ignores (5.16), and
    // --input-ms no longer matters for pacing now that frameRenderTime is vblank-snapped.
    // Both remain available as consumer command-line switches.
    // 运动矢量：UI 不再选择。bridge 每帧读取 producer 的 ProducerInfo/StreamShared 自动决定
    // （游戏 MV / 光流 / 零矢量），日志一行 "mv: auto -> ..."。命令行开关仍然保留。
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
    // 简易模式的状态卡要把"exact/total"翻成一句人话，所以单独留一份。
    if (strstr(s.c_str(), "pair:")) {
        const int exact = (int)num("exact=");
        const int total = (int)num("(total=");
        if (total > 0) {
            g_pair_exact = exact;
            g_pair_total = total;
        }
    }
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
    _snwprintf_s(name, _TRUNCATE, L"\\lock_frame_ui_%02u%02u%02u.txt",
                 st.wHour, st.wMinute, st.wSecond);
    const std::wstring log_path = exe_dir() + name;
    uilog("consumer stdout -> %ls", name + 1);
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
            if (buf[i] == '\n') { parse_line(acc); log_line_utf8(acc); acc.clear(); }
            else if (buf[i] != '\r') { acc.push_back(buf[i]); }
        }
        if (g_hwnd) { PostMessageW(g_hwnd, WM_APP + 1, 0, 0); }
    }
    if (log != INVALID_HANDLE_VALUE) { CloseHandle(log); }
    return 0;
}

// bridge clears the game's WS_EX_TOPMOST itself on every exit path (hard_exit); this
// is the safety net for the paths it cannot cover - the TerminateProcess fallback
// above, a crash, or the user killing the consumer from Task Manager.
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
        // We lost track of the process (stale state): kill whatever consumer is left.
        const int k = kill_stray_bridge();
        uilog("stop: no pid, killed %d stray lock_frame.exe", k);
    }

    if (g_stdout_rd) { CloseHandle(g_stdout_rd); g_stdout_rd = nullptr; }
    if (g_child) { CloseHandle(g_child); g_child = nullptr; }
    clear_target_topmost();
    g_child_pid = 0;
    g_running = false;
    g_show = 0; g_xn = 0; g_skip = 0; g_gpu = 0; g_game = 0; g_present = 0;
    g_pair_exact = 0; g_pair_total = 0;
}

// A target only makes sense if it is the process that created the shared mailbox,
// i.e. the one running fakerFG + xess_mfg_producer.dll. Without this check, pressing the
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

static void start_bridge(bool user_initiated)
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
            // ppid == 0 covers two very different situations, and the old text said "no add-on"
            // for both - which sent people looking for a missing file when the add-on was
            // loaded and simply had not captured a frame yet (the shared block is created on the
            // first captured frame, because it needs the game's device and stream sizes).
            // Wuchang/PBZ, 2026-11-10.
            _snwprintf_s(m, _TRUNCATE,
                         L"目标进程 (pid %lu) 还没有抓到第一帧：确认游戏里超分（DLSS / XeSS）"
                         L"已开启、且已进入游戏画面，再按热键；若一直如此，把该目录的 "
                         L"fakerFG.ini 里 Log=1 后重开游戏，看 <exe>_mfg_producer.log",
                         (unsigned long)tpid);
        }
        set_msg(m);
        return;
    }
    const std::wstring consumer = exe_dir() + L"\\lock_frame.exe";
    if (GetFileAttributesW(consumer.c_str()) == INVALID_FILE_ATTRIBUTES) {
        set_msg(L"lock_frame_ui.exe 旁边找不到 lock_frame.exe");
        uilog("start: lock_frame.exe missing next to %ls", exe_dir().c_str());
        return;
    }

    // 启动前检查（只在用户主动点启动时弹，热键不打断游戏）。全部是"可以继续"的
    // 警告：宁可让用户带着已知问题跑一次，也不要把它挡在门外。
    if (user_initiated) {
        refresh_readiness();
        std::wstring warn;
        if (g_adapters.size() < 2) {
            warn += L"· 只检测到 1 块 GPU：可以跑，但不建议（插帧会和游戏抢同一张卡）\r\n";
        }
        if (!exe_file_exists(L"libxess_fg.dll")) {
            warn += L"· 缺少 libxess_fg.dll（Intel XeSS-FG 运行时，正式包默认不内置）\r\n";
        } else if (!g_game_dir.empty() && !game_runtime_ok(g_game_dir)) {
            // XeFG 在游戏进程里加载它；没有这份运行时，swap chain 拿不到，
            // producer_present() 不执行，共享内存不创建（UI 会报 not the producer）。
            warn += L"· 游戏目录里没有 XeSS-FG 运行时（<游戏>\\fakerFG\\libxess_fg.dll）："
                    L"点[重新应用配置]会自动写入，否则 producer 不会启动\r\n";
        }
        if (S().fg_mode >= 1 && !exe_file_exists(L"libxell.dll")) {
            warn += L"· 缺少 libxell.dll（3x / 4x 需要）\r\n";
        }
        if (!g_game_dir.empty() && !faker_ini_ok(g_game_dir)) {
            warn += L"· 游戏目录的 fakerFG.ini 不是消费者链路配置"
                    L"（FgOutput 应为 Capture、Passthrough=0）\r\n";
        }
        const unsigned hz = (g_refresh > 1) ? g_refresh : 120;
        const int mul = S().fg_mode + 2;
        if (hz % static_cast<unsigned>(mul) != 0) {
            wchar_t b[160]{};
            _snwprintf_s(b, _TRUNCATE,
                         L"· 面板 %u Hz 不能被 %dx 整除：输出节奏不会完美\r\n", hz, mul);
            warn += b;
        }
        if (!g_ready.producer) {
            warn += L"· 还没检测到 producer：确认游戏已开 DLSS 帧生成并进入实际画面\r\n";
        }
        if (!warn.empty()) {
            warn = L"启动前检查发现：\r\n\r\n" + warn + L"\r\n仍然启动吗？";
            if (MessageBoxW(g_hwnd, warn.c_str(), L"启动前检查",
                            MB_YESNO | MB_ICONWARNING) != IDYES) {
                uilog("start: cancelled by preflight");
                return;
            }
        }
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

    const BOOL ok = CreateProcessW(consumer.c_str(), mc.data(), nullptr, nullptr, TRUE,
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
    // 成功启动即记住这个游戏的目录和当前倍率/输入源/锁帧，下次直接恢复。
    {
        const std::wstring tdir = game_dir_for_window(target);
        if (!tdir.empty()) {
            g_game_dir = tdir;
            remember_game(tdir);
        }
    }
    save_all();
    uilog("start: pid=%lu cmd=%ls", (unsigned long)g_child_pid, cmd.c_str());
    // Point out the one combination that pays for MV without seeing the benefit: at 3x the
    // generated frames come from the patched 2x model applied twice, and that model-edge
    // ghosting dominates whatever the motion vectors fix (measured 2026-11-08).
    if (false) {
    } else {
        set_msg(L"锁帧消费端已启动（lock_frame.exe）");
    }
}

static void toggle_bridge(bool user_initiated)
{
    uilog("hotkey/button: running=%d child_pid=%lu", (int)g_running, (unsigned long)g_child_pid);
    if (g_running) { stop_bridge(); set_msg(L"消费端已停止"); }
    else           { start_bridge(user_initiated); }
    if (g_hwnd) { InvalidateRect(g_hwnd, nullptr, FALSE); }
}


// ------------------------------------------------------------------- layout
static const int kWinW = 1240, kWinH = 720;
static const int kTitleH = 42;
static const int kSideW = 232;
static const int kMargin = 20;
static const int kGap = 16;
// 统一的圆角与控件高度：卡片 10、控件 7；紧凑按钮 30、常规 36、主按钮 44+
static const int RAD_CARD = 12;
static const int RAD_CTL  = 7;
static const int BTN_H    = 36;
static const int kCardHead = 40;
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

// 鼠标坐标是设备像素，绘制用的是逻辑坐标：所有命中测试前先换算一次。
static POINT to_logical(int mx, int my)
{
    POINT p{ mx, my };
    RECT cr{};
    if (g_hwnd != nullptr && GetClientRect(g_hwnd, &cr) &&
        cr.right > 0 && cr.bottom > 0) {
        p.x = static_cast<LONG>((long long)p.x * kWinW / cr.right);
        p.y = static_cast<LONG>((long long)p.y * kWinH / cr.bottom);
    }
    return p;
}

// 启动命令默认折叠：折叠时和普通行一样高，点一下展开成 106px 的换行命令行。
static int row_h(RowKind k, Id id = ID_NONE)
{
    if (k == RK_CMDLINE) {
        return (id == ID_CMD && !g_show_cmd) ? kRowH : 106;
    }
    return kRowH;
}

static int ctl_w(RowKind k)
{
    switch (k) {
    case RK_DD:   return 210;
    case RK_EDIT: return 210;
    case RK_SPIN: return 116;
    case RK_TOG:  return 46;
    default:      return 0;
    }
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
    const wchar_t* text = s.c_str();
    if (const wchar_t* e = tr_lookup(text)) { text = e; }
    DrawTextW(dc, text, -1, &rc, fmt | DT_SINGLELINE | DT_NOPREFIX);
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

// -- 手输数字框：输出帧率上限用。点一下开始编辑，只吃数字与退格，Enter/点击别处提交，
// Esc 取消。没有单独的输入控件，所以状态都放在这两个全局里，由 wnd_proc 的 WM_CHAR 驱动。
static Id          g_edit_id  = ID_NONE;
static std::wstring g_edit_buf;

static void draw_edit(HDC dc, const RECT& r, const std::wstring& value, bool hover,
                      bool editing)
{
    RECT b{ r.right - 240, r.top + 4, r.right, r.bottom - 4 };
    stroke_round(dc, b, 6, (hover || editing) ? C_CTL_HOVER : C_CTL,
                 editing ? RGB(120, 170, 255) : C_BORDER);
    RECT vt{ b.left + 10, b.top, b.right - 26, b.bottom };
    txt(dc, g_f_ui, vt, value, editing ? C_TEXT : C_TEXT,
        DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    if (editing) {
        // Caret after the text: measure it, then draw a 1px bar.
        SIZE sz{};
        HGDIOBJ old = SelectObject(dc, g_f_ui);
        GetTextExtentPoint32W(dc, value.c_str(), (int)value.size(), &sz);
        SelectObject(dc, old);
        const int cx = b.left + 10 + sz.cx + 1;
        RECT caret{ cx, b.top + 6, cx + 1, b.bottom - 6 };
        fill_round(dc, caret, 0, RGB(220, 230, 255));
    }
}

static void draw_dd(HDC dc, const RECT& r, const std::wstring& value, bool hover, bool open)
{
    RECT b{ r.right - 240, r.top + 4, r.right, r.bottom - 4 };
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
        wchar_t b[96]{};
        const unsigned hz = (g_refresh > 1) ? g_refresh : 120;
        const int mul = S().fg_mode + 2;
        _snwprintf_s(b, _TRUNCATE, L"锁 %.1f fps（%dx -> %u fps）",
                     static_cast<double>(hz) / static_cast<double>(mul), mul, hz);
        return b;
    }
    // Cadence + motion vectors, measured 2026-11-09:
    //   --fg-of  = one more full GPU pass on the card that also generates. A 30 s segment
    //              looked clean, sustained play drops clearly more often (the user caught
    return opt_text(id, opt_get(id));
}

// 简易模式的窗口列表可能很长：先按可用高度压缩行高，再决定向上/向下展开。
// draw_popup 与 popup_hit 必须用同一套计算，否则会"看得见点不到"。
static int popup_row_h(int n)
{
    int rowH = 30;
    const int avail = kWinH - kBottomH - kTitleH - 12;
    while (n * rowH + 8 > avail && rowH > 18) { rowH -= 2; }
    return rowH;
}

static int popup_top(int n, int rowH)
{
    const int h = n * rowH + 8;
    int y = g_popup_rect.bottom + 2;
    if (y + h > kWinH - kBottomH) { y = g_popup_rect.top - h - 2; }   // open upwards
    if (y < kTitleH + 4) { y = kTitleH + 4; }
    return y;
}

static void draw_popup(HDC dc)
{
    if (!g_popup_id) { return; }
    const Id id = (Id)g_popup_id;
    const int n = opt_count(id);
    if (n <= 0) { return; }
    const int rowH = popup_row_h(n);
    const int w = (int)(g_popup_rect.right - g_popup_rect.left);
    const int x = g_popup_rect.left;
    const int y = popup_top(n, rowH);
    const int h = n * rowH + 8;
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

static bool hit_rect(const RECT& r, int x, int y);
static RECT help_rect_entry();
static RECT help_rect_close();

static RECT title_min_rect()   { return RECT{ kWinW - 92, 0, kWinW - 52, kTitleH }; }
static RECT title_close_rect() { return RECT{ kWinW - 48, 0, kWinW - 8,  kTitleH }; }

// 「高级设置」与安装/还原同一行，右对齐（定义在卡片常量之后）。

// toggle_advanced 定义在抽屉全局变量之后（见 paint_drawer 附近）。
static void toggle_advanced();

static void draw_titlebar(HDC dc, int W)
{
    fill_round(dc, RECT{ 0, 0, W, kTitleH }, 0, RGB(28, 28, 28));
    fill_round(dc, RECT{ 0, kTitleH - 1, W, kTitleH }, 0, RGB(44, 44, 44));

    // 品牌点 + 标题；标题栏只保留最小化/关闭，启动按钮不在这里重复。
    fill_round(dc, RECT{ 14, 13, 28, 27 }, 4, C_ACCENT);
    txt(dc, g_f_ui, RECT{ 34, 0, W - 160, kTitleH }, L"MGPU  双显卡帧生成", C_TEXT,
        DT_LEFT | DT_VCENTER);

    // 标题栏只保留最小化/关闭；高级入口放到主内容区右上角。
    const RECT mn = title_min_rect();
    const RECT cl = title_close_rect();
    if (hit_rect(mn, g_hover_x, g_hover_y)) { fill_round(dc, mn, 4, RGB(48, 48, 48)); }
    if (hit_rect(cl, g_hover_x, g_hover_y)) { fill_round(dc, cl, 4, RGB(160, 60, 60)); }
    txt(dc, g_f_ui, mn, L"\u2013", C_TEXT, DT_CENTER | DT_VCENTER);
    txt(dc, g_f_ui, cl, L"\u2715", C_TEXT, DT_CENTER | DT_VCENTER);
}


static const int kSideRowH = 46;
static int g_side_scroll = 0;   // 侧栏游戏列表滚动了多少行
static int side_rows_max()
{
    const int top = kTitleH + 42;
    const int bottom = kWinH - 144;   // 下方只留"帮助"
    return (bottom > top) ? ((bottom - top) / kSideRowH) : 1;
}
static int side_max_scroll()
{
    const int m = (int)g_games.size() - side_rows_max();
    return (m > 0) ? m : 0;
}
static void side_ensure_visible(int idx)
{
    const int rows = side_rows_max();
    if (idx < g_side_scroll) { g_side_scroll = idx; }
    if (idx >= g_side_scroll + rows) { g_side_scroll = idx - rows + 1; }
    if (g_side_scroll < 0) { g_side_scroll = 0; }
    if (g_side_scroll > side_max_scroll()) { g_side_scroll = side_max_scroll(); }
}
static RECT side_row_rect(int slot)
{
    const int y = kTitleH + 42 + slot * kSideRowH;
    return RECT{ 10, y, kSideW - 10, y + kSideRowH - 2 };
}

static void draw_sidebar(HDC dc)
{
    fill_round(dc, RECT{ 0, kTitleH, kSideW, kWinH }, 0, C_SIDEBAR);
    fill_round(dc, RECT{ kSideW - 1, kTitleH, kSideW, kWinH }, 0, RGB(34, 34, 34));
    txt(dc, g_f_small, RECT{ 16, kTitleH + 12, kSideW - 16, kTitleH + 34 },
        T(L"游戏", L"Games"), C_DIM, DT_LEFT | DT_VCENTER);
    {
        wchar_t cnt[32]{};
        _snwprintf_s(cnt, _TRUNCATE, L"%d", (int)g_games.size());
        txt(dc, g_f_small, RECT{ kSideW - 60, kTitleH + 12, kSideW - 16, kTitleH + 34 },
            cnt, C_DIM, DT_RIGHT | DT_VCENTER);
    }

    const int rows = side_rows_max();
    const int n = (int)g_games.size();
    if (n == 0) {
        txt(dc, g_f_small, RECT{ 16, kTitleH + 44, kSideW - 16, kTitleH + 96 },
            T(L"还没有游戏：点右边[选择游戏]",
              L"No games yet: click [Choose game] on the right"),
            C_DIM, DT_LEFT | DT_WORDBREAK);
    }

    const GameEntry* cur = find_game(g_game_dir);
    const int first = g_side_scroll;
    for (int slot = 0; slot < rows && first + slot < n; ++slot) {
        const GameEntry& e = g_games[(size_t)(first + slot)];
        const RECT rc = side_row_rect(slot);
        const bool sel = (cur == &e);
        const bool hv = hit_rect(rc, g_hover_x, g_hover_y);
        if (sel || hv) {
            fill_round(dc, rc, RAD_CTL, sel ? RGB(44, 44, 44) : RGB(32, 32, 32));
        }
        if (sel) {
            fill_round(dc, RECT{ rc.left, rc.top + 8, rc.left + 3, rc.bottom - 8 }, 2, C_ACCENT);
        }
        const bool inst = game_entry_installed(e);
        draw_dot(dc, rc.left + 12, (rc.top + rc.bottom) / 2, inst ? C_OK : C_DIM);
        txt(dc, g_f_ui, RECT{ rc.left + 24, rc.top, rc.right - 8, rc.bottom }, e.name,
            sel ? C_TEXT : C_LABEL, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }
    // 超出可见行数：右侧细滚动条（滚轮可直接翻）
    if (n > rows) {
        const int top = kTitleH + 42;
        const int track = rows * kSideRowH - 4;
        const int thumb = (int)((long long)track * rows / n);
        const int maxs = side_max_scroll();
        const int ty = top + 2 + (int)((long long)(track - thumb) * g_side_scroll /
                                      (maxs ? maxs : 1));
        fill_round(dc, RECT{ kSideW - 7, top + 2, kSideW - 3, top + 2 + track }, 2,
                   RGB(34, 34, 34));
        fill_round(dc, RECT{ kSideW - 7, ty, kSideW - 3, ty + thumb }, 2, RGB(90, 90, 90));
    }

    // 底部：帮助 + 版本（选游戏 / 高级入口都不再放在侧栏）
    const RECT help = help_rect_entry();
    if (hit_rect(help, g_hover_x, g_hover_y)) {
        fill_round(dc, help, RAD_CTL, RGB(32, 32, 32));
    }
    txt(dc, g_f_ui, RECT{ help.left + 14, help.top, help.right - 8, help.bottom },
        L"帮助 / 快捷键 (F1)", C_LABEL, DT_LEFT | DT_VCENTER);

    wchar_t ver[96]{};
    _snwprintf_s(ver, _TRUNCATE, T(L"v%s · 协议 v%u", L"v%s · protocol v%u"),
                 LOCK_FRAME_VERSION_WSTRING, mgpu::kSharedVersion);
    txt(dc, g_f_small, RECT{ 16, kWinH - 34, kSideW - 16, kWinH - 10 }, ver, C_DIM,
        DT_LEFT | DT_VCENTER);
}

// 简易模式的绘制在 paint() 之后定义（它要用全部 draw_* 助手）。
static void paint_simple(HDC dc);
static void paint_drawer(HDC dc);
static void draw_tip(HDC dc);
static void layout_drawer_items(std::vector<Item>& out, int* content_h);
static void draw_log_view(HDC dc);
static void draw_welcome(HDC dc);
static void draw_library(HDC dc);
static void draw_help(HDC dc);
static void draw_exe_picker(HDC dc);

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

    // 逻辑坐标 -> 设备像素。窗口按 g_scale 建好尺寸，这里只做等比映射；离屏位图
    // 仍是设备尺寸，所以文字是"按目标大小重新栅格化"的，不是把 1200x620 拉大。
    const int vw = (cl.right > 0) ? (int)cl.right : kWinW;
    const int vh = (cl.bottom > 0) ? (int)cl.bottom : kWinH;
    SetMapMode(dc, MM_ANISOTROPIC);
    SetWindowExtEx(dc, kWinW, kWinH, nullptr);
    SetViewportExtEx(dc, vw, vh, nullptr);
    SetWindowOrgEx(dc, 0, 0, nullptr);
    SetViewportOrgEx(dc, 0, 0, nullptr);

    auto finish = [&]() {
        SetMapMode(dc, MM_TEXT);              // 还原成 1:1 设备拷贝
        BitBlt(wdc, 0, 0, vw, vh, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old_bmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
    };

    fill_round(dc, RECT{ 0, 0, kWinW, kWinH }, 0, C_BG);
    draw_titlebar(dc, kWinW);
    draw_sidebar(dc);

    // 单页布局：三步卡片始终在；高级设置改成右侧抽屉覆盖，不再整页切换。
    paint_simple(dc);
    paint_drawer(dc);
    draw_log_view(dc);
    draw_library(dc);
    draw_exe_picker(dc);
    draw_help(dc);
    draw_welcome(dc);
    draw_popup(dc);
    draw_tip(dc);
    finish();
    return;

}


// ============================================================================
//  简易模式（P0）：三步 + 状态卡
//  小白只需要：① 装进游戏  ② 选中游戏窗口  ③ 选倍率并启动。
//  老手点左侧「高级设置」回到原来的卡片视图，两套界面共享同一份 State。
// ============================================================================
static const int kSC1 = 94,  kSC1B = 308;      // 当前游戏（选择 + 安装/还原/高级 + 配置）
static const int kSC2 = 322, kSC2B = 528;      // 启动补帧（窗口 + 倍率 + 启动）
static const int kSST = 542, kSSTB = 700;      // 运行状态

// 与安装/还原同一行，右对齐：高级设置（次级）在主程序… 左侧。
static RECT adv_rect()
{
    const int x1 = kWinW - kMargin;
    return RECT{ x1 - 266, kSC1B - 48, x1 - 146, kSC1B - 12 };
}

struct SBtn { Id id; RECT rc; };

// 键盘焦点：Tab 在简易模式的按钮之间循环，Enter/Space 触发。
static int g_focus = -1;

static bool hit_rect(const RECT& r, int x, int y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// paint 与 click 共用这一份坐标，避免"看得见点不到"。
static const std::vector<SBtn>& simple_buttons()
{
    static std::vector<SBtn> v;
    const int x0 = kSideW + kMargin, x1 = kWinW - kMargin;
    v.clear();
    v.push_back({ ID_S_INSTALL,  RECT{ x0 + 16,  kSC1B - 48, x0 + 266, kSC1B - 12 } });
    v.push_back({ ID_S_UNDEPLOY, RECT{ x0 + 278, kSC1B - 48, x0 + 508, kSC1B - 12 } });
    v.push_back({ ID_S_PICKEXE,  RECT{ x1 - 136, kSC1B - 48, x1 - 16,  kSC1B - 12 } });
    // 唯一的"选择游戏"按钮：当前游戏卡片的右上角，强调色。
    v.push_back({ ID_S_GAMELIB,  RECT{ x1 - 160, kSC1 + 6, x1 - 16, kSC1 + 34 } });
    v.push_back({ ID_S_WINDOW,   RECT{ x0 + 16,  kSC2 + 62, x1 - 148,  kSC2 + 100 } });
    v.push_back({ ID_S_REFRESH,  RECT{ x1 - 136, kSC2 + 62, x1 - 16,   kSC2 + 100 } });
    // 倍率按钮铺满左侧（启动按钮固定 200 px 在右端）
    const int mw = (x1 - 216 - 12 - (x0 + 16) - 12) / 3;
    for (int i = 0; i < 3; ++i) {
        RECT r{ x0 + 16 + i * (mw + 6), kSC2 + 112, x0 + 16 + i * (mw + 6) + mw, kSC2 + 172 };
        v.push_back({ (Id)(ID_S_2X + i), r });
    }
    v.push_back({ ID_S_START, RECT{ x1 - 216, kSC2 + 112, x1 - 16, kSC2 + 172 } });
    v.push_back({ ID_S_LOGVIEW, RECT{ x1 - 316, kSST + 4, x1 - 176, kSST + 40 } });
    v.push_back({ ID_S_DIAG,  RECT{ x1 - 164, kSST + 4,  x1 - 16,  kSST + 40 } });
    return v;
}

static void draw_btn(HDC dc, const RECT& r, const std::wstring& s, bool primary, bool hover,
                     bool disabled = false)
{
    COLORREF fill = C_BTN;
    COLORREF border = C_BORDER;
    COLORREF text = C_TEXT;
    if (disabled) {
        fill = RGB(33, 33, 33);
        border = RGB(44, 44, 44);
        text = C_DIM;
    } else if (primary) {
        fill = hover ? RGB(74, 136, 212) : RGB(62, 120, 196);
        border = fill;
        text = RGB(252, 252, 252);
    } else {
        fill = hover ? C_BTN_HOVER : C_BTN;
    }
    stroke_round(dc, r, RAD_CTL, fill, border);
    txt(dc, g_f_ui, r, s, text, DT_CENTER | DT_VCENTER);
}

// 步骤徽章：① ② ③ 用圆形数字，完成态用绿色描边。
static void draw_step_badge(HDC dc, int cx, int cy, int n, bool done)
{
    const COLORREF edge = done ? C_OK : C_ACCENT;
    HBRUSH br = CreateSolidBrush(done ? RGB(38, 74, 52) : RGB(42, 56, 76));
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    Ellipse(dc, cx - 12, cy - 12, cx + 12, cy + 12);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
    wchar_t b[8]{};
    _snwprintf_s(b, _TRUNCATE, L"%d", n);
    txt(dc, g_f_ui, RECT{ cx - 12, cy - 12, cx + 12, cy + 12 }, b, C_TEXT,
        DT_CENTER | DT_VCENTER);
}

static void draw_dot(HDC dc, int cx, int cy, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    Ellipse(dc, cx - 5, cy - 5, cx + 5, cy + 5);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void draw_focus(HDC dc, const RECT& r)
{
    RECT b = r;
    InflateRect(&b, 2, 2);
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(150, 200, 255));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, b.left, b.top, b.right, b.bottom, 9, 9);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

// ------------------------------------------------------------------ 日志面板
// 模态覆盖层：整窗压暗 + 面板，显示最近 400 行（consumer stdout + UI 消息）。
static const int kLogL = kSideW + 40;
static const int kLogT = kTitleH + 26;
static const int kLogR = kWinW - 40;
static const int kLogB = kWinH - 34;
static const int kLogLineH = 20;

static RECT log_rect_refresh() { return RECT{ kLogR - 206, kLogT + 4, kLogR - 118, kLogT + 34 }; }
static RECT log_rect_close()   { return RECT{ kLogR - 104, kLogT + 4, kLogR - 14,  kLogT + 34 }; }
static RECT log_rect_folder()  { return RECT{ kLogR - 320, kLogT + 4, kLogR - 216, kLogT + 34 }; }
static RECT log_rect_copy()    { return RECT{ kLogR - 432, kLogT + 4, kLogR - 330, kLogT + 34 }; }

static void copy_to_clipboard(const std::wstring& s)
{
    if (s.empty()) { return; }
    if (!OpenClipboard(g_hwnd)) { set_msg(L"复制失败：剪贴板被占用"); return; }
    EmptyClipboard();
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem != nullptr) {
        void* p = GlobalLock(mem);
        if (p != nullptr) {
            memcpy(p, s.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
    set_msg(L"已复制到剪贴板");
}

static int log_view_rows()
{
    const int top = kLogT + 42, bottom = kLogB - 40;
    const int rows = (bottom - top) / kLogLineH;
    return (rows > 0) ? rows : 1;
}

static int log_max_scroll()
{
    const int rows = log_view_rows();
    const int total = (int)g_log_snap.size();
    return (total > rows) ? (total - rows) : 0;
}

static void open_log_view()
{
    close_advanced_silent();
    std::vector<std::wstring> snap;
    if (g_line_cs_ready) {
        EnterCriticalSection(&g_line_cs);
        snap.assign(g_log_lines.begin(), g_log_lines.end());
        LeaveCriticalSection(&g_line_cs);
    }
    if (snap.empty()) {
        snap.push_back(T(L"（还没有日志：点「启动」跑一次消费端，这里会显示它的输出）",
                         L"(no log yet: click [Start] once and the consumer output will show here)"));
    }
    g_log_snap.swap(snap);
    g_log_view = true;
    g_log_scroll = 0;
    g_popup_id = 0;
    g_msg.clear();                     // 别让 4 秒提示盖住面板底部
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void close_log_view()
{
    g_log_view = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void log_view_scroll(int lines)
{
    g_log_scroll += lines;
    if (g_log_scroll < 0) { g_log_scroll = 0; }
    const int maxs = log_max_scroll();
    if (g_log_scroll > maxs) { g_log_scroll = maxs; }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void draw_log_view(HDC dc)
{
    if (!g_log_view) { return; }

    fill_round(dc, RECT{ 0, 0, kWinW, kWinH }, 0, RGB(12, 12, 12));
    RECT panel{ kLogL, kLogT, kLogR, kLogB };
    stroke_round(dc, panel, 10, RGB(30, 30, 30), C_BORDER);

    txt(dc, g_f_card, RECT{ panel.left + 16, panel.top + 4, panel.right - 450,
                            panel.top + 34 },
        L"运行日志（最近 400 行）", C_TEXT, DT_LEFT | DT_VCENTER);
    const RECT fo = log_rect_folder();
    const RECT rb = log_rect_refresh();
    const RECT cb = log_rect_close();
    const RECT cp = log_rect_copy();
    draw_btn(dc, cp, L"复制全部", false, hit_rect(cp, g_hover_x, g_hover_y));
    draw_btn(dc, fo, L"打开目录", false, hit_rect(fo, g_hover_x, g_hover_y));
    draw_btn(dc, rb, L"刷新", false, hit_rect(rb, g_hover_x, g_hover_y));
    draw_btn(dc, cb, L"关闭", false, hit_rect(cb, g_hover_x, g_hover_y));

    const int top = panel.top + 42;
    const int rows = log_view_rows();
    const int total = (int)g_log_snap.size();
    int first = total - rows - g_log_scroll;
    if (first < 0) { first = 0; }
    for (int i = 0; i < rows && first + i < total; ++i) {
        RECT lr{ panel.left + 12, top + i * kLogLineH, panel.right - 12,
                 top + (i + 1) * kLogLineH };
        const std::wstring& s = g_log_snap[(size_t)(first + i)];
        const bool warn = (s.find(L"WARNING") != std::wstring::npos) ||
                          (s.find(L"ERR") != std::wstring::npos) ||
                          (s.find(L"失败") != std::wstring::npos);
        txt(dc, g_f_mono, lr, s, warn ? C_WARN : C_LABEL,
            DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }

    wchar_t foot[220]{};
    _snwprintf_s(foot, _TRUNCATE,
                 T(L"共 %d 行 · 滚轮 / PageUp / PageDown 翻页 · Esc 关闭 · 完整日志在 exe 旁的 txt",
                   L"%d lines | wheel / PageUp / PageDown | Esc close | full log in the txt next to the exe"),
                 total);
    txt(dc, g_f_small, RECT{ panel.left + 14, panel.bottom - 34, panel.right - 14,
                             panel.bottom - 8 },
        foot, C_DIM, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
}

// ------------------------------------------------------------------ 欢迎页
// 首次运行一次性提示："双卡 + 无边框 + 开 FG"三件事，点一下开始就不再出现。
static const int kWL = 220, kWT = 140, kWR = kWinW - 220, kWBT = 500;

static RECT welcome_rect_ok()
{
    const int cx = (kWL + kWR) / 2;
    return RECT{ cx - 90, kWBT - 64, cx + 90, kWBT - 32 };
}

static void dismiss_welcome()
{
    g_welcome = false;
    ini_w(L"ui", L"welcomed", 1);
    save_all();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void draw_welcome(HDC dc)
{
    if (!g_welcome) { return; }

    fill_round(dc, RECT{ 0, 0, kWinW, kWinH }, 0, RGB(12, 12, 12));
    RECT p{ kWL, kWT, kWR, kWBT };
    stroke_round(dc, p, 10, RGB(30, 30, 30), C_BORDER);
    txt(dc, g_f_title, RECT{ p.left + 24, p.top + 12, p.right - 24, p.top + 48 },
        L"第一次使用？先确认这三件事", C_TEXT, DT_LEFT | DT_VCENTER);

    struct Bullet { const wchar_t* head; const wchar_t* body; };
    const Bullet bs[] = {
        { L"1  需要两块 GPU",
          L"游戏在自己的卡上渲染，帧生成在另一块卡上跑。单卡也能试，但会和游戏抢性能。" },
        { L"2  游戏要开 DLSS 帧生成",
          L"用「无边框 / 窗口化全屏」，并在游戏设置里开启 DLSS Frame Generation；"
          L"进入实际画面后再点[刷新]。" },
        { L"3  一个游戏一份配置",
          L"先在[当前游戏]卡片右上角[选择游戏]挑游戏；它的倍率和安装状态会一直保存。"
          L"装好、游戏在跑、状态变绿，就能启动补帧。" },
    };
    int y = p.top + 56;
    for (const Bullet& b : bs) {
        txt(dc, g_f_card, RECT{ p.left + 24, y, p.right - 24, y + 24 }, b.head,
            C_ACCENT, DT_LEFT | DT_VCENTER);
        RECT br{ p.left + 24, y + 24, p.right - 24, y + 70 };
        HGDIOBJ old = SelectObject(dc, g_f_ui);
        SetTextColor(dc, C_LABEL);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, b.body, -1, &br, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        y += 76;
    }

    const RECT okr = welcome_rect_ok();
    draw_btn(dc, okr, L"知道了，开始", true, hit_rect(okr, g_hover_x, g_hover_y));
    txt(dc, g_f_small, RECT{ p.left + 24, p.bottom - 34, p.right - 24, p.bottom - 10 },
        L"提示：这个窗口随时可以关掉，游戏不受影响；按热键可随时开关插帧。",
        C_DIM, DT_LEFT | DT_VCENTER);
}

// ---------------------------------------------------------------- 游戏库面板
// 扫描结果 / 手动添加 / 已配置的游戏都放这里：名称 + 来源 + 安装位置 + 状态。
// 索引是"已解析的 exe 目录 > 安装根目录"，两者都做规范化比较。
static bool         g_lib_view = false;
static int          g_lib_sel = -1;
static int          g_lib_scroll = 0;
static std::wstring g_lib_msg;

static const int kLibL = 260, kLibT = 72, kLibR = kWinW - 30;
static const int kLibRowH = 52;

static int lib_bottom()
{
    const int n = (int)g_games.size();
    int h = 54 + ((n > 0) ? n : 1) * kLibRowH + 62;
    if (h < 210) { h = 210; }
    const int maxh = kWinH - 2 * kLibT;
    if (h > maxh) { h = maxh; }
    return kLibT + h;
}
static RECT lib_panel() { return RECT{ kLibL, kLibT, kLibR, lib_bottom() }; }

static int lib_list_top() { return kLibT + 54; }
static int lib_list_bottom() { return lib_bottom() - 58; }
static int lib_rows_visible()
{
    const int h = lib_list_bottom() - lib_list_top();
    return (h > 0) ? (h / kLibRowH) : 1;
}

static RECT lib_rect_manual()  { return RECT{ kLibR - 228, kLibT + 8, kLibR - 118, kLibT + 42 }; }
static RECT lib_rect_close()   { return RECT{ kLibR - 110, kLibT + 8, kLibR - 16,  kLibT + 42 }; }

static RECT lib_row_rect(int i)
{
    const int y = lib_list_top() + i * kLibRowH - g_lib_scroll;
    return RECT{ kLibL + 18, y, kLibR - 18, y + kLibRowH };
}

static bool game_entry_installed(const GameEntry& e)
{
    const std::wstring d = !e.dir.empty() ? e.dir : e.root;
    return file_in_dir(d, L"fakerFG.dll");
}

static bool game_entry_running(const GameEntry& e)
{
    HWND w = (S().win_hwnd && IsWindow(S().win_hwnd)) ? S().win_hwnd : nullptr;
    if (w == nullptr || !g_ready.producer) { return false; }
    const std::wstring d = game_dir_for_window(w);
    return !d.empty() && find_game(d) == &e;
}

static std::wstring game_entry_path(const GameEntry& e)
{
    if (!e.dir.empty()) { return e.dir; }
    if (!e.root.empty()) { return e.root; }
    return L"(未设置路径)";
}

static void lib_ensure_visible()
{
    if (g_lib_sel < 0) { return; }
    const int top = g_lib_sel * kLibRowH;
    const int view = lib_rows_visible() * kLibRowH;
    if (top < g_lib_scroll) { g_lib_scroll = top; }
    if (top + kLibRowH > g_lib_scroll + view) { g_lib_scroll = top + kLibRowH - view; }
    if (g_lib_scroll < 0) { g_lib_scroll = 0; }
    const int maxs = (int)g_games.size() * kLibRowH - view;
    if (g_lib_scroll > maxs) { g_lib_scroll = (maxs > 0) ? maxs : 0; }
}

static void open_library()
{
    name_edit_commit(false);
    close_advanced_silent();   // 子页互斥：先收高级抽屉
    // 最近用过的排前面；名字做次级排序，顺序稳定。
    std::stable_sort(g_games.begin(), g_games.end(),
                     [](const GameEntry& a, const GameEntry& b) {
                         if (a.last_use != b.last_use) { return a.last_use > b.last_use; }
                         return a.name < b.name;
                     });
    save_games();
    g_lib_view = true;
    g_lib_scroll = 0;
    g_lib_sel = -1;
    if (!g_game_dir.empty()) {
        for (int i = 0; i < (int)g_games.size(); ++i) {
            if (norm_path(game_entry_path(g_games[(size_t)i])) == norm_path(g_game_dir)) {
                g_lib_sel = i;
                break;
            }
        }
    }
    if (g_lib_sel < 0 && !g_games.empty()) { g_lib_sel = 0; }
    g_lib_msg.clear();
    lib_ensure_visible();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 扫描/已安装的游戏在后台逐个完成初始化（4 s 一个，不卡界面）。
static void background_initialize()
{
    if (g_running || g_lib_view || g_log_view || g_help_view || g_pick_view || g_welcome) {
        return;
    }
    if (g_games.empty()) { return; }
    const size_t n = g_games.size();
    for (size_t k = 0; k < n; ++k) {
        const size_t i = (g_resolve_cursor + k) % n;
        GameEntry& e = g_games[i];
        if (e.root.empty() || !e.exe.empty() || !e.dir.empty()) { continue; }
        ensure_entry_resolved(e);
        g_resolve_cursor = i + 1;
        if (!e.exe.empty() || !e.dir.empty()) {
            uilog("bg-init: resolved entry %d", (int)i);
            save_games();
            InvalidateRect(g_hwnd, nullptr, FALSE);
        }
        return;
    }
}

static void close_library()
{
    g_lib_view = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------ 主程序候选面板
// 自动解析不确定时，把候选 exe 按分数排序列出来，点一行即设为主程序。
static int                    g_pick_owner = -1;
static std::vector<ExePick>   g_pick_list;
static bool                   g_pick_then_install = false;

static const int kPickL = 280, kPickT = 100, kPickR = kWinW - 280;
static const int kPickRowH = 54;

static int pick_bottom()
{
    int h = 60 + (int)g_pick_list.size() * kPickRowH + 48;
    if (h < 150) { h = 150; }
    const int b = kPickT + h;
    return (b > kWinH - 80) ? (kWinH - 80) : b;
}
static RECT pick_panel_rect() { return RECT{ kPickL, kPickT, kPickR, pick_bottom() }; }
static RECT pick_rect_close() { return RECT{ kPickR - 104, kPickT + 8, kPickR - 16, kPickT + 38 }; }
static RECT pick_row_rect(int i)
{
    const int y = kPickT + 60 + i * kPickRowH;
    return RECT{ kPickL + 16, y, kPickR - 16, y + kPickRowH };
}

static void close_exe_picker()
{
    g_pick_view = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void library_install_selected();   // 前向声明（选完候选后可继续安装）

static bool open_exe_picker(int owner, bool then_install)
{
    if (owner < 0 || owner >= (int)g_games.size()) { return false; }
    const GameEntry& e = g_games[(size_t)owner];
    const std::wstring base = !e.root.empty() ? e.root : e.dir;
    if (base.empty()) { return false; }
    g_pick_list.clear();
    resolve_game_exes(base, &g_pick_list, 8);
    if (g_pick_list.empty()) {
        g_lib_msg = T(L"这个目录里没有可用的 exe：请用[自选 exe…]指定",
                      L"No usable exe in this folder: use [Pick exe...]");
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return false;
    }
    g_pick_owner = owner;
    g_pick_then_install = then_install;
    g_pick_view = true;
    g_popup_id = 0;
    InvalidateRect(g_hwnd, nullptr, FALSE);
    return true;
}

static void pick_choose(int i)
{
    if (i < 0 || i >= (int)g_pick_list.size()) { return; }
    if (g_pick_owner < 0 || g_pick_owner >= (int)g_games.size()) { return; }
    GameEntry& e = g_games[(size_t)g_pick_owner];
    // 换主程序时，如果名字还是目录名，则跟着新的 exe 名更新。
    if (e.name.empty() || same_name(e.name, last_component(e.dir.empty() ? e.root : e.dir))) {
        e.name = file_stem(g_pick_list[(size_t)i].path);
    }
    e.exe = g_pick_list[(size_t)i].path;
    e.dir = g_pick_list[(size_t)i].dir;
    save_games();
    const std::wstring msg = T(L"已设置主程序：", L"Main exe set: ") + last_component(e.exe);
    const bool install_now = g_pick_then_install;
    close_exe_picker();
    set_msg(msg.c_str());
    if (install_now) { library_install_selected(); }
}

static void draw_exe_picker(HDC dc)
{
    if (!g_pick_view) { return; }
    fill_round(dc, RECT{ 0, 0, kWinW, kWinH }, 0, RGB(12, 12, 12));
    RECT p = pick_panel_rect();
    stroke_round(dc, p, RAD_CARD, RGB(28, 28, 28), C_BORDER);

    std::wstring title = T(L"选择主程序", L"Choose the main executable");
    if (g_pick_owner >= 0 && g_pick_owner < (int)g_games.size()) {
        title += L"  ·  " + g_games[(size_t)g_pick_owner].name;
    }
    txt(dc, g_f_card, RECT{ p.left + 16, p.top + 6, p.right - 120, p.top + 42 }, title,
        C_TEXT, DT_LEFT | DT_VCENTER);
    const RECT cb = pick_rect_close();
    draw_btn(dc, cb, T(L"关闭", L"Close"), false, hit_rect(cb, g_hover_x, g_hover_y));

    txt(dc, g_f_small, RECT{ p.left + 16, p.top + 36, p.right - 120, p.top + 58 },
        T(L"按匹配分数排序；点击一行即设为主程序并保存", L"Sorted by match score; click a row to set it"),
        C_DIM, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    const int n = (int)g_pick_list.size();
    for (int i = 0; i < n; ++i) {
        const RECT r = pick_row_rect(i);
        const ExePick& c = g_pick_list[(size_t)i];
        const bool hv = hit_rect(r, g_hover_x, g_hover_y);
        const bool cur = (g_pick_owner >= 0 && g_pick_owner < (int)g_games.size() &&
                          g_games[(size_t)g_pick_owner].exe == c.path);
        if (hv) { fill_round(dc, r, RAD_CTL, RGB(40, 40, 40)); }
        txt(dc, g_f_ui, RECT{ r.left + 10, r.top + 4, r.right - 10, r.top + 28 },
            (cur ? std::wstring(L"✓ ") : std::wstring()) + last_component(c.path),
            cur ? C_OK : (hv ? C_TEXT : C_LABEL),
            DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        wchar_t meta[560]{};
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        double size_mb = 0.0;
        if (GetFileAttributesExW(c.path.c_str(), GetFileExInfoStandard, &fad)) {
            const unsigned long long sz =
                ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            size_mb = (double)sz / (1024.0 * 1024.0);
        }
        _snwprintf_s(meta, _TRUNCATE, L"%.1f MB  ·  score %d  ·  %s", size_mb, c.score,
                     c.path.c_str());
        txt(dc, g_f_small, RECT{ r.left + 10, r.top + 26, r.right - 10, r.bottom - 4 },
            meta, C_DIM, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }

    txt(dc, g_f_small, RECT{ p.left + 16, p.bottom - 34, p.right - 16, p.bottom - 8 },
        T(L"Esc 关闭 · 想换别的 exe 用[自选 exe…]", L"Esc to close · use [Pick exe...] for anything else"),
        C_DIM, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
}

static void draw_library(HDC dc)
{
    if (!g_lib_view) { return; }

    fill_round(dc, RECT{ 0, 0, kWinW, kWinH }, 0, RGB(12, 12, 12));
    RECT p = lib_panel();
    stroke_round(dc, p, 10, RGB(28, 28, 28), C_BORDER);

    wchar_t title[96]{};
    _snwprintf_s(title, _TRUNCATE, T(L"选择游戏（%d）", L"Choose a game (%d)"),
                 (int)g_games.size());
    txt(dc, g_f_card, RECT{ p.left + 16, p.top + 4, p.right - 360, p.top + 38 },
        title, C_TEXT, DT_LEFT | DT_VCENTER);
    draw_btn(dc, lib_rect_manual(), T(L"添加 exe…", L"Add exe…"), false,
             hit_rect(lib_rect_manual(), g_hover_x, g_hover_y));
    draw_btn(dc, lib_rect_close(), T(L"取消", L"Cancel"), false,
             hit_rect(lib_rect_close(), g_hover_x, g_hover_y));
    txt(dc, g_f_small, RECT{ p.left + 16, p.top + 40, p.right - 16, p.top + 58 },
        T(L"点一行即可选择；[添加 exe…] 只往列表里加游戏，不会安装。",
          L"Click a row to select; [Add exe…] only adds to the list, nothing is installed."),
        C_DIM, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    RECT list{ p.left + 10, lib_list_top(), p.right - 10, lib_list_bottom() };
    fill_round(dc, list, 8, RGB(22, 22, 22));

    if (g_games.empty()) {
        txt(dc, g_f_ui, RECT{ list.left + 16, list.top + 16, list.right - 16, list.top + 46 },
            T(L"还没有游戏：点右上角[添加 exe…]把游戏加进来。",
              L"No games yet: use [Add exe…] at the top right."),
            C_DIM, DT_LEFT | DT_VCENTER);
    }

    const int n = (int)g_games.size();
    int first = g_lib_scroll / kLibRowH;
    if (first < 0) { first = 0; }
    const int rows = lib_rows_visible();
    for (int k = 0; k < rows && first + k < n; ++k) {
        const int i = first + k;
        const GameEntry& e = g_games[(size_t)i];
        const RECT r = lib_row_rect(i);
        if (r.bottom <= lib_list_top() || r.top >= lib_list_bottom()) { continue; }
        const bool sel = (i == g_lib_sel);
        const bool hv = hit_rect(r, g_hover_x, g_hover_y);
        if (sel || hv) {
            fill_round(dc, RECT{ r.left - 8, r.top + 2, r.right + 8, r.bottom - 2 }, 6,
                       sel ? RGB(42, 42, 42) : RGB(30, 30, 30));
        }
        const bool run = game_entry_running(e);
        const bool inst = game_entry_installed(e);
        draw_dot(dc, r.left + 7, r.top + 17, run ? C_OK : (inst ? C_ACCENT : C_DIM));
        txt(dc, g_f_ui, RECT{ r.left + 22, r.top + 4, r.right - 10, r.top + 30 }, e.name,
            C_TEXT, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        std::wstring meta = e.source.empty() ? T(L"手动", L"Manual") : e.source;
        meta += run ? T(L" · 运行中", L" | running")
                    : (inst ? T(L" · 已安装", L" | installed")
                            : T(L" · 未安装", L" | not installed"));
        meta += L" · ";
        meta += e.exe.empty()
                    ? std::wstring(T(L"主程序待解析", L"exe not resolved"))
                    : (std::wstring(T(L"主程序：", L"exe: ")) + last_component(e.exe));
        meta += L" · ";
        meta += e.exe.empty() ? game_entry_path(e) : e.exe;
        txt(dc, g_f_small, RECT{ r.left + 22, r.top + 28, r.right - 10, r.bottom - 4 },
            meta, C_DIM, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }

    // 细滚动条（只有超出时才画）
    const int total_h = n * kLibRowH;
    const int view_h = lib_list_bottom() - lib_list_top();
    if (total_h > view_h) {
        const int track_h = view_h - 8;
        const int thumb_h = (int)((long long)track_h * view_h / total_h);
        const int maxs = total_h - view_h;
        const int thumb_y = list.top + 4 +
            (int)((long long)(track_h - thumb_h) * g_lib_scroll / (maxs ? maxs : 1));
        fill_round(dc, RECT{ list.right - 8, list.top + 4, list.right - 4,
                             list.top + 4 + track_h }, 2, RGB(34, 34, 34));
        fill_round(dc, RECT{ list.right - 8, thumb_y, list.right - 4, thumb_y + thumb_h },
                   2, RGB(90, 90, 90));
    }

    const GameEntry* e = (g_lib_sel >= 0 && g_lib_sel < n)
                             ? &g_games[(size_t)g_lib_sel] : nullptr;
    const std::wstring fl = e ? (T(L"将选择：", L"Will select: ") + e->name)
                              : T(L"点一行选择，Esc 取消", L"Click a row, Esc to cancel");
    txt(dc, g_f_small, RECT{ p.left + 16, p.bottom - 46, p.right - 16, p.bottom - 14 },
        fl, e ? C_LABEL : C_DIM, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
}

static std::wstring last_line_snapshot()
{
    EnterCriticalSection(&g_line_cs);
    std::wstring s = g_last_line;
    LeaveCriticalSection(&g_line_cs);
    return s;
}

// ------------------------------------------------------------ 游戏库操作
static GameEntry* lib_selected()
{
    if (g_lib_sel < 0 || g_lib_sel >= (int)g_games.size()) { return nullptr; }
    return &g_games[(size_t)g_lib_sel];
}

// 全局扫描保留实现但默认不暴露：当前筛选规则不够可靠，先只走"添加 exe…"。
// 添加一个游戏条目（只入列表，不安装）；加完直接选中并关闭选择页。
static void library_use_selected();     // 定义在下面
static void library_add_manual()
{
    std::wstring exe;
    if (!pick_game_file(&exe)) { return; }
    const size_t s = exe.find_last_of(L'\\');
    if (s == std::wstring::npos) { return; }
    const std::wstring d = exe.substr(0, s);

    GameEntry* e = find_game_mut(d);
    if (e == nullptr) {
        GameEntry n;
        n.dir = d;
        n.root = d;
        n.exe = exe;
        n.name = file_stem(exe);       // 配置名默认 = 选中的 exe 名
        n.source = T(L"手动", L"Manual");
        n.st = baseline_state();
        g_games.push_back(n);
        e = &g_games.back();
    } else {
        // 以前用目录名自动起的名字：跟着 exe 名一起更新（用户改过的名字不动）。
        if (e->name.empty() || same_name(e->name, last_component(d))) { e->name = file_stem(exe); }
    }
    e->dir = d;
    e->exe = exe;
    if (e->root.empty()) { e->root = d; }
    g_lib_sel = (int)(e - &g_games[0]);
    save_games();
    lib_ensure_visible();
    library_use_selected();
}

static void library_use_selected()
{
    GameEntry* e = lib_selected();
    if (e == nullptr) { return; }
    if (!select_game(g_lib_sel)) { InvalidateRect(g_hwnd, nullptr, FALSE); return; }
    g_focus = -1;
    side_ensure_visible(g_lib_sel);
    const std::wstring m = T(L"已选择：", L"Selected: ") + e->name;
    close_library();
    set_msg(m.c_str());
}

static void library_install_selected()
{
    GameEntry* e = lib_selected();
    if (e == nullptr) { return; }
    ensure_entry_resolved(*e);
    std::wstring d = e->dir;
    std::wstring exe = e->exe;
    if (d.empty()) {
        if (open_exe_picker(g_lib_sel, true)) { return; }
        std::wstring picked;
        if (!pick_game_file(&picked)) {
            g_lib_msg = T(L"没找到主程序：请用[自选 exe…]指定",
                          L"Main exe not found: use [Pick exe...]");
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        }
        const size_t s = picked.find_last_of(L'\\');
        if (s == std::wstring::npos) { return; }
        d = picked.substr(0, s);
        exe = picked;
    }
    std::wstring q = T(L"把锁帧组件安装到：\r\n\r\n", L"Install into:\r\n\r\n") + short_path(d);
    if (game_dir_running(d)) {
        MessageBoxW(g_hwnd,
                    T(L"游戏正在运行：请先完全退出游戏再安装。",
                      L"The game is running: close it completely before installing."),
                    T(L"请先关闭游戏", L"Close the game first"), MB_OK | MB_ICONWARNING);
        return;
    }
    if (!exe.empty()) { q += T(L"\r\n\r\n主程序：", L"\r\n\r\nexe: ") + last_component(exe); }
    q += T(L"\r\n\r\n会覆盖 dxgi.dll / nvngx_dlssg.dll / fakerFG.dll / xess_mfg_producer.dll，"
           L"并按所选版本重写 fakerFG.ini（旧文件会备份；原 dxgi.dll / nvngx_dlssg.dll 可还原）。继续？",
           L"\r\n\r\ndxgi.dll / nvngx_dlssg.dll / fakerFG.dll / xess_mfg_producer.dll will be "
           L"overwritten, and fakerFG.ini is rewritten from the selected template "
           L"(old file backed up; original dxgi.dll / nvngx_dlssg.dll can be restored). Continue?");
    if (MessageBoxW(g_hwnd, q.c_str(), T(L"安装到游戏", L"Install to game"),
                    MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    std::wstring log;
    const bool ok = deploy_game_dir(d, &log);
    if (ok) {
        e->dir = d;
        if (!exe.empty()) { e->exe = exe; }
        e->last_use = (unsigned long long)std::time(nullptr);
        g_game_dir = d;
        ini_ws(L"ui", L"game_dir", g_game_dir);
        remember_game(d);
        apply_config_after_deploy(d);   // 安装 = 把该游戏保存的配置写进 fakerFG.ini
    }
    refresh_readiness();
    MessageBoxW(g_hwnd, log.c_str(), ok ? T(L"安装完成", L"Installed") : T(L"安装失败", L"Install failed"),
                MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void status_summary(std::wstring* badge, COLORREF* color, std::wstring* advice)
{
    const int total = g_pair_total.load();
    const int exact = g_pair_exact.load();
    if (!g_running) {
        *badge = T(L"未运行", L"Not running");
        *color = C_DIM;
        *advice = T(L"点卡片里的[启动补帧]开始；运行中按 ", L"Click [Start] in the card; press ")
                  + hotkey_name()
                  + T(L" 可以随时开关。", L" to toggle while running.");
        return;
    }
    if (total == 0) {
        *badge = T(L"已启动，等待数据", L"Started, waiting");
        *color = C_WARN;
        *advice = g_ready.producer
            ? std::wstring(T(L"正在等第一个统计窗口（约 40 帧）…",
                             L"Waiting for the first report window (~40 frames)..."))
            : std::wstring(T(L"还没检测到 producer：确认游戏里已开 DLSS 帧生成，并已进入实际游戏画面。",
                             L"No producer yet: enable DLSS Frame Generation and reach the in-game scene."));
        return;
    }
    if (exact * 10 >= total * 9) {
        *badge = T(L"插帧中", L"Interpolating");
        *color = C_OK;
        wchar_t b[200]{};
        _snwprintf_s(b, _TRUNCATE,
                     T(L"正在补帧：最近一个统计窗口 %d/%d 帧精确配对。",
                       L"Interpolating: %d/%d frames paired in the last window."),
                     exact, total);
        *advice = b;
        return;
    }
    if (exact * 2 >= total) {
        *badge = T(L"部分插帧", L"Partial interpolation");
        *color = C_WARN;
        wchar_t b[240]{};
        _snwprintf_s(b, _TRUNCATE,
                     T(L"有 %d/%d 帧没配上（那些帧会退回原生）。常见原因：HUD 挡住画面四角、窗口被缩放、游戏不是无边框全屏。",
                       L"%d/%d frames were not paired (they fall back to native). Common causes: HUD over the corners, scaled window, exclusive fullscreen."),
                     total - exact, total);
        *advice = b;
        return;
    }
    *badge = T(L"几乎没插帧", L"Almost no interpolation");
    *color = C_BAD;
    *advice = T(L"配对基本失败，输出接近原生。点「一键诊断」收日志，再按提示排查。",
                L"Pairing is failing and output is nearly native. Use [Diagnostics] to collect logs.");
}

// ---------------------------------------------------------------- 高级抽屉
// 不再整页切换：三步流程始终在，高级设置覆盖在右侧，随时[完成]收起。
static const int kDrawerW = kWinW - kSideW;   // 抽屉从侧栏右侧开始，不切断卡片文字
static int g_drawer_scroll = 0;
static int g_drawer_focus = -1;     // 键盘焦点（抽屉内可操作行的 items 下标）
static int g_drawer_h = 0;

static int drawer_x0() { return kWinW - kDrawerW; }
static RECT drawer_rect_close() { return RECT{ kWinW - 110, kTitleH + 6, kWinW - 16, kTitleH + 40 }; }

static void toggle_advanced()
{
    g_advanced = !g_advanced;
    g_drawer_scroll = 0;
    g_drawer_focus = -1;
    if (g_advanced) { drawer_focus_step(1); }
    ini_w(L"ui", L"advanced", g_advanced ? 1 : 0);
    save_all();
    set_msg(g_advanced ? T(L"高级设置已展开（Esc/[完成]收起）",
                          L"Advanced settings opened (Esc/[Done] closes)")
                       : T(L"已收起高级设置", L"Advanced settings closed"));
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 子页互斥：打开游戏库 / 日志 / 帮助时，抽屉自动收起（不弹提示）。
static void close_advanced_silent()
{
    if (!g_advanced) { return; }
    g_advanced = false;
    g_drawer_focus = -1;
    ini_w(L"ui", L"advanced", 0);
}

static void layout_drawer_items(std::vector<Item>& out, int* content_h)
{
    out.clear();
    const int x0 = drawer_x0() + kMargin;
    const int w = kDrawerW - 2 * kMargin;
    int y = kTitleH + 52;
    for (int ci = 0; ci < kCardCount; ++ci) {
        const CardDef& cd = kCards[ci];
        int body = 0;
        for (int r = 0; r < cd.n; ++r) { body += row_h(cd.rows[r].kind, cd.rows[r].id); }
        const int bottom = y + kCardHead + body + kCardTail;

        Item head{};
        head.rc = RECT{ x0, y, x0 + w, bottom };
        head.id = (Id)(1000 + ci);
        out.push_back(head);

        int ry = y + kCardHead;
        for (int r = 0; r < cd.n; ++r) {
            Item it{};
            it.id = cd.rows[r].id;
            it.kind = cd.rows[r].kind;
            it.is_row = true;
            const int hh = row_h(it.kind, it.id);
            it.rc = RECT{ x0 + 16, ry, x0 + w - 16, ry + hh };
            ry += hh;
            it.on = (it.id == ID_JITZ) ? S().jitter_zero
                  : (it.id == ID_NOMARKER) ? S().no_marker
                  : (it.id == ID_UPSAMPLE) ? S().upsample
                  : (it.id == ID_STRAUTO) ? S().stream_auto
                  : (it.id == ID_STRASYM) ? S().stream_asym
                  : (it.id == ID_PREFETCH) ? S().input_prefetch
                  : (it.id == ID_UNLOCK) ? S().unlock_mfg
                  : (it.id == ID_GAME_QUIRKS) ? S().game_quirks
                  : (it.id == ID_SLACK) ? S().slack1
                  : (it.id == ID_TEAR) ? S().tearing
                  : (it.id == ID_SDR) ? S().sdr
                  : (it.id == ID_LAYERED) ? S().overlay_layered
                  : (it.id == ID_RELAY) ? S().relay_input
                  : (it.id == ID_OSD) ? S().osd
                  : (it.id == ID_PERF) ? S().perf_no_throttle
                  : (it.id == ID_FG_LOG) ? S().fg_log
                  : false;
            out.push_back(it);
        }
        y = bottom + kGap;
    }
    *content_h = y - (kTitleH + 52) + 8;
}

// --------------------------------------------------------------- 悬浮说明
static const wchar_t* tip_for(Id id)
{
    switch (id) {
    case ID_S_INSTALL:
        return L"选择游戏的 exe，把 fakerFG.dll / producer 装进它所在目录。"
               L"旧组件会被覆盖（dxgi.dll / nvngx_dlssg.dll 先备份，可还原），"
               L"游戏侧 fakerFG.ini 会按所选版本重写并备份旧文件。";
    case ID_S_UNDEPLOY:
        return L"移除组件并把游戏原来的 dxgi.dll / nvngx_dlssg.dll / version.dll 换回去；"
               L"fakerFG.ini 保留。";
    case ID_S_GAMELIB:
        return L"从一个游戏的主程序 exe 添加条目；"
               L"每个游戏单独记住倍率和锁帧。";
    case ID_S_WINDOW:
        return L"要抓取并叠加的游戏窗口。先进到实际游戏画面再选；"
               L"无边框 / 窗口化全屏最稳。";
    case ID_S_REFRESH:
        return L"重新枚举当前可见窗口（≥1280×720）。改分辨率或刚进游戏时点一下。";
    case ID_S_2X:
        return L"2 倍：每帧补 1 张。120 Hz 面板建议游戏内锁 60 fps，延迟最低。";
    case ID_S_3X:
        return L"3 倍：每帧补 2 张。120 Hz 面板建议锁 40 fps。需要 libxell。";
    case ID_S_4X:
        return L"4 倍：每帧补 3 张。120 Hz 面板建议锁 30 fps。延迟最高，适合慢节奏游戏。";
    case ID_S_START:
        return L"启动 / 停止消费端（lock_frame.exe）。运行中按热键也能开关；"
               L"关闭这个窗口不会影响游戏。";
    case ID_S_LOGVIEW:
        return L"查看最近 400 行运行日志（consumer 输出 + UI 消息），不用离开界面。";
    case ID_S_DIAG:
        return L"生成诊断包（系统 / 显卡 / 日志），本地打包不上传，方便发给别人排查。";
    case ID_S_HELP:
        return L"三步流程、快捷键、常见问题。按 F1 也能随时打开。";
    case ID_FG_MODE:
        return L"固定倍率：每帧输入生成 1/2/3 张插值帧。输入帧率 = 面板刷新率 ÷ 倍率。";
    case ID_OUT:
        return L"输出分辨率：1:1 原生最清晰；0.75x / 半分辨率省消费卡性能，但会软一点。";
    case ID_UPSAMPLE_INFO:
        return L"消费端把 MV/深度从 StreamPercent 尺寸联合重建到输入画面尺寸后再交给 XeFG。";
    case ID_GAME_QUIRKS:
        return L"OptiScaler 的逐游戏 FG 输入表（49 行）。开启时按游戏 exe 自动套用："
               L"UE 无限远平面、jittered MV、inverted depth 等；日志会打印命中的行。";
    case ID_FG_INPUT:
        return L"本版只实现 DLSSG 输入（Streamline tag），固定不可改；"
               L"NVNGX / Upscaler / FSR 车道未实现，ini 里写了也会被强制回 DLSSG。";
    case ID_INI_PROFILE:
        return L"游戏侧 fakerFG.ini 的版本：精简（发布，只保留常用键）/ 完整（测试，含全部"
               L"实验与调试键）。切换会备份并覆盖游戏目录的 fakerFG.ini，重启游戏生效。";
    case ID_STREAM_PCT:
        return L"跨卡只传 MV/深度的比例：100%=1:1 最清晰、150%=每维 2/3（默认）、"
               L"200%=每维 1/2、400%=每维 1/4 最省；改动需重启游戏。"
               L"注意：无损传输开启时该条流强制 1:1，这个比例只对走老路径的流有效。";
    case ID_CODEC:
        return L"无损传输（协议 v30）：渲染卡把 MV/深度编码进跨卡槽，副卡解码回逐位一致的"
               L"原数据，并强制 1:1。自动 = 能跑就用，硬件/格式不支持时自动退回桥接拷贝+"
               L"升降采样。改动需重启游戏。";
    case ID_DEPTH_PCT:
        return L"深度码流的分辨率（MV 永远是 100% 逐像素无损，不受这里影响）。"
               L"深度只用于遮挡判断，实测 4K 下 150% 让 depth 编码 2.05→0.79 ms、码流减半，"
               L"MV 一字不改。100% = 逐像素无损（最保守）；改动需重启游戏。";
    case ID_UI_COMPOSE:
        return L"默认开启：渲染卡只传「被 UI 遮挡的画面」的支撑域 patch，副卡还原干净 HUDless，"
               L"XeFG 走 mode 4；插值帧的 HUD 不再残影。"
               L"需要游戏在 DLSS-G evaluate 里提供 HUDless；没有 HUDless 时自动退化。"
               L"UiPatch=0 可完全关闭；改动需重启游戏。";
    case ID_FG_LOG:
        return L"游戏侧日志（fakerFG.ini 的 Log）：排查问题打开，稳定后可关闭以减少写盘；"
               L"改动需重启游戏。";
    case ID_FG_INI:
        return L"当前游戏目录里的 fakerFG.ini 路径。改 FgInput 后需要重启游戏生效。";
    case ID_VERSION:
        return L"UI 与共享协议版本。游戏侧 DLL 必须同版本，否则会提示协议不符。";
    case ID_CHECK_GPU:
        return L"DXGI 枚举到的非软件适配器数量。推荐两块：游戏一块、帧生成一块。";
    case ID_CHECK_RUNTIME:
        return L"libxess_fg.dll 是 XeSS-FG 运行时；3x/4x 还需要 libxell.dll。"
               L"正式包默认不内置。";
    case ID_CHECK_GAME:
        return L"检查游戏目录 fakerFG.ini：FgOutput 必须是 Capture、"
               L"Passthrough=0、FgInput 非空。";
    case ID_CHECK_SHA:
        return L"启动时用同目录的 SHA256SUMS.txt 校验一次包内文件（不周期采样）；"
               L"缺文件或哈希不一致会显示在这里。";
    case ID_CAP_API:
        return L"颜色抓取方式。锁帧版固定用 WGC（DWM 本来就要搬画面，复用这笔传输）。";
    case ID_STREAM_INFO:
        return L"跨卡只传 MV + 深度，比例由 fakerFG.ini 的 StreamPercent 决定（默认 150%）。";
    case ID_SDR:
        return L"输出色彩空间。HDR 屏选 SDR 会发灰，SDR 屏保持 HDR10 会发亮——按显示器选。";
    case ID_POOL:
        return L"帧池深度：同时排队的输入帧数。越大越抗抖动，但增加延迟。";
    case ID_LAT:
        return L"最大帧延迟：允许排队多少帧再上屏。1 最低延迟，3 更平顺。";
    case ID_GPUSEL:
        return L"跑帧生成 / 上屏的 GPU。自动 = 按窗口所在显示器选，并尽量避开游戏卡。";
    case ID_DISPLAY:
        return L"当前输出显示器的分辨率和刷新率（只读），用于推导建议锁帧。";
    case ID_UISCALE:
        return L"界面缩放。自动跟随系统 DPI；也可手动 100%–200%，适合电视或视力不便。";
    case ID_TARGET:
        return L"启动时要抓的游戏窗口。选过一次会记住；也可以直接用热键抓前台窗口。";
    case ID_HOTKEY:
        return L"全局开关热键（默认 Alt+PageUp）。点击这一行可以重新录制组合键。";
    case ID_LOCK:
        return L"建议在游戏里锁的输入帧率 = 面板刷新率 ÷ 倍率。UI 只给建议，不改游戏设置。";
    case ID_CMD:
        return L"实际传给 lock_frame.exe 的命令行。默认折叠，需要排查时展开复制。";
    case ID_LAYERED:
        return L"A/B：给叠加层加 WS_EX_LAYERED，实测能让游戏菜单收到点击（默认开）。";
    case ID_RELAY:
        return L"A/B：叠加层不再穿透，而是把鼠标转发给游戏窗口。和分层窗口二选一。";
    case ID_OSD:
        return L"在游戏画面上画一行 OSD：fps / 倍率 / 输入帧率 / GPU 等待 / 丢帧。";
    default:
        return L"";
    }
}

static Id hover_id_at(int mx, int my)
{
    if (g_lib_view || g_log_view || g_welcome || g_help_view) { return ID_NONE; }
    if (mx < kSideW) {
        if (hit_rect(help_rect_entry(), mx, my)) { return ID_S_HELP; }
        return ID_NONE;
    }
    if (g_advanced) {
        if (hit_rect(drawer_rect_close(), mx, my)) { return ID_NONE; }
        std::vector<Item> items;
        int ch = 0;
        layout_drawer_items(items, &ch);
        for (const Item& it : items) {
            if (!it.is_row) { continue; }
            RECT rc = it.rc;
            OffsetRect(&rc, 0, -g_drawer_scroll);
            if (hit_rect(rc, mx, my)) { return it.id; }
        }
        return ID_NONE;
    }
    for (const SBtn& b : simple_buttons()) {
        if (hit_rect(b.rc, mx, my)) { return b.id; }
    }
    return ID_NONE;
}

// 鼠标是否停在可点击的控件上（用于手型光标）。
static bool is_clickable_at(int mx, int my)
{
    if (g_welcome) { return hit_rect(welcome_rect_ok(), mx, my); }
    if (g_pick_view) {
        if (hit_rect(pick_rect_close(), mx, my)) { return true; }
        for (int i = 0; i < (int)g_pick_list.size(); ++i) {
            if (hit_rect(pick_row_rect(i), mx, my)) { return true; }
        }
        return false;
    }
    if (g_lib_view) {
        if (hit_rect(lib_rect_manual(), mx, my) || hit_rect(lib_rect_close(), mx, my)) {
            return true;
        }
        const int first = g_lib_scroll / kLibRowH;
        for (int k = 0; k < lib_rows_visible(); ++k) {
            const int i = first + k;
            if (i >= (int)g_games.size()) { break; }
            if (hit_rect(lib_row_rect(i), mx, my)) { return true; }
        }
        return false;
    }
    if (g_log_view) {
        return hit_rect(log_rect_folder(), mx, my) || hit_rect(log_rect_copy(), mx, my) ||
               hit_rect(log_rect_refresh(), mx, my) || hit_rect(log_rect_close(), mx, my);
    }
    if (g_help_view) { return hit_rect(help_rect_close(), mx, my); }
    if (hit_rect(title_min_rect(), mx, my) || hit_rect(title_close_rect(), mx, my)) {
        return true;
    }
    if (mx >= kSideW && hit_rect(adv_rect(), mx, my)) { return true; }
    if (mx < kSideW) {
        const int rows = side_rows_max();
        for (int slot = 0; slot < rows && g_side_scroll + slot < (int)g_games.size(); ++slot) {
            if (hit_rect(side_row_rect(slot), mx, my)) { return true; }
        }
        if (hit_rect(help_rect_entry(), mx, my)) { return true; }
        return false;
    }
    const Id h = hover_id_at(mx, my);
    switch (h) {
    case ID_S_INSTALL: case ID_S_UNDEPLOY: case ID_S_GAMELIB: case ID_S_PICKEXE:
    case ID_S_WINDOW:
    case ID_S_REFRESH: case ID_S_2X: case ID_S_3X: case ID_S_4X: case ID_S_START:
    case ID_S_LOGVIEW: case ID_S_DIAG:
    case ID_FG_MODE: case ID_OUT: case ID_INI_PROFILE: case ID_SDR: case ID_POOL:
    case ID_LAT: case ID_GPUSEL: case ID_UISCALE: case ID_TARGET: case ID_HOTKEY:
    case ID_CMD: case ID_TMODE: case ID_FOV: case ID_IVL: case ID_UI: case ID_RT:
    case ID_LOCK: case ID_LAYERED: case ID_RELAY: case ID_OSD: case ID_JITZ:
    case ID_NOMARKER: case ID_UPSAMPLE: case ID_STRAUTO: case ID_STRASYM:
    case ID_PREFETCH: case ID_UNLOCK: case ID_SLACK: case ID_TEAR: case ID_PERF:
        return true;
    default:
        return false;
    }
}

static void draw_tip(HDC dc)
{
    if (g_tip_id == ID_NONE) { return; }
    if (g_lib_view || g_log_view || g_welcome) { return; }
    if (GetTickCount() - g_tip_since < 450) { return; }
    const wchar_t* t = tip_for((Id)g_tip_id);
    if (t == nullptr || t[0] == 0) { return; }

    RECT calc{ 0, 0, 300, 0 };
    HGDIOBJ old = SelectObject(dc, g_f_small);
    DrawTextW(dc, t, -1, &calc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    const int w = calc.right + 20;
    const int h = calc.bottom + 16;
    int x = g_hover_x + 16;
    int y = g_hover_y + 22;
    if (x + w > kWinW - 8) { x = kWinW - 8 - w; }
    if (x < 8) { x = 8; }
    if (y + h > kWinH - 8) { y = g_hover_y - h - 12; }
    if (y < kTitleH + 4) { y = kTitleH + 4; }

    RECT box{ x, y, x + w, y + h };
    stroke_round(dc, box, 6, RGB(18, 18, 18), RGB(96, 96, 96));
    RECT tr{ x + 10, y + 8, x + w - 10, y + h - 8 };
    HGDIOBJ of = SelectObject(dc, g_f_small);
    SetTextColor(dc, C_TEXT);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, t, -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, of);
}

// 卡片绘制（抽屉里用；scroll 是内容偏移）
static int drawer_view_h() { return (kWinH - 52) - (kTitleH + 48); }

static void paint_cards(HDC dc, const std::vector<Item>& items, int scroll)
{
    for (const Item& it : items) {
        RECT rc = it.rc;
        OffsetRect(&rc, 0, -scroll);
        if (rc.bottom < kTitleH + 40 || rc.top > kWinH) { continue; }

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
                            (it.kind == RK_DD || it.kind == RK_TOG || it.kind == RK_SPIN ||
                             it.kind == RK_EDIT || it.kind == RK_CMDLINE));
        if (g_advanced && (int)(&it - items.data()) == g_drawer_focus) {
            draw_focus(dc, RECT{ rc.left - 6, rc.top + 1, rc.right + 6, rc.bottom - 1 });
        }
        if (hover && (it.kind == RK_DD || it.kind == RK_TOG || it.kind == RK_SPIN ||
                      it.kind == RK_EDIT || it.kind == RK_CMDLINE)) {
            fill_round(dc, RECT{ rc.left - 6, rc.top + 1, rc.right + 6, rc.bottom - 1 },
                       RAD_CTL, RGB(44, 44, 44));
        }
        if (it.kind == RK_TOG) { draw_toggle(dc, rc, it.on, hover); }

        if (it.kind == RK_HEAD) {
            HGDIOBJ old = SelectObject(dc, g_f_small);
            txt(dc, g_f_small, RECT{ rc.left + 2, rc.top + 6, rc.right, rc.bottom },
                def->label, C_DIM, DT_LEFT | DT_VCENTER);
            SelectObject(dc, old);
            continue;
        }
        const bool enabled = row_enabled(it.id);
        if (it.kind != RK_CMDLINE) {
            txt(dc, g_f_ui, RECT{ rc.left, rc.top, rc.left + 200, rc.bottom },
                def->label, (!enabled) ? C_DIM : (hover ? C_TEXT : C_LABEL),
                DT_LEFT | DT_VCENTER);
        }
        if (!enabled && it.kind != RK_CMDLINE) {
            txt(dc, g_f_ui, RECT{ rc.left + 210, rc.top, rc.right, rc.bottom },
                row_value(it.id), C_DIM, DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS);
            continue;
        }

        switch (it.kind) {
        case RK_INFO: draw_info(dc, rc, row_value(it.id)); break;
        case RK_CMDLINE: {
            const int headH = g_show_cmd ? 24 : (rc.bottom - rc.top);
            txt(dc, g_f_ui, RECT{ rc.left, rc.top, rc.right - 90, rc.top + headH },
                def->label, C_LABEL, DT_LEFT | DT_VCENTER);
            txt(dc, g_f_small, RECT{ rc.right - 84, rc.top, rc.right, rc.top + headH },
                g_show_cmd ? L"收起 \u25B4" : L"展开 \u25BE",
                hover ? C_TEXT : C_ACCENT, DT_RIGHT | DT_VCENTER);
            if (g_show_cmd) {
                txt(dc, g_f_small, RECT{ rc.right - 190, rc.top, rc.right - 96, rc.top + headH },
                    L"复制", hit_rect(RECT{ rc.right - 190, rc.top, rc.right - 96, rc.top + headH },
                                      g_hover_x, g_hover_y) ? C_TEXT : C_ACCENT,
                    DT_RIGHT | DT_VCENTER);
                RECT tr{ rc.left, rc.top + 26, rc.right, rc.bottom - 2 };
                HGDIOBJ old = SelectObject(dc, g_f_small);
                SetTextColor(dc, C_DIM);
                SetBkMode(dc, TRANSPARENT);
                DrawTextW(dc, display_cmdline().c_str(), -1, &tr,
                          DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
                SelectObject(dc, old);
            }
            break;
        }
        case RK_DD: draw_dd(dc, rc, dd_caption(it.id), hover, g_popup_id == (int)it.id); break;
        case RK_EDIT: {
            const bool editing = (g_edit_id == it.id);
            draw_edit(dc, rc, editing ? (g_edit_buf + L"_") : row_value(it.id), hover, editing);
            break;
        }
        case RK_SPIN: {
            const bool up = hover && (g_hover_x > rc.right - 26) && (g_hover_y < rc.top + 18);
            const bool dn = hover && (g_hover_x > rc.right - 26) && (g_hover_y >= rc.top + 18);
            draw_spin(dc, rc, row_value(it.id), up, dn);
            break;
        }
        default: break;
        }
    }
}

static void paint_drawer(HDC dc)
{
    if (!g_advanced) { return; }

    const int x0 = drawer_x0();
    fill_round(dc, RECT{ x0, 0, kWinW, kWinH }, 0, RGB(22, 22, 22));
    fill_round(dc, RECT{ x0, 0, x0 + 2, kWinH }, 0, RGB(62, 62, 62));

    txt(dc, g_f_card, RECT{ x0 + 18, kTitleH + 2, kWinW - 260, kTitleH + 28 },
        L"高级设置", C_TEXT, DT_LEFT | DT_VCENTER);
    {
        const GameEntry* de = find_game(g_game_dir);
        const std::wstring who = de
            ? (std::wstring(T(L"正在编辑：", L"Editing: ")) + de->name)
            : std::wstring(T(L"未选择游戏（改的是通用配置）",
                             L"No game selected (editing the generic config)"));
        txt(dc, g_f_small, RECT{ x0 + 18, kTitleH + 28, kWinW - 260, kTitleH + 44 },
            who, de ? C_LABEL : C_WARN, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }
    const RECT cb = drawer_rect_close();
    draw_btn(dc, cb, T(L"完成", L"Done"), false, hit_rect(cb, g_hover_x, g_hover_y));

    std::vector<Item> items;
    int content_h = 0;
    layout_drawer_items(items, &content_h);
    g_drawer_h = content_h;

    const int view_top = kTitleH + 52;
    const int view_bottom = kWinH - 52;
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, x0, view_top, kWinW, view_bottom);
    paint_cards(dc, items, g_drawer_scroll);
    RestoreDC(dc, saved);

    fill_round(dc, RECT{ x0, kWinH - 52, kWinW, kWinH }, 0, RGB(22, 22, 22));
    txt(dc, g_f_small, RECT{ x0 + 18, kWinH - 46, kWinW - 18, kWinH - 20 },
        L"滚轮翻页 · Tab/上下切换 · Enter 激活 · Esc 收起",
        C_DIM, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
}

// ---------------------------------------------------------------- 帮助面板
static const int kHelpL = 200, kHelpT = 100, kHelpR = kWinW - 200, kHelpB = kWinH - 190;
static RECT help_rect_entry() { return RECT{ 10, kWinH - 84, kSideW - 10, kWinH - 44 }; }
static RECT help_rect_close() { return RECT{ kHelpR - 104, kHelpT + 8, kHelpR - 16, kHelpT + 38 }; }

static void open_help()
{
    close_advanced_silent();
    g_help_view = true;
    g_popup_id = 0;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void close_help()
{
    g_help_view = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void draw_help(HDC dc)
{
    if (!g_help_view) { return; }

    fill_round(dc, RECT{ 0, 0, kWinW, kWinH }, 0, RGB(12, 12, 12));
    RECT p{ kHelpL, kHelpT, kHelpR, kHelpB };
    stroke_round(dc, p, RAD_CARD, RGB(28, 28, 28), C_BORDER);

    txt(dc, g_f_card, RECT{ p.left + 20, p.top + 6, p.right - 120, p.top + 42 },
        L"帮助与快捷键", C_TEXT, DT_LEFT | DT_VCENTER);
    const RECT cb = help_rect_close();
    draw_btn(dc, cb, L"关闭", false, hit_rect(cb, g_hover_x, g_hover_y));

    auto section = [&](int y, const wchar_t* t) {
        txt(dc, g_f_small, RECT{ p.left + 20, y, p.right - 20, y + 20 }, t, C_ACCENT,
            DT_LEFT | DT_VCENTER);
    };
    auto line = [&](int y, const std::wstring& t) {
        txt(dc, g_f_ui, RECT{ p.left + 20, y, p.right - 20, y + 22 }, t, C_LABEL,
            DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    };

    int y = p.top + 52;
    section(y, L"上手顺序"); y += 24;
    line(y, L"1. 点[当前游戏]右上角[选择游戏]：从列表里挑一个游戏（配置随游戏保存）。"); y += 24;
    line(y, L"2. 点[安装 / 应用配置]：把组件和当前配置一起写进游戏目录。"); y += 24;
    line(y, L"3. 进游戏（无边框 + DLSS 帧生成）；窗口和 producer 就绪后点[启动补帧]。"); y += 34;

    section(y, L"快捷键"); y += 24;
    line(y, std::wstring(T(L"开关插帧：", L"Toggle interpolation: ")) + hotkey_name()); y += 24;
    line(y, L"Tab / Shift+Tab 切换焦点　Enter / Space 触发　Esc 关闭面板"); y += 24;
    line(y, L"PageUp / PageDown / Home / End：列表翻页　F1：打开本帮助"); y += 34;

    section(y, L"常见问题"); y += 24;
    line(y, L"看不到游戏窗口：先进实际画面再点[刷新]；确认游戏不是独占全屏。"); y += 24;
    line(y, L"状态是“部分插帧”：HUD 可能挡住画面四角，或窗口被缩放。"); y += 24;
    line(y, L"只有 1 块 GPU：能跑，但插帧会和游戏抢性能；建议双卡。");

    wchar_t foot[128]{};
    _snwprintf_s(foot, _TRUNCATE,
                 T(L"v%s · 协议 v%u · 完整日志与诊断在状态卡下方",
                   L"v%s · protocol v%u · log and diagnostics are on the status card"),
                 LOCK_FRAME_VERSION_WSTRING, mgpu::kSharedVersion);
    txt(dc, g_f_small, RECT{ p.left + 20, p.bottom - 34, p.right - 20, p.bottom - 10 },
        foot, C_DIM, DT_LEFT | DT_VCENTER);
}

static void paint_simple(HDC dc)
{
    const int x0 = kSideW + kMargin, x1 = kWinW - kMargin;
    const unsigned hz = (g_refresh > 1) ? g_refresh : 120;
    const int mul = S().fg_mode + 2;

    auto btn = [&](Id id) -> RECT {
        for (const SBtn& b : simple_buttons()) {
            if (b.id == id) { return b.rc; }
        }
        return RECT{ 0, 0, 0, 0 };
    };
    auto hover = [&](Id id) { return hit_rect(btn(id), g_hover_x, g_hover_y); };

    // 卡片头：40px，左侧标题，右侧状态文字 + 状态点。
    auto card = [&](int top, int bottom, const wchar_t* title, bool ok,
                    const wchar_t* state) {
        fill_round(dc, RECT{ x0, top, x1, bottom }, RAD_CARD, C_CARD);
        RECT head{ x0, top, x1, top + 40 };
        fill_round(dc, head, RAD_CARD, C_CARD_HEAD);
        fill_round(dc, RECT{ head.left, head.bottom - 12, head.right, head.bottom },
                   RAD_CARD, C_CARD_HEAD);
        fill_round(dc, RECT{ head.left, head.bottom - 1, head.right, head.bottom }, 0,
                   RGB(46, 46, 46));
        txt(dc, g_f_card, RECT{ head.left + 18, head.top, head.right - 200, head.bottom },
            title, C_TEXT, DT_LEFT | DT_VCENTER);
        if (state != nullptr && state[0] != 0) {
            txt(dc, g_f_small, RECT{ head.right - 190, head.top, head.right - 34, head.bottom },
                state, ok ? C_OK : C_DIM, DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS);
        }
        draw_dot(dc, head.right - 20, (head.top + head.bottom) / 2, ok ? C_OK : C_WARN);
    };

    const GameEntry* ge = find_game(g_game_dir);
    const bool gsel = (ge != nullptr);

    // ------------------------------------------------------------ 当前游戏
    card(kSC1, kSC1B, T(L"当前游戏", L"Current game"), gsel && g_ready.deployed, nullptr);
    {
        const int name_x = x0 + 18;
        g_name_hit = RECT{ name_x - 4, kSC1 + 40, name_x + 320, kSC1 + 72 };
        if (gsel && g_name_edit) {
            RECT er = g_name_hit;
            fill_round(dc, er, 6, RGB(24, 24, 24));
            stroke_round(dc, er, 6, RGB(24, 24, 24), C_ACCENT);
            txt(dc, g_f_card, RECT{ er.left + 8, er.top, er.right - 8, er.bottom },
                g_name_buf + L"|", C_TEXT, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        } else {
            txt(dc, g_f_card, RECT{ name_x, kSC1 + 42, x1 - 200, kSC1 + 70 },
                gsel ? ge->name : std::wstring(T(L"还没有选择游戏", L"No game selected")),
                gsel ? C_TEXT : C_DIM, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            if (gsel) {
                // 名字可点：悬停时给一个浅底 + 提示
                if (hit_rect(g_name_hit, g_hover_x, g_hover_y)) {
                    txt(dc, g_f_small,
                        RECT{ g_name_hit.right - 90, g_name_hit.top, g_name_hit.right,
                              g_name_hit.bottom },
                        T(L"点击改名", L"click to rename"), C_DIM,
                        DT_RIGHT | DT_VCENTER);
                }
                SIZE ns{};
                HGDIOBJ nf = SelectObject(dc, g_f_card);
                GetTextExtentPoint32W(dc, ge->name.c_str(), (int)ge->name.size(), &ns);
                SelectObject(dc, nf);
                const int bx = name_x + ns.cx + 12;
                if (bx + 96 < x1 - 200) {
                    const wchar_t* st = g_ready.deployed ? T(L"已安装", L"Installed")
                                                         : T(L"未安装", L"Not installed");
                    RECT br{ bx, kSC1 + 48, bx + 84, kSC1 + 68 };
                    fill_round(dc, br, 10, RGB(30, 30, 30));
                    stroke_round(dc, br, 10, RGB(30, 30, 30),
                                 g_ready.deployed ? C_OK : C_BORDER);
                    txt(dc, g_f_small, br, st, g_ready.deployed ? C_OK : C_DIM,
                        DT_CENTER | DT_VCENTER);
                }
            }
        }
        txt(dc, g_f_small, RECT{ x0 + 18, kSC1 + 72, x1 - 18, kSC1 + 94 },
            gsel ? (!ge->exe.empty() ? ge->exe : game_entry_path(*ge))
                 : std::wstring(T(L"点右上角[选择游戏]从列表挑一个：配置和安装状态都跟着游戏保存。",
                                  L"Click [Choose game...]: settings and install state are saved per game.")),
            C_DIM, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        wchar_t cfg[256]{};
        if (gsel) {
            _snwprintf_s(cfg, _TRUNCATE,
                         T(L"%dx  ·  输入 %.1f fps  ·  FgInput %s  ·  MV 比例 %d%%%s%s",
                           L"%dx  |  input %.1f fps  |  FgInput %s  |  MV %d%%%s%s"),
                         mul, static_cast<double>((g_refresh > 1) ? g_refresh : 120) / mul,
                         fg_input_ini_value(S().fg_input), S().stream_pct,
                         g_ready.deployed ? T(L"  ·  已写入游戏", L"  |  applied to game") : L"",
                         g_ini_drift ? T(L"  ·  ini 被改过，点[重新应用配置]",
                                          L"  |  ini changed, click [Re-apply config]")
                                     : L"");
        } else {
            _snwprintf_s(cfg, _TRUNCATE,
                         T(L"选择游戏后，这里显示它保存的倍率 / 输入源 / MV 比例。",
                           L"After picking a game, its saved multiplier / input / MV ratio appears here."));
        }
        txt(dc, g_f_ui, RECT{ x0 + 18, kSC1 + 96, x1 - 18, kSC1 + 118 }, cfg,
            gsel ? (g_ini_drift ? C_WARN : C_LABEL) : C_DIM,
            DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        struct Chip { const wchar_t* label; bool ok; };
        const Chip chips[] = {
            { L"Intel 运行时", g_ready.runtime },
            { L"fakerFG.ini",  g_ready.ini },
            { L"producer",     g_ready.producer },
        };
        int cx = x0 + 18;
        for (const Chip& c : chips) {
            SIZE cs{};
            const wchar_t* label = c.label;
            if (const wchar_t* e = tr_lookup(label)) { label = e; }
            HGDIOBJ cf = SelectObject(dc, g_f_small);
            GetTextExtentPoint32W(dc, label, (int)wcslen(label), &cs);
            SelectObject(dc, cf);
            RECT cr{ cx, kSC1 + 122, cx + cs.cx + 38, kSC1 + 148 };
            stroke_round(dc, cr, 12, RGB(32, 32, 32), C_BORDER);
            draw_dot(dc, cr.left + 13, (cr.top + cr.bottom) / 2, c.ok ? C_OK : C_WARN);
            txt(dc, g_f_small, RECT{ cr.left + 24, cr.top, cr.right - 8, cr.bottom },
                label, c.ok ? C_LABEL : C_WARN, DT_LEFT | DT_VCENTER);
            cx = cr.right + 10;
        }

        draw_btn(dc, btn(ID_S_INSTALL),
                 g_ready.deployed ? T(L"重新应用配置", L"Re-apply config")
                                  : T(L"安装 / 应用配置", L"Install / apply"),
                 true, hover(ID_S_INSTALL), !gsel);
        draw_btn(dc, btn(ID_S_UNDEPLOY), T(L"还原 / 移除组件", L"Restore / remove"), false,
                 hover(ID_S_UNDEPLOY), !gsel || !g_ready.deployed);
        draw_btn(dc, btn(ID_S_PICKEXE), T(L"主程序…", L"Main exe…"), false,
                 hover(ID_S_PICKEXE), !gsel);
        // 与安装/还原同一行的「高级设置」（次级样式 + 左侧强调条）。
        {
            const RECT ar = adv_rect();
            const bool ha = hit_rect(ar, g_hover_x, g_hover_y);
            fill_round(dc, ar, RAD_CTL, ha ? C_BTN_HOVER : C_BTN);
            stroke_round(dc, ar, RAD_CTL, RGB(70, 70, 70), C_BORDER);
            fill_round(dc, RECT{ ar.left + 1, ar.top + 8, ar.left + 4, ar.bottom - 8 }, 2,
                       C_ACCENT);
            txt(dc, g_f_ui, ar, T(L"高级设置", L"Advanced"), C_TEXT,
                DT_CENTER | DT_VCENTER);
        }
        // 唯一的"选择游戏"入口：卡片标题右侧，强调色（放在标题行）。
        draw_btn(dc, btn(ID_S_GAMELIB),
                 gsel ? T(L"更换游戏", L"Change game") : T(L"选择游戏", L"Choose game"),
                 true, hover(ID_S_GAMELIB));
    }

    // ------------------------------------------------------------ 启动补帧
    const bool ready = g_ready.deployed && g_ready.window && g_ready.runtime;
    const wchar_t* bstate = !gsel ? T(L"未选择游戏", L"No game")
                          : !g_ready.deployed ? T(L"未安装", L"Not installed")
                          : !g_ready.window ? T(L"等待游戏窗口", L"Waiting for window")
                          : !g_ready.producer ? T(L"等待 producer", L"Waiting for producer")
                                              : T(L"就绪", L"Ready");
    card(kSC2, kSC2B, T(L"启动补帧", L"Start frame generation"),
         g_ready.deployed && g_ready.window && g_ready.producer, bstate);
    {
        txt(dc, g_f_small, RECT{ x0 + 18, kSC2 + 42, x1 - 18, kSC2 + 62 },
            !gsel ? T(L"先在[当前游戏]卡片点[选择游戏]；再进游戏（无边框 + DLSS 帧生成）。",
                      L"Pick a game with [Choose game] in the Current game card; then enter the game (borderless + DLSS FG).")
                  : g_ready.window ? T(L"已找到这个游戏的窗口；确认无边框 / 窗口化全屏，并已开启 DLSS 帧生成。",
                                       L"Window for this game found; make sure it is borderless and DLSS FG is on.")
                                   : T(L"还没有找到这个游戏的窗口：先进游戏画面，再点[刷新]。",
                                       L"No window for this game yet: enter the game, then click [Refresh]."),
            !gsel ? C_DIM : (g_ready.window ? (g_ready.producer ? C_OK : C_WARN) : C_LABEL),
            DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        std::wstring wt = T(L"（还没有该游戏的窗口）", L"(no window for this game yet)");
        if (g_ready.window && g_win_sel >= 0 && g_win_sel < (int)g_wins.size()) {
            wt = g_wins[(size_t)g_win_sel].title;
            wchar_t sz[128]{};
            _snwprintf_s(sz, _TRUNCATE, L"   %dx%d", g_wins[(size_t)g_win_sel].w,
                         g_wins[(size_t)g_win_sel].h);
            wt += sz;
        }
        RECT wr = btn(ID_S_WINDOW);
        stroke_round(dc, wr, RAD_CTL, hit_rect(wr, g_hover_x, g_hover_y) ? C_CTL_HOVER : C_CTL,
                     C_BORDER);
        txt(dc, g_f_ui, RECT{ wr.left + 12, wr.top, wr.right - 28, wr.bottom }, wt, C_TEXT,
            DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        draw_chevron(dc, wr.right - 15, (wr.top + wr.bottom) / 2 + 1, +1, C_DIM);
        draw_btn(dc, btn(ID_S_REFRESH), T(L"刷新", L"Refresh"), false, hover(ID_S_REFRESH));
    }

    // 倍率 + 启动（同一张卡片：改倍率立即保存到当前游戏）
    {
        for (int i = 0; i < 3; ++i) {
            const int m = i + 2;
            RECT r = btn((Id)(ID_S_2X + i));
            const bool sel = (S().fg_mode == i) && gsel;
            const bool hv = hit_rect(r, g_hover_x, g_hover_y);
            const bool on = gsel;   // 没选游戏时按钮置灰（避免写进"通用配置"）
            stroke_round(dc, r, RAD_CTL, sel ? RGB(52, 80, 118) : (hv ? C_CTL_HOVER : C_CTL),
                         sel ? C_ACCENT : C_BORDER);
            wchar_t t1[16]{}, t2[48]{};
            _snwprintf_s(t1, _TRUNCATE, L"%dx", m);
            _snwprintf_s(t2, _TRUNCATE, T(L"输入 %.1f fps", L"input %.1f fps"),
                         static_cast<double>(hz) / m);
            txt(dc, g_f_card, RECT{ r.left, r.top + 9, r.right, r.top + 37 }, t1,
                on ? (sel ? C_TEXT : C_LABEL) : C_DIM, DT_CENTER | DT_VCENTER);
            txt(dc, g_f_small, RECT{ r.left, r.top + 37, r.right, r.bottom - 7 }, t2,
                on ? (sel ? RGB(200, 220, 245) : C_DIM) : RGB(90, 90, 90),
                DT_CENTER | DT_VCENTER);
        }

        std::wstring hint;
        COLORREF hc = C_LABEL;
        if (!gsel) {
            hint = T(L"先点[当前游戏]卡片右上角的[选择游戏]。",
                     L"Pick a game first with [Choose game] at the top right.");
            hc = C_WARN;
        } else if (!g_ready.deployed) {
            hint = T(L"还没安装到游戏：点[安装 / 应用配置]把当前配置写进去。",
                     L"Not installed yet: click [Install / apply] to write this config.");
            hc = C_WARN;
        } else if (!g_ready.window) {
            hint = T(L"游戏还没运行：启动游戏并进入画面（无边框 + DLSS 帧生成）。",
                     L"Game not running: launch it and enter the game (borderless + DLSS FG).");
            hc = C_WARN;
        } else if (!g_ready.producer) {
            hint = T(L"窗口已找到，但 producer 未加载：确认游戏侧组件已生效、DLSS 帧生成已开。",
                     L"Window found but producer is not loaded: check the game-side files and DLSS FG.");
            hc = C_WARN;
        } else {
            hint = T(L"一切就绪，可以启动补帧了。",
                     L"Everything is ready: you can start frame generation.");
            hc = C_OK;
        }
        txt(dc, g_f_ui, RECT{ x0 + 18, kSC2 + 176, x1 - 232, kSC2 + 198 }, hint, hc,
            DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        RECT sr = btn(ID_S_START);
        const bool sh = hit_rect(sr, g_hover_x, g_hover_y);
        const bool can = g_running || ready;
        COLORREF sc;
        if (g_running) { sc = sh ? RGB(176, 78, 78) : RGB(148, 62, 62); }
        else if (!can)  { sc = RGB(58, 58, 58); }
        else            { sc = sh ? RGB(74, 136, 212) : RGB(62, 120, 196); }
        stroke_round(dc, sr, RAD_CARD, sc, sc);
        txt(dc, g_f_card, sr, g_running ? L"停止" : T(L"启动补帧", L"Start"),
            can ? RGB(252, 252, 252) : RGB(150, 150, 150), DT_CENTER | DT_VCENTER);
    }

    // ---------------------------------------------------------------- 状态
    {
        std::wstring badge, advice;
        COLORREF bc = C_DIM;
        status_summary(&badge, &bc, &advice);

        fill_round(dc, RECT{ x0, kSST, x1, kSSTB }, RAD_CARD, C_CARD);
        txt(dc, g_f_small, RECT{ x0 + 18, kSST + 4, x0 + 96, kSST + 40 }, L"运行状态",
            C_DIM, DT_LEFT | DT_VCENTER);

        SIZE sz{};
        HGDIOBJ of = SelectObject(dc, g_f_ui);
        GetTextExtentPoint32W(dc, badge.c_str(), (int)badge.size(), &sz);
        SelectObject(dc, of);
        RECT bd{ x0 + 96, kSST + 4, x0 + 96 + sz.cx + 40, kSST + 40 };
        stroke_round(dc, bd, 18, RGB(32, 32, 32), bc);
        draw_dot(dc, bd.left + 15, (bd.top + bd.bottom) / 2, bc);
        txt(dc, g_f_ui, RECT{ bd.left + 28, bd.top, bd.right - 10, bd.bottom }, badge,
            bc, DT_LEFT | DT_VCENTER);

        draw_btn(dc, btn(ID_S_LOGVIEW), L"查看日志", false, hover(ID_S_LOGVIEW));
        draw_btn(dc, btn(ID_S_DIAG), L"一键诊断", false, hover(ID_S_DIAG));

        // 运行数据：5 个 KPI（标签 + 大数字）
        struct Kpi { const wchar_t* label; std::wstring value; };
        wchar_t v[32]{};
        std::vector<Kpi> kpis;
        _snwprintf_s(v, _TRUNCATE, L"%d", (int)g_show);
        kpis.push_back({ T(L"输出 fps", L"output fps"), v });
        _snwprintf_s(v, _TRUNCATE, L"%.2fx", static_cast<double>(g_xn) / 100.0);
        kpis.push_back({ T(L"插帧", L"interp"), v });
        _snwprintf_s(v, _TRUNCATE, L"%.1f", (double)g_gpu);
        kpis.push_back({ T(L"消费卡 ms", L"gpu ms"), v });
        _snwprintf_s(v, _TRUNCATE, L"%.1f", (double)g_game);
        kpis.push_back({ T(L"游戏 ms", L"game ms"), v });
        _snwprintf_s(v, _TRUNCATE, L"%d", (int)g_skip);
        kpis.push_back({ T(L"丢帧", L"dropped"), v });

        const int kw = (x1 - x0 - 36) / 5;
        for (int i = 0; i < (int)kpis.size(); ++i) {
            const int kx = x0 + 18 + i * kw;
            txt(dc, g_f_small, RECT{ kx, kSST + 42, kx + kw - 8, kSST + 62 },
                kpis[(size_t)i].label, C_DIM, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            txt(dc, g_f_card, RECT{ kx, kSST + 62, kx + kw - 8, kSST + 94 },
                kpis[(size_t)i].value, C_TEXT, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        }

        txt(dc, g_f_ui, RECT{ x0 + 18, kSST + 100, x1 - 18, kSST + 124 }, advice, C_TEXT,
            DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        const std::wstring last = (!g_msg.empty() && GetTickCount() < g_msg_until)
                                      ? g_msg : last_line_snapshot();
        if (!last.empty()) {
            txt(dc, g_f_small, RECT{ x0 + 18, kSST + 130, x1 - 18, kSST + 152 },
                L"日志：" + last, C_DIM, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        } else {
            txt(dc, g_f_small, RECT{ x0 + 18, kSST + 130, x1 - 18, kSST + 152 },
                L"需要细调（GPU / 色彩 / 热键 / 帧池）？点[高级设置]；快捷键与常见问题按 F1。",
                C_DIM, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }

    if (g_focus >= 0) {
        const std::vector<SBtn>& v = simple_buttons();
        if (g_focus < (int)v.size()) { draw_focus(dc, v[g_focus].rc); }
    }
}

// 自动发现游戏窗口：游戏刚起来（producer 已加载）时直接选中它，省掉"刷新列表"。
// 只在用户没手动选过窗口、且当前没在跑插帧时生效；每 2 s 由定时器调用。
static void auto_detect_tick()
{
    if (g_running || g_user_picked) { return; }
    rescan_windows();
    int idx = -1;
    for (int i = 0; i < (int)g_wins.size(); ++i) {
        if (proc_has_module(g_wins[(size_t)i].pid, L"xess_mfg_producer.dll")) {
            idx = i;
            break;
        }
    }
    if (idx < 0) { return; }
    const WinEntry& w = g_wins[(size_t)idx];
    if (g_last_auto_hwnd == w.hwnd) { return; }
    g_last_auto_hwnd = w.hwnd;

    g_win_sel = idx;
    S().win_hwnd = w.hwnd;
    S().win_title = w.title;
    S().target_mode = 1;
    g_refresh = refresh_for_window(w.hwnd);
    sync_fg_input_from_target();
    const std::wstring dir = game_dir_for_window(w.hwnd);
    if (!dir.empty()) {
        if (g_game_dir.empty() || file_in_dir(dir, L"fakerFG.dll")) { g_game_dir = dir; }
        apply_game(dir);
        sync_fg_input_from_dir(dir);
    }
    save_all();

    wchar_t m[220]{};
    _snwprintf_s(m, _TRUNCATE, L"已自动选中游戏：%s（直接点启动即可）", w.title.c_str());
    set_msg(m);
    InvalidateRect(g_hwnd, nullptr, FALSE);
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
    case ID_FG_LOG:
        S().fg_log = !S().fg_log;
        apply_fg_input();
        break;
    case ID_NOMARKER:
        S().no_marker = !S().no_marker;
        set_msg(S().no_marker
                    ? L"关闭标记配对：退回时间戳/FIFO，可能偶发错配"
                    : L"标记配对开启（默认，推荐）");
        break;
    case ID_UPSAMPLE:
        S().upsample = !S().upsample;
        set_msg(S().upsample
                    ? L"升采样：副卡把 MV/深度重建到渲染尺寸（实验，A/B）"
                    : L"升采样关闭：MV/深度按流尺寸直接交给 XeFG");
        break;
    case ID_STRAUTO:
        S().stream_auto = !S().stream_auto;
        set_msg(S().stream_auto
                    ? L"自动档位：按 WGC 延迟自动选 StreamPercent（慢速、逐窗口）"
                    : L"自动档位关闭：沿用 fakerFG.ini 的 StreamPercent");
        break;
    case ID_STRASYM:
        S().stream_asym = !S().stream_asym;
        if (S().stream_asym && !S().upsample) {
            S().upsample = true;      // XeFG 要求 MV/深度同尺寸
            set_msg(L"MV 全分辨率：已同时打开升采样（depth 会重建到 MV 尺寸）");
        } else {
            set_msg(S().stream_asym
                        ? L"MV 全分辨率 / 深度降档（A/B）"
                        : L"恢复同档：MV 与深度使用同一个 StreamPercent");
        }
        break;
    case ID_PREFETCH:
        S().input_prefetch = !S().input_prefetch;
        set_msg(S().input_prefetch
                    ? L"输入预拷：等 WGC 时并行把 MV/深度拷进本地显存（默认）"
                    : L"输入预拷关闭：WGC 到达后再拷（旧顺序）");
        break;
    case ID_UNLOCK:  S().unlock_mfg = !S().unlock_mfg; break;
    case ID_GAME_QUIRKS:
        S().game_quirks = !S().game_quirks;
        set_msg(S().game_quirks
                    ? L"游戏适配开启：启动时套用 OptiScaler 的逐游戏输入表（默认）"
                    : L"游戏适配关闭：不套用逐游戏输入表（仅命令行 --game-quirks off 时）");
        break;
    case ID_SLACK:   S().slack1 = !S().slack1; break;
    case ID_TEAR:    S().tearing = !S().tearing; break;
    // ID_SDR is a dropdown (色彩空间 HDR10/SDR), handled in set_opt() - not a toggle.
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

// 手动缩放：按逻辑尺寸 * 比例重新摆窗口，并居中到当前显示器。
static void apply_ui_scale()
{
    g_scale = effective_scale(g_hwnd);
    if (g_hwnd == nullptr) { return; }
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        const int w = (int)(kWinW * g_scale + 0.5);
        const int h = (int)(kWinH * g_scale + 0.5);
        const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
        const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
        SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    InvalidateRect(g_hwnd, nullptr, TRUE);
}

static void set_opt(Id id, int idx)
{
    switch (id) {
    case ID_FG_MODE:
        S().fg_mode = idx % 3;
        S().interp = S().fg_mode + 2;
        break;
    case ID_INI_PROFILE:
        S().ini_full = (idx % 2) == 1;
        apply_ini_profile();
        remember_game(g_game_dir);
        break;
    case ID_STREAM_PCT:
        S().stream_pct = kStreamPct[(idx < 0 || idx > 3) ? 1 : idx];
        apply_fg_input();
        break;
    case ID_CODEC:
        S().codec = (idx < 0 || idx > 2) ? 2 : idx;
        apply_fg_input();
        set_msg(S().codec == 0 ? L"已关闭无损传输：MV/深度走跨卡直接拷贝"
                : S().codec == 1 ? L"已开启无损传输：MV/深度逐位无损（强制 1:1）"
                                 : L"无损传输=自动：能跑码流就用，跑不了自动退回老路径");
        break;
    case ID_DEPTH_PCT: {
        const int k = (idx < 0 || idx > 2) ? 1 : idx;
        S().depth_pct = kDepthPct[k];
        apply_fg_input();          // write DepthCodecPercent now, like the codec dropdown
        set_msg(S().depth_pct == 100
                    ? L"深度码流 100%：逐像素无损（depth 编码 +1.3 ms）"
                    : S().depth_pct == 150
                          ? L"深度码流 150%：depth 编码 -61%，MV 仍逐像素无损（需重启游戏）"
                          : L"深度码流 200%：更省，快速转视角时遮挡边缘风险更高（需重启游戏）");
        break;
    }
    case ID_UI_COMPOSE:
        S().ui_compose = (idx % 2) == 1;
        apply_fg_input();          // writes UiStream/UiSource now
        set_msg(S().ui_compose
                    ? L"UI 合成已开启（默认）：producer patch + XeFG mode 4，HUD 不残影（需重启游戏）"
                    : L"UI 合成已关闭：最大帧率，HUD 会在插值帧里残影（需重启游戏）");
        break;
    case ID_OUT:     S().out_mode = idx % 3; break;
    case ID_SDR:
        S().sdr = (idx % 2) == 1;
        set_msg(S().sdr ? L"SDR 输出（BT.709）：HDR 显示器上会发灰，属正常"
                        : L"HDR10 输出（BT.2020 + PQ）");
        break;
    case ID_FOV:     S().fov_sel = idx % 4; break;
    case ID_IVL:     S().ivl_sel = idx % 6; break;
    case ID_UI:      S().ui_none = (idx % 2) == 0; break;
    case ID_RT:      S().rt_sel = idx % 2; break;
    case ID_LOCK:    S().lock_fps = idx % 5; break;
    case ID_TMODE:   S().target_mode = idx % 2; break;
    case ID_UISCALE: {
        const int k = (idx < 0 || idx > 6) ? 0 : idx;
        g_scale_pct = kUiScalePct[k];
        apply_ui_scale();
        set_msg(g_scale_pct == 0 ? T(L"界面缩放：跟随系统", L"UI scale: follow system")
                                 : T(L"界面缩放已更新", L"UI scale updated"));
        break;
    }
    case ID_LANG:
        g_lang = (idx % 2 == 0) ? 0 : 1;
        ini_w(L"ui", L"lang", g_lang);
        set_msg(g_lang == 0 ? L"界面语言：中文" : L"Language: English");
        InvalidateRect(g_hwnd, nullptr, TRUE);
        break;
    case ID_GPUSEL:  S().gpu_sel = idx % (1 + (int)g_adapters.size()); break;
    case ID_HOTKEY:
        S().hotkey = idx % kHotkeyCount;
        g_hk_custom = false;
        apply_hotkey();
        break;
    case ID_TARGET:
        if (idx >= 0 && idx < (int)g_wins.size()) {
            g_user_picked = true;
            g_win_sel = idx;
            S().win_title = g_wins[idx].title;
            S().win_hwnd = g_wins[idx].hwnd;
            S().target_mode = 1;      // a picked window wins over "foreground at hotkey"
            g_refresh = refresh_for_window(g_wins[idx].hwnd);
            // 游戏库：这个游戏以前用过的倍率/锁帧先铺上，FgInput 以游戏目录的
            // fakerFG.ini 为准（sync_fg_input_from_target 会覆盖）。
            {
                const std::wstring dir = game_dir_for_window(g_wins[idx].hwnd);
                if (!dir.empty()) {
                    g_game_dir = dir;
                    apply_game(dir);
                }
            }
            sync_fg_input_from_target();
            refresh_readiness();
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
    const int rowH = popup_row_h(n);
    const int w = (int)(g_popup_rect.right - g_popup_rect.left);
    const int x = g_popup_rect.left;
    const int y = popup_top(n, rowH);
    if (mx < x || mx >= x + w || my < y + 4 || my >= y + 4 + n * rowH) { return -1; }
    return (my - (y + 4)) / rowH;
}


// ------------------------------------------------------------------- actions
static void drawer_focus_step(int dir);   // 抽屉键盘焦点（供 simple_action 使用）
static void uia_notify_focus();           // UIA 焦点事件（Tab 改变焦点时）

static void simple_action(Id id)
{
    switch (id) {
    case ID_S_INSTALL: {
        // 有选中游戏时直接装到它自己的目录（不再每次都弹文件选择框）。
        std::wstring dir = g_game_dir;
        if (dir.empty() || find_game_mut(dir) == nullptr) {
            if (!pick_game_exe(&dir)) { return; }
        }
        if (game_dir_running(dir)) {
            uilog("install: blocked (game running)");
            MessageBoxW(g_hwnd,
                        T(L"游戏正在运行：请先完全退出游戏再安装。\r\n\r\n"
                           L"替换 dxgi.dll 时文件被占用会失败。",
                          L"The game is running: close it completely before installing.\r\n\r\n"
                          L"dxgi.dll is locked while the game runs."),
                        T(L"请先关闭游戏", L"Close the game first"),
                        MB_OK | MB_ICONWARNING);
            return;
        }
        wchar_t cfg[160]{};
        _snwprintf_s(cfg, _TRUNCATE,
                     T(L"\r\n\r\n同时写入当前配置：%dx / %s。",
                       L"\r\n\r\nCurrent config will be written: %dx / %s."),
                     S().fg_mode + 2, fg_input_ini_value(S().fg_input));
        const std::wstring q = T(L"把锁帧组件安装到：\r\n\r\n", L"Install into:\r\n\r\n") + short_path(dir) +
                               cfg +
                               T(L"\r\n\r\n会覆盖 dxgi.dll / nvngx_dlssg.dll / fakerFG.dll / xess_mfg_producer.dll，"
                                 L"并按所选版本重写 fakerFG.ini（旧文件会备份；原 dxgi.dll / nvngx_dlssg.dll 可还原）。继续？",
                                 L"\r\n\r\ndxgi.dll / nvngx_dlssg.dll / fakerFG.dll / xess_mfg_producer.dll will be "
                                 L"overwritten, and fakerFG.ini is rewritten from the selected template "
                                 L"(old file backed up; original dxgi.dll / nvngx_dlssg.dll can be restored). Continue?");
        if (MessageBoxW(g_hwnd, q.c_str(), T(L"安装到游戏", L"Install to game"),
                        MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
        std::wstring log;
        const bool ok = deploy_game_dir(dir, &log);
        if (ok) {
            g_game_dir = dir;
            remember_game(dir);
            if (GameEntry* ge2 = find_game_mut(dir)) { ensure_entry_resolved(*ge2); }
            apply_config_after_deploy(dir);   // 安装 = 应用该游戏保存的配置
        }
        ini_ws(L"ui", L"game_dir", g_game_dir);
        refresh_readiness();
        uilog("deploy: %ls -> %d", dir.c_str(), ok ? 1 : 0);
        MessageBoxW(g_hwnd, log.c_str(),
                    ok ? T(L"安装完成", L"Installed") : T(L"安装失败", L"Install failed"),
                    MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
        break;
    }
    case ID_S_UNDEPLOY: {
        if (g_game_dir.empty()) { set_msg(L"还没有选择游戏目录，无法还原"); break; }
        if (game_dir_running(g_game_dir)) {
            MessageBoxW(g_hwnd,
                        T(L"游戏正在运行：请先完全退出游戏再还原。",
                          L"The game is running: close it completely before restoring."),
                        T(L"请先关闭游戏", L"Close the game first"),
                        MB_OK | MB_ICONWARNING);
            break;
        }
        const std::wstring q = L"要从下面目录移除锁帧组件吗？\r\n\r\n" + short_path(g_game_dir) +
                               L"\r\n\r\n会还原游戏原来的 dxgi.dll；fakerFG.ini 会保留。";
        if (MessageBoxW(g_hwnd, q.c_str(), L"还原游戏目录",
                        MB_YESNO | MB_ICONWARNING) != IDYES) {
            return;
        }
        std::wstring log;
        const bool ok = undeploy_game_dir(g_game_dir, &log);
        refresh_readiness();
        uilog("undeploy: %ls -> %d", g_game_dir.c_str(), ok ? 1 : 0);
        MessageBoxW(g_hwnd, log.c_str(), ok ? L"已还原" : L"无需还原",
                    MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
        break;
    }
    case ID_S_WINDOW: {
        rescan_windows();
        refresh_readiness();
        if (g_wins.empty()) {
            set_msg(L"没找到游戏窗口：先进游戏画面，再点[刷新列表]");
            break;
        }
        for (const SBtn& b : simple_buttons()) {
            if (b.id == ID_S_WINDOW) { g_popup_rect = b.rc; }
        }
        g_popup_id = (int)ID_TARGET;
        g_popup_hover = -1;
        break;
    }
    case ID_S_REFRESH:
        rescan_windows();
        refresh_readiness();
        set_msg(g_wins.empty() ? L"没找到游戏窗口（需要 ≥1280x720 的可见窗口）"
                               : L"窗口列表已刷新");
        break;
    case ID_S_GAMELIB:
        open_library();
        break;
    case ID_S_PICKEXE: {
        GameEntry* e = find_game_mut(g_game_dir);
        if (e == nullptr) {
            set_msg(T(L"先选择一个游戏，再改它的主程序",
                      L"Select a game first, then change its main exe"));
            break;
        }
        open_exe_picker((int)(e - &g_games[0]), false);
        break;
    }
    case ID_S_2X:
    case ID_S_3X:
    case ID_S_4X:
        if (find_game_mut(g_game_dir) == nullptr) {
            set_msg(T(L"先在左上角选择一个游戏，再改倍率",
                      L"Pick a game at the top right before changing the multiplier"));
            break;
        }
        set_opt(ID_FG_MODE, (int)id - (int)ID_S_2X);
        refresh_readiness();
        break;
    case ID_S_START:
        if (!g_running) {
            if (!g_ready.deployed) {
                set_msg(T(L"先安装到游戏：点[安装 / 应用配置]",
                          L"Install to the game first: click [Install / apply]"));
                break;
            }
            if (!g_ready.window) {
                set_msg(T(L"还没检测到游戏窗口：先进游戏画面，再点[刷新]",
                          L"No game window yet: enter the game, then click [Refresh]"));
                break;
            }
        }
        toggle_bridge(true);
        break;
    case ID_S_DIAG:  run_diagnostics(); break;
    case ID_S_LOGVIEW: open_log_view(); break;
    case ID_S_HELP:  open_help(); break;
    case ID_S_ADV: {
        toggle_advanced();
        break;
    }
    default: break;
    }
    // 配置属于当前游戏：任何一步改动都立刻落盘（倍率 / 输入源 / 锁帧…）。
    save_all();
}
static bool simple_key(UINT vk)
{
    const std::vector<SBtn>& v = simple_buttons();
    const int n = (int)v.size();
    if (n == 0) { return false; }

    // 下拉列表优先：方向键选择、Enter 确认、Esc 关闭。
    if (g_popup_id) {
        const int cnt = opt_count((Id)g_popup_id);
        if (vk == VK_ESCAPE) {
            g_popup_id = 0;
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return true;
        }
        if (vk == VK_DOWN || vk == VK_UP) {
            if (cnt > 0) {
                g_popup_hover = (g_popup_hover < 0) ? 0 : g_popup_hover;
                g_popup_hover += (vk == VK_DOWN) ? 1 : -1;
                if (g_popup_hover < 0) { g_popup_hover = cnt - 1; }
                if (g_popup_hover >= cnt) { g_popup_hover = 0; }
                InvalidateRect(g_hwnd, nullptr, FALSE);
            }
            return true;
        }
        if (vk == VK_RETURN) {
            if (g_popup_hover >= 0) { set_opt((Id)g_popup_id, g_popup_hover); }
            g_popup_id = 0;
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return true;
        }
        return false;
    }

    if (vk == VK_TAB) {
        const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (g_focus < 0) { g_focus = back ? n - 1 : 0; }
        else { g_focus = (g_focus + (back ? n - 1 : 1)) % n; }
        uia_notify_focus();
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return true;
    }
    if (vk == VK_RETURN || vk == VK_SPACE) {
        if (g_focus >= 0 && g_focus < n) {
            simple_action(v[(size_t)g_focus].id);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return true;
        }
        return false;
    }
    if (vk == VK_ESCAPE) {
        if (g_focus >= 0) {
            g_focus = -1;
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return true;
        }
        return false;
    }
    return false;
}

// 抽屉键盘：Tab/上下移动焦点，Enter/Space 激活；下拉打开时方向键+Enter。
static bool drawer_row_focusable(const Item& it)
{
    if (!it.is_row) { return false; }
    switch (it.kind) {
    case RK_DD: case RK_TOG: case RK_SPIN: case RK_EDIT: case RK_CMDLINE: return true;
    case RK_INFO: return it.id == ID_HOTKEY;
    default: return false;
    }
}

static void drawer_focus_step(int dir)
{
    std::vector<Item> items;
    int ch = 0;
    layout_drawer_items(items, &ch);
    std::vector<int> idxs;
    for (int i = 0; i < (int)items.size(); ++i) {
        if (drawer_row_focusable(items[(size_t)i])) { idxs.push_back(i); }
    }
    if (idxs.empty()) { g_drawer_focus = -1; return; }
    int cur = -1;
    for (int k = 0; k < (int)idxs.size(); ++k) {
        if (idxs[(size_t)k] == g_drawer_focus) { cur = k; break; }
    }
    if (cur < 0) { cur = (dir >= 0) ? 0 : (int)idxs.size() - 1; }
    else { cur = (cur + dir + (int)idxs.size()) % (int)idxs.size(); }
    g_drawer_focus = idxs[(size_t)cur];

    const int view = drawer_view_h();
    const int top = items[(size_t)g_drawer_focus].rc.top - (kTitleH + 52);
    const int bottom = items[(size_t)g_drawer_focus].rc.bottom - (kTitleH + 52);
    if (top < g_drawer_scroll) { g_drawer_scroll = top; }
    if (bottom > g_drawer_scroll + view) { g_drawer_scroll = bottom - view; }
    const int maxs = (ch > view) ? (ch - view) : 0;
    if (g_drawer_scroll < 0) { g_drawer_scroll = 0; }
    if (g_drawer_scroll > maxs) { g_drawer_scroll = maxs; }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void drawer_activate()
{
    std::vector<Item> items;
    int ch = 0;
    layout_drawer_items(items, &ch);
    if (g_drawer_focus < 0 || g_drawer_focus >= (int)items.size()) { return; }
    const Item& it = items[(size_t)g_drawer_focus];
    RECT rc = it.rc;
    OffsetRect(&rc, 0, -g_drawer_scroll);
    switch (it.kind) {
    case RK_TOG: toggle_field(it.id); break;
    case RK_DD:
        g_popup_id = (int)it.id;
        g_popup_rect = RECT{ rc.right - 240, rc.top + 4, rc.right, rc.bottom - 4 };
        g_popup_hover = opt_get(it.id);
        break;
    case RK_SPIN: bump_spin(it.id, +1); break;
    case RK_EDIT:
        g_edit_id = it.id;
        g_edit_buf.clear();
        set_msg(L"输入数字，Enter 确认，Esc 取消");
        break;
    case RK_CMDLINE:
        g_show_cmd = !g_show_cmd;
        ini_w(L"ui", L"show_cmd", g_show_cmd ? 1 : 0);
        break;
    case RK_INFO:
        if (it.id == ID_HOTKEY) {
            g_capture_hotkey = true;
            set_msg(L"按下新的组合键（Esc 取消）");
        }
        break;
    default: break;
    }
    save_all();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void on_click(int mx, int my)
{
    // 0) 正在手输数字：点到别处就把当前内容提交（空 = 0 = 面板刷新）
    if (g_edit_id != ID_NONE) {
        const int v = g_edit_buf.empty() ? 0 : _wtoi(g_edit_buf.c_str());
        if (g_edit_id == ID_OUTFPS) {
            S().out_fps = (v >= 5 && v <= 1000) ? v : 0;
            save_all();
        }
        g_edit_id = ID_NONE;
        g_edit_buf.clear();
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }

    // 0.5) 欢迎页：只有一个"知道了"
    if (g_welcome) {
        if (hit_rect(welcome_rect_ok(), mx, my)) { dismiss_welcome(); }
        return;
    }

    // 0.55) 主程序候选面板（盖在游戏库之上）
    if (g_pick_view) {
        if (hit_rect(pick_rect_close(), mx, my)) { close_exe_picker(); return; }
        for (int i = 0; i < (int)g_pick_list.size(); ++i) {
            if (hit_rect(pick_row_rect(i), mx, my)) { pick_choose(i); return; }
        }
        if (!hit_rect(pick_panel_rect(), mx, my)) { close_exe_picker(); }
        return;
    }

    // 0.6) 选择游戏面板：只负责"选择 / 添加"
    if (g_lib_view) {
        if (hit_rect(lib_rect_close(), mx, my)) { close_library(); return; }
        if (hit_rect(lib_rect_manual(), mx, my)) { library_add_manual(); return; }
        const int first = g_lib_scroll / kLibRowH;
        for (int k = 0; k < lib_rows_visible(); ++k) {
            const int i = first + k;
            if (i >= (int)g_games.size()) { break; }
            if (hit_rect(lib_row_rect(i), mx, my)) {
                g_lib_sel = i;
                g_lib_msg.clear();
                library_use_selected();     // 点一行 = 选择并关闭
                return;
            }
        }
        return;
    }

    // 0.7) 帮助面板
    if (g_help_view) {
        if (hit_rect(help_rect_close(), mx, my)) { close_help(); }
        return;
    }

    // 1) 日志面板是模态的：只认刷新/关闭
    if (g_log_view) {
        if (hit_rect(log_rect_close(), mx, my)) { close_log_view(); return; }
        if (hit_rect(log_rect_refresh(), mx, my)) { open_log_view(); return; }
        if (hit_rect(log_rect_folder(), mx, my)) { open_log_folder(); return; }
        if (hit_rect(log_rect_copy(), mx, my)) {
            std::wstring all;
            for (const std::wstring& s : g_log_snap) { all += s; all += L"\r\n"; }
            copy_to_clipboard(all);
            return;
        }
        return;
    }

    // 2) an open dropdown eats the click first
    if (g_popup_id) {
        const int idx = popup_hit(mx, my);
        if (idx >= 0) { set_opt((Id)g_popup_id, idx); }
        g_popup_id = 0;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }

    // 2) title bar
    if (my < kTitleH) {
        if (hit_rect(title_min_rect(), mx, my)) { ShowWindow(g_hwnd, SW_MINIMIZE); return; }
        if (hit_rect(title_close_rect(), mx, my)) { SendMessageW(g_hwnd, WM_CLOSE, 0, 0); return; }
        return;
    }

    // 3) sidebar：游戏列表（选中即切换该游戏的配置；滚轮翻页）
    if (mx < kSideW) {
        const int rows = side_rows_max();
        for (int slot = 0; slot < rows; ++slot) {
            const int idx = g_side_scroll + slot;
            if (idx >= (int)g_games.size()) { break; }
            if (!hit_rect(side_row_rect(slot), mx, my)) { continue; }
            g_lib_sel = idx;
            if (select_game(idx)) {
                g_focus = -1;
                g_user_picked = false;
                g_last_auto_hwnd = nullptr;
                rescan_windows();
                apply_hotkey();
                refresh_readiness();
                const std::wstring m = T(L"已切换到：", L"Switched to: ") +
                                       g_games[(size_t)idx].name;
                set_msg(m.c_str());
            }
            side_ensure_visible(idx);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        }
        if (hit_rect(help_rect_entry(), mx, my)) {
            open_help();
            return;
        }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }

    // 3.4) 当前游戏名字：点击进入行内重命名；点到别处则提交
    if (g_name_edit) {
        if (hit_rect(g_name_hit, mx, my)) { return; }
        name_edit_commit(true);
    }
    if (!g_advanced && mx >= kSideW && hit_rect(g_name_hit, mx, my) &&
        find_game_mut(g_game_dir) != nullptr) {
        name_edit_start();
        return;
    }

    // 3.5) 安装/还原同一行的「高级设置」按钮（抽屉打开时它被抽屉盖住，不可点）
    if (hit_rect(adv_rect(), mx, my)) { toggle_advanced(); return; }

    // 3.6) 抽屉打开时，点击归抽屉（后面的三步流程不响应）
    if (g_advanced) {
        if (hit_rect(drawer_rect_close(), mx, my)) {
            g_advanced = false;
            g_drawer_focus = -1;
            ini_w(L"ui", L"advanced", 0);
            save_all();
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        }
        std::vector<Item> items;
        int content_h = 0;
        layout_drawer_items(items, &content_h);
        for (const Item& it : items) {
            if (!it.is_row) { continue; }
            RECT rc = it.rc;
            OffsetRect(&rc, 0, -g_drawer_scroll);
            if (it.kind == RK_HEAD) { continue; }
            if (my < rc.top || my >= rc.bottom) { continue; }
            if (mx < rc.left || mx >= rc.right) { continue; }
            if (!row_enabled(it.id)) { return; }

            switch (it.kind) {
            case RK_TOG: {
                RECT sw{ rc.right - 68, rc.top + 4, rc.right, rc.bottom - 4 };
                if (mx < sw.left || mx > sw.right || my < sw.top || my > sw.bottom) { return; }
                toggle_field(it.id);
                break;
            }
            case RK_DD: {
                RECT box{ rc.right - 240, rc.top + 4, rc.right, rc.bottom - 4 };
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
            case RK_EDIT: {
                RECT box{ rc.right - 240, rc.top + 4, rc.right, rc.bottom - 4 };
                if (mx < box.left || mx > box.right || my < box.top || my > box.bottom) { return; }
                g_edit_id = it.id;
                g_edit_buf.clear();
                set_msg(L"输入数字，Enter 确认，Esc 取消");
                break;
            }
            case RK_INFO:
                if (it.id == ID_HOTKEY) {
                    g_capture_hotkey = true;
                    set_msg(L"按下新的组合键（Esc 取消）");
                    InvalidateRect(g_hwnd, nullptr, FALSE);
                }
                return;
            case RK_CMDLINE:
                if (it.id == ID_CMD) {
                    if (g_show_cmd && mx >= rc.right - 190 && mx < rc.right - 96 &&
                        my >= rc.top && my < rc.top + 24) {
                        copy_to_clipboard(display_cmdline());
                        return;
                    }
                    g_show_cmd = !g_show_cmd;
                    g_drawer_scroll = 0;
                    ini_w(L"ui", L"show_cmd", g_show_cmd ? 1 : 0);
                    save_all();
                }
                break;
            default:
                return;
            }
            save_all();
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        }
        return;
    }

    // 4) 简易模式：只有卡片上的按钮响应
    {
        const std::vector<SBtn>& v = simple_buttons();
        for (size_t i = 0; i < v.size(); ++i) {
            if (hit_rect(v[i].rc, mx, my)) {
                g_focus = (int)i;
                uia_notify_focus();
                simple_action(v[i].id);
                InvalidateRect(g_hwnd, nullptr, FALSE);
                return;
            }
        }
    }
}


// ============================================================================
//  UIA 可访问性：把"简易模式"的按钮与运行状态暴露给讲述人 / NVDA。
//  只读属性 + Invoke 模式；抽屉与模态面板暂不暴露（收起抽屉后即可读取）。
// ============================================================================
static const int kUiaCount = 15;   // 1..12 = 简易按钮，13 帮助，14 高级，15 状态
static void uia_notify_focus();

static const Id kUiaIds[14] = {
    ID_S_INSTALL, ID_S_UNDEPLOY, ID_S_PICKEXE, ID_S_GAMELIB, ID_S_WINDOW,
    ID_S_REFRESH, ID_S_2X, ID_S_3X, ID_S_4X, ID_S_START, ID_S_LOGVIEW,
    ID_S_DIAG, ID_S_HELP, ID_S_ADV
};

static bool uia_children_visible()
{
    return !(g_advanced || g_lib_view || g_log_view || g_help_view || g_welcome);
}

static std::wstring uia_name(int elem)
{
    if (elem <= 0) { return L"MGPU 双显卡帧生成"; }
    if (elem == 15) {
        std::wstring badge, advice;
        COLORREF c = C_DIM;
        status_summary(&badge, &c, &advice);
        return L"运行状态：" + badge + L"。 " + advice;
    }
    if (elem == 13) { return L"帮助与快捷键"; }
    if (elem == 14) { return g_advanced ? L"收起高级设置" : L"高级设置"; }
    if (elem < 1 || elem > 12) { return L""; }
    switch (kUiaIds[elem - 1]) {
    case ID_S_INSTALL: return L"安装 / 应用配置";
    case ID_S_UNDEPLOY: return L"还原游戏目录";
    case ID_S_PICKEXE: return L"更换主程序";
    case ID_S_GAMELIB: return L"选择游戏";
    case ID_S_WINDOW: return L"选择游戏窗口";
    case ID_S_REFRESH: return L"刷新窗口列表";
    case ID_S_2X: return L"2 倍插帧";
    case ID_S_3X: return L"3 倍插帧";
    case ID_S_4X: return L"4 倍插帧";
    case ID_S_START: return g_running ? L"停止" : L"启动";
    case ID_S_LOGVIEW: return L"查看日志";
    case ID_S_DIAG: return L"一键诊断";
    default: return L"";
    }
}

static const wchar_t* uia_automation_id(int elem)
{
    static const wchar_t* ids[kUiaCount + 1] = {
        L"root", L"install", L"undeploy", L"pickexe", L"gamelib", L"window",
        L"refresh", L"2x", L"3x", L"4x", L"start", L"logview", L"diag",
        L"help", L"advanced", L"status"
    };
    return (elem >= 0 && elem <= kUiaCount) ? ids[elem] : L"";
}

static bool uia_logical_rect(int elem, RECT* out)
{
    if (elem >= 1 && elem <= 12) {
        const std::vector<SBtn>& v = simple_buttons();
        if (elem - 1 >= (int)v.size()) { return false; }
        *out = v[(size_t)(elem - 1)].rc;
        return true;
    }
    if (elem == 13) { *out = help_rect_entry(); return true; }
    if (elem == 14) { *out = adv_rect(); return true; }
    if (elem == 15) { *out = RECT{ kSideW + kMargin, kSST, kWinW - kMargin, kSSTB }; return true; }
    return false;
}

static RECT uia_to_screen(const RECT& r)
{
    RECT cr{};
    GetClientRect(g_hwnd, &cr);
    const double sx = (cr.right > 0) ? (double)cr.right / kWinW : 1.0;
    const double sy = (cr.bottom > 0) ? (double)cr.bottom / kWinH : 1.0;
    POINT p{ (LONG)(r.left * sx), (LONG)(r.top * sy) };
    ClientToScreen(g_hwnd, &p);
    RECT out{ p.x, p.y,
              p.x + (LONG)((r.right - r.left) * sx),
              p.y + (LONG)((r.bottom - r.top) * sy) };
    return out;
}

static HRESULT uia_set_str(VARIANT* v, const wchar_t* s)
{
    v->vt = VT_BSTR;
    v->bstrVal = SysAllocString(s != nullptr ? s : L"");
    return (v->bstrVal != nullptr) ? S_OK : E_OUTOFMEMORY;
}

static HRESULT uia_set_int(VARIANT* v, int n)
{
    v->vt = VT_I4;
    v->lVal = n;
    return S_OK;
}

static HRESULT uia_set_bool(VARIANT* v, bool b)
{
    v->vt = VT_BOOL;
    v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

static HRESULT uia_set_rect(VARIANT* v, const RECT& r)
{
    SAFEARRAY* sa = SafeArrayCreateVector(VT_R8, 0, 4);
    if (sa == nullptr) { return E_OUTOFMEMORY; }
    double* p = nullptr;
    SafeArrayAccessData(sa, reinterpret_cast<void**>(&p));
    p[0] = (double)r.left;
    p[1] = (double)r.top;
    p[2] = (double)(r.right - r.left);
    p[3] = (double)(r.bottom - r.top);
    SafeArrayUnaccessData(sa);
    v->vt = VT_ARRAY | VT_R8;
    v->parray = sa;
    return S_OK;
}

static SAFEARRAY* uia_runtime_id(int elem)
{
    SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 0, (elem == 0) ? 1 : 2);
    if (sa == nullptr) { return nullptr; }
    LONG* p = nullptr;
    SafeArrayAccessData(sa, reinterpret_cast<void**>(&p));
    p[0] = UiaAppendRuntimeId;
    if (elem != 0) { p[1] = elem; }
    SafeArrayUnaccessData(sa);
    return sa;
}

class UiaProvider;
static UiaProvider* uia_root_ptr();
static UiaProvider* uia_child_ptr(int elem);

class UiaProvider final : public IRawElementProviderSimple,
                          public IRawElementProviderFragment,
                          public IRawElementProviderFragmentRoot,
                          public IInvokeProvider
{
public:
    explicit UiaProvider(int elem) : m_elem(elem) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) { return E_INVALIDARG; }
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) {
            *ppv = static_cast<IRawElementProviderSimple*>(this);
        } else if (riid == __uuidof(IRawElementProviderFragment)) {
            *ppv = static_cast<IRawElementProviderFragment*>(this);
        } else if (riid == __uuidof(IRawElementProviderFragmentRoot) && m_elem == 0) {
            *ppv = static_cast<IRawElementProviderFragmentRoot*>(this);
        } else if (riid == __uuidof(IInvokeProvider) && m_elem >= 1 && m_elem <= 14) {
            *ppv = static_cast<IInvokeProvider*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_ref);
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        // 进程级单例，不 delete：避免 UIA 异步回调时的悬空引用。
        const long r = InterlockedDecrement(&m_ref);
        return (ULONG)(r > 0 ? r : 0);
    }

    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* p) override
    {
        if (p == nullptr) { return E_INVALIDARG; }
        *p = ProviderOptions_ServerSideProvider;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** ret) override
    {
        if (ret == nullptr) { return E_INVALIDARG; }
        *ret = nullptr;
        if (id == UIA_InvokePatternId && m_elem >= 1 && m_elem <= 14) {
            *ret = static_cast<IInvokeProvider*>(this);
            AddRef();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* v) override
    {
        if (v == nullptr) { return E_INVALIDARG; }
        VariantInit(v);
        const bool root = (m_elem == 0);
        const bool shown = uia_children_visible();

        if (id == UIA_NamePropertyId) {
            return uia_set_str(v, root ? L"MGPU 双显卡帧生成" : uia_name(m_elem).c_str());
        }
        if (id == UIA_AutomationIdPropertyId) {
            return uia_set_str(v, root ? L"root" : uia_automation_id(m_elem));
        }
        if (id == UIA_FrameworkIdPropertyId) { return uia_set_str(v, L"Win32"); }
        if (id == UIA_ClassNamePropertyId) { return uia_set_str(v, L"MGPU"); }
        if (id == UIA_IsControlElementPropertyId || id == UIA_IsContentElementPropertyId) {
            return uia_set_bool(v, true);
        }
        if (id == UIA_IsEnabledPropertyId) { return uia_set_bool(v, root || shown); }
        if (id == UIA_IsKeyboardFocusablePropertyId) {
            return uia_set_bool(v, !root && shown && m_elem <= 14);
        }
        if (id == UIA_HasKeyboardFocusPropertyId) {
            return uia_set_bool(v, !root && m_elem >= 1 && m_elem <= 12 && g_focus == m_elem - 1);
        }
        if (id == UIA_IsOffscreenPropertyId) { return uia_set_bool(v, !root && !shown); }
        if (id == UIA_ControlTypePropertyId) {
            if (root) { return uia_set_int(v, UIA_WindowControlTypeId); }
            return uia_set_int(v, (m_elem == 15) ? UIA_TextControlTypeId
                                                 : UIA_ButtonControlTypeId);
        }
        if (id == UIA_BoundingRectanglePropertyId) {
            RECT wr{};
            if (root) {
                GetWindowRect(g_hwnd, &wr);
            } else {
                RECT lr{};
                if (!uia_logical_rect(m_elem, &lr)) { return S_OK; }
                wr = uia_to_screen(lr);
            }
            return uia_set_rect(v, wr);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** p) override
    {
        if (p == nullptr) { return E_INVALIDARG; }
        *p = nullptr;
        if (m_elem != 0) { return S_OK; }
        return UiaHostProviderFromHwnd(g_hwnd, p);
    }

    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection dir,
                                       IRawElementProviderFragment** ret) override
    {
        if (ret == nullptr) { return E_INVALIDARG; }
        *ret = nullptr;
        if (m_elem == 0) {
            if (!uia_children_visible()) { return S_OK; }
            int target = 0;
            if (dir == NavigateDirection_FirstChild) { target = 1; }
            else if (dir == NavigateDirection_LastChild) { target = kUiaCount; }
            if (target != 0) {
                UiaProvider* c = uia_child_ptr(target);
                if (c != nullptr) {
                    c->AddRef();
                    *ret = static_cast<IRawElementProviderFragment*>(c);
                }
            }
            return S_OK;
        }
        UiaProvider* target = nullptr;
        if (dir == NavigateDirection_Parent) {
            target = uia_root_ptr();
        } else if (dir == NavigateDirection_NextSibling && m_elem < kUiaCount) {
            target = uia_child_ptr(m_elem + 1);
        } else if (dir == NavigateDirection_PreviousSibling && m_elem > 1) {
            target = uia_child_ptr(m_elem - 1);
        }
        if (target != nullptr) {
            target->AddRef();
            *ret = static_cast<IRawElementProviderFragment*>(target);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** ret) override
    {
        if (ret == nullptr) { return E_INVALIDARG; }
        *ret = uia_runtime_id(m_elem);
        return (*ret != nullptr) ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* r) override
    {
        if (r == nullptr) { return E_INVALIDARG; }
        RECT wr{};
        if (m_elem == 0) {
            GetWindowRect(g_hwnd, &wr);
        } else {
            RECT lr{};
            if (!uia_logical_rect(m_elem, &lr)) {
                r->left = r->top = r->width = r->height = 0.0;
                return S_OK;
            }
            wr = uia_to_screen(lr);
        }
        r->left = (double)wr.left;
        r->top = (double)wr.top;
        r->width = (double)(wr.right - wr.left);
        r->height = (double)(wr.bottom - wr.top);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** ret) override
    {
        if (ret == nullptr) { return E_INVALIDARG; }
        *ret = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetFocus() override
    {
        if (m_elem >= 1 && m_elem <= 12 && uia_children_visible()) {
            g_focus = m_elem - 1;
            InvalidateRect(g_hwnd, nullptr, FALSE);
            uia_notify_focus();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** ret) override
    {
        if (ret == nullptr) { return E_INVALIDARG; }
        *ret = nullptr;
        UiaProvider* root = uia_root_ptr();
        if (root != nullptr) {
            root->AddRef();
            *ret = static_cast<IRawElementProviderFragmentRoot*>(root);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y,
                                                       IRawElementProviderFragment** ret) override
    {
        if (ret == nullptr) { return E_INVALIDARG; }
        *ret = nullptr;
        if (!uia_children_visible()) { return S_OK; }
        const POINT pt{ (LONG)x, (LONG)y };
        for (int elem = 1; elem <= kUiaCount; ++elem) {
            RECT lr{};
            if (!uia_logical_rect(elem, &lr)) { continue; }
            const RECT sr = uia_to_screen(lr);
            if (pt.x >= sr.left && pt.x < sr.right && pt.y >= sr.top && pt.y < sr.bottom) {
                UiaProvider* c = uia_child_ptr(elem);
                if (c != nullptr) {
                    c->AddRef();
                    *ret = static_cast<IRawElementProviderFragment*>(c);
                }
                return S_OK;
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** ret) override
    {
        if (ret == nullptr) { return E_INVALIDARG; }
        *ret = nullptr;
        if (!uia_children_visible()) { return S_OK; }
        if (g_focus >= 0 && g_focus <= 10) {
            UiaProvider* c = uia_child_ptr(g_focus + 1);
            if (c != nullptr) {
                c->AddRef();
                *ret = static_cast<IRawElementProviderFragment*>(c);
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Invoke() override
    {
        if (!uia_children_visible() || m_elem < 1 || m_elem > 14) {
            return UIA_E_ELEMENTNOTENABLED;
        }
        PostMessageW(g_hwnd, WM_APP + 2, (WPARAM)kUiaIds[m_elem - 1], 0);
        return S_OK;
    }

private:
    long m_ref = 1;
    int m_elem = 0;
};

static UiaProvider* g_uia_root = nullptr;
static UiaProvider* g_uia_child[kUiaCount + 1] = {};

static UiaProvider* uia_root_ptr()
{
    if (g_uia_root == nullptr) { g_uia_root = new UiaProvider(0); }
    return g_uia_root;
}

static UiaProvider* uia_child_ptr(int elem)
{
    if (elem < 1 || elem > kUiaCount) { return nullptr; }
    if (g_uia_child[elem] == nullptr) { g_uia_child[elem] = new UiaProvider(elem); }
    return g_uia_child[elem];
}

static void uia_notify_focus()
{
    if (g_hwnd == nullptr || !uia_children_visible()) { return; }
    if (g_focus < 0 || g_focus > 10) { return; }
    UiaProvider* c = uia_child_ptr(g_focus + 1);
    if (c == nullptr) { return; }
    if (UiaClientsAreListening()) {
        UiaRaiseAutomationEvent(static_cast<IRawElementProviderSimple*>(c),
                                UIA_AutomationFocusChangedEventId);
    }
    NotifyWinEvent(EVENT_OBJECT_FOCUS, g_hwnd, OBJID_CLIENT, g_focus + 1);
}

// ------------------------------------------------------------ window procedure
static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_GETOBJECT:
        if ((LONG)lp == (LONG)UiaRootObjectId) {
            return (LRESULT)UiaReturnRawElementProvider(
                h, wp, lp, static_cast<IRawElementProviderSimple*>(uia_root_ptr()));
        }
        break;

    case WM_APP + 2:
        simple_action((Id)wp);
        return 0;

    case WM_CREATE:
        g_hwnd = h;
        g_scale = effective_scale(h);
        {
            RECT cr{};
            GetClientRect(h, &cr);
            uilog("dpi: aware_call=%d dpi=%u scale=%.2f client=%ldx%ld",
                  (int)g_dpi_aware, GetDpiForWindow(h), g_scale,
                  (long)cr.right, (long)cr.bottom);
        }
        verify_package();
        InitializeCriticalSection(&g_line_cs);
        g_line_cs_ready = true;
        // Rounded corners + a hairline border, like the reference UI (Win11 only; the
        // call fails harmlessly on older builds).
        {
            const DWORD pref = 2 /* DWMWCP_ROUND */;
            DwmSetWindowAttribute(h, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
                                  &pref, sizeof(pref));
            const COLORREF border = RGB(48, 48, 48);
            DwmSetWindowAttribute(h, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
        }
        g_f_title = CreateFontW(-26, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_f_card = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_f_ui = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_f_small = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_f_mono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               FIXED_PITCH, L"Consolas");
        {
            DEVMODEW dm{}; dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
                g_refresh = dm.dmDisplayFrequency;
            }
        }
        CloseHandle(CreateThread(nullptr, 0, hotkey_poll_thread, nullptr, 0, nullptr));
        uilog("ui started, version %s, hotkey index=%d",
              LOCK_FRAME_VERSION_STRING, S().hotkey);
        prune_old_logs();
        rescan_windows();
        sync_fg_input_from_target();
        // 用游戏库选中的目录覆盖（它才是 UI 里显示的"当前游戏"）。
        if (!g_game_dir.empty()) { sync_fg_input_from_dir(g_game_dir); }
        if (HWND t = config_target_window()) { g_refresh = refresh_for_window(t); }
        refresh_readiness();
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
                set_msg(L"消费端已退出");
            }
        }
        // 就绪状态 2 s 刷新一次（模块枚举比 UI 重绘贵，不跟着 500 ms 跑）。
        static int tick = 0;
        ++tick;
        if ((tick & 3) == 0) { auto_detect_tick(); }      // 自动找游戏窗口：4 s 一次
        if ((tick & 7) == 0) { background_initialize(); } // 后台初始化游戏条目：4 s 一个
        if ((tick & 1) == 0) { refresh_readiness(); }     // 就绪状态：2 s 一次
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_APP + 1: InvalidateRect(h, nullptr, FALSE); return 0;

    case WM_HOTKEY:
        uilog("WM_HOTKEY id=%u", (unsigned)wp);
        if (wp == 1) { toggle_bridge(false); }
        return 0;

    case WM_MOUSEMOVE: {
        const POINT lp_ = to_logical(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        g_hover_x = lp_.x;
        g_hover_y = lp_.y;
        const Id hid = hover_id_at(g_hover_x, g_hover_y);
        if (hid != (Id)g_tip_id) {
            g_tip_id = (int)hid;
            g_tip_since = GetTickCount();
        }
        if (g_popup_id) { g_popup_hover = popup_hit(g_hover_x, g_hover_y); }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSELEAVE:
        g_hover_x = g_hover_y = -1;
        g_popup_hover = -1;
        g_tip_id = 0;
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        if (g_capture_hotkey) { return 0; }
        const POINT lp_ = to_logical(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        on_click(lp_.x, lp_.y);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (g_lib_view) {
            const int view = lib_rows_visible() * kLibRowH;
            g_lib_scroll += (delta / WHEEL_DELTA) * (kLibRowH * 3);
            if (g_lib_scroll < 0) { g_lib_scroll = 0; }
            const int maxs = (int)g_games.size() * kLibRowH - view;
            if (g_lib_scroll > maxs) { g_lib_scroll = (maxs > 0) ? maxs : 0; }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (g_log_view) {
            log_view_scroll((delta / WHEEL_DELTA) * 3);
            return 0;
        }
        if (g_advanced) {
            const int view = drawer_view_h();
            const int maxs = (g_drawer_h > view) ? (g_drawer_h - view) : 0;
            g_drawer_scroll -= (delta / WHEEL_DELTA) * 60;
            if (g_drawer_scroll < 0) { g_drawer_scroll = 0; }
            if (g_drawer_scroll > maxs) { g_drawer_scroll = maxs; }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        {   // 侧栏游戏列表翻页
            POINT mp{};
            GetCursorPos(&mp);
            ScreenToClient(h, &mp);
            const POINT lq = to_logical(mp.x, mp.y);
            if (lq.x < kSideW && lq.y > kTitleH) {
                g_side_scroll -= (delta / WHEEL_DELTA) * 3;
                if (g_side_scroll < 0) { g_side_scroll = 0; }
                if (g_side_scroll > side_max_scroll()) { g_side_scroll = side_max_scroll(); }
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
        }
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p{};
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            const POINT q = to_logical(p.x, p.y);
            SetCursor(LoadCursorW(nullptr, is_clickable_at(q.x, q.y) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_NCHITTEST: {
        const LRESULT r = DefWindowProcW(h, msg, wp, lp);
        if (r == HTCLIENT) {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(h, &p);
            const POINT q = to_logical(p.x, p.y);
            if (q.y < kTitleH && q.x < kWinW - 200) { return HTCAPTION; }
        }
        return r;
    }

    case WM_CHAR:
        // 游戏配置名的行内编辑
        if (g_name_edit) {
            const wchar_t ch = (wchar_t)wp;
            if (ch == 8) {
                if (g_name_fresh) { g_name_buf.clear(); g_name_fresh = false; }
                else if (!g_name_buf.empty()) { g_name_buf.pop_back(); }
            } else if (ch == 13) {
                name_edit_commit(true);
                return 0;
            } else if (ch == 27) {
                name_edit_commit(false);
                return 0;
            } else if (ch >= 32 && g_name_buf.size() < 64) {
                if (g_name_fresh) { g_name_buf.clear(); g_name_fresh = false; }
                g_name_buf.push_back(ch);
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        // 手输数字框（输出帧率上限）。只吃 0-9 / 退格 / 回车 / Esc，其余丢掉，
        // 免得一次误按把别的字母塞进去。
        if (g_edit_id != ID_NONE) {
            const wchar_t ch = (wchar_t)wp;
            if (ch >= L'0' && ch <= L'9') {
                if (g_edit_buf.size() < 4) { g_edit_buf.push_back(ch); }
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (ch == 8) {                       // backspace
                if (!g_edit_buf.empty()) { g_edit_buf.pop_back(); }
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (ch == 13) {                      // enter: commit
                const int v = g_edit_buf.empty() ? 0 : _wtoi(g_edit_buf.c_str());
                if (g_edit_id == ID_OUTFPS) {
                    S().out_fps = (v >= 5 && v <= 1000) ? v : 0;
                    set_msg(S().out_fps ? L"输出帧率上限已设置" : L"输出帧率 = 面板刷新");
                    save_all();
                }
                g_edit_id = ID_NONE;
                g_edit_buf.clear();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (ch == 27) {                      // esc: cancel
                g_edit_id = ID_NONE;
                g_edit_buf.clear();
                set_msg(L"已取消");
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        return 0;

    case WM_KEYDOWN:
        // 行内重命名时不响应热键 / 焦点切换；Esc 取消、Enter 保存。
        if (g_name_edit) {
            if (wp == VK_ESCAPE) { name_edit_commit(false); }
            else if (wp == VK_RETURN) { name_edit_commit(true); }
            return 0;
        }
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
            return 0;
        }
        // 简易模式：Tab / Enter / Space / Esc / 方向键（下拉列表）
        if (g_help_view) {
            if (wp == VK_ESCAPE || wp == VK_F1) { close_help(); }
            return 0;
        }
        if (wp == VK_F1) { open_help(); return 0; }
        if (g_pick_view) {
            if (wp == VK_ESCAPE) { close_exe_picker(); }
            else if (wp == VK_RETURN && !g_pick_list.empty()) { pick_choose(0); }
            return 0;
        }
        if (g_lib_view) {
            const int n = (int)g_games.size();
            if (wp == VK_ESCAPE) { close_library(); return 0; }
            if (wp == VK_DOWN || wp == VK_UP) {
                if (n > 0) {
                    if (g_lib_sel < 0) { g_lib_sel = 0; }
                    else { g_lib_sel += (wp == VK_DOWN) ? 1 : -1; }
                    if (g_lib_sel < 0) { g_lib_sel = 0; }
                    if (g_lib_sel >= n) { g_lib_sel = n - 1; }
                    lib_ensure_visible();
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
            if (wp == VK_RETURN) { library_use_selected(); return 0; }
            if (wp == VK_PRIOR || wp == VK_NEXT) {
                const int step = (wp == VK_PRIOR) ? lib_rows_visible() : -lib_rows_visible();
                if (n > 0) {
                    if (g_lib_sel < 0) { g_lib_sel = 0; }
                    else { g_lib_sel += step; }
                    if (g_lib_sel < 0) { g_lib_sel = 0; }
                    if (g_lib_sel >= n) { g_lib_sel = n - 1; }
                    lib_ensure_visible();
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
            return 0;
        }
        if (g_welcome) {
            if (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_SPACE) { dismiss_welcome(); }
            return 0;
        }
        if (g_log_view) {
            if (wp == VK_ESCAPE) { close_log_view(); return 0; }
            if (wp == VK_PRIOR)  { log_view_scroll(log_view_rows()); return 0; }
            if (wp == VK_NEXT)   { log_view_scroll(-log_view_rows()); return 0; }
            if (wp == VK_HOME)   { g_log_scroll = log_max_scroll();
                                   InvalidateRect(h, nullptr, FALSE); return 0; }
            if (wp == VK_END)    { g_log_scroll = 0;
                                   InvalidateRect(h, nullptr, FALSE); return 0; }
            return 0;
        }
        if (!g_advanced && simple_key((UINT)wp)) { return 0; }
        if (g_advanced) {
            if (g_popup_id) {
                const int cnt = opt_count((Id)g_popup_id);
                if (wp == VK_ESCAPE) {
                    g_popup_id = 0;
                } else if (wp == VK_DOWN || wp == VK_UP) {
                    if (cnt > 0) {
                        g_popup_hover = (g_popup_hover < 0) ? 0 : g_popup_hover;
                        g_popup_hover += (wp == VK_DOWN) ? 1 : -1;
                        if (g_popup_hover < 0) { g_popup_hover = cnt - 1; }
                        if (g_popup_hover >= cnt) { g_popup_hover = 0; }
                    }
                } else if (wp == VK_RETURN) {
                    if (g_popup_hover >= 0) { set_opt((Id)g_popup_id, g_popup_hover); }
                    g_popup_id = 0;
                }
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            const int view = drawer_view_h();
            const int maxs = (g_drawer_h > view) ? (g_drawer_h - view) : 0;
            if (wp == VK_ESCAPE) {
                g_advanced = false;
                g_drawer_focus = -1;
                ini_w(L"ui", L"advanced", 0);
                save_all();
            } else if (wp == VK_TAB) {
                const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                drawer_focus_step(back ? -1 : 1);
            } else if (wp == VK_DOWN || wp == VK_UP) {
                drawer_focus_step((wp == VK_DOWN) ? 1 : -1);
            } else if (wp == VK_RETURN || wp == VK_SPACE) {
                drawer_activate();
            } else if (wp == VK_PRIOR) {
                g_drawer_scroll -= view - 40;
            } else if (wp == VK_NEXT) {
                g_drawer_scroll += view - 40;
            } else if (wp == VK_HOME) {
                g_drawer_scroll = 0;
            } else if (wp == VK_END) {
                g_drawer_scroll = maxs;
            }
            if (g_drawer_scroll < 0) { g_drawer_scroll = 0; }
            if (g_drawer_scroll > maxs) { g_drawer_scroll = maxs; }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        return 0;

    case WM_DPICHANGED: {
        if (g_scale_pct >= 80) {                 // 手动缩放优先，不跟显示器 DPI 变
            g_scale = (double)g_scale_pct / 100.0;
            InvalidateRect(h, nullptr, TRUE);
            return 0;
        }
        const UINT dpi = (UINT)LOWORD(wp);
        g_scale = (dpi < 96 ? 96.0 : (double)dpi) / 96.0;
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        if (r != nullptr) {
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InvalidateRect(h, nullptr, TRUE);
        return 0;
    }

    case WM_CLOSE:
        if (g_running) { stop_bridge(); }
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        UiaDisconnectAllProviders();
        KillTimer(h, 1);
        UnregisterHotKey(h, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Keep the producer alive across the UI's lifetime: the producer only creates the
// shared mapping while this named event exists. Without it, the "is this the producer"
// check below runs before lock_frame.exe has ever started and would refuse the first
// launch of a freshly started game.
static HANDLE g_consumer_present_ev = nullptr;

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_dpi_aware = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_consumer_present_ev = CreateEventW(nullptr, TRUE, FALSE, mgpu::kConsumerPresentEvent);
    if (g_consumer_present_ev != nullptr) { SetEvent(g_consumer_present_ev); }
    enum_adapters();      // must precede load_all(): the GPU row is index-based
    load_all();
    if (g_profiles.empty()) { g_profiles.push_back(Profile{}); }
    g_scale = effective_scale(nullptr);   // 手动缩放（ini）优先，否则跟随系统 DPI

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"lock_frame_ui";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"lock_frame_ui", L"MGPU 固定倍率 2x / 3x / 4x", WS_POPUP | WS_MINIMIZEBOX,
                                (GetSystemMetrics(SM_CXSCREEN) - (int)(kWinW * g_scale)) / 2,
                                (GetSystemMetrics(SM_CYSCREEN) - (int)(kWinH * g_scale)) / 2,
                                (int)(kWinW * g_scale + 0.5), (int)(kWinH * g_scale + 0.5),
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) { return 1; }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}

