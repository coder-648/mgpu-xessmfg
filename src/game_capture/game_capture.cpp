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
// Where the add-on reads its ini from.
//  1. next to the DLL (the normal case);
//  2. %LOCALAPPDATA%\mgpu\game_capture.ini when that file does not exist - some titles run
//     with a redirected / read-only game directory (PRAGMATA loads the add-on from
//     E:\PRAGMATA\_storage_, so the ini is looked up there and a copy in the game root is
//     never read; see docs/findings/RE_ENGINE_NOTES.md 6.4).
static void resolve_ini_path(char* out, size_t count)
{
    out[0] = '\0';
    if (g_self != nullptr) { GetModuleFileNameA(g_self, out, (DWORD)count); }
    if (char* dot = strrchr(out, '.')) {
        strcpy_s(dot, count - (size_t)(dot - out), ".ini");
    }
    if (out[0] != '\0' && GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) { return; }

    char dir[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) { return; }         // keep the local path (defaults)
    char alt[MAX_PATH]{};
    _snprintf_s(alt, sizeof(alt), _TRUNCATE, "%s\\mgpu\\game_capture.ini", dir);
    if (GetFileAttributesA(alt) == INVALID_FILE_ATTRIBUTES) { return; }
    strcpy_s(out, count, alt);
}

static bool log_requested()
{
    char env[16]{};
    if (GetEnvironmentVariableA("MGPU_CAPTURE_LOG", env, sizeof(env)) > 0 &&
        env[0] != '0') {
        return true;
    }
    char ini[MAX_PATH]{};
    resolve_ini_path(ini, sizeof(ini));
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
    if (g_log == nullptr) {
        // Fallback: some titles (or their DRM) refuse writes next to the game executable,
        // and then *every* diagnostic goes silent - which is exactly what happened with
        // PRAGMATA (RE Engine, 2026-11-09): ReShade logged "Registered add-on" from inside
        // AddonInit, yet this file never appeared. Record the attempt next to the user's
        // profile instead, including why the local one failed.
        const DWORD err = GetLastError();
        char dir[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            strcat_s(dir, "\\mgpu");
            CreateDirectoryA(dir, nullptr);
            char alt[MAX_PATH]{};
            _snprintf_s(alt, sizeof(alt), _TRUNCATE, "%s\\game_capture-%lu.log", dir,
                        (unsigned long)GetCurrentProcessId());
            g_log = _fsopen(alt, "a", _SH_DENYNO);
            if (g_log != nullptr) {
                logf_("=== local log '%s' could not be opened (error %lu), using %s ===\n",
                      path, (unsigned long)err, alt);
            }
        }
    }
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

// A source cannot resolve the ini path itself (resolve_ini_path is the host's, and it
// knows about the %LOCALAPPDATA% fallback), so config reads go through here. Cheap
// enough for the few times a source asks - in_fg.cpp caches the answer.
int mgpu::input::config_int(const char* key, int def)
{
    if (key == nullptr) { return def; }
    char path[MAX_PATH]{};
    resolve_ini_path(path, sizeof(path));
    return GetPrivateProfileIntA("capture", key, def, path);
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
//
// **Queued, not handled here.** This runs inside LoadLibrary, i.e. with the loader lock held,
// and the handlers do heavy things: install MinHook trampolines (patching another module's
// code) and - for the Streamline sniffer - CreateToolhelp32Snapshot, which takes the loader
// lock itself. Doing that re-entrantly is a classic wedged-process: Wuchang: Fallen Feathers
// hung during start-up exactly here on 2026-11-10 (log ends on the sniffer's first line, no
// crash dump). The queue is drained from on_begin_effects, outside the loader lock.
static HMODULE g_pending_mods[32] = {};
static volatile LONG g_pending_count = 0;
// Set in AddonInit, serviced by the first on_begin_effects (see the comment there).
static bool g_scan_pending = false;
// ReShade unloads add-ons by calling AddonUninit and then freeing the module. A loader-hook
// detour that fires between those two points would touch g_sources (a std::vector with a
// destructor) - the same hot-unload hazard that produced the null-mutex crash on Wuchang.
// Every callback that can outlive AddonUninit checks this first.
static volatile bool g_addon_alive = false;

static void notify_sources(HMODULE m)
{
    if (!m || !g_addon_alive) { return; }
    const LONG i = InterlockedIncrement(&g_pending_count) - 1;
    if (i < static_cast<LONG>(sizeof(g_pending_mods) / sizeof(g_pending_mods[0]))) {
        g_pending_mods[i] = m;
    }
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

// Handle what the loader hooks queued, outside the loader lock (see notify_sources).
static void drain_pending_modules()
{
    if (!g_addon_alive) { return; }
    const LONG n = InterlockedExchange(&g_pending_count, 0);
    if (n <= 0) { return; }
    const LONG cap = static_cast<LONG>(sizeof(g_pending_mods) / sizeof(g_pending_mods[0]));
    const LONG done = (n < cap) ? n : cap;
    if (n > cap) { logf_("modules: %ld loads arrived, only %ld queued (the 120-frame rescan"
                         " will pick the rest up)\n", n, cap); }
    static bool logged = false;
    if (!logged && done > 0) {
        logged = true;
        logf_("modules: %ld queued load(s) handled outside the loader lock\n", done);
    }
    for (LONG i = 0; i < done; ++i) {
        HMODULE m = g_pending_mods[i];
        g_pending_mods[i] = nullptr;
        if (!m) { continue; }
        for (auto* s : g_sources) { s->on_module(m); }
    }
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
// Streamline camera hand-off (protocol v12).
// Why a named shared-memory section instead of plain globals: PBZ rebuilds its swapchain at
// startup (init_swapchain resize=0/1 in the log), ReShade reloads add-ons on that, and the XeFG
// hooks stay installed in the *old* module instance while the present path runs in the new one.
// Their globals are different memory, which is exactly what `raw_tick=0 seq=0` while the writer
// demonstrably ran looked like (2026-11-10 04:23). A named section is the same bytes for every
// instance in the session, so the hand-off cannot depend on who owns the module.
struct FgCamSlot
{
    volatile LONG64 seq;      // odd while a write is in flight
    uint64_t tick;            // when the matrix was captured (0 = nothing yet)
    float    m[16];
    float    near_z, far_z, vfov;
    uint32_t magic;
};
static constexpr uint32_t kFgCamMagic = 0x4d475055u;   // 'MGUP'
static FgCamSlot g_fg_cam_local = {};                  // fallback when the mapping fails
static FgCamSlot* g_fg_cam_slot = nullptr;

static FgCamSlot* fg_cam_slot()
{
    if (g_fg_cam_slot) { return g_fg_cam_slot; }
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  sizeof(FgCamSlot), "Local\\mgpu_fg_camera_v1");
    if (h != nullptr) {
        void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FgCamSlot));
        if (p != nullptr) {
            g_fg_cam_slot = static_cast<FgCamSlot*>(p);
            if (g_fg_cam_slot->magic != kFgCamMagic) {
                memset(g_fg_cam_slot, 0, sizeof(FgCamSlot));
                g_fg_cam_slot->magic = kFgCamMagic;
            }
            return g_fg_cam_slot;
        }
        CloseHandle(h);
    }
    g_fg_cam_slot = &g_fg_cam_local;
    g_fg_cam_local.magic = kFgCamMagic;
    return g_fg_cam_slot;
}

static bool     g_fg_cam_ok   = true;    // [capture] StreamlineCamera=0 turns it off
static bool     g_fg_cam_used = false;   // logged once
static bool     g_fg_cam_live = false;   // set by current_fg_camera() for the current frame


static bool   g_logged_missing_state[3] = { false, false, false };
static bool   g_step_logged[3] = { false, false, false };
static UINT64 g_skip_capture_until = 0;
// Hard wall-clock cap for that pause. The frame count is the real criterion (a resize
// invalidates every game resource and the next few frames are still settling), but on a
// title that presents at ~1 fps while loading 120 frames is minutes - PRAGMATA (RE Engine,
// 2026-11-09) sat at 0.8 fps for 148 s, so the capture stayed paused for the whole start-up.
static double g_skip_until_ms = 0.0;

// --- callback timing (2026-11-09) ------------------------------------------------------
// PRAGMATA (RE Engine) took minutes to load with this add-on and loaded normally without it,
// yet every callback below is a handful of instructions. Measure instead of argue: these
// accumulators are printed with the 120-frame heartbeat, so one run says where the time
// goes. Cost: two QPC reads per callback (~40 ns).
static double   g_perf_barrier_ms    = 0.0;
static double   g_perf_begin_ms      = 0.0;
static double   g_perf_present_ms    = 0.0;
static uint64_t g_perf_barrier_calls = 0;
static uint64_t g_perf_barrier_items = 0;
static bool     g_loader_hook        = true;   // [capture] LoaderHook=0 disables it

struct PerfAccum
{
    double* dst;
    double  t0;
    explicit PerfAccum(double* d) : dst(d), t0(now_ms()) {}
    ~PerfAccum() { *dst += now_ms() - t0; }
};
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

static int  g_stream_percent = 100;   // 100 = the StreamHeight baseline (see below)
// [capture] StreamHeight: the cross-adapter stream never exceeds this height; the width is
// derived from the render aspect and rounded to even. 0 = no cap (follow the render size).
//
// This is what makes "100% = 2560x1440" work: StreamPercent still shrinks relative to the
// render size (>100 = smaller, the historical meaning), and then the height cap pulls the
// result down to 1440p. A 4K render with the defaults therefore streams MV/depth at
// 2560x1440 = 28 MB/frame instead of 63 MB, which is the single biggest lever on the
// render card's per-frame cost (HANDOFF 8-27/8-28, FINAL_CONFIG 5.17.2).
//
// Never upscales: a 1080p render stays 1080p. Set StreamHeight=0 to get the old behaviour
// (full render size) back, or StreamHeight=1080 for the next step down.
static int  g_stream_height = 1440;
// [capture] SkipMv: do not produce the motion-vector stream at all (DEFAULT 1).
// XeSS-FG in this libxess_fg build does not consume tagged motion vectors (MV=0,
// MV=100000 px and the game's own real MVs all render identically - docs/findings/HANDOFF.md
// 8.6), so the converted MV stream is pure cost: a fullscreen pass plus a 14.7 MB
// cross-adapter write per frame. Set SkipMv=0 to bring it back (different FG library).
// One place that decides the stream size: render size -> StreamPercent -> StreamHeight cap.
// Both the layout helper (shared_layout.h) and the downsample setup call this, so the slots
// and the pass targets can never disagree.
static void stream_target_from(UINT render_w, UINT render_h, UINT* out_w, UINT* out_h)
{
    UINT w = render_w, h = render_h;
    if (g_stream_percent > 100) {
        w = w * 100u / static_cast<UINT>(g_stream_percent);
        h = h * 100u / static_cast<UINT>(g_stream_percent);
    }
    if (g_stream_height > 0 && h > static_cast<UINT>(g_stream_height)) {
        w = static_cast<UINT>(static_cast<uint64_t>(w) * static_cast<UINT>(g_stream_height) / h);
        h = static_cast<UINT>(g_stream_height);
    }
    w &= ~1u;                       // even sizes only: the 2:1 downsample and the crop maths
    h &= ~1u;                       // both assume it
    if (w < 256u) { w = 256u; }
    if (h < 256u) { h = 256u; }
    *out_w = w;
    *out_h = h;
}
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
// touching the game: [capture] MvNormalize=auto follows what the source declared, 1 forces
// "divide by the render size", 0 keeps the stream as-is (default - changing the stream's
// unit silently would change what the bridge receives), MvFlip=1 negates.
static bool  g_mv_pass = false;
static int   g_mv_normalize_cfg = 0;    // 0 = as-is (default), -1 = auto (follow declaration)
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

// Defined with the probe code below ds_prepare (it needs the shared state there).
static void probe_create(ID3D12Device* device);

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
    probe_create(device);
    return true;
}


