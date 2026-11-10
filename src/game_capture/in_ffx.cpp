// ============================================================================
// in_ffx.cpp - AMD FidelityFX Super Resolution input source (FSR 2.2 / 3.x)
//
// The add-on used to be NGX-only, which pinned the whole pipeline to an NVIDIA card
// running DLSS. FSR is the piece that lets the *game* render on any card (here: the
// AMD one) while XeSS-FG keeps doing the frame generation on the other GPU
// (docs/design/VENDOR_COMPAT_PLAN.md M6.4).
//
// What is hooked: the five ffx-api entry points a game uses when it links the FSR
// runtime DLL (amd_fidelityfx_upscaler_dx12.dll for FSR 3.1, amd_fidelityfx_dx12.dll
// for 2.2/3.0). The upscale dispatch description carries everything XeSS-FG needs,
// including a real camera - which DLSS-SR never gives us:
//
//   ffxDispatchDescUpscale {
//       depth, motionVectors,            -> FrameInputs.depth / .mv
//       jitterOffset, motionVectorScale, -> .jitter_* / .mv_scale_*
//       renderSize, upscaleSize,         -> .render_* / .output_*
//       cameraNear/Far/FovAngleVertical, -> .near/.far/.vfov + kFlagHasCamera
//       frameTimeDelta                   -> .frame_time_ms
//   }
//
// Like the NGX source this is an *observer*: every call is forwarded unchanged. That
// also means a game that runs its own FSR frame generation still works - we simply
// warn about it (three layers of interpolation is never what anyone wants).
//
// Headers: third_party/ffx/api/include/ffx_api/* (vendored from FidelityFX-SDK
// v1.1.4, MIT - see that folder's SOURCE.txt/LICENSE.txt). The classic per-effect
// headers in third_party/ffx/include are a *different* API and are not used here.
// ============================================================================
#include "inputs.h"

#include <tlhelp32.h>

#include <atomic>
#include <cwctype>
#include <string>

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/ffx_upscale.h>

#include "MinHook.h"

