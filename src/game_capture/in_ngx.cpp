// ============================================================================
// in_ngx.cpp - NVIDIA DLSS-SR input source (NVSDK_NGX_EvaluateFeature hook)
//
// This is the code that was inline in game_capture.cpp until M6.3, unchanged in
// behaviour, now behind mgpu::input::Source. It is what gives us motion vectors,
// depth, jitter, render size and the MV scale - the inputs XeSS-FG needs - on any
// card whose game runs DLSS.
//
// Two things from docs/findings/milestones/M2_RESULTS.md are load-bearing and must not be "cleaned up":
//   * CP2077 loads several nvngx_dlss.dll copies just to read their version; the
//     module that actually executes DLSS is the driver-store shim `_nvngx.dll`, so
//     only that one is instrumented.
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

    wchar_t full[MAX_PATH]{};
    if (!GetModuleFileNameW(m, full, MAX_PATH)) { return; }
    if (!wcsstr(full, L"_nvngx.dll")) { return; }   // driver shim only, per M2

    g_module = m;
    mgpu::input::logf("\nNGX provider: %ls @ %p\n", full, (void*)m);
    if (install_ngx_hooks(m)) { mgpu::input::logf("NGX hooks active\n"); }
    else { mgpu::input::logf("NGX hooks FAILED\n"); }
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
        // DLSS-SR motion vectors are UV-space, not jittered, and the depth is handed
        // over exactly as the game wrote it.
        out.flags  = 0;
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
