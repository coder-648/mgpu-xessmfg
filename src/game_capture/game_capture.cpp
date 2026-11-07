// ============================================================================
// game_capture - M3.2: pull the real per-frame payload out of the game
//
// Three streams, and where each one comes from (established by M2 and M3.1):
//
//   colour  5120x2880 R10G10B10A2  <- swap chain back buffer (ReShade)
//   mv      2560x1440 R16G16_FLOAT <- NGX EvaluateFeature parameters
//   depth   2560x1440 R32_FLOAT    <- NGX EvaluateFeature parameters
//
// M3.1 proved ReShade's DEPTH semantic hands back a *different* buffer
// (5120x2880 D32S8, the post-upscale one), so depth has to come from NGX, right
// next to the motion vectors. That also guarantees the two are the same size,
// which XeSS-FG requires.
//
// Flow per frame:
//   EvaluateFeature hook  -> remember mv/depth/jitter        (game thread)
//   reshade_begin_effects -> record barrier + CopyResource into the mailbox
//                            source textures on the GAME's command list, so
//                            ordering and state stay the game's business
//   execute_command_list  -> when that list is submitted, signal a fence on the
//                            game queue; the mailbox producer queue waits on it
//                            and then does source -> shared slot -> publish
//
// Nothing is drawn on screen and no game resource is modified beyond a barrier
// that is restored immediately.
// ============================================================================

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <deque>
#include <string>
#include <vector>

#include "reshade.hpp"
#include "MinHook.h"

#include "inputs.h"

#include "cross_adapter_mailbox.h"
#include "mgpu_shared.h"
#include "mgpu_ds_shaders.h"   // embedded DXBC for the MV/depth downsample (temp/gen_ds_shaders.py)

using Microsoft::WRL::ComPtr;
namespace rx = reshade;
using namespace reshade::api;

// ---------------------------------------------------------------- logging

static FILE*   g_log = nullptr;
static HMODULE g_self = nullptr;

static void vlogf_(const char* fmt, va_list a)
{
    if (!g_log) { return; }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(g_log, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
    vfprintf(g_log, fmt, a);
    fflush(g_log);
}

static void logf_(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vlogf_(fmt, a);
    va_end(a);
}

// How long the *game* has been running when the add-on gets loaded. ReShade loads and
// unloads add-ons once per device it sees, and UE5 creates several probe devices during
// start-up, so this shows the reload churn (and how late in start-up we appear).
static unsigned long long process_uptime_ms()
{
    FILETIME create{}, exit_t{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit_t, &kernel, &user)) { return 0; }
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = now.dwLowDateTime;   a.HighPart = now.dwHighDateTime;
    b.LowPart = create.dwLowDateTime; b.HighPart = create.dwHighDateTime;
    return (a.QuadPart - b.QuadPart) / 10000ull;    // 100 ns -> ms
}

// Logging is opt-in. A normal run must leave nothing behind, so the file is not even
// opened unless asked for: [capture] Log=1 in game_capture.ini, or MGPU_CAPTURE_LOG=1
// in the environment (handy, because the env var can be set by a launcher while the
// ini has to be edited before the game starts).
static bool log_requested()
{
    char env[16]{};
    if (GetEnvironmentVariableA("MGPU_CAPTURE_LOG", env, sizeof(env)) > 0 &&
        env[0] != '0') {
        return true;
    }
    char ini[MAX_PATH]{};
    GetModuleFileNameA(g_self, ini, MAX_PATH);
    if (char* dot = strrchr(ini, '.')) {
        strcpy_s(dot, MAX_PATH - (dot - ini), ".ini");
    }
    return GetPrivateProfileIntA("capture", "Log", 0, ini) != 0;
}

static void open_log()
{
    if (!log_requested()) { return; }        // g_log stays null -> logf_ is a no-op
    char path[MAX_PATH]{};
    GetModuleFileNameA(g_self, path, MAX_PATH);
    if (char* dot = strrchr(path, '.')) {
        strcpy_s(dot, MAX_PATH - (dot - path), ".log");
    }
    // Append, not truncate: loading is not once-per-process (see above), and "w" threw
    // away every earlier attempt. _SH_DENYNO so the file can be read while the game runs
    // (fopen's default sharing denied it, which made live diagnosis impossible).
    g_log = _fsopen(path, "a", _SH_DENYNO);
    if (g_log) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        logf_("=== game_capture  %04d-%02d-%02d %02d:%02d:%02d  (game uptime %llu ms) ===\n",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
              process_uptime_ms());
    }
}