// ============== [capture] OffloadQueue: cross-adapter copies on our own queue ==========
// Why: the MV/depth downscale passes and the 28-63 MB/frame cross-adapter *write* used to be
// recorded into the game's own command list, i.e. they sat between the game's last draw and
// its Present(). Measured cost of that traffic on the render card: ~3.6 ms/frame at 4K
// (docs/findings/PERF_BASELINE.md 3.3), which is exactly what makes frame delivery late on a
// saturated card (3.26).
//
// What this does NOT do (deliberately): it never hands a *game-owned* resource to a second
// queue. Only our own render target (g_ds_rt[which]) and our own shared slot (g_slot[...])
// move, so there is no cross-queue state hand-over with anything the game touches. The
// conversion pass itself stays on the game's list (its source IS a game resource).
//
// Flow per frame:
//   game list : [pass] -> g_ds_rt[which]: RENDER_TARGET -> COPY_SOURCE   (restore deferred)
//   present   : game_queue->Signal(off_fence)   [ordered after that list]
//   our queue : Wait(off_fence) -> CopyTextureRegion(g_ds_rt -> slot)
//               -> g_ds_rt: COPY_SOURCE -> RENDER_TARGET
//               -> Signal(g_fwd[slot])   [the consumer waits on this one]
// Next frame's begin_effects waits (CPU, bounded) for our queue to have restored the state
// before the pass renders into g_ds_rt again. If that wait ever times out we restart the
// texture on the game's list instead and stay in-frame this time (see offload_guard).
//
// Off by default: a second queue inside the game process is the one change whose failure
// mode is a device reset, so it has to be switched on deliberately and watched.
static bool  g_offload = false;                 // [capture] OffloadQueue
static ID3D12CommandQueue*        g_off_queue = nullptr;
static ID3D12CommandAllocator*    g_off_alloc = nullptr;
static ID3D12GraphicsCommandList* g_off_list  = nullptr;
static ID3D12Fence*               g_off_fence = nullptr;   // local, same device
static ID3D12Fence*               g_off_ready = nullptr;   // game queue -> our queue
// The device our queue/allocator/list/fences were created on. Every resource they touch has
// to belong to the same one; the check lives in offload_same_device().
static ID3D12Device*              g_off_dev   = nullptr;
static UINT64 g_off_value   = 0;                // last value signalled on g_off_fence
static UINT64 g_off_ready_value = 0;            // ... and on g_off_ready
static UINT64 g_off_pending = 0;                // value not yet waited for
static UINT   g_off_frames  = 0;                // frames whose copies ran on our queue
static UINT   g_off_stalls  = 0;                // CPU stalls in offload_guard()
static UINT   g_off_failed  = 0;                // times we had to fall back mid-run
static bool   g_off_stream[3] = { false, false, false };   // 1 = mv, 2 = depth
// Which streams our render target is currently left in COPY_SOURCE for. This is the
// authoritative flag for "someone still has to restore this state", so a fallback can always
// put it back even when the copy itself was skipped.
static bool   g_off_deferred[3] = { false, false, false };
static bool   g_off_logged = false;

static void offload_fail(const char* why)
{
    if (g_off_failed == 0) { logf_("  [offload] %s - falling back to the game's queue\r\n", why); }
    ++g_off_failed;
    g_offload = false;
}

static bool offload_ensure(ID3D12Device* dev)
{
    if (g_off_list || g_off_queue) { return true; }
    if (!dev) { return false; }
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&q, __uuidof(ID3D12CommandQueue),
                                       reinterpret_cast<void**>(&g_off_queue))) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           __uuidof(ID3D12CommandAllocator),
                                           reinterpret_cast<void**>(&g_off_alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_off_alloc,
                                      nullptr, __uuidof(ID3D12GraphicsCommandList),
                                      reinterpret_cast<void**>(&g_off_list))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                reinterpret_cast<void**>(&g_off_fence))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                reinterpret_cast<void**>(&g_off_ready)))) {
        offload_fail("could not create the second queue");
        return false;
    }
    g_off_list->Close();
    g_off_dev = dev;
    if (!g_off_logged) {
        g_off_logged = true;
        logf_("  [offload] MV/depth cross-adapter copies run on our own queue"
             " (conversion passes stay on the game's list)\r\n");
    }
    return true;
}

// CPU-side wait for the previous frame's offload work to have restored g_ds_rt. Bounded: our
// copies are ~0.2 ms of copy-engine work and the game frame is 16-33 ms, so this is a
// safety net, not a pacing mechanism. Returns false when the state could not be restored.
static bool offload_guard()
{
    if (!g_offload || !g_off_fence || g_off_pending == 0) { return true; }
    if (g_off_fence->GetCompletedValue() >= g_off_pending) {
        g_off_pending = 0;
        return true;
    }
    const ULONGLONG t0 = GetTickCount64();
    while (g_off_fence->GetCompletedValue() < g_off_pending) {
        if (GetTickCount64() - t0 > 4) {
            ++g_off_stalls;
            offload_fail("the second queue did not retire in time");
            return false;      // caller must re-establish the state on the game's list
        }
        YieldProcessor();
    }
    g_off_pending = 0;
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
// `defer_copy` (OffloadQueue) ends the pass with our render target in COPY_SOURCE instead of
// copying + restoring here: the copy then runs on our own queue in the present callback.
// ---------------------------------------------------------------- online MV probe
//
// Reads a 64x64 patch of the MV stream back every ~2 s and reports which *unit* the field
// is in (pixels vs UV) - the question that used to need a `--dump` and a python fit.
// Everything here is read-only and non-blocking: the copy rides on the command list that
// already has the MV target in COPY_SOURCE, and the map only happens once the fence says
// the GPU is done (a 16 KB staging read, no stall).
//
// The *sign* (previous-current vs current-previous) needs the previous frame's depth to
// test against, so it is not decided here; the line says so explicitly instead of
// guessing.
// 256 instead of 128: PBZ measured mean|v| = 33..56 px per frame during a normal camera
// turn, so a 128 px patch left 73% of the texels outside the "offset <= 16" window
// (2026-11-10 03:26: big=46222 of 63504) - the *correct* scale was starved of votes and a
// wrong one won. A 256 px patch with a 64 texel cap covers ordinary motion at 4K.
static constexpr UINT kProbeDim = 256;
// An offset larger than a quarter patch cannot be tested at all: the sample would come
// from outside the copied patch (nothing was copied there).
static constexpr float kProbeMaxOff = static_cast<float>(kProbeDim) / 4.0f;
static constexpr uint32_t kProbeEvery = 120;   // frames between probes (~2 s at 60 fps)

// Depth is sampled at four places instead of one corner: a single 64x64 patch of a wall
// has no far plane in it, which is exactly what made the first version report
// "ambiguous" on PBZ (min 0.0062 / max 0.0091 - a flat surface 11-16 m away). Sky or the
// far plane nearly always lands in one of these.
static constexpr UINT kProbeSpots = 4;

static ComPtr<ID3D12Resource> g_probe_buf;        // MV patches: 2 gens x kProbeSpots
static ComPtr<ID3D12Resource> g_probe_depth_buf;  // depth patches: 2 gens x kProbeSpots
static ComPtr<ID3D12Fence>    g_probe_fence;
static UINT64                 g_probe_fence_val = 0;
static UINT64                 g_probe_pending   = 0;
static UINT                   g_probe_pitch     = 0;   // mv row pitch
static UINT                   g_probe_offset    = 0;
static UINT                   g_probe_mv_slot   = 0;   // bytes per mv patch
static UINT                   g_probe_d_pitch   = 0;   // depth row pitch
static UINT                   g_probe_d_offset  = 0;
static UINT                   g_probe_d_slot    = 0;   // bytes per depth patch
static bool                   g_probe_ready     = false;
static bool                   g_probe_armed     = false;   // a copy was recorded this frame
// Last probe verdict packed as mgpu::kSem* bits. Published per frame (protocol v11) so the
// consumer can stop guessing; stays 0 until the probe has an answer.
static uint32_t               g_sem             = 0;
// 1 = mv, 2 = depth, and <<2 for generation 1. Generation 0 is "the previous frame",
// copied one frame before generation 1 - that pair is what makes the sign test possible.
static uint32_t               g_probe_have      = 0;
static int                    g_probe_gen       = -1;      // -1 idle, 0/1 = copying
static uint32_t               g_probe_gen_frame = 0;

static float half_to_float(uint16_t h)
{
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits = 0;
    if (exp == 0) {
        if (mant == 0) { bits = sign; }
        else {                          // subnormal
            uint32_t e = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) { mant <<= 1; --e; }
            bits = sign | (e << 23) | ((mant & 0x3FFu) << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    float f = 0.0f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

// Creates a READBACK buffer of `bytes` - used for the multi-patch depth probe.
static bool probe_make_readback_sized(ID3D12Device* device, UINT64 bytes,
                                      ComPtr<ID3D12Resource>& out)
{
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&out)));
}

static void probe_create(ID3D12Device* device)
{
    if (g_probe_ready || !device) { return; }

    // Both streams live in a multi-patch READBACK buffer: 2 generations x 4 spots, so the
    // sign test can compare "this frame" against "the frame before it" without a second
    // copy point or any extra state transition.
    auto alloc = [&](DXGI_FORMAT fmt, ComPtr<ID3D12Resource>& buf, UINT* pitch,
                     UINT* offset, UINT* slot) -> bool {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kProbeDim;
        td.Height = kProbeDim;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0;
        UINT64 row_size = 0, total = 0;
        device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &row_size, &total);
        if (total == 0) { return false; }
        if (!probe_make_readback_sized(device, total * kProbeSpots * 2, buf)) { return false; }
        *pitch = fp.Footprint.RowPitch;
        *offset = static_cast<UINT>(fp.Offset);
        *slot = static_cast<UINT>(total);
        return true;
    };

    if (!alloc(DXGI_FORMAT_R16G16_FLOAT, g_probe_buf, &g_probe_pitch, &g_probe_offset,
               &g_probe_mv_slot)) {
        logf_("probe: mv readback FAILED - probing off\n");
        return;
    }
    // Depth goes through the same machinery. Its direction is what the APIs disagree about
    // in the field (PBZ: DLSS/FSR say reversed, XeSS has no depth at all), so it is
    // measured instead of trusted.
    if (!alloc(DXGI_FORMAT_R32_FLOAT, g_probe_depth_buf, &g_probe_d_pitch, &g_probe_d_offset,
               &g_probe_d_slot)) {
        logf_("probe: depth readback FAILED - probing off\n");
        g_probe_buf.Reset();
        return;
    }
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_probe_fence)))) {
        logf_("probe: CreateFence FAILED - probing off\n");
        g_probe_buf.Reset();
        g_probe_depth_buf.Reset();
        return;
    }
    g_probe_ready  = true;
    logf_("probe: sampling ON (%ux%u x %u spots x 2 frames, every %u frames)\n",
          kProbeDim, kProbeDim, kProbeSpots, kProbeEvery);
}