// One TU (unity.cpp) includes every source, so each file keeps its own named
// namespace: an anonymous one would merge and the state would collide.
namespace ffx_impl {

using namespace reshade::api;

typedef ffxReturnCode_t (*pfn_create)(ffxContext*, ffxCreateContextDescHeader*,
                                      const ffxAllocationCallbacks*);
typedef ffxReturnCode_t (*pfn_destroy)(ffxContext*, const ffxAllocationCallbacks*);
typedef ffxReturnCode_t (*pfn_configure)(ffxContext*, const ffxConfigureDescHeader*);
typedef ffxReturnCode_t (*pfn_query)(ffxContext*, ffxQueryDescHeader*);
typedef ffxReturnCode_t (*pfn_dispatch)(ffxContext*, const ffxDispatchDescHeader*);


// Set by ffxCreateContext for the upscale context: depth convention, MV units.
std::atomic<uint32_t> g_up_flags{ 0 };
std::atomic<uint32_t> g_max_out_w{ 0 };
std::atomic<uint32_t> g_max_out_h{ 0 };
std::atomic<bool>     g_logged_create{ false };

struct FfxFrame
{
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* mv = nullptr;
    uint32_t render_w = 0, render_h = 0;
    uint32_t output_w = 0, output_h = 0;
    float jitter_x = 0.0f, jitter_y = 0.0f;
    float mv_scale_x = 1.0f, mv_scale_y = 1.0f;
    float near_plane = 0.0f, far_plane = 0.0f, vfov_rad = 0.0f;
    float frame_time_ms = 0.0f;
    uint64_t serial = 0;
    uint64_t tick_ms = 0;   // GetTickCount64() when the API call happened
};

FfxFrame g_reported;
std::atomic<bool>     g_valid{ false };
uint64_t              g_last_serial = 0;   // last serial handed to the host (poll gate)
std::atomic<uint32_t> g_game_fg_calls{ 0 };
std::atomic<bool>     g_logged_first_frame{ false };


// --------------------------------------------------------- what a call means
//
// Split from the detours themselves: the same analysis runs no matter which module
// (loader or provider) the game happened to call.

void on_create_desc(const ffxCreateContextDescHeader* desc)
{
    if (!desc || desc->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) { return; }
    const auto* c = reinterpret_cast<const ffxCreateContextDescUpscale*>(desc);
    g_up_flags.store(c->flags, std::memory_order_relaxed);
    g_max_out_w.store(c->maxUpscaleSize.width, std::memory_order_relaxed);
    g_max_out_h.store(c->maxUpscaleSize.height, std::memory_order_relaxed);
    if (!g_logged_create.exchange(true)) {
        mgpu::input::logf("ffx: upscale context created (flags=0x%X maxRender=%ux%u"
                          " maxUpscale=%ux%u)\n",
                          c->flags, c->maxRenderSize.width, c->maxRenderSize.height,
                          c->maxUpscaleSize.width, c->maxUpscaleSize.height);
    }
}

void on_dispatch_desc(const ffxDispatchDescHeader* desc)
{
    if (!desc) { return; }

    if (desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        const auto* d = reinterpret_cast<const ffxDispatchDescUpscale*>(desc);

        auto* depth = static_cast<ID3D12Resource*>(d->depth.resource);
        auto* mv = static_cast<ID3D12Resource*>(d->motionVectors.resource);
        if (!depth) { return; }

        g_reported.depth      = depth;
        g_reported.mv         = mv;
        g_reported.render_w   = d->renderSize.width;
        g_reported.render_h   = d->renderSize.height;
        g_reported.output_w   = d->upscaleSize.width ? d->upscaleSize.width
                                                     : g_max_out_w.load();
        g_reported.output_h   = d->upscaleSize.height ? d->upscaleSize.height
                                                      : g_max_out_h.load();
        g_reported.jitter_x   = d->jitterOffset.x;
        g_reported.jitter_y   = d->jitterOffset.y;
        g_reported.mv_scale_x = d->motionVectorScale.x;
        g_reported.mv_scale_y = d->motionVectorScale.y;
        g_reported.near_plane = d->cameraNear;
        g_reported.far_plane  = d->cameraFar;
        g_reported.vfov_rad   = d->cameraFovAngleVertical;
        g_reported.frame_time_ms = d->frameTimeDelta;
        g_reported.tick_ms = GetTickCount64();
    ++g_reported.serial;

        if (!g_logged_first_frame.exchange(true)) {
            mgpu::input::logf("ffx: first frame: render=%ux%u out=%ux%u jitter=(%.4f, %.4f)"
                              " mvscale=(%.1f, %.1f) camera=(near %.3f far %.1f vfov %.4f)"
                              " dt=%.2fms\n",
                              d->renderSize.width, d->renderSize.height,
                              g_reported.output_w, g_reported.output_h,
                              d->jitterOffset.x, d->jitterOffset.y,
                              d->motionVectorScale.x, d->motionVectorScale.y,
                              d->cameraNear, d->cameraFar, d->cameraFovAngleVertical,
                              d->frameTimeDelta);
        }

        // FSR hands the game's own resources to the runtime: the depth is bound as a
        // depth target / depth SRV and the vectors as a pixel shader resource. The
        // barrier tracker learns the real state from the game's own barriers and
        // replaces these guesses immediately.
        mgpu::input::track(mv, depth, resource_usage::shader_resource,
                           resource_usage::depth_stencil);
        mgpu::input::hold(depth);
        mgpu::input::hold(mv);
        g_valid.store(true, std::memory_order_release);
    } else if (desc->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION ||
               desc->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE) {
        // The game runs its own FSR frame generation. Interpolating that again would
        // be a third layer; say it once, loudly, and keep forwarding.
        if (g_game_fg_calls.fetch_add(1) == 0) {
            mgpu::input::logf("ffx: the game dispatches its OWN frame generation"
                              " (ffxDispatchDescFrameGeneration) - turn in-game FG"
                              " off, it competes with the bridge\n");
        }
    }
}


// ------------------------------------------------- per-module detour thunks
//
// The game may go through amd_fidelityfx_loader_dx12.dll (which forwards to a
// provider) or call the provider directly - PBZ loads both. MinHook hands back one
// trampoline per hook site, so each hooked module gets its own fixed-index thunk that
// forwards to *its* original; a single shared detour would have to guess the caller.

constexpr int kMaxModules = 4;

typedef ffxReturnCode_t (*pfn_create)(ffxContext*, ffxCreateContextDescHeader*,
                                      const ffxAllocationCallbacks*);
typedef ffxReturnCode_t (*pfn_destroy)(ffxContext*, const ffxAllocationCallbacks*);
typedef ffxReturnCode_t (*pfn_configure)(ffxContext*, const ffxConfigureDescHeader*);
typedef ffxReturnCode_t (*pfn_query)(ffxContext*, ffxQueryDescHeader*);
typedef ffxReturnCode_t (*pfn_dispatch)(ffxContext*, const ffxDispatchDescHeader*);

struct Slot
{
    HMODULE        module = nullptr;
    std::wstring   path;
    pfn_create     create = nullptr;
    pfn_destroy    destroy = nullptr;
    pfn_configure  configure = nullptr;
    pfn_query      query = nullptr;
    pfn_dispatch   dispatch = nullptr;
};

Slot             g_slots[kMaxModules];
std::atomic<int> g_slot_count{ 0 };

template <int N>
ffxReturnCode_t create_thunk(ffxContext* c, ffxCreateContextDescHeader* d,
                             const ffxAllocationCallbacks* cb)
{
    on_create_desc(d);
    return g_slots[N].create(c, d, cb);
}

template <int N>
ffxReturnCode_t dispatch_thunk(ffxContext* c, const ffxDispatchDescHeader* d)
{
    on_dispatch_desc(d);
    return g_slots[N].dispatch(c, d);
}

template <int N>
ffxReturnCode_t query_thunk(ffxContext* c, ffxQueryDescHeader* d)
{
    return g_slots[N].query(c, d);
}

template <int N>
ffxReturnCode_t configure_thunk(ffxContext* c, const ffxConfigureDescHeader* d)
{
    return g_slots[N].configure(c, d);
}

template <int N>
ffxReturnCode_t destroy_thunk(ffxContext* c, const ffxAllocationCallbacks* cb)
{
    return g_slots[N].destroy(c, cb);
}

struct Thunks { LPVOID create, dispatch, query, configure, destroy; };

const Thunks kThunks[kMaxModules] = {
    { reinterpret_cast<LPVOID>(&create_thunk<0>), reinterpret_cast<LPVOID>(&dispatch_thunk<0>),
      reinterpret_cast<LPVOID>(&query_thunk<0>), reinterpret_cast<LPVOID>(&configure_thunk<0>),
      reinterpret_cast<LPVOID>(&destroy_thunk<0>) },
    { reinterpret_cast<LPVOID>(&create_thunk<1>), reinterpret_cast<LPVOID>(&dispatch_thunk<1>),
      reinterpret_cast<LPVOID>(&query_thunk<1>), reinterpret_cast<LPVOID>(&configure_thunk<1>),
      reinterpret_cast<LPVOID>(&destroy_thunk<1>) },
    { reinterpret_cast<LPVOID>(&create_thunk<2>), reinterpret_cast<LPVOID>(&dispatch_thunk<2>),
      reinterpret_cast<LPVOID>(&query_thunk<2>), reinterpret_cast<LPVOID>(&configure_thunk<2>),
      reinterpret_cast<LPVOID>(&destroy_thunk<2>) },
    { reinterpret_cast<LPVOID>(&create_thunk<3>), reinterpret_cast<LPVOID>(&dispatch_thunk<3>),
      reinterpret_cast<LPVOID>(&query_thunk<3>), reinterpret_cast<LPVOID>(&configure_thunk<3>),
      reinterpret_cast<LPVOID>(&destroy_thunk<3>) },
};


// ------------------------------------------------------------ hook installation

bool is_ffx_module(const wchar_t* path)
{
    // FSR 2.2/3.0 merged entry point, the FSR 3.1 upscaler, and the loader that
    // forwards to either.
    static const wchar_t* kNames[] = {
        L"amd_fidelityfx_upscaler_dx12.dll",
        L"amd_fidelityfx_dx12.dll",
        L"amd_fidelityfx_loader_dx12.dll",
    };
    std::wstring low(path);
    for (wchar_t& c : low) { c = static_cast<wchar_t>(towlower(c)); }
    for (const wchar_t* n : kNames) {
        if (low.find(n) != std::wstring::npos) { return true; }
    }
    return false;
}

bool install(HMODULE m, const std::wstring& path)
{
    if (!mgpu::input::ensure_minhook()) { return false; }

    const int slot = g_slot_count.load();
    if (slot >= kMaxModules) {
        mgpu::input::logf("ffx: no free slot for %ls (max %d modules)\n",
                          path.c_str(), kMaxModules);
        return false;
    }
    g_slots[slot].module = m;
    g_slots[slot].path = path;

    struct Target { const char* name; LPVOID detour; LPVOID* original; };
    Target targets[] = {
        { "ffxCreateContext",  kThunks[slot].create,
          reinterpret_cast<LPVOID*>(&g_slots[slot].create) },
        { "ffxDispatch",       kThunks[slot].dispatch,
          reinterpret_cast<LPVOID*>(&g_slots[slot].dispatch) },
        { "ffxQuery",          kThunks[slot].query,
          reinterpret_cast<LPVOID*>(&g_slots[slot].query) },
        { "ffxConfigure",      kThunks[slot].configure,
          reinterpret_cast<LPVOID*>(&g_slots[slot].configure) },
        { "ffxDestroyContext", kThunks[slot].destroy,
          reinterpret_cast<LPVOID*>(&g_slots[slot].destroy) },
    };

    bool any = false;
    for (auto& t : targets) {
        LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(m, t.name));
        if (!addr) { continue; }
        if (MH_CreateHook(addr, t.detour, t.original) != MH_OK ||
            MH_EnableHook(addr) != MH_OK) {
            mgpu::input::logf("ffx: hook %s FAILED\n", t.name);
            continue;
        }
        any = true;
    }