static double now_ms()
{
    static LARGE_INTEGER f{};
    if (f.QuadPart == 0) { QueryPerformanceFrequency(&f); }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// ============================================================================
// Input sources (M6.3)
//
// Everything API specific lives behind mgpu::input::Source (in_ngx.cpp today,
// in_ffx.cpp / in_xess.cpp next - docs/design/VENDOR_COMPAT_PLAN.md M6.4/M6.5). The host
// keeps what is common to all of them: the per-frame loop, the transport, the
// resource state machine and the deferred release. `g_inputs` is the only thing the
// rest of this file reads.
// ============================================================================

static std::vector<mgpu::input::Source*> g_sources;
static mgpu::input::Source*              g_source = nullptr;  // picked once, kept
static mgpu::input::FrameInputs          g_inputs;
// Which API is feeding us (reported in ProducerInfo) and an optional pin from
// [capture] Input=auto|ngx|ffx|xess|depth.
static UINT        g_input_api = mgpu::kInputApiNone;
static std::string g_force_input = "auto";
// Consecutive frames in which the pinned source had nothing new (see fetch_inputs).
static int         g_source_quiet = 0;

// MinHook is shared by the loader hooks (below) and by whatever each source hooks.
static bool g_mh_ready = false;

bool mgpu::input::ensure_minhook()
{
    if (g_mh_ready) { return true; }
    if (MH_Initialize() == MH_OK) { g_mh_ready = true; }
    else { logf_("MH_Initialize FAILED\n"); }
    return g_mh_ready;
}

void mgpu::input::logf(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vlogf_(fmt, a);
    va_end(a);
}

// ---------------------------------------------------------- resource tracking
// Only the resources a source reported are watched: tracking *every* barrier was
// the original mistake - the loading screen issues tens of thousands of them while
// creating the world, and a global lock plus an unbounded map on each one starved
// the render threads (frozen picture, no TDR, no error).
// A small fixed table, not a map: the barrier callback fires tens of thousands of
// times while a level loads, and the old unbounded-map version starved the render
// threads (frozen picture, no TDR). Two entries are not enough any more either:
// FSR alternates TWO depth textures between frames, so a single "last seen" slot
// made us emit transitions from a state the resource was not in - D3D12 then fails
// Close() with E_INVALIDARG and the game dies.
constexpr int kTrackedResources = 8;
struct TrackedResource
{
    std::atomic<uint64_t> handle{ 0 };
    std::atomic<uint32_t> hint{ 0 };     // expected state until a barrier is observed
    std::atomic<uint32_t> state{ 0 };    // last observed state
    std::atomic<bool>     seen{ false };
    std::atomic<uint32_t> last_used{ 0 };  // tick of the last frame that reported it
};
static TrackedResource g_tracked[kTrackedResources];
// One tick per captured frame. Used for ageing: a resource the source stopped
// reporting (upscaler/preset switch replaces the textures) must not keep a stale
// state, and a handle reused by a new allocation must not inherit it either.
static std::atomic<uint32_t> g_track_tick{ 0 };
constexpr uint32_t kTrackedIdleTicks = 2;

static TrackedResource* tracked_find(uint64_t handle)
{
    if (!handle) { return nullptr; }
    for (int i = 0; i < kTrackedResources; ++i) {
        if (g_tracked[i].handle.load(std::memory_order_relaxed) == handle) {
            return &g_tracked[i];
        }
    }
    return nullptr;
}

// How well do we know a watched resource's state?
//   0 = unknown or stale  -> DO NOT touch it (no barrier)
//   1 = state observed from one of its own barrier events (trustworthy)
//   2 = reported this frame but no barrier seen yet -> the hint, best effort
//
// "Stale" is what makes an upscaler/mode switch survivable: the source (NGX or
// FSR) stops reporting the old textures, they are destroyed, and the address can be
// handed to a new allocation. Keeping a guessed state for those and emitting a
// transition is what killed the game with Close() = E_INVALIDARG.
static int tracked_state(uint64_t handle, uint32_t* state)
{
    TrackedResource* t = tracked_find(handle);
    if (!t) { return 0; }
    const uint32_t tick = g_track_tick.load(std::memory_order_relaxed);
    const uint32_t used = t->last_used.load(std::memory_order_relaxed);
    if (tick - used > kTrackedIdleTicks) {
        t->seen.store(false, std::memory_order_relaxed);
        return 0;                       // nobody reports it any more
    }
    if (t->seen.load(std::memory_order_relaxed)) {
        *state = t->state.load(std::memory_order_relaxed);
        return 1;
    }
    *state = t->hint.load(std::memory_order_relaxed);
    return 2;
}

static void tracked_set_state(uint64_t handle, uint32_t state)
{
    TrackedResource* t = tracked_find(handle);
    if (!t) { return; }
    t->state.store(state, std::memory_order_relaxed);
    t->seen.store(true, std::memory_order_relaxed);
}

static void tracked_add(uint64_t handle, uint32_t hint)
{
    if (!handle) { return; }
    const uint32_t tick = g_track_tick.load(std::memory_order_relaxed);

    TrackedResource* t = tracked_find(handle);
    if (t) { t->last_used.store(tick, std::memory_order_relaxed); return; }

    // Free slot, otherwise the least recently used one: an in-game upscaler or
    // preset switch allocates new depth/MV textures and the old ones are never
    // reported again, so a full table used to mean "no state at all" for the new
    // ones - which is one way to emit an invalid barrier.
    TrackedResource* victim = nullptr;
    for (int i = 0; i < kTrackedResources; ++i) {
        TrackedResource* c = &g_tracked[i];
        if (c->handle.load(std::memory_order_relaxed) == 0) { victim = c; break; }
        if (!victim || c->last_used.load(std::memory_order_relaxed) <
                       victim->last_used.load(std::memory_order_relaxed)) {
            victim = c;
        }
    }
    if (!victim) { return; }
    victim->hint.store(hint, std::memory_order_relaxed);
    victim->state.store(hint, std::memory_order_relaxed);
    victim->seen.store(false, std::memory_order_relaxed);
    victim->last_used.store(tick, std::memory_order_relaxed);
    victim->handle.store(handle, std::memory_order_relaxed);
}

static std::vector<std::pair<ID3D12Resource*, UINT64>> g_deferred_release;
static SRWLOCK g_release_lock = SRWLOCK_INIT;

static void hold_resource(ID3D12Resource* res);

void mgpu::input::track(ID3D12Resource* mv, ID3D12Resource* depth,
                        resource_usage mv_state, resource_usage depth_state)
{
    g_track_tick.fetch_add(1, std::memory_order_relaxed);
    if (mv)    { tracked_add(reinterpret_cast<uint64_t>(mv),
                             static_cast<uint32_t>(mv_state)); }
    if (depth) { tracked_add(reinterpret_cast<uint64_t>(depth),
                             static_cast<uint32_t>(depth_state)); }
}

void mgpu::input::hold(ID3D12Resource* res)
{
    hold_resource(res);
}


static HMODULE (WINAPI* g_real_load_library_ex_w)(LPCWSTR, HANDLE, DWORD) = nullptr;
static HMODULE (WINAPI* g_real_load_library_w)(LPCWSTR) = nullptr;

// Every module load is offered to every source; each one decides by path whether it
// cares (ngx wants `_nvngx.dll`, ffx will want `amd_fidelityfx*`, xess `libxess`).
static void notify_sources(HMODULE m)
{
    if (!m) { return; }
    for (auto* s : g_sources) { s->on_module(m); }
}

static HMODULE WINAPI hook_load_library_ex_w(LPCWSTR name, HANDLE file, DWORD flags)
{
    const HMODULE m = g_real_load_library_ex_w(name, file, flags);
    if (name) { notify_sources(m); }
    return m;
}

static HMODULE WINAPI hook_load_library_w(LPCWSTR name)
{
    const HMODULE m = g_real_load_library_w(name);
    if (name) { notify_sources(m); }
    return m;
}

static void scan_loaded_modules()
{
    // Sources that hook a module the game loaded *before* the add-on was injected.
    for (auto* s : g_sources) { s->scan_existing(); }
}

// -------------------------------------------------------------- capture state

static ComPtr<ID3D12Device>   g_game_device;
static bool                   g_targets_ready = false;   // unused, kept for clarity
static UINT64                 g_frame = 0;

// cross-process transport
static HANDLE                 g_map = nullptr;
static void*                  g_map_view = nullptr;
static ComPtr<ID3D12Resource> g_slot[3][mgpu::kMaxSlots];
static ComPtr<ID3D12Fence>    g_fwd[mgpu::kMaxSlots];
static ComPtr<ID3D12Fence>    g_back[mgpu::kMaxSlots];
static UINT64                 g_fwd_value[mgpu::kMaxSlots] = {};
// v5: the last fence value signalled per slot - what GetCompletedValue() has to reach
// before we may advertise the slot as complete to the consumer.
static UINT64                 g_fwd_expected[mgpu::kMaxSlots] = {};
static UINT                   g_slot_index = 0;
// v10: the slot this frame is being written into (picked with the consumer's ACKs, so
// we never overwrite a slot it is still reading). Valid for one capture+publish cycle.
static UINT                   g_cur_slot = 0;
// [capture] SlotPolicy: 0 = drop the publication when no slot is free (real-time default),
// 1 = spin up to SlotWaitUs for one to come free (adds latency, never loses a frame).
static int                    g_slot_policy = 0;
static int                    g_slot_wait_us = 2000;
static bool                   g_shared_ready = false;
// v8: the slot-layout size currently published, the mapping-backed heap (it survives
// layout rebuilds) and the resources of the previous layout, freed a few frames later.
static UINT                   g_stream_w = 0, g_stream_h = 0;
static ComPtr<ID3D12Heap>     g_shared_heap;
static std::vector<std::pair<ComPtr<ID3D12Resource>, UINT64>> g_retired_slots;
static bool                   g_copied_this_frame = false;
static ID3D12CommandQueue*    g_game_queue = nullptr;

// statistics
static int    g_frames_seen = 0;
static int    g_frames_copied = 0;
static int    g_frames_skipped = 0;
static double g_capture_ms_total = 0.0;
// Protocol v6: the game's own present interval, measured between two Present calls
// (one per real frame). The bridge is told about it so it can sanity check its own
// capture cadence; it is not used for pacing.
static double g_last_present_ms = 0.0;
static float  g_present_interval_ms = 0.0f;
static bool   g_logged_missing_state[3] = { false, false, false };
static bool   g_step_logged[3] = { false, false, false };
static UINT64 g_skip_capture_until = 0;
static bool   g_first_colour_logged = false;

static const char* kStreamName[3] = { "color", "mv", "depth" };

// game_capture.ini: Color=0 disables it. Set from load_config() before the shared
// block is built, so ensure_shared() can skip the colour stream's allocation.
static bool   g_capture_colour = true;

// MV/depth are sized to the *render* resolution, which depends on the DLSS mode
// (Quality at 4K renders 1440p, DLAA renders 2160p, Performance 1080p). The size is
// taken from the first NGX report; [capture] RenderWidth/RenderHeight override it when
// a title reports nonsense.
static UINT g_render_w_ini = 0;
static UINT g_render_h_ini = 0;
static UINT g_mv_w = 0;          // size the MV/depth streams were actually built at
static UINT g_mv_h = 0;
static bool g_logged_size_mismatch = false;

// Creates the three destination textures on the GAME's device only.
//
// An earlier build also created an AMD device here and hung the game: ReShade's
// D3D12 hooks are process-global and intercept that second device, mistaking it
// for the game's. The consumer therefore has to live in its own process, talking
// to us through a named file mapping; this build only proves the capture path.
// ================== MV/depth downsample ([capture] StreamPercent) ============
//
// Motion vectors and depth are the only things that travel from the game's GPU to the
// bridge's over the bus. With DLAA both are 2160p: 33 MB each, 66 MB per frame, and
// that measured 3.6 ms out of the bridge's 25.2 ms real-frame budget - enough to push
// 4x@120 out of the 33.3 ms window (HANDOFF §8-27).
//
// XeFG does not care about their resolution (a dummy at 2160p costs the same as the
// real thing), but it does need the depth *data* for disocclusion - dropping it
// entirely smears object edges. So: keep both, send them smaller.
//
//   [capture] StreamPercent=150   ->  streams at render * 100 / 150  (2160p -> 1440p)
//
// The pass renders the NGX textures into a local target at the stream size and copies
// that into the shared slot, so the slots stay ALLOW_CROSS_ADAPTER + row-major and are
// never used as render targets themselves.

static int  g_stream_percent = 100;   // 100 = full resolution (no downsample)
// [capture] SkipMv: do not produce the motion-vector stream at all (DEFAULT 1).
// XeSS-FG in this libxess_fg build does not consume tagged motion vectors (MV=0,
// MV=100000 px and the game's own real MVs all render identically - docs/findings/HANDOFF.md
// 8.6), so the converted MV stream is pure cost: a fullscreen pass plus a 14.7 MB
// cross-adapter write per frame. Set SkipMv=0 to bring it back (different FG library).
static bool g_skip_mv = true;      // [capture] SkipMv=1: never produce the MV stream
// v7: MV is produced only when the consumer asks for it (bridge --mv / UI).
static bool g_mv_active = false;
static bool g_mv_active_logged = false;
static int  g_stream_align = 1;       // 1 = exact capture size (no crop)
// [capture] TestRebuild=N: dev hook, forces a v8 layout rebuild every N frames so the
// rebuild path can be exercised without touching the game settings. 0 = off.
static int  g_test_rebuild = 0;
                                       // >1 rounds down and crops, which shifts the MV field
static int  g_depth_convert_cfg = 0;  // [capture] DepthConvert: 1 = try the depth R32 pass
static bool g_ds_on = false;          // StreamPercent: MV AND depth rendered smaller
static bool g_depth_pass = false;     // depth converted to R32 (and scaled) by ds_run
static bool g_ds_ready = false;       // PSOs/RTs for the pass exist

// Some titles hand NGX motion vectors that are not in the unit space XeFG expects (UE5
// has been observed mixing pixel units and backwards vectors). Normalising and/or flipping
// them through the same pass costs one fullscreen draw and fixes the interpolation without
// touching the game: [capture] MvNormalize=1 divides by the render size, MvFlip=1 negates.
static bool  g_mv_pass = false;
static int   g_mv_normalize_cfg = 0;
static int   g_mv_flip_cfg = 0;
static float g_mv_scale_x = 1.0f;
static float g_mv_scale_y = 1.0f;
// MvNormalize=2 divides by the *output* size instead of the render size: some UE5 builds
// appear to express vectors relative to the display resolution, not the render target.
static UINT  g_rt_buffer_w = 0;
static UINT  g_rt_buffer_h = 0;

// ---- depth path 3: same-frame real depth via a typeless copy -----------------
// Creating a shader view on the game's D32_FLOAT_S8X24 depth killed the device, and
// ReShade's tap output is one frame late (which made XeFG's disocclusion flicker). The way
// out: copy the depth (same layout, only a barrier) into a texture WE create as
// R32G8X24_TYPELESS - a shader view on that is perfectly legal - and render that into the
// R32 stream with the existing pass. Same frame, no typed-view, no 64-bit cross-adapter.
static ComPtr<ID3D12Resource> g_depth_tl;
static UINT        g_depth_tl_w = 0;
static UINT        g_depth_tl_h = 0;
static DXGI_FORMAT g_depth_tl_fmt = DXGI_FORMAT_UNKNOWN;
static bool        g_depth_tl_in_srv = false;
static bool        g_depth_tl_failed = false;

static DXGI_FORMAT typeless_of(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:  return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:       return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:     return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:       return DXGI_FORMAT_R16_TYPELESS;
    default:                             return f;
    }
}

static bool ensure_depth_tl(ID3D12Device* dev, ID3D12Resource* src)
{
    if (g_depth_tl_failed || !dev || !src) { return false; }
    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    const DXGI_FORMAT tf = typeless_of(sd.Format);

    // Very common case: the game's depth is ALREADY typeless (UE5 hands NGX
    // R32G8X24_TYPELESS, i.e. 19). A shader view on a typeless resource is legal, so no
    // copy is needed at all - the earlier crash came from allocating a 64-bit
    // cross-adapter *stream*, not from the view.
    if (tf == sd.Format) {
        switch (sd.Format) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R16_TYPELESS:
            return true;            // already typeless: use the source directly
        default:
            g_depth_tl_failed = true;
            logf_("depth: format %u is typed and has no typeless pair - convert path disabled\n",
                  static_cast<unsigned>(sd.Format));
            return false;
        }
    }

    D3D12_RESOURCE_DESC d = sd;
    d.Format = tf;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    g_depth_tl.Reset();
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&g_depth_tl)))) {
        g_depth_tl_failed = true;
        logf_("depth: typeless copy create FAILED (%llux%llu fmt %u)\n",
              static_cast<unsigned long long>(d.Width),
              static_cast<unsigned long long>(d.Height), static_cast<unsigned>(tf));
        return false;
    }
    g_depth_tl_w = static_cast<UINT>(d.Width);
    g_depth_tl_h = d.Height;
    g_depth_tl_fmt = tf;
    g_depth_tl_in_srv = false;
    logf_("depth: typeless copy %ux%u fmt %u (source fmt %u) - same-frame depth enabled\n",
          g_depth_tl_w, g_depth_tl_h, static_cast<unsigned>(tf),
          static_cast<unsigned>(sd.Format));
    return true;
}


// Creating a shader view on the game's D32_FLOAT_S8X24 depth killed the device (PBZ),
// so the depth now comes from mgpu_depth_tap.fx: ReShade samples the depth correctly and
// leaves it in its private R32F texture, which we copy. Reading it one frame later means
// the state is known (ReShade rendered into it last frame and nothing else touched it).
static effect_runtime*        g_rt = nullptr;
static effect_texture_variable g_rt_depth_var{};   // handle struct: check .handle
static bool                   g_rt_depth_lookup_done = false;
static ID3D12Resource*        g_rt_depth_res = nullptr;
static UINT g_ds_w = 0;
static UINT g_ds_h = 0;
static ID3D12Device* g_ds_device = nullptr;

