// ============================================================================
// in_ngx.cpp - NVIDIA DLSS-SR input source (NVSDK_NGX_EvaluateFeature hook)
//
// This is the code that was inline in game_capture.cpp until M6.3, unchanged in
// behaviour, now behind mgpu::input::Source. It is what gives us motion vectors,
// depth, jitter, render size and the MV scale - the inputs XeSS-FG needs - on any
// card whose game runs DLSS.
//
// Two things from docs/findings/milestones/M2_RESULTS.md are load-bearing and must not be "cleaned up":
//   * CP2077 loads several nvngx_dlss.dll copies just to read their version; the module that
//     actually executes DLSS is the driver-store shim `_nvngx.dll`. Since 2026-11-09 the
//     filter also accepts a game-local `nvngx_dlss.dll` **if it exports the NGX entry
//     points**, because some engines (RE Engine / Streamline: PRAGMATA) call the runtime
//     directly from the game folder - with the old shim-only rule those titles produced no
//     data at all.
//   * The parameters are read but never modified, and the original function is
//     always called: we are an observer here, not a replacement (that is
//     OptiScaler's job, and it is a different architecture - see
//     docs/design/VENDOR_COMPAT_PLAN.md 0.4).
// ============================================================================
#include "inputs.h"

#include <tlhelp32.h>

#include <atomic>
#include <string>

#include "MinHook.h"
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_params.h"

// One TU (unity.cpp) includes every source, so each file keeps its own named
// namespace: an anonymous one would merge and the state would collide.
namespace ngx_impl {

using namespace reshade::api;

// ------------------------------------------------------------ NGX function types

typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_evaluate_t)(
    ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_create_t)(
    ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);

pfn_evaluate_t g_real_evaluate = nullptr;
pfn_create_t   g_real_create = nullptr;
// True once one provider's hooks are live. The driver shim `_nvngx.dll` forwards its
// exports to the runtime we already hooked, so a second attempt lands on the same
// (already trampolined) addresses and MinHook fails - which used to print a scary
// "NGX hooks FAILED" on RE Engine titles (PRAGMATA, 2026-11-09) although the first hook
// was live and working. One working provider is enough.
bool g_ngx_hooked = false;

// What the NGX hook saw for the most recent frame. Written by the game thread inside
// NGX, copied out by poll() (which runs on the ReShade thread) - that copy *is* the
// per-frame snapshot, same as the old g_ngx_reported/g_ngx_consumed pair.
struct NgxFrame
{
    ID3D12Resource* mv = nullptr;
    ID3D12Resource* depth = nullptr;
    float  jitter_x = 0.0f;
    float  jitter_y = 0.0f;
    UINT   render_w = 0;
    UINT   render_h = 0;
    float  mv_scale_x = 0.0f;
    float  mv_scale_y = 0.0f;
    UINT64 serial = 0;
    uint64_t tick_ms = 0;   // GetTickCount64() when the API call happened
};

NgxFrame g_reported;
std::atomic<bool> g_valid{ false };
uint64_t g_last_serial = 0;          // last serial handed to the host (poll gate)
HMODULE g_module = nullptr;

// What the game declared about its inputs when it created the DLSS feature
// (NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags):
//   MVLowRes      - the vectors are at the (lower) render resolution, not the display one
//   MVJittered    - the vectors were computed on the jittered render, so the frame's
//                   jitter delta is still inside them
//   DepthInverted - reversed Z
// OptiScaler reads the same flags and forwards them to its XeFG output
// (OptiScaler/upscalers/IFeature.cpp:42-72 -> framegen/xefg/XeFG_Dx12.cpp:359-382).
// We forward MVJittered (and the high-res-MV hint) only: MVJittered is what
// XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV - "remove jitter from input velocity" - consumes,
// and the old hardcoded `flags = 0` is what made UE5 titles look like they had broken
// vectors (docs/findings/UE5_MV_OPTISCALER.md 5.1).
// 2026-11-09 measurement, PBZ: it declares 0x4B = IsHDR | MVLowRes | DepthInverted |
// AutoExposure, i.e. **MVJittered = 0** - so the jitter hypothesis does not apply there,
// and forcing the flag only made things worse. DepthInverted is forwarded now (it was the
// part that was genuinely missing); MVJittered stays forwarded for the titles that declare
// it, which is what OptiScaler does.
std::atomic<int>  g_feature_flags{ 0 };
std::atomic<bool> g_feature_flags_known{ false };

float read_float(const NVSDK_NGX_Parameter* p, const char* name, bool* ok)
{
    float v = 0.0f;
    if (p && p->Get(name, &v) == NVSDK_NGX_Result_Success) { if (ok) { *ok = true; } return v; }
    if (ok) { *ok = false; }
    return 0.0f;
}

unsigned read_uint(const NVSDK_NGX_Parameter* p, const char* name, bool* ok)
{
    unsigned v = 0;
    if (p && p->Get(name, &v) == NVSDK_NGX_Result_Success) { if (ok) { *ok = true; } return v; }
    if (ok) { *ok = false; }
    return 0u;
}

