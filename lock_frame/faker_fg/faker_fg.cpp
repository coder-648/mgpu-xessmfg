// ============================================================================
//  faker_fg.cpp - module lifetime, config, logging, and the capture-side API.
//
//  Everything here is policy and bookkeeping; the pipeline lives in
//  proxy_dxgi.cpp (swap chain), proxy_ngx.cpp (the contract), provider.cpp
//  (harvest + answer) and backend_xefg.cpp (generation + present).
//
//  Rules this file keeps (CAPTURE_RULES.md, the same discipline as the add-on):
//    * DllMain does I/O and nothing else: no LoadLibrary of anything that can
//      call back, no hooks, no GPU objects;
//    * config is read once;
//    * one log file, opened lazily so a failure to open never breaks startup.
// ============================================================================
#include "fg_internal.h"

#include <atomic>
#include <new>
#include <string.h>

namespace fakerfg {

// ---------------------------------------------------------------- clock
// One clock for every frame timestamp: QPC milliseconds, the same base WGC reports
// in IDirect3D11CaptureFrame::SystemRelativeTime (100 ns units). GetTickCount64's
// ~15.6 ms tick is fine for logs, but it quantised frame_qpc_ms and made the
// consumer's colour<->MV slot matching jump between frames.
uint64_t now_qpc_ms()
{
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(static_cast<double>(now.QuadPart) * 1000.0 /
                                 static_cast<double>(freq.QuadPart));
}

namespace {

Config g_cfg{};
bool   g_cfg_loaded = false;

FILE*  g_log = nullptr;
bool   g_log_tried = false;

// ---------------------------------------------------------------- paths
void exe_dir(wchar_t* out, size_t count)
{
    GetModuleFileNameW(nullptr, out, static_cast<DWORD>(count));
    if (wchar_t* slash = wcsrchr(out, L'\\')) { *slash = L'\0'; }
}

void exe_stem(wchar_t* out, size_t count)
{
    GetModuleFileNameW(nullptr, out, static_cast<DWORD>(count));
    if (wchar_t* dot = wcsrchr(out, L'.')) { *dot = L'\0'; }
}

void ini_path(wchar_t* out, size_t count)
{
    exe_dir(out, count);
    wcscat_s(out, count, L"\\fakerFG.ini");
}

int ini_int(const wchar_t* path, const wchar_t* key, int fallback)
{
    return GetPrivateProfileIntW(L"fakerFG", key, fallback, path);
}

// ---------------------------------------------------------------- the record
// One slot with a sequence number: the capture reads it inside the same
// Evaluate call (mgpu_fg_api.h states the contract), so one slot is enough and
// there is no allocator to get wrong on a render thread.
MgpuFgFrame g_record{};
uint64_t    g_seq = 0;
uint64_t    g_consumed = 0;
CRITICAL_SECTION g_record_lock{};
bool        g_record_lock_ready = false;

// v22: the present ring (WGC timestamp -> record seq) is gone; the publication slot is
// the identity. The pending_* fields below still describe the record being presented.
uint64_t                  g_pending_seq = 0;
uint64_t                  g_pending_eval_ms = 0;
uint64_t                  g_pending_key = 0;
// Pre-marker tile captured by backend_xefg's marker probe. The present ring is
// committed right after the real Present, so this sidecar travels with the seq
// the consumer already uses for pairing.
uint8_t                   g_pending_marker_tile[mgpu::kFrameMarkerBytes]{};
uint32_t                  g_pending_marker_format = 0;
uint32_t                  g_pending_marker_x = 0;
uint32_t                  g_pending_marker_y = 0;
bool                      g_pending_marker_valid = false;

void lock_record()
{
    if (!g_record_lock_ready) {
        InitializeCriticalSection(&g_record_lock);
        g_record_lock_ready = true;
    }
    EnterCriticalSection(&g_record_lock);
}

void unlock_record()
{
    if (g_record_lock_ready) { LeaveCriticalSection(&g_record_lock); }
}

MgpuFgFrame empty_record()
{
    MgpuFgFrame f{};
    f.size    = sizeof(MgpuFgFrame);
    f.version = MGPU_FG_API_VERSION;
    return f;
}

// ---------------------------------------------------------------- capture API
int api_frame_available() { return (g_seq != g_consumed) ? 1 : 0; }

int api_acquire(MgpuFgFrame* out)
{
    if (out == nullptr || out->size < sizeof(MgpuFgFrame)) { return 1; }
    lock_record();
    *out = g_record;
    unlock_record();
    return 0;
}

void api_release()
{
    lock_record();
    g_consumed = g_seq;
    unlock_record();
}

void api_log(const char* line)
{
    if (line != nullptr) { logf("capture: %s\n", line); }
}

// ---------------------------------------------------------------- v2: sink
// One subscriber, published atomically. The capture is the only consumer and it
// needs the record *while the title's command list is open*, which polling can
// never guarantee - see mgpu_fg_api.h.
std::atomic<MgpuFgFrameSink> g_sink{ nullptr };
std::atomic<void*>           g_sink_user{ nullptr };

int api_subscribe(MgpuFgFrameSink sink, void* user)
{
    if (sink == nullptr) { return 1; }
    g_sink_user.store(user, std::memory_order_release);
    g_sink.store(sink, std::memory_order_release);
    logf("api: capture sink subscribed\n");
    return 0;
}

void api_unsubscribe()
{
    if (g_sink.exchange(nullptr, std::memory_order_acq_rel) != nullptr) {
        logf("api: capture sink unsubscribed\n");
    }
    g_sink_user.store(nullptr, std::memory_order_release);
}

// Runs outside the record lock: the subscriber records GPU copies into the
// title's list, which is not instant, and holding the record would make the
// next Evaluate wait on it.
void dispatch_sink(const MgpuFgFrame& f)
{
    MgpuFgFrameSink sink = g_sink.load(std::memory_order_acquire);
    if (sink == nullptr) { return; }
    void* user = g_sink_user.load(std::memory_order_acquire);
    MgpuFgFrame copy = f;          // the sink sees a stable snapshot, never g_record
    sink(&copy, user);
}

MgpuFgApi g_api{};
bool      g_api_ready = false;

void api_init()
{
    if (g_api_ready) { return; }
    g_api.size            = sizeof(MgpuFgApi);
    g_api.version         = MGPU_FG_API_VERSION;
    g_api.mode            = &api_mode;
    g_api.frame_available = &api_frame_available;
    g_api.acquire         = &api_acquire;
    g_api.release         = &api_release;
    g_api.log             = &api_log;
    g_api.shared_block    = nullptr;
    g_api.subscribe       = &api_subscribe;
    g_api.unsubscribe     = &api_unsubscribe;
    g_record              = empty_record();
    g_api_ready           = true;
    logf("api ready (version %u, record %u bytes)\n",
         MGPU_FG_API_VERSION, static_cast<unsigned>(sizeof(MgpuFgFrame)));
}

} // namespace

// Derived API mode (not a user setting): Passthrough disables the API path; FgOutput
// chooses local XeFG vs the bridge/capture path. MgpuFgFrame.mode still travels on the
// wire because producer.cpp only checks MGPU_FG_MODE_OFF.
uint32_t api_mode()
{
    const Config& c = cfg();
    if (c.passthrough != 0 || c.fg_output == static_cast<int>(FgOutputSink::NoFG)) {
        return MGPU_FG_MODE_OFF;
    }
    if (c.fg_output == static_cast<int>(FgOutputSink::XeFG)) {
        return MGPU_FG_MODE_LOCAL;
    }
    return MGPU_FG_MODE_BRIDGE;
}

// ---------------------------------------------------------------- present ring
static uint64_t qpc_100ns()
{
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (freq.QuadPart <= 0) { return 0; }
    const uint64_t s = static_cast<uint64_t>(now.QuadPart / freq.QuadPart);
    const uint64_t r = static_cast<uint64_t>(now.QuadPart % freq.QuadPart);
    return s * 10000000ull + (r * 10000000ull) / static_cast<uint64_t>(freq.QuadPart);
}

// v22: present_ring_start/begin/commit deleted with the WGC pairing path.
const MgpuFgApi* api_table()
{
    api_init();
    return &g_api;
}

// ---------------------------------------------------------------- config & log
void log_open()
{
    if (g_log != nullptr || g_log_tried) { return; }
    g_log_tried = true;

    wchar_t path[MAX_PATH]{};
    exe_stem(path, MAX_PATH);
    wcscat_s(path, MAX_PATH, L"_fakerFG.log");
    g_log = _wfsopen(path, L"a", _SH_DENYNO);
    if (g_log == nullptr) { return; }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(g_log, "\n==== fakerFG pid=%lu %04u-%02u-%02u %02u:%02u:%02u ====\n",
            GetCurrentProcessId(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fflush(g_log);
}

void logf(const char* fmt, ...)
{
    log_open();
    if (g_log == nullptr) { return; }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(g_log, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fflush(g_log);
}

void log_flush()
{
    if (g_log != nullptr) { fflush(g_log); }
}

// ---------------------------------------------------------------- crash reporting
// A dead process says nothing; the log just stops. This writes the exception code, the
// faulting instruction and every stack word that resolves to a loaded module, which is
// enough to name the module (ours, Streamline, the NGX core) that actually crashed.
namespace {

typedef PVOID(WINAPI* pfn_pc_to_file)(PVOID, PVOID*);

pfn_pc_to_file pc_to_file()
{
    static pfn_pc_to_file fn = []() -> pfn_pc_to_file {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        return (k32 != nullptr)
                   ? reinterpret_cast<pfn_pc_to_file>(GetProcAddress(k32, "RtlPcToFileHeader"))
                   : nullptr;
    }();
    return fn;
}

void describe_pc(const char* what, void* pc)
{
    pfn_pc_to_file f = pc_to_file();
    PVOID base = nullptr;
    if (f == nullptr || pc == nullptr || f(pc, &base) == nullptr || base == nullptr) {
        logf("crash:   %s %p (no module)\n", what, pc);
        return;
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = (name != nullptr) ? name + 1 : path;
    logf("crash:   %s %p = %ls+0x%llX\n", what, pc, name,
         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pc) -
                                         reinterpret_cast<uintptr_t>(base)));
}

void log_crash(EXCEPTION_POINTERS* ep, const char* phase)
{
    if (ep == nullptr || ep->ExceptionRecord == nullptr) { return; }
    const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    logf("crash: %s code=0x%08X at %p info0=%llu tid=%lu\n", phase,
         static_cast<unsigned>(rec->ExceptionCode), rec->ExceptionAddress,
         static_cast<unsigned long long>(rec->ExceptionInformation[0]), GetCurrentThreadId());
    describe_pc("rip", rec->ExceptionAddress);
#if defined(_M_X64)
    if (ep->ContextRecord != nullptr) {
        const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(ep->ContextRecord->Rsp);
        for (int i = 0; i < 32 && sp != nullptr; ++i) {
            void* pc = reinterpret_cast<void*>(sp[i]);
            if (reinterpret_cast<uintptr_t>(pc) < 0x10000) { continue; }
            describe_pc("stack", pc);
        }
    }
#endif
    logf("crash: (end)\n");
    log_flush();
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
{
    log_crash(ep, "unhandled");
    return EXCEPTION_CONTINUE_SEARCH;   // let the game's own handler / WER finish
}

// First chance: only the codes that always end the process are worth a line.
LONG CALLBACK crash_veh(EXCEPTION_POINTERS* ep)
{
    static LONG told = 0;
    if (ep == nullptr || ep->ExceptionRecord == nullptr) { return EXCEPTION_CONTINUE_SEARCH; }
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const bool fatal = code == 0xC0000005u || code == 0xC000001Du || code == 0xC0000409u ||
                       code == 0xC0000374u || code == 0xC00000FDu;
    if (fatal && InterlockedExchange(&told, 1) == 0) { log_crash(ep, "first-chance"); }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void install_crash_log()
{
    SetUnhandledExceptionFilter(&crash_filter);
    AddVectoredExceptionHandler(1, &crash_veh);
}

// OptiScaler's Config::SaveXeFG(): the three init flags it derives from the upscaler's
// create flags are written back so the *next* init of XeFG uses them (the context is
// created at swap-chain time, long before the upscaler initialises).
void cfg_save_upscaler_flags(int depth_inverted, int mv_jittered, int high_res_mv)
{
    wchar_t path[MAX_PATH]{};
    ini_path(path, MAX_PATH);

    Config& c = cfg();
    if (c.depth_inverted == depth_inverted && c.mv_jittered == mv_jittered &&
        c.high_res_mv == high_res_mv) {
        return;                                            // already saved
    }
    c.depth_inverted = depth_inverted;
    c.mv_jittered    = mv_jittered;
    c.high_res_mv    = high_res_mv;

    wchar_t num[3][16]{};
    swprintf_s(num[0], L"%d", depth_inverted);
    swprintf_s(num[1], L"%d", mv_jittered);
    swprintf_s(num[2], L"%d", high_res_mv);
    const wchar_t* keys[3] = { L"DepthInverted", L"MvJittered", L"HighResMv" };
    for (int i = 0; i < 3; ++i) {
        WritePrivateProfileStringW(L"fakerFG", keys[i], num[i], path);
    }
    logf("cfg: saved XeFG init flags from the upscaler (DepthInverted=%d MvJittered=%d"
         " HighResMv=%d) - they apply from the next launch\n",
         depth_inverted, mv_jittered, high_res_mv);
}

// ---------------------------------------------------------------- game quirks
typedef struct GameProfile
{
    const wchar_t* exe;        // exact base name, compared case-insensitively
    const wchar_t* ue;         // UE project name: expands to <name>-win64-shipping.exe etc.
    unsigned       quirks;
    int            skip_first_frames;   // GameConfigs SkipFirstFrames (0 = off)
    int            force_borderless;    // FGXeFGForceBorderless
    int            mv_state;            // MVResourceBarrier  (0 = ini/code default)
    int            depth_state;         // DepthResourceBarrier
    int            color_state;         // ColorResourceBarrier
} GameProfile;

// Ported from misc/GameConfigs.cpp, restricted to the settings this module actually has
// (the other entries there only affect replacing the upscaler itself - FSR/XeSS inputs,
// DxgiSpoofing, fakenvapi - which we do not do). `FGDisableHUDFix=true` needs no entry:
// the Upscaler lane never tags a colour here, which is that setting's effect.
// 0x40 = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE (their default when unset).
static const GameProfile g_profiles[] = {
    { L"cyberpunk2077.exe",                 nullptr, Quirk_CyberpunkHudlessState,
      0, 0, 0, 0, 0 },
    // Wuchang: `.defaults = { SkipFirstFrames = 10 }` ("Skip 1 frame use of upscaler which
    // cause crash").
    { L"project_plague-win64-shipping.exe", nullptr, 0u, 10, 0, 0, 0, 0 },
    { L"project_plague-deck-shipping.exe",  nullptr, 0u, 10, 0, 0, 0, 0 },
    // MVResourceBarrier / ColorResourceBarrier = NON_PIXEL_SHADER_RESOURCE
    { nullptr, L"returnal",       0u, 0, 0, 0x40, 0, 0x40 },
    { nullptr, L"hellbladegame",  0u, 0, 0, 0x40, 0, 0x40 },
    { nullptr, L"outriders",      0u, 0, 0, 0x40, 0, 0x40 },
    { nullptr, L"medium",         0u, 0, 0, 0x40, 0, 0x40 },
    { nullptr, L"sackboy",        0u, 0, 0, 0x40, 0, 0x40 },
    { nullptr, L"pumpkinjack",    0u, 0, 0, 0x40, 0, 0x40 },
    { nullptr, L"dungeonhaven",   0u, 0, 0, 0x40, 0, 0x40 },
    { nullptr, L"pal7",           0u, 0, 0, 0x40, 0, 0x40 },
    { L"observersystemredux.exe",  nullptr, 0u, 0, 0, 0x40, 0, 0x40 },
    // ColorResourceBarrier = NON_PIXEL_SHADER_RESOURCE
    { nullptr, L"avowed",         0u, 0, 0, 0, 0, 0x40 },
    // FGXeFGForceBorderless = true
    { L"dead space.exe",           nullptr, 0u, 0, 1, 0, 0, 0 },
    { L"metroexodus.exe",          nullptr, 0u, 0, 1, 0, 0, 0 },
    // ForceUnrealEngine
    { nullptr, L"ff7rebirth_",    Quirk_ForceUnrealEngine, 0, 0, 0, 0, 0 },
    { nullptr, 0u, 0u, 0, 0, 0, 0, 0 },
};

namespace {
unsigned g_quirks = 0;

bool exe_matches(const wchar_t* name)
{
    if (name == nullptr) { return false; }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* base = wcsrchr(path, L'\\');
    base = (base != nullptr) ? base + 1 : path;
    return _wcsicmp(base, name) == 0;
}

// GameConfigs `ueExes`: a UE project name matches <project>-win64-shipping.exe and the
// other UE suffixes (Util.cpp:278-284).
bool ue_matches(const wchar_t* project)
{
    if (project == nullptr) { return false; }
    static const wchar_t* kSuffixes[] = {
        L"-win64-shipping.exe", L"-wingdk-shipping.exe", L"-windows-shipping.exe",
        L"-win64-test.exe", L"-win64-debug.exe", L"-windows-test.exe", L"-windows-debug.exe",
    };
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* base = wcsrchr(path, L'\\');
    base = (base != nullptr) ? base + 1 : path;
    const size_t plen = wcslen(project);
    if (_wcsnicmp(base, project, plen) != 0) { return false; }
    for (const wchar_t* suffix : kSuffixes) {
        if (_wcsicmp(base + plen, suffix) == 0) { return true; }
    }
    return false;
}
} // namespace

bool force_borderless() { return cfg().force_borderless != 0; }
bool simulate_waitable_object() { return cfg().simulate_waitable != 0; }

unsigned game_quirks() { return g_quirks; }
void set_game_quirk(GameQuirk q) { g_quirks |= static_cast<unsigned>(q); }
bool has_game_quirk(GameQuirk q) { return (g_quirks & static_cast<unsigned>(q)) != 0u; }

Config& cfg()
{
    if (g_cfg_loaded) { return g_cfg; }
    g_cfg_loaded = true;

    int profile_skip = 0;
    int profile_borderless = 0;
    int profile_mv_state = 0;
    int profile_depth_state = 0;
    int profile_color_state = 0;
    for (const GameProfile* p = g_profiles; p->exe != nullptr || p->ue != nullptr; ++p) {
        if (!exe_matches(p->exe) && !ue_matches(p->ue)) { continue; }
        g_quirks |= p->quirks;
        profile_skip        = p->skip_first_frames;
        profile_borderless  = p->force_borderless;
        profile_mv_state    = p->mv_state;
        profile_depth_state = p->depth_state;
        profile_color_state = p->color_state;
        break;
    }

    wchar_t path[MAX_PATH]{};
    ini_path(path, MAX_PATH);

    g_cfg.interpolated   = ini_int(path, L"Interpolated", 1);
    g_cfg.inject_reshade = ini_int(path, L"InjectReshade", 1);
    g_cfg.own_swapchain  = ini_int(path, L"OwnSwapchain", 1);
    g_cfg.passthrough    = ini_int(path, L"Passthrough", 0);
    g_cfg.depth_inverted = ini_int(path, L"DepthInverted", 1);
    g_cfg.mv_jittered    = ini_int(path, L"MvJittered", 0);
    g_cfg.high_res_mv    = ini_int(path, L"HighResMv", 0);
    g_cfg.mv_resample      = ini_int(path, L"MvResample", 0);
    g_cfg.mv_scale_resolution = ini_int(path, L"MvScaleMultiplyResolution", 1);
    g_cfg.fg_depth_valid_now    = ini_int(path, L"FGDepthValidNow", 0);
    g_cfg.fg_velocity_valid_now = ini_int(path, L"FGVelocityValidNow", 0);
    g_cfg.fg_hudless_valid_now  = ini_int(path, L"FGHudlessValidNow", 0);
    {
        wchar_t hs[32]{};
        GetPrivateProfileStringW(L"fakerFG", L"HudlessSource", L"Auto", hs, 32, path);
        if (_wcsicmp(hs, L"Scaling") == 0) { g_cfg.hudless_source = 1; }
        else if (_wcsicmp(hs, L"Hudless") == 0) { g_cfg.hudless_source = 2; }
        else { g_cfg.hudless_source = 0; }
    }
    g_cfg.real_swapchain_object = ini_int(path, L"RealSwapchainObject", 0);
    g_cfg.fake_dlssg_feature = ini_int(path, L"FakeDlssgFeature", 1);
    g_cfg.write_output_real = ini_int(path, L"WriteOutputReal", 0);
    g_cfg.write_output_interp = ini_int(path, L"WriteOutputInterp", 0);
    g_cfg.fg_draw_ui_over_fg = ini_int(path, L"FGDrawUIOverFG", 0);
    g_cfg.fg_ui_premultiplied = ini_int(path, L"FGUIPremultipliedAlpha", 1);
    g_cfg.fg_hud_cutoff    = ini_int(path, L"FGHudCutoff", 0);
    g_cfg.fg_hudless_compare = ini_int(path, L"FGHudlessCompare", 0);
    g_cfg.ui_premultiplied = ini_int(path, L"UiPremultiplied", 0);
    g_cfg.ui_tag_synth = ini_int(path, L"UiTagSynth", 0);
    g_cfg.ui_snapshot_pq = ini_int(path, L"UiSnapshotPQ", 0);
    g_cfg.tag_hudless    = ini_int(path, L"TagHudless", 0);
    g_cfg.tag_backbuffer = ini_int(path, L"TagBackbuffer", 1);
    g_cfg.tag_at_evaluate = ini_int(path, L"TagAtEvaluate", 1);
    g_cfg.rect_left      = ini_int(path, L"RectLeft", 0);
    g_cfg.rect_top       = ini_int(path, L"RectTop", 0);
    g_cfg.rect_w         = ini_int(path, L"RectWidth", 0);
    g_cfg.rect_h         = ini_int(path, L"RectHeight", 0);
    g_cfg.tag_ui         = ini_int(path, L"TagUi", 0);
    g_cfg.strip_dlssg    = ini_int(path, L"StripDlssGPlugin", 0);
    g_cfg.fake_sl_player = ini_int(path, L"FakeSlDlssg", 1);
    g_cfg.ngx_core_proxy = ini_int(path, L"NgxCoreProxy", 0);
    g_cfg.arch_spoof     = ini_int(path, L"ArchSpoof", 1);
    g_cfg.log            = ini_int(path, L"Log", 1);
    // Publish=1 (default) / 0. The negative form NoPublish=1 is kept as an alias so an
    // older ini keeps working; Publish wins when both are present.
    {
        const int publish = ini_int(path, L"Publish", -1);
        const int no_publish = ini_int(path, L"NoPublish", 0);
        g_cfg.no_publish = (publish >= 0) ? (publish == 0 ? 1 : 0) : no_publish;
    }
    g_cfg.debug_view     = ini_int(path, L"DebugView", 0);
    g_cfg.debug_only_generated = ini_int(path, L"DebugOnlyGenerated", 0);
    g_cfg.ignore_init_checks = ini_int(path, L"IgnoreInitChecks", 0);
    g_cfg.mv_state = ini_int(path, L"MvState", profile_mv_state);
    g_cfg.depth_state = ini_int(path, L"DepthState", profile_depth_state);
    g_cfg.color_state = ini_int(path, L"ColorState", profile_color_state);
    g_cfg.force_borderless = ini_int(path, L"ForceBorderless", profile_borderless);
    g_cfg.skip_first_frames = ini_int(path, L"SkipFirstFrames", profile_skip);
    g_cfg.simulate_waitable = ini_int(path, L"SimulateWaitableObject", 0);
    {
        wchar_t v[32]{};
        GetPrivateProfileStringW(L"fakerFG", L"CameraNear", L"0.1", v, 32, path);
        g_cfg.camera_near = static_cast<float>(_wtof(v));
        GetPrivateProfileStringW(L"fakerFG", L"CameraFar", L"100000", v, 32, path);
        g_cfg.camera_far = static_cast<float>(_wtof(v));
        GetPrivateProfileStringW(L"fakerFG", L"VerticalFov", L"60", v, 32, path);
        g_cfg.vertical_fov_deg = static_cast<float>(_wtof(v));
    }

    // [FrameGen] FgInput / FgOutput, with OptiScaler's own codes and its own validation
    // (dllmain: an input without an output is downgraded to NoFG).
    wchar_t fgi[32]{};
    GetPrivateProfileStringW(L"fakerFG", L"FgInput", L"NoFG", fgi, 32, path);
    char fgi_a[32]{};
    WideCharToMultiByte(CP_UTF8, 0, fgi, -1, fgi_a, sizeof(fgi_a), nullptr, nullptr);
    if (_stricmp(fgi_a, "Upscaler") == 0)      { g_cfg.fg_input = 1; }
    else if (_stricmp(fgi_a, "DLSSG") == 0)    { g_cfg.fg_input = 2; }
    else if (_stricmp(fgi_a, "NVNGX") == 0)    { g_cfg.fg_input = 3; }
    else if (_stricmp(fgi_a, "FSR") == 0)      { g_cfg.fg_input = 4; }
    else                                        { g_cfg.fg_input = 0; }

    wchar_t fgo[32]{};
    GetPrivateProfileStringW(L"fakerFG", L"FgOutput", L"", fgo, 32, path);
    char fgo_a[32]{};
    WideCharToMultiByte(CP_UTF8, 0, fgo, -1, fgo_a, sizeof(fgo_a), nullptr, nullptr);
    const bool output_explicit = fgo_a[0] != '\0';
    g_cfg.fg_output = 0;
    if (_stricmp(fgo_a, "XeFG") == 0)        { g_cfg.fg_output = static_cast<int>(FgOutputSink::XeFG); }
    else if (_stricmp(fgo_a, "Capture") == 0) { g_cfg.fg_output = static_cast<int>(FgOutputSink::Capture); }

    // Only FgInput is a public choice for the input source. Everything else below is
    // derived, so an old ini cannot leave a lane half-configured.
    const bool input_explicit = fgi_a[0] != '\0';
    const bool input_no_fg = _stricmp(fgi_a, "NoFG") == 0;
    // This build implements the DLSSG lane only (Streamline tags). Keep the key readable
    // for old configs but pin the lane: a missing value or any other source falls back to
    // DLSSG (an explicit NoFG still disables the pipeline).
    if (!input_no_fg && g_cfg.fg_input != static_cast<int>(FgInputSource::DLSSG)) {
        if (input_explicit) {
            logf("config: FgInput=%s is not implemented in this build - using DLSSG\n", fgi_a);
        }
        g_cfg.fg_input = static_cast<int>(FgInputSource::DLSSG);
    }
    if (g_cfg.fg_input != 0) {
        if (g_cfg.fg_output == 0 && !output_explicit) {
            g_cfg.fg_output = static_cast<int>(FgOutputSink::Capture);
        }
        switch (g_cfg.fg_input) {
        case static_cast<int>(FgInputSource::Upscaler):
            g_cfg.fake_dlssg_feature = 0;
            g_cfg.fake_sl_player     = 1;
            g_cfg.strip_dlssg        = 0;
            g_cfg.tag_at_evaluate    = 1;
            g_cfg.mv_resample        = 1;
            g_cfg.write_output_real  = 0;
            g_cfg.write_output_interp= 0;
            break;
        case static_cast<int>(FgInputSource::FSR):
            g_cfg.fake_dlssg_feature = 0;
            g_cfg.fake_sl_player     = 1;
            g_cfg.strip_dlssg        = 0;
            g_cfg.tag_at_evaluate    = 0;
            g_cfg.mv_resample        = 0;
            g_cfg.write_output_real  = 0;
            g_cfg.write_output_interp= 0;
            break;
        case static_cast<int>(FgInputSource::DLSSG):
            g_cfg.fake_dlssg_feature = 0;
            g_cfg.fake_sl_player     = 1;
            g_cfg.strip_dlssg        = 1;
            g_cfg.tag_at_evaluate    = 0;
            g_cfg.mv_resample        = 0;
            g_cfg.write_output_real  = 0;
            g_cfg.write_output_interp= 0;
            break;
        case static_cast<int>(FgInputSource::NVNGX):
            g_cfg.fake_dlssg_feature = 1;
            g_cfg.fake_sl_player     = 0;
            g_cfg.strip_dlssg        = 0;
            g_cfg.tag_at_evaluate    = 0;
            g_cfg.write_output_real  = 1;
            g_cfg.write_output_interp= 1;
            break;
        default: break;                         // Upscaler keeps its legacy keys
        }
        // OptiScaler DLSSG-input has two layers: strip the Streamline plugin above,
        // and stop a direct native NVNGX FrameGeneration caller here. Answer
        // CreateFeature with our fake handle and harvest its Evaluate; never forward
        // the game's native DLSSG execution.
        if (g_cfg.fg_output == static_cast<int>(FgOutputSink::Capture) &&
            g_cfg.fg_input == static_cast<int>(FgInputSource::DLSSG)) {
            g_cfg.fake_dlssg_feature = 1;
            g_cfg.write_output_real  = 1;
            g_cfg.write_output_interp= 0;
        }
        // Capture + NVNGX: keep the real caller so NVNGX Evaluate still arrives, but
        // remove the DLSS-G plugin's own swapchain hooks (OptiScaler's
        // hkdlssg_slOnPluginLoad) and never write OutputInterpolated.
        if (g_cfg.fg_output == static_cast<int>(FgOutputSink::Capture) &&
            g_cfg.fg_input == static_cast<int>(FgInputSource::NVNGX)) {
            g_cfg.fake_sl_player     = 0;
            g_cfg.strip_dlssg        = 0;
            g_cfg.write_output_real   = 1;
            g_cfg.write_output_interp = 0;
        }
        // A/B: CP2077 may not need the provider to copy Backbuffer into OutputReal when
        // faker owns the presenting path. NoOutputReal=1 disables this full 4K copy.
        if (ini_int(path, L"NoOutputReal", 0) != 0) {
            g_cfg.write_output_real = 0;
            logf("config: NoOutputReal=1 - skipping Backbuffer -> OutputReal copy\n");
        }
        logf("config: FgInput profile: %s -> fake_dlssg=%d fake_sl=%d strip_sl=%d"
             " tag_at_eval=%d mv_resample=%d write_real=%d write_interp=%d\n",
             fgi_a, g_cfg.fake_dlssg_feature, g_cfg.fake_sl_player, g_cfg.strip_dlssg,
             g_cfg.tag_at_evaluate, g_cfg.mv_resample, g_cfg.write_output_real,
             g_cfg.write_output_interp);
    }

    if (g_cfg.fg_output == 0 && g_cfg.fg_input != 0) {
        logf("config: FgInput without FgOutput - using none (OptiScaler's dllmain check)\n");
        g_cfg.fg_input = 0;
    }

    if (g_cfg.interpolated < 1) { g_cfg.interpolated = 1; }
    if (g_cfg.interpolated > 6) { g_cfg.interpolated = 6; }

    logf("config: %ls FgInput=%d FgOutput=%d Interpolated=%d InjectReshade=%d"
         " OwnSwapchain=%d Passthrough=%d DepthInverted=%d MvJittered=%d HighResMv=%d"
         " TagHudless=%d TagUi=%d TagAtEvaluate=%d ArchSpoof=%d Log=%d Publish=%d\n",
         path, g_cfg.fg_input, g_cfg.fg_output, g_cfg.interpolated,
         g_cfg.inject_reshade, g_cfg.own_swapchain, g_cfg.passthrough, g_cfg.depth_inverted,
         g_cfg.mv_jittered, g_cfg.high_res_mv, g_cfg.tag_hudless, g_cfg.tag_ui,
         g_cfg.tag_at_evaluate, g_cfg.arch_spoof, g_cfg.log, g_cfg.no_publish ? 0 : 1);
    return g_cfg;
}

// One accepted record is queued for the current presentation until producer_present()
// publishes it. This is the cross-lane "one seq per present" guard.
static bool g_record_pending = false;
static unsigned g_collapsed_same_present = 0;

// Releasing the flag is producer_present()'s job, and that call lives on the Present
// detour XeFG installs only after it has taken the swap chain. When that never happens
// (libxess_fg.dll without xefgSwapChainD3D12GetProperties -> "XeFG could not take the
// swap chain"), the flag stayed true for the whole session and every later record was
// collapsed into "same present": exactly ONE record reached the producer, its 1.2 s
// publish gate expired and Local\mgpu_mailbox_v1 was never created, so the UI reported
// "target ... is not the producer (producer_pid=0)" (Cyberpunk 2077, 2026-11-20).
// A record this much newer than the pending one cannot belong to the same presentation;
// release the stale slot instead of wedging the transport for the rest of the session.
inline constexpr uint64_t kStalePendingMs = 100;

// ---------------------------------------------------------------- record
void publish_record(const MgpuFgFrame& f)
{
    // [measurement] NoPublish=1 removes the whole publish path: no snapshot copy, no
    // lock, no sink call (and therefore no transport work in the consumer). This is the
    // baseline for measuring what publishing costs on the render thread.
    if (cfg().no_publish != 0) { return; }

    // Several lanes (provider + Streamline + NVNGX) can publish the same Evaluate
    // record. They carry the same MV/depth resources and a frame_qpc within a few
    // milliseconds; treating them as separate frames made the producer publish 3x and
    // made the present ring's seq miss the producer's frame_id. Collapse them here.
    bool duplicate = false;
    bool same_present = false;
    bool stale_present = false;
    uint64_t prev_qpc = 0;
    lock_record();
    prev_qpc = g_record.frame_qpc_ms;
    if (f.frame_qpc_ms != 0 && g_record.frame_qpc_ms != 0 &&
        f.stream[MGPU_FG_STREAM_MV].resource == g_record.stream[MGPU_FG_STREAM_MV].resource &&
        f.stream[MGPU_FG_STREAM_DEPTH].resource == g_record.stream[MGPU_FG_STREAM_DEPTH].resource &&
        f.frame_qpc_ms >= g_record.frame_qpc_ms &&
        f.frame_qpc_ms - g_record.frame_qpc_ms <= 5) {
        duplicate = true;
    }
    // A second lane describing the same presentation: the first record already owns this
    // present's seq and the producer publishes it. A second seq would only make the marker
    // point at a frame_id that never reaches frames[] (the classic no_slot).
    // The producer_present() boundary is the authoritative "same presentation" scope.
    if (g_record_pending) {
        if (f.frame_qpc_ms != 0 && prev_qpc != 0 && f.frame_qpc_ms > prev_qpc &&
            f.frame_qpc_ms - prev_qpc > kStalePendingMs) {
            stale_present = true;      // no present boundary ever arrived for the old one
        } else {
            same_present = true;
        }
    }
    unlock_record();
    if (duplicate) { return; }
    if (stale_present) {
        static unsigned told_stall = 0;
        ++told_stall;
        if (told_stall <= 5 || (told_stall % 600u) == 0u) {
            logf("record: pending record never reached a present (%llu ms older) -"
                 " publishing anyway; is the swap chain hooked? (XeFG/libxess_fg; %u so far)\n",
                 static_cast<unsigned long long>(f.frame_qpc_ms - prev_qpc), told_stall);
        }
        fg_present_done();
    }
    if (same_present) {
        ++g_collapsed_same_present;
        if ((g_collapsed_same_present % 600u) == 0u) {
            logf("record: same-present second lane collapsed (%u so far)\n",
                 g_collapsed_same_present);
        }
        return;
    }

    MgpuFgFrame snap;
    // v22: attach the chain back buffer as the colour stream when the record has none
    // (the upscaler lane never carries colour). The reference is held only for the
    // duration of dispatch_sink(): the producer records the GPU copy there.
    MgpuFgFrame rec = f;
    ID3D12Resource* colour_ref = nullptr;
    if (rec.stream[MGPU_FG_STREAM_BACKBUFFER].resource == nullptr && provider_owns_output()) {
        if (xefg_record_backbuffer(&rec)) {
            colour_ref = static_cast<ID3D12Resource*>(
                rec.stream[MGPU_FG_STREAM_BACKBUFFER].resource);
        }
    }
    lock_record();
    g_record = rec;
    g_record.seq = ++g_seq;
    // lock_frame: fakerFG 在 present 前把这个 seq 画成四角 marker。producer 发布的
    // frame_id 就是同一个 seq，消费端用 marker 的 seq12 + ms12 精确配对。
    g_pending_seq = g_record.seq;
    g_pending_eval_ms = g_record.frame_qpc_ms;
    snap = g_record;
    unlock_record();
    // v2: the capture records its copies here, while the record's command list is
    // still the open one. Never inside the lock (see dispatch_sink).
    dispatch_sink(snap);
    g_record_pending = true;
    xefg_native_frame();
    if (colour_ref != nullptr) { colour_ref->Release(); }
}

bool fg_record_pending() { return g_record_pending; }
void fg_present_done()     { g_record_pending = false; }

void fg_output_submit(const MgpuFgFrame& f)
{
    // Input sources only fill the record. The output sink decides whether that record is
    // tagged for XeFG; Capture means the producer is the only consumer.
    if (cfg().fg_output != static_cast<int>(FgOutputSink::XeFG)) { return; }
    if (!xefg_wants_generation()) { return; }
    if (f.cmd_list == nullptr || f.present_id == 0) { return; }
    if (cfg().fg_input == static_cast<int>(FgInputSource::Upscaler) &&
        cfg().tag_at_evaluate != 0) {
        return;                                 // already tagged inside SR Evaluate
    }
    xefg_tag(static_cast<ID3D12GraphicsCommandList*>(f.cmd_list),
             static_cast<uint32_t>(f.present_id), f,
             cfg().fg_input == static_cast<int>(FgInputSource::Upscaler));
    // Present only adds frame constants + SetPresentId; it must dispatch THE frame whose
    // tags the library just accepted (not a fresh id, and never a rejected one).
    if (xefg_tags_complete()) { xefg_note_submit(static_cast<uint32_t>(f.present_id)); }
}

void clear_record()
{
    lock_record();
    g_record = empty_record();
    g_consumed = g_seq;
    unlock_record();
}

MgpuFgFrame make_empty_record()
{
    return empty_record();
}

// ---------------------------------------------------------------- loader
namespace {

// ReShade is displaced to ReShade64.dll by our deployment: it cannot be
// dxgi.dll, because that is us. Loading it here is what makes the capture
// add-on exist at all (OptiScaler's [Loader] LoadReshade does the same).
void load_reshade()
{
    if (cfg().inject_reshade == 0) {
        logf("reshade: InjectReshade=0, not loading\n");
        return;
    }
    if (GetModuleHandleW(L"ReShade64.dll") != nullptr ||
        GetModuleHandleW(L"d3d12-ReShade64.dll") != nullptr) {
        logf("reshade: already loaded\n");
        return;
    }

    wchar_t dir[MAX_PATH]{};
    exe_dir(dir, MAX_PATH);

    const wchar_t* names[] = { L"\\ReShade64.dll", L"\\d3d12-ReShade64.dll", L"\\ReShade.dll" };
    for (const wchar_t* n : names) {
        wchar_t path[MAX_PATH]{};
        wcscpy_s(path, dir);
        wcscat_s(path, n);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) { continue; }

        HMODULE m = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m != nullptr) {
            logf("reshade: loaded %ls -> %p (the add-on follows on its own)\n", path, (void*)m);
            return;
        }
        logf("reshade: LoadLibrary(%ls) failed, GetLastError=%lu\n", path, GetLastError());
    }
    logf("reshade: not found next to the exe (ReShade64.dll / d3d12-ReShade64.dll) - the"
         " capture add-on will not load; the FG pipeline still runs\n");
}

DWORD WINAPI loader_thread(LPVOID)
{
    // 50 ms so this thread never holds (or waits on) the loader lock our own
    // DllMain was entered with (CAPTURE_RULES 3).
    Sleep(50);
    logf("loader: thread up\n");

    // 0a. The transport module (optional, independent of the FG chain). Loaded here
    //     because this is the only thread allowed to call LoadLibrary.
    producer_link_start();
    // v22: present_ring_start() removed. The consumer no longer pairs by WGC timestamp;
    // the publication slot carries colour + MV + depth.

    // 0. NGX core interposition first: Streamline resolves every DLSS-G entry point
    //    from the driver's nvngx.dll/_nvngx.dll (measured), so this has to be in place
    //    before slInit - it is the cheapest of the three and has no deadline of its own.
    //    The LoadLibrary layer goes first of all: it is what makes the NGX core request
    //    return our module (OptiScaler's LibraryLoad_Hooks), and DLSS-SR only reaches us
    //    through the module that answered that request.
    kernel_install_if_possible();
    nvngx_install_if_possible();
    ffx_scan_loaded_modules();

    // 1. Architecture gate: the question comes from sl_common, so the hook has to
    //    exist before Streamline initialises.
    for (int i = 0; i < 2000; ++i) {
        if (GetModuleHandleW(L"nvapi64.dll") != nullptr ||
            GetModuleHandleW(L"nvapi.dll") != nullptr) {
            arch_install_if_possible();
            break;
        }
        Sleep(10);
    }

    // 2. Streamline: install as soon as sl.interposer.dll loads. The module-load hook
    //    signals (it cannot install there: MH_EnableHook suspends threads and that hook runs
    //    under the loader lock), so this thread only waits - no GetModuleHandleW polling,
    //    which was contending with the game's own LoadLibrary calls and showing up as a
    //    startup stutter. OptiScaler installs at load time; this is the same ordering with
    //    the suspension moved off the loader thread.
    // Our own Streamline first: OptiScaler loads its `streamline\` copies by full path so the
    // game's layout does not matter (proxies/Streamline_Proxy.h:88-105). When that succeeds,
    // the game's later LoadLibrary("sl.interposer.dll") resolves to this copy by base name.
    if (sl_preload_own_copy()) {
        sl_install_if_possible();
    } else if (GetModuleHandleW(L"sl.interposer.dll") != nullptr) {
        sl_install_if_possible();        // preloaded before us by the game
    }
    for (int i = 0; i < 120 && !sl_installed(); ++i) {
        sl_wait_for_interposer(500);     // signalled by hook_sl_plugin()
        if (GetModuleHandleW(L"sl.interposer.dll") != nullptr) { sl_install_if_possible(); }
    }
    if (!sl_installed()) {
        logf("loader: sl.interposer never appeared - the DLSS-G menu gate stays closed\n");
    }

    // 3. ReShade next: nothing needs it before the first frame.
    load_reshade();

    // 4. The upscaler runtimes (DLSS-SR) load after Streamline, and their Evaluate is
    //    only worth watching once they exist. This has to stay on THIS thread: a hook
    //    installation suspends every other thread (MinHook), so doing it from the
    //    present path can deadlock on a D3D12/DXGI lock - and patching code under a
    //    thread that is executing it is a crash. Low cadence, finite: 15 minutes.
    if (nvngx_installed()) {
        for (int i = 0; i < 900; ++i) {
            nvngx_scan_upscalers();
            Sleep(1000);
        }
    }
    logf("loader: done\n");
    return 0;
}

bool g_loader_started = false;

} // namespace

// Set by DllMain for the instances that only serve NGX exports (nvngx_dlssg.dll and
// friends). They must not install any hook: the proxy instance owns the single hook set.
bool g_exports_only = false;

void start_loader_thread()
{
    if (g_exports_only) { return; }
    if (g_loader_started) { return; }
    g_loader_started = true;
    HANDLE t = CreateThread(nullptr, 0, &loader_thread, nullptr, 0, nullptr);
    if (t != nullptr) { CloseHandle(t); }
    else { logf("loader: CreateThread failed (%lu)\n", GetLastError()); }
}

// ---------------------------------------------------------------- parameter block
// Minimal NVSDK_NGX_Parameter for when no real runtime exists. Only the
// capability queries a title makes before creating a feature have to work;
// everything else is stored and returned as-is.
namespace {

class ParameterBlock final : public NVSDK_NGX_Parameter
{
public:
    void Set(const char* n, unsigned long long v) override { set(n, v); }
    void Set(const char* n, float v) override              { set(n, bits_of(v)); }
    void Set(const char* n, double v) override             { set(n, bits_of(static_cast<float>(v))); }
    void Set(const char* n, unsigned int v) override       { set(n, v); }
    void Set(const char* n, int v) override                { set(n, static_cast<unsigned>(v)); }
    void Set(const char* n, ID3D11Resource*) override      { set(n, 0ull); }
    void Set(const char* n, ID3D12Resource* r) override    { set(n, reinterpret_cast<unsigned long long>(r)); }
    void Set(const char* n, void* p) override              { set(n, reinterpret_cast<unsigned long long>(p)); }

    NVSDK_NGX_Result Get(const char* n, unsigned long long* o) const override { return get(n, o, 8); }
    NVSDK_NGX_Result Get(const char* n, float* o) const override              { return get(n, o, 4); }
    NVSDK_NGX_Result Get(const char* n, double* o) const override {
        float f = 0.0f;
        const NVSDK_NGX_Result r = get(n, &f, 4);
        if (o != nullptr) { *o = f; }
        return r;
    }
    NVSDK_NGX_Result Get(const char* n, unsigned int* o) const override       { return get(n, o, 4); }
    NVSDK_NGX_Result Get(const char* n, int* o) const override {
        unsigned u = 0;
        const NVSDK_NGX_Result r = get(n, &u, 4);
        if (o != nullptr) { *o = static_cast<int>(u); }
        return r;
    }
    NVSDK_NGX_Result Get(const char* n, ID3D11Resource** o) const override {
        if (o != nullptr) { *o = nullptr; }
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** o) const override {
        unsigned long long v = 0;
        const NVSDK_NGX_Result r = get(n, &v, 8);
        if (o != nullptr) { *o = reinterpret_cast<ID3D12Resource*>(static_cast<uintptr_t>(v)); }
        return r;
    }
    NVSDK_NGX_Result Get(const char* n, void** o) const override {
        unsigned long long v = 0;
        const NVSDK_NGX_Result r = get(n, &v, 8);
        if (o != nullptr) { *o = reinterpret_cast<void*>(static_cast<uintptr_t>(v)); }
        return r;
    }

    void Reset() override { m_values.clear(); }

private:
    static unsigned long long bits_of(float f) {
        unsigned u = 0;
        memcpy(&u, &f, sizeof(u));
        return u;
    }
    void set(const char* n, unsigned long long v) {
        if (n != nullptr) { m_values[n] = v; }
    }
    NVSDK_NGX_Result get(const char* n, void* out, size_t bytes) const {
        if (n == nullptr || out == nullptr) { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
        const auto it = m_values.find(n);
        if (it == m_values.end()) { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
        memcpy(out, &it->second, (bytes > sizeof(it->second)) ? sizeof(it->second) : bytes);
        return NVSDK_NGX_Result_Success;
    }

    std::unordered_map<std::string, unsigned long long> m_values;
};

} // namespace

NVSDK_NGX_Parameter* new_parameter_block()
{
    auto* p = new (std::nothrow) ParameterBlock();
    if (p != nullptr) {
        // The three answers a menu is built from (FG_BACKEND.md 8).
        p->Set(NVSDK_NGX_Parameter_FrameGeneration_Available, 1);
        p->Set(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, 0);
        p->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, 1);
    }
    return p;
}

void destroy_parameter_block(NVSDK_NGX_Parameter* p)
{
    delete p;
}

} // namespace fakerfg

// ---------------------------------------------------------------- exports
// Resolved by export name, never by file name: the module is deployed as
// dxgi.dll / nvngx_dlssg.dll, so GetModuleHandle("fakerFG.dll") finds nothing.
extern "C" __declspec(dllexport) const MgpuFgApi* __cdecl MgpuFgGetApi(uint32_t min_version,
                                                                      uint32_t max_version)
{
    // The table is append-only, so a v1 consumer (one built before the sink existed) is
    // still served correctly: it checks `api->size >= sizeof(its own MgpuFgApi)` and never
    // reads past the fields it knows. Only a request whose whole range is newer than us
    // (min_version > current) or older than v1 is refused.
    if (min_version > MGPU_FG_API_VERSION || max_version < 1u) {
        fakerfg::logf("MgpuFgGetApi: range %u..%u excludes 1..%u\n",
                      min_version, max_version, MGPU_FG_API_VERSION);
        return nullptr;
    }
    return fakerfg::api_table();
}

// ---------------------------------------------------------------- lifecycle
BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        fakerfg::log_open();

        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(self, path, MAX_PATH);
        const wchar_t* base = wcsrchr(path, L'\\');
        base = (base != nullptr) ? base + 1 : path;
        fakerfg::logf("DllMain: attached as %ls, pid=%lu self=%p\n", base,
                      GetCurrentProcessId(), (void*)self);
        fakerfg::install_crash_log();

        // OptiScaler installs its global hook set once: the instance that acts as the API
        // proxy (`dxgi.dll`) owns kernel/GetProcAddress/Streamline/XeFG, while its
        // `nvngx_dlssg.dll` copy only serves the NGX exports - calls to those are routed
        // into the proxy instance by the GetProcAddress hook (`thunk_create`/`thunk_evaluate`
        // live there). Running the loader thread in BOTH instances means two MinHook sets
        // patching the same kernel32/Streamline addresses, whose trampolines jump into each
        // other: measured as 0xC00000FD (stack overflow) inside the nvngx_dlssg instance.
        const bool exports_only = (_wcsicmp(base, L"nvngx_dlssg.dll") == 0 ||
                                   _wcsicmp(base, L"nvngx_dlss.dll") == 0 ||
                                   _wcsicmp(base, L"_nvngx.dll") == 0 ||
                                   _wcsicmp(base, L"nvngx.dll") == 0);
        fakerfg::g_exports_only = exports_only;
        if (exports_only) {
            fakerfg::logf("DllMain: export-only instance - no hooks, no XeFG (the proxy"
                          " instance owns those)\n");
        } else {
            fakerfg::start_loader_thread();
        }

        // Two jobs need the earliest possible slot - the Streamline architecture
        // gate and ReShade - and neither may run under the loader lock, so they
        // happen on a thread started here (it sleeps 50 ms first).
    } else if (reason == DLL_PROCESS_DETACH) {
        // OptiScaler dllmain.cpp:2010-2045 does no cleanup here at all - its own comment is
        // "Unhooking and cleaning stuff causing issues during shutdown. Disabled for now".
        // Destroying the XeFG context from DllMain is measured as a crash inside
        // libxess_fg with the Steam overlay still resident:
        //     DllMain: detach / xefg[debug] Destroy Context called
        //     crash 0xC0000005 at gameoverlayrenderer64.dll+0x848f4
        // The process is going away; the driver reclaims everything.
        fakerfg::logf("DllMain: detach (no teardown, as OptiScaler)\n");
    }
    return TRUE;
}