static ComPtr<ID3D12RootSignature>  g_ds_root;
static ComPtr<ID3D12PipelineState>  g_ds_pso[2];    // 0 = MV, 1 = depth
static ComPtr<ID3D12Resource>       g_ds_rt[2];
static ComPtr<ID3D12DescriptorHeap> g_ds_rtv_heap;
static ComPtr<ID3D12DescriptorHeap> g_ds_srv_heap;
static UINT     g_ds_rtv_stride = 0;
static UINT     g_ds_srv_stride = 0;
static uint64_t g_ds_src_seen[2] = { 0, 0 };        // last source pointer per stream
static bool     g_ds_logged = false;

static void ds_prepare_log(const char* why)
{
    if (g_ds_logged) { return; }
    g_ds_logged = true;
    logf_("downsample: unavailable (%s) - MV/depth stay at full resolution\n", why);
}

// Builds the PSOs and the local render targets. Called from ensure_shared() *before*
// the shared block exists, so a failure can fall back to full-resolution streams.
static bool ds_prepare(ID3D12Device* device, UINT w, UINT h)
{
    if (g_ds_ready) { return true; }

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER pa[2]{};
    pa[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    pa[0].DescriptorTable.NumDescriptorRanges = 1;
    pa[0].DescriptorTable.pDescriptorRanges = &range;
    pa[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    pa[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;   // mvScale
    pa[1].Constants.ShaderRegister = 0;
    pa[1].Constants.RegisterSpace = 0;
    pa[1].Constants.Num32BitValues = 4;
    pa[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC smp{};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;     // the pass is a nearest decimation
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = pa;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &smp;

    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        ds_prepare_log("SerializeRootSignature");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                           IID_PPV_ARGS(&g_ds_root)))) {
        ds_prepare_log("CreateRootSignature");
        return false;
    }

    const DXGI_FORMAT rt_fmt[2] = { DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_TYPELESS };
    const DXGI_FORMAT rtv_fmt[2] = { DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT };

    for (int i = 0; i < 2; ++i) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature = g_ds_root.Get();
        p.VS = { k_ds_vs, k_ds_vs_size };
        p.PS = { k_ds_ps, k_ds_ps_size };
        p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        p.SampleMask = 0xFFFFFFFFu;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable = TRUE;
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets = 1;
        p.RTVFormats[0] = rtv_fmt[i];
        p.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&g_ds_pso[i])))) {
            ds_prepare_log("CreateGraphicsPipelineState");
            return false;
        }

        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        // Fixed capacity, like the shared slots: the pass draws into the top-left
        // `g_ds_w x g_ds_h` corner (viewport/scissor), so a render-size change never
        // has to recreate these - and the producer never has to stall for it (v9).
        d.Width = mgpu::kStreamMaxW;
        d.Height = mgpu::kStreamMaxH;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = rt_fmt[i];
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1;
        hp.VisibleNodeMask = 1;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                   IID_PPV_ARGS(&g_ds_rt[i])))) {
            ds_prepare_log("CreateCommittedResource(rt)");
            return false;
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC rd{};
    rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rd.NumDescriptors = 2;
    if (FAILED(device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&g_ds_rtv_heap)))) {
        ds_prepare_log("CreateDescriptorHeap(rtv)");
        return false;
    }
    g_ds_rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC sd{};
    sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sd.NumDescriptors = 2;
    sd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&g_ds_srv_heap)))) {
        ds_prepare_log("CreateDescriptorHeap(srv)");
        return false;
    }
    g_ds_srv_stride = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (int i = 0; i < 2; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_ds_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(i) * g_ds_rtv_stride;
        D3D12_RENDER_TARGET_VIEW_DESC rdv{};
        rdv.Format = rtv_fmt[i];
        rdv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(g_ds_rt[i].Get(), &rdv, rtv);
    }

    g_ds_device = device;
    g_ds_w = w;
    g_ds_h = h;
    g_ds_ready = true;
    logf_("downsample: MV/depth pass targets are %ux%u (capacity), drawing %ux%u\n",
          mgpu::kStreamMaxW, mgpu::kStreamMaxH, w, h);
    return true;
}


// One stream per call: `which` 0 = MV (R16G16_FLOAT), 1 = depth (R32).
// The shared slots are placed at a fixed kStreamMaxW x kStreamMaxH (protocol v9), so a
// copy into one must cover only the live content rectangle: the consumer copies exactly
// `StreamShared.width x height` out again, and the row pitch comes from the slot capacity.
// If the geometries happen to match, let D3D12 take the whole-resource fast path.
static void copy_into_slot(ID3D12GraphicsCommandList* list, ID3D12Resource* src,
                           ID3D12Resource* dst)
{
    if (!list || !src || !dst) { return; }
    const D3D12_RESOURCE_DESC a = src->GetDesc();
    const D3D12_RESOURCE_DESC b = dst->GetDesc();
    UINT w = static_cast<UINT>(a.Width  < b.Width  ? a.Width  : b.Width);
    UINT h = static_cast<UINT>(a.Height < b.Height ? a.Height : b.Height);
    if (g_stream_w > 0 && g_stream_w < w) { w = g_stream_w; }
    if (g_stream_h > 0 && g_stream_h < h) { h = g_stream_h; }
    if (w == 0 || h == 0) { return; }

    if (w == static_cast<UINT>(a.Width) && h == static_cast<UINT>(a.Height) &&
        w == static_cast<UINT>(b.Width) && h == static_cast<UINT>(b.Height)) {
        list->CopyResource(dst, src);
        return;
    }

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
    list->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
}


// The caller has already transitioned `src` to a shader-readable state and restores it
// afterwards; `dst` is the shared slot (COMMON, row-major, cross-adapter).
static bool ds_run(ID3D12GraphicsCommandList* list, int which, ID3D12Resource* src,
                   ID3D12Resource* dst)
{
    if (!g_ds_ready || !src || !dst || which < 0 || which > 1) { return false; }

    const uint64_t key = reinterpret_cast<uint64_t>(src);
    if (g_ds_src_seen[which] != key) {
        // The SRV format has to match what the game's resource actually is: UE5 hands us
        // D32_FLOAT_S8X24_UINT, whose shader view is R32_FLOAT_X8X24_TYPELESS; a plain
        // R32_FLOAT view would be rejected and the pass would sample nothing.
        const DXGI_FORMAT src_fmt = src->GetDesc().Format;
        DXGI_FORMAT srv_fmt = (which == 0) ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R32_FLOAT;
        if (which == 1) {
            switch (src_fmt) {
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            case DXGI_FORMAT_R32G8X24_TYPELESS: srv_fmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
            case DXGI_FORMAT_D24_UNORM_S8_UINT: srv_fmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;   break;
            case DXGI_FORMAT_D16_UNORM:         srv_fmt = DXGI_FORMAT_R16_UNORM;               break;
            default:                            srv_fmt = DXGI_FORMAT_R32_FLOAT;               break;
            }
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = srv_fmt;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_ds_srv_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(which) * g_ds_srv_stride;
        g_ds_device->CreateShaderResourceView(src, &srv, cpu);
        g_ds_src_seen[which] = key;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_ds_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(which) * g_ds_rtv_stride;

    D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(g_ds_w), static_cast<float>(g_ds_h),
                       0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, static_cast<LONG>(g_ds_w), static_cast<LONG>(g_ds_h) };

    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    ID3D12DescriptorHeap* heaps[] = { g_ds_srv_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(g_ds_root.Get());
    list->SetPipelineState(g_ds_pso[which].Get());
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_ds_srv_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<SIZE_T>(which) * g_ds_srv_stride;
    list->SetGraphicsRootDescriptorTable(0, gpu);
    const float k[4] = { (which == 0) ? g_mv_scale_x : 1.0f,
                         (which == 0) ? g_mv_scale_y : 1.0f, 0.0f, 0.0f };
    list->SetGraphicsRoot32BitConstants(1, 4, k, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = g_ds_rt[which].Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);

    copy_into_slot(list, g_ds_rt[which].Get(), dst);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &b);
    return true;
}

// Resolves mgpu_depth_tap.fx's output texture. ReShade only has it once the effect is
// loaded and the preset enabled it, so this retries until it shows up (a few frames).
static void native_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
                           resource_usage before, resource_usage after);   // defined later
static int g_rt_depth_tries = 0;
// Same-frame depth: copy the game's depth into our typeless texture, then run the existing
// pass on it (converts to R32 and scales to the MV size in one go).
static bool record_depth_convert(ID3D12GraphicsCommandList* list, ID3D12Resource* src,
                                 ID3D12Resource* dst)
{
    if (!src || !dst || !g_ds_ready) { return false; }
    if (!ensure_depth_tl(g_ds_device, src)) { return false; }

    const uint64_t key = reinterpret_cast<uint64_t>(src);
    uint32_t tracked = 0;
    if (tracked_state(key, &tracked) == 0) {
        // Not (or no longer) a resource the source reports: emit nothing rather
        // than a transition from a guessed state, which is what killed the game
        // when the upscaler texture changed mid-run.
        if (!g_logged_missing_state[2]) {
            g_logged_missing_state[2] = true;
            logf_("  [depth] %p is not a watched resource - depth skipped\n", (void*)src);
        }
        return false;
    }
    resource_usage before = static_cast<resource_usage>(tracked);
    if (static_cast<uint32_t>(before) & 0x80000000u) { before = resource_usage::undefined; }

    if (!g_step_logged[2]) {
        g_step_logged[2] = true;
        const D3D12_RESOURCE_DESC a = src->GetDesc();
        // Note: the game's depth arrived already typeless here, so there is no
        // intermediate copy - "-> %ux%u" is that copy's size and stays 0 (it used to
        // print "-> 0x0" and read like a failure; it is not).
        logf_("  [depth] same-frame convert in place: %llux%llu fmt %u, tracked state = 0x%X\n",
              static_cast<unsigned long long>(a.Width),
              static_cast<unsigned long long>(a.Height), static_cast<unsigned>(a.Format),
              static_cast<uint32_t>(before));
    }

    // The source is already typeless (or we made a typeless copy): run the pass straight
    // on it. ds_run picks R32_FLOAT_X8X24_TYPELESS / R32_FLOAT for the view.
    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    if (sd.Format == DXGI_FORMAT_R32G8X24_TYPELESS || sd.Format == DXGI_FORMAT_R32_TYPELESS ||
        sd.Format == DXGI_FORMAT_R24G8_TYPELESS || sd.Format == DXGI_FORMAT_R16_TYPELESS) {
        if (!g_step_logged[2]) {
            g_step_logged[2] = true;
            logf_("  [depth] direct convert %llux%llu fmt %u -> %ux%u, tracked state = 0x%X\n",
                  static_cast<unsigned long long>(sd.Width),
                  static_cast<unsigned long long>(sd.Height), static_cast<unsigned>(sd.Format),
                  g_depth_tl_w, g_depth_tl_h, static_cast<uint32_t>(before));
        }
        if (static_cast<uint32_t>(before) != static_cast<uint32_t>(resource_usage::shader_resource)) {
            native_barrier(list, src, before, resource_usage::shader_resource);
        }
        const bool ok2 = ds_run(list, 1, src, dst);
        if (static_cast<uint32_t>(before) != static_cast<uint32_t>(resource_usage::shader_resource)) {
            native_barrier(list, src, resource_usage::shader_resource, before);
        }
        tracked_set_state(key, static_cast<uint32_t>(before));
        return ok2;
    }

    native_barrier(list, src, before, resource_usage::copy_source);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = g_depth_tl.Get();
    b.Transition.StateBefore = g_depth_tl_in_srv ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
                                                 : D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = g_depth_tl.Get();
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);   // whole subresource

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &b);
    g_depth_tl_in_srv = true;

    native_barrier(list, src, resource_usage::copy_source, before);

    const bool ok = ds_run(list, 1, g_depth_tl.Get(), dst);

    tracked_set_state(key, static_cast<uint32_t>(before));
    return ok;
}

