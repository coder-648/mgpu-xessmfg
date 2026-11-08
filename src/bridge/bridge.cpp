// ============================================================================
// bridge.exe - M4.2b: run XeSS-FG on the second GPU and present the result
//
// Runs in its own process (ReShade's D3D12 hooks are process global, so a
// second-vendor device inside the game deadlocks - see docs/findings/milestones/M3.2_RESULTS.md).
//
// Inputs, all without adding GPU cost to the game:
//   colour : Windows.Graphics.Capture of the game window, R16G16B16A16_FLOAT
//            (scRGB, HDR preserved). DWM already copies the frame across to this
//            GPU to put it on screen, so we reuse that transfer.
//   mv     : shared memory slot, 2560x1440 R16G16_FLOAT   (from the NGX hook)
//   depth  : shared memory slot, 2560x1440 R32_TYPELESS
//   jitter : shared memory frame record
//
// Flow per WGC frame:
//   WGC texture (D3D11) --CopyResource--> shared NT texture (D3D11)
//      --D3D11Fence--> D3D12 CopyResource --> XeSS-FG back buffer
//      tag mv/depth (one frame behind), tag constants (jitter + approximate
//      projection), SetPresentId, Present, xellSleep
//
// Build: ./scripts/build_m0.sh bridge
// ============================================================================

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <dwmapi.h>      // DWMWA_*: keep the overlay out of Aero Peek / Task View thumbnails
#include <shobjidl.h>    // ITaskbarList: remove a task-view entry that already exists
#include <tlhelp32.h>
#include <wrl/client.h>
#include <roapi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")     // CLSID_TaskbarList

#include <winrt/windows.graphics.capture.h>
#include <winrt/windows.graphics.directx.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include "xell/xell.h"
#include "xell/xell_d3d12.h"
#include "osd_panel.h"
#include "xess_fg/xefg_swapchain.h"
#include "xess_fg/xefg_swapchain_d3d12.h"
#include "xess_fg/xefg_swapchain_debug.h"

// libxess_fg.dll is loaded at runtime instead of being linked: MFG support lives
// in that file (see src/xefg_dyn.h for the evidence), so the file has to be
// selectable and reportable.
#include "xefg_dyn.h"
// OptiScaler's Dynamic MFG count controller (verbatim port, MIT) - see the header.
#include "xefg_dynamic_count.h"
// --unlock-mfg: the byte patch that lifts the non-Intel MFG clamp (src/xefg_unlock.h).
#include "xefg_unlock.h"

#include "mgpu_shared.h"

// AMD FidelityFX optical flow: compute the motion vectors locally on the AMD card
// instead of trusting the producer's MV stream (that stream measured as garbage on
// PBZ - no correlation with the real motion at any scale, docs/findings/HANDOFF.md 8.6).
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cwctype>
#include <deque>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureItem;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics2;
using ABI::Windows::Graphics::SizeInt32;
using ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface;
using Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

// Exit now, without teardown.
//
// `return 0` from main runs the static destructors of the D3D12/COM/XeFG objects,
// and that teardown can block for seconds (measured: the launcher had to wait 1.2 s
// for WM_CLOSE, another 0.8 s for WM_QUIT and then TerminateProcess the process).
// An overlay that is being torn down has nothing worth waiting for, and the log is
// already flushed by logf_, so killing ourselves is both correct and instant.
static void hard_exit(const char* why);
// Every exit path goes through hard_exit(), so the z-order repair lives there too.
static void restore_game_zorder(const char* why);

// Line output. Every logf_ used to fflush(stdout), which is one write syscall per line and
// shows up in CPU time as soon as a diagnostic mode prints per frame (--dump, --fg-probe,
// --mv-shadow). A log line is worth a few hundred milliseconds at most, so the flush is
// paced instead: flush at most every 200 ms, and always for a line that looks like bad
// news or for anything that is about to exit. `--log-flush` restores per-line flushing
// for debugging, where the last few lines before a hard_exit are the interesting ones.
static int    g_log_flush_each = 0;      // --log-flush: fflush every line
static DWORD  g_log_last_flush = 0;

static bool log_needs_flush(const char* fmt)
{
    if (g_log_flush_each) { return true; }
    // Durability where it matters: a warning is the line someone is looking for, and it
    // is never in a hot loop.
    static const char* const kAlways[] = {
        "WARNING", "FAILED", "FAIL", "error", "ERROR", "note:", "note(", "exiting", "MISMATCH",
    };
    for (size_t i = 0; i < sizeof(kAlways) / sizeof(kAlways[0]); ++i) {
        if (strstr(fmt, kAlways[i])) { return true; }
    }
    const DWORD now = GetTickCount();
    if (now - g_log_last_flush >= 200) { g_log_last_flush = now; return true; }
    return false;
}

static void logf_(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    if (log_needs_flush(fmt)) { fflush(stdout); }
}

static void hard_exit(const char* why)
{
    logf_("[bridge] %s - exiting now\n", why);
    restore_game_zorder(why);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
    ExitProcess(0);          // not reached
}

// printf's %ls stops at the first character the C locale cannot represent, which
// truncated a log line (and swallowed its newline) when the DLL sat under a path
// with Chinese characters. Convert explicitly instead.
static std::string narrow_utf8(const std::wstring& w)
{
    if (w.empty()) { return std::string(); }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) { return std::string(); }
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &s[0], n, nullptr, nullptr);
    return s;
}

// The library reports why it refused to interpolate; without this the only
// symptom is frameGenResult = ERR_INVALID_ARGUMENT.
static void fg_log_cb(const char* message, xefg_swapchain_logging_level_t level, void*)
{
    static const char* names[] = { "debug", "info", "warn", "error" };
    logf_("[xefg %s] %s\n", names[(level <= 3) ? level : 3], message ? message : "");
}

static const char* xefg_str(xefg_swapchain_result_t r)
{
    switch (r) {
    case XEFG_SWAPCHAIN_RESULT_SUCCESS:                  return "SUCCESS";
    case XEFG_SWAPCHAIN_RESULT_WARNING_OLD_DRIVER:       return "WARN_OLD_DRIVER";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE: return "ERR_UNSUPPORTED_DEVICE";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DRIVER: return "ERR_UNSUPPORTED_DRIVER";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED:      return "ERR_UNINITIALIZED";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT:   return "ERR_INVALID_ARGUMENT";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE:             return "ERR_DEVICE";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED:        return "ERR_UNSUPPORTED";
    case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED:
        return "ERR_LATENCY_REDUCTION_UNSUPPORTED";
    case XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER:   return "ERR_WRONG_CALL_ORDER";
    default:                                            return "ERR_OTHER";
    }
}

// ------------------------------------------------------------ window lookup

// A game can run as more than one process with the same name (launcher + shipping exe,
// crash handler, ...), so collect every PID that matches and pick the largest window of
// any of them. Picking the first match used to grab the launcher, which owns no window -
// that is the "game window not found" case on UE5 titles.
struct FindWindowCtx
{
    DWORD pid = 0;          // current PID being enumerated
    HWND  best = nullptr;
    LONG  best_area = 0;
    DWORD best_pid = 0;
    int   windows = 0;      // visible windows seen for the current PID
};

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindWindowCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid || !IsWindowVisible(hwnd)) { return TRUE; }
    RECT r{};
    if (!GetClientRect(hwnd, &r)) { return TRUE; }
    const LONG area = (r.right - r.left) * (r.bottom - r.top);
    ++ctx->windows;
    if (area > ctx->best_area) {
        ctx->best_area = area;
        ctx->best = hwnd;
        ctx->best_pid = pid;
    }
    return TRUE;
}

// --game <exe>: which process owns the window we capture. Defaults to Cyberpunk, but
// any game with a ReShade add-on + DLSS works - e.g. `--game PBZ.exe`.
static std::wstring g_game_exe;               // --game <exe> (empty = use the foreground window)
static bool         g_game_exe_set = false;   // --game was given explicitly
// --pid N / --hwnd H: target a process or an exact window instead of a name. The GUI
// front-end (src/mgpu_ui) uses these so "any window" works without knowing the exe.
static DWORD  g_game_pid = 0;
static HWND   g_game_hwnd = nullptr;

// The window a GUI picker would have chosen: the largest visible, unowned, non-tool
// top-level window of that process.
static BOOL CALLBACK pick_by_pid_proc(HWND h, LPARAM lp)
{
    struct Pick { DWORD pid; HWND best; LONG area; };
    Pick* p = reinterpret_cast<Pick*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != p->pid) { return TRUE; }
    if (!IsWindowVisible(h)) { return TRUE; }
    if (GetWindow(h, GW_OWNER) != nullptr) { return TRUE; }
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) { return TRUE; }
    RECT r{};
    if (!GetClientRect(h, &r)) { return TRUE; }
    const LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > p->area) { p->area = area; p->best = h; }
    return TRUE;
}

static HWND find_window_of_pid(DWORD pid)
{
    struct Pick { DWORD pid; HWND best; LONG area; } p{ pid, nullptr, 0 };
    EnumWindows(pick_by_pid_proc, reinterpret_cast<LPARAM>(&p));
    return p.best;
}

static HWND find_game_window(const wchar_t* process_name)
{
    DWORD pids[8]{};
    int   pid_count = 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { return nullptr; }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, process_name) == 0) {
                if (pid_count < 8) { pids[pid_count++] = pe.th32ProcessID; }
            }
        } while (Process32NextW(snap, &pe));
    }
    if (pid_count == 0 && process_name[0]) {
        // Fall back to a substring match: UE5 titles often ship as
        // "Something-Win64-Shipping.exe" while the launcher is the short name.
        std::wstring stem(process_name);
        const size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos) { stem.erase(dot); }
        if (Process32FirstW(snap, &pe)) {
            do {
                if (stem.size() >= 2 && _wcsnicmp(pe.szExeFile, stem.c_str(), stem.size()) == 0) {
                    if (pid_count < 8) { pids[pid_count++] = pe.th32ProcessID; }
                }
            } while (Process32NextW(snap, &pe));
        }
    }
    CloseHandle(snap);

    if (pid_count == 0) {
        logf_("[bridge] no process named %ls is running\n", process_name);
        return nullptr;
    }

    FindWindowCtx ctx{};
    for (int i = 0; i < pid_count; ++i) {
        ctx.pid = pids[i];
        ctx.windows = 0;
        EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&ctx));
        logf_("[bridge] pid %u: %d visible window(s)\n", pids[i], ctx.windows);
    }
    if (ctx.best) {
        RECT r{};
        GetClientRect(ctx.best, &r);
        logf_("[bridge] using window %p of pid %u (client %ldx%ld)\n",
              reinterpret_cast<void*>(ctx.best), ctx.best_pid,
              static_cast<long>(r.right - r.left), static_cast<long>(r.bottom - r.top));
    } else {
        logf_("[bridge] %d matching process(es) but none has a visible window\n", pid_count);
    }
    return ctx.best;
}

// ------------------------------------------------------------ shared state

static ComPtr<ID3D11Device>         g_d3d11;
static ComPtr<ID3D11DeviceContext>  g_ctx11;
static ComPtr<ID3D11Device5>        g_dev11_5;
static ComPtr<ID3D11DeviceContext4> g_ctx11_4;
static ComPtr<ID3D11Fence>          g_fence11;
static ComPtr<ID3D12Fence>          g_fence12;

static ComPtr<ID3D12Device>              g_device;
static ComPtr<ID3D12CommandQueue>        g_queue;
static ComPtr<ID3D12CommandAllocator>    g_alloc;
static ComPtr<ID3D12GraphicsCommandList> g_list;
static ComPtr<ID3D12Fence>               g_frame_fence;
static HANDLE                            g_frame_event = nullptr;
static UINT64                            g_frame_fence_value = 0;

static ComPtr<ID3D11Texture2D> g_shared11;    // WGC frame sink (D3D11)
static ComPtr<ID3D12Resource>  g_shared12;    // the same memory, seen by D3D12

static xefg_swapchain_handle_t g_fg = nullptr;
static xell_context_handle_t   g_xell = nullptr;
static ComPtr<IDXGISwapChain4> g_swapchain;

// The application swap chain handed to XeSS-FG must outlive the call that passes
// it: it used to be a block-local ComPtr, so our only reference died right after
// InitFromSwapChain returned and the process died with it.
static ComPtr<IDXGISwapChain1> g_app_swap;

// Composition fallback: kept alive for the whole run, otherwise the swap chain
// stops being presented once these go out of scope.
static ComPtr<IDCompositionDevice> g_dcomp;
static ComPtr<IDCompositionTarget> g_dcomp_target;
static ComPtr<IDCompositionVisual> g_dcomp_visual;

static HWND g_bridge_wnd = nullptr;
// The game window we are mirroring. Focus has to stay with it: the overlay is
// click-through, but keyboard input still goes to whatever window owns the
// foreground.
static HWND g_game_wnd = nullptr;
// assert_topmost() puts the game window directly underneath our WM_EX_TOPMOST
// overlay, and Windows promotes any window inserted above a topmost one - so the
// game itself becomes topmost and *stays* topmost after we die. That is the
// "退出 FG 后游戏一直贴在桌面最上层" report. Every exit path calls
// restore_game_zorder() to undo it.
static void restore_game_zorder(const char* why)
{
    if (!g_game_wnd || !IsWindow(g_game_wnd)) { return; }
    const LONG_PTR ex = GetWindowLongPtrW(g_game_wnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOPMOST) == 0) { return; }
    SetWindowLongPtrW(g_game_wnd, GWL_EXSTYLE,
                      ex & ~static_cast<LONG_PTR>(WS_EX_TOPMOST));
    SetWindowPos(g_game_wnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    logf_("[bridge] %s: cleared WS_EX_TOPMOST on the game window %p\n",
          why, reinterpret_cast<void*>(g_game_wnd));
}
// --no-fg keeps the proxy swap chain but leaves frame generation off, so the
// picture can be compared against the interpolated one.
static bool g_no_fg = false;
// --overlay-layered: A/B switch. Some titles ignore the click-through bit until the
// window is also layered with a fully opaque alpha.
static bool g_overlay_layered = false;
// --relay-input: our overlay stops being click-through and forwards the mouse to the
// game window itself. Fallback for titles whose UI never sees the clicks even though
// the OS delivers them to their window (raw input still bypasses this, so aim/attack
// keeps working).
static bool g_relay_input = false;
static ULONGLONG g_relay_last_move = 0;
static volatile LONG g_relay_clicks = 0;
// --no-inputs skips importing the producer's cross-adapter heap entirely. That
// peer access (AMD reading NVIDIA VRAM) is the prime suspect for the NVIDIA TDRs
// (nvlddmkm id 153) that happen while the bridge runs, so this flag isolates it.
static bool g_no_inputs = false;
// --no-depth: hand XeFG a dummy depth of the right size instead of importing the real
// one. Depth is half of the cross-adapter traffic (33 MB/frame at 2160p) and only feeds
// disocclusion clamping, so this is both a measurable knob and a usable fallback when the
// import is what pushes `gpu` over the budget.
static bool g_no_depth = false;
// --no-mv: zeroed motion vectors but the real depth. Isolates "the vectors are wrong" from
// "the vectors are merely coarse" - with zero MVs a moving object must double unless depth
// plus the projection matrix can carry the motion.
static bool g_no_mv = false;
// --mv-shadow (diagnostic): with --no-mv, STILL copy the producer's motion vectors into a
// sink texture that is never tagged. That pays the full cross-adapter copy cost while XeFG
// keeps seeing zeroed vectors, which is the only way to separate "copy is expensive" from
// "XeFG actually consumes the vectors" (they were mixed together in every earlier A/B).
static bool g_mv_shadow = false;
static ComPtr<ID3D12Resource> g_mv_shadow_tex[mgpu::kMaxSlots];
// ================== EXPERIMENTAL: --fg-of (FidelityFX optical flow) =========
// Motion vectors are a DEAD INPUT for libxess_fg 1.3.1: zeroed MVs, a constant
// 100000 px field and the game's own (verified real) vectors all produce the
// identical picture, so the model estimates motion itself. Every --fg-of* knob
// and the whole flow pass are compiled OUT by default; the code and
// third_party/ffx are kept only so the conclusion can be re-tested after a
// libxess_fg upgrade (`--fg-of-const 100000 0` must break the picture again).
// Build with /DMGPU_FG_OF=1 to bring the switch back.
//   docs/findings/FINAL_CONFIG.md 2.1.2 (结论 1), docs/findings/HANDOFF.md 8.6 (第五/六轮)
#ifndef MGPU_FG_OF
#define MGPU_FG_OF 0
#endif
#if MGPU_FG_OF
// --fg-of: compute the motion vectors with FidelityFX optical flow on the AMD card from
// the captured colour, instead of copying the producer's stream.
static bool g_fg_of = false;
// --fg-of-sign / --fg-of-scale: hit the MV convention A/B without a rebuild.
// --fg-of-guide: colour-similarity weight for the edge-aware upsample (0 = bilinear).
static float g_of_sign = 1.0f;
static float g_of_scale = 1.0f;
static float g_of_guide = 48.0f;
// --fg-of-depth-k: relative-depth weight for the depth-guided upsample (0 = off).
static float g_of_depth_k = 40.0f;
static UINT  g_of_depth_desc = 0;                          // ring entry the depth SRV was last bound for
static bool  g_of_depth_bound[mgpu::kMaxSlots] = {};       // ... and whether it has been bound at all
// MV unit convention. XeFG wants pixels of the MV texture, sign (previous - current):
// see third_party/optiscaler/shaders/camera_motion/CM_Common.h.
// 0 = pixels, 1 = UV, 2 = NDC, 3 = "mvpx" (XeFG native, default).
static int   g_of_units = 3;
static float g_of_const_x = 0.0f;      // --fg-of-const X Y (px): diagnostic override
static float g_of_const_y = 0.0f;
static bool  g_of_const_on = false;
static float g_of_deadzone = 0.60f;    // px; below this a vector means "static"
static float g_of_maxlen = 240.0f;     // px; clamp for bad matches
#endif
// Debug features (OptiScaler exposes the same two): corner tags on generated frames, and
// presenting *only* interpolated frames - the real frames otherwise hide the artifacts.
static bool  g_fg_tag_interp = false;    // --fg-debug-tag-interp
static bool  g_fg_only_interp = false;   // --fg-debug-only-interp
// --capture-monitor: capture the display instead of the game window. Removes any
// window/swap-chain size mismatch between what the game renders and what WGC hands over.
// Kept for experiments
// only: it also captures our own overlay (feedback loop), so it is not usable in practice.
static bool g_capture_monitor = false;
// 0 = HDR10 + BT.2020 + PQ, 1 = HDR10 without the primaries matrix, 2 = SDR.
static int  g_colour_mode = 0;
// --half presents at half resolution: XeSS-FG interpolates at the swap chain size, so
// this quarters its GPU cost and isolates model cost from our own overhead.
static bool g_half = false;
static float g_sharpen = 0.0f;  // --sharp: unsharp-mask amount on the resize
// --down N picks the resampling kernel for the resize that happens before frame
// generation: 0 = single bilinear tap (the original behaviour), 1 = Catmull-Rom,
// 2 = Mitchell-Netravali, 3 = Lanczos-3. All three kernels are proper low-pass
// filters stretched by the scale factor, so 0.75x stops folding detail into moire.
static int   g_down = 0;
static float g_down_support = 0.0f;   // kernel support in output-pixel units
static float g_scale = 0.0f;   // --scale f: present at f x the captured size (0 = use g_out_den)
static int  g_out_den = 1;      // --den N: output at 1/N of the captured size
static UINT g_swap_buffers = 2; // --buffers N: XeSS-FG presents twice per input frame
static UINT g_wgc_pool = 2;     // --pool N: WGC frame pool depth (2..8)
static UINT g_max_latency = 1;  // --latency N: DXGI maximum frame latency
static UINT g_refresh_hz = 0;   // panel refresh, filled in once the output is known
static bool g_tearing = false;  // --tearing: never block on vblank (may tear)
static UINT g_present_flags() { return g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u; }
static int  g_xell_mode = 1;    // --xell-mode N: bLowLatencyMode handed to xellSetSleepMode
static bool g_no_xell = true;   // skip XeLL per-frame pacing by default (--xell re-enables)

// ---------------------------------------------------------------- MFG state
// --interp N: number of interpolated frames per real frame, so N + 1 frames are
// presented (1 = 2x, 2 = 3x, 3 = 4x). 0 = "whatever this DLL supports best".
// The DLL decides how much is really possible: maxSupportedInterpolations is 1
// on a non-Intel adapter with the official build, and only a community MFG build
// raises it (src/xefg_dyn.h).
static int          g_fg_interp = 0;
static bool         g_fg_interp_set = false;  // --interp was given (1 = 2x is a valid ask)
static UINT         g_fg_interp_actual = 1;   // what XeSS-FG really applies
static UINT         g_fg_interp_max = 1;      // maxSupportedInterpolations of the loaded DLL
static std::wstring g_fg_lib;              // --fg-lib <path>: alternative libxess_fg.dll
static bool         g_fg_probe = false;    // --fg-probe: report the DLL's MFG capability, exit
static bool         g_unlock_mfg = false;  // --unlock-mfg: lift the non-Intel clamp (byte patch)

// ------------------------------------------------- consumer adapter (M6.1)
// Which GPU runs XeSS-FG. This used to be hard-coded to the AMD adapter
// (VendorId 0x1002), which was the only vendor assumption left in this process:
// XeFG itself is a cross-vendor library (it ran on an RTX 5050 already, see
// docs/findings/XESS_FG_NOTES.md 1.5), and the swap chain only cares that the adapter owns
// the monitor our overlay sits on. See docs/design/VENDOR_COMPAT_PLAN.md M6.1.
//
//   --gpu <spec>          force the adapter: index / vendor id (10de, 1002, 8086)
//                         / nv|amd|intel / luid:HI:LO / description substring
//   --producer-luid HI:LO the GPU the game renders on; the pick avoids it.
//                         (protocol v6 publishes this from the add-on; until then
//                         it can be passed by hand)
static std::wstring g_gpu_spec;            // --gpu
static bool         g_have_producer = false;
static uint64_t     g_producer_luid = 0;   // packed Hi<<32 | Lo
static UINT         g_consumer_vendor = 0; // VendorId of the picked adapter
static DXGI_ADAPTER_DESC1 g_consumer_desc{};
// Protocol v6: what the producer said about its own inputs (see mgpu_shared.h).
// Set from the shared block before the FG context is created, because
// kInputFlagInvertedDepth changes the init flags.
static UINT         g_producer_api = 0;      // mgpu::kInputApi*
static UINT         g_producer_flags = 0;    // mgpu::kInputFlag*
static UINT         g_producer_render_w = 0; // upscaler input size
static UINT         g_producer_render_h = 0;
static UINT         g_producer_output_w = 0;
static UINT         g_producer_output_h = 0;
// --slack N: hold up to N captured frames and process the OLDEST first.
//
// Default 0 = drain the pool and keep only the newest frame, which is what makes
// the pipeline "zero latency": an over-budget frame costs an input frame. With
// N > 0 a late frame is absorbed by the next one instead, at the price of N input
// intervals of latency. It only helps when gpu_p50 is well under the interval and
// the tail is what misses - a sustained deficit fills the buffer and then drops
// frames anyway (measured, docs/findings/FINAL_CONFIG.md 9.8).
static UINT         g_slack = 0;
// --jbuf N (0..3): the *correct* version of the same idea (P3). Every captured frame is
// copied into one of OUR textures immediately and the WGC frame is released at once, so
// the capture pool is never held hostage - holding it is exactly what made --slack cost
// +5.7 ms of gpu time for nothing (docs/findings/FINAL_CONFIG.md 9.9). Deferring the *processing*
// by N frames absorbs jitter at the price of N input intervals of latency; unlike --slack
// it does not touch WGC or the producer.
static int          g_jbuf = 0;
static std::vector<ComPtr<ID3D11Texture2D>> g_jbuf_free;
// Returned textures wait one frame before they can be handed out again: the immediate
// context copy that filled them has to retire first (D3D11 would serialise it anyway,
// this just keeps that off the critical path).
static std::vector<ComPtr<ID3D11Texture2D>> g_jbuf_pending;
// One-shot warning: XeFG paces its batch against vblank, so a whole-frame cost
// far below the input interval while several frames are being presented means
// something else took over the presentation (an OSD or a latency tool hooking
// Present, or an occluded overlay). FPS counters still read N+1 x but the
// picture judders, so it is worth saying out loud exactly once.
static bool         g_nopace_warned = false;
static UINT         g_nopace_windows = 0;   // consecutive suspicious windows

// XeFG tuning knobs. Each mirrors something OptiScaler exposes for its own XeFG
// output; all default to our previous behaviour so an A/B run only changes one
// thing at a time.
//   --fg-rt 0      frameRenderTime = 0        (OptiScaler [FrameGen] FTInput=2)
//   --fg-ui none   UI_MODE_NONE, not AUTO     (OptiScaler [XeFG] UIComposition=false)
//   --fg-scene F   xefgSwapChainSetSceneChangeThreshold(F) (default in the DLL: 0.7)
//   --fg-depth-inv INVERTED_DEPTH init flag    (reversed/infinite-Z depth)
static bool         g_fg_rt_zero = false;
// The frame constants are the one input XeFG gets that we can only approximate. UE5's
// jitter sign and the synthetic projection matrix are the two remaining suspects for the
// "never goes away" edge / model-edge artifacts, so both are switchable at runtime.
static bool         g_fg_jitter_flip = false;   // --fg-jitter-flip
// --fg-jitter-zero: DEPRECATED (2026-11-08) and now ignored. The reasoning below used to be
// "the colour is the upscaled output, so zero the jitter" - measured wrong once motion
// vectors are enabled: the game computes its vectors on the jittered render, so XeFG needs
// the real jitter to align history. Passing the game jitter removed both the flicker and
// most of the model-edge ghosting that jitter=0 showed (PBZ, 2x@40, real MV). The flag is
// still parsed so old scripts do not fail, but it no longer does anything.
static bool         g_fg_jitter_zero = false;   // set but never used
static bool         g_fg_no_proj = false;       // --fg-no-proj
static bool         g_fg_proj_rz = false;       // --fg-proj-rz: reversed-Z infinite-far
// The projection is the one frame constant we can only approximate, so its two numbers are
// tunable: --fg-fov (vertical field of view, degrees) and --fg-zn (near plane). UE5's
// defaults are ~58.7 deg vertical (90 horizontal at 16:9) and a near plane of 10.
float               g_fg_fov_y = 60.0f;
float               g_fg_near_z = 10.0f;
static bool         g_fg_consts_logged = false;
static bool         g_fg_ui_none = false;
static float        g_fg_scene = 0.0f;
static bool         g_fg_depth_inv = false;
static xefg_dyn::Api g_api;                // resolved libxess_fg.dll entry points

// The window that actually owns the swap chain. It is not always g_bridge_wnd:
// this machine rejects CreateSwapChainForHwnd on our own class, so a plain window
// is used instead and has to become the visible overlay.
static HWND g_sc_wnd = nullptr;

// The window that is actually presented on. Since the HDR10 change XeSS-FG creates
// the swap chain itself, so g_sc_wnd stays null and g_bridge_wnd is the overlay -
// every z-order and focus decision must use this.
static HWND overlay_wnd()
{
    return g_sc_wnd ? g_sc_wnd : g_bridge_wnd;
}
static UINT g_width = 5120;
static UINT g_full_w = 5120;   // window size: always the full screen
static UINT g_full_h = 2880;
static UINT g_height = 2880;

// ---------------------------------------------------------------- overlay placement
// Which display the overlay (and therefore XeSS-FG's swap chain) is presented on.
//   -1    follow the game window (historical behaviour)
//   >=0   the Nth monitor in EnumDisplayMonitors() order   (--overlay-monitor N)
//
// This is the mirror of --capture-monitor ("which screen do we capture"): the present
// side is a separate decision, and it is also the only way to give the game its
// independent flip back - with a fullscreen topmost overlay on the SAME monitor, DWM has
// to compose both, which measured as 5-10% of captures arriving one vblank late
// (docs/findings/PERF_BASELINE.md 3.10/3.11).
static int  g_overlay_monitor = -1;
// --overlay-hidden: create the swap chain and keep presenting, but never show the window.
// Measurement only - it answers "is our overlay the reason the game loses its flip layer?"
// because with the overlay hidden the game window is the topmost visible thing again.
static bool g_overlay_hidden = false;
static bool g_overlay_hidden_warned = false;
// The adapter XeSS-FG runs on, kept so --overlay-monitor can be validated before the
// window/swap chain are created (CreateSwapChainForHwnd requires the device to drive the
// output, and failing there leaves the user with no picture at all).
static ComPtr<IDXGIAdapter1> g_consumer_adapter;

struct MonitorPick { int want; int index; HMONITOR mon; };

static BOOL CALLBACK pick_monitor_proc(HMONITOR mon, HDC, LPRECT, LPARAM lp)
{
    auto* p = reinterpret_cast<MonitorPick*>(lp);
    if (p->index == p->want) { p->mon = mon; return FALSE; }   // stop enumerating
    ++p->index;
    return TRUE;
}

static HMONITOR overlay_monitor(HWND game_wnd)
{
    if (g_overlay_monitor >= 0) {
        MonitorPick p{ g_overlay_monitor, 0, nullptr };
        EnumDisplayMonitors(nullptr, nullptr, pick_monitor_proc,
                            reinterpret_cast<LPARAM>(&p));
        if (p.mon) { return p.mon; }
        if (!g_overlay_hidden_warned) {
            g_overlay_hidden_warned = true;
            logf_("[bridge] WARNING: --overlay-monitor %d matched no monitor (%d present)"
                  " - following the game window instead\n", g_overlay_monitor, p.index);
        }
    }
    if (game_wnd) { return MonitorFromWindow(game_wnd, MONITOR_DEFAULTTOPRIMARY); }
    const POINT origin{ 0, 0 };
    return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

// The display mode of the monitor we actually present on. It used to read the *primary*
// display (EnumDisplaySettingsW(nullptr, ...)), which is wrong the moment the overlay
// lives on another output: --fg-rt-mode quant and --list-modes would snap to the wrong
// vblank. --overlay-monitor makes that a supported configuration, so read the right one.
static bool overlay_display_mode(DEVMODEW* dm)
{
    if (!dm) { return false; }
    ZeroMemory(dm, sizeof(*dm));
    dm->dmSize = sizeof(*dm);
    const HMONITOR mon = overlay_monitor(nullptr);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, reinterpret_cast<MONITORINFO*>(&mi))) {
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, dm)) { return true; }
    }
    return EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, dm) != FALSE;
}

// Where the overlay window has to be: exactly the monitor we present on. --den/--scale
// change the swap chain's back-buffer size, never the window rect (DXGI stretches it).
static void overlay_rect(RECT* out)
{
    if (!out) { return; }
    out->left = 0;
    out->top = 0;
    out->right = static_cast<LONG>(g_full_w);
    out->bottom = static_cast<LONG>(g_full_h);

    const HMONITOR mon = overlay_monitor(nullptr);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) { return; }
    *out = mi.rcMonitor;

    static bool logged = false;
    if (!logged) {
        logged = true;
        logf_("[bridge] overlay target: monitor %p rect=(%ld,%ld)-(%ld,%ld) %lux%lu"
              " (%s)\n", reinterpret_cast<void*>(mon),
              mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
              static_cast<unsigned long>(mi.rcMonitor.right - mi.rcMonitor.left),
              static_cast<unsigned long>(mi.rcMonitor.bottom - mi.rcMonitor.top),
              (g_overlay_monitor >= 0) ? "--overlay-monitor" : "follows the game window");
    }
}

static mgpu::SharedControl*   g_ctl = nullptr;
static ComPtr<ID3D12Resource> g_mv[mgpu::kMaxSlots];
static ComPtr<ID3D12Resource> g_depth[mgpu::kMaxSlots];
// v8: the mapping-backed heap is kept so the views can be re-placed when the add-on
// rebuilds its slot layout (upscaler preset/mode switch).
static ComPtr<ID3D12Heap> g_shared_heap;
static uint32_t g_seen_generation = 0;
// Separate copy for the geometry cache: the "release local copies" path and the
// "re-read stream geometry" path both key off generation, but they must be able to
// disagree (e.g. geometry unknown while generation is already current).
static uint32_t g_seen_geo_generation = 0xFFFFFFFFu;

static void refresh_input_geo();   // defined next to the stream-copy helpers below
static bool     g_layout_pending = false;
static ComPtr<ID3D12Fence>    g_fwd[mgpu::kMaxSlots];
static UINT64                 g_fwd_last[mgpu::kMaxSlots] = {};
static UINT                   g_shared_slot = 0;

// A plain perspective projection, since DLSS-SR does not publish the real one
// (docs/findings/milestones/M2_RESULTS.md section 4). Row-major.
// UE5 (and most modern titles) use a REVERSED-Z projection with an infinite far plane:
// near maps to 1, far to 0. The standard matrix below maps near to 0, so unprojecting the
// game's depth with it inverts every occlusion test - which is exactly what "artifacts on
// everything that moves, plus flicker" looks like.
//
// Since protocol v6 the FOV/near plane can come from the producer (FSR 3.1 exports
// near/far/vfov); --fg-fov/--fg-zn are the fallback for sources without a camera.
static void approx_projection_rz(float* m, float aspect, float fov_y_rad, float zn)
{
    const float f = 1.0f / tanf(fov_y_rad * 0.5f);
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect;
    m[5] = f;
    m[10] = 0.0f;                            // infinite far plane
    m[11] = -1.0f;
    m[14] = zn;                              // near plane -> 1
}

static void approx_projection(float* m, float aspect, float fov_y_rad, float zn, float zf)
{
    const float f = 1.0f / tanf(fov_y_rad * 0.5f);
    if (zf <= zn) { zf = zn + 1.0f; }
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect;
    m[5] = f;
    m[10] = zf / (zn - zf);
    m[11] = -1.0f;
    m[14] = (zn * zf) / (zn - zf);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    // The overlay must never take input away from the game.
    if (msg == WM_NCHITTEST)    { return HTTRANSPARENT; }
    if (msg == WM_MOUSEACTIVATE) { return MA_NOACTIVATE; }
    // The launcher closes us with WM_CLOSE. Answer it from here instead of letting
    // DefWindowProc destroy the window and hoping the frame loop is pumping - if it
    // happens to be blocked inside Present, that hope is worth seconds.
    if (msg == WM_CLOSE) { hard_exit("WM_CLOSE from the launcher"); }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

// The window that ends up owning the swap chain may be a system class window, so
// it has to be subclassed to make it click-through.
static WNDPROC g_overlay_prev_proc = nullptr;
static volatile LONG g_hittest_count = 0;

static void focus_game();

static LRESULT CALLBACK overlay_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST) {
        // Counted so the log proves the overlay really is in the hit-test path.
        ++g_hittest_count;
        return HTTRANSPARENT;
    }
    if (msg == WM_MOUSEACTIVATE) { return MA_NOACTIVATE; }
    if (msg == WM_CLOSE) { hard_exit("WM_CLOSE on the overlay"); }
    if (msg == WM_ACTIVATE) {
        if (LOWORD(wp) != WA_INACTIVE) { focus_game(); }
    }
    // --relay-input: forward the mouse to the game in ITS client coordinates. The
    // overlay covers the whole screen at 0,0, so screen space is the common ground.
    if (g_relay_input && g_game_wnd) {
        switch (msg) {
        case WM_MOUSEMOVE: {
            const ULONGLONG t = GetTickCount64();
            if (t - g_relay_last_move < 8) { return 0; }   // throttle: the game also gets raw input
            g_relay_last_move = t;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(h, &pt);
            ScreenToClient(g_game_wnd, &pt);
            PostMessageW(g_game_wnd, msg, wp, MAKELPARAM(pt.x, pt.y));
            return 0;
        }
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            LPARAM out = lp;
            if (msg != WM_MOUSEWHEEL) {          // wheel lParam is already screen space
                ClientToScreen(h, &pt);
                ScreenToClient(g_game_wnd, &pt);
                out = MAKELPARAM(pt.x, pt.y);
            }
            PostMessageW(g_game_wnd, msg, wp, out);
            const LONG k = InterlockedIncrement(&g_relay_clicks);
            if (k <= 4) {
                logf_("[bridge] relay #%ld: msg=0x%04X -> game at %ld,%ld\n", k, msg, pt.x, pt.y);
            }
            return 0;
        }
        default: break;
        }
    }

    // Smoking gun for "the overlay eats the click": a click-through window must never
    // see these. Logged once each so a real report is unambiguous.
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) {
        static bool logged_l = false, logged_r = false;
        bool& done = (msg == WM_LBUTTONDOWN) ? logged_l : logged_r;
        if (!done) {
            done = true;
            logf_("[bridge] WARNING overlay received %s - the click was NOT passed through\n",
                  (msg == WM_LBUTTONDOWN) ? "a left click" : "a right click");
        }
    }
    return CallWindowProcW(g_overlay_prev_proc, h, msg, wp, lp);
}

static HWND create_bridge_window(UINT w, UINT h)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"mgpu_bridge_window";
    RegisterClassExW(&wc);

    // Created without WS_EX_TOPMOST and without WS_VISIBLE, and with an exact
    // client size: a topmost or not-yet-mapped window made
    // CreateSwapChainForHwnd return E_ACCESSDENIED here (the identical swap chain
    // worked on a plain, hidden, exactly sized window). The overlay is turned into
    // a visible borderless topmost window by make_borderless() once the swap chain
    // exists.
    //
    // WS_EX_TOOLWINDOW is set *here*, at creation, so the shell never registers this as an
    // application window in the first place - that is what kept it out of Alt+Tab/Win+Tab
    // without needing an owner relationship (which broke the game's mouse input).
    RECT r{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"MGPU Bridge",
                                WS_OVERLAPPEDWINDOW, 0, 0,
                                r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd) {
        RECT client{};
        GetClientRect(hwnd, &client);
        // System metrics are not always enough for an exact client size, so measure
        // and correct once.
        const LONG dw = static_cast<LONG>(w) - (client.right - client.left);
        const LONG dh = static_cast<LONG>(h) - (client.bottom - client.top);
        if (dw || dh) {
            SetWindowPos(hwnd, nullptr, 0, 0, (r.right - r.left) + dw, (r.bottom - r.top) + dh,
                         SWP_NOZORDER | SWP_NOMOVE);
            GetClientRect(hwnd, &client);
        }
        logf_("[bridge] window client %ldx%ld (requested %ux%u)\n",
              client.right - client.left, client.bottom - client.top, w, h);
    }
    return hwnd;
}

// Turns the overlay into a visible, borderless, topmost window. Called only after
// the swap chain exists, because the window style present during
// CreateSwapChainForHwnd matters on this machine.
// Giving the foreground back is not just SetForegroundWindow: Windows blocks a
// background process from activating a window owned by another thread unless both
// input queues are attached for the call.
static void focus_game()
{
    if (!g_game_wnd) { return; }
    const HWND fg = GetForegroundWindow();
    if (fg == g_game_wnd) { return; }

    const DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD my_thread = GetCurrentThreadId();
    bool attached = false;
    if (fg_thread && fg_thread != my_thread) {
        attached = AttachThreadInput(my_thread, fg_thread, TRUE) != FALSE;
    }
    SetForegroundWindow(g_game_wnd);
    if (attached) { AttachThreadInput(my_thread, fg_thread, FALSE); }
}

// Keeps the overlay out of Alt+Tab, Task View (Win+Tab), Aero Peek and the taskbar.
//
// Why this is not just WS_EX_TOOLWINDOW: the window is created hidden and *plain* on
// purpose (CreateSwapChainForHwnd rejects a topmost/visible window on this machine), so a
// style applied afterwards can arrive after the shell already registered the entry. Hence:
//   * WS_EX_TOOLWINDOW at creation (see create_bridge_window) - never registered as an app
//   * DWMWA_EXCLUDED_FROM_PEEK / DWMWA_DISALLOW_PEEK - out of peek/thumbnail previews
//   * ITaskbarList::DeleteTab - drops an entry that already exists
//
// Deliberately NOT used: SetWindowLongPtr(GWLP_HWNDPARENT, game_wnd). Making the overlay an
// owned window of the game does group it in Task View, but it also changed how input
// reached the game: the mouse stopped responding while the keyboard kept working (measured
// by the user right after it was added). A mirror window must stay a plain top-level window.
static void overlay_hide_from_switcher(HWND hwnd)
{
    if (!hwnd) { return; }

    BOOL peek = TRUE;
    const HRESULT h_ex = DwmSetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &peek,
                                               sizeof(peek));
    const HRESULT h_dp = DwmSetWindowAttribute(hwnd, DWMWA_DISALLOW_PEEK, &peek,
                                               sizeof(peek));

    HRESULT h_tab = E_NOINTERFACE;
    ComPtr<ITaskbarList> bar;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&bar))) && bar) {
        if (SUCCEEDED(bar->HrInit())) { h_tab = bar->DeleteTab(hwnd); }
    }

    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    logf_("[bridge] switcher: peek excl hr=0x%08lX/0x%08lX, DeleteTab hr=0x%08lX,"
          " exstyle=0x%08lX (no owner - see the comment; owner broke mouse input)\n",
          static_cast<unsigned long>(h_ex), static_cast<unsigned long>(h_dp),
          static_cast<unsigned long>(h_tab),
          static_cast<unsigned long>(ex));
}

static void make_borderless(HWND hwnd, int x, int y, UINT w, UINT h)
{
    // WS_EX_TRANSPARENT  - the window is skipped when the mouse is hit tested
    // WS_EX_NOACTIVATE   - it never becomes active, so the game keeps focus
    // WS_EX_TOOLWINDOW   - keeps it out of Alt+Tab and Task View
    LONG_PTR ex = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    if (!g_relay_input) { ex |= WS_EX_TRANSPARENT; }   // relay mode must RECEIVE the mouse
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    // No WS_VISIBLE in the style: setting that bit with SetWindowLongPtr can
    // activate the window, which is what stole input from the game.
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP);

    if (!g_overlay_prev_proc) {
        g_overlay_prev_proc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(overlay_proc)));
    }

    // --overlay-hidden: same window, same swap chain, same presents - just never shown.
    // Everything the frame loop measures (arr/out/game histograms, age, colour) still
    // works; what changes is that the game window is the topmost visible thing on its
    // monitor again. That is the whole point of the switch: it isolates "our overlay
    // costs the game its flip layer" from "WGC/composition is lossy anyway".
    if (g_overlay_hidden) {
        SetWindowPos(hwnd, nullptr, x, y, static_cast<int>(w), static_cast<int>(h),
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW);
        ShowWindow(hwnd, SW_HIDE);
        focus_game();
        logf_("[bridge] --overlay-hidden: window stays hidden (presents keep running);"
              " the game window should be the topmost visible one now\n");
        return;
    }

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, static_cast<int>(w), static_cast<int>(h),
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    focus_game();

    // The window exists, is in the topmost band and has its ex-styles: strip it from the
    // taskbar / Alt+Tab / Task View so the user only ever sees one picture (the game's).
    overlay_hide_from_switcher(hwnd);

    RECT client{};
    GetClientRect(hwnd, &client);
    logf_("[bridge] borderless: client %ldx%ld (wanted %ux%u) scroll=%d fg=%d\n",
          client.right - client.left, client.bottom - client.top, w, h,
          (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) ? 1 : 0,
          (GetForegroundWindow() == g_game_wnd) ? 1 : 0);
}

// Hand the foreground back to the game if the overlay grabbed it. Other
// applications are left alone, so alt-tabbing away still works. Throttled: doing
// this on every idle poll caused foreground storms.
static ULONGLONG g_last_focus_try = 0;

static void keep_game_focus()
{
    if (!g_game_wnd) { return; }
    const HWND fg = GetForegroundWindow();
    if (fg != nullptr && fg != overlay_wnd()) { return; }

    const ULONGLONG now = GetTickCount64();
    if (now - g_last_focus_try < 1000) { return; }
    g_last_focus_try = now;
    focus_game();
}

// --------------------------------------------------------------- devices

struct AdapterPick
{
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1    desc{};
    const wchar_t*        why = L"none";
};

static const wchar_t* vendor_name(UINT vendor)
{
    switch (vendor) {
        case 0x1002: return L"AMD";
        case 0x10DE: return L"NVIDIA";
        case 0x8086: return L"Intel";
        case 0x1414: return L"Microsoft";
        default:     return L"unknown";
    }
}

static uint64_t pack_luid(const LUID& luid)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) |
           static_cast<uint32_t>(luid.LowPart);
}

static std::wstring lower_w(std::wstring s)
{
    for (wchar_t& c : s) { c = static_cast<wchar_t>(towlower(c)); }
    return s;
}

static bool all_digits(const std::wstring& s)
{
    if (s.empty()) { return false; }
    for (const wchar_t c : s) { if (c < L'0' || c > L'9') { return false; } }
    return true;
}

// One --gpu token -> "is this adapter it?". Everything is case-insensitive.
//   luid:HI:LO                      exact LUID (hex)
//   nv|nvidia / amd / intel / ms    vendor alias
//   10de / 1002 / 8086 / 0x10de     vendor id (hex)
//   0 / 1                           adapter index (EnumAdapters1 order)
//   6750 / 3080                     description substring
static bool adapter_matches(const DXGI_ADAPTER_DESC1& d, unsigned index, const std::wstring& spec)
{
    const std::wstring s = lower_w(spec);
    if (s.empty()) { return false; }

    if (s.rfind(L"luid:", 0) == 0) {
        unsigned long long hi = 0, lo = 0;
        if (swscanf_s(s.c_str() + 5, L"%llx:%llx", &hi, &lo) == 2) {
            return pack_luid(d.AdapterLuid) == ((hi << 32) | (lo & 0xFFFFFFFFull));
        }
        return false;
    }

    unsigned long vendor = 0;
    if (s == L"nv" || s == L"nvidia")         { vendor = 0x10DE; }
    else if (s == L"amd")                     { vendor = 0x1002; }
    else if (s == L"intel")                   { vendor = 0x8086; }
    else if (s == L"ms" || s == L"microsoft") { vendor = 0x1414; }
    else if (s.rfind(L"0x", 0) == 0)          { vendor = wcstoul(s.c_str() + 2, nullptr, 16); }
    else if (s == L"10de" || s == L"1002" || s == L"8086" || s == L"1414") {
        vendor = wcstoul(s.c_str(), nullptr, 16);
    }
    if (vendor != 0) { return d.VendorId == vendor; }

    if (all_digits(s)) {
        // an index, when it is a plausible one; otherwise fall through so
        // `--gpu 3080` still works as a description substring
        if (wcstoul(s.c_str(), nullptr, 10) == index) { return true; }
        if (s.size() <= 2) { return false; }
    }

    return lower_w(d.Description).find(s) != std::wstring::npos;
}

// Does this adapter drive that monitor? CreateSwapChainForHwnd returns
// E_ACCESSDENIED when it does not, so this is the one hard constraint.
static bool drives_monitor(IDXGIAdapter1* adapter, HMONITOR mon)
{
    if (!adapter || !mon) { return false; }
    for (unsigned j = 0;; ++j) {
        ComPtr<IDXGIOutput> out;
        if (adapter->EnumOutputs(j, &out) == DXGI_ERROR_NOT_FOUND) { break; }
        DXGI_OUTPUT_DESC od{};
        if (FAILED(out->GetDesc(&od))) { continue; }
        if (od.Monitor == mon) { return true; }
    }
    return false;
}

// Picks the GPU that runs XeSS-FG.
//
// Order: --gpu (explicit) > the adapter that drives the game window's monitor
// (the only one our overlay can present on) > "not the producer" > largest VRAM.
// Every adapter is logged, so "why did it pick that one" is answerable from the log
// alone - that was the expensive part of getting the old AMD-only check right.
static AdapterPick find_consumer_adapter(HWND game_wnd)
{
    AdapterPick pick;
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) { return pick; }

    const HMONITOR mon = overlay_monitor(game_wnd);
    logf_("[bridge] enumerating adapters (overlay monitor %p)\n",
          reinterpret_cast<void*>(mon));

    AdapterPick by_gpu, by_monitor, by_other, by_vram;
    for (unsigned i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) { break; }
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) { continue; }
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            logf_("[bridge]   %u: '%ls' is a software adapter - skipped\n", i, d.Description);
            continue;
        }

        unsigned outputs = 0;
        for (;; ++outputs) {
            ComPtr<IDXGIOutput> o;
            if (a->EnumOutputs(outputs, &o) == DXGI_ERROR_NOT_FOUND) { break; }
        }
        const bool producer = g_have_producer && pack_luid(d.AdapterLuid) == g_producer_luid;
        const bool on_mon   = drives_monitor(a.Get(), mon);
        logf_("[bridge]   %u: '%ls' %ls vendor=%04X device=%04X vram=%llu MB outputs=%u"
              " luid=%08lX:%08lX%s%s\n",
              i, d.Description, vendor_name(d.VendorId), d.VendorId, d.DeviceId,
              static_cast<unsigned long long>(d.DedicatedVideoMemory / (1024ull * 1024ull)),
              outputs,
              static_cast<unsigned long>(d.AdapterLuid.HighPart),
              static_cast<unsigned long>(d.AdapterLuid.LowPart),
              on_mon ? "  <== overlay monitor" : "", producer ? "  (producer)" : "");

        if (!g_gpu_spec.empty() && !by_gpu.adapter && adapter_matches(d, i, g_gpu_spec)) {
            by_gpu.adapter = a; by_gpu.desc = d; by_gpu.why = L"--gpu";
        }
        if (!by_monitor.adapter && on_mon) {
            by_monitor.adapter = a; by_monitor.desc = d;
            by_monitor.why = L"drives the overlay monitor";
        }
        if (!by_other.adapter && g_have_producer && !producer) {
            by_other.adapter = a; by_other.desc = d; by_other.why = L"not the producer";
        }
        if (!by_vram.adapter || d.DedicatedVideoMemory > by_vram.desc.DedicatedVideoMemory) {
            by_vram.adapter = a; by_vram.desc = d; by_vram.why = L"largest VRAM";
        }
    }

    if (!g_gpu_spec.empty() && !by_gpu.adapter) {
        logf_("[bridge] WARNING: --gpu %ls matched no adapter - falling back to the"
              " automatic pick\n", g_gpu_spec.c_str());
    }
    if (by_gpu.adapter)          { pick = by_gpu; }
    else if (by_monitor.adapter) { pick = by_monitor; }
    else if (by_other.adapter)   { pick = by_other; }
    else                         { pick = by_vram; }

    if (pick.adapter) {
        logf_("[bridge] consumer adapter: '%ls' (%ls vendor=%04X) - %ls\n",
              pick.desc.Description, vendor_name(pick.desc.VendorId), pick.desc.VendorId,
              pick.why);
        if (mon && !drives_monitor(pick.adapter.Get(), mon)) {
            logf_("[bridge] WARNING: this adapter does not drive the monitor the overlay is"
                  " on; CreateSwapChainForHwnd may fail with E_ACCESSDENIED - move the"
                  " overlay there with --overlay-monitor, or pick another --gpu\n");
        }
        if (g_have_producer && pack_luid(pick.desc.AdapterLuid) == g_producer_luid) {
            logf_("[bridge]   note: same GPU as the producer - frame generation competes"
                  " with the game for GPU time (use --gpu to choose the other card)\n");
        }
    }
    return pick;
}

static bool init_devices(IDXGIAdapter1* adapter)
{
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                 D3D11_SDK_VERSION, &g_d3d11, nullptr, &g_ctx11))) {
        logf_("[bridge] D3D11CreateDevice FAILED\n");
        return false;
    }
    if (FAILED(g_d3d11.As(&g_dev11_5)) || FAILED(g_ctx11.As(&g_ctx11_4))) {
        logf_("[bridge] ID3D11Device5/Context4 unavailable\n");
        return false;
    }
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device)))) {
        logf_("[bridge] D3D12CreateDevice FAILED\n");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue))) ||
        FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&g_alloc))) ||
        FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           g_alloc.Get(), nullptr, IID_PPV_ARGS(&g_list))) ||
        FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_frame_fence)))) {
        logf_("[bridge] D3D12 queue/list/fence FAILED\n");
        return false;
    }
    g_list->Close();
    g_frame_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (FAILED(g_dev11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_fence11)))) {
        logf_("[bridge] ID3D11Fence FAILED\n");
        return false;
    }
    HANDLE fh = nullptr;
    if (FAILED(g_fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh))) {
        logf_("[bridge] fence CreateSharedHandle FAILED\n");
        return false;
    }
    const HRESULT hr = g_device->OpenSharedHandle(fh, IID_PPV_ARGS(&g_fence12));
    CloseHandle(fh);
    if (FAILED(hr)) { logf_("[bridge] fence OpenSharedHandle FAILED\n"); return false; }

    logf_("[bridge] devices ready (D3D11 + D3D12 on '%ls', %ls)\n",
          g_consumer_desc.Description, vendor_name(g_consumer_vendor));
    return true;
}

// ------------------------------------------------------------------- WGC

static ComPtr<IDirect3D11CaptureFramePool> g_pool;
static ComPtr<IGraphicsCaptureSession>     g_session;
static ComPtr<IGraphicsCaptureItem>        g_capture_item;
static SizeInt32 g_item_size{};
static SizeInt32 g_pool_size{};

// The add-on's shared block still advertises a colour stream at a fixed 5120x2880 (and
// with `Color=0` does not even allocate it), so that size is a placeholder, not the game's
// resolution. What WGC hands us is the real window content size, and everything built from
// it - colour sink, frame pool, FG swap chain, the resize ratio - has to agree with it,
// otherwise a 4K window gets a 5K picture stretched over it.
static void adopt_capture_size()
{
    if (g_item_size.Width <= 0 || g_item_size.Height <= 0) { return; }
    const UINT w = static_cast<UINT>(g_item_size.Width);
    const UINT h = static_cast<UINT>(g_item_size.Height);
    if (w == g_width && h == g_height) { return; }
    logf_("[bridge] capture size taken from the window: %ux%u (shared block declared %ux%u)\n",
          w, h, g_width, g_height);
    g_width  = w;
    g_height = h;
    g_full_w = w;
    g_full_h = h;
}

static bool create_capture_item(HWND game_wnd)
{
    ComPtr<IGraphicsCaptureItemInterop> interop;
    {
        const wchar_t* kClass = L"Windows.Graphics.Capture.GraphicsCaptureItem";
        HSTRING hs = nullptr;
        WindowsCreateString(kClass, static_cast<UINT32>(wcslen(kClass)), &hs);
        const HRESULT h = RoGetActivationFactory(hs, IID_PPV_ARGS(&interop));
        WindowsDeleteString(hs);
        if (FAILED(h)) { logf_("[bridge] capture item activation FAILED\n"); return false; }
    }

    // --capture-monitor: capture the display instead of the window. What the panel shows is
    // what we get, so any window/swap-chain height mismatch (which showed up as a one-line
    // shift at the bottom edge) cannot happen.
    if (g_capture_monitor) {
        const HMONITOR mon = MonitorFromWindow(game_wnd, MONITOR_DEFAULTTOPRIMARY);
        if (mon && SUCCEEDED(interop->CreateForMonitor(mon, __uuidof(IGraphicsCaptureItem),
                                                      reinterpret_cast<void**>(
                                                          g_capture_item.GetAddressOf())))) {
            g_capture_item->get_Size(&g_item_size);
            logf_("[bridge] capture item (monitor) %dx%d\n", g_item_size.Width, g_item_size.Height);
            return true;
        }
        logf_("[bridge] CreateForMonitor FAILED - falling back to the window\n");
    }

    if (FAILED(interop->CreateForWindow(game_wnd, __uuidof(IGraphicsCaptureItem),
                                        reinterpret_cast<void**>(g_capture_item.GetAddressOf())))) {
        logf_("[bridge] CreateForWindow FAILED (exclusive fullscreen?)\n");
        return false;
    }
    g_capture_item->get_Size(&g_item_size);
    logf_("[bridge] capture item %dx%d\n", g_item_size.Width, g_item_size.Height);
    return true;
}

static bool start_wgc()
{
    // Pool at the captured size: asking WGC for a smaller pool crops instead of scaling,
    // so the resize happens in our own pass, where we control the filter.
    g_pool_size.Width  = static_cast<INT32>(g_width);
    g_pool_size.Height = static_cast<INT32>(g_height);
    logf_("[bridge] capture pool %dx%d\n", g_pool_size.Width, g_pool_size.Height);

    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(g_d3d11.As(&dxgi_device))) { return false; }
    ComPtr<IInspectable> inspectable;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), &inspectable))) {
        logf_("[bridge] CreateDirect3D11DeviceFromDXGIDevice FAILED\n");
        return false;
    }
    ComPtr<IDirect3DDevice> rt_device;
    if (FAILED(inspectable.As(&rt_device))) { return false; }

    ComPtr<IDirect3D11CaptureFramePoolStatics2> statics;
    {
        const wchar_t* kClass = L"Windows.Graphics.Capture.Direct3D11CaptureFramePool";
        HSTRING hs = nullptr;
        WindowsCreateString(kClass, static_cast<UINT32>(wcslen(kClass)), &hs);
        const HRESULT h = RoGetActivationFactory(hs, IID_PPV_ARGS(&statics));
        WindowsDeleteString(hs);
        if (FAILED(h)) { logf_("[bridge] frame pool activation FAILED\n"); return false; }
    }

    if (FAILED(statics->CreateFreeThreaded(
            rt_device.Get(),
            ABI::Windows::Graphics::DirectX::DirectXPixelFormat_R16G16B16A16Float,
            g_wgc_pool, g_pool_size, &g_pool))) {
        logf_("[bridge] CreateFreeThreaded FAILED\n");
        return false;
    }

    if (FAILED(g_pool->CreateCaptureSession(g_capture_item.Get(), &g_session))) {
        logf_("[bridge] CreateCaptureSession FAILED\n");
        return false;
    }
    ComPtr<IGraphicsCaptureSession2> session2;
    if (SUCCEEDED(g_session.As(&session2))) { session2->put_IsCursorCaptureEnabled(FALSE); }
    if (FAILED(g_session->StartCapture())) {
        logf_("[bridge] StartCapture FAILED\n");
        return false;
    }
    logf_("[bridge] WGC running (FP16 scRGB, free-threaded)\n");
    return true;
}

// --------------------------------------------------------------- XeSS-FG

// --dyn-mfg needs to be visible to fg_apply_interp() below (it decides the start count).
static bool g_dyn_mfg = false;

// Runtime count changes go through xefgSwapChainSetNumInterpolatedFrames. Only a
// DLL that exports it can change the count after initialization; the official
// build already fixed the count with the init parameter, and on a non-Intel
// adapter it clamps that to 1 anyway.
//
// The XeSS-FG guide warns that the call "may be expensive (XeSS-FG may switch to a
// different model and trigger shader compilation)" and recommends it only as a
// response to a settings change. --interp-auto therefore changes it at most once
// per two report windows (~2.7 s) and only when the target really moved.
static void fg_set_interp(UINT want, const char* when, bool quiet = false)
{
    if (want < 1) { want = 1; }
    if (want > g_fg_interp_max) { want = g_fg_interp_max; }
    if (want == g_fg_interp_actual) { return; }

    if (!g_api.has_mfg()) {
        logf_("[bridge] cannot apply interp=%u %s: this DLL has no"
              " xefgSwapChainSetNumInterpolatedFrames\n", want, when);
        return;
    }

    const xefg_swapchain_result_t r = g_api.SetNumInterpolatedFrames(g_fg, want);
    if (!quiet) {
        logf_("[bridge] SetNumInterpolatedFrames(%u) %s: %s (%d)\n",
              want, when, xefg_str(r), static_cast<int>(r));
    }
    if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS) { g_fg_interp_actual = want; }
}

static void fg_apply_interp(const char* when)
{
    UINT want = (g_fg_interp > 0) ? static_cast<UINT>(g_fg_interp) : g_fg_interp_actual;
    // --dyn-mfg: --interp is the *ceiling* (the unlock patches it in), the start is 4x.
    // Without this the "after SetEnabled" call below overwrote the start with the
    // ceiling and the first run began at 6x (bridge_pbz_dynmfg.txt: gpu 47 ms, skip 67).
    if (g_dyn_mfg) { want = (g_fg_interp_max >= 3) ? 3u : g_fg_interp_max; }
    fg_set_interp(want, when);
}

// ------------------------------------------------------------ --list-modes
// Without VRR the picture can only be perfectly even when the *input* rate is
// locked to an exact divisor of the panel refresh:
//     output fps = input fps * (interp + 1)  must divide the refresh rate
// so 120 Hz accepts 120 / 60 / 40 / 30 / 24 / 20 ... fps output. This prints the
// refresh rates the current output really offers, plus the exact input rate for
// each multiplier, so the right lock can be picked without guessing.
static int list_display_modes()
{
    DEVMODEW cur{};
    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur)) {
        logf_("[bridge] EnumDisplaySettingsW(current) failed\n");
        return 1;
    }
    logf_("[bridge] current mode: %lux%lu @ %lu Hz\n",
          static_cast<unsigned long>(cur.dmPelsWidth),
          static_cast<unsigned long>(cur.dmPelsHeight),
          static_cast<unsigned long>(cur.dmDisplayFrequency));

    UINT hz[64] = {};
    int n = 0;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i) {
        if (dm.dmPelsWidth != cur.dmPelsWidth || dm.dmPelsHeight != cur.dmPelsHeight) { continue; }
        bool dup = false;
        for (int k = 0; k < n; ++k) { if (hz[k] == dm.dmDisplayFrequency) { dup = true; break; } }
        if (!dup && n < 64) { hz[n++] = dm.dmDisplayFrequency; }
    }
    if (n == 0) { logf_("[bridge] no modes enumerated for this resolution\n"); return 1; }

    logf_("[bridge] refresh rates at %lux%lu:",
          static_cast<unsigned long>(cur.dmPelsWidth),
          static_cast<unsigned long>(cur.dmPelsHeight));
    for (int k = 0; k < n; ++k) { logf_(" %u", hz[k]); }
    logf_("\n");

    // Exact input rates per output multiplier (output = input * mul must divide Hz).
    for (int k = 0; k < n; ++k) {
        const double r = static_cast<double>(hz[k]);
        logf_("[bridge]   %3u Hz -> exact input for", hz[k]);
        for (int mul = 1; mul <= 6; ++mul) {
            logf_("  %dx@%.2f", mul, r / mul);
        }
        logf_("  (fps)\n");
    }
    logf_("[bridge] pick the highest exact input rate the game can always hold, lock RTSS"
          " to it, and set --interp so that input*(interp+1) == refresh.\n");
    return 0;
}

// The cadence helpers (median of the input intervals, refresh period, double-abs)
// are defined below; the solver needs them before their definitions.
static double arr_median_ms();
static double refresh_ms();
static double dabs_(double v);

// --lock N: enforce an input rate by dropping captured frames (see the frame loop).
// --lock-auto: solve the pair (rate, multiplier) from the refresh and the measured
// input capability, then enforce both. This is the general form of "the input must
// be an exact divisor of the refresh": for 120 Hz the exact options are
//   120/2 = 60 fps (2x), 120/3 = 40 (3x), 120/4 = 30 (4x), 120/5 = 24 (5x), 120/6 = 20 (6x)
// and the solver takes the highest one the game can actually sustain, preferring the
// shortest output period (1 vblank) and then the fewest generated frames.
static double    g_lock_fps = 0.0;
static bool      g_lock_auto = false;
static ULONGLONG g_lock_next = 0;
static UINT      g_lock_drops = 0;
static int       g_lock_pending_m = 0;
static double    g_lock_pending_fps = 0.0;
static UINT      g_lock_hits = 0;
static bool      g_lock_none_logged = false;

static void fg_solve_cadence(const char* why)
{
    if (!g_lock_auto || g_refresh_hz < 1) { return; }
    const double cap = 1000.0 / arr_median_ms();          // input fps the game sustains
    if (cap < 1.0) { return; }
    const int mmax = static_cast<int>(g_fg_interp_max) + 1;

    int best_m = 0, best_vb = 0;
    double best_lock = 0.0;
    for (int vb = 1; vb <= 4 && best_m == 0; ++vb) {
        for (int m = 2; m <= mmax; ++m) {
            const double lock = static_cast<double>(g_refresh_hz) / (static_cast<double>(m) * vb);
            if (lock > cap * 0.97) { continue; }          // the game cannot hold this
            // We can only *drop* captured frames, never hold or re-time them, so a
            // dropped frame turns one interval of T into two intervals of 2T. That is
            // only invisible when T is an exact multiple of the lock period, i.e.
            // when cap / lock is a whole number (dropping 1 of every 2 / 3 / 4 ...).
            // Anything else (26 -> 24 fps = drop 1 in 13) would show up as a 2T gap
            // every 13th frame - worse than the judder we are trying to remove.
            const double ratio = cap / lock;
            const double rfrac = dabs_(ratio - static_cast<double>(static_cast<int>(ratio + 0.5)));
            if (rfrac > 0.12) { continue; }
            best_m = m;
            best_vb = vb;
            best_lock = lock;
            break;                                        // fewest frames for this vb
        }
    }
    if (best_m == 0) {
        if (!g_lock_none_logged) {
            g_lock_none_logged = true;
            logf_("[bridge] --lock-auto: input %.1f fps at %u Hz has no exact cadence with"
                  " up to %dx - falling back to the unlocked rate\n", cap, g_refresh_hz, mmax);
        }
        return;
    }

    const int want_m = best_m;
    const bool same = (want_m == g_lock_pending_m);
    g_lock_pending_m = want_m;
    g_lock_pending_fps = best_lock;
    if (same) { if (++g_lock_hits < 2) { return; } } else { g_lock_hits = 1; return; }

    // Stable for two windows: apply. The lock takes effect immediately; the count
    // change costs one model switch (documented as expensive in the XeSS-FG guide).
    g_lock_hits = 0;
    if (dabs_(g_lock_fps - best_lock) > 0.05) {
        g_lock_fps = best_lock;
        g_lock_next = 0;                                  // restart the schedule
        logf_("[bridge] --lock-auto: input %.1f fps -> lock %.2f fps + %dx = %.1f fps"
              " (%.2f vblank/frame, %s)\n",
              cap, best_lock, want_m,
              best_lock * want_m,
              (best_lock * want_m > 0.0)
                  ? (1000.0 / (best_lock * want_m)) / refresh_ms() : 0.0,
              why);
    }
    fg_set_interp(static_cast<UINT>(want_m - 1), "lock-auto (exact cadence)");
}

// ------------------------------------------------- cadence (smoothness) helpers
//
// Why this exists: XeSS-FG paces generated frames into the panel's vblanks, so the
// output is only *evenly* spaced when the output interval T_in/(N+1) is a whole
// number of vblanks. 4x of a locked 30 fps on a 120 Hz panel is exactly 8.33 ms =
// one vblank (smooth). A *dynamic* 38 fps gives 6.6 ms = 1.26 vblanks, and the
// pacer can only alternate 1,1,1,2 - no stutter, but visibly uneven. That is the
// "dynamic frame time looks unsmooth" symptom, and it is arithmetic, not MV/depth.
//
// Two knobs, both off by default (docs/findings/FINAL_CONFIG.md 4.2):
//   --fg-rt-mode ema|raw|median|quant   what we hand XeFG as frameRenderTime
//   --interp-auto [floor|round]         keep the output on a whole-vblank cadence
enum { kFgRtEma = 0, kFgRtRaw = 1, kFgRtMedian = 2, kFgRtQuant = 3, kFgRtAuto = 4 };
static int g_fg_rt_mode = kFgRtQuant;   // quant: median snapped to whole vblanks (see fg_render_time_ms)
// --fg-rt-mode auto: pick quant or raw from whether the *input rate* is moving.
// Two EMAs of the interval, one fast, one slow; when they disagree by more than a quarter
// of a vblank the rate is changing (unlocked / GPU-bound) and raw tracks it better, because
// the median lags a step by a couple of windows. A locked rate keeps them together even
// though individual frames jitter by +-1 vblank, which is exactly the distinction we want
// (a locked 40 fps also arrives as 2/3/4 vblanks and belongs to quant).
static double g_rt_ema_fast = 0.0;
static double g_rt_ema_slow = 0.0;
static int    g_rt_auto_state = 0;      // 0 = rate is steady (quant), 1 = rate is moving (raw)
static int    g_rt_auto_agree = 0;      // consecutive frames agreeing to flip (hysteresis)
static UINT   g_rt_auto_flips = 0;
// --fg-out-hz N: cap the output rate (0 = the panel refresh, i.e. fill every vblank).
// Only meaningful together with --interp-cadence, where the per-frame frame count is ours
// to choose: "120 Hz panel but I only want 60 fps" then comes out as 2x for a 30 fps input
// instead of 4x. The floor is 2x (libxess_fg cannot generate zero frames); if the target
// needs fewer, bridge warns once and says what rate it will actually run at.
static double g_fg_out_hz = 0.0;
// --fg-cad-rt 0|1 (default 1): in --interp-cadence mode derive frameRenderTime from the
// batches themselves (k * vblank) instead of from the median interval.
//
// This is the fix for "the picture judders while the input rate fluctuates": cadence asks
// XeFG for k frames because k vblanks actually elapsed, but the pacer was still being told
// the *median* length. Whenever the two disagree (a 33.3 ms gap while the median is 25 ms)
// the pacer budgets the wrong time for the batch and has to hold or rush a frame, which is
// visible. Feeding k * vblank makes the two agree by construction - it is exactly the
// interval those k frames have to cover.
static UINT   g_cad_last_k = 0;
// Measured in a heavily fluctuating scene (2..6 vblanks between inputs): feeding k*vblank
// raised the empty-refresh rate from 1.0% to 2.6% and produced one 37-vblank hole, while the
// median feed stayed at 1.0%. So this is OFF by default and exists only for A/B.
static bool   g_fg_cad_rt = false;
static bool   g_cad_rt_logged = false;
// --cadence-hold N (default 15 frames, 0/1 = off): how long to keep the interpolated-frame
// count before the cadence logic is allowed to change it again.
//
// Why: changing that count is expensive (the guide says it may reload the model / recompile
// shaders), and with a fluctuating input the phase accumulator asks for a different count
// almost every frame - measured 24-32 switches per 1 s window (40 fps input), which is
// exactly the "stutters even though no vblank is empty" the ear sees. Holding the count for
// ~15 frames and then stepping by at most one keeps the long-run average (the phase keeps
// accumulating) while making the picture's rhythm steady.
static int    g_cad_hold_frames = 15;
static int    g_cad_hold = 0;
static bool   g_out_hz_floor_warned = false;
static bool   g_out_hz_max_warned = false;
// A longer ring for the "slow" rate. Per-sample EMAs could not tell a locked 40 fps (which
// arrives as 2/3/4 vblanks) from a rate that is actually moving: the +-1 vblank jitter of a
// locked input drives a fast EMA just as much as a real step does - measured, it flipped a
// locked 40 fps to "raw". Medians are immune to that jitter (it is exactly why quant uses
// one), so the decision compares a short median against a long one.
#define kArrSlowRing 64
static double g_arr_slow_ring[kArrSlowRing] = {};
static UINT   g_arr_slow_n = 0;
static UINT   g_arr_slow_i = 0;

static double arr_slow_median_ms()
{
    if (g_arr_slow_n == 0) { return 0.0; }
    static double v[kArrSlowRing];
    const UINT n = g_arr_slow_n;
    for (UINT i = 0; i < n; ++i) { v[i] = g_arr_slow_ring[i]; }
    for (UINT i = 1; i < n; ++i) {
        const double key = v[i];
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; --j; }
        v[j + 1] = key;
    }
    return v[n / 2];
}

// --fg-rt-spike 0/1 (default 1): when ONE input frame arrives much later than the plan
// (a fast camera turn, a streamed asset), report the real length to XeFG for that frame so
// the pacer spreads its batch over the gap instead of laying it out too short and leaving an
// empty refresh period. This is the difference between "the game dipped to 57 fps" and "the
// picture juddered". See fg_render_time_ms().
static bool   g_fg_rt_spike = true;
static const double kSpikeRatio = 1.35;   // last interval this much longer than the plan
static int    g_spike_logs_left = 8;      // log the first few, then stay quiet
static UINT   g_spike_count = 0;          // spikes this report window
// How often we processed two captured frames closer together than 0.6x the input interval.
// That happens after a stall: WGC has several frames queued and the loop drains them back to
// back (each one with its own Present). It is what made `show` read 148 fps on a 120 Hz panel,
// so it is worth counting explicitly instead of guessing.
static UINT   g_burst_count = 0;

static const char* fg_rt_mode_name()
{
    switch (g_fg_rt_mode) {
    case kFgRtRaw:    return "raw";
    case kFgRtMedian: return "median";
    case kFgRtAuto:   return (g_rt_auto_state == 1) ? "auto:raw" : "auto:quant";
    case kFgRtQuant:  return "quant";
    default:          return "ema";
    }
}
// Last input intervals, for a median that is robust against WGC's own +-1 vblank
// quantization (a game at 35 fps on 120 Hz reaches us as 25/33/25 ms, not 28.6).
#define kArrRing 16
static double g_arr_ring[kArrRing] = {};
static UINT   g_arr_ring_n = 0;
static UINT   g_arr_ring_i = 0;
static double g_arr_last_raw = 0.0;

// Interval between WGC frame arrivals (i.e. the game's own frame time), EMA'd.
// Declared here because the cadence helpers need it; the frame-loop section below
// still owns updating it (note_arrival).
static double g_arrival_ms_ema = 26.0;

static void arr_push(double dt_ms)
{
    if (!(dt_ms > 0.0) || dt_ms > 1000.0) { return; }
    g_arr_ring[g_arr_ring_i] = dt_ms;
    g_arr_ring_i = (g_arr_ring_i + 1) % kArrRing;
    if (g_arr_ring_n < kArrRing) { ++g_arr_ring_n; }
    g_arr_last_raw = dt_ms;
}

// ============ --interp-cadence: variable-rate (per-frame) generation ========
//
// The problem this solves: a *constant* multiplier only lands on whole vblanks when
// the input rate divides the refresh exactly, so with UE5's wandering frame time the
// choice is either "lock the game (throws GPU headroom away)" or "accept judder".
//
// The multiplier does not have to be constant. The pacer shows at most one frame per
// vblank, so if the number of vblanks between two real frames is k, asking XeFG for
// k-1 generated frames makes every output frame land on its own vblank - whatever the
// input rate is. The output then runs at the refresh rate and the input is free to
// wander. A phase accumulator (Bresenham) keeps the long-run average exact:
//
//     phase += T_in / vblank
//     k     = round(phase)                 frames to output for this real frame
//     phase -= k                           carry the remainder into the next frame
//     interp = clamp(k - 1, 1, max)
//
// So 26 fps on 120 Hz alternates 5x/5x/4x/5x... (avg 4.615 frames per input frame =
// 120 fps), 35 fps alternates 4x/3x, and nothing is locked: 20..60 fps input all
// land on exactly one vblank per output frame.
//
// Cost to watch (this is why the counter exists): the XeSS-FG guide says changing the
// count "may be expensive (may switch to a different model and trigger shader
// compilation)". In this build every generated frame uses the 2x model, so a count
// change may only change the number of dispatches - but that is an assumption, and
// `switch_fail`/`gpu`/`HITCH` in the log is how we find out. If it turns out to be
// expensive, use --interp-auto instead (it changes the count at most every 2 windows).
static bool   g_interp_cadence = false;
static double g_cad_phase = 0.0;
static UINT   g_cad_hist[8] = {};        // how often each count was asked for
static UINT   g_cad_switches = 0;        // count changes actually issued
static UINT   g_cad_fail = 0;            // ... that the DLL refused
// (g_dyn_mfg is declared above, next to fg_apply_interp.)

// Phase-accumulator step; returns the count to use for this real frame.
static UINT cadence_next_slots()
{
    const double vb = refresh_ms();
    // Track the raw last interval (reacts immediately) but fall back to the EMA while
    // it warms up; a median would lag behind the very transitions we are following.
    const double t_in = (g_arr_last_raw > 0.5) ? g_arr_last_raw : g_arrival_ms_ema;
    // Target output rate. Without --fg-out-hz this is the panel refresh, i.e. "fill every
    // vblank" (the historical behaviour). With it the same Bresenham accumulator produces
    // as many frames as the *target* rate needs for this input interval, so "120 Hz panel,
    // but I only want 60 fps" becomes 2x instead of 4x for a 30 fps input.
    const double panel_hz = (vb > 0.5) ? 1000.0 / vb : 120.0;
    const double out_hz = (g_fg_out_hz > 1.0) ? g_fg_out_hz : panel_hz;
    g_cad_phase += t_in * out_hz / 1000.0;
    int k = static_cast<int>(g_cad_phase + 0.5);
    if (k < 2) {
        // libxess_fg cannot generate zero frames (that is --no-fg), so the floor is 2x.
        // Warn once: the target rate is unreachable and the output will run faster.
        if (!g_out_hz_floor_warned) {
            g_out_hz_floor_warned = true;
            logf_("[bridge] --fg-out-hz %.0f: a %.1f ms input needs %u generated frame(s),"
                  " but 2x is the minimum here - the output will run at ~%.0f Hz instead."
                  " Use --no-fg for 1:1.\n",
                  out_hz, t_in, (k < 1) ? 1u : static_cast<UINT>(k), 2000.0 / t_in);
        }
        k = 2;
    }
    const int kmax = static_cast<int>(g_fg_interp_max) + 1;
    const bool clamped = (k > kmax);
    if (k > kmax) {
        k = kmax;
        if (!g_out_hz_max_warned && g_fg_out_hz > 1.0) {
            g_out_hz_max_warned = true;
            logf_("[bridge] --fg-out-hz %.0f needs %d generated frames for a %.1f ms input,"
                  " but this DLL allows %d (--interp %d) - raise --interp.\n",
                  out_hz, k, t_in, g_fg_interp_max, g_fg_interp_max);
        }
    }
    g_cad_phase -= static_cast<double>(k);
    if (clamped) { g_cad_phase = 0.0; }              // do not accumulate a debt
    if (g_cad_phase < -1.5) { g_cad_phase = -1.5; }
    if (g_cad_phase > 1.5) { g_cad_phase = 1.5; }
    g_cad_last_k = static_cast<UINT>(k);      // frameRenderTime follows this (see fg_render_time_ms)
    return static_cast<UINT>(k);
}

static double arr_median_ms()
{
    if (g_arr_ring_n == 0) { return g_arrival_ms_ema; }
    double v[kArrRing];
    const UINT n = g_arr_ring_n;
    for (UINT i = 0; i < n; ++i) { v[i] = g_arr_ring[i]; }
    for (UINT i = 1; i < n; ++i) {              // n <= 16: insertion sort is fine
        const double k = v[i];
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; --j; }
        v[j + 1] = k;
    }
    return v[n / 2];
}

static double refresh_ms()
{
    return (g_refresh_hz > 0) ? 1000.0 / static_cast<double>(g_refresh_hz) : 1000.0 / 120.0;
}

// What goes into xefg_swapchain_frame_constant_data_t::frameRenderTime. The guide:
// "On non-Intel GPUs, XeSS-FG can use frameRenderTime to sanity-check presentation
// pacing" - this is the one number that tells the pacer what we think the input
// cadence is, so an EMA with a ~330 ms tail (alpha 0.1 at 30 fps) is exactly wrong
// when the frame time moves. OptiScaler feeds the game's raw value and only
// auto-detects seconds vs milliseconds.
// --input-ms X: pretend the input interval is X ms instead of the measured arrival gap.
// Two uses: (a) a game whose frame pacing is erratic (22-28 ms jitter) can be treated as a
// steady 25 ms; (b) it decides when `slow`/`load` complain and what frameRenderTime XeFG
// sees. It does NOT create GPU time - if the work genuinely needs 27 ms in a 25 ms slot the
// frames still arrive late, they are just no longer reported as "slow".
static double g_input_ms_override = 0.0;

static double input_interval_ms()
{
    return (g_input_ms_override > 1.0) ? g_input_ms_override : g_arrival_ms_ema;
}

// Raw: the interval the producer actually delivered. Most responsive, and the noisiest -
// it feeds one sample of jitter straight into the pacer's plan.
static double rt_raw_value()
{
    return (g_arr_last_raw > 0.0) ? g_arr_last_raw : g_arrival_ms_ema;
}

// Quant: 16-sample median snapped to whole vblanks (plus the single-frame spike handling
// below). This is the cadence the pacer can actually lay out, so its sanity check stops
// fighting the real pacing.
static double rt_quant_value()
{
    const double vb = refresh_ms();
    int k = static_cast<int>(arr_median_ms() / vb + 0.5);
    if (k < 1) { k = 1; }
    const double plan = static_cast<double>(k) * vb;

    // ---- single-frame spikes -------------------------------------------------
    // A fast camera turn (or a streamed asset) makes the game miss its slot once: the
    // input arrives 2 vblanks late instead of 1. Telling XeFG the *plan* length for that
    // frame is what turns one late input into an empty refresh period - it lays its batch
    // out as if it had `plan` ms and the rest of the gap shows nothing (measured:
    // `out: vblanks > presents`, the picture hitches).
    // Feeding the real, vblank-snapped length instead lets the same batch be spread over
    // the longer gap: motion is a touch slower for that one interval, but there is no
    // hitch. That is the difference between "the game dipped to 57 fps" and "the picture
    // juddered".
    if (g_fg_rt_spike && g_arr_last_raw > plan * kSpikeRatio &&
        g_arr_last_raw < 100.0) {
        int kr = static_cast<int>(g_arr_last_raw / vb + 0.5);
        if (kr < k) { kr = k; }
        const int kcap = k * 3;              // never chase a real hitch (>3x the plan)
        if (kr > kcap) { kr = kcap; }
        if (kr > k) {
            const double rt = static_cast<double>(kr) * vb;
            if (g_spike_logs_left > 0) {
                --g_spike_logs_left;
                logf_("[bridge] rt spike: last input %.1fms (%.1f vblanks) > plan %.1fms"
                      " - frameRenderTime %.1fms for this frame (%.1f vblanks)%s\n",
                      g_arr_last_raw, g_arr_last_raw / vb, plan, rt, rt / vb,
                      (g_spike_logs_left == 0) ? " (further spikes not logged)" : "");
            }
            ++g_spike_count;
            return rt;
        }
    }
    return plan;
}

static double fg_render_time_ms()
{
    if (g_fg_rt_zero) { return 0.0; }
    // In cadence mode the batch size and the length it covers have to agree, otherwise the
    // pacer fights the batch and the picture judders whenever the input rate moves.
    if (g_interp_cadence && g_fg_cad_rt && g_cad_last_k >= 2) {
        const double rt = static_cast<double>(g_cad_last_k) * refresh_ms();
        if (!g_cad_rt_logged) {
            g_cad_rt_logged = true;
            logf_("[bridge] cadence: frameRenderTime = batch * vblank (%.1fms for %u frames);"
                  " --fg-cad-rt 0 restores the median feed\n", rt, g_cad_last_k);
        }
        return rt;
    }
    switch (g_fg_rt_mode) {
    case kFgRtRaw:
        return rt_raw_value();
    case kFgRtMedian:
        return arr_median_ms();
    case kFgRtAuto:
        // Measured both ends (PERF_BASELINE 3.19): with --interp-cadence, "raw" wins when the
        // input rate is actually moving (0.7% empty vs 1.0%), while "quant" wins - or ties -
        // when the rate is held by a limiter, because then the per-frame jitter is pure noise.
        // Decide from the *rate*, not from the per-frame spread: a locked 40 fps also arrives
        // as 2/3/4 vblanks, and that case belongs to quant.
        return (g_rt_auto_state == 1) ? rt_raw_value() : rt_quant_value();
    case kFgRtQuant:
    default:
        return rt_quant_value();
    }
}

// --interp-auto: choose N so that (N+1) output frames per input frame land on whole
// vblanks, i.e. N + 1 ~= T_in / vblank.
//
// "round" (default) sits closest to a whole vblank count; "floor" never asks for
// more frames than the panel can show in one input interval. Measured 2026-11-07
// (PBZ, 120 Hz, input 26/32/47 ms): floor dropped to 3x at 32 ms (3.84 vblanks,
// 93.8 fps = 1.28 vblank/frame) while round kept 4x (125 fps, 4% over-subscribed
// but only ~0.16 vblanks of error) - the round answer was measurably smoother and
// stopped the 3x/4x churn. Keep floor only for A/B.
static bool g_interp_auto = false;
static bool g_interp_auto_round = true;
static int  g_auto_pending = -1;
static UINT g_auto_hits = 0;
static bool g_auto_capped_logged = false;

static double dabs_(double v) { return (v < 0.0) ? -v : v; }

// ================= --dyn-mfg: OptiScaler 的 Dynamic MFG（完整控制器）===========
//
// Uses the verbatim ported state machine in src/xefg_dynamic_count.h (819 lines, from
// OptiScaler, MIT). Unlike my earlier hand-written version - which computed
// floor(target / input fps), oscillated 2x<->3x every 2 s on this game and could not
// recover from a 6x start (docs/findings/FINAL_CONFIG.md 9.17/9.18) - this one:
//   * keeps the presented rate nearest the target from the measured frame time and a
//     LEARNED cost of one generated frame, instead of the (poisoned) input fps;
//   * holds every step (0.3 s up, 1 s down) and verifies the 1.5 s after a change,
//     undoing a step that did not pay off;
//   * doubles its back-off while a step stays blocked, and requires every generated
//     frame to earn 30% of the rate a free frame would add;
//   * supports 1X (generation off) as a normal state.
//
// Called once per REAL frame (its holds count frames, not report windows), before the
// resources are tagged. now_ms/wait_ms come from the caller, where both are in scope.
static double g_dyn_target = 0.0;        // --fg-target N (0 = refresh - 4)
static int    g_dyn_rounding = 1;        // --fg-round 0 at most / 1 nearest / 2 at least
static bool   g_dyn_allow1x = true;      // --fg-allow-1x off => 2x is the floor
static bool   g_dyn_fg_off = false;      // is FG currently switched off by us?
static XeFG_DynamicCount g_dyn_ctrl;     // the ported controller
static double g_dyn_log_ms = 0.0;        // last state log

static double dyn_target_fps()
{
    if (g_dyn_target > 0.0) { return g_dyn_target; }
    const double r = static_cast<double>(g_refresh_hz);
    if (r < 1.0) { return 0.0; }
    return (r > 60.0) ? (r - 4.0) : r;    // 116 at 120 Hz, as OptiScaler defaults to
}

// count = interpolated frames (1 = 2x ... 5 = 6x); 0 = 1X, generation off.
static void fg_dyn_mfg_apply(int count)
{
    if (count <= 0) {
        if (!g_dyn_fg_off) {
            const xefg_swapchain_result_t r = g_api.SetEnabled(g_fg, 0);
            logf_("[bridge] dyn-mfg: 1X (frame generation off): %s (%d)\n", xefg_str(r), (int)r);
            if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS) { g_dyn_fg_off = true; }
        }
        return;
    }
    if (g_dyn_fg_off) {
        const xefg_swapchain_result_t r = g_api.SetEnabled(g_fg, 1);
        logf_("[bridge] dyn-mfg: back on: %s (%d)\n", xefg_str(r), (int)r);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { return; }
        g_dyn_fg_off = false;
    }
    fg_set_interp(static_cast<UINT>(count), "dyn-mfg");
}

static void fg_dyn_mfg_frame(double now_ms, double wait_ms)
{
    if (!g_dyn_mfg || g_refresh_hz < 1) { return; }
    const double target = dyn_target_fps();
    if (target <= 0.0) { return; }

    XeFG_DynamicCount::Sample s{};
    s.nowMs = now_ms;
    // The game's own frame interval: the raw last arrival (the controller keeps its own
    // EMA and rejects anything above 250 ms as a hitch).
    s.frameMs = (g_arr_last_raw > 0.5) ? g_arr_last_raw : g_arrival_ms_ema;
    // The part of the interval spent waiting inside the present path.
    s.waitMs = wait_ms;
    s.targetFps = target;
    s.refreshFps = static_cast<double>(g_refresh_hz);
    s.limitFps = 0.0;                       // no in-process limiter
    s.maxCount = static_cast<int>(g_fg_interp_max);
    s.currentCount = g_dyn_fg_off ? 0 : static_cast<int>(g_fg_interp_actual);
    s.rounding = g_dyn_rounding;
    s.minCount = g_dyn_allow1x ? 0 : 1;

    const int want = g_dyn_ctrl.Update(s);
    const int have = s.currentCount;
    if (want == have) {
        // A state line every 5 s, so a log always shows what the controller thinks.
        if (now_ms - g_dyn_log_ms >= 5000.0) {
            g_dyn_log_ms = now_ms;
            logf_("[bridge] dyn-mfg: %dx held: presented %.1f fps, target %.1f, lower %.1f,"
                  " wait %.0f%%, frame cost %.2f ms, back-off %.0f s, %s\n",
                  have + 1, g_dyn_ctrl.PresentedFps(), g_dyn_ctrl.TargetFps(),
                  g_dyn_ctrl.LowerFps(), g_dyn_ctrl.WaitShare() * 100.0,
                  g_dyn_ctrl.FrameCostMs(), g_dyn_ctrl.BackoffMs() / 1000.0,
                  g_dyn_ctrl.Uncapped() ? "uncapped" : "capped");
        }
        return;
    }

    static const char* reasons[] = { "", "up", "down", "try down", "undo", "no gain, back" };
    const auto reason = g_dyn_ctrl.LastReason();
    const char* why = (reason != XeFG_DynamicCount::Reason::None)
                    ? reasons[static_cast<int>(reason)] : "";
    logf_("[bridge] dyn-mfg: %dx -> %dx (%s): presented %.1f fps, target %.1f, lower %.1f,"
          " wait %.0f%%, frame cost %.2f ms, back-off %.0f s\n",
          have + 1, want + 1, why, g_dyn_ctrl.PresentedFps(), g_dyn_ctrl.TargetFps(),
          g_dyn_ctrl.LowerFps(), g_dyn_ctrl.WaitShare() * 100.0,
          g_dyn_ctrl.FrameCostMs(), g_dyn_ctrl.BackoffMs() / 1000.0);
    fg_dyn_mfg_apply(want);
    g_dyn_log_ms = now_ms;
}


static void fg_auto_interp_tick(const char* why)
{
    if (!g_interp_auto || g_refresh_hz == 0 || g_fg_interp_max < 2) { return; }
    const double t_in = arr_median_ms();
    if (t_in < 1.0) { return; }
    const double slots = t_in / refresh_ms();          // vblanks per input frame
    int k = g_interp_auto_round ? static_cast<int>(slots + 0.5)
                                : static_cast<int>(slots);
    if (k < 2) { k = 2; }                              // 1 frame = no generation
    const int kmax = static_cast<int>(g_fg_interp_max) + 1;
    bool capped = false;
    if (k > kmax) { k = kmax; capped = true; }
    const int want = k - 1;

    if (capped && !g_auto_capped_logged) {
        g_auto_capped_logged = true;
        logf_("[bridge] --interp-auto: input %.1fms wants %dx for a whole-vblank cadence"
              " but this DLL caps at %ux - staying at %ux (VRR or a faster game frame"
              " rate are the only ways to 120 here)\n",
              t_in, static_cast<int>(slots + 0.5), g_fg_interp_max + 1,
              g_fg_interp_actual + 1);
    }

    if (want == static_cast<int>(g_fg_interp_actual)) {
        g_auto_pending = -1;
        g_auto_hits = 0;
        return;
    }
    // Only move when the new count is meaningfully closer to a whole number of
    // vblanks than the current one: a 0.1 vblank difference is not worth a model
    // switch (and the switch costs a visible hitch).
    const double err_want = dabs_(slots - static_cast<double>(k));
    const double err_have = dabs_(slots - static_cast<double>(g_fg_interp_actual + 1));
    if (err_want + 0.15 > err_have) {
        g_auto_pending = -1;
        g_auto_hits = 0;
        return;
    }

    if (want == g_auto_pending) {
        if (++g_auto_hits >= 2) {                      // stable for two windows
            g_auto_pending = -1;
            g_auto_hits = 0;
            fg_set_interp(static_cast<UINT>(want), why);
        }
    } else {
        g_auto_pending = want;
        g_auto_hits = 1;
    }
}

// The overlay has to fill the monitor the game is on, NOT the capture size.
//
// Those differ as soon as VSR/DSR is in play: the game renders and presents at
// 5120x2880 while the panel is 3840x2160. Sizing the window from the capture
// pixels would make a 4K output get stretched to 5K by DXGI and then downscaled
// to 4K by the driver - two resamples for nothing. Sized to the monitor, a 4K
// output lands exactly 1:1 on a 4K desktop (and a 5K output still fills a 5K
// desktop as before).
static void overlay_size(UINT* x, UINT* y, UINT* w, UINT* h)
{
    RECT r{};
    overlay_rect(&r);
    *x = static_cast<UINT>(r.left);
    *y = static_cast<UINT>(r.top);
    *w = static_cast<UINT>(r.right - r.left);
    *h = static_cast<UINT>(r.bottom - r.top);

    logf_("[bridge] overlay fits the monitor %ux%u at (%u,%u) (capture %ux%u)\n",
          *w, *h, *x, *y, g_full_w, g_full_h);
    if (*w != g_full_w || *h != g_full_h) {
        logf_("[bridge]   captured %ux%u will be resampled once by us to the output size;"
              " pick it with --scale/--den so it matches the panel for a 1:1 present\n",
              g_full_w, g_full_h);
    }
}

static bool init_fg()
{
    // --overlay-monitor must name a display the *consumer* adapter drives, otherwise every
    // CreateSwapChainForHwnd attempt returns E_ACCESSDENIED and bridge would exit with no
    // picture - the opposite of a useful fallback. Validate it here and fall back to the
    // game window's display instead.
    if (g_overlay_monitor >= 0 && g_consumer_adapter) {
        const HMONITOR want = overlay_monitor(g_game_wnd);
        if (!drives_monitor(g_consumer_adapter.Get(), want)) {
            logf_("[bridge] WARNING: --overlay-monitor %d is not driven by the consumer"
                  " adapter (%ls), so the swap chain cannot be created there - presenting"
                  " on the game window's display instead. Put both displays on one GPU, or"
                  " use --gpu to move frame generation to the card that drives that"
                  " display.\n", g_overlay_monitor, g_consumer_desc.Description);
            g_overlay_monitor = -1;
        }
    }

    UINT ox = 0, oy = 0, ow = 0, oh = 0;
    overlay_size(&ox, &oy, &ow, &oh);
    g_bridge_wnd = create_bridge_window(ow, oh);
    if (!g_bridge_wnd) { logf_("[bridge] window creation FAILED\n"); return false; }

    xefg_swapchain_result_t r = g_api.D3D12CreateContext(g_device.Get(), &g_fg);
    logf_("[bridge] xefgSwapChainD3D12CreateContext: %s (%d)\n", xefg_str(r), (int)r);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { return false; }

    // Let the library explain itself. Without this the only symptom of a refused
    // interpolation is frameGenResult = ERR_INVALID_ARGUMENT.
    g_api.SetLoggingCallback(g_fg, XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG, fg_log_cb, nullptr);

    // How many interpolated frames this DLL adds on THIS device. 1 means the
    // vendor check won: 2x only, whatever --interp says.
    {
        xefg_swapchain_properties_t props{};
        if (g_api.GetProperties(g_fg, &props) == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            g_fg_interp_max = props.maxSupportedInterpolations ? props.maxSupportedInterpolations : 1;
            logf_("[bridge] maxSupportedInterpolations = %u (descriptors=%u)\n",
                  g_fg_interp_max, props.requiredDescriptorCount);
        } else {
            g_fg_interp_max = 1;
            logf_("[bridge] GetProperties FAILED - assuming 1 interpolated frame\n");
        }
    }

    // Only a *request* above what the DLL allows means the patch failed. With --interp 1
    // (2x) the patch code deliberately does nothing ("nothing to patch"), so warning here
    // was a false alarm - it fired on every plain 2x run and made people distrust the log.
    if (g_unlock_mfg && g_fg_interp_max <= 1 &&
        (g_fg_interp > 1 || g_interp_cadence || g_interp_auto || g_dyn_mfg)) {
        logf_("[bridge] WARNING: --unlock-mfg is on and more than 2x was requested, but the"
              " DLL still reports 1 interpolated frame. The patch did not take - do not"
              " expect MFG.\n");
    }

    UINT want_interp = (g_fg_interp > 0) ? static_cast<UINT>(g_fg_interp) : g_fg_interp_max;
    // --interp-cadence starts at the maximum so the count can go either way per frame
    // without a re-init; cadence_next_slots() then sets 1..max every frame.
    if (g_interp_cadence) { want_interp = g_fg_interp_max; }
    // --dyn-mfg starts at 4x even when the ceiling is 6x: starting at 6x at 4K measured
    // 45 ms of GPU wait per real frame and collapsed the loop to 13 fps
    // (bridge_pbz_dynmfg.txt). The controller now only climbs with evidence.
    if (g_dyn_mfg) { want_interp = (g_fg_interp_max >= 3) ? 3u : g_fg_interp_max; }
    if (want_interp < 1) { want_interp = 1; }
    if (want_interp > g_fg_interp_max) {
        logf_("[bridge] --interp %d above this DLL's limit (%u) - using %u\n",
              g_fg_interp, g_fg_interp_max, g_fg_interp_max);
        want_interp = g_fg_interp_max;
    }
    g_fg_interp_actual = want_interp;

    if (g_fg_interp_max <= 1) {
        logf_("[bridge] MFG unavailable: libxess_fg.dll allows 1 interpolated frame here,"
              " so the output is 2x.\n");
        logf_("[bridge]   a community MFG build (plus a matching libxell.dll) raises this;"
              " select it with --fg-lib <path>\n");
        if (g_fg_interp > 1) {
            logf_("[bridge]   --interp %d ignored, running 2x\n", g_fg_interp);
        }
    } else {
        logf_("[bridge] MFG available: up to %ux (%u interpolated frames per real frame)\n",
              g_fg_interp_max + 1, g_fg_interp_max);
        logf_("[bridge]   running %ux (%u interpolated)\n", want_interp + 1, want_interp);
    }
    if (!g_api.has_mfg()) {
        logf_("[bridge] note: DLL has no SetNumInterpolatedFrames, so the count cannot be"
              " changed at runtime\n");
    }

    // XeLL must be attached BEFORE the proxy swap chain is initialized: without
    // it InitFromSwapChain fails with ERR_LATENCY_REDUCTION_UNSUPPORTED (-15).
    const xell_result_t xr = xellD3D12CreateContext(g_device.Get(), &g_xell);
    logf_("[bridge] xellD3D12CreateContext: %d\n", (int)xr);
    if (xr == XELL_RESULT_SUCCESS) {
        // Creating the context is not enough - XeLL has to be *enabled*, and
        // xefgSwapChainSetEnabled returns ERR_LATENCY_REDUCTION_UNSUPPORTED (-15)
        // until bLowLatencyMode is on.
        xell_sleep_params_t sp{};
        sp.minimumIntervalUs = 0;
        sp.bLowLatencyMode = g_xell_mode;
        const xell_result_t sr = xellSetSleepMode(g_xell, &sp);
        logf_("[bridge] xellSetSleepMode(lowLatency): %d\n", (int)sr);

        const xefg_swapchain_result_t lr =
            g_api.SetLatencyReduction(g_fg, reinterpret_cast<void*>(g_xell));
        logf_("[bridge] SetLatencyReduction: %s (%d)\n", xefg_str(lr), (int)lr);
    } else {
        logf_("[bridge] no XeLL context - XeSS-FG cannot initialize\n");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 sc{};
    if (g_scale > 0.0f) {
        sc.Width  = (static_cast<UINT>(g_full_w * g_scale)) & ~1u;
        sc.Height = (static_cast<UINT>(g_full_h * g_scale)) & ~1u;
    } else if (g_out_den > 1) {
        sc.Width  = g_full_w / static_cast<UINT>(g_out_den);
        sc.Height = g_full_h / static_cast<UINT>(g_out_den);
    } else {
        sc.Width  = g_full_w;
        sc.Height = g_full_h;
    }
    sc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;      // HDR10, the only format XeSS-FG accepts
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    if (g_tearing) { sc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING; }
    sc.BufferCount = g_swap_buffers;    // two presents per input frame out of this ring
    sc.Scaling = DXGI_SCALING_STRETCH;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs{};
    fs.Windowed = TRUE;

    // D3D12 devices do not implement IDXGIDevice (E_NOINTERFACE), so the factory
    // must be found through the device adapter LUID.
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGIFactory4> f4;
    {
        const LUID luid = g_device->GetAdapterLuid();
        logf_("[bridge] device LUID %08lX:%08lX\n",
              static_cast<unsigned long>(luid.HighPart),
              static_cast<unsigned long>(luid.LowPart));

        HRESULT h = CreateDXGIFactory2(0, IID_PPV_ARGS(&f4));
        logf_("[bridge] CreateDXGIFactory2: hr=0x%08lX\n", static_cast<unsigned long>(h));
        if (FAILED(h)) { return false; }

        ComPtr<IDXGIAdapter1> ad;
        h = f4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad));
        logf_("[bridge] EnumAdapterByLuid: hr=0x%08lX\n", static_cast<unsigned long>(h));
        if (SUCCEEDED(h) && ad) {
            h = ad->GetParent(IID_PPV_ARGS(&factory));
            logf_("[bridge] Adapter->GetParent(IDXGIFactory2): hr=0x%08lX\n",
                  static_cast<unsigned long>(h));
        }
        if (!factory) { f4.As(&factory); }
        if (!factory) { logf_("[bridge] no DXGI factory\n"); return false; }

        // Which adapter drives the monitor our window sits on? A mismatch here is
        // one way CreateSwapChainForHwnd ends in E_ACCESSDENIED.
        const HMONITOR mon = MonitorFromWindow(g_bridge_wnd, MONITOR_DEFAULTTOPRIMARY);
        logf_("[bridge] window monitor %p\n", reinterpret_cast<void*>(mon));
        for (unsigned i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (f4->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) { break; }
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            for (unsigned j = 0;; ++j) {
                ComPtr<IDXGIOutput> out;
                if (a->EnumOutputs(j, &out) == DXGI_ERROR_NOT_FOUND) { break; }
                DXGI_OUTPUT_DESC od{};
                out->GetDesc(&od);
                const bool ours = (od.Monitor == mon);
                logf_("[bridge]   '%ls' output %u %p%s\n", d.Description, j,
                      reinterpret_cast<void*>(od.Monitor), ours ? "  <== our window" : "");

                // HDR state decides how the frame has to be handed to the display.
                ComPtr<IDXGIOutput6> o6;
                if (ours && SUCCEEDED(out.As(&o6))) {
                    DXGI_OUTPUT_DESC1 od1{};
                    if (SUCCEEDED(o6->GetDesc1(&od1))) {
                        DEVMODEW dm{};
                        // The mode of the display we present on, not of the primary one:
                        // with --overlay-monitor they are different monitors, and quant
                        // would otherwise snap frameRenderTime to the wrong vblank.
        if (overlay_display_mode(&dm)) {
            logf_("[bridge] display mode: %lux%lu @ %lu Hz%s\n",
                  dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency,
                  (g_overlay_monitor >= 0) ? " (overlay monitor)" : "");

            g_refresh_hz = dm.dmDisplayFrequency;
            // A 30 fps x4 profile needs the panel to actually run at 120 Hz. 4K HDR over
            // a link without DSC lands on 59/60 Hz, and then 4x can never show more than
            // ~60 fps no matter what the frame generator does.
            if (g_refresh_hz > 0 && g_refresh_hz < 100) {
                logf_("[bridge] WARNING: panel runs at %lu Hz - 30 fps x%d needs %lu Hz; the"
                      " output will cap near %lu fps. Check the display mode and the cable"
                      " (4K HDR 120 Hz needs DP 1.4 + DSC or HDMI 2.1).\n",
                      dm.dmDisplayFrequency, g_fg_interp + 1,
                      static_cast<unsigned long>(30 * (g_fg_interp + 1)),
                      dm.dmDisplayFrequency);
            }
        }
        logf_("[bridge]   colour space %d (%s) bits=%u maxLum=%.0f maxFull=%.0f\n",
                              (int)od1.ColorSpace,
                              (od1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
                                  ? "HDR10 active" : "SDR (no HDR10)",
                              od1.BitsPerColor, od1.MaxLuminance, od1.MaxFullFrameLuminance);
                    }
                }
            }
        }
    }

    xefg_swapchain_d3d12_init_params_t ip{};
    // The count asked for here is what the DLL will actually add per real frame;
    // want_interp was already clamped to maxSupportedInterpolations above.
    ip.maxInterpolatedFrames = want_interp;
    ip.initFlags = g_fg_depth_inv ? XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH
                                  : XEFG_SWAPCHAIN_INIT_FLAG_NONE;
    // --fg-hi-res-mv / --fg-jittered-mv used to set the HIGH_RES_MV / JITTERED_MV flags
    // here. Both were removed with the rest of the MV work: neither flag changes the
    // picture on libxess_fg 1.3.1, because tagged MVs are a dead input
    // (docs/findings/FINAL_CONFIG.md 2.1.2 结论 1).
    if (ip.initFlags != XEFG_SWAPCHAIN_INIT_FLAG_NONE) {
        logf_("[bridge] initFlags=0x%x (inverted_depth=%d)\n",
              static_cast<unsigned>(ip.initFlags), g_fg_depth_inv ? 1 : 0);
    }
    // AUTO probes for a UI texture we never provide; NONE says it outright.
    ip.uiMode = g_fg_ui_none ? XEFG_SWAPCHAIN_UI_MODE_NONE : XEFG_SWAPCHAIN_UI_MODE_AUTO;
    ip.creationNodeMask = 1;
    ip.visibleNodeMask  = 1;

    if (g_out_den > 1 || g_scale > 0.0f) {
        // XeSS-FG creates the native swap chain from our description but does not keep
        // DXGI_SCALING_STRETCH, so a smaller back buffer is shown 1:1 in the corner.
        // Creating the swap chain here (with STRETCH) and handing it over fixes that.
        r = XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        logf_("[bridge] InitFromSwapChainDesc: skipped (output %ux%u), creating it ourselves\n",
              sc.Width, sc.Height);
    } else {
        r = g_api.D3D12InitFromSwapChainDesc(g_fg, g_bridge_wnd, &sc, &fs,
                                             g_queue.Get(), factory.Get(), &ip);
        logf_("[bridge] InitFromSwapChainDesc: %s (%d)\n", xefg_str(r), (int)r);
    }

    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        // The description route was rejected, so create the swap chain ourselves
        // and hand it over. Every attempt is logged, so one run shows exactly what
        // this machine accepts. app_swap is the global g_app_swap so the reference
        // survives past this block.
        ComPtr<IDXGISwapChain1>& app_swap = g_app_swap;

        struct Combo { DXGI_FORMAT format; DXGI_SWAP_EFFECT effect; const char* name; };
        const Combo combos[] = {
            { DXGI_FORMAT_R10G10B10A2_UNORM,   DXGI_SWAP_EFFECT_FLIP_DISCARD,    "10bit FLIP_DISCARD" },
            { DXGI_FORMAT_R10G10B10A2_UNORM,   DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, "10bit FLIP_SEQ" },
            { DXGI_FORMAT_R8G8B8A8_UNORM,      DXGI_SWAP_EFFECT_FLIP_DISCARD,    "8bit FLIP_DISCARD" },
            { DXGI_FORMAT_R16G16B16A16_FLOAT,  DXGI_SWAP_EFFECT_FLIP_DISCARD,    "FP16 FLIP_DISCARD" },
        };

        auto hwnd_attempts = [&](HWND wnd, const char* where) {
            for (const Combo& c : combos) {
                DXGI_SWAP_CHAIN_DESC1 d = sc;
                d.Format = c.format;
                d.SwapEffect = c.effect;
                ComPtr<IDXGISwapChain1> tmp;
                const HRESULT h = factory->CreateSwapChainForHwnd(g_queue.Get(), wnd, &d, &fs,
                                                                 nullptr, &tmp);
                logf_("[bridge] hwnd %-8s %-18s hr=0x%08lX\n", where, c.name,
                      static_cast<unsigned long>(h));
                if (SUCCEEDED(h)) { app_swap = tmp; g_sc_wnd = wnd; return true; }
            }
            return false;
        };

        // 1) our own overlay window
        if (hwnd_attempts(g_bridge_wnd, "popup")) {
            logf_("[bridge] using hwnd swap chain on the overlay window\n");
        }

        // 2) a plain window in case the popup style is the reason for E_ACCESSDENIED.
        //    It must carry the same click-through / never-activate styles: without them
        //    this fallback swallows the mouse and the keyboard of the game (measured on
        //    a UE5 title where route 1 failed - the overlay is a separate process, so
        //    HTTRANSPARENT alone does not pass input through).
        HWND plain = nullptr;
        if (!app_swap) {
            plain = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                        WS_EX_TOOLWINDOW,
                                    L"STATIC", L"MGPU Bridge SC", WS_POPUP,
                                    0, 0, static_cast<int>(g_full_w), static_cast<int>(g_full_h),
                                    nullptr, nullptr, nullptr, nullptr);
            logf_("[bridge] probe window %p (click-through)\n", reinterpret_cast<void*>(plain));
            if (plain) { hwnd_attempts(plain, "plain"); }
        }

        // 3) composition swap chain + DirectComposition: needs no hwnd ownership
        if (!app_swap) {
            DXGI_SWAP_CHAIN_DESC1 d = sc;
            d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            ComPtr<IDXGISwapChain1> tmp;
            const HRESULT h = factory->CreateSwapChainForComposition(g_queue.Get(), &d, nullptr,
                                                                     &tmp);
            logf_("[bridge] CreateSwapChainForComposition: hr=0x%08lX\n",
                  static_cast<unsigned long>(h));
            if (SUCCEEDED(h)) {
                ComPtr<IDXGIDevice> d11_dxgi;
                HRESULT hc = g_d3d11.As(&d11_dxgi);
                if (SUCCEEDED(hc)) { hc = DCompositionCreateDevice(d11_dxgi.Get(),
                                                                    IID_PPV_ARGS(&g_dcomp)); }
                if (SUCCEEDED(hc)) { hc = g_dcomp->CreateTargetForHwnd(g_bridge_wnd, TRUE,
                                                                       &g_dcomp_target); }
                if (SUCCEEDED(hc)) { hc = g_dcomp->CreateVisual(&g_dcomp_visual); }
                if (SUCCEEDED(hc)) { hc = g_dcomp_visual->SetContent(tmp.Get()); }
                if (SUCCEEDED(hc)) { hc = g_dcomp_target->SetRoot(g_dcomp_visual.Get()); }
                if (SUCCEEDED(hc)) { hc = g_dcomp->Commit(); }
                logf_("[bridge] DirectComposition attach: hr=0x%08lX\n",
                      static_cast<unsigned long>(hc));
                if (SUCCEEDED(hc)) { app_swap = tmp; }
            }
        }

        if (!app_swap) {
            logf_("[bridge] no swap chain could be created on this system\n");
            return false;
        }

        xefg_swapchain_d3d12_init_params_t ip2{};
        // Ownership is transferred: the guide says the passed swap chain is
        // released, and any extra outstanding reference makes re-creating the
        // native swap chain fail.
        ip2.pApplicationSwapChain = app_swap.Detach();
        ip2.maxInterpolatedFrames = want_interp;
        ip2.initFlags = ip.initFlags;
        ip2.uiMode = ip.uiMode;
        ip2.creationNodeMask = 1;
        ip2.visibleNodeMask  = 1;

        r = g_api.D3D12InitFromSwapChain(g_fg, g_queue.Get(), &ip2);
        logf_("[bridge] InitFromSwapChain: %s (%d)\n", xefg_str(r), (int)r);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { return false; }
    }

    // What the library really applied (it may clamp the request).
    if (g_api.D3D12GetInitParams) {
        xefg_swapchain_d3d12_init_params_t got{};
        if (g_api.D3D12GetInitParams(g_fg, &got) == XEFG_SWAPCHAIN_RESULT_SUCCESS &&
            got.maxInterpolatedFrames > 0) {
            g_fg_interp_actual = got.maxInterpolatedFrames;
            logf_("[bridge] applied maxInterpolatedFrames = %u -> %ux output\n",
                  g_fg_interp_actual, g_fg_interp_actual + 1);
        }
    }

    r = g_api.D3D12GetSwapChainPtr(g_fg, IID_PPV_ARGS(&g_swapchain));
    logf_("[bridge] GetSwapChainPtr: %s (%d) ptr=%p\n", xefg_str(r), (int)r,
          reinterpret_cast<void*>(g_swapchain.Get()));
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS || !g_swapchain) {
        logf_("[bridge] GetSwapChainPtr FAILED\n");
        return false;
    }

    {
        DXGI_SWAP_CHAIN_DESC1 real{};
        if (SUCCEEDED(g_swapchain->GetDesc1(&real))) {
            logf_("[bridge] FG swap chain: %ux%u format=%u buffers=%u flags=0x%X\n",
                  real.Width, real.Height, static_cast<unsigned>(real.Format),
                  real.BufferCount, real.Flags);
        }
    }

    // Present was blocking 31-47 ms, which capped the loop at ~24 fps (below the
    // game's own rate) and let stale frames pile up: that is where the huge input
    // latency came from. One queued frame instead of three.
    {
        ComPtr<IDXGISwapChain2> sc2;
        if (SUCCEEDED(g_swapchain.As(&sc2))) {
            const HRESULT h = sc2->SetMaximumFrameLatency(g_max_latency);
            logf_("[bridge] SetMaximumFrameLatency(%u): hr=0x%08lX\n", g_max_latency,
                  static_cast<unsigned long>(h));
        }
    }

    // which reports HDR10 active. scRGB cannot be used here - XeSS-FG rejects it.
    const DXGI_COLOR_SPACE_TYPE want_cs =
        (g_colour_mode == 2) ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
                             : DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const HRESULT cs = g_swapchain->SetColorSpace1(want_cs);
    logf_("[bridge] SetColorSpace1(%s): hr=0x%08lX\n",
          (g_colour_mode == 2) ? "SDR" : "HDR10", static_cast<unsigned long>(cs));

    // The swap chain exists now, so the window that owns it becomes the visible
    // overlay. Our own window stays hidden unless it was the one that won, which
    // avoids a blank white window sitting on top of the picture.
    make_borderless(overlay_wnd(), static_cast<int>(ox), static_cast<int>(oy), ow, oh);
    if (g_overlay_layered) {
        const LONG_PTR ex = GetWindowLongPtrW(overlay_wnd(), GWL_EXSTYLE);
        SetWindowLongPtrW(overlay_wnd(), GWL_EXSTYLE, ex | WS_EX_LAYERED);
        const BOOL ok = SetLayeredWindowAttributes(overlay_wnd(), 0, 255, LWA_ALPHA);
        logf_("[bridge] --overlay-layered: WS_EX_LAYERED + alpha 255 (ok=%d)\n",
              static_cast<int>(ok));
    }

    if (g_no_fg) {
        logf_("[bridge] --no-fg: proxy swap chain only, frame generation left off\n");
        return true;
    }

    if (g_fg_scene > 0.0f) {
        if (g_api.SetSceneChangeThreshold) {
            const xefg_swapchain_result_t sc = g_api.SetSceneChangeThreshold(g_fg, g_fg_scene);
            logf_("[bridge] SetSceneChangeThreshold(%.2f): %s (%d)\n",
                  g_fg_scene, xefg_str(sc), static_cast<int>(sc));
        } else {
            logf_("[bridge] --fg-scene ignored: DLL has no SetSceneChangeThreshold\n");
        }
    }

    r = g_api.SetEnabled(g_fg, 1);
    logf_("[bridge] SetEnabled: %s (%d)\n", xefg_str(r), (int)r);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { return false; }

    // If the init parameter could not express the request (older DLL, or a
    // mismatch after the library clamped it) try the runtime call as well.
    fg_apply_interp("after SetEnabled");
    return true;
}


static HANDLE g_map = nullptr;

// Places our views of the producer's MV/depth slots inside the shared heap.
//
// Protocol v9: the slots are placed at their *capacity* (StreamShared.slot_w/h) and never
// move, so this runs exactly once. The pitch comes from the capacity - placing with the
// live content size instead would read a different memory layout. What the producer
// publishes per frame is only the live rectangle (`width x height`), which copy_inputs_local
// copies out of these views.
static bool place_shared_views()
{
    if (!g_ctl || !g_shared_heap) { return false; }
    for (UINT s = 0; s < mgpu::kMaxSlots; ++s) {
        for (int k = 1; k <= 2; ++k) {                 // 1 = MV, 2 = depth
            const mgpu::StreamShared& st = g_ctl->streams[k];
            if (st.slot_size == 0) { continue; }
            ComPtr<ID3D12Resource>& dst = (k == 1) ? g_mv[s] : g_depth[s];
            if (dst) { continue; }                     // already placed (v9: never moves)
            const UINT pw = st.slot_w ? st.slot_w : st.width;
            const UINT ph = st.slot_h ? st.slot_h : st.height;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width            = pw;
            rd.Height           = ph;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.Format           = static_cast<DXGI_FORMAT>(st.format);
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

            const UINT64 off = st.offset + st.slot_size * s;
            if (FAILED(g_device->CreatePlacedResource(g_shared_heap.Get(), off, &rd,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&dst)))) {
                logf_("[bridge] placed resource FAILED (stream %d slot %u)\n", k, s);
                return false;
            }
        }
    }
    return true;
}

static const char* input_api_name(UINT api)
{
    switch (api) {
        case mgpu::kInputApiNgx:   return "ngx/dlss";
        case mgpu::kInputApiFfx:   return "ffx/fsr";
        case mgpu::kInputApiXess:  return "xess";
        case mgpu::kInputApiDepth: return "reshade-depth";
        default:                   return "none";
    }
}

// Protocol v6 peek: read the producer's identity out of an *existing* mapping
// without waiting for it. The bridge is normally started after the game, so this
// usually answers immediately and the adapter pick can avoid the producer's GPU
// without --producer-luid. When the mapping is not there yet, the pick falls back to
// the monitor rule and open_shared() logs the same information later.
static bool peek_producer()
{
    HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, mgpu::kMappingName);
    if (!m) { return false; }

    bool ok = false;
    auto* c = static_cast<const mgpu::SharedControl*>(
        MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(mgpu::SharedControl)));
    if (c) {
        if (c->magic == mgpu::kSharedMagic && c->version == mgpu::kSharedVersion &&
            c->producer.device_luid != 0) {
            g_producer_luid     = c->producer.device_luid;
            g_producer_api      = c->producer.input_api;
            g_producer_flags    = c->producer.input_flags;
            g_producer_render_w = c->producer.render_w;
            g_producer_render_h = c->producer.render_h;
            g_producer_output_w = c->producer.output_w;
            g_producer_output_h = c->producer.output_h;
            g_have_producer     = true;
            ok = true;

            int32_t hi = 0;
            uint32_t lo = 0;
            mgpu::unpack_luid(g_producer_luid, &hi, &lo);
            logf_("[bridge] shared block present: producer pid=%u luid=%08lX:%08lX"
                  " api=%s flags=0x%X render=%ux%u output=%ux%u\n",
                  c->producer_pid, static_cast<unsigned long>(static_cast<uint32_t>(hi)),
                  static_cast<unsigned long>(lo), input_api_name(g_producer_api),
                  g_producer_flags, g_producer_render_w, g_producer_render_h,
                  g_producer_output_w, g_producer_output_h);
        }
        UnmapViewOfFile(const_cast<mgpu::SharedControl*>(c));
    }
    CloseHandle(m);
    return ok;
}

static bool open_shared()
{
    logf_("[bridge] waiting for '%s' ...\n", mgpu::kMappingName);

    void* view = nullptr;
    bool version_mismatch = false;
    for (int i = 0; i < 1200 && !view && !version_mismatch; ++i) {
        g_map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mgpu::kMappingName);
        if (g_map) {
            view = MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            if (view) {
                const auto* c = reinterpret_cast<const mgpu::SharedControl*>(view);
                if (c->magic == mgpu::kSharedMagic) {
                    if (c->version == mgpu::kSharedVersion) {
                        g_ctl = const_cast<mgpu::SharedControl*>(c);
                        break;
                    }
                    // A live mapping that speaks another protocol: waiting cannot
                    // fix that, and silently reading it would pair the wrong fields
                    // (v5 had no `producer`, so the LUID would be garbage).
                    logf_("[bridge] shared block version %u, this bridge speaks %u -"
                          " rebuild/redeploy BOTH sides (add-on + bridge) so they match\n",
                          c->version, mgpu::kSharedVersion);
                    version_mismatch = true;
                }
                UnmapViewOfFile(view);
                view = nullptr;
            }
            CloseHandle(g_map);
            g_map = nullptr;
        }
        if (i == 100) {
            // 10 seconds in: the game window exists, so the missing piece is the add-on.
            logf_("[bridge] still waiting after 10 s. The mapping is created by the add-on on"
                  " its first captured frame, so if it never appears the add-on is not running:"
                  " check (1) ReShade loaded into the game, (2) at least one enabled technique"
                  " (ReShadePreset.ini), (3) DLSS on in-game, (4) %ls is the process you expect."
                  " See docs/findings/FINAL_CONFIG.md 2.1.1.\n",
                  g_game_exe_set ? g_game_exe.c_str() : L"(foreground window)");
        }
        Sleep(100);
    }
    if (version_mismatch) { return false; }
    if (!g_ctl) { logf_("[bridge] shared block never appeared\n"); return false; }

    g_width  = g_ctl->streams[0].width;
    g_height = g_ctl->streams[0].height;
    logf_("[bridge] shared: pid=%u %ux%u slots=%u\n",
          g_ctl->producer_pid, g_width, g_height, g_ctl->slots);

    // v6: producer identity and input semantics. `peek_producer()` usually filled
    // these before the adapter pick; when the mapping appeared only now, adopt it here.
    // kInputFlagInvertedDepth has to be known before init_fg(), which is why this is
    // read here rather than per frame.
    {
        g_producer_luid     = g_ctl->producer.device_luid;
        g_producer_api      = g_ctl->producer.input_api;
        g_producer_flags    = g_ctl->producer.input_flags;
        g_producer_render_w = g_ctl->producer.render_w;
        g_producer_render_h = g_ctl->producer.render_h;
        g_producer_output_w = g_ctl->producer.output_w;
        g_producer_output_h = g_ctl->producer.output_h;
        if (g_producer_luid != 0) { g_have_producer = true; }

        int32_t hi = 0;
        uint32_t lo = 0;
        mgpu::unpack_luid(g_producer_luid, &hi, &lo);
        logf_("[bridge] shared: producer luid=%08lX:%08lX api=%s flags=0x%X"
              " render=%ux%u output=%ux%u\n",
              static_cast<unsigned long>(static_cast<uint32_t>(hi)),
              static_cast<unsigned long>(lo), input_api_name(g_producer_api),
              g_producer_flags, g_producer_render_w, g_producer_render_h,
              g_producer_output_w, g_producer_output_h);

        if (g_producer_flags & mgpu::kInputFlagHasCamera) {
            logf_("[bridge]   producer supplies the camera (near/far/vfov) - the"
                  " projection no longer depends on --fg-fov\n");
        }
        if ((g_producer_flags & mgpu::kInputFlagInvertedDepth) && !g_fg_depth_inv) {
            g_fg_depth_inv = true;
            logf_("[bridge]   producer says the depth is inverted - enabling"
                  " XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH (as --fg-depth-inv would)\n");
        }
        if (g_have_producer) {
            const uint64_t consumer_luid =
                mgpu::pack_luid(g_consumer_desc.AdapterLuid.HighPart,
                                static_cast<uint32_t>(g_consumer_desc.AdapterLuid.LowPart));
            if (consumer_luid == g_producer_luid) {
                logf_("[bridge] WARNING: producer and consumer are the SAME GPU - there is"
                      " no cross-adapter transfer, frame generation competes with the game\n");
            }
        }
    }
    // The colour stream is a placeholder (the add-on does not allocate it with Color=0);
    // MV and depth are the ones that actually matter. Their size follows the game's
    // render resolution - DLSS Quality at 4K is 1440p, DLAA is 2160p - unless the add-on
    // downsamples them ([capture] StreamPercent), which also halves the transfer.
    logf_("[bridge] MV %ux%u, depth %ux%u (stream size)\n",
          g_ctl->streams[1].width, g_ctl->streams[1].height,
          g_ctl->streams[2].width, g_ctl->streams[2].height);

    g_full_w = g_width;
    g_full_h = g_height;

    // Capture stays at the full window size and only the presented image is divided:
    // a smaller WGC frame pool crops instead of scaling, which zoomed the picture.
    // The size printed here is what the *shared block declares*; adopt_capture_size()
    // replaces it with the real window size before anything is built from it.
    if (g_scale > 0.0f) {
        logf_("[bridge] shared block declares %ux%u, present at scale %.2f\n",
              g_width, g_height, g_scale);
    } else if (g_out_den > 1) {
        logf_("[bridge] shared block declares %ux%u, present at 1/%d\n",
              g_width, g_height, g_out_den);
    } else {
        logf_("[bridge] shared block declares %ux%u, present at the same size\n",
              g_width, g_height);
    }

    if (g_no_inputs) {
        logf_("[bridge] --no-inputs: not importing the cross-adapter heap\n");
        return true;
    }
    if (g_no_depth) {
        logf_("[bridge] --no-depth: depth stream skipped, XeFG gets a dummy depth"
              " (disocclusion quality will drop, transfer halves)\n");
    }
    if (g_no_mv) {
        logf_("[bridge] --no-mv: motion vectors are zeroed, the real depth is kept\n");
    }
#if MGPU_FG_OF
    if (g_fg_of) {
        logf_("[bridge] --fg-of: motion vectors come from FidelityFX optical flow on the"
              " captured colour (the producer's MV stream is not copied)\n");
        if (g_of_const_on) {
            logf_("[bridge]   DIAGNOSTIC: constant MV (%.1f, %.1f) px on every frame\n",
                  static_cast<double>(g_of_const_x), static_cast<double>(g_of_const_y));
        }
        logf_("[bridge]   units=%s sign=%.0f scale=%.3f guide=%.0f deadzone=%.2fpx max=%.0fpx\n",
              (g_of_units == 3) ? "mvpx" : ((g_of_units == 0) ? "px"
                                           : ((g_of_units == 1) ? "uv" : "ndc")),
              static_cast<double>(g_of_sign), static_cast<double>(g_of_scale),
              static_cast<double>(g_of_guide), static_cast<double>(g_of_deadzone),
              static_cast<double>(g_of_maxlen));
    }
#endif

    ComPtr<ID3D12Device3> device3;
    if (FAILED(g_device.As(&device3))) { return false; }

    if (FAILED(device3->OpenExistingHeapFromFileMapping(g_map, IID_PPV_ARGS(&g_shared_heap)))) {
        logf_("[bridge] OpenExistingHeapFromFileMapping FAILED\n");
        return false;
    }

    if (!place_shared_views()) { return false; }
    // Read the published stream geometry once here: service_shared_layout() only re-reads
    // it when `generation` changes, so this is the first and possibly only time. If the
    // producer has not published a size yet, that path keeps retrying (mv_w == 0).
    g_seen_generation = g_ctl->generation;
    g_seen_geo_generation = 0xFFFFFFFFu;    // force the geometry read below
    refresh_input_geo();
    g_seen_geo_generation = g_ctl->generation;

    HANDLE producer = OpenProcess(PROCESS_DUP_HANDLE, FALSE, g_ctl->producer_pid);
    if (!producer) { logf_("[bridge] OpenProcess FAILED\n"); return false; }
    for (UINT s = 0; s < g_ctl->slots && s < mgpu::kMaxSlots; ++s) {
        HANDLE lh = nullptr;
        if (!DuplicateHandle(producer, reinterpret_cast<HANDLE>(g_ctl->fence_forward[s]),
                             GetCurrentProcess(), &lh, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            logf_("[bridge] DuplicateHandle FAILED\n");
            CloseHandle(producer);
            return false;
        }
        const HRESULT hr = g_device->OpenSharedHandle(lh, IID_PPV_ARGS(&g_fwd[s]));
        CloseHandle(lh);
        if (FAILED(hr)) {
            logf_("[bridge] fence OpenSharedHandle FAILED\n");
            CloseHandle(producer);
            return false;
        }
    }
    CloseHandle(producer);

    logf_("[bridge] MV/depth placed, fences duplicated\n");
    return true;
}


static ComPtr<IDXGIKeyedMutex> g_colour_mutex;

// ============================ HDR10 output path =============================
//
// XeSS-FG refuses an FP16 back buffer ("Backbuffer format is not valid. Found
// 40"), so the output side must be R10G10B10A2_UNORM in HDR10 / BT.2100 colour
// space. The capture stays FP16 scRGB and is converted here: BT.709 -> BT.2020
// primaries, then PQ (SMPTE ST 2084) encoding. scRGB 1.0 is defined as 80 nits,
// hence the sdrWhiteNits constant.

static ComPtr<ID3D12RootSignature> g_conv_root;
static ComPtr<ID3D12PipelineState> g_conv_pso;
static ComPtr<ID3D12DescriptorHeap> g_srv_heap;
static ComPtr<ID3D12DescriptorHeap> g_rtv_heap;
static ComPtr<ID3D12Resource>       g_rtv_res[4];

// XeSS-FG's swap chain is created once at a fixed size - nothing in here calls
// ResizeBuffers - so the buffers behind it never change identity. GetBuffer() per frame is
// a QueryInterface plus an AddRef/Release round trip for nothing; cache them. One COM
// reference per buffer is held for the life of the process, which is exactly what a
// FLIP_DISCARD swap chain allows.
static ComPtr<ID3D12Resource>       g_bb_cache[4];

static bool back_buffer(UINT index, ID3D12Resource** out)
{
    if (out) { *out = nullptr; }
    if (index > 3 || !g_swapchain || !g_device) { return false; }
    if (!g_bb_cache[index]) {
        if (FAILED(g_swapchain->GetBuffer(index, IID_PPV_ARGS(&g_bb_cache[index])))) {
            logf_("[bridge] swap chain GetBuffer(%u) FAILED\n", index);
            return false;
        }
    }
    if (out) { *out = g_bb_cache[index].Get(); }
    return true;
}
static UINT g_rtv_stride = 0;

static const char* k_conv_hlsl = R"HLSL(
Texture2D<float4>  src : register(t0);
SamplerState       smp : register(s0);
// down/support/taps/dstW/dstH drive the resampling kernel (see --down). down 0 keeps
// the original single bilinear tap, so the old behaviour is one flag away.
cbuffer Params : register(b0) {
    float sdrWhiteNits;   // scRGB 1.0 = 80 nits
    float mode;           // 0 = HDR10 + BT.2020, 1 = HDR10 plain, 2 = SDR
    float sharpen;        // unsharp amount, applied AFTER the resize
    float down;           // 0 bilinear, 1 Catmull-Rom, 2 Mitchell, 3 Lanczos-3
    float support;        // kernel support, in output-pixel units (0 = bilinear)
    float taps;           // taps per axis that cover 2*support at this scale
    float dstW;           // render target size (the kernel needs the ratio)
    float dstH;
    float pad0; float pad1; float pad2; float pad3;
};

float3 pq_encode(float3 nits)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

// ---------------------------------------------------------------- kernels
// All three are evaluated on the *output* grid and stretched by the scale factor,
// which is what turns them into proper low-pass filters instead of a point tap.
float k_catmull(float x)            // support 2, sharp, tiny ringing
{
    x = abs(x);
    if (x < 1.0) { return (1.5 * x - 2.5) * x * x + 1.0; }
    if (x < 2.0) { return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0; }
    return 0.0;
}

float k_mitchell(float x)           // support 2, B = C = 1/3, no ringing
{
    const float B = 1.0 / 3.0, C = 1.0 / 3.0;
    x = abs(x);
    if (x < 1.0) {
        return ((12.0 - 9.0 * B - 6.0 * C) * x * x * x +
                (-18.0 + 12.0 * B + 6.0 * C) * x * x + (6.0 - 2.0 * B)) / 6.0;
    }
    if (x < 2.0) {
        return ((-B - 6.0 * C) * x * x * x + (6.0 * B + 30.0 * C) * x * x +
                (-12.0 * B - 48.0 * C) * x + (8.0 * B + 24.0 * C)) / 6.0;
    }
    return 0.0;
}

float k_lanczos3(float x)           // support 3, sharpest, visible ringing
{
    x = abs(x);
    if (x < 1e-5) { return 1.0; }
    if (x >= 3.0) { return 0.0; }
    const float px = 3.14159265 * x;
    return 3.0 * sin(px) * sin(px / 3.0) / (px * px);
}

float kw(float x)
{
    if (down < 1.5) { return k_catmull(x); }
    if (down < 2.5) { return k_mitchell(x); }
    return k_lanczos3(x);
}

// ---------------------------------------------------------------- resample
// Separable in maths, done in one pass here: the tap count is small (7 at 0.75x,
// 9 at 0.5x) and this only runs once per *real* frame, so the extra fetches are
// cheap next to the frame-generation work that follows.
float3 downsample(VSOut i, uint tw, uint th)
{
    float2 scl = float2(float(tw) / dstW, float(th) / dstH);   // > 1 when shrinking
    float2 c   = (i.pos.xy + 0.5) * scl - 0.5;
    float2 sup = support * scl;                                // support, in input texels
    int2   lo  = int2(floor(c - sup));
    int    n   = int(taps + 0.5);
    int    mw  = int(tw) - 1;
    int    mh  = int(th) - 1;

    float3 acc = 0.0;
    float  wsum = 0.0;
    [loop] for (int y = 0; y < n; ++y) {
        int   iy = lo.y + y;
        float wy = kw((float(iy) - c.y) / scl.y);
        if (wy == 0.0) { continue; }
        // Load() instead of SampleLevel(): the taps sit on texel centres, so this is
        // exact and it skips the bilinear filter that would blur them together.
        int cy = clamp(iy, 0, mh);
        [loop] for (int x = 0; x < n; ++x) {
            int   ix = lo.x + x;
            float wx = kw((float(ix) - c.x) / scl.x);
            float w  = wx * wy;
            if (w == 0.0) { continue; }
            acc  += max(src.Load(int3(clamp(ix, 0, mw), cy, 0)).rgb, 0.0) * w;
            wsum += w;
        }
    }
    return acc / max(wsum, 1e-4);
}

VSOut vs_main(uint id : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((id == 2) ? 2.0 : 0.0, (id == 1) ? 2.0 : 0.0);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 ps_main(VSOut i) : SV_Target
{
    uint tw = 1u, th = 1u;
    src.GetDimensions(tw, th);
    float2 texel = 1.0 / float2(tw, th);

    // scRGB, 1.0 = 80 nits. Both paths filter in linear light, which is what scRGB
    // already is - resampling after a gamma encode would darken every edge.
    float3 lin = (down < 0.5) ? max(src.SampleLevel(smp, i.uv, 0).rgb, 0.0)
                              : downsample(i, tw, th);

    // Unsharp mask: mainly useful on the bilinear path (it restores what the single
    // tap throws away). With a real kernel it mostly amplifies ringing, so keep it off.
    if (sharpen > 0.0) {
        float3 blur = 0.25 * (src.SampleLevel(smp, i.uv + float2( texel.x, 0.0), 0).rgb +
                              src.SampleLevel(smp, i.uv + float2(-texel.x, 0.0), 0).rgb +
                              src.SampleLevel(smp, i.uv + float2(0.0,  texel.y), 0).rgb +
                              src.SampleLevel(smp, i.uv + float2(0.0, -texel.y), 0).rgb);
        lin = max(lin + (lin - blur) * sharpen, 0.0);
    }

    // mode 2: SDR output - plain gamma encode of the linear value.
    if (mode > 1.5) {
        float3 c = saturate(lin);
        float3 s = (c <= 0.0031308) ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
        return float4(s, 1.0);
    }

    float3 nits = lin * sdrWhiteNits;
    float3 v = nits;
    // mode 0: BT.709 -> BT.2020 primaries (rows sum to 1), then PQ.
    // mode 1: same without the primaries conversion, for comparison.
    if (mode < 0.5) {
        const float3x3 k709to2020 = float3x3(
            0.6274, 0.3293, 0.0433,
            0.0691, 0.9195, 0.0114,
            0.0164, 0.0880, 0.8956);
        v = mul(k709to2020, nits);
    }
    return float4(pq_encode(v), 1.0);
}
)HLSL";

static bool init_convert()
{
    ComPtr<ID3DBlob> vs, ps, err;

    HRESULT hr = D3DCompile(k_conv_hlsl, strlen(k_conv_hlsl), "conv.hlsl", nullptr, nullptr,
                            "vs_main", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) {
        logf_("[bridge] VS compile FAILED: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    hr = D3DCompile(k_conv_hlsl, strlen(k_conv_hlsl), "conv.hlsl", nullptr, nullptr,
                    "ps_main", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr)) {
        logf_("[bridge] PS compile FAILED: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 12;        // white nits, mode, sharpen, down,
                                                   // support, taps, dstW, dstH
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;

    ComPtr<ID3DBlob> sig, serr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &serr);
    if (FAILED(hr)) { logf_("[bridge] SerializeRootSignature FAILED\n"); return false; }
    hr = g_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                      IID_PPV_ARGS(&g_conv_root));
    if (FAILED(hr)) { logf_("[bridge] CreateRootSignature FAILED\n"); return false; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_conv_root.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = 0xFFFFFFFFu;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
    pso.SampleDesc.Count = 1;
    hr = g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_conv_pso));
    if (FAILED(hr)) { logf_("[bridge] CreateGraphicsPipelineState FAILED\n"); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = 1;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srv_heap)))) { return false; }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_shared12.Get(), &srv,
                                       g_srv_heap->GetCPUDescriptorHandleForHeapStart());

    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = 6;      // 4 back buffers + dummy depth + dummy MV
    if (FAILED(g_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtv_heap)))) { return false; }
    g_rtv_stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    logf_("[bridge] HDR10 conversion pipeline ready (scRGB -> BT.2020 PQ)\n");
    return true;
}

// ------------------------------ output statistics ----------------------------
// The drawn-in FPS counter (seven-segment digits plus the dark plate behind them)
// has been removed: nothing is drawn on top of the picture any more. g_show_fps
// stays because the single-line OSD and the periodic log still report the rate.
static int  g_show_fps = 0;

static bool create_colour_sink(UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width  = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // NTHANDLE is only legal together with KEYEDMUTEX (D3D11 requirement).
    // D3D12 cannot take a keyed mutex, so only this side acquires/releases it
    // and the D3D12 side is ordered by the shared fence instead.
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    const HRESULT ch = g_d3d11->CreateTexture2D(&d, nullptr, &g_shared11);
    if (FAILED(ch)) {
        logf_("[bridge] colour sink CreateTexture2D FAILED hr=0x%08lX\n",
              static_cast<unsigned long>(ch));
        return false;
    }
    if (FAILED(g_shared11.As(&g_colour_mutex))) {
        logf_("[bridge] colour sink keyed mutex unavailable\n");
        return false;
    }
    ComPtr<IDXGIResource1> dxgi_res;
    if (FAILED(g_shared11.As(&dxgi_res))) { return false; }
    HANDLE sh = nullptr;
    if (FAILED(dxgi_res->CreateSharedHandle(nullptr, GENERIC_ALL, L"mgpu_colour_sink", &sh))) {
        logf_("[bridge] colour sink CreateSharedHandle FAILED\n");
        return false;
    }
    const HRESULT hr = g_device->OpenSharedHandle(sh, IID_PPV_ARGS(&g_shared12));
    CloseHandle(sh);
    if (FAILED(hr)) { logf_("[bridge] colour sink OpenSharedHandle FAILED\n"); return false; }

    logf_("[bridge] colour sink %ux%u FP16 shared to D3D12\n", w, h);
    return true;
}


// ------------------------------------------------------------ frame loop

static UINT64 g_fence11_value = 0;
static UINT64 g_present_id = 0;
static UINT   g_mv_slot = 0;
static UINT   g_slot_timeouts = 0;
static UINT64 g_frames = 0;
// Each back buffer carries its own state: with FLIP_DISCARD and BufferCount=2 the
// presented buffer alternates, and writing only buffer 0 left the other one stale
// - which is what flickered on screen.
static D3D12_RESOURCE_STATES g_bb_state[4] = {
    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
};
static double g_t0 = 0.0;
static ULONGLONG g_t_report = 0;   // start of the current 40-frame report window
// How long Present() blocked, and the measured frame interval handed to XeSS-FG as
// frameRenderTime (a hardcoded value misleads its pacing).
static DWORD g_present_ms = 0;
// Stage breakdown, all in milliseconds, exponential moving average.
static double g_ms_capture = 0.0;   // waiting for the next WGC frame
static double g_ms_work = 0.0;      // our CPU work up to submitting the command list
static double g_ms_present = 0.0;   // Present() blocking
static double g_ms_throttle = 0.0;  // waiting for the GPU to finish the previous frame
static double g_frame_ms_ema = 25.0;      // interval between our run_frame() calls
static double g_ms_wait = 0.0;            // waiting for the producer's MV/depth slot
static double g_max_wait = 0.0;           // worst such wait in this report window
static double g_ms_copywait = 0.0;        // waiting for the 118 MB colour copy
static double g_max_copywait = 0.0;
// The D3D11 side of the same hop (keyed-mutex acquire + CopyResource). Separate from
// `copy` because they measure different halves: `colour` is CPU + this card's copy
// submission, `copy` is the D3D12 queue waiting for it to land.
static double g_ms_colour = 0.0;
static double g_max_colour = 0.0;
static double g_ms_total = 0.0;           // whole run_frame() duration
static double g_max_total = 0.0;
static double g_ms_inputs = 0.0;          // recording the cross-adapter MV/depth copies
static double g_max_inputs = 0.0;
static double g_ms_xell = 0.0;            // xellSleep(): XeLL's own latency pacing delay
static double g_max_xell = 0.0;
static double g_ms_age = 0.0;             // age of the WGC frame when we receive it
static double g_max_age = 0.0;
static double g_ms_fg = 0.0;              // xefgSwapChain tagging calls
static double g_max_fg = 0.0;
static DWORD  g_wait_ms = 20;             // --wait-ms N: timeout for that wait
static ULONGLONG g_last_arrival = 0;
static double g_presented_sum = 0.0;      // frames XeSS-FG emitted per input frame
static UINT   g_presented_n   = 0;
// Arrival jitter diagnostics: how irregular the game's frames reach us, and how many gaps
// were longer than 1.6x the mean interval. NOT a frame-drop counter: with a 40 fps lock on
// a 120 Hz panel the intervals alternate 16.7/33.3/41.7 ms, and every 41.7 ms gap counts -
// so the number tracks the beat pattern, not lost frames (renamed to arr_gaps in the log
// because "wgc_drops" read as if frames were being dropped).
static double g_arr_min = 1e9, g_arr_max = 0.0;
static double g_arr_prev = 0.0;           // previous WGC timestamp in ms
static UINT   g_arr_drops = 0;
static UINT   g_slow_frames = 0;   // frames whose GPU work did not fit the input interval
// Worst-case tracking inside a report window: averages hide the one-frame hitches
// that are exactly what the eye notices.
static double g_max_gpu = 0.0, g_max_present = 0.0, g_max_gap = 0.0;
static double g_last_present_qpc = 0.0;
static UINT   g_x2_miss = 0;       // presents where XeSS-FG emitted no interpolated frame
static UINT   g_hitch_logged = 0;  // hitches reported this window (rate limit)
// One-shot diagnostics: the present-size check only makes sense once the window is at
// its final size, and the "present is the bottleneck" note is worth saying once.
static bool   g_present_size_checked = false;
static bool   g_present_cap_warned = false;
static bool   g_diag_dumped = false;    // producer state printed once (wait_slot)
static UINT   g_report_every = 40; // --report N: frames per statistics window
static UINT   g_arr_count = 0;
static ULONGLONG g_last_frame_tick = 0;

// Bucketed arrival histogram, in vblanks, reset every report window. This is the direct
// answer to "are we losing input frames, or is the game simply not delivering them?":
// a locked 40 fps game on a 120 Hz panel must land in bucket 3 (25.0 ms) almost every
// time; anything in bucket 4+ is a missing/gated frame, and a spread over 2/3/4 means the
// presenter itself is not holding its cadence.
static UINT   g_arr_hist[9] = {};         // index = vblanks rounded, 8 = "8 or more"
// The same bucketing for the interval the *producer* measured between the game's own two
// Present calls (FrameShared.frame_time_ms, published as a raw value - not an EMA).
// Comparing this with the composition-side histogram splits "the game wobbles" from
// "DWM / our own overlay coalesces our captures":
//   game_arr and arr both spread  -> the game's own presents are uneven
//   game_arr flat, arr spread     -> the frames leave the game evenly and are lost after
//                                    that (composition, our overlay, capture pool)
static UINT   g_game_hist[9] = {};
// The same median taken from two different clocks. `dwm` uses the capture frame's
// SystemRelativeTime (what the compositor stamped); `wc` uses the wall clock at the moment
// we dequeued it. They should agree. When they do not, the difference is our own pickup
// jitter, not lost input - which is exactly the mistake `input=` used to make.
static double g_arr_wc_ring[kArrRing] = {};
static UINT   g_arr_wc_n = 0;
static UINT   g_arr_wc_i = 0;

static double arr_wc_median_ms()
{
    if (g_arr_wc_n == 0) { return 0.0; }
    double v[kArrRing];
    const UINT n = g_arr_wc_n;
    for (UINT i = 0; i < n; ++i) { v[i] = g_arr_wc_ring[i]; }
    for (UINT i = 1; i < n; ++i) {
        const double k = v[i];
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; --j; }
        v[j + 1] = k;
    }
    return v[n / 2];
}

static double qpc_ms();   // defined next to the pacer below

// Pacing state. Declared here because run_frame_warp needs g_pace_ms for its own
// present timing (interpolated frame and real frame one half tick apart).
static bool   g_pace_auto = false;      // --pace auto: follow the measured source rate
static double g_pace_ms = 0.0;          // 0 = off
static double g_pace_next = 0.0;        // next tick in QPC milliseconds
static double g_represents = 0.0;       // how often we had to re-send the last frame
static UINT   g_pace_skips = 0;         // extra frames that arrived inside one tick
static bool   g_have_last = false;
static UINT   g_last_slot = 0;
static bool   g_reuse_frame = false;
static bool   g_no_throttle = false;    // --no-throttle: do not wait for the previous frame

// Windows defaults to a 15.6 ms timer, which makes any Sleep()-based pacing overshoot
// the tick by up to that much - the periodic 40 ms gap in a 25 ms cadence. Raising the
// resolution is done through winmm so the build needs no extra import library.
static void enable_fine_timer()
{
    HMODULE wm = LoadLibraryA("winmm.dll");
    if (!wm) { return; }
    using pfn_time_begin = UINT(WINAPI*)(UINT);
    auto begin = reinterpret_cast<pfn_time_begin>(GetProcAddress(wm, "timeBeginPeriod"));
    if (begin && begin(1) == 0) {
        logf_("[bridge] timer resolution raised to 1ms\n");
    }
}

// Cross-adapter fences do not report a value on the consuming node: GetCompletedValue()
// returns UINT64_MAX here (PBZ showed exactly that), which used to poison g_fwd_last and
// make every later wait ask for value 0. Track our own expectation instead - the producer
// signals 1, 2, 3, ... per slot, and SetEventOnCompletion fires immediately when the fence
// is already past the value we ask for.
static UINT64 g_fwd_expect[mgpu::kMaxSlots] = { 1, 1, 1, 1 };

// ---- waiting for a *local* fence without allocating a kernel object every frame ------
// gpu_throttle() runs once per real frame, so CreateEventW + CloseHandle there was 60-120
// object creations per second for no reason. One auto-reset event is reused forever; that
// is safe only because every wait here is on a fence whose GetCompletedValue() is
// meaningful, which lets the loop tell "the event fired" from "the event was still set
// from an earlier timeout":
//   * the fence value is re-checked after every wait, so a late signal can cost one extra
//     iteration but can never make us skip a wait that has not retired yet;
//   * the remaining timeout is recomputed each round, so a spurious wakeup does not turn
//     into a shorter timeout than the caller asked for.
// Everything calling this runs on the frame thread, so one event is shared.
//
// wait_slot() deliberately does NOT use it: its fences are SHARED_CROSS_ADAPTER, where
// GetCompletedValue() reads back UINT64_MAX on this node, so the check that makes the
// reused event safe is unavailable - and a stale signal there would advance
// g_fwd_expect[] past a publication we never saw, i.e. read one slot too old.
static bool wait_fence_value(ID3D12Fence* fence, UINT64 value, DWORD timeout_ms)
{
    if (!fence) { return false; }
    if (fence->GetCompletedValue() >= value) { return true; }
    static HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) {
        // No event: bounded spin. Should never happen, but never wait forever either.
        const ULONGLONG t0 = GetTickCount64();
        while (fence->GetCompletedValue() < value) {
            if (GetTickCount64() - t0 >= timeout_ms) { return false; }
            SwitchToThread();
        }
        return true;
    }
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        if (FAILED(fence->SetEventOnCompletion(value, ev))) { return false; }
        const ULONGLONG spent = GetTickCount64() - t0;
        if (spent >= timeout_ms) { return fence->GetCompletedValue() >= value; }
        WaitForSingleObject(ev, static_cast<DWORD>(timeout_ms - spent));
        if (fence->GetCompletedValue() >= value) { return true; }
        if (GetTickCount64() - t0 >= timeout_ms) { return false; }
    }
}

// Slot selection lives further down (round-robin, see run_frame_ex step 3). Protocol v5
// here used to add a "is this slot GPU-complete?" query on frames[].ready plus
// --mv-newest; both are gone, because publishing `ready` requires polling a
// SHARED_CROSS_ADAPTER fence and four GetCompletedValue() calls per present dragged the
// game from 33 to 60 ms (docs/findings/HANDOFF.md 8.6). The field is still in mgpu_shared.h so the
// add-on/bridge layouts stay identical - nothing reads it.
//
// --depth-lag N: pair the colour with a depth N frames older, applied on OUR side from a
// local ring. The WGC colour is 10-70 ms old (~0.3-2 frames at 30 fps), so the depth that
// belongs to it is usually one publication older - but measured worse anyway, hence 0.
static int g_depth_lag = 0;
// --exit-on-focus-loss N: quit when another application has been in the foreground
// for N seconds (default 5, 0 = never). Without this the topmost overlay survives a
// crashed/closed game and just sits on the desktop forever.
static UINT g_exit_after_s = 5;
static ULONGLONG g_fg_lost_ms = 0;

static void wait_slot(UINT slot)
{
    if (slot >= mgpu::kMaxSlots || !g_fwd[slot]) { return; }
    {
        // One event per call, on purpose: this fence is SHARED_CROSS_ADAPTER, so the
        // GetCompletedValue() re-check that makes a reused event safe is not available
        // here (it reads UINT64_MAX on this node) - see wait_fence_value(). A stale
        // signal would advance g_fwd_expect[] without a publication and make us read a
        // slot one frame too old, which is worse than the ~1 us this costs.
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_fwd[slot]->SetEventOnCompletion(g_fwd_expect[slot], ev);
        // Blocking here shows up as a gap between presents that nothing else explains,
        // so the wait is measured. A timeout just means the producer has not published
        // yet and the previous contents are used.
        const double w0 = qpc_ms();
        const DWORD wr = WaitForSingleObject(ev, g_wait_ms);
        const double w1 = qpc_ms();
        g_ms_wait = g_ms_wait * 0.9 + (w1 - w0) * 0.1;
        if (w1 - w0 > g_max_wait) { g_max_wait = w1 - w0; }
        if (wr == WAIT_OBJECT_0) { ++g_fwd_expect[slot]; }
        else { ++g_slot_timeouts; }
        CloseHandle(ev);
    }
    if (!g_diag_dumped && g_ctl) {
        // One-shot: is the producer publishing at all? If the fences stay at 0 and
        // frame_published never moves, the add-on is not running its present path
        // (no ReShade technique, or its reshade_present callback never fires) and every
        // slot wait is just burning the --wait-ms timeout.
        g_diag_dumped = true;
        logf_("[bridge] diag: frame_published=%llu  fwd=[%llu %llu %llu %llu]"
              "  frames[0]=(id %llu, render %ux%u, mvscale %.6f, %.6f)\n",
              static_cast<unsigned long long>(g_ctl->frame_published),
              static_cast<unsigned long long>(g_fwd[0] ? g_fwd[0]->GetCompletedValue() : 0),
              static_cast<unsigned long long>(g_fwd[1] ? g_fwd[1]->GetCompletedValue() : 0),
              static_cast<unsigned long long>(g_fwd[2] ? g_fwd[2]->GetCompletedValue() : 0),
              static_cast<unsigned long long>(g_fwd[3] ? g_fwd[3]->GetCompletedValue() : 0),
              static_cast<unsigned long long>(g_ctl->frames[0].frame_id),
              g_ctl->frames[0].render_w, g_ctl->frames[0].render_h,
              g_ctl->frames[0].mv_scale_x, g_ctl->frames[0].mv_scale_y);
    }
}

// Local (committed, DEFAULT heap) copies of the producer's MV/depth. XeSS-FG
// cannot work with the imported cross-adapter heap resources: it fails with
// "Failed to allocate memory for a copy of DEPTH" and then reports the motion
// vector resource as missing.
static ComPtr<ID3D12Resource> g_mv_local[mgpu::kMaxSlots];
static ComPtr<ID3D12Resource> g_depth_local[mgpu::kMaxSlots];
// --depth-lag N: our own ring of local depth copies. The shared ring must always be read
// at its newest completed slot (reading an older one collides with the producer's write),
// so the "depth from N frames ago" alignment is done here instead.
static ComPtr<ID3D12Resource> g_depth_ring[mgpu::kMaxSlots];
static UINT g_ring_write = 0;
static UINT g_ring_count = 0;
static UINT g_ring_need  = 1;

// Local (consumer-side) copy of one input stream. Sized to the *content* rectangle, not
// to the slot capacity: XeFG is tagged with `resourceSize = w x h` and samples the whole
// texture, so handing it a padded 3840x2160 buffer would make it read the padding.
// Recreated when the producer publishes a new content size (protocol v9 - no handshake
// needed, the old texture simply stops being referenced).
// Live content size published by the producer (protocol v9): how much of each slot is
// valid. Snapshotted once per frame so MV and depth cannot disagree within one frame if
// the producer publishes a new rectangle mid-frame.
struct InputGeo { UINT mv_w = 0, mv_h = 0, depth_w = 0, depth_h = 0; };
static InputGeo g_geo;

// v10: read one stream's (width,height) through its seqlock. Without it a reader could
// take the new width and the old height (or vice versa) - which is what the old code
// papered over with a clamp.
static bool read_stream_geo(const mgpu::StreamShared& st, UINT* w, UINT* h)
{
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t s0 = st.seq;
        if (s0 & 1u) { continue; }                 // producer is mid-write
        MemoryBarrier();
        const UINT ww = st.width, hh = st.height;
        MemoryBarrier();
        if (st.seq == s0) { *w = ww; *h = hh; return true; }
    }
    return false;
}

static void refresh_input_geo()
{
    if (!g_ctl) { return; }
    // Clamp to the slot capacity: a stale/foreign header must never make us copy out of
    // bounds of the placed view (belt and braces on top of the seqlock).
    auto clamp = [](UINT v, UINT cap) { return (cap && v > cap) ? cap : v; };
    const mgpu::StreamShared& mv = g_ctl->streams[1];
    const mgpu::StreamShared& dp = g_ctl->streams[2];
    UINT mw = mv.width, mh = mv.height, dw = dp.width, dh = dp.height;
    if (!read_stream_geo(mv, &mw, &mh) || !read_stream_geo(dp, &dw, &dh)) {
        static bool logged = false;
        if (!logged) { logged = true; logf_("[bridge] geometry seqlock kept changing - using the last good values\n"); }
        mw = g_geo.mv_w; mh = g_geo.mv_h; dw = g_geo.depth_w; dh = g_geo.depth_h;
    }
    mw = clamp(mw, mv.slot_w);
    mh = clamp(mh, mv.slot_h);
    dw = clamp(dw, dp.slot_w);
    dh = clamp(dh, dp.slot_h);
    if (mw != g_geo.mv_w || mh != g_geo.mv_h ||
        dw != g_geo.depth_w || dh != g_geo.depth_h) {
        logf_("[bridge] input geometry: mv %ux%u, depth %ux%u\n", mw, mh, dw, dh);
        g_geo.mv_w = mw;
        g_geo.mv_h = mh;
        g_geo.depth_w = dw;
        g_geo.depth_h = dh;
    }
}

static bool ensure_local_copy(ComPtr<ID3D12Resource>& dst, ID3D12Resource* src,
                              UINT w, UINT h)
{
    if (w == 0 || h == 0) { return false; }
    if (dst) {
        const D3D12_RESOURCE_DESC have = dst->GetDesc();
        if (have.Width == w && have.Height == h) { return true; }
        dst.Reset();
    }
    D3D12_RESOURCE_DESC d = src->GetDesc();
    d.Width  = w;
    d.Height = h;
    d.Flags  = D3D12_RESOURCE_FLAG_NONE;
    // The producer's textures are row-major because they live on a cross-adapter
    // heap; committed local resources must use the usual (unknown/swizzled) layout.
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    const HRESULT hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(&dst));
    if (FAILED(hr)) {
        logf_("[bridge] local input copy FAILED hr=0x%08lX (format=%u %ux%u)\n",
              static_cast<unsigned long>(hr), static_cast<unsigned>(d.Format),
              static_cast<unsigned>(d.Width), d.Height);
        return false;
    }
    return true;
}

// Copies the live content rectangle out of one of our placed views into a local texture.
// Both sides agree on the rectangle through StreamShared.width/height; the slot capacity
// (and therefore the row pitch) is fixed for the lifetime of the process.
static void copy_content(ID3D12Resource* dst, ID3D12Resource* src, UINT w, UINT h)
{
    if (!dst || !src || w == 0 || h == 0) { return; }
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = dst;
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box{};
    box.left = 0;
    box.top = 0;
    box.front = 0;
    box.right = w;
    box.bottom = h;
    box.back = 1;
    g_list->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
}

static void ensure_dummy_mv(UINT slot);      // declared below (used by --no-mv)
static void ensure_dummy_depth(UINT slot);   // declared below (used by --no-depth)

#if MGPU_FG_OF
static void of_bind_depth(UINT i, ID3D12Resource* res);   // defined with the OF state below
static void of_bind_hist();                              // ditto (history colours)
#endif

static void copy_inputs_local(UINT slot)
{
    if (slot >= mgpu::kMaxSlots) { return; }
    // COMMON is fine on both sides: D3D12 promotes/demotes implicitly for copies.
    if (g_no_mv) {
        // --no-mv: real depth, zeroed motion vectors.
        ensure_dummy_mv(slot);
        if (g_mv_shadow && g_mv[slot]) {
            if (ensure_local_copy(g_mv_shadow_tex[slot], g_mv[slot].Get(),
                                  g_geo.mv_w, g_geo.mv_h)) {
                copy_content(g_mv_shadow_tex[slot].Get(), g_mv[slot].Get(),
                             g_geo.mv_w, g_geo.mv_h);
            }
        }
#if MGPU_FG_OF
    } else if (g_fg_of) {
        // --fg-of: the vectors come from our own flow pass, so the producer's MV
        // stream is neither copied nor trusted (that also removes 16-33 MB/frame
        // of cross-adapter traffic).
#endif
    } else if (g_mv[slot] && ensure_local_copy(g_mv_local[slot], g_mv[slot].Get(),
                                               g_geo.mv_w, g_geo.mv_h)) {
        copy_content(g_mv_local[slot].Get(), g_mv[slot].Get(), g_geo.mv_w, g_geo.mv_h);
    }
    // --no-depth: skip the depth stream. Depth is the bigger half of the cross-adapter
    // traffic at high render resolutions (2160p R32 = 33 MB/frame), and XeFG only uses
    // it for disocclusion clamping - motion vectors carry the interpolation. The tagged
    // resource stays the right size/format, so this isolates the transfer cost.
    if (g_no_depth) {
        ensure_dummy_depth(slot);
    } else if (g_depth[slot]) {
        // Local ring: copy the *newest completed* shared slot (never an older one - the
        // producer would be rewriting it, and two adapters touching the same memory
        // halved both the game and our loop), then hand XeFG a depth from `--depth-lag`
        // frames ago out of our own textures.
        const UINT need = 1u + static_cast<UINT>((g_depth_lag > 0) ? g_depth_lag : 0);
        bool ok = true;
        for (UINT i = 0; i < need; ++i) {
            if (!ensure_local_copy(g_depth_ring[i], g_depth[slot].Get(), g_geo.depth_w, g_geo.depth_h)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            g_ring_write = (g_ring_write + 1) % need;
#if MGPU_FG_OF
            // Only the flow pass reads depth through our own descriptor heap; without it
            // the ring exists purely for --depth-lag.
            if (!g_of_depth_bound[g_ring_write]) {
                of_bind_depth(g_ring_write, g_depth_ring[g_ring_write].Get());
                g_of_depth_bound[g_ring_write] = true;
            }
#endif
            copy_content(g_depth_ring[g_ring_write].Get(), g_depth[slot].Get(),
                         g_geo.depth_w, g_geo.depth_h);
            if (g_ring_count < need) { ++g_ring_count; }
            g_ring_need = need;
        } else if (ensure_local_copy(g_depth_local[slot], g_depth[slot].Get(),
                                     g_geo.depth_w, g_geo.depth_h)) {
            copy_content(g_depth_local[slot].Get(), g_depth[slot].Get(), g_geo.depth_w, g_geo.depth_h);
        }
    }
}

// --no-inputs: still hand XeSS-FG valid resources so the pipeline keeps running,
// but with zero motion vectors and cleared depth, so the peer-access code path is
// completely absent. Interpolation then looks wrong, which is expected.
static void ensure_dummy_mv(UINT slot)
{
    if (slot >= mgpu::kMaxSlots) { return; }
    if (g_mv_local[slot]) { return; }

    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = g_ctl->streams[1].width;
    d.Height = g_ctl->streams[1].height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&g_mv_local[slot])))) {
        logf_("[bridge] dummy MV resource FAILED\n");
        return;
    }
    if (!g_rtv_heap) { return; }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(5) * g_rtv_stride;      // slot 5: dummy MV
    D3D12_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_R16G16_FLOAT;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_device->CreateRenderTargetView(g_mv_local[slot].Get(), &rd, rtv);

    const float zeros[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = g_mv_local[slot].Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_list->ResourceBarrier(1, &b);
    g_list->ClearRenderTargetView(rtv, zeros, 0, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_list->ResourceBarrier(1, &b);
}

static void ensure_dummy_depth(UINT slot)
{
    if (slot >= mgpu::kMaxSlots) { return; }
    if (g_depth_local[slot]) { return; }
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = g_ctl->streams[2].width;
    d.Height = g_ctl->streams[2].height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R32_FLOAT;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;   // so it can be cleared
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&g_depth_local[slot])))) {
        logf_("[bridge] dummy depth resource FAILED\n");
        return;
    }

    // Clear it once. A fresh committed resource is not guaranteed to be zeroed, and
    // uninitialised depth makes XeFG's disocclusion logic reject motion vectors at random -
    // a flat cleared depth behaves like "no depth", which is what this mode means.
    if (g_rtv_heap) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(4) * g_rtv_stride;   // slot 4: reserved for this
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = DXGI_FORMAT_R32_FLOAT;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_device->CreateRenderTargetView(g_depth_local[slot].Get(), &rd, rtv);

        const float zeros[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.pResource = g_depth_local[slot].Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_list->ResourceBarrier(1, &b);
        g_list->ClearRenderTargetView(rtv, zeros, 0, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        g_list->ResourceBarrier(1, &b);
    }
}

static void ensure_dummy_inputs(UINT slot)
{
    if (slot >= mgpu::kMaxSlots) { return; }
    const UINT w = g_ctl->streams[1].width;
    const UINT h = g_ctl->streams[1].height;

    auto make = [&](ComPtr<ID3D12Resource>& res, DXGI_FORMAT fmt) {
        if (res) { return; }
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1;
        hp.VisibleNodeMask = 1;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&res)))) {
            logf_("[bridge] dummy input resource FAILED\n");
        }
    };
    make(g_mv_local[slot], DXGI_FORMAT_R16G16_FLOAT);
    ensure_dummy_depth(slot);
}

// The game (or Windows' fullscreen optimisation) pushes its own window above our
// topmost overlay when it is activated, which hides the frame-generated picture
// behind the game's own. Re-asserting the topmost position puts us back on top.
static ULONGLONG g_last_topmost = 0;
static bool g_zorder_logged = false;

static bool g_clickthrough_logged = false;
static int  g_click_probe = 0;
static ULONGLONG g_click_probe_ms = 0;

// Is that window one of ours (the overlay, the swap-chain window, a helper)?
static bool fg_is_ours(HWND fg)
{
    if (!fg) { return false; }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Is that window part of the shell's own UI (Task View / Alt+Tab / the desktop)?
//
// These take the foreground while the user is still looking at the game. Counting them as
// "the user left the game" made bridge quit after --exit-on-focus-loss seconds whenever
// Win+Tab, Alt+Tab or Win+D was used - which is exactly how a mirror tool must NOT behave.
// Real applications (another game, a browser, the desktop's own windows) are still
// counted, so the "don't sit on the desktop forever" protection keeps working.
static bool fg_is_shell_ui(HWND fg)
{
    if (!fg) { return false; }
    wchar_t cls[64]{};
    if (!GetClassNameW(fg, cls, 63)) { return false; }
    static const wchar_t* const kShellClasses[] = {
        L"Windows.UI.Core.CoreWindow",          // Task View (Win+Tab)
        L"MultitaskingViewFrame",               // Task View, older builds
        L"TaskSwitcherWnd",                     // Alt+Tab
        L"ForegroundStaging",                   // the transient window Alt+Tab/Win+Tab uses
        L"XamlExplorerHostIslandWindow",        // taskbar flyouts / Task View host
        L"Progman",                             // Win+D: "show desktop"
        L"WorkerW",                             // the desktop's wallpaper host
        L"Shell_TrayWnd",                       // the taskbar itself
    };
    for (const wchar_t* c : kShellClasses) {
        if (_wcsicmp(cls, c) == 0) { return true; }
    }
    return false;
}

// Is that window the launcher UI? Opening mgpu_ui.exe to change a setting must not
// count as "the user left the game", otherwise frame generation dies every time.
static bool fg_is_launcher(HWND fg)
{
    if (!fg) { return false; }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) { return false; }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { return false; }
    wchar_t path[MAX_PATH]{};
    DWORD len = MAX_PATH;
    bool match = false;
    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        match = _wcsicmp(base, L"mgpu_ui.exe") == 0;
    }
    CloseHandle(h);
    return match;
}

// ---------------------------------------------------------------------------
// Single-line OSD.
//
// The corner block people see today belongs to RTSS: three lines, a big font and
// numbers that describe the tool, not this pipeline. We already know everything
// worth showing, so draw ONE thin line inside the game window ourselves: GDI text
// into a 32bpp DIB, handed to the compositor with UpdateLayeredWindow (per-pixel
// alpha, premultiplied). Updated from assert_topmost() - 2.5 times a second - and it
// never touches the frame-generation path.
//   --no-osd   turn it off (the UI checkbox "单行 OSD")
// ---------------------------------------------------------------------------
static HWND      g_osd_wnd = nullptr;
static bool      g_no_osd = false;

static LRESULT CALLBACK osd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(h, msg, wp, lp);
}

static bool osd_ensure_window()
{
    if (g_osd_wnd) { return true; }
    static bool class_ready = false;
    if (!class_ready) {
        class_ready = true;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = osd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
        wc.lpszClassName = L"mgpu_osd";
        RegisterClassExW(&wc);
    }
    // Layered + transparent + no-activate: it must never steal a click or the focus
    // (that is the bug we spent the whole day chasing; do not remove these bits).
    g_osd_wnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST,
        L"mgpu_osd", L"", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_osd_wnd) { return false; }
    // No SetLayeredWindowAttributes here: it switches the window to "one alpha for
    // the whole surface" and fights UpdateLayeredWindow()'s per-pixel alpha.
    return true;
}

static void osd_update()
{
    if (g_no_osd || !g_game_wnd || !IsWindow(g_game_wnd)) { return; }
    if (!osd_ensure_window()) { return; }

    // One line, numbers only: "120 fps  x5  in 24.0  gpu 7.9 ms  game 41.2 ms  lock 24"
    wchar_t line[256]{};
    int n = 0;
    const double in_fps = (g_arrival_ms_ema > 0.5) ? 1000.0 / g_arrival_ms_ema : 0.0;
    n = _snwprintf_s(line, 256, _TRUNCATE,
                     L"%d fps   x%u   in %.1f fps   gpu %.1f ms   game %.1f ms",
                     g_show_fps, static_cast<unsigned>(g_fg_interp_actual + 1),
                     in_fps, g_ms_throttle, g_arrival_ms_ema);
    if (n < 0) { n = 0; }
    if (n > 0 && n < 250 && g_lock_fps > 1.0) {
        n += _snwprintf_s(line + n, 256 - n, _TRUNCATE, L"   lock %.0f", g_lock_fps);
    }
    const unsigned drops = g_lock_drops + g_pace_skips;
    if (n > 0 && n < 240 && drops > 0) {
        n += _snwprintf_s(line + n, 256 - n, _TRUNCATE, L"   drop %u", drops);
    }
    if (n <= 0) { return; }

    const UINT dpi = GetDpiForWindow(g_game_wnd);
    const int sc = static_cast<int>(dpi ? dpi : 96);

    // Place it in the game window's top-left corner, 8 px in.
    RECT cr{};
    GetClientRect(g_game_wnd, &cr);
    POINT org{ cr.left, cr.top };
    ClientToScreen(g_game_wnd, &org);
    const int margin = MulDiv(8, sc, 96);
    const int px = org.x + margin;
    const int py = org.y + margin;

    // The numbers change about once a second while this runs every 400 ms. Blitting an
    // identical bitmap forces DWM to recompose the layered window, which kicks the
    // overlay out of its independent flip and shows up as dropped input frames, so
    // skip the redraw when neither the text nor its position changed.
    static wchar_t last_line[256]{};
    static int last_n = -1, last_x = 0, last_y = 0, last_dpi = 0;
    if (n == last_n && px == last_x && py == last_y && sc == last_dpi &&
        wcscmp(last_line, line) == 0) {
        return;
    }
    last_n = n;
    last_x = px;
    last_y = py;
    last_dpi = sc;
    wmemcpy(last_line, line, static_cast<size_t>(n) + 1);

    osd_panel_render(g_osd_wnd, line, n, px, py, sc);
}

static void assert_topmost()
{
    HWND ovl = overlay_wnd();
    if (!ovl) { return; }
    const ULONGLONG now = GetTickCount64();
    if (now - g_last_topmost < 400) { return; }
    g_last_topmost = now;

    // --overlay-hidden: the window must stay invisible and must NOT be forced above the
    // game, otherwise the experiment measures nothing. Only the focus guard still runs.
    if (g_overlay_hidden) {
        focus_game();
        if (g_game_wnd && !IsWindow(g_game_wnd)) {
            logf_("[bridge] the game window disappeared - exiting\n");
            PostQuitMessage(0);
        }
        return;
    }

    // Re-assert the overlay's place in the topmost band, but *below* the OSD window.
    // HWND_TOPMOST means "top of the whole topmost band", so doing it unconditionally
    // every 400 ms put the full-screen overlay above the OSD line until osd_update()
    // pushed the OSD back up - a visible 2.5 Hz flicker. Inserting the overlay after
    // the OSD window keeps a stable order, and skipping the call when the stack has
    // not drifted avoids needless DWM recomposes (those cost the game its flip).
    {
        const HWND want_after = g_osd_wnd ? g_osd_wnd : HWND_TOPMOST;
        const bool already = (want_after == HWND_TOPMOST)
            ? ((GetWindowLongPtrW(ovl, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
            : (GetWindow(ovl, GW_HWNDPREV) == want_after);
        if (!already) {
            SetWindowPos(ovl, want_after, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    // Click-through is not something the library guarantees: check it and put it back.
    {
        const LONG_PTR ex = GetWindowLongPtrW(ovl, GWL_EXSTYLE);
        const bool want_transparent = !g_relay_input;
        const bool has_transparent = (ex & WS_EX_TRANSPARENT) != 0;
        const bool ok = (has_transparent == want_transparent) &&
                        ((ex & WS_EX_NOACTIVATE) != 0);
        if (!ok) {
            LONG_PTR fixed = (ex | WS_EX_NOACTIVATE);
            if (want_transparent) { fixed |= WS_EX_TRANSPARENT; }
            else                  { fixed &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT); }
            SetWindowLongPtrW(ovl, GWL_EXSTYLE, fixed);
            if (!g_clickthrough_logged) {
                g_clickthrough_logged = true;
                logf_("[bridge] overlay had lost WS_EX_TRANSPARENT/NOACTIVATE - re-applied"
                      " (before this, clicks went to the overlay, not the game)\n");
            }
        }
    }

    // Focus guard. Two separate problems live here:
    //  1) If OUR window ever owns the foreground, the game believes it is inactive.
    //     Raw input (attacks) still works, but its UI ignores mouse clicks - that is
    //     exactly the "menus are dead, attacks are fine" report. Hand it back.
    //  2) If a THIRD-PARTY window keeps the foreground, the overlay is not needed any
    //     more: exit after g_exit_after_s seconds instead of staying topmost forever.
    {
        const HWND fg = GetForegroundWindow();
        if (fg_is_ours(fg) && g_game_wnd) {
            focus_game();
            g_fg_lost_ms = 0;
        } else if (fg && fg != g_game_wnd && !fg_is_shell_ui(fg)) {
            // The launcher is exempt (cached: this runs every 400 ms).
            static DWORD launcher_pid = 0;
            static ULONGLONG launcher_check = 0;
            DWORD fpid = 0;
            GetWindowThreadProcessId(fg, &fpid);
            if (fpid != launcher_pid && now - launcher_check > 4000) {
                launcher_check = now;
                launcher_pid = fg_is_launcher(fg) ? fpid : 0;
            }
            if (fpid != 0 && fpid == launcher_pid) {
                g_fg_lost_ms = 0;
            } else if (g_fg_lost_ms == 0) { g_fg_lost_ms = now; }
            else if (g_exit_after_s > 0 && now - g_fg_lost_ms >= g_exit_after_s * 1000ull) {
                logf_("[bridge] another window has been in front for %us - exiting\n",
                      g_exit_after_s);
                PostQuitMessage(0);
            }
        } else {
            g_fg_lost_ms = 0;
        }
        if (g_game_wnd && !IsWindow(g_game_wnd)) {
            logf_("[bridge] the game window disappeared - exiting\n");
            PostQuitMessage(0);
        }
    }

    // Click probe. WindowFromPoint() skips WS_EX_TRANSPARENT windows, i.e. it answers
    // exactly the question "where would this click land?". If the answer is not the
    // game, our overlay (or a helper window of ours) is eating the input no matter
    // what the ex-styles claim, and the log tells us which one.
    //
    // It keeps sampling for the whole session (every 30 s after the first 12 probes): an
    // input regression that only shows up after minutes of play is exactly the kind of bug
    // that cannot be diagnosed from a 30 s test run.
    if (g_game_wnd && (g_click_probe < 12 || now - g_click_probe_ms > 30000)) {
        POINT pt{};
        GetCursorPos(&pt);
        RECT gr{};
        GetWindowRect(g_game_wnd, &gr);
        if (PtInRect(&gr, pt)) {
            HWND under = WindowFromPoint(pt);
            HWND root = under ? GetAncestor(under, GA_ROOT) : nullptr;
            DWORD upid = 0;
            if (under) { GetWindowThreadProcessId(under, &upid); }
            ++g_click_probe;
            g_click_probe_ms = now;
            const char* verdict = "  <-- NOT the game";
            if (root == g_game_wnd) { verdict = "  (ok)"; }
            else if (root == ovl) { verdict = "  <== OUR OVERLAY eats this click"; }
            else if (g_osd_wnd && root == g_osd_wnd) { verdict = "  <== the OSD window eats it"; }
            logf_("[bridge] click probe: cursor %ld,%ld under=%p pid=%lu root=%p game=%p%s\n",
                  pt.x, pt.y, reinterpret_cast<void*>(under), static_cast<unsigned long>(upid),
                  reinterpret_cast<void*>(root), reinterpret_cast<void*>(g_game_wnd), verdict);
        } else {
            g_click_probe_ms = now;      // cursor is outside the game: say nothing
        }
    }

    // Put the game window directly underneath us. Windows only promotes a window to
    // a direct-flip layer when it is the topmost visible thing on the monitor; with
    // the game tucked below our overlay, normal DWM composition is used and the
    // frame-generated picture stays visible.
    if (g_game_wnd && GetWindow(ovl, GW_HWNDNEXT) != g_game_wnd) {
        SetWindowPos(g_game_wnd, ovl, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else if (!g_zorder_logged) {
        g_zorder_logged = true;
        logf_("[bridge] z-order: game window is directly below the overlay\n");
    }

    // Same 400 ms cadence as everything else here.
    osd_update();
}

// Dumps the real window stacking order: tells us whether the game is genuinely
// above our overlay in the z-order (software problem) or whether the game is
// scanning out directly while our window sits on top (DWM/direct-flip problem).
static void dump_zorder()
{
    logf_("[bridge] zorder: fg=%p game=%p overlay=%p overlay_visible=%d overlay_topmost=%d\n",
          reinterpret_cast<void*>(GetForegroundWindow()),
          reinterpret_cast<void*>(g_game_wnd),
          reinterpret_cast<void*>(overlay_wnd()),
          overlay_wnd() ? IsWindowVisible(overlay_wnd()) : 0,
          (overlay_wnd() && (GetWindowLongPtrW(overlay_wnd(), GWL_EXSTYLE) & WS_EX_TOPMOST)) ? 1 : 0);

    HWND w = GetTopWindow(nullptr);
    for (int i = 0; i < 8 && w; ++i) {
        wchar_t cls[64]{};
        GetClassNameW(w, cls, 64);
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        RECT r{};
        GetWindowRect(w, &r);
        logf_("[bridge]   z#%d %p pid=%u vis=%d %ldx%ld cls=%ls%s\n", i,
              reinterpret_cast<void*>(w), pid, IsWindowVisible(w),
              r.right - r.left, r.bottom - r.top, cls,
              (w == overlay_wnd()) ? "  <== OVERLAY" : (w == g_game_wnd ? "  <== GAME" : ""));
        w = GetWindow(w, GW_HWNDNEXT);
    }
}

// Keeps at most one frame in flight on the GPU. Without this the queue builds up and
// every Present waits for work submitted several frames earlier, which is felt as
// input latency. It also measures the real GPU time per frame.
static void gpu_throttle()
{
    if (!g_frame_fence) { return; }
    // Depth 1: wait for the previous frame's signal to retire before starting new work.
    const UINT64 target = g_frame_fence_value;
    if (target == 0 || g_frame_fence->GetCompletedValue() >= target) { return; }

    // No per-frame event: see wait_fence_value(). QPC, not GetTickCount64, because this
    // number is reported as `gpu` and feeds `load` - a 15.6 ms clock quantised it to
    // 0/16 ms and made both look like noise.
    const double t0 = qpc_ms();
    wait_fence_value(g_frame_fence.Get(), target, 200);
    g_ms_throttle = g_ms_throttle * 0.9 + (qpc_ms() - t0) * 0.1;
}

// ------------------------- our own interpolator (--fg warp) -------------------
//
// XeSS-FG interpolates at the swap chain size, so presenting native 5K cost 12-13 ms
// on the 6750 GRE and dragged the input rate down to ~27 fps. Lossless Scaling avoids
// that by analysing motion at a low resolution and warping at native resolution. We
// can skip the analysis completely: the game already hands us per-pixel motion
// vectors, so a single warp of two native 5K frames is all that is needed. Nothing in
// this path is ever scaled, so the output keeps the render card's 5K detail.
//
// Per input frame: 5K copy + 5K warp + two 5K colour conversions, two presents.
// -----------------------------------------------------------------------------

static bool  g_fg_warp     = false;
static bool  g_warp_mv_logged = false;
static float g_mv_mul      = 0.0f;   // --mv-mul <f>: force the MV scale (UV units)
static float g_mv_sign     = 1.0f;   // --mv-flip: use -MV
static float g_warp_clamp  = 1.0f;   // --warp-clamp <f>: 0 disables the neighbour clamp
static float g_warp_conf   = 90.0f;  // --warp-conf <f>: MV-consistency sensitivity
static float g_warp_conf_on = 0.98f; // run the pixel search below this confidence
static float g_warp_search  = 2.0f;  // --warp-search <f>: search radius in MV texels

static ComPtr<ID3D12Resource> g_hist[2];
static D3D12_RESOURCE_STATES  g_hist_state[2] = { D3D12_RESOURCE_STATE_COMMON,
                                                  D3D12_RESOURCE_STATE_COMMON };
static ComPtr<ID3D12Resource> g_interp;
static D3D12_RESOURCE_STATES  g_interp_state = D3D12_RESOURCE_STATE_COMMON;
static ComPtr<ID3D12DescriptorHeap> g_interp_rtv;
static ComPtr<ID3D12DescriptorHeap> g_srv_interp;
static ComPtr<ID3D12DescriptorHeap> g_warp_srv;
static ComPtr<ID3D12RootSignature>  g_warp_root;
static ComPtr<ID3D12PipelineState>  g_warp_pso;
static UINT  g_hist_write = 0;
static bool  g_hist_valid = false;

static void barrier_to(ID3D12Resource* res, D3D12_RESOURCE_STATES& state,
                       D3D12_RESOURCE_STATES next)
{
    if (!res || state == next) { return; }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = next;
    g_list->ResourceBarrier(1, &b);
    state = next;
}

static const char* k_warp_hlsl = R"HLSL(
Texture2D<float4> tA   : register(t0);      // one history frame
Texture2D<float4> tB   : register(t1);      // the other history frame
Texture2D<float2> tMv  : register(t2);
SamplerState      smp  : register(s0);
// aIsCurrent: 1 when tA holds the newer frame. Both frames stay at fixed descriptors
// so nothing has to be rewritten while a previous list may still be running.
cbuffer P : register(b0) {
    float2 mvScale;     // MV units -> UV delta
    float  aIsCurrent;  // 1 when tA holds the newer frame
    float  clampK;      // neighbourhood clamp strength
    float  confGain;    // per-UV forward/backward error that zeroes the history
    float  confOn;      // below this confidence the local pixel search runs
    float  searchR;     // search radius in motion-vector texels
    float  pad0;
};

static const float2 kOffsets[8] = {
    float2( 1.0,  0.0), float2(-1.0,  0.0), float2( 0.0,  1.0), float2( 0.0, -1.0),
    float2( 1.0,  1.0), float2(-1.0,  1.0), float2( 1.0, -1.0), float2(-1.0, -1.0),
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut vs_main(uint id : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((id == 2) ? 2.0 : 0.0, (id == 1) ? 2.0 : 0.0);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// The conditional operator cannot select between texture objects, so the branch
// has to happen on the sampled value instead.
float3 cur_at(float2 uv)
{
    return (aIsCurrent > 0.5) ? tA.SampleLevel(smp, uv, 0).rgb
                              : tB.SampleLevel(smp, uv, 0).rgb;
}
float3 prev_at(float2 uv)
{
    return (aIsCurrent > 0.5) ? tB.SampleLevel(smp, uv, 0).rgb
                              : tA.SampleLevel(smp, uv, 0).rgb;
}

float4 ps_main(VSOut i) : SV_Target
{
    // The game's motion vectors become a UV delta through the scale it hands NGX.
    float2 mv = tMv.SampleLevel(smp, i.uv, 0) * mvScale;
    // A wrong scale or a hard camera cut would smear the whole screen; a quarter of
    // the screen per frame is already far beyond anything real.
    mv = clamp(mv, -0.25, 0.25);

    float2 pPrev = i.uv - mv * 0.5;     // where this pixel was half a frame ago
    float2 pCur  = i.uv + mv * 0.5;

    float3 a = prev_at(pPrev);
    float3 b = cur_at(pCur);

    // Forward/backward consistency. The MV sampled where the history landed should
    // point back at this pixel; when it does not, the history is untrustworthy - the
    // surface was just revealed, or the content has no motion vector at all (particles,
    // reflections, screen-space effects).
    float2 mvBack = tMv.SampleLevel(smp, pPrev, 0) * mvScale;
    float  fbErr  = length(mvBack + mv);
    float  conf   = saturate(1.0 - fbErr * confGain);

    uint tw = 1u, th = 1u;
    tA.GetDimensions(tw, th);
    float2 tx = float2(1.0, 1.0) / float2(tw, th);

    // Pixel-domain fallback: where the engine's motion vector is not trustworthy the
    // content has no motion vector at all, so search the previous frame locally for the
    // patch that actually matches the current one. This is the cheap version of what a
    // learned optical-flow net does, and it is applied per pixel only when MV fails, so
    // geometry keeps the exact engine motion. Without it, rain, smoke, water, reflections
    // and screen-space effects smear.
    if (conf < confOn) {
        float3 best    = a;
        float  bestErr = length(a - b);
        [unroll]
        for (int k = 0; k < 8; ++k) {
            float2 o = kOffsets[k] * tx * searchR;
            float3 cand = prev_at(pPrev + o);
            float  e    = length(cand - b);
            if (e < bestErr) { bestErr = e; best = cand; }
        }
        a    = best;
        // a good match restores confidence in the history
        conf = saturate(1.0 - bestErr * confGain * 0.5);
    }

    // Clamp the reprojected history to the current neighbourhood. Without this a
    // background pixel revealed by a moving object smears its old colour forward.
    float3 n0 = cur_at(pCur + float2( tx.x, 0.0));
    float3 n1 = cur_at(pCur + float2(-tx.x, 0.0));
    float3 n2 = cur_at(pCur + float2(0.0,  tx.y));
    float3 n3 = cur_at(pCur + float2(0.0, -tx.y));
    float3 lo = min(min(n0, n1), min(n2, n3));
    float3 hi = max(max(n0, n1), max(n2, n3));
    a = lerp(a, clamp(a, min(lo, b), max(hi, b)), clampK);

    // Photometric disagreement the clamp could not explain: the history is simply
    // wrong here, so take it out of the blend.
    float3 rel   = abs(a - b) / (0.05 + abs(a) + abs(b));
    float  photo = saturate(max(rel.r, max(rel.g, rel.b)));
    conf *= 1.0 - photo;

    return float4(lerp(b, a, 0.5 * conf), 1.0);   // midpoint of the two real frames
}
)HLSL";

static void make_srv(ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE where, DXGI_FORMAT fmt)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = fmt;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(res, &srv, where);
}

static bool init_warp()
{
    const UINT w = g_width, h = g_height;

    auto make_tex = [&](ComPtr<ID3D12Resource>& res) -> HRESULT {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1;
        hp.VisibleNodeMask = 1;
        return g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&res));
    };

    if (FAILED(make_tex(g_hist[0])) || FAILED(make_tex(g_hist[1])) ||
        FAILED(make_tex(g_interp))) {
        logf_("[bridge] --fg warp: history textures FAILED (%ux%u)\n", w, h);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = 3;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_warp_srv)))) {
        return false;
    }
    const UINT srv_stride = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE base = g_warp_srv->GetCPUDescriptorHandleForHeapStart();
    make_srv(g_hist[0].Get(), base, DXGI_FORMAT_R16G16B16A16_FLOAT);
    D3D12_CPU_DESCRIPTOR_HANDLE d1 = base;
    d1.ptr += srv_stride;
    make_srv(g_hist[1].Get(), d1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    // Slot 2 (motion vectors) is written each frame for whichever slot is in use.

    sh.NumDescriptors = 1;
    if (FAILED(g_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srv_interp)))) {
        return false;
    }
    make_srv(g_interp.Get(), g_srv_interp->GetCPUDescriptorHandleForHeapStart(),
             DXGI_FORMAT_R16G16B16A16_FLOAT);

    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = 1;
    if (FAILED(g_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_interp_rtv)))) {
        return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_device->CreateRenderTargetView(g_interp.Get(), &rtv,
                                     g_interp_rtv->GetCPUDescriptorHandleForHeapStart());

    logf_("[bridge] --fg warp: %ux%u history + interpolation targets ready\n", w, h);
    return true;
}

static void warp_convert(ID3D12Resource* bb, UINT bbi, ID3D12DescriptorHeap* srv_heap);
static bool init_warp_pso()
{
    ComPtr<ID3DBlob> vs, ps, err;
    HRESULT hr = D3DCompile(k_warp_hlsl, strlen(k_warp_hlsl), "warp.hlsl", nullptr, nullptr,
                            "vs_main", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) {
        logf_("[bridge] warp VS FAILED: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    hr = D3DCompile(k_warp_hlsl, strlen(k_warp_hlsl), "warp.hlsl", nullptr, nullptr,
                    "ps_main", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr)) {
        logf_("[bridge] warp PS FAILED: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 3;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 12;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;

    ComPtr<ID3DBlob> sig, serr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &serr);
    if (FAILED(hr)) { logf_("[bridge] warp SerializeRootSignature FAILED\n"); return false; }
    hr = g_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                       IID_PPV_ARGS(&g_warp_root));
    if (FAILED(hr)) { logf_("[bridge] warp CreateRootSignature FAILED\n"); return false; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_warp_root.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = 0xFFFFFFFFu;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.SampleDesc.Count = 1;
    hr = g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_warp_pso));
    if (FAILED(hr)) {
        logf_("[bridge] warp CreateGraphicsPipelineState FAILED hr=0x%08lX\n",
              static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

// ------------------------------------------------------- resize filter constants
// Taps that cover 2*support at the current scale, so the same kernel works for
// 1.33x (5K -> 4K) and 2x (5K -> 1440p) without touching the shader.
static void conv_constants(float* cc, UINT dst_w, UINT dst_h)
{
    float taps = 1.0f;
    float down = static_cast<float>(g_down);
    float support = g_down_support;
    if (g_down_support > 0.0f && dst_w > 0 && dst_h > 0 && g_width > 0 && g_height > 0) {
        const float sx = static_cast<float>(g_width) / static_cast<float>(dst_w);
        const float sy = static_cast<float>(g_height) / static_cast<float>(dst_h);
        if (fabsf(sx - 1.0f) < 1e-3f && fabsf(sy - 1.0f) < 1e-3f) {
            // 1:1 (4K capture -> 4K present): the resize is an identity copy, so the
            // single tap is exact. Paying for a 7x7 kernel here would only cost time,
            // and any kernel rounding would soften a picture that needs no resampling.
            down = 0.0f;
            support = 0.0f;
            static bool logged = false;
            if (!logged) {
                logged = true;
                logf_("[bridge] resize filter: not needed - %ux%u -> %ux%u is 1:1\n",
                      g_width, g_height, dst_w, dst_h);
            }
        } else {
            const float s = (sx > sy) ? sx : sy;
            taps = ceilf(2.0f * g_down_support * s) + 1.0f;
            if (taps < 2.0f) { taps = 2.0f; }
            if (taps > 16.0f) { taps = 16.0f; }
        }
    }
    cc[0] = 80.0f;                                   // scRGB 1.0 = 80 nits
    cc[1] = static_cast<float>(g_colour_mode);
    cc[2] = g_sharpen;
    cc[3] = down;
    cc[4] = support;
    cc[5] = taps;
    cc[6] = static_cast<float>(dst_w);
    cc[7] = static_cast<float>(dst_h);
    cc[8] = cc[9] = cc[10] = cc[11] = 0.0f;
}

static void warp_convert(ID3D12Resource* bb, UINT bbi, ID3D12DescriptorHeap* srv_heap)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(bbi & 3) * g_rtv_stride;
    if (!g_rtv_res[bbi & 3]) {
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_device->CreateRenderTargetView(bb, &rd, rtv);
        g_rtv_res[bbi & 3] = bb;
    }

    D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(g_full_w),
                       static_cast<float>(g_full_h), 0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(g_full_w), static_cast<LONG>(g_full_h) };
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &scissor);
    g_list->SetGraphicsRootSignature(g_conv_root.Get());
    g_list->SetPipelineState(g_conv_pso.Get());
    ID3D12DescriptorHeap* heaps[] = { srv_heap };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetGraphicsRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
    float cc[12];
    conv_constants(cc, g_full_w, g_full_h);
    g_list->SetGraphicsRoot32BitConstants(1, 12, cc, 0);
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->DrawInstanced(3, 1, 0, 0);
}

static bool service_shared_layout();

static void run_frame_warp(ID3D11Texture2D* colour11)
{
    if (!service_shared_layout()) { return; }
    ++g_present_id;
    if (g_no_throttle) { g_ms_throttle = 0.0; } else { gpu_throttle(); }
    const double t_work0 = qpc_ms();

    if (g_colour_mutex) { g_colour_mutex->AcquireSync(0, 20); }
    g_ctx11->CopyResource(g_shared11.Get(), colour11);
    if (g_colour_mutex) { g_colour_mutex->ReleaseSync(0); }
    const UINT64 fv = ++g_fence11_value;
    g_ctx11_4->Signal(g_fence11.Get(), fv);
    g_queue->Wait(g_fence12.Get(), fv);

    const UINT slots = (g_ctl->slots != 0) ? g_ctl->slots : 1;
    const UINT slot = g_mv_slot % slots;
    wait_slot(slot);

    // NGX's MV.Scale is the number of motion-vector units per screen UV, i.e. the
    // game reports its render size here (2560x1440 for a 5K DLSS Performance run), so
    // the delta is mv / scale. A zero value means "no scale given", in which case the
    // same formula falls back to 1/render-size.
    const mgpu::FrameShared& fr = g_ctl->frames[slot];
    const float rw = static_cast<float>(g_ctl->streams[1].width ? g_ctl->streams[1].width : 1);
    const float rh = static_cast<float>(g_ctl->streams[1].height ? g_ctl->streams[1].height : 1);
    float msx = (fr.mv_scale_x > 0.0f) ? (1.0f / fr.mv_scale_x) : (1.0f / rw);
    float msy = (fr.mv_scale_y > 0.0f) ? (1.0f / fr.mv_scale_y) : (1.0f / rh);
    if (g_mv_mul > 0.0f) { msx = g_mv_mul; msy = g_mv_mul; }
    msx *= g_mv_sign;
    msy *= g_mv_sign;
    if (!g_warp_mv_logged) {
        g_warp_mv_logged = true;
        logf_("[bridge] warp: mv scale=(%.8f, %.8f) (game said %.8f, %.8f) clamp=%.2f\n",
              msx, msy, fr.mv_scale_x, fr.mv_scale_y, g_warp_clamp);
    }

    const UINT wi = g_hist_write;
    const UINT pi = 1u - wi;
    const bool interp_ok = g_hist_valid;

    g_alloc->Reset();
    g_list->Reset(g_alloc.Get(), nullptr);

    // --- list 1: history update, then the interpolated (midpoint) frame --------
    D3D12_RESOURCE_STATES shared_state = D3D12_RESOURCE_STATE_COMMON;
    barrier_to(g_shared12.Get(), shared_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier_to(g_hist[wi].Get(), g_hist_state[wi], D3D12_RESOURCE_STATE_COPY_DEST);
    g_list->CopyResource(g_hist[wi].Get(), g_shared12.Get());
    barrier_to(g_hist[wi].Get(), g_hist_state[wi], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barrier_to(g_shared12.Get(), shared_state, D3D12_RESOURCE_STATE_COMMON);

    ID3D12Resource* bb1 = nullptr;
    UINT bbi1 = 0;
    bool have_bb1 = false;

    if (interp_ok) {
        if (g_no_inputs) { ensure_dummy_inputs(slot); } else { copy_inputs_local(slot); }

        // Motion vectors occupy slot 2 of the warp heap. That slot was last used two
        // frames ago and we waited for the previous frame, so rewriting it is safe.
        const UINT srv_stride = g_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE mv_where = g_warp_srv->GetCPUDescriptorHandleForHeapStart();
        mv_where.ptr += 2ull * srv_stride;
        if (g_mv_local[slot]) {
            make_srv(g_mv_local[slot].Get(), mv_where, DXGI_FORMAT_R16G16_FLOAT);
        } else {
            make_srv(g_hist[0].Get(), mv_where, DXGI_FORMAT_R16G16B16A16_FLOAT);
        }

        barrier_to(g_interp.Get(), g_interp_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE irtv = g_interp_rtv->GetCPUDescriptorHandleForHeapStart();
        D3D12_VIEWPORT wvp{ 0.0f, 0.0f, static_cast<float>(g_width),
                            static_cast<float>(g_height), 0.0f, 1.0f };
        D3D12_RECT wsc{ 0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height) };
        g_list->OMSetRenderTargets(1, &irtv, FALSE, nullptr);
        g_list->RSSetViewports(1, &wvp);
        g_list->RSSetScissorRects(1, &wsc);
        g_list->SetGraphicsRootSignature(g_warp_root.Get());
        g_list->SetPipelineState(g_warp_pso.Get());
        ID3D12DescriptorHeap* wheaps[] = { g_warp_srv.Get() };
        g_list->SetDescriptorHeaps(1, wheaps);
        g_list->SetGraphicsRootDescriptorTable(0, g_warp_srv->GetGPUDescriptorHandleForHeapStart());
        // hist[wi] is the newest frame; it sits in descriptor 0 only when wi == 0
        const float wc[8] = { msx, msy, (wi == 0) ? 1.0f : 0.0f, g_warp_clamp,
                              g_warp_conf, g_warp_conf_on, g_warp_search, 0.0f };
        g_list->SetGraphicsRoot32BitConstants(1, 8, wc, 0);
        g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_list->DrawInstanced(3, 1, 0, 0);
        barrier_to(g_interp.Get(), g_interp_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Present the midpoint first so the display order is prev -> mid -> current.
        bbi1 = g_swapchain->GetCurrentBackBufferIndex();
        have_bb1 = back_buffer(bbi1, &bb1);
        if (have_bb1) {
            barrier_to(bb1, g_bb_state[bbi1 & 3], D3D12_RESOURCE_STATE_RENDER_TARGET);
            warp_convert(bb1, bbi1, g_srv_interp.Get());
            barrier_to(bb1, g_bb_state[bbi1 & 3], D3D12_RESOURCE_STATE_PRESENT);
        }
    }

    g_list->Close();
    {
        ID3D12CommandList* lists[] = { g_list.Get() };
        g_queue->ExecuteCommandLists(1, lists);
        g_queue->Signal(g_frame_fence.Get(), ++g_frame_fence_value);
    }

    if (have_bb1) {
        // QPC here: g_present_ms is reported and used by the "display paces the loop"
        // warning, and GetTickCount64's 15.6 ms granularity made it read 0 or 16.
        const double t_p = qpc_ms();
        g_swapchain->Present(0, g_present_flags());
        const double pres_ms = qpc_ms() - t_p;
        g_present_ms = static_cast<DWORD>(pres_ms + 0.5);
        g_ms_present = g_ms_present * 0.9 + pres_ms * 0.1;

        // Half a tick later the real frame goes out. XeSS-FG decides this timing
        // internally for its own path (and gets it wrong on non-Intel GPUs), but here
        // we own Present, so the interpolated frame and the real frame land one half
        // tick apart - exactly where they belong in time.
        if (g_pace_ms > 2.0) {
            const double mid = qpc_ms() + g_pace_ms * 0.5;
            while (qpc_ms() < mid) { Sleep(0); }
        }

        // --- list 2: the real frame, which has to go out after the midpoint -----
        g_alloc->Reset();
        g_list->Reset(g_alloc.Get(), nullptr);
    }

    const UINT bbi2 = g_swapchain->GetCurrentBackBufferIndex();
    ID3D12Resource* bb2 = nullptr;
    const bool have_bb2 = back_buffer(bbi2, &bb2);
    if (have_bb2) {
        barrier_to(bb2, g_bb_state[bbi2 & 3], D3D12_RESOURCE_STATE_RENDER_TARGET);
        warp_convert(bb2, bbi2, g_srv_heap.Get());
        barrier_to(bb2, g_bb_state[bbi2 & 3], D3D12_RESOURCE_STATE_PRESENT);
    }
    g_list->Close();
    {
        ID3D12CommandList* lists[] = { g_list.Get() };
        g_queue->ExecuteCommandLists(1, lists);
        g_queue->Signal(g_frame_fence.Get(), ++g_frame_fence_value);
    }
    if (have_bb2) {
        const double t_p = qpc_ms();
        g_swapchain->Present(0, g_present_flags());
        const double pres_ms = qpc_ms() - t_p;
        g_present_ms = static_cast<DWORD>(pres_ms + 0.5);
        g_ms_present = g_ms_present * 0.9 + pres_ms * 0.1;
    }

    g_hist_write = pi;
    g_hist_valid = true;
    g_mv_slot = (g_mv_slot + 1) % slots;
    ++g_frames;
    g_ms_work = g_ms_work * 0.9 + (qpc_ms() - t_work0) * 0.1;

    if ((g_frames % g_report_every) == 1) {
        const ULONGLONG now_ms = GetTickCount64();
        const double win = (g_t_report && now_ms > g_t_report)
                         ? (now_ms - g_t_report) / 1000.0 : 0.0;
        g_t_report = now_ms;
        const double secs = (now_ms / 1000.0) - g_t0;
        const double in_fps = (secs > 0) ? g_frames / secs : 0.0;
        g_show_fps = static_cast<int>(in_fps * 2.0 + 0.5);
        logf_("[bridge] warp frame %llu  slot %u  (%.1f fps in)  present=%ums"
              "  bb=%ux%u  out=2x  show=%d\n",
              (unsigned long long)g_frames, slot, in_fps,
              g_present_ms, g_full_w, g_full_h, g_show_fps);
        logf_("[bridge]   breakdown: capture=%.1fms work=%.1fms present=%.1fms gpu=%.1fms"
              " timeouts=%u mode=%d\n", g_ms_capture, g_ms_work, g_ms_present,
              g_ms_throttle, g_slot_timeouts, g_colour_mode);
    }

    assert_topmost();
}

// --pace: re-time our presents to a fixed cadence. The game hands us frames at an
// alternating 16.7/33.3 ms pattern (RTSS lands off the 120 Hz grid), and XeSS-FG
// assumes an even input cadence, so the output judders even though nothing is dropped.
// Sampling "the newest frame that exists at tick N" turns that into a steady 80 Hz.
// (The globals live above run_frame_warp, which needs g_pace_ms for its own presents.)

static double qpc_ms()
{
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// ====================== --dump: raw frame export for analysis ================
// Writes the exact colour / depth / MV resources we hand to XeSS-FG to disk so
// the pairing question ("does this depth/MV belong to the same game frame as
// this colour?") can be answered offline instead of by eye.
//
//     <dir>\f<NN>_colour.raw   RGBA16F, the captured (WGC) colour
//     <dir>\f<NN>_depth.raw    R32_FLOAT, the depth we tag
//     <dir>\f<NN>_mv.raw       R16G16_FLOAT, the MV we tag
//     <dir>\f<NN>_meta.txt     sizes/format/slot/frame_id/jitter/...
//
// Header of every .raw = 4 x uint32 { width, height, dxgiFormat, rowPitchBytes }
// then the rows at their natural pitch (copy padding stripped), so the file is
// exactly width*height*bytesPerPixel + 16 bytes.
//
// The dump waits for the GPU, so the frames it writes have distorted timings -
// that is intentional, only the data matters.
static int  g_dump_frames = 0;                              // --dump N
static int  g_dump_index  = 0;
static int  g_dump_after  = 60;    // --dump-after N: skip the settle-in frames
static char g_dump_dir[MAX_PATH] = "dump";   // --dump-dir DIR (relative to the CWD by default)

// One stream's readback target. The copy is recorded inside the frame's own command
// list (so it is ordered with the MV/depth copies that feed XeSS-FG) and only
// mapped once the whole dump is complete. The first version waited for the GPU
// inside the frame loop instead, which put five game frames between consecutive
// dumped frames - far too coarse to answer "one frame ahead or one behind".
struct DumpStream
{
    ComPtr<ID3D12Resource> rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT   rows = 0;
    UINT64 row_size = 0;
    D3D12_RESOURCE_DESC desc{};
};

struct DumpEntry
{
    DumpStream s[3];                 // 0 = colour, 1 = depth, 2 = mv
    bool have[3] = { false, false, false };   // which streams had a resource
    bool recorded = false;
    // The shared per-frame record changes every frame, so snapshot it now.
    uint64_t bridge_frame = 0, present_id = 0, tick_ms = 0, producer_frame_id = 0;
    uint32_t slot = 0, render_w = 0, render_h = 0;
    float    jitter_x = 0.0f, jitter_y = 0.0f, mv_scale_x = 0.0f, mv_scale_y = 0.0f;
    uint32_t stream_mv_w = 0, stream_mv_h = 0, stream_depth_w = 0, stream_depth_h = 0;
    float    fov = 0.0f;
    uint32_t no_mv = 0, no_depth = 0, inputs_on = 1, proj_rz = 0;
};

static std::vector<DumpEntry> g_dump_entries;
static bool g_dump_flushed = false;

static bool dump_prepare(DumpStream& s, ID3D12Resource* res, int which)
{
    static bool warned[3] = { false, false, false };
    auto bail = [&](const char* why) {
        if (!warned[which & 3]) {
            warned[which & 3] = true;
            logf_("[bridge] dump: stream %d skipped (%s)\n", which, why);
        }
        return false;
    };
    if (!res) { return bail("no resource this frame"); }
    s.desc = res->GetDesc();
    if (s.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) { return bail("not a texture2D"); }
    UINT64 total = 0;
    g_device->GetCopyableFootprints(&s.desc, 0, 1, 0, &s.fp, &s.rows, &s.row_size, &total);
    if (total == 0 || s.rows == 0) { return bail("GetCopyableFootprints returned nothing"); }

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    const HRESULT hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&s.rb));
    if (FAILED(hr)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "readback buffer failed hr=0x%08lX (%.1f MB)",
                 static_cast<unsigned long>(hr), static_cast<double>(total) / (1024.0 * 1024.0));
        return bail(msg);
    }
    return true;
}

// Records the three readback copies for this frame into the caller's command list.
static void dump_record(ID3D12GraphicsCommandList* list, UINT slot, UINT64 present_id)
{
    if (g_dump_frames <= 0 || g_dump_index >= g_dump_frames || g_dump_flushed) { return; }
    // Loading screens and menus come first; only export settled gameplay frames.
    if (g_frames < static_cast<UINT64>(g_dump_after)) { return; }
    if (g_dump_entries.empty()) { g_dump_entries.resize(static_cast<size_t>(g_dump_frames)); }

    DumpEntry& e = g_dump_entries[static_cast<size_t>(g_dump_index)];
    // Depth normally lives in the local RING (the newest entry is what was handed to XeFG),
    // and g_depth_local[slot] is only used as a fallback when the ring could not be built -
    // reading the wrong one made every dump report depth=0 even with a healthy depth stream.
    ID3D12Resource* depth_res = nullptr;
    if (g_ring_count > 0 && g_depth_ring[g_ring_write]) {
        depth_res = g_depth_ring[g_ring_write].Get();
    } else if (slot < mgpu::kMaxSlots && g_depth_local[slot]) {
        depth_res = g_depth_local[slot].Get();
    }
    ID3D12Resource* res[3] = { g_shared12.Get(),
                               depth_res,
                               (slot < mgpu::kMaxSlots) ? g_mv_local[slot].Get() : nullptr };

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    for (int i = 0; i < 3; ++i) {
        // A missing stream (XeSS publishes no depth, --no-depth, --no-mv before the dummy
        // exists) must not throw the whole frame away - that is why `--dump` used to leave
        // an empty directory with no explanation at all.
        if (!dump_prepare(e.s[i], res[i], i)) { continue; }
        e.have[i] = true;
        b.Transition.pResource = res[i];
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = res[i];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = e.s[i].rb.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = e.s[i].fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &b);
    }

    const mgpu::FrameShared& fr = g_ctl->frames[slot];
    e.recorded = (e.have[0] || e.have[1] || e.have[2]);   // at least one usable stream
    e.bridge_frame = g_frames;
    e.present_id = present_id;
    e.tick_ms = GetTickCount64();
    e.slot = slot;
    e.producer_frame_id = fr.frame_id;
    e.render_w = fr.render_w;
    e.render_h = fr.render_h;
    e.jitter_x = fr.jitter_x;
    e.jitter_y = fr.jitter_y;
    e.mv_scale_x = fr.mv_scale_x;
    e.mv_scale_y = fr.mv_scale_y;
    e.stream_mv_w = g_ctl->streams[1].width;
    e.stream_mv_h = g_ctl->streams[1].height;
    e.stream_depth_w = g_ctl->streams[2].width;
    e.stream_depth_h = g_ctl->streams[2].height;
    e.fov = g_fg_fov_y;
    e.no_mv = g_no_mv ? 1 : 0;
    e.no_depth = g_no_depth ? 1 : 0;
    e.inputs_on = g_no_inputs ? 0 : 1;
    e.proj_rz = g_fg_proj_rz ? 1 : 0;
    ++g_dump_index;
}

static void run_frame_ex(ID3D11Texture2D* colour11);

// ---- --probe-colour N: is the captured picture black? -----------------------
// Copies a 64x36 window of the shared colour texture (what the present path is about to
// draw) into a readback buffer and counts non-zero samples. This is the one measurement
// that separates "the capture is black" from "the presentation is black": every other
// counter in this program (SUCCESS, show, gpu) stays healthy in both cases.
static int  g_probe_colour = 0;          // --probe-colour N (0 = off)
static ComPtr<ID3D12Resource>            g_probe_rb;
static ComPtr<ID3D12CommandAllocator>    g_probe_alloc;
static ComPtr<ID3D12GraphicsCommandList> g_probe_list;
static ComPtr<ID3D12Fence>               g_probe_fence;
static UINT64                            g_probe_value = 0;

static void probe_colour_black()
{
    if (g_probe_colour <= 0 || !g_shared12 || !g_queue) { return; }
    if ((g_frames % static_cast<UINT64>(g_probe_colour)) != 0) { return; }

    const UINT W = 64, H = 36;                 // 64 * 8 B = 512 B/row: 256-aligned
    const UINT rowpitch = W * 8;
    if (!g_probe_rb) {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = static_cast<UINT64>(rowpitch) * H;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_probe_rb)))) {
            logf_("[bridge] probe: readback buffer FAILED - probe disabled\n");
            g_probe_colour = 0; return;
        }
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&g_probe_alloc))) ||
            FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               g_probe_alloc.Get(), nullptr,
                                               IID_PPV_ARGS(&g_probe_list))) ||
            FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                         IID_PPV_ARGS(&g_probe_fence)))) {
            logf_("[bridge] probe: command objects FAILED - probe disabled\n");
            g_probe_colour = 0; return;
        }
        g_probe_list->Close();
    }

    const D3D12_RESOURCE_DESC cd = g_shared12->GetDesc();
    if (cd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) { g_probe_colour = 0; return; }
    D3D12_BOX box{};
    box.right  = (W < static_cast<UINT>(cd.Width))  ? W : static_cast<UINT>(cd.Width);
    box.bottom = (H < cd.Height) ? H : cd.Height;
    box.back = 1;

    g_probe_alloc->Reset();
    g_probe_list->Reset(g_probe_alloc.Get(), nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = g_shared12.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_probe_list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g_shared12.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_probe_rb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = cd.Format;
    dst.PlacedFootprint.Footprint.Width = box.right;
    dst.PlacedFootprint.Footprint.Height = box.bottom;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = rowpitch;
    g_probe_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_probe_list->ResourceBarrier(1, &b);
    g_probe_list->Close();
    ID3D12CommandList* lists[] = { g_probe_list.Get() };
    g_queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++g_probe_value;
    if (FAILED(g_queue->Signal(g_probe_fence.Get(), v))) { return; }
    wait_fence_value(g_probe_fence.Get(), v, 200);

    void* mapped = nullptr;
    if (FAILED(g_probe_rb->Map(0, nullptr, &mapped)) || !mapped) { return; }
    const uint8_t* p = static_cast<const uint8_t*>(mapped);
    UINT nonzero = 0, samples = 0;
    for (UINT y = 0; y < box.bottom; ++y) {
        const uint8_t* row = p + static_cast<size_t>(y) * rowpitch;
        for (UINT x = 0; x < box.right; ++x) {
            const uint8_t* px = row + static_cast<size_t>(x) * 8;   // R16G16B16A16
            ++samples;
            if (px[0] | px[1] | px[2] | px[3] | px[4] | px[5] | px[6] | px[7]) { ++nonzero; }
        }
    }
    g_probe_rb->Unmap(0, nullptr);
    logf_("[bridge] probe: captured colour %ux%u fmt %u -> %u/%u samples non-zero (%.1f%%)"
          " %s\n", static_cast<unsigned>(cd.Width), cd.Height,
          static_cast<unsigned>(cd.Format), nonzero, samples,
          100.0 * nonzero / (samples ? samples : 1),
          (nonzero == 0) ? "<== BLACK CAPTURE" : "ok");
}

static bool dump_write_stream(const DumpStream& s, const char* path)
{
    if (!s.rb) { return false; }
    void* mapped = nullptr;
    if (FAILED(s.rb->Map(0, nullptr, &mapped))) { return false; }
    FILE* f = fopen(path, "wb");
    bool ok = false;
    if (f) {
        const uint32_t hdr[4] = { static_cast<uint32_t>(s.desc.Width), s.desc.Height,
                                  static_cast<uint32_t>(s.desc.Format),
                                  static_cast<uint32_t>(s.fp.Footprint.RowPitch) };
        fwrite(hdr, sizeof(hdr), 1, f);
        const uint8_t* p = static_cast<const uint8_t*>(mapped) + s.fp.Offset;
        ok = true;
        for (UINT y = 0; y < s.rows; ++y) {
            if (fwrite(p + static_cast<size_t>(y) * s.fp.Footprint.RowPitch, 1,
                       static_cast<size_t>(s.row_size), f) != static_cast<size_t>(s.row_size)) {
                ok = false;
                break;
            }
        }
        fclose(f);
    }
    s.rb->Unmap(0, nullptr);
    return ok;
}

// Writes everything out once the last requested frame has been recorded. Called after
// Present, so the fence already covers every copy that was recorded.
static void dump_flush()
{
    if (g_dump_frames <= 0 || g_dump_flushed || g_dump_index < g_dump_frames) { return; }
    g_dump_flushed = true;

    if (g_frame_fence) {
        wait_fence_value(g_frame_fence.Get(), g_frame_fence_value, 5000);
    }

    for (size_t i = 0; i < g_dump_entries.size(); ++i) {
        DumpEntry& e = g_dump_entries[i];
        if (!e.recorded) { continue; }
        char path[MAX_PATH];
        const int n = static_cast<int>(i);
        snprintf(path, sizeof(path), "%s\\f%02d_colour.raw", g_dump_dir, n);
        const bool okc = e.have[0] && dump_write_stream(e.s[0], path);
        snprintf(path, sizeof(path), "%s\\f%02d_depth.raw", g_dump_dir, n);
        const bool okd = e.have[1] && dump_write_stream(e.s[1], path);
        snprintf(path, sizeof(path), "%s\\f%02d_mv.raw", g_dump_dir, n);
        const bool okm = e.have[2] && dump_write_stream(e.s[2], path);

        snprintf(path, sizeof(path), "%s\\f%02d_meta.txt", g_dump_dir, n);
        if (FILE* f = fopen(path, "w")) {
            fprintf(f, "index %d\n", n);
            fprintf(f, "bridge_frame %llu\n", static_cast<unsigned long long>(e.bridge_frame));
            fprintf(f, "present_id %llu\n", static_cast<unsigned long long>(e.present_id));
            fprintf(f, "tick_ms %llu\n", static_cast<unsigned long long>(e.tick_ms));
            fprintf(f, "slot %u\n", e.slot);
            fprintf(f, "producer_frame_id %llu\n",
                    static_cast<unsigned long long>(e.producer_frame_id));
            fprintf(f, "jitter %.6f %.6f\n", e.jitter_x, e.jitter_y);
            fprintf(f, "render %u %u\n", e.render_w, e.render_h);
            fprintf(f, "mv_scale %.8f %.8f\n", e.mv_scale_x, e.mv_scale_y);
            fprintf(f, "stream_mv %u %u\n", e.stream_mv_w, e.stream_mv_h);
            fprintf(f, "stream_depth %u %u\n", e.stream_depth_w, e.stream_depth_h);
            fprintf(f, "present colour=%d depth=%d mv=%d\n", e.have[0] ? 1 : 0,
                    e.have[1] ? 1 : 0, e.have[2] ? 1 : 0);
            fprintf(f, "colour %ux%u fmt %u\n", static_cast<unsigned>(e.s[0].desc.Width),
                    e.s[0].desc.Height, static_cast<unsigned>(e.s[0].desc.Format));
            fprintf(f, "depth %ux%u fmt %u\n", static_cast<unsigned>(e.s[1].desc.Width),
                    e.s[1].desc.Height, static_cast<unsigned>(e.s[1].desc.Format));
            fprintf(f, "mv %ux%u fmt %u\n", static_cast<unsigned>(e.s[2].desc.Width),
                    e.s[2].desc.Height, static_cast<unsigned>(e.s[2].desc.Format));
            fprintf(f, "flags no_mv %u no_depth %u inputs_on %u fov %.3f proj_rz %u\n",
                    e.no_mv, e.no_depth, e.inputs_on, e.fov, e.proj_rz);
            fclose(f);
        }
        logf_("[bridge] dump %d: colour=%d depth=%d mv=%d slot=%u frame_id=%llu -> %s\n",
              n, okc ? 1 : 0, okd ? 1 : 0, okm ? 1 : 0, e.slot,
              static_cast<unsigned long long>(e.producer_frame_id), g_dump_dir);
    }
    g_dump_entries.clear();
    logf_("[bridge] dump complete (%d frames) - analyse with scripts/analyse_dump.sh\n",
          g_dump_frames);
}

// ===================== --fg-of: local optical flow (FidelityFX) ==============
// The producer's motion vectors measured as garbage on PBZ (docs/findings/HANDOFF.md 8.6),
// so the bridge can compute its own from the colour it already has on the AMD
// card: no cross-adapter transfer at all, and the vectors are same-frame.
//
// Cost at 3840x2160 measured 1.2-1.5 ms per frame (temp/ffx_of_test.cpp), i.e.
// cheaper than moving the 29.5-66 MB MV/depth streams across the bus.
//
// FidelityFX writes a coarse R16G16_SINT field at ceil(size/8) in *pixels*; the
// compute pass below bilinearly upscales it to the size of the depth stream and
// converts to the R16G16_FLOAT screen-UV field XeSS-FG expects. XeFG refuses a
// frame whose MV and depth sizes differ ("ERR_INVALID_ARGUMENT"), which is why the
// field cannot be tagged at its native 1/8 size. The first 6 dispatches return
// zeros on purpose (FFX treats FrameIndex() <= 5 as a scene cut), so real MVs only
// start after ~200 ms.
// --- everything from here to the matching #endif is the EXPERIMENTAL flow pass ---
// Compiled out by default (see MGPU_FG_OF next to the globals): MV is a dead input
// for libxess_fg 1.3.1, so this path is kept as a reference/re-test rig, not as a
// shipping feature.
static void run_frame_ex(ID3D11Texture2D* colour11);
// Timing of the flow dispatch. Declared outside the gate because the periodic report
// prints them unconditionally (they simply stay 0.0 when --fg-of is compiled out).
static double g_ms_of = 0.0;
static double g_max_of = 0.0;

#if MGPU_FG_OF
static const char* k_of_hlsl = R"HLSL(
Texture2D<int2>   src   : register(t0);   // FidelityFX field, pixels, ofW x ofH
Texture2D<float4> guide : register(t1);   // the captured colour (scRGB)
Texture2D<float>  dguide: register(t3);   // the depth we tag (same size as dst)
Texture2D<float4> prevc : register(t2);   // the previous presented colour (per-pixel refinement)
RWTexture2D<float2> dst : register(u0);   // tagged MV, screen UV
cbuffer P : register(b0) {
    float2 invSize;   // 1/captureSize
    float2 ofSize;    // the SINT field's own size
    float  signScale; // --fg-of-sign * --fg-of-scale, for convention A/B
    float  guideK;    // edge sensitivity: 0 = plain bilinear
    float  deadzone;  // px: below this the vector is treated as "no motion"
    float  maxLen;    // px: clamp, a bad match can otherwise smear the screen
    float2 constXY;   // px: --fg-of-const override (does XeFG consume our MV at all?)
    float  constOn;
    float  depthK;    // relative-depth weight for the upsample (0 = off)
};

float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
    uint gtw = 0u, gth = 0u;
    guide.GetDimensions(gtw, gth);
    uint tw = 0u, th = 0u;
    dst.GetDimensions(tw, th);

    float2 uv = (float2(id.xy) + 0.5) / float2(tw, th);
    float2 hp = uv * float2(gtw, gth) - 0.5;
    int2   hi = clamp((int2)floor(hp), int2(0, 0), int2(gtw, gth) - 1);
    const float l0 = luma(guide.Load(int3(hi, 0)).rgb);

    float2 g = uv * ofSize - 0.5;
    int2   i0 = (int2)floor(g);
    float2 f  = g - float2(i0);

    float2 acc = 0.0;
    float  wsum = 0.0;
    [unroll] for (int dy = 0; dy < 2; ++dy) {
        [unroll] for (int dx = 0; dx < 2; ++dx) {
            int2 c = clamp(i0 + int2(dx, dy), int2(0, 0), int2(ofSize) - 1);
            // Same-surface weight: a neighbouring vector only contributes when its
            // colour matches this pixel, which is what stops the field from bleeding
            // across object edges (plain bilinear smears moving objects into the
            // background - that was the extra ghosting).
            float2 tuv = (float2(c) + 0.5) / ofSize;
            int2   hc  = clamp((int2)floor(tuv * float2(gtw, gth)), int2(0, 0),
                               int2(gtw, gth) - 1);
            float  l   = luma(guide.Load(int3(hc, 0)).rgb);
            float  wl  = 1.0 / (1.0 + abs(l - l0) * guideK);
            // Depth is the better surface id: it separates objects from the background
            // even where the colours are similar, and XeFG consumes it anyway. Use the
            // *relative* difference so reverse-Z (UE5) and standard depth both work.
            if (depthK > 0.0) {
                float dc = dguide.Load(int3(clamp((int2)(uv * float2(tw, th)),
                                                 int2(0, 0), int2(tw, th) - 1), 0));
                float dt = dguide.Load(int3(clamp((int2)floor(tuv * float2(tw, th)),
                                                 int2(0, 0), int2(tw, th) - 1), 0));
                float rel = abs(dc - dt) / max(dc, 1e-4);
                wl *= 1.0 / (1.0 + rel * depthK);
            }
            float2 d   = float2(dx ? f.x : 1.0 - f.x, dy ? f.y : 1.0 - f.y);
            float  w   = wl * d.x * d.y;
            acc  += float2(src.Load(int3(c, 0))) * w;
            wsum += w;
        }
    }
    float2 v = acc / max(wsum, 1e-4);

    // ---- per-pixel refinement (the idea behind OptiScaler's camera-motion pass, but
    // supervised by the motion we actually measured): the upsampled vector is only a
    // seed. Take the seed and the four neighbouring OF texels, warp the *previous*
    // colour by each and keep the one that reproduces this pixel's colour best. At an
    // object boundary that selects the surface's own vector instead of the block
    // average, which is what left the narrow halo.
    {
        const float3 c0 = guide.Load(int3(clamp((int2)id.xy, int2(0, 0), int2(tw, th) - 1), 0)).rgb;
        float2 best = v;
        float  bestErr = 1e9;
        [unroll] for (int k = 0; k < 5; ++k) {
            float2 cand = v;
            if (k > 0) {
                int2  step = int2(0, 0);
                if (k == 1) { step = int2(-1, 0); }
                else if (k == 2) { step = int2(1, 0); }
                else if (k == 3) { step = int2(0, -1); }
                else { step = int2(0, 1); }
                int2 c = clamp(i0 + step, int2(0, 0), int2(ofSize) - 1);
                cand = float2(src.Load(int3(c, 0)));
            }
            // the content at this pixel was at (uv - cand/ofSize) one frame ago
            float2 puv = uv - cand / ofSize;
            int2   pp  = clamp((int2)(puv * float2(tw, th)), int2(0, 0), int2(tw, th) - 1);
            float3 cp  = prevc.Load(int3(pp, 0)).rgb;
            float  e   = abs(cp.x - c0.x) + abs(cp.y - c0.y) + abs(cp.z - c0.z);
            if (e < bestErr) { bestErr = e; best = cand; }
        }
        v = best;
    }
    if (constOn > 0.5) {
        v = constXY;                       // diagnostic: a fixed vector everywhere
    }
    float  len = length(v);
    if (len < deadzone) {
        v = float2(0.0, 0.0);
    } else {
        v *= min(1.0, maxLen / max(len, 1e-5));
    }
    dst[id.xy] = v * invSize * signScale;
}
)HLSL";

static FfxOpticalflowContext            g_of_ctx{};
static bool                             g_of_ready = false;
static UINT                             g_of_w = 0, g_of_h = 0;      // SINT field size
static UINT                             g_of_out_w = 0, g_of_out_h = 0; // tagged MV size
static std::vector<unsigned char>       g_of_scratch;
static ComPtr<ID3D12Resource>           g_of_sint;      // FFX output, ceil(size/8)
static ComPtr<ID3D12Resource>           g_of_scd;       // scene-change output, 3x1
static ComPtr<ID3D12Resource>           g_of_mv;        // R16G16_FLOAT UV, tagged
static ComPtr<ID3D12DescriptorHeap>     g_of_heap;      // [0] = SRV sint, [1] = UAV mv
static ComPtr<ID3D12RootSignature>      g_of_root;
static ComPtr<ID3D12PipelineState>      g_of_pso;

// Pixel-field -> UV-field convert pass (the only shader --fg-of adds).
static bool of_build_pso(UINT w, UINT h)
{
    ComPtr<ID3DBlob> cs;
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(k_of_hlsl, strlen(k_of_hlsl), "of.hlsl", nullptr, nullptr,
                            "cs_main", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr)) {
        logf_("[bridge] --fg-of: D3DCompile FAILED: %s\n",
              err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[4]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;                 // t0 = field, t1 = current colour
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;                 // t2 = previous colour (hist 0 or 1)
    ranges[1].BaseShaderRegister = 2;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 1;                 // t3 = depth guide (one ring slot)
    ranges[2].BaseShaderRegister = 3;
    ranges[2].OffsetInDescriptorsFromTableStart = 4;
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[3].NumDescriptors = 1;                 // u0 = the MV we tag
    ranges[3].BaseShaderRegister = 0;
    ranges[3].OffsetInDescriptorsFromTableStart = 4 + mgpu::kMaxSlots;

    D3D12_ROOT_PARAMETER params[5]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[2];
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &ranges[3];
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[4].Constants.Num32BitValues = 12;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 5;
    rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sig;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) { logf_("[bridge] --fg-of: serialize root FAILED\n"); return false; }
    hr = g_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                       IID_PPV_ARGS(&g_of_root));
    if (FAILED(hr)) { logf_("[bridge] --fg-of: CreateRootSignature FAILED\n"); return false; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_of_root.Get();
    pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    if (FAILED(g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_of_pso)))) {
        logf_("[bridge] --fg-of: CreateComputePipelineState FAILED\n");
        return false;
    }
    (void)w; (void)h;
    return true;
}

static bool of_init();

// One attempt only: a failure here is permanent (wrong device, missing scratch),
// and retrying every frame would flood the log.
static bool of_init_once();

static bool of_init()
{
    static bool tried = false;
    if (g_of_ready) { return true; }
    if (tried) { return false; }
    tried = true;
    return of_init_once();
}

static bool of_init_once()
{
    const UINT w = g_full_w, h = g_full_h;
    if (w < 256 || h < 256) { return false; }

    g_of_scratch.assign(ffxGetScratchMemorySizeDX12(1), 0);
    FfxInterface ifc{};
    const FfxErrorCode ifc_rc = ffxGetInterfaceDX12(&ifc, ffxGetDeviceDX12(g_device.Get()),
                                                    g_of_scratch.data(), g_of_scratch.size(), 1);
    if (ifc_rc != FFX_OK) {
        // rc 1 = invalid pointer, 2 = insufficient memory, 5 = backend API error
        unsigned int head = 0;
        if (g_of_scratch.size() >= 4) { memcpy(&head, g_of_scratch.data(), 4); }
        logf_("[bridge] --fg-of: ffxGetInterfaceDX12 FAILED rc=%d"
              " (device=%p scratch=%p size=%zu need=%zu head=%08x align=%zu)\n",
              static_cast<int>(ifc_rc), static_cast<void*>(g_device.Get()),
              static_cast<void*>(g_of_scratch.data()), g_of_scratch.size(),
              ffxGetScratchMemorySizeDX12(1), head,
              static_cast<size_t>(reinterpret_cast<uintptr_t>(g_of_scratch.data()) & 63u));
        return false;
    }

    FfxOpticalflowContextDescription cd{};
    cd.backendInterface = ifc;
    cd.flags = 0;
    cd.resolution = { w, h };
    if (ffxOpticalflowContextCreate(&g_of_ctx, &cd) != FFX_OK) {
        logf_("[bridge] --fg-of: ffxOpticalflowContextCreate FAILED\n");
        return false;
    }

    FfxOpticalflowSharedResourceDescriptions shared{};
    if (ffxOpticalflowGetSharedResourceDescriptions(&g_of_ctx, &shared) != FFX_OK) {
        logf_("[bridge] --fg-of: shared descriptions FAILED\n");
        return false;
    }
    g_of_w = shared.opticalFlowVector.resourceDescription.width;
    g_of_h = shared.opticalFlowVector.resourceDescription.height;

    // XeFG requires the MV and depth resources to have the *same* size (mismatched
    // sizes are rejected with ERR_INVALID_ARGUMENT and nothing is interpolated), so
    // the field is upscaled to whatever the depth stream is.
    g_of_out_w = (g_ctl && g_ctl->streams[2].width >= 256) ? g_ctl->streams[2].width : w;
    g_of_out_h = (g_ctl && g_ctl->streams[2].height >= 256) ? g_ctl->streams[2].height : h;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto make = [&](ComPtr<ID3D12Resource>& r, DXGI_FORMAT fmt, UINT tw, UINT th,
                    D3D12_RESOURCE_STATES st, D3D12_RESOURCE_FLAGS fl) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = tw; d.Height = th; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = fmt; d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = fl;
        return g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr,
                                                 IID_PPV_ARGS(&r));
    };

    if (FAILED(make(g_of_sint, DXGI_FORMAT_R16G16_SINT, g_of_w, g_of_h,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ||
        FAILED(make(g_of_scd, DXGI_FORMAT_R32_UINT, 3, 1,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ||
        FAILED(make(g_of_mv, DXGI_FORMAT_R16G16_FLOAT, g_of_out_w, g_of_out_h,
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))) {
        logf_("[bridge] --fg-of: output textures FAILED\n");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4 + mgpu::kMaxSlots + 1;   // [0]field [1]cur [2..3]hist [4..7]depth [8]uav
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_of_heap)))) {
        logf_("[bridge] --fg-of: descriptor heap FAILED\n");
        return false;
    }
    const UINT dstride = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_of_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R16G16_SINT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_of_sint.Get(), &sd, cpu);
    // descriptor 1: the captured colour, used as the guide for the edge-aware upsample
    D3D12_SHADER_RESOURCE_VIEW_DESC gd{};
    gd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    gd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    gd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    gd.Texture2D.MipLevels = 1;
    cpu.ptr += dstride;
    g_device->CreateShaderResourceView(g_shared12.Get(), &gd, cpu);
    // [2],[3] = the two history colours, [4..7] = the depth ring: all bound lazily, once
    cpu.ptr += static_cast<SIZE_T>(2 + mgpu::kMaxSlots) * dstride;
    // descriptor 6: the MV we tag
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R16G16_FLOAT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(g_of_mv.Get(), nullptr, &ud, cpu);

    if (!of_build_pso(w, h)) { return false; }
    g_of_ready = true;
    logf_("[bridge] --fg-of: FidelityFX optical flow at %ux%u, field %ux%u -> tagged at %ux%u"
          " (must match the depth stream)\n", w, h, g_of_w, g_of_h, g_of_out_w, g_of_out_h);
    return true;
}

// Binds the two history colours (t2 selects one of them per frame). Called once.
static void of_bind_hist()
{
    static bool done = false;
    if (done || !g_of_heap || !g_hist[0] || !g_hist[1]) { return; }
    done = true;
    const UINT dstride = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_of_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(2) * dstride;
    for (int i = 0; i < 2; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(g_hist[i].Get(), &sd, cpu);
        cpu.ptr += dstride;
    }
}

// Binds depth ring entry i as the OF upsample's depth guide (t2). Called once per ring
// texture, so no descriptor is rewritten while a frame that reads it may be in flight.
static void of_bind_depth(UINT i, ID3D12Resource* res)
{
    if (!g_of_heap || !res || i >= mgpu::kMaxSlots) { return; }
    const UINT dstride = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_of_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(2 + i) * dstride;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_FLOAT;      // the stream is R32_TYPELESS
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(res, &sd, cpu);
}

// Bring-up check for --fg-of-selftest: init + two dispatches and the convert pass,
// on a dummy 4K colour, so a broken barrier/PSO shows up without starting the game.
static bool of_record(ID3D12GraphicsCommandList* list);
static bool of_selftest()
{
    g_full_w = 3840;
    g_full_h = 2160;
    g_fg_of = true;
    if (!of_init()) { return false; }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = g_full_w; d.Height = g_full_h;
    d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&g_shared12)))) {
        logf_("[bridge] --fg-of-selftest: dummy colour FAILED\n");
        return false;
    }

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                           IID_PPV_ARGS(&list))) ||
        FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        logf_("[bridge] --fg-of-selftest: command objects FAILED\n");
        return false;
    }

    UINT64 fv = 0;
    for (int i = 0; i < 3; ++i) {
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        if (!of_record(list.Get())) {
            logf_("[bridge] --fg-of-selftest: of_record FAILED on frame %d\n", i);
            return false;
        }
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        g_queue->ExecuteCommandLists(1, lists);
        g_queue->Signal(fence.Get(), ++fv);
        wait_fence_value(fence.Get(), fv, 5000);
    }
    logf_("[bridge] --fg-of-selftest: 3 dispatches + convert OK\n");
    return true;
}

// Runs the optical flow for the current colour (already in g_shared12) and leaves the
// UV field in g_of_mv. The colour must be in COMMON and the caller's list must be open.
static bool of_record(ID3D12GraphicsCommandList* list)
{
    if (!g_fg_of || !g_of_ready) { return false; }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // colour: COMMON -> compute read
    b.Transition.pResource = g_shared12.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &b);

    FfxOpticalflowDispatchDescription dd{};
    dd.commandList = ffxGetCommandListDX12(list);
    dd.color = ffxGetResourceDX12(g_shared12.Get(), ffxGetResourceDescriptionDX12(g_shared12.Get()),
                                  L"OFColor", FFX_RESOURCE_STATE_COMPUTE_READ);
    dd.opticalFlowVector = ffxGetResourceDX12(
        g_of_sint.Get(), ffxGetResourceDescriptionDX12(g_of_sint.Get(), FFX_RESOURCE_USAGE_UAV),
        L"OFVector", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.opticalFlowSCD = ffxGetResourceDX12(
        g_of_scd.Get(), ffxGetResourceDescriptionDX12(g_of_scd.Get(), FFX_RESOURCE_USAGE_UAV),
        L"OFSCD", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.reset = false;
    // The captured colour is scRGB (linear FP16, 1.0 = 80 nits).
    dd.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
    dd.minMaxLuminance = { 0.0f, 1000.0f };
    const FfxErrorCode drc = ffxOpticalflowContextDispatch(&g_of_ctx, &dd);
    if (drc != FFX_OK) {
        if (g_max_of >= 0.0) {
            g_max_of = -1.0;      // log it once
            logf_("[bridge] --fg-of: ffxOpticalflowContextDispatch FAILED rc=%d\n",
                  static_cast<int>(drc));
        }
        return false;
    }

    // colour back to COMMON (the convert draw re-transitions it), SINT -> SRV, mv -> UAV
    D3D12_RESOURCE_BARRIER back[3]{};
    for (int i = 0; i < 3; ++i) {
        back[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        back[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    back[0].Transition.pResource = g_shared12.Get();
    back[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    back[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    back[1].Transition.pResource = g_of_sint.Get();
    back[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    back[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    back[2].Transition.pResource = g_of_mv.Get();
    back[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    back[2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->ResourceBarrier(3, back);

    // pixels -> UV
    list->SetDescriptorHeaps(1, g_of_heap.GetAddressOf());
    list->SetComputeRootSignature(g_of_root.Get());
    list->SetPipelineState(g_of_pso.Get());
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_of_heap->GetGPUDescriptorHandleForHeapStart();
    const UINT dstride = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // [2]/[3] hold hist0/hist1; the previous frame's colour is whichever is not being
    // written this frame.
    of_bind_hist();
    list->SetComputeRootDescriptorTable(0, gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_prev = gpu;
    gpu_prev.ptr += static_cast<UINT64>(2 + (1u - (g_hist_write & 1u))) * dstride;
    list->SetComputeRootDescriptorTable(1, gpu_prev);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_depth = gpu;
    gpu_depth.ptr += static_cast<UINT64>(4 + g_of_depth_desc) * dstride;
    list->SetComputeRootDescriptorTable(2, gpu_depth);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
    gpu_uav.ptr += static_cast<UINT64>(4 + mgpu::kMaxSlots) * dstride;
    list->SetComputeRootDescriptorTable(3, gpu_uav);
    // Motion-vector units: the field is in pixels of the captured frame, so
    //   px  -> (1, 1)              pixels, what XeFG expects without the NDC flag
    //   uv  -> (1/w, 1/h)          screen UV
    //   ndc -> (2/w, -2/h)         NDC, y flipped because NDC counts up
    // Units XeSS-FG wants, taken from OptiScaler's camera-motion shader (CM_Common.h):
    //
    //     Output[pixel] = -velocity.xy * size;   // game MV: UV(cur-prev) -> px(prev-cur)
    //     Output[pixel] = (previousUv - uv) * size;   // size = the VELOCITY texture
    //
    // i.e. pixels *of the MV texture's own resolution* and the sign of
    // (previous - current). Our field is pixels of the captured frame with the opposite
    // sign, hence the scale factor and the negation below (mode 3 = "mvpx").
    float ux = 1.0f, uy = 1.0f;
    if (g_of_units == 3) {
        ux = -static_cast<float>(g_of_out_w) / static_cast<float>(g_full_w);
        uy = -static_cast<float>(g_of_out_h) / static_cast<float>(g_full_h);
    } else if (g_of_units == 1) {
        ux = 1.0f / static_cast<float>(g_full_w);
        uy = 1.0f / static_cast<float>(g_full_h);
    } else if (g_of_units == 2) {
        ux = 2.0f / static_cast<float>(g_full_w);
        uy = -2.0f / static_cast<float>(g_full_h);
    }
    const float konst[12] = { ux,
                              uy,
                              static_cast<float>(g_of_w),
                              static_cast<float>(g_of_h),
                              g_of_sign * g_of_scale,
                              g_of_guide,
                              g_of_deadzone,
                              g_of_maxlen,
                              g_of_const_x,
                              g_of_const_y,
                              g_of_const_on ? 1.0f : 0.0f,
                              (g_of_depth_bound[g_of_depth_desc] ? g_of_depth_k : 0.0f) };
    list->SetComputeRoot32BitConstants(4, 12, konst, 0);
    list->Dispatch((g_of_out_w + 7) / 8, (g_of_out_h + 7) / 8, 1);

    // and put them back: SINT -> UAV (FFX expects that next frame), mv -> COMMON (what
    // the XeFG tagging below declares as its incoming state).
    D3D12_RESOURCE_BARRIER after[2]{};
    for (int i = 0; i < 2; ++i) {
        after[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        after[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    after[0].Transition.pResource = g_of_sint.Get();
    after[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    after[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    after[1].Transition.pResource = g_of_mv.Get();
    after[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    after[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(2, after);
    return true;
}
#endif  // MGPU_FG_OF

static void run_frame(ID3D11Texture2D* colour11)
{
    run_frame_ex(colour11);
}

// XeFG keeps a raw reference to whatever we tagged last on its own queue, so a texture
// we stop using must NOT be destroyed the moment the producer publishes a new content
// size - that removed the consumer's device (0x887A0005) the first time the size changed.
// Retire list: hold it for a few presents, far beyond any in-flight window.
static std::vector<ComPtr<ID3D12Resource>> g_retired_inputs;
static std::vector<UINT64> g_retired_at;
static const UINT64 kRetirePresents = 16;

static void retire_input(ComPtr<ID3D12Resource>& r)
{
    if (!r) { return; }
    g_retired_inputs.push_back(r);
    g_retired_at.push_back(g_present_id);
    r.Reset();
}

static void collect_retired_inputs()
{
    for (size_t i = 0; i < g_retired_inputs.size();) {
        if (g_present_id - g_retired_at[i] >= kRetirePresents) {
            g_retired_inputs.erase(g_retired_inputs.begin() + static_cast<ptrdiff_t>(i));
            g_retired_at.erase(g_retired_at.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

// ---- v10 credit window (P1) -------------------------------------------------
// We tell the producer which slot we are done reading, and only then may it overwrite it.
// "Done" = the GPU work that read those textures has completed, i.e. after gpu_throttle()
// waited for the *previous* frame's fence. The value we report is the publication stored in
// frames[slot].published (cross-adapter fences cannot be queried from this node, so the
// header field is the only way to name it - docs/findings/HANDOFF.md 8.6).
static UINT g_ack_slot = mgpu::kMaxSlots;

static void ack_previous_slot()
{
    if (!g_ctl) { return; }
    // Hand back everything we will never touch, not just the slot we read. We only ever
    // read one slot per frame (round-robin), so the others are idle on our side and the
    // producer may overwrite them at once. Acking only one slot per frame made the queue
    // sit at 3/4 and cost 25-65% of the publications for ~10 s after every preset switch
    // (measured, docs/design/VENDOR_COMPAT_PLAN.md M6.4.4): the slot counters drifted out of
    // phase while there was only a single free slot to absorb the drift.
    // Never ack the slot this frame is about to read: the producer could otherwise start
    // overwriting it in the microseconds between here and our copy command.
    const UINT slots = (g_ctl->slots != 0) ? g_ctl->slots : 1;
    const UINT next_slot = g_reuse_frame ? g_ack_slot : (g_mv_slot % slots);
    for (UINT k = 0; k < mgpu::kMaxSlots; ++k) {
        if (k == g_ack_slot || k == next_slot) { continue; }   // in flight / about to be read
        const uint64_t published = g_ctl->frames[k].published;
        if (published != 0) { g_ctl->consumed[k] = published; }
    }
    // The slot we just finished reading is safe too - the caller only invokes this after
    // gpu_throttle() confirmed the previous frame's GPU work retired.
    if (g_ack_slot < mgpu::kMaxSlots) {
        const uint64_t published = g_ctl->frames[g_ack_slot].published;
        if (published != 0) { g_ctl->consumed[g_ack_slot] = published; }
    }
    g_ack_slot = mgpu::kMaxSlots;
}

// Drops our local (consumer-side) copies of the input streams. Only their *size* can
// change now: the placed views are fixed for the life of the process (protocol v9).
static void release_local_inputs()
{
    for (UINT s = 0; s < mgpu::kMaxSlots; ++s) {
        retire_input(g_mv_local[s]);
        retire_input(g_depth_local[s]);
        retire_input(g_depth_ring[s]);
#if MGPU_FG_OF
        // The flow pass caches a descriptor per ring slot; a rebuilt ring texture would
        // otherwise leave the pass reading a destroyed resource.
        g_of_depth_bound[s] = false;
#endif
    }
    g_ring_count = 0;
    g_ring_need  = 1;
    g_ring_write = 0;
}

// Drops every view we placed inside the shared heap (plus the local copies).
// v9: only used on teardown / when the producer's block becomes invalid.
static void release_shared_views()
{
    for (UINT s = 0; s < mgpu::kMaxSlots; ++s) {
        g_mv[s].Reset();
        g_depth[s].Reset();
        g_mv_local[s].Reset();
        g_depth_local[s].Reset();
        g_depth_ring[s].Reset();
        // g_fwd_expect[] is deliberately NOT reset: the producer keeps incrementing the
        // same fences, so zeroing our side would make us read slots nobody wrote.
    }
    g_ring_count = 0;
    g_ring_need  = 1;
    g_ring_write = 0;
    g_shared_slot = 0;
}

// v9: nobody rebuilds anything. `generation` only says "the live rectangle inside the
// slots changed size", which costs us one log line and a lazily re-created local copy -
// the placed views, the fences and the mapping all stay exactly as they are. `magic` is
// still checked because a *new* producer process would recreate the block; that is the
// only case where this frame has to be skipped.
static bool service_shared_layout()
{
    if (!g_ctl) { return true; }

    if (g_ctl->magic != mgpu::kSharedMagic) {
        if (!g_layout_pending) {
            g_layout_pending = true;
            logf_("[bridge] the producer's block is invalid (restarting?) - pausing\n");
            release_shared_views();
        }
        return false;
    }
    g_layout_pending = false;

    if (g_ctl->generation != g_seen_generation) {
        g_seen_generation = g_ctl->generation;
        // Content size changed: the local copies are the only things to rebuild, and
        // ensure_local_copy() does that on demand.
        release_local_inputs();
        logf_("[bridge] producer published gen %u: mv %ux%u, depth %ux%u (slots stay %ux%u)\n",
              g_seen_generation, g_ctl->streams[1].width, g_ctl->streams[1].height,
              g_ctl->streams[2].width, g_ctl->streams[2].height,
              mgpu::kStreamMaxW, mgpu::kStreamMaxH);
    }
    // The geometry fields only change together with `generation` (the producer bumps it
    // from update_content_size() on every width/height change), so re-reading the seqlock
    // every frame was a handful of uncached PCIe reads for nothing. The seqlock still
    // makes a *concurrent* change safe - it just no longer runs when nothing changed.
    // The `== 0` half keeps one edge case honest: if we attached before the producer
    // published any size, we keep looking until it does.
    if (g_ctl->generation != g_seen_geo_generation ||
        g_geo.mv_w == 0 || g_geo.depth_w == 0) {
        g_seen_geo_generation = g_ctl->generation;
        refresh_input_geo();
    }
    return true;
}

static void run_frame_ex(ID3D11Texture2D* colour11)
{
    if (!service_shared_layout()) { return; }
    collect_retired_inputs();
    probe_colour_black();

    // 1. colour: capture frame -> shared sink (D3D11, on the AMD card)
    // One frame id for XeLL markers and the XeSS-FG present id, as the guide
    // requires. XeLL sleeps at simulation start and needs the markers to pace.
    const uint32_t present_id = static_cast<uint32_t>(++g_present_id);
    const double t_r0 = qpc_ms();
    // Catch-up burst detector: after a stall, WGC hands over several queued frames and the
    // loop processes them back to back (each with its own Present). This is the "the missing
    // inputs get dumped into the next batch" effect - measurable instead of guessed.
    {
        static double s_prev_proc = 0.0;
        if (s_prev_proc > 0.0) {
            const double dt = t_r0 - s_prev_proc;
            const double nom = input_interval_ms();
            if (nom > 2.0 && dt < nom * 0.6) { ++g_burst_count; }
        }
        s_prev_proc = t_r0;
    }
    // Protocol v7: tell the add-on whether its motion-vector stream is wanted. It is
    // ~14 MB/frame across PCIe, so it is produced only when we will tag it.
    if (g_ctl) {
        // --mv-shadow needs the producer to keep feeding real vectors even though we tag
        // zeroed ones, otherwise the copy would transfer stale data and measure nothing.
        bool want_producer_mv = !(g_no_mv && !g_mv_shadow);
#if MGPU_FG_OF
        // --fg-of builds its own vectors from the colour, so the producer stream (and its
        // ~14.7 MB/frame across PCIe) would be pure waste.
        if (g_fg_of) { want_producer_mv = false; }
#endif
        g_ctl->consumer_request = want_producer_mv ? mgpu::kRequestMv : 0u;
    }
    // --dyn-mfg: OptiScaler's controller, once per real frame (before anything is
    // tagged, so a new count applies to this present).
    if (g_dyn_mfg && !g_interp_cadence) { fg_dyn_mfg_frame(t_r0, g_ms_present); }
    // --interp-cadence: pick this frame's multiplier from the vblanks that elapsed
    // since the previous real frame (see cadence_next_slots). Done before anything is
    // tagged, so the count applies to this present.
    if (g_interp_cadence && g_refresh_hz > 0) {
        const UINT slots = cadence_next_slots();     // what the phase accumulator wants
        UINT want = slots - 1;
        // Rate-limit the expensive part: see g_cad_hold_frames. While holding we tag the
        // count the DLL is already configured for, so what we ask for and what it produces
        // always agree; the phase accumulator keeps the long-run average correct anyway.
        if (g_cad_hold > 0) {
            --g_cad_hold;
            want = g_fg_interp_actual;
        } else if (want != g_fg_interp_actual) {
            want = (want > g_fg_interp_actual) ? (g_fg_interp_actual + 1)
                                              : (g_fg_interp_actual - 1);
            if (want < 1) { want = 1; }
            g_cad_hold = (g_cad_hold_frames > 0) ? g_cad_hold_frames : 0;
        }
        const UINT before = g_fg_interp_actual;
        fg_set_interp(want, "interp-cadence", /*quiet=*/true);
        if (g_fg_interp_actual != before) { ++g_cad_switches; }
        if (g_fg_interp_actual != want) { ++g_cad_fail; }
        if (want < 8) { ++g_cad_hist[want]; }
        g_cad_last_k = want + 1;      // frameRenderTime follows what we actually tagged
    }
    // Waiting for the previous frame keeps queue depth at 1: latency stays bounded and
    // "gpu" is a real measurement, but our submissions cannot overlap XeSS-FG's work.
    if (g_no_throttle) { g_ms_throttle = 0.0; } else { gpu_throttle(); }
    // v10: the previous frame's GPU work (which read its MV/depth slot) has completed, so
    // hand that slot back to the producer.
    ack_previous_slot();
    const double t_work0 = qpc_ms();

    // Real frame interval for XeSS-FG's pacing model.
    const ULONGLONG now_tick = GetTickCount64();
    if (g_last_frame_tick) {
        const double dt = static_cast<double>(now_tick - g_last_frame_tick);
        g_frame_ms_ema = g_frame_ms_ema * 0.9 + dt * 0.1;
    }
    g_last_frame_tick = now_tick;
    if (g_xell && !g_reuse_frame) {
        const uint32_t fid = present_id;
        xellAddMarkerData(g_xell, fid, XELL_SIMULATION_START);
        // XeLL's whole job is to delay work to reduce latency, so its sleep is timed:
        // it is the one component inside run_frame that could hide tens of ms.
        const double x0 = qpc_ms();
        xellSleep(g_xell, fid);
        const double x1 = qpc_ms();
        g_ms_xell = g_ms_xell * 0.9 + (x1 - x0) * 0.1;
        if (x1 - x0 > g_max_xell) { g_max_xell = x1 - x0; }
        xellAddMarkerData(g_xell, fid, XELL_SIMULATION_END);
    }

    const double t_c0 = qpc_ms();
    if (g_colour_mutex) { g_colour_mutex->AcquireSync(0, 20); }
    if (!g_reuse_frame) {
        // CopyResource is silent about mismatches and a failed copy leaves the sink at its
        // initial (black) content - which looks exactly like "the pipeline runs but the
        // screen is black" (protocol-independent, every counter healthy). Say it once.
        static bool copy_checked = false;
        if (!colour11) {
            if (!copy_checked) {
                copy_checked = true;
                logf_("[bridge] colour copy: colour11 is NULL on a non-reuse frame"
                      " - nothing can reach the sink\n");
            }
        } else if (!copy_checked) {
            copy_checked = true;
            D3D11_TEXTURE2D_DESC sd{}, dd{};
            colour11->GetDesc(&sd);
            if (g_shared11) { g_shared11->GetDesc(&dd); }
            logf_("[bridge] colour copy: src %ux%u fmt %u -> sink %ux%u fmt %u %s\n",
                  (unsigned)sd.Width, sd.Height, (unsigned)sd.Format,
                  (unsigned)dd.Width, dd.Height, (unsigned)dd.Format,
                  (sd.Width == dd.Width && sd.Height == dd.Height &&
                   sd.Format == dd.Format) ? "(match)" : "<== MISMATCH");
        }
        if (colour11) { g_ctx11->CopyResource(g_shared11.Get(), colour11); }
    }
    if (g_colour_mutex) { g_colour_mutex->ReleaseSync(0); }
    // The D3D11 half of the colour hop: keyed-mutex acquire + the full-frame CopyResource
    // (66 MB at 4K, 118 MB at 5K, both on this card). It used to be invisible - `work`
    // only starts after it and `copy` only covers the D3D12 wait - so the largest single
    // per-frame transfer had no line of its own in the breakdown.
    {
        const double c1 = qpc_ms();
        g_ms_colour = g_ms_colour * 0.9 + (c1 - t_c0) * 0.1;
        if (c1 - t_c0 > g_max_colour) { g_max_colour = c1 - t_c0; }
    }

    if (!g_reuse_frame) {
        const UINT64 fv = ++g_fence11_value;
        g_ctx11_4->Signal(g_fence11.Get(), fv);
        // 2. D3D12 waits for that copy. This is a 118 MB transfer on the same GPU
        //    XeSS-FG is using, and it was never measured - it is now.
        const double f0 = qpc_ms();
        g_queue->Wait(g_fence12.Get(), fv);
        const double f1 = qpc_ms();
        g_ms_copywait = g_ms_copywait * 0.9 + (f1 - f0) * 0.1;
        if (f1 - f0 > g_max_copywait) { g_max_copywait = f1 - f0; }
    }

    // 3. MV/depth slot paired with this colour: one frame behind, because WGC
    //    only delivers a frame after it has been presented. On a re-presented frame
    //    the slot is unchanged, so nothing has to be waited for.
    const UINT slots = (g_ctl->slots != 0) ? g_ctl->slots : 1;
    UINT slot = g_mv_slot % slots;
    if (g_reuse_frame) {
        slot = g_last_slot;
    } else {
        // Round-robin slot walking. Protocol v5 was meant to let us pick the newest
        // GPU-complete slot (frames[].ready), but publishing that "ready" needs a
        // GetCompletedValue() poll on a SHARED_CROSS_ADAPTER fence, which stalls the
        // game's render thread (33 -> 60 ms, picture flickers - docs/findings/HANDOFF.md 8.6,
        // "两个必须记住的坑"). The producer therefore never advertises it, the poll and
        // --mv-newest are gone, and the slots are consumed in order - which is the
        // behaviour that always ran at full rate.
        g_last_slot = slot;
        g_have_last = true;
    }
    g_ack_slot = slot;          // v10: ACKed once this frame's GPU work is done

    // Cached: see back_buffer(). The buffer identity is fixed for the life of the swap
    // chain, so this also removes the per-frame QueryInterface/refcount traffic.
    ID3D12Resource* bbres = nullptr;
    const UINT bbi = g_swapchain->GetCurrentBackBufferIndex();
    if (!back_buffer(bbi, &bbres)) { return; }
    D3D12_RESOURCE_STATES& bb_state = g_bb_state[bbi & 3];

    g_alloc->Reset();
    g_list->Reset(g_alloc.Get(), nullptr);

    D3D12_RESOURCE_BARRIER bb[2]{};
    bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bb[0].Transition.pResource = bbres;
    bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bb[0].Transition.StateBefore = bb_state;
    bb[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    bb[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bb[1].Transition.pResource = g_shared12.Get();
    bb[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bb[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g_list->ResourceBarrier(2, bb);

    // --fg-of: motion vectors from the colour we just captured. Recorded before the
    // convert draw so the colour is still in COMMON for the flow dispatch; of_record
    // restores COMMON on the way out, which is what the barrier above assumes.
#if MGPU_FG_OF
    if (g_fg_of) {
        const double o0 = qpc_ms();
        if (!g_of_ready) { of_init(); }
        if (of_record(g_list.Get())) {
            const double o1 = qpc_ms();
            g_ms_of = g_ms_of * 0.9 + (o1 - o0) * 0.1;
            if (o1 - o0 > g_max_of) { g_max_of = o1 - o0; }
        }
    }
#endif

    // scRGB (FP16) -> BT.2020 + PQ in R10G10B10A2: the format XeSS-FG requires.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(bbi & 3) * g_rtv_stride;
    if (!g_rtv_res[bbi & 3]) {
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_device->CreateRenderTargetView(bbres, &rtv_desc, rtv);
        g_rtv_res[bbi & 3] = bbres;
    }

    // Draw to the real back buffer size. XeSS-FG creates the native swap chain
    // itself, so its size need not equal the captured size; a viewport that is too
    // small leaves an uncovered border of uninitialised pixels around the picture.
    const D3D12_RESOURCE_DESC bb_desc = bbres->GetDesc();
    const UINT draw_w = static_cast<UINT>(bb_desc.Width);
    const UINT draw_h = bb_desc.Height;

    // Checked on the first frame, not during init: the window is only resized to the
    // borderless size after the swap chain exists, so checking earlier reported a
    // bogus mismatch. A present size that differs from the client area means DXGI
    // stretches the picture - that is the "everything looks scaled" symptom.
    if (!g_present_size_checked) {
        g_present_size_checked = true;
        RECT cr{};
        if (g_bridge_wnd && GetClientRect(g_bridge_wnd, &cr)) {
            const UINT cw = static_cast<UINT>(cr.right - cr.left);
            const UINT ch = static_cast<UINT>(cr.bottom - cr.top);
            if (cw && ch && (cw != draw_w || ch != draw_h)) {
                logf_("[bridge] WARNING: present size %ux%u != window client %ux%u - DXGI will"
                      " stretch the picture. Align the capture, the output and the monitor"
                      " (see --den/--scale).\n", draw_w, draw_h, cw, ch);
            } else {
                RECT wr{};
                GetWindowRect(g_bridge_wnd, &wr);
                logf_("[bridge] present %ux%u == window client %ux%u (1:1, no stretch)"
                      " window rect=(%ld,%ld)-(%ld,%ld) monitor=%ux%u\n",
                      draw_w, draw_h, cw, ch, wr.left, wr.top, wr.right, wr.bottom,
                      g_full_w, g_full_h);
            }
        }
    }

    D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(draw_w), static_cast<float>(draw_h),
                       0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(draw_w), static_cast<LONG>(draw_h) };
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &scissor);
    g_list->SetGraphicsRootSignature(g_conv_root.Get());
    g_list->SetPipelineState(g_conv_pso.Get());
    ID3D12DescriptorHeap* heaps[] = { g_srv_heap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetGraphicsRootDescriptorTable(0, g_srv_heap->GetGPUDescriptorHandleForHeapStart());
    float conv_consts[12];
    conv_constants(conv_consts, draw_w, draw_h);
    g_list->SetGraphicsRoot32BitConstants(1, 12, conv_consts, 0);
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->DrawInstanced(3, 1, 0, 0);

    bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bb[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_list->ResourceBarrier(1, &bb[1]);

    bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bb[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_list->ResourceBarrier(1, &bb[0]);
    bb_state = D3D12_RESOURCE_STATE_PRESENT;

    // 4. tag the FG inputs while the list is still open. The producer's resources
    //    live on a heap imported from a file mapping with ALLOW_CROSS_ADAPTER;
    //    XeSS-FG fails to copy those ("Failed to allocate memory for a copy of
    //    DEPTH"), so plain local copies are tagged instead. Those copies pull data
    //    that physically lives on the NVIDIA card across to the AMD one, so both the
    //    recording time and the GPU wait for the previous frame are worth watching.
    {
        const double m0 = qpc_ms();
        if (g_no_inputs) { ensure_dummy_inputs(slot); } else { copy_inputs_local(slot); }
        const double m1 = qpc_ms();
        g_ms_inputs = g_ms_inputs * 0.9 + (m1 - m0) * 0.1;
        if (m1 - m0 > g_max_inputs) { g_max_inputs = m1 - m0; }
    }

    xefg_swapchain_d3d12_resource_data_t rd{};
    rd.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
    rd.resourceBase = { 0, 0 };
    // MV and depth can be different sizes (UE5: MVs at render size, depth at full res), so
    // each is tagged with its own stream size - the old code used streams[1] for both.
#if MGPU_FG_OF
    if (g_fg_of && g_of_ready) {
        // Our own field: 1/8 resolution, screen-UV units, computed from this frame's
        // colour on this GPU (the producer's MV stream measured as garbage).
        rd.resourceSize = { g_of_out_w, g_of_out_h };
        rd.pResource = g_of_mv.Get();
    } else
#endif
    {
        rd.resourceSize = { g_ctl->streams[1].width, g_ctl->streams[1].height };
        rd.pResource = g_mv_local[slot].Get();
    }
    rd.incomingState = D3D12_RESOURCE_STATE_COMMON;
    rd.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
    g_api.D3D12TagFrameResource(g_fg, g_list.Get(), present_id, &rd);

    rd.resourceSize = { g_ctl->streams[2].width, g_ctl->streams[2].height };
    rd.pResource = g_depth_local[slot].Get();
    if (!g_no_depth && g_ring_need > 0 && g_ring_count > 0 && g_depth_ring[0]) {
        UINT back = static_cast<UINT>((g_depth_lag > 0) ? g_depth_lag : 0);
        if (back >= g_ring_count) { back = g_ring_count - 1; }
        const UINT idx = (g_ring_write + g_ring_need - back) % g_ring_need;
        if (g_depth_ring[idx]) {
            rd.pResource = g_depth_ring[idx].Get();
#if MGPU_FG_OF
            g_of_depth_desc = idx;          // guide next frame's upsample with it
#endif
        }
    }
    rd.type = XEFG_SWAPCHAIN_RES_DEPTH;
    g_api.D3D12TagFrameResource(g_fg, g_list.Get(), present_id, &rd);

    // --dump: record the readback copies in this list, i.e. after everything that
    // feeds XeSS-FG and before Close, so they see exactly what was tagged.
    if (g_dump_frames > 0) { dump_record(g_list.Get(), slot, present_id); }

    g_list->Close();
    if (g_xell) { xellAddMarkerData(g_xell, present_id,
                                    XELL_RENDERSUBMIT_START); }
    ID3D12CommandList* lists[] = { g_list.Get() };
    g_queue->ExecuteCommandLists(1, lists);
    if (g_xell) { xellAddMarkerData(g_xell, present_id,
                                    XELL_RENDERSUBMIT_END); }
    g_ms_work = g_ms_work * 0.9 + (qpc_ms() - t_work0) * 0.1;

    // 5. frame constants: jitter from the producer, approximate matrices
    xefg_swapchain_frame_constant_data_t cd{};
    // The projection has to match the aspect the frame was RENDERED at, not the one we
    // capture/present at. They are almost always identical, but the render size is the
    // honest source (and it is the only one that also matches the MV/depth field).
    UINT proj_w = g_ctl->frames[slot].render_w;
    UINT proj_h = g_ctl->frames[slot].render_h;
    if (proj_w < 64 || proj_h < 64) { proj_w = g_width; proj_h = g_height; }
    const float const_aspect = static_cast<float>(proj_w) / static_cast<float>(proj_h);
    // v6: a real camera beats --fg-fov. FSR 3.1 exports near/far/vfov; DLSS-SR does
    // not, so those games keep the --fg-fov / --fg-zn constants.
    const mgpu::FrameShared& fr = g_ctl->frames[slot];
    // Bucket the producer's own present interval exactly like the composition-side arrival
    // histogram, so the two can be compared row by row (see g_game_hist).
    if (fr.frame_time_ms > 0.5f && fr.frame_time_ms < 1000.0f) {
        const double vb_g = refresh_ms();
        if (vb_g > 0.5) {
            int kg = static_cast<int>(static_cast<double>(fr.frame_time_ms) / vb_g + 0.5);
            if (kg < 1) { kg = 1; }
            if (kg > 8) { kg = 8; }
            ++g_game_hist[kg];
        }
    }
    const bool cam_ok = ((g_producer_flags & mgpu::kInputFlagHasCamera) != 0) &&
                        std::isfinite(fr.near_plane) && fr.near_plane > 0.0f &&
                        fr.far_plane > fr.near_plane &&
                        fr.vfov_rad > 0.05f && fr.vfov_rad < 2.5f;
    const float proj_fov = cam_ok ? fr.vfov_rad : (g_fg_fov_y * 3.14159265f / 180.0f);
    const float proj_zn  = cam_ok ? fr.near_plane : g_fg_near_z;
    const float proj_zf  = cam_ok ? fr.far_plane : 10000.0f;
    // An infinite far plane (what a reversed-Z game reports) only makes sense in the
    // reversed-Z form; the finite form divides by infinity and produces NaNs.
    const bool use_rz = g_fg_proj_rz || !std::isfinite(proj_zf);
    if (use_rz) { approx_projection_rz(cd.projectionMatrix, const_aspect, proj_fov, proj_zn); }
    else { approx_projection(cd.projectionMatrix, const_aspect, proj_fov, proj_zn, proj_zf); }
    cd.viewMatrix[0] = 1.0f;
    cd.viewMatrix[5] = 1.0f;
    cd.viewMatrix[10] = 1.0f;
    cd.viewMatrix[15] = 1.0f;
    if (g_fg_no_proj) {
        // Test switch: hand XeFG a plain identity instead of our aspect guess.
        for (int i = 0; i < 16; ++i) { cd.projectionMatrix[i] = 0.0f; }
        cd.projectionMatrix[0] = cd.projectionMatrix[5] = cd.projectionMatrix[10] = 1.0f;
        cd.projectionMatrix[15] = 1.0f;
    }
    // Always the producer's jitter: the game's motion vectors are computed on the jittered
    // render, so this is what XeFG needs to align its history (see g_fg_jitter_zero above).
    cd.jitterOffsetX = g_ctl->frames[slot].jitter_x;
    cd.jitterOffsetY = g_ctl->frames[slot].jitter_y;
    if (g_fg_jitter_flip) {
        cd.jitterOffsetX = -cd.jitterOffsetX;
        cd.jitterOffsetY = -cd.jitterOffsetY;
    }
    // The producer knows how its vectors are scaled (NGX MV.Scale.X/Y; FSR's
    // motionVectorScale is in pixels, e.g. (2880, 1620)). Pass it through instead of
    // assuming 1.0 and use --mv-flip's sign for the (previous-current) convention.
    cd.motionVectorScaleX = g_ctl->frames[slot].mv_scale_x;
    cd.motionVectorScaleY = g_ctl->frames[slot].mv_scale_y;
    if (cd.motionVectorScaleX == 0.0f && cd.motionVectorScaleY == 0.0f) {
        cd.motionVectorScaleX = 1.0f;
        cd.motionVectorScaleY = 1.0f;
    }
    if (g_mv_sign < 0.0f) {
        cd.motionVectorScaleX = -cd.motionVectorScaleX;
        cd.motionVectorScaleY = -cd.motionVectorScaleY;
    }
    cd.resetHistory = 0;
    if (!g_fg_consts_logged) {
        g_fg_consts_logged = true;
        char fov_buf[64]{};
        if (cam_ok) {
            _snprintf_s(fov_buf, sizeof(fov_buf), _TRUNCATE,
                        "camera=producer fov=%.1fdeg zn=%.2f zf=%.0f",
                        proj_fov * 180.0f / 3.14159265f, proj_zn, proj_zf);
        } else {
            _snprintf_s(fov_buf, sizeof(fov_buf), _TRUNCATE,
                        "camera=synthetic (--fg-fov %.1f, --fg-zn %.2f)",
                        g_fg_fov_y, g_fg_near_z);
        }
        logf_("[bridge] frame constants: jitter=(%.4f, %.4f)%s proj=%s %s"
              " aspect=%.5f (%ux%u) mvscale=(%.4f, %.4f) game_dt=%.2fms\n",
              cd.jitterOffsetX, cd.jitterOffsetY,
              g_fg_jitter_flip ? " (flipped)" : "",
              g_fg_no_proj ? "identity"
                           : (use_rz ? "reversed-Z approximation" : "aspect approximation"),
              fov_buf, const_aspect, proj_w, proj_h,
              cd.motionVectorScaleX, cd.motionVectorScaleY,
              static_cast<double>(fr.frame_time_ms));
    }
    // XeFG's pacing model eats this. OptiScaler lets it be the input frame time,
    // our present interval, or zero ([FrameGen] FTInput). We measured that the
    // input frame time works best for 2x; --fg-rt 0 is there to test whether 0
    // stops the library from budgeting the whole batch.
    // --fg-rt-mode picks *which* estimate of the input frame time: the historical
    // EMA (default), the raw last interval, a 16-sample median that ignores WGC's
    // +-1 vblank quantization, or that median snapped to whole vblanks.
    const double rt_ms = fg_render_time_ms();
    cd.frameRenderTime = static_cast<float>(rt_ms);
    // v6 cross-check: the add-on measures the game's own present interval. A large
    // mismatch means our capture cadence is not the game's (WGC batching, a limiter
    // in between), and frameRenderTime comes from *our* estimate - worth one line.
    {
        static unsigned dt_frames = 0;
        static bool dt_warned = false;
        ++dt_frames;
        if (!dt_warned && dt_frames > 120 && fr.frame_time_ms > 1.0f && rt_ms > 1.0 &&
            (fr.frame_time_ms > rt_ms * 1.35 || fr.frame_time_ms < rt_ms * 0.74)) {
            dt_warned = true;
            logf_("[bridge] note: producer frame time %.2f ms vs our capture cadence"
                  " %.2f ms - WGC batching or a limiter between them (frameRenderTime"
                  " uses ours, docs/findings/FINAL_CONFIG.md 5.4)\n",
                  static_cast<double>(fr.frame_time_ms), rt_ms);
            // The dangerous direction: WGC delivers MORE frames than the game renders.
            // That is the signature of the game's own frame generation (DLSS-G / FSR FG):
            // it replaces the present path with its own proxy swap chain, WGC then captures
            // black and the overlay goes black while every counter below stays healthy.
            if (fr.frame_time_ms > rt_ms * 1.35) {
                logf_("[bridge]   WARNING: WGC sees %.1f fps but the game only renders %.1f fps."
                      " That is almost always the game's OWN frame generation (DLSS-G / FSR FG)"
                      " - turn it OFF and keep only the upscaler (docs/findings/FINAL_CONFIG.md 2.1.2),"
                      " otherwise the capture is black even though the pipeline reports SUCCESS\n",
                      1000.0 / rt_ms, 1000.0 / static_cast<double>(fr.frame_time_ms));
            }
        }
    }
    const double fg0 = qpc_ms();
    g_api.TagFrameConstants(g_fg, present_id, &cd);

    g_api.SetPresentId(g_fg, present_id);
    const double fg1 = qpc_ms();
    g_ms_fg = g_ms_fg * 0.9 + (fg1 - fg0) * 0.1;
    if (fg1 - fg0 > g_max_fg) { g_max_fg = fg1 - fg0; }
    if (g_xell) { xellAddMarkerData(g_xell, present_id,
                                    XELL_PRESENT_START); }
    const double t_present = qpc_ms();
    // Present(1,0) blocked for ~31 ms here, capping the whole loop at ~31 fps and
    // competing with the game's own presents. The guide uses sync interval 0, and
    // XeSS-FG does its own pacing anyway.
    g_swapchain->Present(0, g_present_flags());
    const double pres_ms = qpc_ms() - t_present;
    g_present_ms = static_cast<DWORD>(pres_ms + 0.5);
    g_ms_present = g_ms_present * 0.9 + pres_ms * 0.1;
    // Signal after Present so this also covers XeSS-FG's own submissions.
    g_queue->Signal(g_frame_fence.Get(), ++g_frame_fence_value);
    if (g_xell) { xellAddMarkerData(g_xell, present_id,
                                    XELL_PRESENT_END); }

    // --dump: export this frame's colour/depth/MV so the pairing can be checked
    // offline. Placed after Present so the resources a dump reads are the exact
    // ones XeSS-FG was tagged with for this present id.
    if (g_dump_frames > 0) { dump_flush(); }

    // Did this frame's GPU work fit inside the interval the game gives us? When the sum
    // exceeds it we cannot consume frames as fast as they arrive, the WGC pool backs up
    // and frames get lost - which is what a "dip" looks like on screen.
    if (input_interval_ms() > 1.0 &&
        (g_ms_throttle + g_ms_present) > input_interval_ms()) {
        ++g_slow_frames;
    }

    if (!g_reuse_frame) { g_mv_slot = (g_mv_slot + 1) % slots; }
    ++g_frames;

    // Re-assert our z-order every frame batch: the game raises itself on activation.
    assert_topmost();

    // Whole-function duration: whatever is not attributed to a component above shows up
    // here, which is how the "40 ms gap that nothing explains" gets pinned down.
    {
        const double tot = qpc_ms() - t_r0;
        g_ms_total = g_ms_total * 0.9 + tot * 0.1;
        if (tot > g_max_total) { g_max_total = tot; }
    }

    // Every frame, so the on-screen counter is smooth: framesPresented > 1 means
    // XeSS-FG really emitted interpolated frames for that Present, 1 = passthrough.
    xefg_swapchain_present_status_t ps{};
    const xefg_swapchain_result_t sr = g_api.GetLastPresentStatus(g_fg, &ps);
    g_presented_sum += ps.framesPresented;
    g_presented_n   += 1;
    // "xNmiss" counts real frames that produced fewer frames than the configured
    // multiplier, which is how a refused MFG step (old XeLL, model limit) shows up.
    // In --interp-cadence mode the multiplier changes every frame on purpose, so the
    // comparison is meaningless there; the histogram below is the equivalent check.
    if (!g_interp_cadence && ps.framesPresented < g_fg_interp_actual + 1) { ++g_x2_miss; }

    // Did somebody else take over presentation? With N+1 frames per real frame on
    // a vblank-paced display the batch cannot honestly cost much less than the
    // input interval; when it does, Present is returning without waiting (an OSD
    // or latency tool hooking it, or our overlay being occluded) and the picture
    // judders even though xN still reads 4.00.
    //
    // One window is not enough evidence: a catch-up burst (two frames processed
    // back to back after a stall) drops the EMA for a single window and used to
    // trigger this on a perfectly healthy run. Require three in a row.
    if (!g_nopace_warned && g_frames > 200 && ps.framesPresented >= 2 &&
        g_arrival_ms_ema > 8.0 && g_ms_total > 0.0 &&
        g_ms_total < 0.35 * g_arrival_ms_ema) {
        if (++g_nopace_windows >= 3) {
            g_nopace_warned = true;
            logf_("[bridge] WARNING: presentation is not vblank-paced - total=%.1fms but the input"
                  " interval is %.1fms while %u frames are presented per real frame.\n",
                  g_ms_total, g_arrival_ms_ema, ps.framesPresented);
            logf_("[bridge]   something else hooks Present or our overlay is occluded"
                  " (OSD / latency tool / FLM, RTSS, Steam overlay, a topmost window).\n");
            logf_("[bridge]   MFG still produces %u frames, but they go out in bursts, so the"
                  " picture judders while the counter reads %.0f fps.\n",
                  ps.framesPresented, 1000.0 / std::max(g_ms_total, 0.001) * ps.framesPresented);
        }
    } else {
        g_nopace_windows = 0;
    }
    {
        const double nq = qpc_ms();
        double gap = 0.0;
        if (g_last_present_qpc > 0.0) {
            gap = nq - g_last_present_qpc;
            if (gap > g_max_gap) { g_max_gap = gap; }
        }
        g_last_present_qpc = nq;
        if (g_ms_throttle > g_max_gpu) { g_max_gpu = g_ms_throttle; }
        if (g_ms_present > g_max_present) { g_max_present = g_ms_present; }

        // Immediate hitch report. A 40-frame average hides exactly the one-frame stalls
        // the eye notices, so anything 45% over the expected spacing is logged at once
        // (rate limited, so a burst cannot flood the file).
        const double expect = (g_pace_ms > 0.0) ? g_pace_ms : g_arrival_ms_ema;
        // Without pacing the gap simply follows the source, and RTSS makes that
        // alternate between 16.7 and 41.7 ms on a 120 Hz panel - a 1.45x threshold
        // would flag that as a hitch every second. Only report real outliers there.
        const double hitch_limit = expect * ((g_pace_ms > 0.0) ? 1.45 : 2.0);
        if (expect > 1.0 && g_hitch_logged < 60) {
            double worst = gap;
            if (g_ms_throttle > worst) { worst = g_ms_throttle; }
            if (g_ms_present > worst) { worst = g_ms_present; }
            if (worst > hitch_limit) {
                ++g_hitch_logged;
                logf_("[bridge] HITCH gap=%.1fms gpu=%.1fms pres=%.1fms wait=%.1fms"
                      " expect=%.1fms xN=%u slot=%u drop=%u\n",
                      gap, g_ms_throttle, g_ms_present, g_ms_wait, expect,
                      ps.framesPresented, slot, g_arr_drops);
            }
        }
    }
    // `show` is what the OSD and the per-frame log line print as "output fps". It used to be
    // loop_fps * framesPresented, i.e. the *loop* rate (measured with GetTickCount64!) times
    // the last batch size. The moment the loop catches up after a stall - WGC hands us two or
    // three queued frames back to back - that reads 148 or even 164 fps on a 120 Hz panel,
    // which is a number the display can never show and which sent people hunting for a bug
    // that does not exist. The swap chain is the only honest source: count what it really gave
    // the compositor per second, in the report window below (see `out:`).
    if (g_show_fps == 0 && g_frame_ms_ema > 0.1) {
        const double loop_fps = 1000.0 / g_frame_ms_ema;
        const double presented = (ps.framesPresented > 0) ? ps.framesPresented : 1;
        g_show_fps = static_cast<int>(loop_fps * presented + 0.5);   // first window only
    }

    if (g_frames == 30 || g_frames == 200 || g_frames == 600) { dump_zorder(); }

    if ((g_frames % g_report_every) == 1) {
        const ULONGLONG now_ms = GetTickCount64();
        const double win = (g_t_report && now_ms > g_t_report)
                         ? (now_ms - g_t_report) / 1000.0 : 0.0;
        g_t_report = now_ms;
        const double secs = (now_ms / 1000.0) - g_t0;
        logf_("[bridge] frame %llu  slot %u  jitter=(%.3f, %.3f)  (%.1f fps avg, %.1f recent)"
              "  presented=%u fg=%u %s  hittest=%ld fgwin=%d present=%ums bb=%ux%u show=%d\n",
              (unsigned long long)g_frames, slot,
              cd.jitterOffsetX, cd.jitterOffsetY,
              (secs > 0) ? g_frames / secs : 0.0,
              (win > 0.0) ? static_cast<double>(g_report_every) / win : 0.0,
              ps.framesPresented, ps.isFrameGenEnabled,
              (sr == XEFG_SWAPCHAIN_RESULT_SUCCESS) ? xefg_str(ps.frameGenResult) : "status?",
              g_hittest_count,
              (GetForegroundWindow() == g_game_wnd) ? 1 : 0,
              g_present_ms, draw_w, draw_h, g_show_fps);
        const double xn = (g_presented_n > 0) ? g_presented_sum / g_presented_n : 0.0;
        g_presented_sum = 0.0;
        g_presented_n = 0;

        // Cadence control (both flags off by default, so neither the call nor the log
        // line appears in a normal run):
        //   --interp-auto  re-pick the interpolation count so the output lands on whole
        //                  vblanks (hysteresis lives in fg_auto_interp_tick).
        //   --fg-rt-mode   report which frameRenderTime estimate is being fed.
        if (g_interp_auto && !g_lock_auto) { fg_auto_interp_tick("auto (vblank cadence)"); }
        if (g_lock_auto) { fg_solve_cadence("lock-auto"); }
        // --dyn-mfg runs per real frame in run_frame_ex (its holds count frames).
        if (g_fg_rt_mode != kFgRtEma || g_interp_auto || g_lock_auto || g_lock_fps > 1.0 ||
            g_interp_cadence) {
            const double t_in = arr_median_ms();
            const double vb = refresh_ms();
            // In cadence mode the multiplier changes per frame, so report the average
            // this window actually used instead of the last frame's count.
            double mul = static_cast<double>(g_fg_interp_actual + 1);
            if (g_interp_cadence) {
                // g_cad_hist[k] counts the frames that asked for k *interpolated* frames, so
                // the number of output frames per input frame is (k + 1). Forgetting the +1
                // made this line print "avg 1.00x, 60.0 fps out" for a run that was actually
                // doing 2.0x and 120 fps - the most misleading log in the program.
                double sum = 0.0, cnt = 0.0;
                for (int i = 0; i < 8; ++i) {
                    sum += static_cast<double>(i + 1) * g_cad_hist[i];
                    cnt += static_cast<double>(g_cad_hist[i]);
                }
                if (cnt > 0.0) { mul = sum / cnt; }
            }
            const double out_ms = (t_in > 0.0 && mul > 0.0) ? t_in / mul : 0.0;
            const double out_vb = (vb > 0.0) ? out_ms / vb : 0.0;
            // 1.00 / 2.00 / 3.00 vblank per output frame = perfectly even motion;
            // anything else is the pacer alternating short and long holds.
            const double whole = static_cast<double>(static_cast<int>(out_vb + 0.5));
            const double frac = dabs_(out_vb - whole);
            logf_("[bridge]   cadence: rt=%.1fms (%s) input=%.1fms = %.2f vblanks ->"
                  " interp=%u (avg %.2fx, %.1f fps out = %.2f vblank/frame%s)"
                  " refresh=%uHz lock=%.2f drops=%u%s\n",
                  fg_render_time_ms(), fg_rt_mode_name(), t_in, t_in / vb,
                  g_fg_interp_actual, mul,
                  (out_ms > 0.0) ? 1000.0 / out_ms : 0.0, out_vb,
                  (frac > 0.06) ? " NOT-EXACT" : "", g_refresh_hz,
                  g_lock_fps, g_lock_drops,
                  (g_fg_out_hz > 1.0) ? " (out-hz cap)" : "");
            g_lock_drops = 0;
            if (g_interp_cadence) {
                logf_("[bridge]   cadence hist:");
                for (int i = 1; i < 8; ++i) {
                    if (g_cad_hist[i] != 0) { logf_(" %ux=%u", i + 1, g_cad_hist[i]); }
                }
                logf_("  switches=%u refused=%u\n", g_cad_switches, g_cad_fail);
                for (int i = 0; i < 8; ++i) { g_cad_hist[i] = 0; }
                g_cad_switches = 0;
                g_cad_fail = 0;
            } else if (frac > 0.06 && g_refresh_hz > 0) {
                // No VRR: the only fix is locking the input to a divisor of the refresh.
                logf_("[bridge]     whole-vblank at %u Hz needs input %.2f fps (4x) or"
                      " %.2f (3x) or %.2f (2x) - see --list-modes\n",
                      g_refresh_hz, g_refresh_hz / 4.0, g_refresh_hz / 3.0,
                      g_refresh_hz / 2.0);
            }
        }
        const double jit_min = (g_arr_count > 0) ? g_arr_min : 0.0;
        const double jit_max = (g_arr_count > 0) ? g_arr_max : 0.0;
        g_arr_min = 1e9; g_arr_max = 0.0; g_arr_count = 0;
        const UINT drops = g_arr_drops; g_arr_drops = 0;
        const UINT slow = g_slow_frames; g_slow_frames = 0;
        const double budget = (input_interval_ms() > 1.0)
                            ? (g_ms_throttle + g_ms_present) / input_interval_ms() * 100.0 : 0.0;
        const double mx_gpu = g_max_gpu, mx_pres = g_max_present, mx_gap = g_max_gap;
        const double mx_wait = g_max_wait;
        const double mx_cw = g_max_copywait, mx_in = g_max_inputs, mx_tot = g_max_total;
        const double mx_xe = g_max_xell, mx_fg = g_max_fg;
        const double mx_col = g_max_colour; g_max_colour = 0.0;
        const double mx_age = g_max_age; g_max_age = 0.0;
        // Everything run_frame() spends that no other field accounts for: the slot walk,
        // the descriptor/barrier setup, the present bookkeeping and this report itself.
        // If this grows, the next thing to look at is ours, not XeSS-FG's.
        const double accounted = g_ms_colour + g_ms_copywait + g_ms_work + g_ms_wait +
                                 g_ms_inputs + g_ms_of + g_ms_xell + g_ms_fg +
                                 g_ms_throttle + g_ms_present;
        const double cpu_only = (g_ms_total > accounted) ? (g_ms_total - accounted) : 0.0;
        const UINT x2m = g_x2_miss;
        g_hitch_logged = 0;
        g_max_gpu = 0.0; g_max_present = 0.0; g_max_gap = 0.0; g_x2_miss = 0;
        g_max_wait = 0.0; g_max_copywait = 0.0; g_max_total = 0.0; g_max_inputs = 0.0;
        g_max_xell = 0.0; g_max_fg = 0.0;
        const UINT pace_rep = static_cast<UINT>(g_represents); g_represents = 0.0;
        const UINT pace_sk = g_pace_skips; g_pace_skips = 0;

        // Factual one-shot note about who paces the loop: XeFG can only push
        // (interp+1) frames per input frame into the panel's vblanks, so the input rate
        // is topped out at refresh/(interp+1). On a 59 Hz panel at 4x that is ~15 fps
        // no matter how fast the game itself runs.
        // Only worth saying when the panel itself caps the profile (e.g. 59/60 Hz with
        // 4x): on a healthy 120 Hz run the input is *meant* to be 30 fps, so Present
        // blocking ~10 ms of 33 ms is the design, not a problem.
        if (!g_present_cap_warned && g_refresh_hz > 0 && g_refresh_hz < 100 &&
            g_ms_total > 1.0 && g_arrival_ms_ema > 1.0 &&
            g_ms_present > g_ms_total * 0.35 && g_ms_total > g_arrival_ms_ema * 0.85) {
            g_present_cap_warned = true;
            const double in_fps = 1000.0 / g_arrival_ms_ema;
            const double cap = (g_refresh_hz > 0)
                             ? g_refresh_hz / static_cast<double>(g_fg_interp_actual + 1) : 0.0;
            logf_("[bridge] note: Present blocks %.1f of %.1f ms (%.0f%%), so the display side"
                  " paces the loop.", g_ms_present, g_ms_total,
                  g_ms_present / g_ms_total * 100.0);
            if (cap > 0.0) {
                logf_(" %ux output on a %lu Hz panel tops the input out at %.1f fps"
                      " (measured %.1f fps) - lower --interp to go higher, or raise the"
                      " refresh rate.", static_cast<unsigned>(g_fg_interp_actual + 1),
                      static_cast<unsigned long>(g_refresh_hz), cap, in_fps);
            }
            logf_("\n");
        }
        // Cross-adapter budget: the only per-frame traffic that leaves the render card.
        // MV is R16G16 (4 B/px) and depth R32 (4 B/px), and XeSS-FG wants both at the
        // stream size, so this line is exactly what StreamPercent / --no-depth change -
        // it is the number to watch when the goal is "less bus traffic", not `total`.
        {
            const double mv_mb  = static_cast<double>(g_geo.mv_w) * g_geo.mv_h * 4.0 / (1024.0 * 1024.0);
            const double dep_mb = static_cast<double>(g_geo.depth_w) * g_geo.depth_h * 4.0 / (1024.0 * 1024.0);
            const bool send_mv  = !g_no_mv && !g_fg_of && !g_no_inputs;
            const bool send_dep = !g_no_depth && !g_no_inputs;
            const double per_frame = (send_mv ? mv_mb : 0.0) + (send_dep ? dep_mb : 0.0);
            const double rate = (g_arrival_ms_ema > 0.1)
                              ? per_frame * (1000.0 / g_arrival_ms_ema) : 0.0;
            logf_("[bridge]   bus: mv %ux%u%s %.1fMB + depth %ux%u%s %.1fMB = %.1fMB/frame"
                  " (%.1f GB/s at %.1f fps; StreamPercent and --no-depth are the levers)\n",
                  g_geo.mv_w, g_geo.mv_h, send_mv ? "" : " (not sent)", send_mv ? mv_mb : 0.0,
                  g_geo.depth_w, g_geo.depth_h, send_dep ? "" : " (not sent)",
                  send_dep ? dep_mb : 0.0, per_frame, rate * 0.001,
                  (g_arrival_ms_ema > 0.1) ? 1000.0 / g_arrival_ms_ema : 0.0);
        }
        // Arrival histogram: where the input actually comes from. `need` is what the current
        // multiplier requires (interp+1 vblanks per input frame), so a bucket below it means
        // the presenter ran faster than we consume, and a bucket above it means frames were
        // late or gated. `n` is the number of arrivals in this window - if `n` is ~20% short
        // of `frames`, the input itself is not being delivered, not merely reprocessed.
        {
            UINT total_arr = 0;
            for (int k = 1; k <= 8; ++k) { total_arr += g_arr_hist[k]; }
            logf_("[bridge]   arr(vb):");
            for (int k = 1; k <= 8; ++k) {
                if (g_arr_hist[k]) { logf_(" %dx=%u", k, g_arr_hist[k]); }
            }
            const UINT need = g_fg_interp_actual + 1;
            logf_("  n=%u  need %ux (%.1f ms)  frames=%llu  med dwm=%.1f wc=%.1f\n", total_arr,
                  need, need * refresh_ms(), static_cast<unsigned long long>(g_frames),
                  arr_median_ms(), arr_wc_median_ms());
            for (int k = 1; k <= 8; ++k) { g_arr_hist[k] = 0; }
            if (g_fg_rt_mode == kFgRtAuto) {
                const double vb = refresh_ms();
                const double diff = dabs_(g_rt_ema_fast - g_rt_ema_slow);
                const bool moving = diff >= vb * 0.60;
                const int want = moving ? 1 : 0;
                // Decide once per report window (~1 s) and require three windows in a row:
                // the per-frame version flapped the pacer's input 4 times in 18 s.
                if (want != g_rt_auto_state) {
                    if (++g_rt_auto_agree >= 3) {
                        g_rt_auto_state = want;
                        g_rt_auto_agree = 0;
                        ++g_rt_auto_flips;
                        logf_("[bridge] --fg-rt-mode auto: input rate is %s (median %.1f vs"
                              " %.1f ms, %.2f vblanks apart) -> feeding %s\n",
                              moving ? "moving" : "steady", g_rt_ema_fast, g_rt_ema_slow,
                              (vb > 0.5) ? diff / vb : 0.0, moving ? "raw" : "quant");
                    }
                } else {
                    g_rt_auto_agree = 0;
                }
                logf_("[bridge]   rt auto: median %.1f vs %.1f (%.2f vblanks apart) -> %s"
                      "  flips=%u\n", g_rt_ema_fast, g_rt_ema_slow,
                      (vb > 0.5) ? diff / vb : 0.0,
                      (g_rt_auto_state == 1) ? "raw" : "quant", g_rt_auto_flips);
            }
            if (g_spike_count) {
                logf_("[bridge]   rt spikes: %u frame(s) reported at their real length this"
                      " window (--fg-rt-spike %d)\n", g_spike_count, g_fg_rt_spike ? 1 : 0);
                g_spike_count = 0;
            }
            if (g_burst_count) {
                logf_("[bridge]   bursts: %u frame(s) processed faster than 0.6x the input"
                      " interval this window (queued captures drained back to back)\n",
                      g_burst_count);
                g_burst_count = 0;
            }
            // The same buckets for the interval the producer measured between the game's own
            // Present calls. If this is flat while `arr(vb)` is not, the game is presenting
            // evenly and the unevenness appears after it (composition / our overlay).
            UINT game_total = 0;
            for (int k = 1; k <= 8; ++k) { game_total += g_game_hist[k]; }
            logf_("[bridge]   game(vb):");
            for (int k = 1; k <= 8; ++k) {
                if (g_game_hist[k]) { logf_(" %dx=%u", k, g_game_hist[k]); }
            }
            logf_("  n=%u (producer measured)\n", game_total);
            for (int k = 1; k <= 8; ++k) { g_game_hist[k] = 0; }
        }
        // ---- output side: are the generated frames actually reaching the display? -----
        // Nothing above can see this: `xN` is XeFG's own accounting and `cadence` is derived
        // from the input median. Only the swap chain knows what it really handed to the
        // compositor, so count it. `expected` = frames this window * (interp+1) and `vblanks`
        // = refresh periods actually elapsed; presents << expected means XeFG is not
        // emitting the frames it charges us for (the picture judders while every other
        // counter looks perfect), presents > vblanks means they go out in bursts.
        {
            UINT last = 0;
            static UINT s_prev_last = 0;
            if (g_swapchain && SUCCEEDED(g_swapchain->GetLastPresentCount(&last))) {
                const UINT d = (s_prev_last && last >= s_prev_last) ? (last - s_prev_last) : 0;
                s_prev_last = last;
                const double vbs = (win > 0.0) ? win * static_cast<double>(g_refresh_hz) : 0.0;
                const double expect = static_cast<double>(g_report_every) *
                                      static_cast<double>(g_fg_interp_actual + 1);
                logf_("[bridge]   out: presents=%u  expected=%.0f  vblanks=%.0f%s\n",
                      d, expect, vbs,
                      (d > 0 && static_cast<double>(d) + 0.5 < expect)
                          ? "  <== OUTPUT FRAMES MISSING" : "");
                // The honest output rate: what the swap chain actually handed to the
                // compositor per second of this window. This is what `show=` reports now.
                if (win > 0.05 && d > 0) {
                    g_show_fps = static_cast<int>(static_cast<double>(d) / win + 0.5);
                }
            }
            static bool s_stats_failed = false;
            DXGI_FRAME_STATISTICS fs{};
            if (!s_stats_failed) {
                if (g_swapchain && SUCCEEDED(g_swapchain->GetFrameStatistics(&fs))) {
                    static UINT s_pc = 0, s_prc = 0, s_src = 0;
                    const UINT pc  = (fs.PresentCount >= s_pc) ? fs.PresentCount - s_pc : 0;
                    const UINT prc = (fs.PresentRefreshCount >= s_prc)
                                   ? fs.PresentRefreshCount - s_prc : 0;
                    const UINT src = (fs.SyncRefreshCount >= s_src)
                                   ? fs.SyncRefreshCount - s_src : 0;
                    s_pc = fs.PresentCount; s_prc = fs.PresentRefreshCount;
                    s_src = fs.SyncRefreshCount;
                    // presentRefresh lags syncRefresh when the compositor had to skip a
                    // vblank: a *displayed* drop, invisible to every other counter.
                    logf_("[bridge]   out2: count+%u  presentRefresh+%u  syncRefresh+%u%s\n",
                          pc, prc, src, (src > prc + 1) ? "  <== VBLANK MISSED" : "");
                } else {
                    s_stats_failed = true;
                    logf_("[bridge]   out2: GetFrameStatistics unsupported on this swap chain\n");
                }
            }
        }
        logf_("[bridge]   breakdown: capture=%.1fms work=%.1fms present=%.1fms gpu=%.1fms"
              " wait=%.1fms copy=%.1fms colour=%.1fms mvs=%.1fms of=%.1fms age=%.1fms"
              " xell=%.1fms fg=%.1fms total=%.1fms cpu=%.1fms timeouts=%u mode=%d  xN=%.2f"
              "  game=%.1fms  jitter=%.1f..%.1fms  arr_gaps=%u  load=%.0f%%  slow=%u"
              "  pace=%.1fms repres=%u skip=%u"
              "  worst(gpu %.1f/pres %.1f/wait %.1f/copy %.1f/colour %.1f/mvs %.1f/of %.1f/age %.1f"
              "/xell %.1f/fg %.1f/total %.1f/gap %.1f) xNmiss=%u\n",
              g_ms_capture, g_ms_work, g_ms_present, g_ms_throttle, g_ms_wait,
              g_ms_copywait, g_ms_colour, g_ms_inputs, g_ms_of, g_ms_age, g_ms_xell,
              g_ms_fg, g_ms_total, cpu_only,
              g_slot_timeouts, g_colour_mode, xn, g_arrival_ms_ema, jit_min, jit_max,
              drops, budget, slow, g_pace_ms, pace_rep, pace_sk,
              mx_gpu, mx_pres, mx_wait, mx_cw, mx_col, mx_in, g_max_of, mx_age, mx_xe,
              mx_fg, mx_tot, mx_gap, x2m);

        // ---- end-to-end latency estimate, measured from inside the process -----
        //
        // Everything below comes from timestamps we already collect, so nothing has
        // to hook Present (an external tool would disturb presentation and change
        // what it measures - see docs/findings/MFG.md §3.3):
        //
        //   age            WGC's own SystemRelativeTime -> the moment we pick the
        //                  frame up. That is the game compose -> our process hop.
        //   total          our whole run_frame, ending when Present() returned.
        //   scanout        0 .. one refresh interval: the newest frame of the batch
        //                  reaches the panel within one vblank.
        //
        // The game's own input->compose latency is NOT visible here (it needs a
        // click timestamp), so it is stated separately as ">= one input frame".
        if (g_ms_total > 0.0) {
            const double scanout = (g_refresh_hz > 0) ? 1000.0 / g_refresh_hz : 8.33;
            const double input_ms = (g_arrival_ms_ema > 1.0) ? g_arrival_ms_ema : 0.0;
            // --slack holds frames back on purpose; that is latency we added, so it
            // has to show up here instead of being hidden.
            const double queue_ms = (input_ms > 0.0) ? g_slack * input_ms : 0.0;
            const double compose_to_present = g_ms_age + g_ms_total + queue_ms;
            if (queue_ms > 0.0) {
                logf_("[bridge] LATENCY: compose->present %.1fms (game->wgc %.1f + bridge %.1f"
                      " + slack %.1f) | +scanout 0..%.1f => **compose->photons %.1f..%.1fms**\n",
                      compose_to_present, g_ms_age, g_ms_total, queue_ms, scanout,
                      compose_to_present, compose_to_present + scanout);
            } else {
                logf_("[bridge] LATENCY: compose->present %.1fms (game->wgc %.1f + bridge %.1f)"
                      " | +scanout 0..%.1f => **compose->photons %.1f..%.1fms**\n",
                      compose_to_present, g_ms_age, g_ms_total, scanout,
                      compose_to_present, compose_to_present + scanout);
            }
            if (input_ms > 0.0) {
                logf_("[bridge]          + game input->compose >= %.1fms (1 input frame,"
                      " not measurable here) => **est. click->photons >= %.1fms**\n",
                      input_ms, compose_to_present + input_ms);
            }
        }
    }
}


// ------------------------------------------------------------------ fg probe

// Answers "which libxess_fg.dll is loaded, what version is it, and can it do MFG
// here?" without touching the game, the shared memory or a swap chain. It is the
// only way to tell a real MFG build from a file that merely has a different name.
static bool fg_probe()
{
    logf_("\n[bridge] --- libxess_fg probe ---\n");
    logf_("[bridge] probe: adapter    '%ls' %ls vendor=%04X device=%04X luid=%08lX:%08lX\n",
          g_consumer_desc.Description, vendor_name(g_consumer_vendor),
          g_consumer_desc.VendorId, g_consumer_desc.DeviceId,
          static_cast<unsigned long>(g_consumer_desc.AdapterLuid.HighPart),
          static_cast<unsigned long>(g_consumer_desc.AdapterLuid.LowPart));
    if (!g_api.ok()) {
        logf_("[bridge] probe: no DLL loaded\n");
        return false;
    }
    logf_("[bridge] probe: dll        %s\n", narrow_utf8(g_api.path).c_str());
    logf_("[bridge] probe: MFG export %s\n", g_api.has_mfg() ? "yes" : "NO");

    xefg_swapchain_version_t v{};
    if (g_api.GetVersion && g_api.GetVersion(&v) == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        logf_("[bridge] probe: version    %u.%u.%u\n", v.major, v.minor, v.patch);
    } else {
        logf_("[bridge] probe: version    (xefgSwapChainGetVersion failed)\n");
    }

    xefg_swapchain_handle_t ctx = nullptr;
    const xefg_swapchain_result_t cr = g_api.D3D12CreateContext(g_device.Get(), &ctx);
    logf_("[bridge] probe: CreateContext %s (%d)\n", xefg_str(cr), static_cast<int>(cr));
    if (cr != XEFG_SWAPCHAIN_RESULT_SUCCESS || !ctx) { return false; }

    xefg_swapchain_properties_t props{};
    const xefg_swapchain_result_t pr = g_api.GetProperties(ctx, &props);
    logf_("[bridge] probe: GetProperties %s (%d)\n", xefg_str(pr), static_cast<int>(pr));
    if (g_api.Destroy) { g_api.Destroy(ctx); }
    if (pr != XEFG_SWAPCHAIN_RESULT_SUCCESS) { return false; }

    const UINT mx = props.maxSupportedInterpolations ? props.maxSupportedInterpolations : 1;
    logf_("[bridge] probe: maxSupportedInterpolations %u  ->  %ux output\n", mx, mx + 1);

    // xefgSwapChainGetProperties only reports the count before initialization; the
    // heap/descriptor cost comes from the D3D12 variant, which takes the target
    // description. Ask it for the real number so the memory cost of MFG is known
    // up front.
    if (g_api.D3D12GetProperties) {
        xefg_swapchain_handle_t ctx2 = nullptr;
        if (g_api.D3D12CreateContext(g_device.Get(), &ctx2) == XEFG_SWAPCHAIN_RESULT_SUCCESS && ctx2) {
            xefg_swapchain_d3d12_init_params_t ip{};
            ip.maxInterpolatedFrames = mx;
            ip.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_NONE;
            ip.uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO;
            ip.creationNodeMask = 1;
            ip.visibleNodeMask  = 1;
            xefg_swapchain_properties_t p2{};
            const xefg_swapchain_result_t r2 =
                g_api.D3D12GetProperties(ctx2, &ip, 5120, 2880, DXGI_FORMAT_R10G10B10A2_UNORM, &p2);
            logf_("[bridge] probe: D3D12GetProperties(5K, 10bit) %s (%d)\n",
                  xefg_str(r2), static_cast<int>(r2));
            if (r2 == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                logf_("[bridge] probe: descriptors=%u tempBuffer=%.1f MB tempTexture=%.1f MB"
                      " constBuffer=%.1f MB\n",
                      p2.requiredDescriptorCount,
                      p2.tempBufferHeapSize / (1024.0 * 1024.0),
                      p2.tempTextureHeapSize / (1024.0 * 1024.0),
                      p2.constantBufferSize / (1024.0 * 1024.0));
            }
            if (g_api.Destroy) { g_api.Destroy(ctx2); }
        }
    }

    if (mx <= 1) {
        logf_("[bridge] probe: VERDICT 2x only - this DLL clamps MFG on this GPU"
              " (vendor check / model / XeLL).\n");
        logf_("[bridge] probe:   point --fg-lib at an MFG-capable libxess_fg.dll"
              " (+ matching libxell.dll)\n");
    } else {
        logf_("[bridge] probe: VERDICT MFG available, up to %ux (--interp 2..%u = 3x..%ux).\n",
              mx + 1, mx, mx + 1);
        // Exact-cadence reminder: the multiplier is what makes an unlocked refresh
        // reachable without VRR (e.g. 120 Hz = 30x4, 24x5, 20x6).
        logf_("[bridge] probe: at %ux the input rate must be refresh/%u for a whole-vblank"
              " cadence (use --list-modes to see the refresh rates)\n", mx + 1, mx + 1);
    }
    return true;
}


// ------------------------------------------------------------------ MFG patch

// maxSupportedInterpolations as reported *before* any patch (0 = unknown). Only
// used by --fg-probe so the log can show the stock value next to the patched one.
static UINT read_max_interp()
{
    if (!g_api.ok() || !g_device || !g_api.D3D12CreateContext || !g_api.GetProperties) {
        return 0;
    }
    xefg_swapchain_handle_t ctx = nullptr;
    if (g_api.D3D12CreateContext(g_device.Get(), &ctx) != XEFG_SWAPCHAIN_RESULT_SUCCESS || !ctx) {
        return 0;
    }
    xefg_swapchain_properties_t props{};
    const UINT mx = (g_api.GetProperties(ctx, &props) == XEFG_SWAPCHAIN_RESULT_SUCCESS &&
                     props.maxSupportedInterpolations)
                        ? props.maxSupportedInterpolations : 0;
    if (g_api.Destroy) { g_api.Destroy(ctx); }
    return mx;
}

// Lifts the "non-Intel adapter => 1 interpolated frame" clamp (src/xefg_unlock.h,
// table from Universal MFG Unlocker, MIT). The count comes from --interp
// (default 3 = 4x).
//
// Why this is not unconditional any more: the clamp only exists for non-Intel
// adapters, so on an Intel GPU the stock value is already >= 1 *and* the MFG model
// is really there. Patching there would swap real multi-frame generation for
// repeated 2x frames (docs/findings/MFG.md 1). The vendor is known only after the consumer
// adapter is picked, which is why this runs from main() rather than at DLL load.
//
// Returns false only when a requested patch failed (caller decides whether to abort).
static bool apply_unlock_mfg()
{
    // Say it out loud, not only in docs/ops/LEGAL.md: this flag rewrites code inside a
    // commercial DLL while it is loaded. Research use only.
    logf_("[bridge] --unlock-mfg: NOTE - this modifies libxess_fg/libxell machine code in"
          " memory. Personal research only; it may violate the vendor EULA. See"
          " docs/ops/LEGAL.md. The DLLs are not distributed with this project.\n");

    if (!g_api.ok()) {
        logf_("[bridge] --unlock-mfg: no libxess_fg loaded - nothing to patch\n");
        return false;
    }

    if (g_consumer_vendor == 0x8086) {
        logf_("[bridge] --unlock-mfg: consumer adapter is Intel ('%ls') - NOT patching."
              " Its MFG model exists, so this DLL should already allow more than one"
              " interpolated frame\n", g_consumer_desc.Description);
        return true;    // not an error: the native path is the better one
    }

    // --interp 1 asks for 2x, which the stock clamp already allows: nothing to lift.
    // (Apply() only accepts 2..5, so asking it for 1 would fail with "interp out of
    // range" and - worse - leave the caller thinking 4x was applied.)
    const unsigned want = g_fg_interp_set ? static_cast<unsigned>(g_fg_interp) : 3u;
    if (want < 2) {
        logf_("[bridge] --unlock-mfg: %ux requested - the DLL already allows 1"
              " interpolated frame, nothing to patch\n", want + 1);
        return true;
    }

    if (g_fg_probe) {
        const UINT stock = read_max_interp();
        if (stock > 0) {
            logf_("[bridge] probe: maxSupportedInterpolations (stock) %u -> %ux\n",
                  stock, stock + 1);
        }
    }

    // libxell.dll is imported by this exe, so it is already loaded here; libxess_fg
    // loads it by name later and gets the same module.
    HMODULE ll = GetModuleHandleW(L"libxell.dll");
    wchar_t ll_path[MAX_PATH]{};
    if (ll) { GetModuleFileNameW(ll, ll_path, MAX_PATH); }
    logf_("[bridge] --unlock-mfg: target %ux (%u interpolated), libxell=%s\n",
          want + 1, want, ll ? narrow_utf8(ll_path).c_str() : "(not loaded)");

    std::string uerr;
    if (xefg_unlock::Apply(g_api.dll, ll, want, &uerr)) {
        g_fg_interp = static_cast<int>(want);
        g_fg_interp_max = want;
        logf_("[bridge] --unlock-mfg: all 6 sites patched and read back OK\n");
        logf_("[bridge]   every generated frame uses the 2x model (the MFG model is"
              " not in this build), so quality is not 2x quality\n");
        return true;
    }

    logf_("[bridge] --unlock-mfg FAILED: %s\n", uerr.c_str());
    logf_("[bridge]   the patch table is version specific (expects the"
          " libxess_fg 1.3.1.78/1.3.4 family + libxell 1.3.2.x)\n");
    return false;
}

// --------------------------------------------------------------------- main

int main()
{
    // Must happen before any window is created: otherwise Windows virtualizes the
    // window size by the display DPI (175% here) and the client rect no longer
    // matches the 5120x2880 swap chain we ask XeSS-FG for.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (wcsstr(GetCommandLineW(), L"--no-fg")) { g_no_fg = true; }
    if (wcsstr(GetCommandLineW(), L"--no-inputs")) { g_no_inputs = true; }
    if (wcsstr(GetCommandLineW(), L"--no-depth")) { g_no_depth = true; }
    if (wcsstr(GetCommandLineW(), L"--no-mv")) { g_no_mv = true; }
    if (wcsstr(GetCommandLineW(), L"--mv-shadow")) { g_mv_shadow = true; }
    // --log-flush: fflush after every line (pre-change behaviour). Useful when watching
    // for a crash, where the last lines before the process dies are the interesting ones.
    if (wcsstr(GetCommandLineW(), L"--log-flush")) { g_log_flush_each = 1; }
    if (wcsstr(GetCommandLineW(), L"--capture-monitor")) { g_capture_monitor = true; }
    // --overlay-monitor N: present on the Nth display instead of the game window's display.
    // Parsed before the game window is found, because the consumer adapter is chosen by
    // "which adapter drives the overlay monitor".
    if (const wchar_t* om = wcsstr(GetCommandLineW(), L"--overlay-monitor ")) {
        const int n = _wtoi(om + 18);
        if (n >= 0) { g_overlay_monitor = n; }
    }
    // --overlay-hidden: keep presenting, never show the window (measurement only).
    if (wcsstr(GetCommandLineW(), L"--overlay-hidden")) { g_overlay_hidden = true; }

    // --pid N: target a process by id (the GUI knows the window's pid).
    if (const wchar_t* pp = wcsstr(GetCommandLineW(), L"--pid ")) {
        const int n = _wtoi(pp + 6);
        if (n > 0) { g_game_pid = static_cast<DWORD>(n); }
    }
    // --hwnd H: target this exact window (decimal or 0x...). Most precise.
    if (const wchar_t* hp = wcsstr(GetCommandLineW(), L"--hwnd ")) {
        const wchar_t* p = hp + 7;
        while (*p == L' ') { ++p; }
        const unsigned long long v = wcstoull(p, nullptr, 0);
        if (v != 0) { g_game_hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(v)); }
    }
    // --game <exe>: the process whose window we capture and present over.
    if (const wchar_t* gp = wcsstr(GetCommandLineW(), L"--game ")) {
        const wchar_t* p = gp + 7;
        while (*p == L' ' || *p == L'"') { ++p; }
        const wchar_t* end = p;
        const bool quoted = (*(gp + 7) == L'"');
        while (*end && ((quoted && *end != L'"') || (!quoted && *end != L' '))) { ++end; }
        if (end > p) {
            g_game_exe.assign(p, end);
            // accept "PBZ" as well as "PBZ.exe"
            if (g_game_exe.find(L'.') == std::wstring::npos) { g_game_exe += L".exe"; }
            g_game_exe_set = true;
        }
    }
    // --gpu <spec>: which adapter runs XeSS-FG. Default is "the one that drives the
    // game window's monitor", which is also the only one the overlay can present on.
    //   --gpu 0 / 1                          adapter index (EnumAdapters1 order)
    //   --gpu nv / amd / intel               vendor alias
    //   --gpu 10de / 1002 / 8086 / 0x10de    vendor id
    //   --gpu luid:1:2A3B                    exact LUID
    //   --gpu 6750 / 3080                    description substring
    if (const wchar_t* gs = wcsstr(GetCommandLineW(), L"--gpu ")) {
        const wchar_t* p = gs + 6;
        while (*p == L' ') { ++p; }
        const wchar_t* end = p;
        while (*end && *end != L' ') { ++end; }
        if (end > p) { g_gpu_spec.assign(p, end); }
    }
    // --producer-luid HI:LO (hex): the GPU the game renders on, so the pick can avoid
    // it. Protocol v6 publishes this from the add-on; until then it is passed by hand.
    if (const wchar_t* pl = wcsstr(GetCommandLineW(), L"--producer-luid ")) {
        unsigned long long hi = 0, lo = 0;
        if (swscanf_s(pl + 16, L"%llx:%llx", &hi, &lo) == 2) {
            g_producer_luid = (hi << 32) | (lo & 0xFFFFFFFFull);
            g_have_producer = true;
        }
    }
    if (!g_gpu_spec.empty()) {
        logf_("[bridge] --gpu %ls: consumer adapter forced by spec\n", g_gpu_spec.c_str());
    }
    if (g_have_producer) {
        logf_("[bridge] --producer-luid %08lX:%08lX: the pick will avoid the producer's"
              " GPU\n",
              static_cast<unsigned long>(g_producer_luid >> 32),
              static_cast<unsigned long>(g_producer_luid & 0xFFFFFFFFull));
    }
    if (wcsstr(GetCommandLineW(), L"--xell")) { g_no_xell = false; }
    if (wcsstr(GetCommandLineW(), L"--no-xell")) { g_no_xell = true; }
    if (wcsstr(GetCommandLineW(), L"--fg-jitter-flip")) { g_fg_jitter_flip = true; }
    if (wcsstr(GetCommandLineW(), L"--fg-no-proj")) { g_fg_no_proj = true; }
    if (wcsstr(GetCommandLineW(), L"--fg-proj-rz")) { g_fg_proj_rz = true; }
    if (const wchar_t* fv = wcsstr(GetCommandLineW(), L"--fg-fov ")) {
        const float v = static_cast<float>(_wtof(fv + 9));
        if (v > 20.0f && v < 140.0f) { g_fg_fov_y = v; }
    }
    if (const wchar_t* nz = wcsstr(GetCommandLineW(), L"--fg-zn ")) {
        const float v = static_cast<float>(_wtof(nz + 8));
        if (v > 0.001f && v < 1000.0f) { g_fg_near_z = v; }
    }
    // MFG. --interp N is the number of *interpolated* frames per real frame:
    // 1 = 2x, 2 = 3x, 3 = 4x. 0 (default) = the most the loaded DLL allows, which
    // is what we always did (XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES).
    if (const wchar_t* it = wcsstr(GetCommandLineW(), L"--interp ")) {
        const int n = _wtoi(it + 9);
        if (n >= 0 && n <= 7) { g_fg_interp = n; g_fg_interp_set = (n > 0); }
    }
    if (const wchar_t* fl = wcsstr(GetCommandLineW(), L"--fg-lib ")) {
        const wchar_t* p = fl + 9;
        while (*p == L' ' || *p == L'"') { ++p; }
        const wchar_t* end = p;
        bool quoted = (*(fl + 9) == L'"');
        while (*end && ((quoted && *end != L'"') || (!quoted && *end != L' '))) { ++end; }
        if (end > p) { g_fg_lib.assign(p, static_cast<size_t>(end - p)); }
    }
    if (wcsstr(GetCommandLineW(), L"--fg-probe")) { g_fg_probe = true; }
    if (wcsstr(GetCommandLineW(), L"--unlock-mfg")) { g_unlock_mfg = true; }
    // XeFG tuning (see the globals above).
    if (const wchar_t* rt = wcsstr(GetCommandLineW(), L"--fg-rt ")) {
        if (wcsncmp(rt + 8, L"0", 1) == 0) { g_fg_rt_zero = true; }
    }
    if (wcsstr(GetCommandLineW(), L"--fg-ui none")) { g_fg_ui_none = true; }
    if (const wchar_t* sc = wcsstr(GetCommandLineW(), L"--fg-scene ")) {
        const float v = static_cast<float>(_wtof(sc + 11));
        if (v > 0.0f && v <= 10.0f) { g_fg_scene = v; }
    }
    if (wcsstr(GetCommandLineW(), L"--fg-depth-inv")) { g_fg_depth_inv = true; }
    // --probe-colour N: log whether the captured colour is black every N frames.
    if (const wchar_t* pcx = wcsstr(GetCommandLineW(), L"--probe-colour ")) {
        const int n = _wtoi(pcx + 15);
        if (n > 0) { g_probe_colour = n; }
    }
    // --jbuf N: our own jitter buffer (see the comment next to g_jbuf).
    if (const wchar_t* jb = wcsstr(GetCommandLineW(), L"--jbuf ")) {
        const int n = _wtoi(jb + 7);
        if (n >= 0 && n <= 3) { g_jbuf = n; }
    }
    // --slack N: see the comment next to g_slack.
    if (const wchar_t* sl = wcsstr(GetCommandLineW(), L"--slack ")) {
        const int n = _wtoi(sl + 8);
        if (n >= 0 && n <= 3) { g_slack = static_cast<UINT>(n); }
    }
    if (wcsstr(GetCommandLineW(), L"--tearing")) { g_tearing = true; }
    if (wcsstr(GetCommandLineW(), L"--overlay-layered")) { g_overlay_layered = true; }
    if (wcsstr(GetCommandLineW(), L"--relay-input")) { g_relay_input = true; }
    if (wcsstr(GetCommandLineW(), L"--no-osd")) { g_no_osd = true; }
    if (const wchar_t* el = wcsstr(GetCommandLineW(), L"--exit-on-focus-loss ")) {
        const int n = _wtoi(el + 22);
        if (n >= 0 && n <= 3600) { g_exit_after_s = static_cast<UINT>(n); }
    }
    if (const wchar_t* dl = wcsstr(GetCommandLineW(), L"--depth-lag ")) {
        const int n = _wtoi(dl + 12);
        if (n >= 0 && n < static_cast<int>(mgpu::kMaxSlots)) { g_depth_lag = n; }
    }
    if (wcsstr(GetCommandLineW(), L"--fg-jitter-zero")) {
        g_fg_jitter_zero = true;      // kept for compatibility only
        logf_("[bridge] --fg-jitter-zero is DEPRECATED and ignored: the game's jitter is"
              " always passed now (measured better - it removed the flicker and most of"
              " the model-edge ghosting). Drop the flag at your convenience.\n");
    }
    if (wcsstr(GetCommandLineW(), L"--fg-debug-tag-interp")) { g_fg_tag_interp = true; }
    if (wcsstr(GetCommandLineW(), L"--fg-debug-only-interp")) { g_fg_only_interp = true; }
    // --list-modes: print the refresh rates this output supports and the exact input
    // rate for every output multiplier, then exit. No game or shared memory needed.
    if (wcsstr(GetCommandLineW(), L"--list-modes")) { return list_display_modes(); }
    // --fg-rt-mode ema|raw|median|quant: which estimate of the input frame time goes
    // into frameRenderTime (XeFG's presentation-pacing sanity check).
    if (const wchar_t* rm = wcsstr(GetCommandLineW(), L"--fg-rt-mode ")) {
        const wchar_t* p = rm + 13;
        while (*p == L' ') { ++p; }
        if (wcsncmp(p, L"raw", 3) == 0)         { g_fg_rt_mode = kFgRtRaw; }
        else if (wcsncmp(p, L"median", 6) == 0) { g_fg_rt_mode = kFgRtMedian; }
        else if (wcsncmp(p, L"auto", 4) == 0)   { g_fg_rt_mode = kFgRtAuto; }
        else if (wcsncmp(p, L"quant", 5) == 0)  { g_fg_rt_mode = kFgRtQuant; }
        else                                    { g_fg_rt_mode = kFgRtEma; }
    }
    // --fg-rt-spike 0|1: absorb single-frame input spikes by reporting the real interval to
    // XeFG for that frame (default on; --fg-rt-spike 0 disables it for A/B).
    if (const wchar_t* sp = wcsstr(GetCommandLineW(), L"--fg-rt-spike ")) {
        g_fg_rt_spike = (_wtoi(sp + 14) != 0);
    }
    // --cadence-hold N: frames to hold the interpolated count (0/1 = change every frame).
    if (const wchar_t* ch = wcsstr(GetCommandLineW(), L"--cadence-hold ")) {
        const int n = _wtoi(ch + 15);
        if (n >= 0 && n <= 240) { g_cad_hold_frames = n; }
    }
    // --fg-cad-rt 0|1: see fg_render_time_ms(). Default 1 (batch-derived frameRenderTime).
    if (const wchar_t* cr = wcsstr(GetCommandLineW(), L"--fg-cad-rt ")) {
        g_fg_cad_rt = (_wtoi(cr + 12) != 0);
    }
    // --fg-out-hz N: cap the output frame rate (only with --interp-cadence). 0 = panel.
    if (const wchar_t* oh = wcsstr(GetCommandLineW(), L"--fg-out-hz ")) {
        const int n = _wtoi(oh + 12);
        if (n >= 5 && n <= 1000) { g_fg_out_hz = n; }
    }
    // --interp-auto [floor|round]: pick the interpolation count from the measured
    // input cadence so the output stays on whole vblanks (dynamic frame times).
    if (wcsstr(GetCommandLineW(), L"--interp-auto")) {
        g_interp_auto = true;
        if (const wchar_t* ia = wcsstr(GetCommandLineW(), L"--interp-auto ")) {
            const wchar_t* p = ia + 14;
            while (*p == L' ') { ++p; }
            g_interp_auto_round = (wcsncmp(p, L"round", 5) == 0);
        }
    }
    // --interp-cadence: variable-rate generation - pick the multiplier per real frame
    // from the elapsed vblanks so the output keeps one frame per vblank (no lock, no VRR).
    if (wcsstr(GetCommandLineW(), L"--interp-cadence")) { g_interp_cadence = true; }
    // --dyn-mfg: OptiScaler-style Dynamic MFG (auto 1x..6x to hit a presented-fps target).
    if (wcsstr(GetCommandLineW(), L"--dyn-mfg")) { g_dyn_mfg = true; }
    if (const wchar_t* ft = wcsstr(GetCommandLineW(), L"--fg-target ")) {
        const double v = _wtof(ft + 12);
        if (v >= 10.0 && v <= 1000.0) { g_dyn_target = v; }
    }
    if (const wchar_t* dh = wcsstr(GetCommandLineW(), L"--fg-round ")) {
        const int n = _wtoi(dh + 11);
        if (n >= 0 && n <= 2) { g_dyn_rounding = n; }   // 0 at most, 1 nearest, 2 at least
    }
    if (wcsstr(GetCommandLineW(), L"--fg-allow-1x off")) { g_dyn_allow1x = false; }
    // --lock N: enforce the input rate at N fps by dropping captured frames.
    if (const wchar_t* lk = wcsstr(GetCommandLineW(), L"--lock ")) {
        const double v = _wtof(lk + 7);
        if (v >= 5.0 && v <= 1000.0) { g_lock_fps = v; }
    }
    // --lock-auto: solve (rate, multiplier) so that rate*(interp+1) divides the refresh.
    if (wcsstr(GetCommandLineW(), L"--lock-auto")) { g_lock_auto = true; }
    // Echo the cadence knobs before anything can fail, so the log always says which
    // cadence model ran (both default to the old behaviour and stay silent).
    if (g_fg_rt_mode != kFgRtEma) {
        const char* what = (g_fg_rt_mode == kFgRtQuant) ? "median, snapped to whole vblanks"
                          : (g_fg_rt_mode == kFgRtRaw)  ? "the raw last input interval"
                                                        : "median of the last 16 input intervals";
        logf_("[bridge] --fg-rt-mode %s: frameRenderTime = %s\n", fg_rt_mode_name(), what);
    }
    if (g_interp_auto) {
        logf_("[bridge] --interp-auto (%s): the interpolation count follows the measured"
              " input cadence so the output stays on whole vblanks\n",
              g_interp_auto_round ? "round" : "floor");
    }
    if (g_lock_auto) {
        logf_("[bridge] --lock-auto: solving (input lock, multiplier) so that"
              " input*(interp+1) divides the refresh - no RTSS, no VRR needed\n");
    }
    if (g_dyn_mfg) {
        static const char* roundings[] = { "at most", "nearest", "at least" };
        logf_("[bridge] --dyn-mfg: OptiScaler's Dynamic MFG - target %s, rounding %s,"
              " 1X %s; the count follows the measured frame time and the learned cost of"
              " a generated frame\n",
              (g_dyn_target > 0.0) ? "from --fg-target" : "just under the refresh",
              roundings[(g_dyn_rounding < 0 || g_dyn_rounding > 2) ? 1 : g_dyn_rounding],
              g_dyn_allow1x ? "allowed" : "not allowed");
    }
    if (g_lock_fps > 1.0) {
        logf_("[bridge] --lock %.2f fps: captured frames that arrive early are dropped so"
              " the forwarded input rate is exactly this\n", g_lock_fps);
    }
    if (g_interp_cadence) {
        logf_("[bridge] --interp-cadence: variable-rate generation - the multiplier follows"
              " the vblanks between real frames (output runs at the panel refresh, no lock"
              " needed; the refresh is detected during FG init)\n");
        // Measured 2026-11-07 (bridge_pbz_cadence.txt): this made things *worse*
        // (input collapsed 26 -> 63-78 ms, output 63-79 fps) because the arrival
        // interval it consumes is our own throttle, not the game's frame time, and the
        // per-frame count change defeats the pacer's history. Kept for reference.
        logf_("[bridge]   WARNING: measured worse than a constant multiplier on this DLL"
              " - see docs/findings/FINAL_CONFIG.md 9.18 before relying on it\n");
    }
#if MGPU_FG_OF
    if (wcsstr(GetCommandLineW(), L"--fg-of")) { g_fg_of = true; }
    if (const wchar_t* os_ = wcsstr(GetCommandLineW(), L"--fg-of-sign ")) {
        const double v = _wtof(os_ + 13);
        if (v == 1.0 || v == -1.0) { g_of_sign = static_cast<float>(v); }
    }
    if (const wchar_t* osc = wcsstr(GetCommandLineW(), L"--fg-of-scale ")) {
        const double v = _wtof(osc + 14);
        if (v > 0.0 && v < 1000.0) { g_of_scale = static_cast<float>(v); }
    }
    if (const wchar_t* ou = wcsstr(GetCommandLineW(), L"--fg-of-units ")) {
        const wchar_t* p = ou + 14;
        while (*p == L' ') { ++p; }
        if (wcsncmp(p, L"mvpx", 4) == 0) { g_of_units = 3; }
        else if (wcsncmp(p, L"uv", 2) == 0) { g_of_units = 1; }
        else if (wcsncmp(p, L"ndc", 3) == 0) { g_of_units = 2; }
        else { g_of_units = 0; }             // px
    }
    if (const wchar_t* oc = wcsstr(GetCommandLineW(), L"--fg-of-const ")) {
        double a = 0.0, b = 0.0;
        if (swscanf_s(oc + 14, L"%lf %lf", &a, &b) == 2) {
            g_of_const_x = static_cast<float>(a);
            g_of_const_y = static_cast<float>(b);
            g_of_const_on = true;
        }
    }
    if (const wchar_t* dk = wcsstr(GetCommandLineW(), L"--fg-of-depth-k ")) {
        const double v = _wtof(dk + 15);
        if (v >= 0.0 && v <= 100000.0) { g_of_depth_k = static_cast<float>(v); }
    }
    if (const wchar_t* od = wcsstr(GetCommandLineW(), L"--fg-of-deadzone ")) {
        const double v = _wtof(od + 17);
        if (v >= 0.0 && v <= 64.0) { g_of_deadzone = static_cast<float>(v); }
    }
    if (const wchar_t* og = wcsstr(GetCommandLineW(), L"--fg-of-guide ")) {
        const double v = _wtof(og + 14);
        if (v >= 0.0 && v < 10000.0) { g_of_guide = static_cast<float>(v); }
    }
#else
    // The flow pass is compiled out - say so instead of silently ignoring the switch
    // (a silent no-op is exactly how a dead input wastes a whole experiment round).
    if (wcsstr(GetCommandLineW(), L"--fg-of")) {
        logf_("[bridge] --fg-of ignored: not compiled in (MV is a dead input for"
              " libxess_fg 1.3.1; rebuild with /DMGPU_FG_OF=1 to re-test)\n");
    }
#endif
    if (const wchar_t* dp = wcsstr(GetCommandLineW(), L"--dump ")) {
        const int n = _wtoi(dp + 7);
        if (n > 0 && n <= 64) { g_dump_frames = n; }
    }
    if (const wchar_t* da = wcsstr(GetCommandLineW(), L"--dump-after ")) {
        const int n = _wtoi(da + 13);
        if (n >= 0 && n <= 100000) { g_dump_after = n; }
    }
    if (const wchar_t* dd = wcsstr(GetCommandLineW(), L"--dump-dir ")) {
        const wchar_t* p = dd + 11;
        while (*p == L' ' || *p == L'"') { ++p; }
        const wchar_t* end = p;
        const bool quoted = (*(dd + 11) == L'"');
        while (*end && ((quoted && *end != L'"') || (!quoted && *end != L' '))) { ++end; }
        if (end > p) {
            const int len = WideCharToMultiByte(CP_ACP, 0, p, static_cast<int>(end - p),
                                                g_dump_dir, MAX_PATH - 1, nullptr, nullptr);
            if (len > 0) { g_dump_dir[len] = '\0'; }
        }
    }
    if (g_dump_frames > 0) {
        CreateDirectoryA(g_dump_dir, nullptr);
        logf_("[bridge] --dump %d frame(s) to %s (colour/depth/MV raw + meta)\n",
              g_dump_frames, g_dump_dir);
        logf_("[bridge]   run with real inputs (no --no-mv/--no-depth) or the dump"
              " cannot answer the pairing question\n");
    }
    if (wcsstr(GetCommandLineW(), L"--no-throttle")) { g_no_throttle = true; }
    if (const wchar_t* wm = wcsstr(GetCommandLineW(), L"--wait-ms ")) {
        const int n = _wtoi(wm + 10);
        if (n >= 0 && n <= 200) { g_wait_ms = static_cast<DWORD>(n); }
    }
    if (const wchar_t* im = wcsstr(GetCommandLineW(), L"--input-ms ")) {
        const double v = _wtof(im + 11);
        if (v >= 1.0 && v <= 200.0) {
            g_input_ms_override = v;
            logf_("[bridge] --input-ms %.2f: the pacing model treats the input interval as"
                  " %.2f ms (measured cadence is still reported as game=. This does not"
                  " create GPU time - it only changes the slow/load accounting and what"
                  " frameRenderTime XeFG sees)\n", v, v);
        }
    }
    if (const wchar_t* rp = wcsstr(GetCommandLineW(), L"--report ")) {
        const int n = _wtoi(rp + 9);
        if (n >= 5 && n <= 400) { g_report_every = static_cast<UINT>(n); }
    }
    if (const wchar_t* pc = wcsstr(GetCommandLineW(), L"--pace ")) {
        if (wcsncmp(pc + 7, L"auto", 4) == 0) {
            // Follow the source: pacing below the real arrival rate throws frames away,
            // which is worse than no pacing at all.
            g_pace_auto = true;
            g_pace_ms = 1.0;
        } else {
            const int hz = _wtoi(pc + 7);
            if (hz >= 10 && hz <= 240) { g_pace_ms = 1000.0 / static_cast<double>(hz); }
        }
    }
    if (const wchar_t* pl = wcsstr(GetCommandLineW(), L"--pool ")) {
        const int n = _wtoi(pl + 7);
        if (n >= 2 && n <= 8) { g_wgc_pool = static_cast<UINT>(n); }
    }
    if (const wchar_t* lm = wcsstr(GetCommandLineW(), L"--latency ")) {
        const int n = _wtoi(lm + 10);
        if (n >= 1 && n <= 4) { g_max_latency = static_cast<UINT>(n); }
    }
    if (const wchar_t* xm = wcsstr(GetCommandLineW(), L"--xell-mode ")) {
        const int n = _wtoi(xm + 12);
        if (n >= 0 && n <= 1) { g_xell_mode = n; }
    }
    if (const wchar_t* bu = wcsstr(GetCommandLineW(), L"--buffers ")) {
        const int n = _wtoi(bu + 10);
        if (n >= 2 && n <= 4) { g_swap_buffers = static_cast<UINT>(n); }
    }
    if (wcsstr(GetCommandLineW(), L"--sdr")) { g_colour_mode = 2; }
    if (wcsstr(GetCommandLineW(), L"--no-primaries")) { g_colour_mode = 1; }
    if (wcsstr(GetCommandLineW(), L"--half")) { g_half = true; g_out_den = 2; }
    if (wcsstr(GetCommandLineW(), L"--fg")) {
        g_fg_warp = wcsstr(GetCommandLineW(), L"--fg warp") != nullptr;
    }
    if (const wchar_t* mm = wcsstr(GetCommandLineW(), L"--mv-mul ")) {
        g_mv_mul = static_cast<float>(_wtof(mm + 9));
    }
    if (wcsstr(GetCommandLineW(), L"--mv-flip")) { g_mv_sign = -1.0f; }
    if (const wchar_t* wcf = wcsstr(GetCommandLineW(), L"--warp-conf ")) {
        g_warp_conf = static_cast<float>(_wtof(wcf + 12));
        if (g_warp_conf < 0.0f) { g_warp_conf = 0.0f; }
        if (g_warp_conf > 4000.0f) { g_warp_conf = 4000.0f; }
    }
    if (const wchar_t* wsr = wcsstr(GetCommandLineW(), L"--warp-search ")) {
        g_warp_search = static_cast<float>(_wtof(wsr + 14));
        if (g_warp_search < 0.0f) { g_warp_search = 0.0f; }
        if (g_warp_search > 16.0f) { g_warp_search = 16.0f; }
    }
    if (const wchar_t* wcl = wcsstr(GetCommandLineW(), L"--warp-clamp ")) {
        g_warp_clamp = static_cast<float>(_wtof(wcl + 13));
        if (g_warp_clamp < 0.0f) { g_warp_clamp = 0.0f; }
        if (g_warp_clamp > 1.0f) { g_warp_clamp = 1.0f; }
    }
    if (g_fg_warp) {
        // Our interpolator works at the captured size: no scaling anywhere.
        g_out_den = 1;
        g_scale = 0.0f;
        g_no_fg = true;   // XeSS-FG is replaced by the warp pass
    }
    if (const wchar_t* sp = wcsstr(GetCommandLineW(), L"--sharp")) {
        g_sharpen = static_cast<float>(_wtof(sp + 7));
        if (g_sharpen < 0.0f) { g_sharpen = 0.0f; }
        if (g_sharpen > 2.0f) { g_sharpen = 2.0f; }
    }
    // --down N: kernel used by the resize that runs before frame generation.
    if (const wchar_t* dn = wcsstr(GetCommandLineW(), L"--down ")) {
        const int n = _wtoi(dn + 7);
        if (n >= 0 && n <= 3) { g_down = n; }
    }
    if (const wchar_t* sp2 = wcsstr(GetCommandLineW(), L"--scale ")) {
        g_scale = static_cast<float>(_wtof(sp2 + 8));
        if (g_scale < 0.25f) { g_scale = 0.0f; }
        if (g_scale > 1.0f)  { g_scale = 1.0f; }
    }
    if (const wchar_t* d = wcsstr(GetCommandLineW(), L"--den ")) {
        const int n = _wtoi(d + 6);
        if (n >= 1 && n <= 8) { g_out_den = n; }
    }
    // Kernel support per --down mode (in output-pixel units). support 0 keeps the
    // single bilinear tap; the shader stretches whatever support it is given.
    switch (g_down) {
        case 1: g_down_support = 2.0f; break;   // Catmull-Rom      (support 2)
        case 2: g_down_support = 2.0f; break;   // Mitchell B=C=1/3 (support 2)
        case 3: g_down_support = 3.0f; break;   // Lanczos-3        (support 3)
        default: g_down = 0; g_down_support = 0.0f; break;
    }

    logf_("[bridge] mode: fg=%s den=%d scale=%.2f buffers=%u pool=%u latency=%u tearing=%d"
          " xell=%s(mode %d) inputs=%s sharp=%.2f colour=%d interp=%d unlock=%d down=%d\n",
          g_fg_warp ? "warp" : (g_no_fg ? "off" : "xess-fg"), g_out_den, g_scale,
          g_swap_buffers, g_wgc_pool, g_max_latency, g_tearing ? 1 : 0,
          g_no_xell ? "no-sleep" : "on", g_xell_mode,
          g_no_inputs ? "off" : "on", g_sharpen, g_colour_mode, g_fg_interp,
          g_unlock_mfg ? 1 : 0, g_down);
    if (g_overlay_monitor >= 0 || g_overlay_hidden) {
        logf_("[bridge] overlay: monitor=%s hidden=%d (capture stays %s)\n",
              (g_overlay_monitor >= 0) ? "explicit" : "game window",
              g_overlay_hidden ? 1 : 0,
              g_capture_monitor ? "monitor" : "window");
    }

    if (g_game_hwnd != nullptr || g_game_pid != 0) {
        if (g_game_hwnd != nullptr) {
            logf_("[bridge] target window %p (--hwnd)\n", (void*)g_game_hwnd);
            if (!IsWindow(g_game_hwnd)) { g_game_hwnd = nullptr; }
        } else {
            logf_("[bridge] target process %lu (--pid)\n", (unsigned long)g_game_pid);
        }
    } else if (g_game_exe_set) {
        logf_("[bridge] game process: %ls (--game)\n", g_game_exe.c_str());
    } else {
        logf_("[bridge] no --game/--hwnd/--pid: using the foreground window\n");
    }

    {
        static const char* k_down_names[] = { "bilinear (1 tap)",
                                              "catmull-rom (support 2)",
                                              "mitchell B=C=1/3 (support 2)",
                                              "lanczos-3 (support 3)" };
        const int i = (g_down >= 0 && g_down <= 3) ? g_down : 0;
        if (g_down == 0) {
            logf_("[bridge] resize filter: %s - add --down 1 (catmull-rom) for a real"
                  " low-pass; --down 2 soft/no ringing, --down 3 sharpest\n", k_down_names[i]);
        } else {
            logf_("[bridge] resize filter: %s, taps follow the scale at draw time\n",
                  k_down_names[i]);
        }
    }

    if (g_fg_rt_zero || g_fg_ui_none || g_fg_scene > 0.0f || g_fg_depth_inv) {
        logf_("[bridge] XeFG tuning: rt=%s ui=%s scene=%.2f depthInv=%d\n",
              g_fg_rt_zero ? "zero" : "input",
              g_fg_ui_none ? "none" : "auto",
              g_fg_scene, g_fg_depth_inv ? 1 : 0);
    }
    if (g_slack > 0) {
        logf_("[bridge] --slack %u: holding up to %u captured frames, processing the oldest"
              " (adds up to %u input intervals of latency)\n", g_slack, g_slack + 1, g_slack);
        if (g_wgc_pool < g_slack + 2) {
            logf_("[bridge]   note: --pool %u is smaller than slack+2; the WGC pool caps how"
                  " much can be held\n", g_wgc_pool);
        }
    }

    // libxess_fg.dll is loaded here rather than through an import library, so the
    // file can be chosen (--fg-lib) and its real version and MFG capability can be
    // reported before anything is initialised.
    //
    // This happens even for --fg warp: that path replaces the interpolator but still
    // presents through XeSS-FG's proxy swap chain, so the DLL is required either way.
    {
        std::string fg_err;
        if (xefg_dyn::Load(g_api, g_fg_lib, &fg_err)) {
            logf_("[bridge] libxess_fg: %s\n", narrow_utf8(g_api.path).c_str());
            xefg_swapchain_version_t v{};
            if (g_api.GetVersion && g_api.GetVersion(&v) == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                logf_("[bridge] libxess_fg version %u.%u.%u, SetNumInterpolatedFrames=%s\n",
                      v.major, v.minor, v.patch, g_api.has_mfg() ? "yes" : "NO");
            }
        } else {
            logf_("[bridge] libxess_fg.dll NOT loaded: %s\n", fg_err.c_str());
            logf_("[bridge]   the proxy swap chain needs it even with --fg warp;"
                  " pick a build with --fg-lib <path>\n");
            if (!g_fg_probe) { return 11; }
        }
    }

    // --unlock-mfg used to be applied right here. It moved after adapter selection
    // (apply_unlock_mfg(), see main()) because the patch must NOT run on an Intel
    // adapter: there the clamp is not in the way and the native MFG model exists, so
    // patching would replace real multi-frame generation with repeated 2x frames
    // (docs/findings/MFG.md 1). The vendor is only known once the consumer adapter is picked.

    if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) {
        logf_("[bridge] RoInitialize FAILED\n");
        return 1;
    }
    enable_fine_timer();
    g_t0 = GetTickCount64() / 1000.0;

    // --fg-probe only asks the loaded DLL what it supports, so it needs neither the
    // game nor the shared memory - it can be run on its own to find out whether the
    // community MFG build is really in place. --fg-of-selftest is the same idea for
    // the optical-flow bring-up (only exists when MGPU_FG_OF=1).
#if MGPU_FG_OF
    const bool of_selftest_mode = (wcsstr(GetCommandLineW(), L"--fg-of-selftest") != nullptr);
#else
    const bool of_selftest_mode = false;
#endif
    HWND game_wnd = nullptr;
    if (!g_fg_probe && !of_selftest_mode) {
        if (g_game_hwnd != nullptr && IsWindow(g_game_hwnd)) {
            game_wnd = g_game_hwnd;
        } else if (g_game_pid != 0) {
            game_wnd = find_window_of_pid(g_game_pid);
        } else if (g_game_exe_set) {
            game_wnd = find_game_window(g_game_exe.c_str());
        } else {
            // Nothing was named: take whatever the user is looking at. This is what makes
            // a bare `bridge.exe` (or a double-clicked recipe) work; the old code fell
            // back to a hard-coded Cyberpunk2077.exe, which only worked on one machine.
            game_wnd = GetForegroundWindow();
            logf_("[bridge] target: foreground window %p\n", (void*)game_wnd);
        }
        if (!game_wnd) {
            if (g_game_pid != 0) {
                logf_("[bridge] no visible window for pid %lu\n", (unsigned long)g_game_pid);
            } else if (g_game_exe_set) {
                logf_("[bridge] game window not found - is %ls running?\n", g_game_exe.c_str());
            } else {
                logf_("[bridge] no foreground window - click into the game first,"
                      " or pass --game <exe> / --hwnd <hex>\n");
            }
            return 2;
        }
        logf_("[bridge] game window %p\n", (void*)game_wnd);
        g_game_wnd = game_wnd;

        {
            // If the game window itself is topmost, that explains why it covered us.
            const LONG_PTR ex = GetWindowLongPtrW(g_game_wnd, GWL_EXSTYLE);
            RECT gr{};
            GetWindowRect(g_game_wnd, &gr);
            logf_("[bridge] game window style=0x%08lX ex=0x%08lX topmost=%d rect=(%ld,%ld)-(%ld,%ld)%s\n",
                  static_cast<unsigned long>(GetWindowLongPtrW(g_game_wnd, GWL_STYLE)),
                  static_cast<unsigned long>(ex), (ex & WS_EX_TOPMOST) ? 1 : 0,
                  gr.left, gr.top, gr.right, gr.bottom,
                  (ex & WS_EX_TOPMOST)
                      ? "   (already topmost: possibly left over from an earlier run,"
                        " cleared on exit)"
                      : "");
        }
    } else {
        logf_("[bridge] --fg-probe: no game window needed\n");
    }

    // Protocol v6: if the add-on is already running it publishes the adapter it
    // renders on, so "pick the other GPU" works without --producer-luid. A command
    // line --producer-luid still wins (peek only runs when it was not given).
    if (!g_have_producer && !g_fg_probe) { peek_producer(); }

    // Which GPU runs XeSS-FG. Not "the AMD card" any more: XeFG is cross-vendor and
    // the only hard constraint is that the adapter drives the monitor the overlay
    // sits on (docs/design/VENDOR_COMPAT_PLAN.md M6.1). --gpu forces one; --producer-luid
    // says which card to avoid.
    const AdapterPick pick = find_consumer_adapter(game_wnd);
    if (!pick.adapter) {
        logf_("[bridge] no usable consumer adapter (only software adapters, or --gpu"
              " matched nothing)\n");
        logf_("[bridge]   choose one explicitly: --gpu <index|nv|amd|intel|"
              "luid:HI:LO|name substring>\n");
        return 3;
    }
    g_consumer_desc   = pick.desc;
    g_consumer_vendor = pick.desc.VendorId;
    g_consumer_adapter = pick.adapter;

    if (!init_devices(pick.adapter.Get())) { return 4; }

    // The vendor is known now, so the MFG patch can run - or deliberately not run on
    // an Intel adapter, where the native MFG model exists (docs/findings/MFG.md 1).
    if (g_unlock_mfg && !apply_unlock_mfg() && !g_fg_probe) { return 13; }

    if (g_fg_probe) { return fg_probe() ? 0 : 12; }
    // --fg-of-selftest: exercise the optical-flow bring-up without the game, so a
    // failure in ffxGetInterfaceDX12/ContextCreate can be diagnosed in one run.
#if MGPU_FG_OF
    if (wcsstr(GetCommandLineW(), L"--fg-of-selftest")) {
        const bool ok = of_selftest();
        logf_("[bridge] --fg-of-selftest: %s (%ux%u field)\n", ok ? "OK" : "FAILED", g_of_w, g_of_h);
        return ok ? 0 : 13;
    }
#else
    if (wcsstr(GetCommandLineW(), L"--fg-of-selftest")) {
        logf_("[bridge] --fg-of-selftest: not compiled in (MGPU_FG_OF=0)\n");
        return 13;
    }
#endif
    if (!open_shared()) { return 5; }
    // The capture item comes first: its size is the only trustworthy one, and the colour
    // sink / frame pool / FG swap chain all have to be built at that size.
    if (!create_capture_item(game_wnd)) { return 7; }
    adopt_capture_size();
    if (!create_colour_sink(g_width, g_height)) { return 6; }
    if (!init_convert()) { return 9; }
    if (!start_wgc()) { return 7; }
    if (g_pace_ms > 1.0 && !g_fg_warp) {
        logf_("[bridge] note: --pace pins our Present times, but XeSS-FG already paces"
              " itself from the XeLL markers and blocks ~15 ms inside Present, so the two"
              " serialise (load rises, ticks get missed). Leave --pace off for XeSS-FG.\n");
    }
    if (!init_fg()) { return 8; }
    if (g_fg_warp && (!init_warp() || !init_warp_pso())) { return 10; }

    logf_("\n[bridge] running - close the window or press Ctrl+C to stop\n\n");

    // --jbuf helpers: take/return a colour copy from our own pool.
    auto jbuf_take = [](ID3D11Texture2D* src) -> ComPtr<ID3D11Texture2D> {
        if (!g_jbuf_free.empty()) {
            ComPtr<ID3D11Texture2D> t = g_jbuf_free.back();
            g_jbuf_free.pop_back();
            return t;
        }
        D3D11_TEXTURE2D_DESC d{};
        src->GetDesc(&d);
        d.Usage          = D3D11_USAGE_DEFAULT;
        d.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = 0;
        d.MiscFlags      = 0;
        ComPtr<ID3D11Texture2D> t;
        if (FAILED(g_d3d11->CreateTexture2D(&d, nullptr, &t))) { return nullptr; }
        return t;
    };
    auto jbuf_give_back = [](ComPtr<ID3D11Texture2D>& t) {
        if (!t) { return; }
        if (g_jbuf > 0) { g_jbuf_pending.push_back(t); }   // ours: recycle through the pool
        t.Reset();                                          // WGC frame: let go of it now
    };

    // Frame timing bookkeeping shared by both paths: arrival EMA, jitter extremes and
    // the count of gaps (frames that never reached us at all).
    //
    // The interval comes from the capture frame's SystemRelativeTime, NOT from the wall
    // clock at the moment we dequeued it. That difference is load-bearing:
    //   * the compositor's stamp is the game's own cadence, quantised to vblanks;
    //   * GetTickCount64 has 1 ms resolution, so two frames drained in one loop iteration
    //     land in the same millisecond, the second delta is 0, and arr_push() drops it.
    //     That biased the median upward (31.0 ms for a locked 40 fps input) and made
    //     --fg-rt-mode quant snap frameRenderTime to 4 vblanks instead of 3, so the output
    //     ran at 96.8 fps on a 120 Hz panel and the cadence line said NOT-EXACT.
    // The wall clock is kept only as the fallback for a frame with no usable timestamp.
    auto note_arrival = [](IDirect3D11CaptureFrame* f) {
        const ULONGLONG now = GetTickCount64();
        double dt_arr = 0.0;                  // measured interval, compositor clock first

        ABI::Windows::Foundation::TimeSpan wts{};
        if (SUCCEEDED(f->get_SystemRelativeTime(&wts))) {
            const double ts_ms = static_cast<double>(wts.Duration) / 10000.0;
            if (g_arr_prev > 0.0) {
                const double d = ts_ms - g_arr_prev;
                if (d > 0.0 && d < 1000.0) {
                    dt_arr = d;
                    if (d < g_arr_min) { g_arr_min = d; }
                    if (d > g_arr_max) { g_arr_max = d; }
                    if (d > g_arrival_ms_ema * 1.6) { ++g_arr_drops; }
                    ++g_arr_count;
                    // Bucket it in vblanks: 3 = a 40 fps game on a 120 Hz panel.
                    const double vb = refresh_ms();
                    if (vb > 0.5) {
                        int k = static_cast<int>(d / vb + 0.5);
                        if (k < 1) { k = 1; }
                        if (k > 8) { k = 8; }
                        ++g_arr_hist[k];
                    }
                }
            }
            // How old is this frame when it reaches us? That is the WGC/DWM delay the
            // pipeline pays before any of our work starts.
            const double age = qpc_ms() - ts_ms;
            if (age > 0.0 && age < 500.0) {
                g_ms_age = g_ms_age * 0.9 + age * 0.1;
                if (age > g_max_age) { g_max_age = age; }
            }

            g_arr_prev = ts_ms;
        }

        if (dt_arr <= 0.0 && g_last_arrival) {
            const double dw = static_cast<double>(now - g_last_arrival);
            if (dw > 0.0 && dw < 1000.0) { dt_arr = dw; }
        }
        g_last_arrival = now;

        if (dt_arr > 0.0) {
            g_arrival_ms_ema = g_arrival_ms_ema * 0.9 + dt_arr * 0.1;
            arr_push(dt_arr);            // --fg-rt-mode median/quant + --interp-auto
            g_arr_wc_ring[g_arr_wc_i] = dt_arr;
            g_arr_wc_i = (g_arr_wc_i + 1) % kArrRing;
            if (g_arr_wc_n < kArrRing) { ++g_arr_wc_n; }

            // --fg-rt-mode auto: only collect here. The decision itself is taken once per
            // report window (see the report block): a 16-sample median jumps between the
            // quantised values 25.0 / 33.3 while an unlocked rate wobbles, so deciding per
            // frame made the pacer's input flap (measured: 4 flips in 18 s).
            g_arr_slow_ring[g_arr_slow_i] = dt_arr;
            g_arr_slow_i = (g_arr_slow_i + 1) % kArrSlowRing;
            if (g_arr_slow_n < kArrSlowRing) { ++g_arr_slow_n; }
            g_rt_ema_fast = arr_median_ms();          // short median (16 samples)
            g_rt_ema_slow = (g_arr_slow_n >= 32) ? arr_slow_median_ms() : g_rt_ema_fast;
        }
    };

    auto frame_to_texture = [](IDirect3D11CaptureFrame* f,
                               ComPtr<ID3D11Texture2D>& tex) -> bool {
        ComPtr<IDirect3DSurface> surface;
        f->get_Surface(&surface);
        ComPtr<IDirect3DDxgiInterfaceAccess> access;
        if (FAILED(surface.As(&access))) { return false; }
        return SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&tex)));
    };

    ComPtr<IDirect3D11CaptureFrame> pending;   // newest frame, waiting for its tick
    ComPtr<ID3D11Texture2D> pending_tex;

    // Frames we captured but have not processed yet. Only --slack > 0 lets this
    // hold more than one; every extra frame is an input interval of latency we
    // trade for absorbing a late one (see g_slack).
    std::deque<std::pair<ComPtr<IDirect3D11CaptureFrame>, ComPtr<ID3D11Texture2D>>> slack_q;

    // Escape hatch that does not depend on mgpu_ui.exe. If the UI hangs, lost the
    // hotkey or was closed, Ctrl+Alt+End still tears the overlay down.
    constexpr int kEscapeHotkeyId = 0x4D47;   // 'MG'
    const bool escape_hotkey =
        RegisterHotKey(nullptr, kEscapeHotkeyId, MOD_CONTROL | MOD_ALT, VK_END) != FALSE;
    logf_("[bridge] escape hotkey Ctrl+Alt+End registered=%d\n", static_cast<int>(escape_hotkey));

    for (;;) {
        // The window must be pumped or it goes blank and Windows marks the process
        // as not responding.
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { hard_exit("WM_QUIT (launcher request)"); }
            if (msg.message == WM_HOTKEY && static_cast<int>(msg.wParam) == kEscapeHotkeyId) {
                UnregisterHotKey(nullptr, kEscapeHotkeyId);
                hard_exit("escape hotkey Ctrl+Alt+End");
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (g_pace_ms > 0.0) {
            // Paced path: keep the newest frame that has arrived, present on a fixed
            // cadence. Frames arriving early simply wait; if none arrived by the tick
            // the previous frame is presented again, so the output rate never wobbles.
            const ULONGLONG t_poll = GetTickCount64();
            for (;;) {
                ComPtr<IDirect3D11CaptureFrame> f;
                if (FAILED(g_pool->TryGetNextFrame(&f)) || !f) { break; }
                ComPtr<ID3D11Texture2D> tex;
                if (!frame_to_texture(f.Get(), tex)) { continue; }
                note_arrival(f.Get());
                if (pending) { ++g_pace_skips; }
                pending = f;
                pending_tex = tex;
            }
            g_ms_capture = g_ms_capture * 0.9 +
                           static_cast<double>(GetTickCount64() - t_poll) * 0.1;

            if (g_pace_auto) {
                const double target = (g_arrival_ms_ema > 2.0) ? g_arrival_ms_ema : 25.0;
                g_pace_ms = g_pace_ms * 0.9 + target * 0.1;
            }
            const double now = qpc_ms();
            if (g_pace_next == 0.0) { g_pace_next = now + g_pace_ms; }
            if (now < g_pace_next) {
                // Sleep while there is real time left, spin for the last couple of ms:
                // a Sleep(0) this close to the deadline is what overshot the tick.
                if (g_pace_next - now > 2.0) { Sleep(1); }
                keep_game_focus();
                continue;
            }
            if (now - g_pace_next > g_pace_ms) { g_pace_next = now; }  // fell behind: resync
            g_pace_next += g_pace_ms;

            if (pending) {
                if (g_fg_warp) { run_frame_warp(pending_tex.Get()); }
                else           { run_frame(pending_tex.Get()); }
                pending.Reset();
                pending_tex.Reset();
            } else if (g_have_last) {
                g_reuse_frame = true;
                if (g_fg_warp) { run_frame_warp(nullptr); }
                else           { run_frame(nullptr); }
                g_reuse_frame = false;
                g_represents += 1.0;
            }
            assert_topmost();
            continue;
        }

        const ULONGLONG t_poll = GetTickCount64();
        // Keep a bounded queue of captured frames.
        //
        // --slack 0 (default) drains the pool and keeps only the NEWEST frame: an
        // over-budget frame costs an input frame, which is what keeps latency at one
        // input interval. Taking the oldest one unconditionally (what TryGetNextFrame
        // returns first) turns pool depth into latency and was what made the 5K 2x
        // pipeline slower and slower.
        //
        // --slack N keeps up to N+1 frames and processes the OLDEST, so a late frame
        // is absorbed by the next one instead of being thrown away. The cap is what
        // bounds the added latency: the (N+2)th frame in the queue drops the oldest.
        {
            // --jbuf N owns the queue depth in its own mode (N+1 in flight, oldest
            // processed first); --slack N is the legacy variant that holds WGC frames.
            // Promote last frame's returns now that their copies have retired.
            if (!g_jbuf_pending.empty()) {
                for (auto& t : g_jbuf_pending) { g_jbuf_free.push_back(t); }
                g_jbuf_pending.clear();
            }
            const size_t cap = static_cast<size_t>((g_jbuf > 0) ? g_jbuf : g_slack) + 1;
            for (;;) {
                ComPtr<IDirect3D11CaptureFrame> f;
                if (FAILED(g_pool->TryGetNextFrame(&f)) || !f) { break; }
                ComPtr<ID3D11Texture2D> tex;
                if (!frame_to_texture(f.Get(), tex)) { continue; }
                note_arrival(f.Get());
                if (g_jbuf > 0) {
                    // Copy the colour out now and let the capture frame go, so the WGC
                    // pool is never held (that is the difference to --slack).
                    ComPtr<ID3D11Texture2D> own = jbuf_take(tex.Get());
                    if (!own) { continue; }
                    g_ctx11->CopyResource(own.Get(), tex.Get());
                    slack_q.emplace_back(nullptr, own);
                    tex.Reset();
                    f.Reset();
                } else {
                    slack_q.emplace_back(f, tex);
                }
                while (slack_q.size() > cap) {
                    // An input frame we will never process. With --jbuf the colour is ours
                    // (recycle it); without it the texture belongs to the capture frame and
                    // simply dropping the reference returns it to the WGC pool.
                    if (g_jbuf > 0) { jbuf_give_back(slack_q.front().second); }
                    slack_q.pop_front();
                    ++g_pace_skips;
                }
            }
        }

        if (slack_q.empty()) {
            // Idle poll: keep the game focused so input never goes to the overlay.
            keep_game_focus();
            assert_topmost();
            Sleep(1);
            continue;
        }

        g_ms_capture = g_ms_capture * 0.9 +
                       static_cast<double>(GetTickCount64() - t_poll) * 0.1;
        // Take the oldest frame in the queue. With --slack 0 the queue only ever
        // holds one frame, so this is the same "process the newest" behaviour.
        ComPtr<IDirect3D11CaptureFrame> frame = slack_q.front().first;
        ComPtr<ID3D11Texture2D> texture = slack_q.front().second;
        slack_q.pop_front();
        // --jbuf textures belong to our own pool and are returned after the frame has
        // been submitted (see the explicit call at the end of this iteration). Do NOT do
        // that with a scope guard: a guard with a reference member released the texture
        // before run_frame() saw it, which made every frame black while all counters
        // stayed healthy.
        // --lock N / --lock-auto: drop captured frames so the *accepted* input rate is
        // exactly N fps.
        //
        // Why this is the root fix (rather than re-implementing XeSS-FG's
        // presentation): the proxy swap chain can only place frames into vblanks, so
        // motion is even exactly when input * (interp+1) spans an integer number of
        // vblanks. XeFG cannot invent a vblank, and without VRR the panel cannot
        // follow the output either - so the *input* has to be quantised. Doing it
        // here instead of with RTSS means no extra tool, no phase drift (every accept
        // advances the schedule by exactly one period) and it works for any game.
        // Dropping is cheap: the frame is already captured, we simply never tag it.
        if (g_lock_fps > 1.0) {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG period = static_cast<ULONGLONG>(1000.0 / g_lock_fps + 0.5);
            if (g_lock_next == 0) { g_lock_next = now; }
            if (now + 1 < g_lock_next) {
                ++g_lock_drops;          // too early: this frame is not used
                continue;
            }
            g_lock_next += (period > 0) ? period : 1;
            if (g_lock_next <= now) { g_lock_next = now + 1; }   // fell behind: resync
        }
        if (g_fg_warp) { run_frame_warp(texture.Get()); }
        else           { run_frame(texture.Get()); }
        // Return the colour texture: with --jbuf it goes back to our pool, otherwise this
        // just drops our reference and hands the buffer back to the WGC frame pool.
        jbuf_give_back(texture);
    }
}