    if (!any) { return false; }

    g_slot_count.store(slot + 1);

    // Pin the module: a detour that outlives its module would jump into freed code
    // the moment the game unloads the upscaler (M2 saw titles load, query and unload
    // a copy of the DLSS runtime).
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
                       GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(m), &pinned);
    mgpu::input::logf("ffx: %ls hooked (ffx-api, slot %d)\n", path.c_str(), slot);
    return true;
}

// ------------------------------------------------------------------- the source

class FfxSource final : public mgpu::input::Source
{
public:
    const char* name() const override { return "ffx"; }
    uint32_t api_id() const override { return mgpu::kInputApiFfx; }

    void on_module(HMODULE module) override
    {
        if (!module) { return; }
        wchar_t full[MAX_PATH]{};
        if (!GetModuleFileNameW(module, full, MAX_PATH)) { return; }
        if (!is_ffx_module(full)) { return; }

        // Both the loader and the provider may be loaded (PBZ ships both). Hook each
        // one once: the game may go through either, and a thunk per module keeps the
        // right original function attached to the right module.
        const int n = g_slot_count.load();
        for (int i = 0; i < n; ++i) {
            if (g_slots[i].module == module) { return; }
        }
        install(module, full);
    }

    void scan_existing() override
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE) { return; }
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                if (is_ffx_module(me.szModule)) { on_module(me.hModule); }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }

    bool poll(mgpu::input::FrameInputs& out) override
    {
        if (!g_valid.load(std::memory_order_acquire)) { return false; }
        const FfxFrame f = g_reported;
        // Same rule as the NGX source: only a newer dispatch counts as a frame. When
        // the game switches away from FSR its serial freezes and this reports false,
        // so the host stops touching textures that are being destroyed.
        if (f.serial == g_last_serial) { return false; }
        g_last_serial = f.serial;
        const uint32_t up = g_up_flags.load(std::memory_order_relaxed);

        out.mv         = f.mv;
        out.depth      = f.depth;
        out.render_w   = f.render_w;
        out.render_h   = f.render_h;
        out.output_w   = f.output_w;
        out.output_h   = f.output_h;
        out.jitter_x   = f.jitter_x;
        out.jitter_y   = f.jitter_y;
        out.mv_scale_x = f.mv_scale_x;
        out.mv_scale_y = f.mv_scale_y;

        // Reversed-Z games (PBZ is one) fill the two distances the other way round:
        // the depth that is 1.0 at the near plane makes them pass
        // cameraNear = +inf / FLT_MAX and cameraFar = the real near distance (10 m).
        // Swap before the sanity check, otherwise a perfectly good camera would be
        // thrown away - which is exactly what happened on the first FSR run.
        float near_p = f.near_plane;
        float far_p  = f.far_plane;
        if ((up & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) && far_p < near_p) {
            const float t = near_p;
            near_p = far_p;
            far_p  = t;
        }
        out.near_plane = near_p;
        out.far_plane  = far_p;
        out.vfov_rad   = f.vfov_rad;

        uint32_t flags = 0;
        if (near_p > 0.0f && near_p < 1.0e6f && far_p > near_p &&
            f.vfov_rad > 0.05f && f.vfov_rad < 2.5f) {
            flags |= mgpu::kInputFlagHasCamera;
        }
        if (up & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) {
            flags |= mgpu::kInputFlagInvertedDepth;
        }
        if (up & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) {
            flags |= mgpu::kInputFlagHiResMv;
        }
        if (!(up & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION)) {
            flags |= mgpu::kInputFlagMvJittered;
        }
        if (up & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE) {
            flags |= mgpu::kInputFlagHdr;
        }
        out.flags  = flags;
        out.serial = f.serial;
        out.tick_ms = f.tick_ms;
        // ---- normalized semantics -------------------------------------------------
        // FSR 2.2/3.x is the only source that exports a real camera (vertical FOV, near,
        // far). Its velocities are UV-space and motionVectorScale converts them to pixels,
        // so the unit is declared. The sign is not, so it stays unknown.
        out.mv_unit        = mgpu::input::MvUnit::uv;
        // All of these APIs define their vectors as previous-minus-current (a game that
        // flipped it would break its own upscaler); MvFlip is the explicit escape hatch.
        out.mv_sign        = mgpu::input::MvSign::prev_minus_cur;
        out.camera_source  = (flags & mgpu::kInputFlagHasCamera)
                                 ? mgpu::input::CameraSource::declared
                                 : mgpu::input::CameraSource::none;
        out.invalid_marker = 0.0f;
        out.valid_w = f.render_w;
        out.valid_h = f.render_h;
        return true;
    }
};

FfxSource g_ffx_source;

} // namespace ffx_impl

mgpu::input::Source* mgpu::input::make_ffx_source()
{
    return &ffx_impl::g_ffx_source;
}