static ID3D12Resource* reshade_depth_resource()
{
    if (g_rt_depth_res) { return g_rt_depth_res; }
    if (!g_rt || g_rt_depth_tries > 300) { return nullptr; }

    if (!g_rt_depth_var.handle) {
        g_rt_depth_var = g_rt->find_texture_variable("mgpu_depth_tap.fx", "MGPU_DepthOutTex");
        if (!g_rt_depth_var.handle) {
            if (++g_rt_depth_tries == 1) {
                logf_("depth: mgpu_depth_tap.fx / MGPU_DepthOutTex not found yet"
                      " - is the effect enabled in ReShadePreset.ini?\n");
            }
            return nullptr;
        }
        logf_("depth: found mgpu_depth_tap.fx.MGPU_DepthOutTex binding\n");
    }

    resource_view view{};
    g_rt->get_texture_binding(g_rt_depth_var, &view, nullptr);
    if (!view.handle) {
        if (++g_rt_depth_tries == 1) { logf_("depth: MGPU_DepthOutTex has no view yet\n"); }
        return nullptr;
    }

    device* dev = g_rt->get_device();
    const resource res = dev->get_resource_from_view(view);
    if (!res.handle) { ++g_rt_depth_tries; return nullptr; }

    g_rt_depth_res = reinterpret_cast<ID3D12Resource*>(res.handle);
    const D3D12_RESOURCE_DESC d = g_rt_depth_res->GetDesc();
    logf_("depth: using ReShade depth texture %p (%llux%llu fmt %u)\n",
          reinterpret_cast<void*>(g_rt_depth_res),
          static_cast<unsigned long long>(d.Width), static_cast<unsigned long long>(d.Height),
          static_cast<unsigned>(d.Format));
    return g_rt_depth_res;
}

// Copies ReShade's depth texture into our depth stream. `before` is the state ReShade left
// it in (render target: it rendered the tap pass into it last frame and nothing else uses
// it), which is why this needs no state guessing on the game's own depth resource.
static bool record_shadow_depth_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* src,
                                     ID3D12Resource* dst, resource_usage before)
{
    if (!src || !dst) { return false; }

    if (!g_step_logged[2]) {
        g_step_logged[2] = true;
        const D3D12_RESOURCE_DESC a = src->GetDesc();
        const D3D12_RESOURCE_DESC b = dst->GetDesc();
        logf_("  [depth] ReShade copy %llux%llu fmt %u -> %llux%llu fmt %u (state 0x%X)\n",
              static_cast<unsigned long long>(a.Width),
              static_cast<unsigned long long>(a.Height), static_cast<unsigned>(a.Format),
              static_cast<unsigned long long>(b.Width),
              static_cast<unsigned long long>(b.Height), static_cast<unsigned>(b.Format),
              static_cast<uint32_t>(before));
    }

    native_barrier(list, src, before, resource_usage::copy_source);

    const D3D12_RESOURCE_DESC a = src->GetDesc();
    const D3D12_RESOURCE_DESC b = dst->GetDesc();
    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = dst;
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box{};
    box.right  = static_cast<UINT>(a.Width  < b.Width  ? a.Width  : b.Width);
    box.bottom = static_cast<UINT>(a.Height < b.Height ? a.Height : b.Height);
    box.back = 1;
    list->CopyTextureRegion(&d, 0, 0, 0, &s, &box);

    native_barrier(list, src, resource_usage::copy_source, before);
    return true;
}

// Builds the cross-process transport: a NAMED file mapping wrapped as a D3D12
// heap on the game's device, with one placed texture per stream per ring slot,
// plus shared fences whose handle values are published for the bridge process.
#include "shared_layout.h"   // v8: layout helpers + rebuild (same TU, uses the state above)

static bool ensure_shared(ID3D12Device* device)
{
    if (g_shared_ready) { return true; }

    // MV/depth have to be sized to the *render* resolution, and that depends on the
    // DLSS mode: Quality at 4K renders 1440p, DLAA renders 2160p, Performance 1080p.
    // The old hard-coded 2560x1440 only ever worked because both "5K + Performance"
    // and "4K + Quality" happen to render at 1440p; under DLAA every copy was rejected
    // by copy_compatible() and the bridge silently received no motion vectors at all.
    // on_begin_effects() only gets here after the first valid NGX report, so the size
    // is known; the ini keys override it if a title reports nonsense.
    UINT mv_w = 2560, mv_h = 1440;
    const char* mv_src = "default";
    if (g_inputs.render_w >= 256 && g_inputs.render_h >= 256) {
        mv_w = g_inputs.render_w;
        mv_h = g_inputs.render_h;
        mv_src = "input render size";
    }
    if (g_render_w_ini >= 256 && g_render_h_ini >= 256) {
        mv_w = g_render_w_ini;
        mv_h = g_render_h_ini;
        mv_src = "ini";
    }

    // Optional: shrink the two streams (and with them the per-frame cross-adapter copy)
    // by rendering them at a lower resolution. dlss-mode independent, so DLAA's 2160p
    // MV/depth stop costing 66 MB/frame. Falls back to full size if anything fails.
    if (g_stream_percent > 100) {
        const UINT w = (mv_w * 100u / static_cast<UINT>(g_stream_percent)) & ~1u;
        const UINT h = (mv_h * 100u / static_cast<UINT>(g_stream_percent)) & ~1u;
        if (w >= 256 && h >= 256 && ds_prepare(device, w, h)) {
            mv_w = w;
            mv_h = h;
            mv_src = "downsampled";
        } else {
            ds_prepare_log("size/PSO");
        }
    }
    // Depth is copied with a crop when it is a couple of pixels larger than the MVs
    // (UE5: MVs 2562x1441, depth 2564x1444). Both streams must be the SAME size for XeFG -
    // tagging different sizes made every frame fail with ERR_INVALID_ARGUMENT and no
    // interpolated frames at all. CopyResource cannot rescale, CopyTextureRegion can crop.
    UINT depth_src_w = mv_w;
    UINT depth_src_h = mv_h;
    if (g_inputs.depth) {
        const D3D12_RESOURCE_DESC dd = g_inputs.depth->GetDesc();
        if (dd.Width >= 256 && dd.Height >= 256) {
            depth_src_w = static_cast<UINT>(dd.Width);
            depth_src_h = static_cast<UINT>(dd.Height);
        }
    }

    // UE5 hands NGX a padded render size (2562x1441 for a 2560x1440 render). XeFG appears
    // to silently ignore motion vectors at odd/unaligned sizes - changing their scale or
    // sign made no difference at all, which is exactly what "not used" looks like. Round
    // the stream size down to a multiple of 8 and crop the rest away; CP2077's 2560x1440
    // is already aligned, so nothing changes there.
    if (g_stream_align > 1) {
        const UINT a = static_cast<UINT>(g_stream_align);
        const UINT aw = mv_w - (mv_w % a);
        const UINT ah = mv_h - (mv_h % a);
        if (aw >= 256 && ah >= 256 && (aw != mv_w || ah != mv_h)) {
            logf_("shared: MV %ux%u is not %u-aligned - using %ux%u (crop)\n",
                  mv_w, mv_h, a, aw, ah);
            mv_w = aw;
            mv_h = ah;
        }
    }

    // Optional MV fix-up: if the game's vectors are in pixels or point backwards, the pass
    // scales/flips them on the way into the stream. Costs one fullscreen draw.
    g_mv_scale_x = 1.0f;
    g_mv_scale_y = 1.0f;
    if (g_mv_normalize_cfg == 1 && mv_w > 0 && mv_h > 0) {
        g_mv_scale_x = 1.0f / static_cast<float>(mv_w);
        g_mv_scale_y = 1.0f / static_cast<float>(mv_h);
    } else if (g_mv_normalize_cfg == 2 && g_rt_buffer_w > 0 && g_rt_buffer_h > 0) {
        g_mv_scale_x = 1.0f / static_cast<float>(g_rt_buffer_w);
        g_mv_scale_y = 1.0f / static_cast<float>(g_rt_buffer_h);
    }
    if (g_mv_flip_cfg) {
        g_mv_scale_x = -g_mv_scale_x;
        g_mv_scale_y = -g_mv_scale_y;
    }
    g_mv_pass = (g_mv_normalize_cfg != 0 || g_mv_flip_cfg != 0);
    g_mv_w = mv_w;
    g_mv_h = mv_h;

    // The stream is always R32_TYPELESS: that is the only depth format we know allocates
    // on a cross-adapter heap, and XeFG expects a 32-bit float depth anyway. UE5 hands NGX
    // D32_FLOAT_S8X24_UINT (64-bit, stencil included) - mirroring *that* format in the
    // stream made CreatePlacedResource fail and took the game down with it, so the depth is
    // CONVERTED by a small pass (ds_run) instead of copied.
    const DXGI_FORMAT depth_fmt = DXGI_FORMAT_R32_TYPELESS;
    UINT depth_src_fmt = 0;
    if (g_inputs.depth) {
        depth_src_fmt = static_cast<UINT>(g_inputs.depth->GetDesc().Format);
    }

    struct Spec { DXGI_FORMAT format; UINT w, h; };
    const Spec specs[3] = {
        { DXGI_FORMAT_R10G10B10A2_UNORM, 5120, 2880 },   // colour: unused with Color=0
        { DXGI_FORMAT_R16G16_FLOAT,      mv_w, mv_h },
        { depth_fmt,                     mv_w, mv_h },
    };
    logf_("shared: mv at %ux%u (%s, format %u), depth source %ux%u format %u -> stream R32 (%ux%u)"
          " via %s\n",
          mv_w, mv_h, mv_src,
          g_inputs.mv ? static_cast<unsigned>(g_inputs.mv->GetDesc().Format) : 0u,
          depth_src_w, depth_src_h, depth_src_fmt, mv_w, mv_h,
          g_ds_on ? "downsample pass" : "convert pass");

    // Depth always goes through the pass when nothing else did: it converts whatever
    // depth format the game used to R32 and rescales to the MV size in one go.
    if (!g_ds_on && (g_depth_convert_cfg || g_mv_pass)) {
        const bool ready = ds_prepare(device, mv_w, mv_h);
        if (ready && g_depth_convert_cfg) { g_depth_pass = true; }
        if (!ready) { ds_prepare_log("pass setup"); }
        if (ready) {
            logf_("pass ready: mv scale (%.6f, %.6f), depth %s\n",
                  g_mv_scale_x, g_mv_scale_y, g_depth_convert_cfg ? "on" : "off");
        }
    } else if (!g_ds_on) {
        logf_("depth: pass disabled by [capture] DepthConvert=0 - depth will be skipped\n");
    }

    ComPtr<ID3D12Device3> device3;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device3)))) {
        logf_("shared: ID3D12Device3 unavailable\n");
        return false;
    }

    mgpu::SharedControl ctl{};
    const uint64_t used = layout_streams(device, &ctl, mv_w, mv_h);
    // The mapping is created at the largest layout the reserve allows; v9 places every
    // slot at that capacity, so `used` should now be exactly the reserve.
    const uint64_t reserve = reserve_bytes(device);
    if (used > reserve) {
        logf_("shared: layout needs %.1f MB but only %.1f MB is reserved - disabling\n",
              static_cast<double>(used) / (1024.0 * 1024.0),
              static_cast<double>(reserve) / (1024.0 * 1024.0));
        return false;
    }

    std::wstring name;
    {
        const char* narrow = mgpu::kMappingName;
        name.assign(narrow, narrow + strlen(narrow));
    }

    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                               static_cast<DWORD>(reserve >> 32),
                               static_cast<DWORD>(reserve & 0xFFFFFFFFu), name.c_str());
    if (!g_map) {
        logf_("shared: CreateFileMapping FAILED err=%lu\n", GetLastError());
        return false;
    }
    g_map_view = MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!g_map_view) {
        logf_("shared: MapViewOfFile FAILED err=%lu\n", GetLastError());
        return false;
    }

    if (FAILED(device3->OpenExistingHeapFromFileMapping(g_map, IID_PPV_ARGS(&g_shared_heap)))) {
        logf_("shared: OpenExistingHeapFromFileMapping FAILED\n");
        return false;
    }

    for (int i = 0; i < 3; ++i) {
        if (ctl.streams[i].slot_size == 0) { continue; }   // colour disabled
        if (!place_stream_slots(device, g_shared_heap.Get(), ctl.streams[i], g_slot[i])) {
            return false;
        }
    }

    const D3D12_FENCE_FLAGS ff =
        (D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER);
    for (UINT s = 0; s < mgpu::kMaxSlots; ++s) {
        if (FAILED(device->CreateFence(0, ff, IID_PPV_ARGS(&g_fwd[s]))) ||
            FAILED(device->CreateFence(0, ff, IID_PPV_ARGS(&g_back[s])))) {
            logf_("shared: CreateFence FAILED\n");
            return false;
        }
        HANDLE h = nullptr;
        if (FAILED(device->CreateSharedHandle(g_fwd[s].Get(), nullptr, GENERIC_ALL, nullptr, &h))) {
            logf_("shared: forward fence handle FAILED\n");
            return false;
        }
        ctl.fence_forward[s] = reinterpret_cast<uint64_t>(h);

        h = nullptr;
        if (FAILED(device->CreateSharedHandle(g_back[s].Get(), nullptr, GENERIC_ALL, nullptr, &h))) {
            logf_("shared: back fence handle FAILED\n");
            return false;
        }
        ctl.fence_back[s] = reinterpret_cast<uint64_t>(h);
    }

    ctl.version        = mgpu::kSharedVersion;
    ctl.producer_pid   = GetCurrentProcessId();
    ctl.slots          = mgpu::kMaxSlots;
    ctl.stream_count   = 3;
    ctl.mapping_size   = reserve;
    ctl.generation     = 1;
    ctl.frame_published = 0;
    memset(ctl.frames, 0, sizeof(ctl.frames));   // v5: no slot is ready yet

    // ---- v6: who we are ----------------------------------------------------
    // The adapter LUID is what lets the bridge pick the *other* GPU without
    // hard-coding a vendor (it used to insist on VendorId 0x1002). input_api says
    // which upscaler fed these streams; until the input-source layer lands
    // (docs/design/VENDOR_COMPAT_PLAN.md M6.3) NGX is the only one, so what the flags can
    // say is limited to "no camera, depth as the game wrote it".
    {
        const LUID luid = device->GetAdapterLuid();
        ctl.producer.device_luid    = mgpu::pack_luid(luid.HighPart, luid.LowPart);
        ctl.producer.input_api      = g_input_api ? g_input_api : mgpu::kInputApiNgx;
        ctl.producer.input_flags    = g_inputs.flags;
        ctl.producer.render_w       = mv_w;
        ctl.producer.render_h       = mv_h;
        ctl.producer.output_w       = g_inputs.output_w ? g_inputs.output_w : g_rt_buffer_w;
        ctl.producer.output_h       = g_inputs.output_h ? g_inputs.output_h : g_rt_buffer_h;
        int32_t luid_hi = 0;
        uint32_t luid_lo = 0;
        mgpu::unpack_luid(ctl.producer.device_luid, &luid_hi, &luid_lo);
        logf_("shared: producer luid=%08lX:%08lX api=%s flags=0x%X render=%ux%u"
              " (source: %s) output=%ux%u\n",
              static_cast<unsigned long>(static_cast<uint32_t>(luid_hi)),
              static_cast<unsigned long>(luid_lo),
              g_source ? g_source->name() : "none", ctl.producer.input_flags,
              ctl.producer.render_w, ctl.producer.render_h, mv_src,
              ctl.producer.output_w, ctl.producer.output_h);
    }

    ctl.magic          = mgpu::kSharedMagic;   // written last: consumer waits for it
    memcpy(g_map_view, &ctl, sizeof(ctl));

    g_shared_ready = true;
    g_stream_w = mv_w;
    g_stream_h = mv_h;
    logf_("shared ready: name=%hs pid=%u used=%.1f MB reserved=%.1f MB slots=%u gen=%u\n",
          mgpu::kMappingName, ctl.producer_pid, static_cast<double>(used) / (1024.0 * 1024.0),
          static_cast<double>(reserve) / (1024.0 * 1024.0), ctl.slots, ctl.generation);
    return true;
}