// Called from ds_run while the target is already in COPY_SOURCE. `which` matches the
// stream: 0 = motion vectors, 1 = depth.
static void probe_issue(ID3D12GraphicsCommandList* list, int which, uint32_t frame)
{
    if (!g_probe_ready || !list || (which != 0 && which != 1)) { return; }
    if (g_ds_w < kProbeDim || g_ds_h < kProbeDim) { return; }

    // Round gating. A previous round must have been consumed before a new one starts, but
    // the *second frame* of the current round must still be copied while its own copies are
    // in flight - the old code bailed out on `pending` here, which kept generation 1 from
    // ever landing and left `sign` permanently unknown (found 2026-11-10: bridge logged
    // "sign=unknown(unknown)" every time).
    if (g_probe_gen < 0) {
        if (g_probe_have != 0 || g_probe_pending != 0) { return; }
        if ((frame % kProbeEvery) != 0) { return; }
        g_probe_gen = 0;
        g_probe_gen_frame = frame;
    } else if (g_probe_gen == 0) {
        if (frame == g_probe_gen_frame) {
            // the other stream of the same frame - stay in generation 0
        } else if (frame == g_probe_gen_frame + 1) {
            g_probe_gen = 1;
        } else {
            g_probe_gen = -1;      // missed the pair (stall, menu); retry next round
            return;
        }
    }

    ID3D12Resource* buf = (which == 0) ? g_probe_buf.Get() : g_probe_depth_buf.Get();
    if (!buf) { return; }
    const UINT pitch  = (which == 0) ? g_probe_pitch : g_probe_d_pitch;
    const UINT offset = (which == 0) ? g_probe_offset : g_probe_d_offset;
    const UINT slot   = (which == 0) ? g_probe_mv_slot : g_probe_d_slot;
    const DXGI_FORMAT fmt = (which == 0) ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R32_FLOAT;

    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = g_ds_rt[which].Get();
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = 0;

    // Same four places for both streams, so MV and depth describe the same pixels.
    const UINT w = g_ds_w, h = g_ds_h;
    const UINT xs[kProbeSpots] = { (w - kProbeDim) / 2, (w - kProbeDim) / 2,
                                   0, w - kProbeDim };
    const UINT ys[kProbeSpots] = { 0, h - kProbeDim,
                                   (h - kProbeDim) / 2, (h - kProbeDim) / 2 };
    const UINT base = static_cast<UINT>(g_probe_gen) * kProbeSpots;

    for (UINT i = 0; i < kProbeSpots; ++i) {
        D3D12_TEXTURE_COPY_LOCATION d{};
        d.pResource = buf;
        d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        d.PlacedFootprint.Offset = offset + static_cast<UINT64>(base + i) * slot;
        d.PlacedFootprint.Footprint.Format = fmt;
        d.PlacedFootprint.Footprint.Width = kProbeDim;
        d.PlacedFootprint.Footprint.Height = kProbeDim;
        d.PlacedFootprint.Footprint.Depth = 1;
        d.PlacedFootprint.Footprint.RowPitch = pitch;

        D3D12_BOX box{};
        box.left = xs[i];
        box.top = ys[i];
        box.front = 0;
        box.right = xs[i] + kProbeDim;
        box.bottom = ys[i] + kProbeDim;
        box.back = 1;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
    }
    g_probe_have |= 1u << (which + 2 * g_probe_gen);
    // The list is only submitted by the game, so the fence has to be signalled from the
    // queue later (probe_report does it once the frame is in flight). This keeps the
    // whole probe off the GPU's critical path.
    g_probe_armed = true;
}