ID3D12Resource* read_resource(const NVSDK_NGX_Parameter* p, const char* name)
{
    ID3D12Resource* r = nullptr;
    if (p && p->Get(name, &r) == NVSDK_NGX_Result_Success) { return r; }
    return nullptr;
}

// ------------------------------------------------------------------ NGX hooks

NVSDK_NGX_Result NVSDK_CONV hook_evaluate(
    ID3D12GraphicsCommandList* cmd_list,
    const NVSDK_NGX_Handle* feature_handle,
    const NVSDK_NGX_Parameter* parameters,
    void* callback)
{
    bool ok_w = false, ok_h = false, ok_jx = false, ok_jy = false;

    ID3D12Resource* mv = read_resource(parameters, NVSDK_NGX_Parameter_MotionVectors);
    ID3D12Resource* dp = read_resource(parameters, NVSDK_NGX_Parameter_Depth);

    if (mv && dp) {
        g_reported.mv    = mv;
        g_reported.depth = dp;

        // Start watching exactly these two resources, and nothing else: watching
        // every barrier starved the render threads on the loading screen (see the
        // comment in game_capture.cpp next to the tracker).
        mgpu::input::track(mv, dp, resource_usage::shader_resource,
                           resource_usage::depth_stencil);

        mgpu::input::hold(mv);
        mgpu::input::hold(dp);
        g_reported.jitter_x = read_float(parameters, NVSDK_NGX_Parameter_Jitter_Offset_X, &ok_jx);
        g_reported.jitter_y = read_float(parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y, &ok_jy);
        g_reported.render_w = read_uint(parameters, NVSDK_NGX_Parameter_Width, &ok_w);
        g_reported.render_h = read_uint(parameters, NVSDK_NGX_Parameter_Height, &ok_h);
        bool ok_msx = false, ok_msy = false;
        g_reported.mv_scale_x = read_float(parameters, NVSDK_NGX_Parameter_MV_Scale_X, &ok_msx);
        g_reported.mv_scale_y = read_float(parameters, NVSDK_NGX_Parameter_MV_Scale_Y, &ok_msy);
        g_reported.tick_ms = GetTickCount64();
    ++g_reported.serial;
        g_valid.store(true, std::memory_order_release);
        (void)feature_handle;
    }

    return g_real_evaluate(cmd_list, feature_handle, parameters, callback);
}

NVSDK_NGX_Result NVSDK_CONV hook_create(
    ID3D12GraphicsCommandList* cmd_list,
    NVSDK_NGX_Feature feature_id,
    NVSDK_NGX_Parameter* parameters,
    NVSDK_NGX_Handle** out_handle)
{
    // The game states here what its motion vectors are; the evaluator does not repeat it.
    // Read it once per Create so an in-game upscaler switch is followed as well.
    int flags = 0;
    if (parameters != nullptr &&
        parameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags) ==
            NVSDK_NGX_Result_Success) {
        const int prev = g_feature_flags.exchange(flags);
        if (!g_feature_flags_known.exchange(true) || prev != flags) {
            mgpu::input::logf("dlss feature flags 0x%X: MVJittered=%d MVLowRes=%d DepthInverted=%d"
                              " (feature %d)\n",
                              static_cast<unsigned>(flags),
                              (flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) ? 1 : 0,
                              (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) ? 1 : 0,
                              (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) ? 1 : 0,
                              static_cast<int>(feature_id));
        }
    }

    return g_real_create(cmd_list, feature_id, parameters, out_handle);
}

bool install_ngx_hooks(HMODULE ngx)
{
    if (!mgpu::input::ensure_minhook()) { return false; }

    struct Target { const char* name; LPVOID detour; LPVOID* original; };
    Target targets[] = {
        { "NVSDK_NGX_D3D12_EvaluateFeature",   reinterpret_cast<LPVOID>(&hook_evaluate),
          reinterpret_cast<LPVOID*>(&g_real_evaluate) },
        { "NVSDK_NGX_D3D12_CreateFeature",     reinterpret_cast<LPVOID>(&hook_create),
          reinterpret_cast<LPVOID*>(&g_real_create) },
    };

    bool any = false;
    for (auto& t : targets) {
        LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(ngx, t.name));
        if (!addr) { continue; }
        if (MH_CreateHook(addr, t.detour, t.original) != MH_OK ||
            MH_EnableHook(addr) != MH_OK) {
            mgpu::input::logf("hook %s FAILED\n", t.name);
            continue;
        }
        mgpu::input::logf("hooked %s @ %p\n", t.name, addr);
        any = true;
    }
    return any;
}