// ------------------------------------------------------------ ReShade events

static void on_barrier(command_list*, uint32_t count, const resource* resources,
                       const resource_usage*, const resource_usage* new_states)
{
    // Hot path: a handful of relaxed atomic reads, no lock. Only resources we are
    // actually watching are looked up (at most kTrackedResources compares).
    if (count == 0) { return; }
    for (uint32_t i = 0; i < count; ++i) {
        tracked_set_state(resources[i].handle, static_cast<uint32_t>(new_states[i]));
    }
}

// ReShade's resource_usage bit values are laid out identically to
// D3D12_RESOURCE_STATES (shader_resource 0xC0, depth_stencil 0x30, copy_source
// 0x800, ...), so the barrier can be issued natively. That keeps the call out of
// ReShade's own wrapper while a callback is in flight.
static void native_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
                           resource_usage before, resource_usage after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource   = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(static_cast<uint32_t>(before));
    b.Transition.StateAfter  = static_cast<D3D12_RESOURCE_STATES>(static_cast<uint32_t>(after));
    list->ResourceBarrier(1, &b);
}

// Records "game texture -> capture target" on the GAME's command list, so the
// resource stays the game's business: bracket the copy with a barrier out of the
// last state we observed and put it back immediately afterwards.
static bool record_copy_tracked(command_list* /*cmd*/, ID3D12GraphicsCommandList* native_list,
                                ID3D12Resource* src, ID3D12Resource* dst,
                                int stream_index)
{
    const uint64_t key = reinterpret_cast<uint64_t>(src);

    uint32_t state = 0;
    const bool found = tracked_state(key, &state) != 0;

    if (!found) {
        if (!g_logged_missing_state[stream_index]) {
            g_logged_missing_state[stream_index] = true;
            logf_("  [%s] %p is not the watched resource - stream skipped\n",
                  kStreamName[stream_index], (void*)src);
        }
        return false;
    }

    resource_usage before = static_cast<resource_usage>(state);

    const bool first_time = !g_step_logged[stream_index];
    if (first_time) {
        g_step_logged[stream_index] = true;
        logf_("  [%s] first copy from %p, tracked state = 0x%X\n",
              kStreamName[stream_index], (void*)src, static_cast<uint32_t>(before));
    }

    // 0x80000000 is ReShade's "general", which has no D3D12 equivalent; treat it
    // as COMMON rather than emitting an invalid transition.
    if (static_cast<uint32_t>(before) & 0x80000000u) { before = resource_usage::undefined; }

    if (first_time) { logf_("  [%s] barrier -> copy_source\n", kStreamName[stream_index]); }
    native_barrier(native_list, src, before, resource_usage::copy_source);

    if (first_time) { logf_("  [%s] copy into slot\n", kStreamName[stream_index]); }
    copy_into_slot(native_list, src, dst);

    if (first_time) { logf_("  [%s] barrier -> restore\n", kStreamName[stream_index]); }
    native_barrier(native_list, src, resource_usage::copy_source, before);

    // keep our view of the state in sync with the transition we just issued
    tracked_set_state(key, static_cast<uint32_t>(before));
    return true;
}

// Same bookkeeping as record_copy_tracked(), but the source is sampled into the local
// stream-sized target first (ds_run), which is what shrinks the cross-adapter copy.
// Only the barrier targets differ: the draw reads the source as a shader resource.
static bool record_downsample(command_list* cmd, ID3D12GraphicsCommandList* native_list,
                              ID3D12Resource* src, ID3D12Resource* dst, int which)
{
    const int sidx = (which == 0) ? 1 : 2;      // stream index: 1 = mv, 2 = depth
    const uint64_t key = reinterpret_cast<uint64_t>(src);

    uint32_t state = 0;
    const bool found = tracked_state(key, &state) != 0;
    if (!found) { return record_copy_tracked(cmd, native_list, src, dst, sidx); }

    resource_usage before = static_cast<resource_usage>(state);
    if (static_cast<uint32_t>(before) & 0x80000000u) { before = resource_usage::undefined; }
    const resource_usage read_as = resource_usage::shader_resource;

    if (!g_step_logged[sidx]) {
        g_step_logged[sidx] = true;
        logf_("  [%s] %s pass %ux%u -> %ux%u, tracked state = 0x%X\n",
              kStreamName[sidx], g_ds_on ? "downsample" : "convert",
              static_cast<unsigned>(src->GetDesc().Width),
              static_cast<unsigned>(src->GetDesc().Height), g_ds_w, g_ds_h,
              static_cast<uint32_t>(before));
    }

    // D3D12 allows DEPTH_WRITE -> ALL_SHADER_RESOURCE directly, and ReShade's
    // shader_resource flag is exactly ALL_SHADER_RESOURCE, so one transition is enough.
    // A "transition" to the state the resource is already in is pointless and was a suspect
    // in the PBZ crash, so skip it.
    if (static_cast<uint32_t>(before) != static_cast<uint32_t>(read_as)) {
        native_barrier(native_list, src, before, read_as);
    }
    const bool ok = ds_run(native_list, which, src, dst);
    if (static_cast<uint32_t>(before) != static_cast<uint32_t>(read_as)) {
        native_barrier(native_list, src, read_as, before);
    }

    tracked_set_state(key, static_cast<uint32_t>(before));
    return ok;
}

// Sizes can differ by a couple of pixels (UE5: MVs 2562x1441, depth 2564x1444) and XeFG
// needs both streams at the same size - mismatched tags made every frame fail with
// ERR_INVALID_ARGUMENT. CopyResource cannot rescale, but CopyTextureRegion can crop: copy
// the top-left min(src,dst) region, which loses at most a couple of edge pixels.
static bool record_copy_region(command_list* cmd, ID3D12GraphicsCommandList* native_list,
                               ID3D12Resource* src, ID3D12Resource* dst, int stream_index)
{
    const uint64_t key = reinterpret_cast<uint64_t>(src);
    uint32_t state = 0;
    const bool found = tracked_state(key, &state) != 0;
    if (!found) { return record_copy_tracked(cmd, native_list, src, dst, stream_index); }

    resource_usage before = static_cast<resource_usage>(state);
    if (static_cast<uint32_t>(before) & 0x80000000u) { before = resource_usage::undefined; }

    if (!g_step_logged[stream_index]) {
        g_step_logged[stream_index] = true;
        const D3D12_RESOURCE_DESC a = src->GetDesc();
        const D3D12_RESOURCE_DESC b = dst->GetDesc();
        logf_("  [%s] cropped copy %llux%llu -> %llux%llu, tracked state = 0x%X\n",
              kStreamName[stream_index], static_cast<unsigned long long>(a.Width),
              static_cast<unsigned long long>(a.Height),
              static_cast<unsigned long long>(b.Width),
              static_cast<unsigned long long>(b.Height), static_cast<uint32_t>(before));
    }

    native_barrier(native_list, src, before, resource_usage::copy_source);

    copy_into_slot(native_list, src, dst);

    native_barrier(native_list, src, resource_usage::copy_source, before);

    tracked_set_state(key, static_cast<uint32_t>(before));
    return true;
}