// Called from the present callback: consume the probe when the GPU has finished it and
// turn the numbers into one decision-oriented line.
static void probe_report()
{
    if (!g_probe_ready) { return; }
    // The copies were recorded on the game's list during on_begin_effects; by the time we
    // get to present that list is in flight, so signalling here orders the fence after
    // them. The map itself happens one present later (never blocks).
    if (g_probe_armed) {
        g_probe_armed = false;
        if (g_game_queue != nullptr) {
            ++g_probe_fence_val;
            if (SUCCEEDED(g_game_queue->Signal(g_probe_fence.Get(), g_probe_fence_val))) {
                g_probe_pending = g_probe_fence_val;
            }
        }
        return;
    }
    if (g_probe_pending == 0) { return; }
    if (g_probe_fence->GetCompletedValue() < g_probe_pending) { return; }

    const bool have_mv0  = (g_probe_have & (1u << 0)) != 0;
    const bool have_dep0 = (g_probe_have & (1u << 1)) != 0;
    const bool have_mv1  = (g_probe_have & (1u << 2)) != 0;
    const bool have_dep1 = (g_probe_have & (1u << 3)) != 0;

    void* mv_map = nullptr;
    void* dp_map = nullptr;
    if ((have_mv0 || have_mv1) && g_probe_buf) {
        if (FAILED(g_probe_buf->Map(0, nullptr, &mv_map))) { mv_map = nullptr; }
    }
    if ((have_dep0 || have_dep1) && g_probe_depth_buf) {
        if (FAILED(g_probe_depth_buf->Map(0, nullptr, &dp_map))) { dp_map = nullptr; }
    }

    auto mv_at = [&](UINT gen, UINT spot, UINT x, UINT y, float* ox, float* oy) {
        const uint8_t* b = static_cast<const uint8_t*>(mv_map) + g_probe_offset +
                           static_cast<UINT64>(gen * kProbeSpots + spot) * g_probe_mv_slot;
        const uint16_t* row = reinterpret_cast<const uint16_t*>(b + y * g_probe_pitch);
        *ox = half_to_float(row[x * 2]);
        *oy = half_to_float(row[x * 2 + 1]);
    };
    auto dp_at = [&](UINT gen, UINT spot, int x, int y) -> float {
        if (x < 0) { x = 0; }
        if (y < 0) { y = 0; }
        if (x >= static_cast<int>(kProbeDim)) { x = static_cast<int>(kProbeDim) - 1; }
        if (y >= static_cast<int>(kProbeDim)) { y = static_cast<int>(kProbeDim) - 1; }
        const uint8_t* b = static_cast<const uint8_t*>(dp_map) + g_probe_d_offset +
                           static_cast<UINT64>(gen * kProbeSpots + spot) * g_probe_d_slot;
        const float* row = reinterpret_cast<const float*>(b + y * g_probe_d_pitch);
        return row[x];
    };

    // ---- 1. unit: magnitude class over every patch we have ----
    uint32_t n_valid = 0, n_invalid = 0, n_large = 0;
    double sum_x = 0.0, sum_y = 0.0;
    if (mv_map != nullptr) {
        for (UINT gen = 0; gen < 2; ++gen) {
            if ((gen == 0 && !have_mv0) || (gen == 1 && !have_mv1)) { continue; }
            for (UINT spot = 0; spot < kProbeSpots; ++spot) {
                for (UINT y = 0; y < kProbeDim; ++y) {
                    for (UINT x = 0; x < kProbeDim; ++x) {
                        float vx = 0.0f, vy = 0.0f;
                        mv_at(gen, spot, x, y, &vx, &vy);
                        if (!(vx == vx) || !(vy == vy) || fabsf(vx) > 6.0e4f || fabsf(vy) > 6.0e4f) {
                            ++n_invalid;
                            continue;
                        }
                        if (fabsf(vx) > 1000.0f || fabsf(vy) > 1000.0f) { ++n_large; continue; }
                        sum_x += fabsf(vx);
                        sum_y += fabsf(vy);
                        ++n_valid;
                    }
                }
            }
        }
    }
    if (n_valid >= 256) {
        float raw_x = static_cast<float>(sum_x / n_valid);
        float raw_y = static_cast<float>(sum_y / n_valid);
        if (g_mv_scale_x != 0.0f) { raw_x = fabsf(raw_x / g_mv_scale_x); }
        if (g_mv_scale_y != 0.0f) { raw_y = fabsf(raw_y / g_mv_scale_y); }
        const float total = static_cast<float>(n_valid + n_invalid + n_large);
        const float big = (raw_x > raw_y) ? raw_x : raw_y;
        const char* unit = "ambiguous (scene nearly static? retry)";
        if (big > 0.5f) { unit = "pixels"; } else if (big < 0.02f) { unit = "UV"; }
        const mgpu::input::MvUnit declared = g_inputs.mv_unit;
        const char* verdict = "";
        if (declared != mgpu::input::MvUnit::unknown) {
            const bool declared_small = (declared != mgpu::input::MvUnit::pixels);
            const bool measured_small = (big < 0.5f);
            verdict = (declared_small == measured_small)
                          ? "  (agrees with the declaration)"
                          : "  (DISAGREES with the declaration - trust the measurement)";
        }
        logf_("probe: mv mean|v|=(%.3f,%.3f) invalid=%.1f%% large=%.1f%% -> unit=%s"
              "  magnitude=%.3f  declared=%s%s\n",
              raw_x, raw_y, 100.0f * n_invalid / total, 100.0f * n_large / total,
              unit, big, mgpu::input::mv_unit_name(declared), verdict);
        // Publish the verdict as one code: 1 = pixels, 2 = "small" (uv or ndc - the
        // magnitude test cannot tell those two apart, and only a declaration can).
        // A declaration always wins: the probe is a cross-check there, not a source of
        // truth. It only fills the field when the API said nothing.
        if (declared == mgpu::input::MvUnit::unknown) {
            uint32_t code = 0;
            if (big > 0.5f) { code = 1; } else if (big < 0.02f) { code = 2; }
            g_sem = (g_sem & ~(3u << mgpu::kSemMvUnitShift)) | (code << mgpu::kSemMvUnitShift);
            // Clear the "measured" flag when the magnitude class is inconclusive, otherwise the
            // consumer sees "unknown(measured)" - a stale bit from an earlier round.
            if (code != 0) { g_sem |= mgpu::kSemMvMeasured; }
            else           { g_sem &= ~mgpu::kSemMvMeasured; }
        }
    }

    // ---- 2. depth direction over the newest generation ----
    if (dp_map != nullptr) {
        const UINT gen = have_dep1 ? 1u : 0u;
        float mn = 1.0e30f, mx = -1.0e30f;
        double sum = 0.0;
        uint32_t n = 0, n_far0 = 0, n_near1 = 0, n_bad = 0;
        for (UINT spot = 0; spot < kProbeSpots; ++spot) {
            for (UINT y = 0; y < kProbeDim; ++y) {
                for (UINT x = 0; x < kProbeDim; ++x) {
                    const float v = dp_at(gen, spot, static_cast<int>(x), static_cast<int>(y));
                    if (!(v == v)) { ++n_bad; continue; }
                    if (v < mn) { mn = v; }
                    if (v > mx) { mx = v; }
                    sum += v;
                    ++n;
                    if (v <= 1.0e-6f) { ++n_far0; }
                    if (v >= 0.999f) { ++n_near1; }
                }
            }
        }
        if (n >= 256) {
            const float pz = 100.0f * static_cast<float>(n_far0) / static_cast<float>(n);
            const float po = 100.0f * static_cast<float>(n_near1) / static_cast<float>(n);
            int measured = 0;                 // +1 reversed, -1 forward, 0 unknown
            const char* what = "ambiguous (range spans the middle)";
            if (pz > 20.0f && po < 20.0f) {
                measured = 1;
                what = "reversed-Z (far plane is 0)";
            } else if (po > 20.0f && pz < 20.0f) {
                measured = -1;
                what = "forward-Z (far plane is 1)";
            } else if (mx < 0.5f) {
                measured = 1;
                what = "reversed-Z (tentative: nothing near 1)";
            } else if (mn > 0.5f) {
                measured = -1;
                what = "forward-Z (tentative: everything near 1)";
            }
            const bool declared_inv = (g_inputs.flags & mgpu::kInputFlagInvertedDepth) != 0;
            const char* agree = "";
            if (measured != 0) {
                agree = ((measured > 0) == declared_inv)
                            ? "  (agrees with the declaration)"
                            : "  (DISAGREES with the declaration - trust the measurement)";
            }
            logf_("probe: depth min=%.4f mean=%.4f max=%.4f far0=%.0f%% near1=%.0f%%"
                  " bad=%.1f%% (%u patches) -> %s  declared=%s%s\n",
                  mn, static_cast<float>(sum / n), mx, pz, po,
                  100.0f * static_cast<float>(n_bad) / static_cast<float>(n), kProbeSpots,
                  what, declared_inv ? "reversed" : "forward", agree);
            const uint32_t dc = (measured > 0) ? 1u : ((measured < 0) ? 2u : 0u);
            g_sem = (g_sem & ~(3u << mgpu::kSemDepthShift)) | (dc << mgpu::kSemDepthShift);
            if (dc != 0) { g_sem |= mgpu::kSemDepthMeasured; }
            else         { g_sem &= ~mgpu::kSemDepthMeasured; }
        }
    }

    // ---- 3. sign (paired win rate, and a second independent unit measurement) ----
    // Only a LAST RESORT now: every upscaler API defines its vectors as previous-minus-
    // current, and the sources publish that contract (in_ngx.cpp). The ±mv depth test is for
    // sources that declare nothing, so it stays off the hot path instead of gating the
    // pipeline on a fragile statistic (three rewrites of this test on 2026-11-10 is enough
    // evidence that it should never be the primary answer).
    if (g_inputs.mv_unit != mgpu::input::MvUnit::unknown) {
        static bool once = false;
        if (!once) {
            once = true;
            logf_("probe: sign skipped (mv unit declared=%s, sign is the API contract)\n",
                  mgpu::input::mv_unit_name(g_inputs.mv_unit));
        }
    } else if (mv_map != nullptr && dp_map != nullptr && have_mv0 && have_dep0 && have_dep1) {
        const float rw = (g_inputs.valid_w > 0) ? static_cast<float>(g_inputs.valid_w)
                                               : static_cast<float>(g_ds_w);
        const float rh = (g_inputs.valid_h > 0) ? static_cast<float>(g_inputs.valid_h)
                                               : static_cast<float>(g_ds_h);
        const float sw = static_cast<float>(g_ds_w);
        const float sh = static_cast<float>(g_ds_h);
        auto dp_bilin = [&](UINT gen, UINT spot, float fx, float fy) -> float {
            const float maxf = static_cast<float>(kProbeDim) - 1.001f;
            if (fx < 0.0f) { fx = 0.0f; }
            if (fy < 0.0f) { fy = 0.0f; }
            if (fx > maxf) { fx = maxf; }
            if (fy > maxf) { fy = maxf; }
            const int x0 = static_cast<int>(fx);
            const int y0 = static_cast<int>(fy);
            const float tx = fx - static_cast<float>(x0);
            const float ty = fy - static_cast<float>(y0);
            const float a = dp_at(gen, spot, x0, y0);
            const float b = dp_at(gen, spot, x0 + 1, y0);
            const float c = dp_at(gen, spot, x0, y0 + 1);
            const float d = dp_at(gen, spot, x0 + 1, y0 + 1);
            return (a + (b - a) * tx) * (1.0f - ty) + (c + (d - c) * tx) * ty;
        };

        struct Cand { const char* name; uint32_t unit_code; float kx, ky; };
        const Cand cands[3] = {
            { "pixels", 1u, sw / rw, sh / rh },
            { "uv",     2u, sw,      sh      },
            { "ndc",    3u, sw * 0.5f, sh * 0.5f },
        };

        int      best = -1;
        double   best_z = 0.0;           // |wins - n/2| / sqrt(n/4): ranking by a z-score
                                         // instead of the raw margin means a lucky 446-vote
                                         // subsample cannot outrank a solid 22k-vote one -
                                         // which is exactly what happened on 2026-11-10
                                         // 03:21 (uv/446 beat pixels/22206 on a 0.3% residual
                                         // difference that was pure noise).
        double   best_margin = 0.0;      // |win rate - 0.5|, reported for humans
        double   best_rate = 0.5;
        double   best_plus = 0.0, best_minus = 0.0;
        uint32_t best_used = 0, best_static = 0, best_big = 0, best_flat = 0, best_out = 0;

        for (int ci = 0; ci < 3; ++ci) {
            double sum_plus = 0.0, sum_minus = 0.0;
            uint32_t used = 0, wins = 0, skipped_static = 0, skipped_big = 0;
            uint32_t skipped_flat = 0, skipped_out = 0;
            for (UINT spot = 0; spot < kProbeSpots; ++spot) {
                // Stride 2: the field is spatially correlated, so every other texel carries
                // the same verdict for a quarter of the work. The probe runs on the render
                // thread inside the present callback, so cost matters (256^2 x 4 spots x 3
                // scales with full bilinear would be a visible hitch every 2 s).
                for (int y = 1; y + 1 < static_cast<int>(kProbeDim); y += 2) {
                    for (int x = 1; x + 1 < static_cast<int>(kProbeDim); x += 2) {
                        float mx2 = 0.0f, my2 = 0.0f;
                        mv_at(1, spot, static_cast<UINT>(x), static_cast<UINT>(y), &mx2, &my2);
                        if (!(mx2 == mx2) || !(my2 == my2)) { continue; }
                        const float sx = mx2 * cands[ci].kx;
                        const float sy = my2 * cands[ci].ky;
                        // A wrong scale blows the offsets up by orders of magnitude, and a
                        // correct one can still exceed what a patch can test; both land here
                        // so the log shows *why* a candidate had few votes.
                        if (fabsf(sx) > kProbeMaxOff || fabsf(sy) > kProbeMaxOff) {
                            ++skipped_big;
                            continue;
                        }
                        // Sub-quarter-texel motion cannot be separated from interpolation
                        // noise: both offsets land in the same texels.
                        if (fabsf(sx) < 0.25f && fabsf(sy) < 0.25f) { ++skipped_static; continue; }
                        const float fx = static_cast<float>(x);
                        const float fy = static_cast<float>(y);
                        // Both warped samples must come from inside the copied patch.
                        const float lim = static_cast<float>(kProbeDim) - 1.001f;
                        if (fx + sx < 0.0f || fx + sx > lim || fy + sy < 0.0f || fy + sy > lim ||
                            fx - sx < 0.0f || fx - sx > lim || fy - sy < 0.0f || fy - sy > lim) {
                            ++skipped_out;
                            continue;
                        }
                        const float cur = dp_bilin(1, spot, fx, fy);
                        // Skip the degenerate poles (cleared / sky): relative depth error is
                        // meaningless there and the vectors are usually invalid anyway. Both
                        // ends are excluded so the rule is direction-agnostic (reversed-Z puts
                        // sky at 0, forward-Z puts it at 1).
                        if (!(cur > 1.0e-5f) || !(cur < 0.99999f)) { continue; }
                        // Discriminating power: how much does the depth actually change when
                        // the sample moves by |offset|? On a flat wall it does not change at
                        // all, both signs fit equally well, and the "vote" is pure rounding -
                        // that is exactly what produced plus=0.00323 minus=0.00322 on
                        // 2026-11-10 03:21. Only texels where a wrong warp must show up can
                        // decide the sign.
                        const float gx = dp_at(1, spot, x + 2, y) - dp_at(1, spot, x - 2, y);
                        const float gy = dp_at(1, spot, x, y + 2) - dp_at(1, spot, x, y - 2);
                        const float slope = 0.25f * sqrtf(gx * gx + gy * gy);   // depth per texel
                        const float off = sqrtf(sx * sx + sy * sy);
                        if (!(slope * off > 0.02f * cur)) { ++skipped_flat; continue; }
                        const float a = dp_bilin(0, spot, fx + sx, fy + sy);
                        const float b = dp_bilin(0, spot, fx - sx, fy - sy);
                        if (!(a > 1.0e-5f) || !(a < 0.99999f)) { continue; }
                        if (!(b > 1.0e-5f) || !(b < 0.99999f)) { continue; }
                        const float e_plus  = fabsf(a - cur);
                        const float e_minus = fabsf(b - cur);
                        sum_plus  += e_plus;
                        sum_minus += e_minus;
                        if (e_plus < e_minus) { ++wins; }
                        ++used;
                    }
                }
            }
            // At least 2000 votes out of 4 x 126 x 126 = 63504 possible: below that the win
            // rate is too noisy to carry a verdict, however extreme it looks.
            if (used < 2000) { continue; }
            const double rate = static_cast<double>(wins) / static_cast<double>(used);
            const double margin = fabs(rate - 0.5);
            // z of a binomial vote count: (wins - n/2) / sqrt(n/4).
            const double z = fabs(static_cast<double>(wins) - 0.5 * used) /
                             sqrt(0.25 * static_cast<double>(used));
            if (z > best_z) {
                best = ci;
                best_z = z;
                best_margin = margin;
                best_rate = rate;
                best_plus = sum_plus / used;
                best_minus = sum_minus / used;
                best_used = used;
                best_static = skipped_static;
                best_big = skipped_big;
                best_flat = skipped_flat;
                best_out = skipped_out;
            }
        }

        if (best >= 0) {
            const bool  plus_better = (best_rate > 0.5);
            const float quality = static_cast<float>(2.0 * best_margin);   // 0..1
            // How much better the winning direction actually aligns. A vote majority that
            // moves the residual by 0.3% (03:21: plus=0.00323 minus=0.00322) is not a
            // direction, it is rounding - require a visible gap as well as a clear vote.
            const float gap = (best_plus > best_minus)
                                  ? static_cast<float>((best_plus - best_minus) / (best_plus + 1.0e-9))
                                  : static_cast<float>((best_minus - best_plus) / (best_minus + 1.0e-9));
            const bool  decided = (best_z > 5.0) && (gap > 0.02);
            const char* what = "unknown (win rate is a coin flip - vectors do not align)";
            if (decided) {
                what = plus_better ? "prev-cur (+mv aligns)"
                                   : "cur-prev (-mv aligns)  (game vectors are flipped)";
            } else if (best_z > 5.0) {
                what = "unknown (votes lean one way but both offsets fit equally well)";
            }
            logf_("probe: sign scale=%s used=%u static=%u flat=%u big=%u out=%u"
                  " wins=%.1f%% z=%.1f mean|d| plus=%.5f minus=%.5f -> %s  quality=%.2f\n",
                  cands[best].name, best_used, best_static, best_flat, best_big, best_out,
                  best_rate * 100.0,
                  static_cast<float>(best_z),
                  static_cast<float>(best_plus), static_cast<float>(best_minus), what,
                  quality);
            // Publish: the scale with the clearest win rate is the MV unit, the rate itself
            // is the sign, and the margin over a coin flip is the quality.
            const uint32_t best_unit = cands[best].unit_code;
            g_sem = (g_sem & ~(3u << mgpu::kSemMvUnitShift)) |
                    (best_unit << mgpu::kSemMvUnitShift);
            g_sem |= mgpu::kSemMvMeasured;
            g_sem = (g_sem & ~(0xFFu << mgpu::kSemMvQualityShift)) |
                    (static_cast<uint32_t>(quality * 255.0f) << mgpu::kSemMvQualityShift);
            if (decided) {
                g_sem |= mgpu::kSemQualityKnown;
                g_sem = (g_sem & ~(3u << mgpu::kSemMvSignShift)) |
                        ((plus_better ? 1u : 2u) << mgpu::kSemMvSignShift);
                g_sem |= mgpu::kSemSignMeasured;
                if (quality > 0.15f) { g_sem |= mgpu::kSemMvUsable; }
                else                 { g_sem &= ~mgpu::kSemMvUsable; }
            } else {
                g_sem &= ~(mgpu::kSemQualityKnown | (3u << mgpu::kSemMvSignShift) |
                           mgpu::kSemSignMeasured | mgpu::kSemMvUsable);
            }
        } else {
            // No candidate got 2000 votes: say so instead of staying silent, otherwise the
            // absence of a `probe: sign` line looks like the probe is broken.
            logf_("probe: sign unavailable (every scale had < 2000 usable texels - scene too"
                  " still, or the MV does not cover the sampled corners)\n");
        }
    }

     if (mv_map != nullptr) { g_probe_buf->Unmap(0, nullptr); }
    if (dp_map != nullptr) { g_probe_depth_buf->Unmap(0, nullptr); }
    g_probe_have = 0;
    g_probe_pending = 0;
    g_probe_gen = -1;
}