void hook_module_if_ngx(HMODULE m)
{
    if (!m || g_module == m) { return; }
    // Already live through another module - see g_ngx_hooked.
    if (g_ngx_hooked) { return; }

    wchar_t full[MAX_PATH]{};
    if (!GetModuleFileNameW(m, full, MAX_PATH)) { return; }

    // NGX entry points are exported by TWO kinds of module:
    //   * the driver shim `_nvngx.dll` (driver store) - what CP2077 ends up calling, and
    //   * the DLSS runtime itself, `nvngx_dlss.dll`, which some engines load from the game
    //     folder and call directly (RE Engine / Streamline: PRAGMATA exports
    //     NVSDK_NGX_D3D12_{Create,Evaluate}Feature from its own 44 MB copy).
    // The old rule matched the shim's file name only, so PRAGMATA produced no data at all
    // (add-on loaded by ReShade, "Registered add-on game_capture", but nothing ever hooked).
    // Match on the file name and then *verify* the export, which keeps the filter honest
    // for the version-probing copies CP2077 loads (they never export it).
    const wchar_t* base = wcsrchr(full, L'\\');
    base = (base != nullptr) ? base + 1 : full;
    if (wcsstr(base, L"nvngx") == nullptr) { return; }
    // Frame generation / ray reconstruction / NR runtimes: not our input path.
    if (wcsstr(base, L"dlssg") || wcsstr(base, L"dlssd") || wcsstr(base, L"dlssnr")) { return; }
    if (GetProcAddress(m, "NVSDK_NGX_D3D12_EvaluateFeature") == nullptr) { return; }

    mgpu::input::logf("\nNGX provider: %ls @ %p\n", full, (void*)m);
    if (install_ngx_hooks(m)) {
        g_module = m;
        g_ngx_hooked = true;
        mgpu::input::logf("NGX hooks active\n");
    } else {
        // Not fatal any more: either another provider is already live (handled above) or
        // this module's exports were forwarded/blocked. Keep going quietly.
        mgpu::input::logf("NGX hook attempt failed, ignored (module %ls)\n", base);
    }
}

// ------------------------------------------------------------------- the source

class NgxSource final : public mgpu::input::Source
{
public:
    const char* name() const override { return "ngx"; }
    uint32_t api_id() const override { return mgpu::kInputApiNgx; }

    void on_module(HMODULE module) override { hook_module_if_ngx(module); }

    void scan_existing() override
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE) { return; }
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                if (wcsstr(me.szModule, L"nvngx")) { hook_module_if_ngx(me.hModule); }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }

    bool poll(mgpu::input::FrameInputs& out) override
    {
        if (!g_valid.load(std::memory_order_acquire)) { return false; }

        // Plain copy of the hook's record: NGX hands the same textures every frame and
        // only the jitter/size/serial change, which is exactly what the old
        // g_ngx_consumed snapshot did.
        const NgxFrame f = g_reported;

        // "false = nothing new" has to mean it: once the game stops calling
        // EvaluateFeature (it switched to FSR, or destroyed the context) the serial
        // freezes, and re-reporting the same textures would keep the host copying
        // from resources that no longer exist.
        if (f.serial == g_last_serial) { return false; }
        g_last_serial = f.serial;

        out.mv         = f.mv;
        out.depth      = f.depth;
        out.jitter_x   = f.jitter_x;
        out.jitter_y   = f.jitter_y;
        out.render_w   = f.render_w;
        out.render_h   = f.render_h;
        out.mv_scale_x = f.mv_scale_x;
        out.mv_scale_y = f.mv_scale_y;
        // NGX does not export the camera (docs/findings/milestones/M2_RESULTS.md 4); the bridge keeps
        // using --fg-fov for these games.
        out.near_plane = 0.0f;
        out.far_plane  = 0.0f;
        out.vfov_rad   = 0.0f;
        // What the game declared at DLSS CreateFeature time (see g_feature_flags). Until a
        // Create has been seen, keep publishing 0 - a guess would silently change the
        // library's behaviour on engines that never tell us anything.
        out.flags = 0;
        if (g_feature_flags_known.load(std::memory_order_acquire)) {
            const int ff = g_feature_flags.load(std::memory_order_acquire);
            // "the vectors still carry the frame's jitter" -> the bridge turns that into
            // XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV ("remove jitter from input velocity").
            if (ff & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) {
                out.flags |= mgpu::kInputFlagMvJittered;
            }
            // MVLowRes says "the vectors are at render resolution"; our kInputFlagHiResMv
            // means the opposite, so it is set exactly when the game says it is NOT low res.
            // Nothing consumes it yet (XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV is deprecated),
            // it is published so a future build can act on it.
            if ((ff & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) == 0) {
                out.flags |= mgpu::kInputFlagHiResMv;
            }
            // DepthInverted is the game's own statement about its depth buffer (reversed-Z,
            // which is nearly every UE5 title and Cyberpunk). Telling XeFG the opposite sign
            // makes its disocclusion test look the wrong way round, and it costs nothing to
            // pass it on - the bridge turns it into XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH.
            // Measured on PBZ 2026-11-09 (declares 0x4B = IsHDR|MVLowRes|DepthInverted|
            // AutoExposure): forcing it changed nothing visible, so this is correctness, not
            // a fix for the model-edge artifacts.
            if (ff & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) {
                out.flags |= mgpu::kInputFlagInvertedDepth;
            }
        }
        out.serial = f.serial;
        out.tick_ms = f.tick_ms;
        return true;
    }
};

NgxSource g_ngx_source;

} // namespace ngx_impl

mgpu::input::Source* mgpu::input::make_ngx_source()
{
    return &ngx_impl::g_ngx_source;
}