static UINT queue_depth();          // v10, defined with the credit-window helpers below

static void log_stats()
{
    const double avg = (g_frames_copied > 0) ? g_capture_ms_total / g_frames_copied : 0.0;
    // v10: q = publications the consumer has not ACKed (the real queue depth),
    //     ov = publications we had to drop because no slot was free. Both were
    //     invisible before - "were we ever starved?" could only be guessed.
    const auto* ctl = reinterpret_cast<const mgpu::SharedControl*>(g_map_view);
    const unsigned overrun = ctl ? ctl->producer_overrun : 0;
    const unsigned waits   = ctl ? ctl->producer_waits : 0;
    logf_("  stats: seen=%d copied=%d skipped=%d  avg record=%.3f ms  frame=%llu"
          "  q=%u/%u ov=%u waits=%u\n",
          g_frames_seen, g_frames_copied, g_frames_skipped,
          avg, (unsigned long long)g_frame, queue_depth(), mgpu::kMaxSlots, overrun, waits);
}

// Formats and sizes must match exactly for CopyResource. The loading screen can
// transiently use different resources, so every copy is verified first.
static bool copy_formats_ok(ID3D12Resource* src, ID3D12Resource* dst)
{
    if (!src || !dst) { return false; }
    const D3D12_RESOURCE_DESC a = src->GetDesc();
    const D3D12_RESOURCE_DESC b = dst->GetDesc();
    // a typeless source is allowed to feed its matching typed format
    auto family = [](DXGI_FORMAT f) -> int {
        switch (f) {
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT:      return 32;
        default:                         return static_cast<int>(f);
        }
    };
    return family(a.Format) == family(b.Format);
}

static bool copy_compatible(ID3D12Resource* src, ID3D12Resource* dst)
{
    if (!src || !dst) { return false; }
    const D3D12_RESOURCE_DESC a = src->GetDesc();
    const D3D12_RESOURCE_DESC b = dst->GetDesc();
    if (a.Width != b.Width || a.Height != b.Height) { return false; }
    return copy_formats_ok(src, dst);
}

// Stage 1 documents what this build does: only the two NGX streams. The swap
// chain back buffer is deliberately left alone - a barrier on it inside
// reshade_begin_effects collided with the game's ResizeBuffers path and hung the
// process, so colour will come from the NGX output resource instead (also a
// tracked resource), or from a dedicated capture swap chain later.
static int g_stage = 1;

static void advance_stage()
{
    if (g_stage < 2 && (g_frames_copied % 120) == 0 && g_frames_copied > 0) {
        ++g_stage;
        logf_("  STAGE -> %d (1=mv, 2=+depth)\n", g_stage);
    }
}

// D3D12 does not keep a resource alive just because a command list references
// it: the application must hold a reference. The loading screen recreates
// resources, so a raw pointer captured from NGX and used a moment later can point
// at freed memory and hang the GPU. Hold a reference for a few frames.
static const int kDeferredReleaseFrames = 4;

static void hold_resource(ID3D12Resource* res)
{
    if (!res) { return; }
    res->AddRef();
    AcquireSRWLockExclusive(&g_release_lock);
    g_deferred_release.emplace_back(res, g_frame);
    ReleaseSRWLockExclusive(&g_release_lock);
}

static void release_old_resources()
{
    // v8: placed resources retired by a layout rebuild. The consumer may still have a
    // frame in flight that references this memory, so they outlive the rebuild.
    for (auto it = g_retired_slots.begin(); it != g_retired_slots.end();) {
        if (g_frame > it->second + 8) { it = g_retired_slots.erase(it); }
        else { ++it; }
    }

    std::vector<ID3D12Resource*> to_release;
    {
        AcquireSRWLockExclusive(&g_release_lock);
        for (auto it = g_deferred_release.begin(); it != g_deferred_release.end();) {
            if (g_frame > it->second + kDeferredReleaseFrames) {
                to_release.push_back(it->first);
                it = g_deferred_release.erase(it);
            } else {
                ++it;
            }
        }
        ReleaseSRWLockExclusive(&g_release_lock);
    }
    for (ID3D12Resource* r : to_release) { r->Release(); }
}

static void on_init_swapchain(swapchain* sw, bool resize)
{
    // A resize recreates every game resource; touching them while that happens is
    // what hung earlier runs, so stay out of the way for a couple of seconds.
    g_skip_capture_until = g_frame + 120;
    logf_("\ninit_swapchain: %p resize=%d - pausing capture until frame %llu\n",
          (void*)sw, resize ? 1 : 0, (unsigned long long)g_skip_capture_until);
}

// Reads an optional ini next to the add-on. The colour copy is by far the most
// expensive stream (56 MB/frame, serialised with the game's 3D work), so it has
// to be switchable while the WGC path is being built.
//
// RenderWidth/RenderHeight are only needed when NGX does not report the render size
// (or reports the wrong one). Normally the MV/depth streams follow NGX: DLSS Quality
// at 4K renders 1440p, DLAA renders 2160p, Performance renders 1080p.
static void load_config()
{
    char path[MAX_PATH]{};
    GetModuleFileNameA(g_self, path, MAX_PATH);
    if (char* dot = strrchr(path, '.')) {
        strcpy_s(dot, MAX_PATH - (dot - path), ".ini");
    }

    char value[32]{};
    if (GetPrivateProfileStringA("capture", "Color", "1", value, sizeof(value), path) > 0) {
        g_capture_colour = (atoi(value) != 0);
    }

    g_render_w_ini = static_cast<UINT>(GetPrivateProfileIntA("capture", "RenderWidth", 0, path));
    g_render_h_ini = static_cast<UINT>(GetPrivateProfileIntA("capture", "RenderHeight", 0, path));

    // StreamPercent > 100 renders MV/depth smaller before the cross-adapter copy:
    // 150 sends them at render * 100/150 (2160p -> 1440p, 2.25x less traffic).
    g_stream_percent = GetPrivateProfileIntA("capture", "StreamPercent", 100, path);
    // Default ON: the tagged MVs are a dead input for this libxess_fg build, so the
    // convert pass and the 14.7 MB/frame cross-adapter write buy nothing. SkipMv=0
    // restores the stream (e.g. for a different FG library).
    g_skip_mv = GetPrivateProfileIntA("capture", "SkipMv", 1, path) != 0;
    if (g_stream_percent < 100) { g_stream_percent = 100; }
    if (g_stream_percent > 400) { g_stream_percent = 400; }

    // StreamAlign: 1 = use the capture size exactly (no cropping, the MV field lines up with
    // the picture pixel for pixel). >1 rounds the stream size down to that multiple and
    // crops the rest - useful only if a library rejects odd sizes, but it shifts the field.
    // TestSize=N: dev hook (see g_test_rebuild) - forces the published content size to
    // alternate every N frames, so the resize path can be exercised without touching
    // the game's upscaler settings (protocol v9: only the rectangle changes).
    g_test_rebuild = GetPrivateProfileIntA("capture", "TestSize", 0, path);
    g_slot_policy  = GetPrivateProfileIntA("capture", "SlotPolicy", 0, path);
    g_slot_wait_us = GetPrivateProfileIntA("capture", "SlotWaitUs", 2000, path);

    g_stream_align = GetPrivateProfileIntA("capture", "StreamAlign", 1, path);
    if (g_stream_align != 1 && g_stream_align != 2 && g_stream_align != 4 &&
        g_stream_align != 8 && g_stream_align != 16) {
        g_stream_align = 1;
    }

    // DepthConvert=0 turns the depth pass off (escape hatch: it is the only thing that
    // touches the game's depth resource, so it is the first thing to disable if a title
    // misbehaves). **Default is 1**: depth is the one input XeFG actually consumes, and a
    // missing ini key silently disabling it is invisible (measured: a rewritten ini without
    // this key produced a depth-less stream and `--dump` reported `depth=0`).
    g_depth_convert_cfg = GetPrivateProfileIntA("capture", "DepthConvert", 1, path) ? 1 : 0;

    // MvNormalize=1 divides the motion vectors by the render size (pixel units -> UV),
    // MvFlip=1 negates them (backwards vectors). Both are UE5 fix-ups; each costs one
    // fullscreen draw. 0 / 0 keeps the old identity copy path.
    g_mv_normalize_cfg = GetPrivateProfileIntA("capture", "MvNormalize", 0, path) ? 1 : 0;
    g_mv_flip_cfg      = GetPrivateProfileIntA("capture", "MvFlip", 0, path) ? 1 : 0;

    // Input=auto|ngx|ffx|xess|depth: which upscaler to take the frame from. auto (the
    // default) uses whichever source reports data first; pinning one is for titles
    // that load two upscalers and use only one of them.
    char input_cfg[16]{};
    if (GetPrivateProfileStringA("capture", "Input", "auto", input_cfg,
                                 sizeof(input_cfg), path) > 0) {
        g_force_input = input_cfg;
        for (char& c : g_force_input) { c = static_cast<char>(tolower(c)); }
    }
    if (g_force_input.empty()) { g_force_input = "auto"; }

    logf_("config: Color=%d RenderWidth=%u RenderHeight=%u StreamPercent=%d SkipMv=%d DepthConvert=%d"
          " MvNormalize=%d MvFlip=%d Input=%s (path %s)\n",
          g_capture_colour ? 1 : 0, g_render_w_ini, g_render_h_ini, g_stream_percent,
          g_skip_mv ? 1 : 0,
          g_depth_convert_cfg, g_mv_normalize_cfg, g_mv_flip_cfg,
          g_force_input.c_str(), path);
}