// Called from ds_run while the MV target is already in COPY_SOURCE.

static bool ds_run(ID3D12GraphicsCommandList* list, int which, ID3D12Resource* src,
                   ID3D12Resource* dst, bool defer_copy = false)
{
    if (!g_ds_ready || !src || !dst || which < 0 || which > 1) { return false; }

    const uint64_t key = reinterpret_cast<uint64_t>(src);
    // Crop the sample to the *valid* sub-rect of the source. Some titles allocate the
    // stream texture a few pixels larger than the render size and keep the image in the
    // top-left corner: PBZ's depth is 2564x1444 while the render size (and the rect SL
    // declares as the tag extent) is 2562x1441. Sampling [0,1] of the whole texture
    // stretches the field by tex/valid and shifts it towards the edges.
    // Conservative by design: only a small excess with the content at (0,0) is corrected;
    // anything else (dynamic resolution sub-rects, padded rows) keeps the old behaviour
    // and says so once.
    float uv_max_x = 1.0f, uv_max_y = 1.0f;
    {
        const D3D12_RESOURCE_DESC sdesc = src->GetDesc();
        const UINT tw = static_cast<UINT>(sdesc.Width);
        const UINT th = sdesc.Height;
        const UINT vw = (g_inputs.render_w > 0 && g_inputs.render_w <= tw) ? g_inputs.render_w : tw;
        const UINT vh = (g_inputs.render_h > 0 && g_inputs.render_h <= th) ? g_inputs.render_h : th;
        if ((vw != tw || vh != th) && (tw - vw) <= 16 && (th - vh) <= 16) {
            uv_max_x = static_cast<float>(vw) / static_cast<float>(tw);
            uv_max_y = static_cast<float>(vh) / static_cast<float>(th);
        }
        // Log the geometry once per *change*. Keyed on the sizes only: the game rotates
        // several depth targets between frames (PBZ alternates two), so including the
        // resource pointer here logged the same line every frame (1150 lines in 3 min).
        static uint32_t logged_dims[2][4] = {};   // per stream: vw, vh, tw, th
        static bool     logged_once[2] = { false, false };
        if (!logged_once[which] || logged_dims[which][0] != vw || logged_dims[which][1] != vh ||
            logged_dims[which][2] != tw || logged_dims[which][3] != th) {
            logged_once[which] = true;
            logged_dims[which][0] = vw;
            logged_dims[which][1] = vh;
            logged_dims[which][2] = tw;
            logged_dims[which][3] = th;
            const char* nm = (which == 0) ? "mv" : "depth";
            if (uv_max_x < 1.0f || uv_max_y < 1.0f) {
                logf_("\ncrop: %s tex %ux%u -> valid %ux%u (uvMax=%.5f,%.5f)\n",
                      nm, tw, th, vw, vh, uv_max_x, uv_max_y);
            } else if (vw != tw || vh != th) {
                // Not a failure: the two sizes differ by more than a padded border, so this is
                // a *different-resolution* input (XeSS hands us 3840x2160 HIGH_RES_MV for a
                // 2562x1441 render). uvMax stays 1.0, the pass samples the whole texture and
                // resamples it onto the render grid - which is exactly what "FG wants
                // render-size MVs" means in OptiScaler's ForceFGRenderSizeMVs quirk
                // (docs/findings/OPTISCALER_QUIRKS.md 3.2). The old wording here ("excess too
                // large") read like we were giving up.
                logf_("\ncrop: %s tex %ux%u vs render %ux%u - resampling the whole texture\n",
                      nm, tw, th, vw, vh);
            }
        }
    }

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
                         (which == 0) ? g_mv_scale_y : 1.0f,
                         uv_max_x, uv_max_y };
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

    // The target is in COPY_SOURCE right here: cheapest possible place for the probe copy
    // (see probe_issue). Both streams are sampled - MV for the unit, depth for the depth
    // direction the APIs disagree about.
    probe_issue(list, which, static_cast<uint32_t>(g_frame));

    if (defer_copy) {
        // OffloadQueue: the copy and the reverse transition happen on our own queue (see
        // offload_submit). Leaving the resource in COPY_SOURCE here is what makes the copy
        // legal on another queue - and the next frame waits for our restore before this pass
        // renders into it again (offload_guard).
        g_off_stream[which + 1] = true;
        g_off_deferred[which + 1] = true;
        return true;
    }

    copy_into_slot(list, g_ds_rt[which].Get(), dst);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &b);
    return true;
}

// Brings g_ds_rt[which] back to RENDER_TARGET on the *game's* list. Only used when an
// offloaded copy could not be submitted (offload_guard timed out / the switch was turned off
// mid-run): without it the next pass would render into a COPY_SOURCE resource.
static void ds_restore_on_game_list(ID3D12GraphicsCommandList* list, int which)
{
    if (!list || !g_ds_rt[which]) { return; }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = g_ds_rt[which].Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &b);
}

// Records the deferred copies + the reverse transitions into g_off_list. Returns false when
// there was nothing to record (no stream was deferred).
static bool offload_same_device(ID3D12Resource* res)
{
    if (!res || !g_off_queue || !g_off_dev) { return true; }
    // The queue, the allocator and everything they touch must belong to ONE device. With two
    // GPUs in the process (the whole point of this project) mixing them is a device removal,
    // and the crash surfaces far from the copy that caused it. OptiScaler carries a quirk for
    // exactly this class of bug (CreateSLOnThe2ndDevice / ForceCreateD3D12Device,
    // docs/findings/OPTISCALER_QUIRKS.md 2.2).
    ID3D12Device* dev = nullptr;
    if (FAILED(res->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dev))) || !dev) {
        return true;               // cannot tell - do not invent a failure
    }
    const bool same = (dev == g_off_dev);
    dev->Release();
    if (!same) {
        offload_fail("a resource lives on a different D3D12 device than our queue");
    }
    return same;
}

static bool offload_record_copies(UINT slot)
{
    if (!g_off_deferred[1] && !g_off_deferred[2]) { return false; }
    g_off_alloc->Reset();
    g_off_list->Reset(g_off_alloc, nullptr);
    bool any = false;
    for (int which = 0; which < 2; ++which) {
        const int stream = which + 1;
        if (!g_off_deferred[stream]) { continue; }
        // The copy is skipped when the target is gone, but the state is always restored -
        // an unresolved COPY_SOURCE is what would break the next frame's pass.
        if (g_ds_rt[which] && g_slot[stream][slot]) {
            if (!offload_same_device(g_ds_rt[which].Get()) ||
                !offload_same_device(g_slot[stream][slot].Get())) {
                // offload_same_device() already turned the queue off. Close what was recorded
                // and return: the caller then rescues these copies on the game's queue
                // (offload_rescue), which also restores the COPY_SOURCE state - so the
                // deferred flags must stay set.
                g_off_list->Close();
                return false;
            }
            copy_into_slot(g_off_list, g_ds_rt[which].Get(), g_slot[stream][slot].Get());
            any = true;
        }
        if (g_ds_rt[which]) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.pResource = g_ds_rt[which].Get();
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_off_list->ResourceBarrier(1, &b);
        }
    }
    g_off_list->Close();
    g_off_deferred[1] = g_off_deferred[2] = false;
    if (any) { ++g_off_frames; }
    return true;
}

// Runs the deferred copies on our own queue and signals the consumer's cross-adapter fence
// from *there*, so the signal a consumer waits on now comes from the queue that actually
// wrote the slot. Two fences are involved:
//   g_off_ready  (local)     : the game's queue signals it after the list holding the pass
//   our queue    -> Wait(ready) -> copies + restore -> Signal(g_off_fence) -> Signal(g_fwd)
// Returns true when the frame's MV/depth were handed over; the caller must then NOT signal
// g_fwd[slot] from the game's queue itself.
static bool offload_submit(UINT slot, UINT64 fwd_value)
{
    if (!g_offload || !g_off_queue || !g_off_list || !g_off_fence || !g_off_ready) { return false; }
    if (!g_game_queue || !g_fwd[slot]) { return false; }

    // Our work must be ordered after the game's list (the pass) and before the consumer is
    // told the slot is ready; both are queue operations, not list ones.
    const UINT64 ready = ++g_off_ready_value;
    if (FAILED(g_game_queue->Signal(g_off_ready, ready))) { return false; }
    if (!offload_record_copies(slot)) { return false; }

    g_off_queue->Wait(g_off_ready, ready);
    ID3D12CommandList* lists[] = { g_off_list };
    g_off_queue->ExecuteCommandLists(1, lists);
    g_off_pending = ++g_off_value;
    g_off_queue->Signal(g_off_fence, g_off_pending);
    g_off_queue->Signal(g_fwd[slot].Get(), fwd_value);
    g_off_stream[1] = g_off_stream[2] = false;
    return true;
}

// Last resort: the deferred streams could not be handed to our queue, so run the same list on
// the *game's* queue (it is still ordered after the pass) and signal our own fence, so the
// guard in the next frame's begin_effects sees a complete state again. The caller then
// signals the consumer's fence as usual.
static void offload_rescue(UINT slot)
{
    if (!g_game_queue || !g_off_list) { g_off_stream[1] = g_off_stream[2] = false; return; }
    if (!offload_record_copies(slot)) { g_off_stream[1] = g_off_stream[2] = false; return; }
    ID3D12CommandList* lists[] = { g_off_list };
    g_game_queue->ExecuteCommandLists(1, lists);
    if (g_off_fence) {
        g_off_pending = ++g_off_value;
        g_game_queue->Signal(g_off_fence, g_off_pending);
    }
    g_off_stream[1] = g_off_stream[2] = false;
}

// Resolves mgpu_depth_tap.fx's output texture. ReShade only has it once the effect is
// loaded and the preset enabled it, so this retries until it shows up (a few frames).
static void native_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
                           resource_usage before, resource_usage after);   // defined later
static int g_rt_depth_tries = 0;
// Same-frame depth: copy the game's depth into our typeless texture, then run the existing
// pass on it (converts to R32 and scales to the MV size in one go).
static bool record_depth_convert(ID3D12GraphicsCommandList* list, ID3D12Resource* src,
                                 ID3D12Resource* dst, bool defer_copy = false)
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

    const bool ok = ds_run(list, 1, g_depth_tl.Get(), dst, defer_copy);

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
    {
        UINT w = mv_w, h = mv_h;
        stream_target_from(mv_w, mv_h, &w, &h);
        if ((w != mv_w || h != mv_h) && ds_prepare(device, w, h)) {
            mv_w = w;
            mv_h = h;
            mv_src = (g_stream_height > 0 && h >= static_cast<UINT>(g_stream_height))
                         ? "height cap" : "downsampled";
        } else if (w != mv_w || h != mv_h) {
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
    //
    // Which conversion to apply is DECLARED, not measured: NGX/FFX hand us MV.Scale, and
    // `pixels = stored * MV.Scale` means the unit is known (see in_ngx.cpp). MvNormalize=auto
    // (the default) therefore follows the source; 0/1/2 stay as explicit overrides for a
    // title that lies about its scale.
    int norm = g_mv_normalize_cfg;
    if (norm < 0) {
        switch (g_inputs.mv_unit) {
        case mgpu::input::MvUnit::pixels: norm = 1; break;   // pixels -> UV
        case mgpu::input::MvUnit::ndc:    norm = 3; break;   // NDC -> UV (halve)
        default:                          norm = 0; break;   // uv, or undeclared: as-is
        }
        logf_("mv unit: declared=%s -> %s\n", mgpu::input::mv_unit_name(g_inputs.mv_unit),
              norm == 1 ? "divide by stream size (pixels -> uv)"
                        : (norm == 3 ? "halve (ndc -> uv)" : "pass through (uv / undeclared)"));
    }
    g_mv_scale_x = 1.0f;
    g_mv_scale_y = 1.0f;
    if (norm == 1 && mv_w > 0 && mv_h > 0) {
        g_mv_scale_x = 1.0f / static_cast<float>(mv_w);
        g_mv_scale_y = 1.0f / static_cast<float>(mv_h);
    } else if (norm == 2 && g_rt_buffer_w > 0 && g_rt_buffer_h > 0) {
        g_mv_scale_x = 1.0f / static_cast<float>(g_rt_buffer_w);
        g_mv_scale_y = 1.0f / static_cast<float>(g_rt_buffer_h);
    } else if (norm == 3) {
        g_mv_scale_x = 0.5f;
        g_mv_scale_y = 0.5f;
    }
    if (g_mv_flip_cfg) {
        g_mv_scale_x = -g_mv_scale_x;
        g_mv_scale_y = -g_mv_scale_y;
    }
    g_mv_pass = (norm != 0 || g_mv_flip_cfg != 0);
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
    PerfAccum acc(&g_perf_barrier_ms);
    ++g_perf_barrier_calls;
    g_perf_barrier_items += count;
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
                              ID3D12Resource* src, ID3D12Resource* dst, int which,
                              bool defer_copy = false)
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
    const bool ok = ds_run(native_list, which, src, dst, defer_copy);
    // The game's own resource is restored here in every mode: our second queue only touches
    // g_ds_rt (our render target) and the shared slot, so the game's state must stay correct
    // on its own list. Only the *copy* + g_ds_rt's reverse transition are deferred.
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
          "  q=%u/%u ov=%u waits=%u",
          g_frames_seen, g_frames_copied, g_frames_skipped,
          avg, (unsigned long long)g_frame, queue_depth(), mgpu::kMaxSlots, overrun, waits);
    // OffloadQueue counters only appear once the switch (or a fallback) has been seen.
    if (g_offload || g_off_frames || g_off_failed) {
        logf_("  offload=%u stalls=%u fail=%u", g_off_frames, g_off_stalls, g_off_failed);
    }
    logf_("\n");
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
    g_skip_until_ms     = now_ms() + 8000.0;   // ... but never longer than 8 s of wall time
    logf_("\ninit_swapchain: %p resize=%d - pausing capture until frame %llu (max 8 s)\n",
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
    resolve_ini_path(path, sizeof(path));

    char value[32]{};
    if (GetPrivateProfileStringA("capture", "Color", "1", value, sizeof(value), path) > 0) {
        g_capture_colour = (atoi(value) != 0);
    }

    g_render_w_ini = static_cast<UINT>(GetPrivateProfileIntA("capture", "RenderWidth", 0, path));
    g_render_h_ini = static_cast<UINT>(GetPrivateProfileIntA("capture", "RenderHeight", 0, path));

    // StreamPercent > 100 renders MV/depth smaller before the cross-adapter copy:
    // 150 sends them at render * 100/150 (2160p -> 1440p, 2.25x less traffic).
    g_stream_percent = GetPrivateProfileIntA("capture", "StreamPercent", 100, path);
    g_stream_height  = GetPrivateProfileIntA("capture", "StreamHeight", 1440, path);
    if (g_stream_height < 0)    { g_stream_height = 0; }
    if (g_stream_height > 4320) { g_stream_height = 4320; }
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

    // OffloadQueue=1 runs the MV/depth *cross-adapter copies* on a command queue of our own
    // (created on the game's device) instead of inside the game's command list, so the
    // 28-63 MB/frame write no longer sits between the game's last draw and its Present().
    // The conversion passes stay on the game's list; only our own render target and the
    // shared slot move. Default 0: it is the one change whose failure mode is a device reset.
    g_offload = GetPrivateProfileIntA("capture", "OffloadQueue", 0, path) != 0;

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
    // MvNormalize: 0 (default) = pass the vectors through untouched, -1/auto = derive the
    // conversion from the declared MV unit, 1 = divide by the render size (pixels -> UV),
    // 2 = divide by the output size. auto is opt-in on purpose: it changes what the stream
    // carries, so it must be a deliberate act, not a silent default.
    {
        char buf[16] = { 0 };
        GetPrivateProfileStringA("capture", "MvNormalize", "0", buf, sizeof(buf), path);
        if (_stricmp(buf, "auto") == 0) { g_mv_normalize_cfg = -1; }
        else { g_mv_normalize_cfg = atoi(buf); }
    }

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

    // [capture] LoaderHook=0: do not patch kernel32!LoadLibrary*. The sources are then only
    // found by rescanning the module list every 120 frames - slower to notice, but it keeps
    // us out of the game's loader, which is the prime suspect for RE Engine's slow start-up.
    g_loader_hook = GetPrivateProfileIntA("capture", "LoaderHook", 1, path) != 0;

    // [capture] StreamlineCamera=0: do not take the projection from the Streamline constants the
    // diagnostic sniffer sees. Default 1 - DLSS-SR exports no camera at all, so without this
    // every NGX title gets the --fg-fov guess. It changes nothing for sources that declare one.
    g_fg_cam_ok = GetPrivateProfileIntA("capture", "StreamlineCamera", 1, path) != 0;

    logf_("config: Color=%d RenderWidth=%u RenderHeight=%u StreamPercent=%d StreamHeight=%d"
          " SkipMv=%d DepthConvert=%d MvNormalize=%d MvFlip=%d LoaderHook=%d FgSniff=%d"
          " Input=%s (path %s)\n",
          g_capture_colour ? 1 : 0, g_render_w_ini, g_render_h_ini, g_stream_percent,
          g_stream_height,
          g_skip_mv ? 1 : 0,
          g_depth_convert_cfg, g_mv_normalize_cfg, g_mv_flip_cfg,
          g_loader_hook ? 1 : 0,
          GetPrivateProfileIntA("capture", "FgSniff", 0, path) != 0 ? 1 : 0,
          g_force_input.c_str(), path);
}

// --------------------------------------------------------- Streamline camera (v12)
// in_fg.cpp sees the game's sl::Constants (jitter-free view-to-clip). It is a *diagnostic*
// source - never polled, never pinned - so it hands the camera over through these instead of
// pretending to be an input. The sequence counter makes the hand-off tear-free; the timestamp
// lets the frame path ignore a camera left over from an API the game has since abandoned.
// (The storage lives next to the other globals, because the ini reader needs g_fg_cam_ok.)

void mgpu::input::publish_fg_camera(const float view_to_clip[16], float near_z, float far_z,
                                    float vfov_rad, uint64_t tick_ms)
{
    if (!view_to_clip) { return; }
    FgCamSlot* s = fg_cam_slot();
    InterlockedIncrement64(&s->seq);
    memcpy(s->m, view_to_clip, sizeof(s->m));
    s->near_z = near_z;
    s->far_z  = far_z;
    s->vfov   = vfov_rad;
    // The timestamp doubles as "there is data": stored last, released by the interlocked
    // increment below (full barrier on x86/x64).
    s->tick = tick_ms;
    InterlockedIncrement64(&s->seq);
}

bool mgpu::input::fg_camera(float out16[16], float* near_z, float* far_z, float* vfov_rad,
                            uint64_t* tick_ms)
{
    if (!out16) { return false; }
    FgCamSlot* s = fg_cam_slot();
    for (int tries = 0; tries < 4; ++tries) {
        const LONG64 s0 = InterlockedCompareExchange64(&s->seq, 0, 0);
        if ((s0 & 1) != 0) { continue; }
        float tmp[16];
        memcpy(tmp, s->m, sizeof(tmp));
        const float nz = s->near_z, fz = s->far_z, vf = s->vfov;
        const uint64_t tk = s->tick;
        if (InterlockedCompareExchange64(&s->seq, 0, 0) != s0) { continue; }
        memcpy(out16, tmp, sizeof(tmp));
        if (near_z)   { *near_z = nz; }
        if (far_z)    { *far_z = fz; }
        if (vfov_rad) { *vfov_rad = vf; }
        if (tick_ms)  { *tick_ms = tk; }
        return tk != 0;
    }
    return false;
}