// Asks the sources for this frame's inputs. The first source that has data wins and
// is pinned for the rest of the process: switching mid-run would change the stream
// sizes (the shared block is sized once) and the meaning of the depth buffer.
static bool fetch_inputs()
{
    const bool forced = !g_force_input.empty() && g_force_input != "auto";

    if (!g_source) {
        for (auto* s : g_sources) {
            if (forced && g_force_input != s->name()) { continue; }
            mgpu::input::FrameInputs in;
            if (!s->poll(in)) { continue; }
            // Only a *live* source counts. After an upscaler switch the old API stops
            // being called, but its last frame is still buffered - pinning it there means
            // the session publishes nothing at all from then on (measured: DLSS->FSR->XeSS
            // pinned the silent ngx source, 0 published frames while XeSS was running).
            if (in.tick_ms == 0 ||
                (GetTickCount64() - in.tick_ms) > 250ull) {
                continue;
            }
            g_source    = s;
            g_inputs    = in;
            g_input_api = s->api_id();
            logf_("\ninput source: %s (api %u)%s - pinned for this process\n",
                  s->name(), g_input_api, forced ? " [forced by [capture] Input]" : "");
            return true;
        }
        if ((g_frames_seen % 120) == 1) {
            char list[128]{};
            for (auto* s : g_sources) {
                if (forced && g_force_input != s->name()) { continue; }
                if (list[0]) { strcat_s(list, ", "); }
                strcat_s(list, s->name());
            }
            logf_("  no upscaler input yet (waiting for %s)\n",
                  list[0] ? list : "a source");
        }
        // After ~15 s of nothing, say why: the common cause is an upscaler we do not hook
        // (UE5 TSR, FSR1, native TAA, or the game's own frame generation). Measured with
        // TSR: 3120 game frames, 149 published - i.e. colour only, no MV/depth at all.
        static bool hinted = false;
        if (!hinted && g_frames_seen > 600) {
            hinted = true;
            char names[128]{};
            for (auto* s : g_sources) {
                if (forced && g_force_input != s->name()) { continue; }
                if (names[0]) { strcat_s(names, ", "); }
                strcat_s(names, s->name());
            }
            logf_("\n  NOTE: none of the hooked upscalers (%s) has produced a frame for ~15 s.\n"
                  "        If the game is set to TSR / FSR1 / native TAA (or its own frame\n"
                  "        generation), the bridge still shows the picture but gets no MV/depth\n"
                  "        until an upscaler we hook is selected again.\n",
                  names[0] ? names : "a source");
        }
        return false;
    }

    mgpu::input::FrameInputs in;
    if (!g_source->poll(in)) {
        // Nothing new. After an upscaler switch the old API simply stops being
        // called, and holding on to its textures means copying from resources the
        // game has already destroyed - release the source and let the registry pick
        // the new one as soon as it produces.
        if (++g_source_quiet >= 90) {
            logf_("\ninput source %s went quiet (no new frame for 90 frames) - re-arming\n",
                  g_source->name());
            g_source = nullptr;
            g_inputs = {};
            g_source_quiet = 0;
        }
        return false;
    }
    g_source_quiet = 0;
    g_inputs    = in;
    g_input_api = g_source->api_id();
    return true;
}
// ---------------------------------------------------------------- v10 credit window
// The consumer ACKs every slot it has finished reading (`consumed[slot]` = that slot's
// published fence value). Before overwriting slot k we must see the ACK, otherwise the
// two GPUs touch the same memory at the same time - which is what the old blind
// round-robin relied on luck for.
//
// Liveness, not "has anybody ever ACKed": consumed[] keeps its last values after the
// bridge exits, so a one-shot test left the producer dropping every publication for ever
// (measured: 39% of frames dropped, q stuck at 3/4). The window is only enforced while the
// ACK sum is still moving; 120 silent frames (3 s at 40 fps) means the consumer is gone and
// we fall back to the old blind round-robin instead of starving the game.
// 30 frames (0.75 s at 40 fps) instead of 120: a short grace only costs a consumer that
// pauses briefly its window for a moment, and the measured start/stop transients dropped
// ~116 pointless publications each at 120 frames.
static const uint64_t kAckGraceFrames = 30;
static uint64_t g_ack_sum = 0;
static uint64_t g_ack_idle = 0;
static bool     g_credit_on = false;

static void update_credit_state()
{
    auto* ctl = reinterpret_cast<mgpu::SharedControl*>(g_map_view);
    if (!ctl || !g_shared_ready) { g_credit_on = false; return; }
    uint64_t sum = 0;
    for (UINT i = 0; i < mgpu::kMaxSlots; ++i) { sum += ctl->consumed[i]; }
    if (sum != g_ack_sum) { g_ack_sum = sum; g_ack_idle = 0; }
    else if (g_ack_idle < 1000000ull) { ++g_ack_idle; }
    const bool alive = (g_ack_idle < kAckGraceFrames);
    if (alive != g_credit_on) {
        g_credit_on = alive;
        logf_("\nshared: credit window %s - consumer %s\n",
              alive ? "ON" : "OFF (falling back to round-robin)",
              alive ? "is ACKing" : "has been silent for 30 frames");
    }
}

static bool slot_free_state(UINT slot)
{
    if (!g_credit_on) { return true; }                     // no live consumer: old behaviour
    auto* ctl = reinterpret_cast<mgpu::SharedControl*>(g_map_view);
    if (!ctl || !g_shared_ready) { return true; }
    const uint64_t published = g_fwd_value[slot];          // last value we signalled
    if (published == 0) { return true; }                   // never published
    return ctl->consumed[slot] >= published;
}

// Round-robin from the next slot, but only onto a slot the consumer is done with.
// Returns mgpu::kMaxSlots when none is free (the caller applies the policy).
static UINT pick_free_slot()
{
    for (UINT i = 0; i < mgpu::kMaxSlots; ++i) {
        const UINT s = (g_slot_index + i) % mgpu::kMaxSlots;
        if (slot_free_state(s)) { return s; }
    }
    return mgpu::kMaxSlots;
}

// Publications the consumer has not ACKed yet = the real queue depth.
static UINT queue_depth()
{
    auto* ctl = reinterpret_cast<mgpu::SharedControl*>(g_map_view);
    if (!ctl || !g_shared_ready) { return 0; }
    UINT n = 0;
    for (UINT i = 0; i < mgpu::kMaxSlots; ++i) {
        if (g_fwd_value[i] != 0 && ctl->consumed[i] < g_fwd_value[i]) { ++n; }
    }
    return n;
}

// Claim the slot for this frame and publish its diagnostics. Policy comes from
// [capture] SlotPolicy (0 = drop, 1 = wait up to SlotWaitUs).
static bool claim_publish_slot()
{
    update_credit_state();
    UINT slot = pick_free_slot();
    if (slot >= mgpu::kMaxSlots && g_slot_policy == 1 && g_slot_wait_us > 0) {
        auto* ctl = reinterpret_cast<mgpu::SharedControl*>(g_map_view);
        if (ctl) { ++ctl->producer_waits; }
        const ULONGLONG t0 = GetTickCount64();
        while (slot >= mgpu::kMaxSlots) {
            YieldProcessor();
            if (static_cast<int>(GetTickCount64() - t0) * 1000 > g_slot_wait_us) { break; }
            slot = pick_free_slot();
        }
    }
    if (slot >= mgpu::kMaxSlots) {
        auto* ctl = reinterpret_cast<mgpu::SharedControl*>(g_map_view);
        if (ctl) { ++ctl->producer_overrun; }
        return false;
    }
    g_cur_slot = slot;
    return true;
}



static void on_begin_effects(effect_runtime* rt, command_list* cmd,
                             resource_view /*rtv*/, resource_view /*rtv_srgb*/)
{
    ++g_frames_seen;
    if (!fetch_inputs()) { return; }

    if (g_frame < g_skip_capture_until) { return; }
    release_old_resources();

    device* dev = rt->get_device();
    ID3D12Device* game_device = reinterpret_cast<ID3D12Device*>(dev->get_native());
    if (!game_device) { return; }

    if (!g_game_device) { g_game_device = game_device; }
    if (!ensure_shared(game_device)) { return; }
    // v8: the render resolution follows the upscaler preset/mode; rebuild the slot
    // layout when it moved so the bridge can re-place without a restart.
    maybe_update_content_size(game_device);

    // v10: only write into a slot the consumer has ACKed; drop (or wait, per
    // [capture] SlotPolicy) when every slot is still in use. Dropping is the real-time
    // choice: the consumer keeps the frame it has and we lose one publication, instead
    // of tearing a slot the other GPU is reading.
    if (!claim_publish_slot()) { return; }

    ID3D12GraphicsCommandList* native_list =
        reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    if (!native_list) { return; }

    g_game_queue = reinterpret_cast<ID3D12CommandQueue*>(rt->get_command_queue()->get_native());

    const double t0 = now_ms();

    // The streams are sized once per game process, so flipping the DLSS mode mid-run
    // (Quality <-> DLAA changes the render size) leaves them mismatched. Say so instead
    // of silently copying nothing. The padding StreamAlign trims is expected, so compare
    // against the aligned size before complaining.
    UINT expect_w = g_inputs.render_w;
    UINT expect_h = g_inputs.render_h;
    if (g_stream_align > 1) {
        const UINT a = static_cast<UINT>(g_stream_align);
        expect_w -= expect_w % a;
        expect_h -= expect_h % a;
    }
    if (!g_logged_size_mismatch && g_mv_w && g_inputs.render_w &&
        (expect_w != g_mv_w || expect_h != g_mv_h)) {
        g_logged_size_mismatch = true;
        logf_("WARNING: render size is now %ux%u but the shared block was built for %ux%u.\n"
              "         MV/depth copies will be rejected - restart the game after changing\n"
              "         the DLSS mode (or set [capture] RenderWidth/RenderHeight).\n",
              g_inputs.render_w, g_inputs.render_h, g_mv_w, g_mv_h);
    }

    bool ok = true;

    // ---- colour: the swap chain back buffer ----
    // Was blamed for the earlier hangs, but the real culprit turned out to be the
    // barrier event hot path. Kept behind the same guard as before: skip while a
    // swap chain resize is in flight, and verify the desc before copying.
    if (!g_capture_colour) {
        // colour will come from WGC in the bridge process instead; this copy is
        // what pushed the game's GPU from ~80% to ~95%
    } else {
        ID3D12Resource* dst = g_slot[0][g_cur_slot].Get();
        const resource bb = rt->get_current_back_buffer();
        ID3D12Resource* bb_native = reinterpret_cast<ID3D12Resource*>(bb.handle);

        if (copy_compatible(bb_native, dst)) {
            const bool first_time = !g_step_logged[0];
            if (first_time) {
                g_step_logged[0] = true;
                logf_("  [color] first copy from back buffer %p\n", (void*)bb_native);
            }
            resource_usage before = resource_usage::render_target;
            if (first_time) { logf_("  [color] barrier -> copy_source\n"); }
            native_barrier(native_list, bb_native, before, resource_usage::copy_source);
            if (first_time) { logf_("  [color] CopyResource\n"); }
            native_list->CopyResource(dst, bb_native);
            if (first_time) { logf_("  [color] barrier -> restore\n"); }
            native_barrier(native_list, bb_native, resource_usage::copy_source, before);
        } else {
            if (!g_logged_missing_state[0]) {
                g_logged_missing_state[0] = true;
                logf_("  [color] skipped: back buffer does not match the capture target\n");
            }
            ok = false;
        }
    }

    // ---- motion vectors (tracked resource, not a swap chain buffer) ----
    // v7: only when the consumer asks (it tags MVs only if its UI selection says so).
    // SkipMv=1 in the ini forces it off; SkipMv=0 means "follow the consumer".
    {
        auto* ctl = g_map_view ? reinterpret_cast<mgpu::SharedControl*>(g_map_view) : nullptr;
        const bool want_mv = ctl && (ctl->consumer_request & mgpu::kRequestMv) != 0;
        g_mv_active = want_mv && !g_skip_mv;
        if (g_mv_active && !g_mv_active_logged) {
            g_mv_active_logged = true;
            logf_("  [mv] the consumer asked for motion vectors - stream enabled\n");
        }
    }
    if (!g_mv_active) {
        // Nobody consumes this stream: neither convert it nor write it across the bus.
        // The slot keeps its last content; the bridge zeroes MV when it asked for none.
    } else {
        ID3D12Resource* dst = g_slot[1][g_cur_slot].Get();
        bool mv_ok = false;
        // Prefer the shader pass. The direct CopyResource pulls the game's swizzled MV
        // texture into a row-major cross-adapter slot, and on PBZ that measured as
        // garbage: a --dump of the stream showed +-40000 values with 3% NaNs and a
        // constant -54 in Y, while the depth stream - which has always gone through
        // this same pass - was perfectly sane. Sampling with an SRV makes the source
        // layout irrelevant.
        if (g_ds_on || g_mv_pass || g_ds_ready) {
            mv_ok = record_downsample(cmd, native_list, g_inputs.mv, dst, 0);
        }
        if (!mv_ok && copy_compatible(g_inputs.mv, dst)) {
            mv_ok = record_copy_tracked(cmd, native_list, g_inputs.mv, dst, 1);
        }
        if (!mv_ok && copy_formats_ok(g_inputs.mv, dst)) {
            // Same format but a padded size (UE5 2562x1441 vs our aligned 2560x1440):
            // crop it instead of giving up - a skipped MV used to stop publishing
            // entirely, which looked exactly like "XeFG ignores the vectors".
            mv_ok = record_copy_region(cmd, native_list, g_inputs.mv, dst, 1);
        }
        if (!mv_ok && !g_logged_missing_state[1]) {
            g_logged_missing_state[1] = true;
            logf_("  [mv] skipped: source does not match the capture target\n");
        }
        ok = mv_ok;
    }

    // ---- depth ----
    // A depth failure must NOT stop us from publishing: `ok` drives the fence signal, and
    // losing it means the bridge reads stale slots for the rest of the session (that is
    // what the 20 ms waits and the black lines came from). Motion vectors alone already
    // give usable interpolation, so depth problems are logged, not fatal.
    if (ok && g_stage >= 2) {
        ID3D12Resource* dst = g_slot[2][g_cur_slot].Get();
        bool depth_ok = false;
        ID3D12Resource* shadow = nullptr;
        if (g_depth_pass && g_depth_convert_cfg == 2) { shadow = reshade_depth_resource(); }
        if (g_depth_pass && g_depth_convert_cfg == 1) {
            // DepthConvert=1: same-frame real depth via our typeless copy.
            depth_ok = record_depth_convert(native_list, g_inputs.depth, dst);
        } else if (shadow) {
            // ReShade's own R32F depth output, one frame old and in a state we know.
            depth_ok = record_shadow_depth_copy(native_list, shadow, dst,
                                                resource_usage::render_target);
        } else if (g_ds_on) {
            // Depth is rendered into the R32 stream: converts D32_S8X24 (UE5) to R32 and
            // rescales to the MV size in the same pass, so no crop and no format guessing.
            depth_ok = record_downsample(cmd, native_list, g_inputs.depth, dst, 1);
        } else if (copy_compatible(g_inputs.depth, dst)) {
            depth_ok = record_copy_tracked(cmd, native_list, g_inputs.depth, dst, 2);
        } else if (copy_formats_ok(g_inputs.depth, dst)) {
            // Same format, a couple of pixels bigger (UE5 2564x1444 vs 2562x1441):
            // crop it so both streams XeFG sees are exactly the same size.
            depth_ok = record_copy_region(cmd, native_list, g_inputs.depth, dst, 2);
        } else {
            if (!g_logged_missing_state[2]) {
                g_logged_missing_state[2] = true;
                const D3D12_RESOURCE_DESC a = g_inputs.depth ? g_inputs.depth->GetDesc()
                                                                  : D3D12_RESOURCE_DESC{};
                const D3D12_RESOURCE_DESC b = dst ? dst->GetDesc() : D3D12_RESOURCE_DESC{};
                logf_("  [depth] skipped: src %llux%llu fmt %u -> dst %llux%llu fmt %u"
                      " (publishing with motion vectors only)\n",
                      static_cast<unsigned long long>(a.Width),
                      static_cast<unsigned long long>(a.Height),
                      static_cast<unsigned>(a.Format),
                      static_cast<unsigned long long>(b.Width),
                      static_cast<unsigned long long>(b.Height),
                      static_cast<unsigned>(b.Format));
            }
        }
        if (!depth_ok && !g_logged_missing_state[2]) {
            g_logged_missing_state[2] = true;
            logf_("  [depth] copy failed - publishing with motion vectors only\n");
        }
    }

    const double dt = now_ms() - t0;
    g_capture_ms_total += dt;
    if (ok) { ++g_frames_copied; } else { ++g_frames_skipped; }
    g_copied_this_frame = ok;
    advance_stage();

    if ((g_frames_seen % 120) == 1) {
        logf_("\n-- frame %llu stage=%d  mv=%p depth=%p  jitter=(%.4f, %.4f)  render=%ux%u "
              "mvscale=(%.6f, %.6f)  record=%.3f ms\n",
              (unsigned long long)g_frame, g_stage, (void*)g_inputs.mv,
              (void*)g_inputs.depth, g_inputs.jitter_x, g_inputs.jitter_y,
              g_inputs.render_w, g_inputs.render_h,
              g_inputs.mv_scale_x, g_inputs.mv_scale_y, dt);
        log_stats();
    }
}
// Present: with no consumer in this process there is nothing to publish. Kept as
// the place where the (future) named-mapping producer will push a frame.
static void on_reshade_present(effect_runtime* /*rt*/)
{
    // Present happens after the game submitted the list our copies live on, so a
    // queue signal here is ordered behind them: the bridge can trust the slot.
    //
    // v6: one Present per real frame, so this is also where the game's own frame
    // interval is measured. ReShade presents on the render thread; now_ms() is QPC.
    {
        const double now = now_ms();
        if (g_last_present_ms > 0.0) {
            const double dt = now - g_last_present_ms;
            if (dt > 0.5 && dt < 1000.0) { g_present_interval_ms = static_cast<float>(dt); }
        }
        g_last_present_ms = now;
    }
    if (g_shared_ready && g_game_queue && g_copied_this_frame) {
        const UINT slot = g_cur_slot;
        const UINT64 value = ++g_fwd_value[slot];
        auto* ctl = g_map_view ? reinterpret_cast<mgpu::SharedControl*>(g_map_view) : nullptr;
        // Protocol v5: stamp the frame record *before* the fence signal, so a consumer
        // that sees the slot complete always also sees the matching metadata (the old
        // order wrote it after the signal, which raced the consumer's read).
        if (ctl) {
            ctl->frames[slot].ready      = 0;      // invalidate while we rewrite the rest
            MemoryBarrier();
            ctl->frame_published         = g_frame;
            ctl->frames[slot].frame_id   = g_inputs.serial;
            ctl->frames[slot].jitter_x   = g_inputs.jitter_x;
            ctl->frames[slot].jitter_y   = g_inputs.jitter_y;
            ctl->frames[slot].render_w   = g_inputs.render_w;
            ctl->frames[slot].render_h   = g_inputs.render_h;
            ctl->frames[slot].mv_scale_x = g_inputs.mv_scale_x;
            ctl->frames[slot].mv_scale_y = g_inputs.mv_scale_y;
            // v6: camera comes from the source when it has one (FSR 3.1 exports
            // near/far/vfov; DLSS-SR does not, so those stay 0 and the bridge keeps
            // using --fg-fov). The matching flag lives in ctl->producer.input_flags.
            ctl->frames[slot].near_plane    = g_inputs.near_plane;
            ctl->frames[slot].far_plane     = g_inputs.far_plane;
            ctl->frames[slot].vfov_rad      = g_inputs.vfov_rad;
            ctl->frames[slot].frame_time_ms = g_present_interval_ms;
            ctl->frames[slot].published     = value;   // v10: what the consumer ACKs with
            ctl->producer.input_flags       = (g_inputs.flags & ~mgpu::kInputFlagMvActive) |
                                              (g_mv_active ? mgpu::kInputFlagMvActive : 0);
            MemoryBarrier();
        }
        if (SUCCEEDED(g_game_queue->Signal(g_fwd[slot].Get(), value))) {
            g_fwd_expected[slot] = value;
            g_slot_index = (g_cur_slot + 1) % mgpu::kMaxSlots;
        }
    }
    g_copied_this_frame = false;
    ++g_frame;

    // NOTE: do NOT poll the shared fences here. GetCompletedValue() on a
    // D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER fence goes through a driver-mediated
    // synchronisation path; four calls per present stalled the game's render thread
    // (game 33 -> 60 ms, our own loop 30 -> 15 fps, frames halved, picture flickers).
    // The consumer therefore falls back to walking the slots round-robin, which has
    // always been the behaviour that ran at full rate.
}

static void on_init_effect_runtime(effect_runtime* rt)
{
    uint32_t w = 0, h = 0;
    rt->get_screenshot_width_and_height(&w, &h);
    logf_("\ninit_effect_runtime: %p  buffer %ux%u\n", (void*)rt, w, h);
    g_rt_buffer_w = w;               // MvNormalize=2 divides by this
    g_rt_buffer_h = h;
    g_rt = rt;                       // needed to find mgpu_depth_tap.fx's output texture
    g_rt_depth_lookup_done = false;
    g_rt_depth_var = {};
    g_rt_depth_res = nullptr;
}

// ------------------------------------------------------------------ add-on

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
    g_self = addon_module;
    open_log();
    logf_("AddonInit (addon=%p reshade=%p)\n", (void*)addon_module, (void*)reshade_module);
    // Build stamp: the DLL is only picked up when the game process starts, so "which
    // version am I running" is the first question every time something looks unchanged.
    logf_("addon build: %s %s\n", __DATE__, __TIME__);

    if (reshade::register_addon(addon_module, reshade_module)) {
        logf_("registered with ReShade\n");
    } else {
        logf_("reshade::register_addon FAILED\n");
    }

    // Input sources first: each one hooks its own upscaler module, so registering
    // them before any module can load means the loader hooks below have somewhere to
    // deliver. Order matters only for `Input=auto` (first one with data wins), and
    // both entries represent what a game actually runs - a title uses DLSS *or* FSR.
    g_sources.push_back(mgpu::input::make_ngx_source());
    g_sources.push_back(mgpu::input::make_ffx_source());
    g_sources.push_back(mgpu::input::make_xess_source());

    // Loader hooks, so a source is told about a module the moment the game loads it
    // (M2: for DLSS that module is the driver-store _nvngx.dll).
    mgpu::input::ensure_minhook();
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    struct LoaderHook { const char* name; LPVOID detour; LPVOID* original; };
    LoaderHook hooks[] = {
        { "LoadLibraryExW", reinterpret_cast<LPVOID>(&hook_load_library_ex_w),
                            reinterpret_cast<LPVOID*>(&g_real_load_library_ex_w) },
        { "LoadLibraryW",   reinterpret_cast<LPVOID>(&hook_load_library_w),
                            reinterpret_cast<LPVOID*>(&g_real_load_library_w) },
    };
    for (auto& lh : hooks) {
        LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(kernel32, lh.name));
        if (!addr) { continue; }
        if (MH_CreateHook(addr, lh.detour, lh.original) == MH_OK &&
            MH_EnableHook(addr) == MH_OK) {
            logf_("loader hook %s OK\n", lh.name);
        }
    }
    scan_loaded_modules();

    reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
    reshade::register_event<reshade::addon_event::barrier>(on_barrier);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
    reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);

    logf_("events: init_effect_runtime, barrier, reshade_begin_effects, reshade_present\n");
    load_config();


    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
{
    logf_("AddonUninit\n");
    log_stats();

    reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present);
    reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
    reshade::unregister_event<reshade::addon_event::barrier>(on_barrier);
    reshade::unregister_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
    reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::unregister_addon(addon_module, reshade_module);

    if (g_map_view) { UnmapViewOfFile(g_map_view); g_map_view = nullptr; }
    if (g_map) { CloseHandle(g_map); g_map = nullptr; }
    if (g_mh_ready) { MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize(); }
    if (g_log) { fclose(g_log); g_log = nullptr; }
}