// Is this a projection matrix at all? The same shape test OptiScaler uses before trusting a
// captured matrix (inputs/FG/XeFG_Inputs_Dx12.cpp:126 IsProjection): w is +-z, no w
// translation, no x/y mixing. A turned-camera matrix fails the last two, a garbage buffer
// fails the first.
static bool looks_like_projection(const float m[16])
{
    for (int i = 0; i < 16; ++i) { if (!(m[i] == m[i])) { return false; } }
    if (fabsf(m[15]) > 1.0e-2f) { return false; }
    if (fabsf(m[1]) > 1.0e-3f || fabsf(m[4]) > 1.0e-3f) { return false; }
    const float a = fabsf(m[11]), b = fabsf(m[14]);
    return (fabsf(a - 1.0f) < 1.0e-3f) || (fabsf(b - 1.0f) < 1.0e-3f);
}

// Once per published frame: is there a fresh real projection to use?
// Deliberately does NOT touch g_inputs.flags: if the sniffer goes quiet (the game switched to
// another API) a flag left behind would freeze the projection forever. The caller publishes a
// timestamp instead, so the consumer sees the matrix go stale by itself.
static bool current_fg_camera(float out16[16], uint64_t* out_tick)
{
    g_fg_cam_live = false;
    // Diagnostics: say *why* the matrix is not used, once per distinct reason, plus a throttled
    // state line - "the camera line never appears" is indistinguishable from "the game has no
    // camera" without it, and that difference decides whether there is a bug here.
    auto why = [](const char* msg) {
        static const char* last = nullptr;
        if (last == msg) { return; }
        last = msg;
        logf_("camera: not used (%s)\n", msg);
    };
    float m[16];
    float nz = 0.0f, fz = 0.0f, vf = 0.0f;
    uint64_t tk = 0;
    const bool src_has_cam = (g_inputs.flags & mgpu::kInputFlagHasCamera) != 0;
    const bool fg_ok = mgpu::input::fg_camera(m, &nz, &fz, &vf, &tk);
    const uint64_t now = GetTickCount64();
    const bool fresh = fg_ok && tk != 0 && now >= tk && (now - tk) <= 250ull;
    const bool shape = fg_ok && looks_like_projection(m);
    const bool ok = g_fg_cam_ok && !src_has_cam && fresh && shape;

    {
        static uint64_t last_log = 0;
        if (now - last_log >= 5000ull) {
            last_log = now;
            logf_("camera: state enabled=%d src_camera=%d read=%d tick=%llu age=%lldms"
                  " shape=%d raw_tick=%llu seq=%lld -> %s\n",
                  g_fg_cam_ok ? 1 : 0, src_has_cam ? 1 : 0, fg_ok ? 1 : 0,
                  static_cast<unsigned long long>(tk),
                  fg_ok ? static_cast<long long>(now - tk) : -1LL,
                  shape ? 1 : 0,
                  static_cast<unsigned long long>(fg_cam_slot()->tick),
                  static_cast<long long>(fg_cam_slot()->seq),
                  ok ? "USED" : "not used");
        }
    }

    if (!g_fg_cam_ok) { why("[capture] StreamlineCamera=0"); return false; }
    if (src_has_cam) {
        why("the input source declares its own camera");
        return false;   // the source wins
    }
    if (!fg_ok) {
        why("no matrix published yet (no sl::Constants and no XeFG frame constants)");
        return false;
    }
    if (!fresh) {
        why("the published matrix is stale (older than 250 ms)");
        return false;
    }
    if (!shape) {
        why("the published matrix does not look like a projection (rejected)");
        return false;
    }
    memcpy(out16, m, 16 * sizeof(float));
    if (out_tick) { *out_tick = tk; }
    g_fg_cam_live = true;
    if (!g_fg_cam_used) {
        g_fg_cam_used = true;
        // Derive what we will actually hand over: XeFG's frame constants carry only the matrix
        // (near/far/fov arrive as 0), so printing those fields showed "fovY=0.00deg" while the
        // bridge correctly used 60.00 deg.
        const float fy = (fabsf(m[5]) > 1.0e-6f) ? (2.0f * atanf(1.0f / fabsf(m[5]))) : 0.0f;
        const float asp = (fabsf(m[5]) > 1.0e-6f && fabsf(m[0]) > 1.0e-6f)
                              ? (fabsf(m[5]) / fabsf(m[0])) : 0.0f;
        logf_("camera: using the game's own projection matrix (Streamline constants / XeFG"
              " frame constants)\n"
              "  rows [%+.4f %+.4f %+.4f %+.4f] [%+.4f %+.4f %+.4f %+.4f]\n"
              "       [%+.4f %+.4f %+.4f %+.4f] [%+.4f %+.4f %+.4f %+.4f]\n"
              "  fovY=%.2fdeg aspect=%.3f - --fg-fov is no longer used for this title\n",
              m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
              m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15],
              fy * 57.2957795f, asp);
    }
    return true;
}

// One line per change of the *declared* semantics. This is what makes "every game is
// different" visible instead of guessable: the source states what its API knows,
// `unknown` states what it does not, and `probe:` fills the gap at runtime.
static void log_semantics()
{
    static uint32_t logged_bits = 0xFFFFFFFFu;
    static uint32_t logged_dims = 0xFFFFFFFFu;
    const mgpu::input::FrameInputs& f = g_inputs;
    const uint32_t bits = static_cast<uint32_t>(f.mv_unit) |
                          (static_cast<uint32_t>(f.mv_sign) << 4) |
                          (static_cast<uint32_t>(f.camera_source) << 8) |
                          ((f.flags & mgpu::kInputFlagInvertedDepth) ? (1u << 12) : 0u) |
                          ((f.depth == nullptr) ? (1u << 13) : 0u);
    const uint32_t dims = (f.valid_w << 16) ^ f.valid_h;
    if (bits == logged_bits && dims == logged_dims) { return; }
    logged_bits = bits;
    logged_dims = dims;
    // No depth texture at all (PBZ's XeSS: "HIGH_RES_MV: no depth") is not "forward-Z" - the
    // first version printed depth=forward there, which reads like a declaration that the
    // buffer is the wrong way round. Say what is true instead.
    const char* dep = (f.depth == nullptr)
                          ? "n/a (no depth)"
                          : ((f.flags & mgpu::kInputFlagInvertedDepth) ? "reversed" : "forward");
    logf_("semantics: mv=%s sign=%s camera=%s depth=%s valid=%ux%u\n",
          mgpu::input::mv_unit_name(f.mv_unit),
          mgpu::input::mv_sign_name(f.mv_sign),
          mgpu::input::camera_source_name(f.camera_source),
          dep, f.valid_w, f.valid_h);
}

// Asks the sources for this frame's inputs. The first source that has data wins and
// is pinned for the rest of the process: switching mid-run would change the stream
// sizes (the shared block is sized once) and the meaning of the depth buffer.
static bool fetch_inputs()
{
    const bool forced = !g_force_input.empty() && g_force_input != "auto";

    if (!g_source) {
        for (auto* s : g_sources) {
            // Diagnostic-only sources (in_fg.cpp) are not inputs: never polled, never
            // pinned, and never listed by the "waiting for ..." hints below.
            if (!s->is_input()) { continue; }
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
            log_semantics();
            return true;
        }
        if ((g_frames_seen % 120) == 1) {
            char list[128]{};
            for (auto* s : g_sources) {
                if (!s->is_input()) { continue; }
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
                if (!s->is_input()) { continue; }
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
    log_semantics();
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
    PerfAccum acc(&g_perf_begin_ms);
    ++g_frames_seen;
    // Module discovery happens *here*, not in AddonInit: the initial scan walks the module
    // list with CreateToolhelp32Snapshot and installs hooks, and doing that from ReShade's
    // add-on init (inside the device-creation call, with the loader lock contended) wedged
    // Wuchang: Fallen Feathers on 2026-11-10. One frame of delay costs nothing.
    if (g_scan_pending) {
        g_scan_pending = false;
        scan_loaded_modules();
    }
    drain_pending_modules();
    if (!g_loader_hook && (g_frames_seen % 120) == 0) {
        // No loader hook (see [capture] LoaderHook): pick up modules that appeared after
        // init, so NGX/FFX/XeSS are still found - just up to 120 frames later.
        scan_loaded_modules();
    }
    if (!fetch_inputs()) { return; }

    // Paused after a swap-chain (re)create: whichever comes first, the frame count or the
    // wall-clock cap (see g_skip_until_ms - a slow-loading title presents far too few
    // frames per second for a pure frame count to mean "a couple of seconds").
    if (g_frame < g_skip_capture_until && now_ms() < g_skip_until_ms) { return; }
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

    // [capture] OffloadQueue: the previous frame's copies must have finished (and restored
    // g_ds_rt) before the passes below render into it again. Bounded CPU wait, normally
    // already satisfied; on timeout the state is re-established on the game's list.
    bool offload_ok = false;
    if (g_offload) {
        if (!offload_ensure(game_device)) {
            // offload_fail() already logged and cleared g_offload
        } else if (offload_guard()) {
            offload_ok = true;
            g_off_deferred[1] = g_off_deferred[2] = false;   // our queue restored them
        } else {
            // Could not confirm the restore: put the *deferred* targets back on the game's
            // list so this frame's passes are legal, and stay in-frame for this frame.
            for (int which = 0; which < 2; ++which) {
                if (!g_off_deferred[which + 1]) { continue; }
                ds_restore_on_game_list(native_list, which);
                g_off_deferred[which + 1] = false;
            }
        }
    }
    g_off_stream[1] = g_off_stream[2] = false;

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
        // The unit probe measures THIS stream, so it cannot run either - say so once
        // instead of leaving the missing `probe:` line a mystery.
        static bool hinted = false;
        if (g_probe_ready && !hinted && g_frames_seen > 120 && g_inputs.mv != nullptr) {
            hinted = true;
            logf_("  [mv] the consumer did not ask for motion vectors, so `probe:` stays"
                  " silent - enable MV in the bridge/UI to have the unit measured\n");
        }
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
            mv_ok = record_downsample(cmd, native_list, g_inputs.mv, dst, 0, offload_ok);
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
            depth_ok = record_depth_convert(native_list, g_inputs.depth, dst, offload_ok);
        } else if (shadow) {
            // ReShade's own R32F depth output, one frame old and in a state we know.
            depth_ok = record_shadow_depth_copy(native_list, shadow, dst,
                                                resource_usage::render_target);
        } else if (g_ds_on) {
            // Depth is rendered into the R32 stream: converts D32_S8X24 (UE5) to R32 and
            // rescales to the MV size in the same pass, so no crop and no format guessing.
            depth_ok = record_downsample(cmd, native_list, g_inputs.depth, dst, 1, offload_ok);
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
    PerfAccum acc(&g_perf_present_ms);
    // Consume a finished MV probe (a 16 KB staging read, never blocks).
    probe_report();
    // Every 120 presents, print what our own callbacks cost in the window. This is the
    // question "is the add-on what makes this game load slowly?" answered with numbers.
    if (g_frames_seen != 0 && (g_frames_seen <= 5 || (g_frames_seen % 120) == 0)) {
        logf_("perf: frames=%llu begin=%.1fms barrier=%.1fms (%llu calls / %llu items)"
              " present=%.1fms\n",
              (unsigned long long)g_frames_seen, g_perf_begin_ms, g_perf_barrier_ms,
              (unsigned long long)g_perf_barrier_calls,
              (unsigned long long)g_perf_barrier_items, g_perf_present_ms);
        g_perf_barrier_ms = 0.0;
        g_perf_begin_ms   = 0.0;
        g_perf_present_ms = 0.0;
        g_perf_barrier_calls = 0;
        g_perf_barrier_items = 0;
    }
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
            // v12: a real projection (Streamline constants) if this source has none of its own.
            float cam_mtx[16];
            uint64_t cam_tick = 0;
            const bool have_cam_mtx = current_fg_camera(cam_mtx, &cam_tick);
            ctl->frame_published         = g_frame;
            ctl->frames[slot].frame_id   = g_inputs.serial;
            ctl->frames[slot].jitter_x   = g_inputs.jitter_x;
            ctl->frames[slot].jitter_y   = g_inputs.jitter_y;
            ctl->frames[slot].render_w   = g_inputs.render_w;
            ctl->frames[slot].render_h   = g_inputs.render_h;
            // The published scale must match what the STREAM holds, not just what the game
            // declared. XeFG's motionVectorScale means "pixels per velocity unit" (OptiScaler
            // derives it the same way: 1.0 for a pixel-space source, size*0.5 with a negated Y
            // for NDC - inputs/FG/XeFG_Inputs_Dx12.cpp:514). With MvNormalize the stream is
            // rewritten (pixels/mv_w, or ndc*0.5), so the declared scale alone would be ~2560x
            // off - a silent quality bug, so it is folded in here.
            float pub_mv_x = g_inputs.mv_scale_x;
            float pub_mv_y = g_inputs.mv_scale_y;
            if (g_mv_normalize_cfg == 1 || (g_mv_normalize_cfg < 0 && g_inputs.mv_unit == mgpu::input::MvUnit::pixels)) {
                pub_mv_x *= static_cast<float>(g_mv_w ? g_mv_w : 1);
                pub_mv_y *= static_cast<float>(g_mv_h ? g_mv_h : 1);
            } else if (g_mv_normalize_cfg == 3 || (g_mv_normalize_cfg < 0 && g_inputs.mv_unit == mgpu::input::MvUnit::ndc)) {
                // NDC: pixels = ndc * size/2, and Y flips (NDC +Y is up, screen/UV +Y is down).
                // OptiScaler's XeFG path does exactly this: mvScaleY *= size.y * -0.5f
                // (inputs/FG/XeFG_Inputs_Dx12.cpp:521). The matching USE_NDC_VELOCITY init flag
                // is not wired up in the bridge yet - followed up in OPTISCALER_QUIRKS.md.
                pub_mv_x *= 2.0f;
                pub_mv_y *= -2.0f;
            }
            if (g_mv_flip_cfg) { pub_mv_x = -pub_mv_x; pub_mv_y = -pub_mv_y; }
            ctl->frames[slot].mv_scale_x = pub_mv_x;
            ctl->frames[slot].mv_scale_y = pub_mv_y;
            // v6: camera comes from the source when it has one (FSR 3.1 exports
            // near/far/vfov; DLSS-SR does not, so those stay 0 and the bridge keeps
            // using --fg-fov). The matching flag lives in ctl->producer.input_flags.
            ctl->frames[slot].near_plane    = g_inputs.near_plane;
            ctl->frames[slot].far_plane     = g_inputs.far_plane;
            ctl->frames[slot].vfov_rad      = g_inputs.vfov_rad;
            ctl->frames[slot].frame_time_ms = g_present_interval_ms;
            // v11: what the probe measured (0 = nothing measured yet). The consumer logs it
            // and - once it trusts it - uses it instead of the per-game switches.
            //
            // Declarations are published too, because they are the primary source: the unit
            // comes from MV.Scale / the XeSS velocity-space flag, the sign from the API
            // contract, the depth direction from the API's own flag. The probe only fills a
            // gap (it is skipped entirely when the unit is declared), so without this the
            // consumer would see "mv=unknown" for a source that declared everything.
            {
                uint32_t sem_out = g_sem;
                if ((sem_out & mgpu::kSemMvMeasured) == 0) {
                    uint32_t u = 0;
                    switch (g_inputs.mv_unit) {
                    case mgpu::input::MvUnit::pixels: u = 1; break;
                    case mgpu::input::MvUnit::uv:     u = 2; break;
                    case mgpu::input::MvUnit::ndc:    u = 3; break;
                    default: break;
                    }
                    sem_out = (sem_out & ~(3u << mgpu::kSemMvUnitShift)) |
                              (u << mgpu::kSemMvUnitShift);
                }
                if ((sem_out & mgpu::kSemSignMeasured) == 0 &&
                    g_inputs.mv_sign != mgpu::input::MvSign::unknown) {
                    sem_out = (sem_out & ~(3u << mgpu::kSemMvSignShift)) |
                              ((g_inputs.mv_sign == mgpu::input::MvSign::prev_minus_cur ? 1u : 2u)
                               << mgpu::kSemMvSignShift);
                }
                if ((sem_out & mgpu::kSemDepthMeasured) == 0 && g_inputs.depth != nullptr) {
                    const uint32_t dc = (g_inputs.flags & mgpu::kInputFlagInvertedDepth) ? 1u : 2u;
                    sem_out = (sem_out & ~(3u << mgpu::kSemDepthShift)) |
                              (dc << mgpu::kSemDepthShift);
                }
                ctl->frames[slot].semantics = sem_out;
            }
            // v12: the game's own projection, when we have one (see adopt_fg_camera).
            // v12: the game's own projection, when the sniffer has a fresh one.
            if (have_cam_mtx) {
                memcpy(ctl->frames[slot].view_to_clip, cam_mtx,
                       sizeof(ctl->frames[slot].view_to_clip));
                ctl->frames[slot].matrix_tick_ms = cam_tick;
            } else {
                ctl->frames[slot].matrix_tick_ms = 0;
            }
            ctl->frames[slot].valid_w       = g_inputs.valid_w;
            ctl->frames[slot].valid_h       = g_inputs.valid_h;
            ctl->frames[slot].published     = value;   // v10: what the consumer ACKs with
            // The NDC bit is what lets the consumer set XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY
            // (only an NDC source needs it; the flag also implies the Y flip, which the mv_scale
            // published above already carries). Derived from the declared unit, so an XeSS source
            // with USE_NDC_VELOCITY gets it for free.
            ctl->producer.input_flags       = (g_inputs.flags & ~mgpu::kInputFlagMvActive) |
                                              (g_mv_active ? mgpu::kInputFlagMvActive : 0) |
                                              (g_fg_cam_live ? mgpu::kInputFlagHasMatrix : 0u) |
                                              (g_inputs.mv_unit == mgpu::input::MvUnit::ndc
                                                   ? mgpu::kInputFlagMvNdc : 0u);
            MemoryBarrier();
        }
        // OffloadQueue: hand the deferred copies to our own queue (which then signals the
        // consumer's fence), or rescue them on the game's queue if that was not possible.
        bool signalled = false;
        if (g_offload && (g_off_stream[1] || g_off_stream[2])) {
            signalled = offload_submit(slot, value);     // signals g_fwd[slot] from our queue
            if (!signalled) { offload_rescue(slot); }    // copies on the game's queue instead
        }
        if (!signalled) { signalled = SUCCEEDED(g_game_queue->Signal(g_fwd[slot].Get(), value)); }
        if (signalled) {
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

// Unconditional breadcrumb in %LOCALAPPDATA%\mgpu\breadcrumb.log.
// PRAGMATA (2026-11-09) showed that "ReShade logged Registered add-on" and "we wrote a byte
// of log" can disagree, and every hypothesis about why costs a game launch. This line needs
// no ini, no writable game directory and no logging switch: it answers "did AddonInit run,
// which ini did we read, what did it say, did the log open" in one shot.
static void write_breadcrumb(const char* stage)
{
    char dir[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { return; }
    strcat_s(dir, "\\mgpu");
    CreateDirectoryA(dir, nullptr);

    char addon[MAX_PATH]{};
    GetModuleFileNameA(g_self, addon, MAX_PATH);
    char ini[MAX_PATH]{};
    strcpy_s(ini, addon);
    if (char* dot = strrchr(ini, '.')) { strcpy_s(dot, MAX_PATH - (dot - ini), ".ini"); }

    char path[MAX_PATH]{};
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\breadcrumb.log", dir);
    FILE* f = _fsopen(path, "a", _SH_DENYNO);
    if (f == nullptr) { return; }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(f, "[%02u:%02u:%02u.%03u] pid %lu  %s  addon=%s  ini=%s  Log=%d  g_log=%s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            (unsigned long)GetCurrentProcessId(), stage, addon, ini,
            GetPrivateProfileIntA("capture", "Log", 0, ini), (g_log != nullptr) ? "open" : "null");
    fclose(f);
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
    g_self = addon_module;
    open_log();
    write_breadcrumb("AddonInit");
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
    // Last, and not an input at all: it only wants the module-load notifications so it
    // can read what the game's *own* frame generation is handed (projection matrix,
    // MV scale, depth direction). Its poll() always returns false, so `Input=auto`
    // can never pin it and nothing it sees is published ([capture] FgSniff).
    g_sources.push_back(mgpu::input::make_fg_source());

    // Loader hooks, so a source is told about a module the moment the game loads it
    // (M2: for DLSS that module is the driver-store _nvngx.dll).
    // The ini has to be read *before* this, because LoaderHook decides whether we patch
    // kernel32 at all.
    load_config();

    if (g_loader_hook) {
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
    } else {
        logf_("loader hooks DISABLED ([capture] LoaderHook=0) - modules are found by"
              " rescanning every 120 frames\n");
    }
    // Deferred: walking the module list (CreateToolhelp32Snapshot, which takes the loader lock)
    // and installing hooks from ReShade's add-on init wedges titles whose start-up is still
    // loading DLLs - Wuchang: Fallen Feathers, 2026-11-10: the log stopped right here and the
    // process sat there. The first on_begin_effects does the same work one frame later,
    // outside that path.
    g_scan_pending = true;
    logf_("modules: initial scan deferred to the first frame\n");

    reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
    reshade::register_event<reshade::addon_event::barrier>(on_barrier);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
    reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);

    logf_("events: init_effect_runtime, barrier, reshade_begin_effects, reshade_present\n");

    g_addon_alive = true;    // late callbacks are safe from here on (cleared in AddonUninit)
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
{
    g_addon_alive = false;   // first thing: stop the loader-hook path from touching our state
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



