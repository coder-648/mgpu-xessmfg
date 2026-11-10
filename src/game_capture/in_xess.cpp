// ============================================================================
// in_xess.cpp - Intel XeSS input source (xessD3D12Execute hook)
//
// Third and last upscaler a game can use. XeSS runs on any SM 6.4 GPU (so an AMD card
// renders with it just fine - PBZ even ships the plugin), which makes it a useful
// fallback when a title offers no FSR but does offer XeSS.
//
// What the execute params give us (third_party/xess/inc/xess/xess_d3d12.h):
//   pDepthTexture / pVelocityTexture / jitterOffsetX,Y / inputWidth,Height / resetHistory
// and from xessD3D12Init: initFlags (HIGH_RES_MV, INVERTED_DEPTH, USE_NDC_VELOCITY,
// JITTERED_MV, LDR_INPUT_COLOR) plus outputResolution.
//
// XeSS does NOT export a camera, so this source never sets kInputFlagHasCamera and the
// bridge keeps using its synthetic projection (--fg-fov) for these games.
//
// Like the other two sources this is an observer: every call is forwarded unchanged.
// ============================================================================
#include "inputs.h"

#include <tlhelp32.h>

#include <atomic>
#include <cwctype>
#include <string>

#include "MinHook.h"
#include "xess/xess.h"
#include "xess/xess_d3d12.h"

namespace xess_impl {

using namespace reshade::api;

typedef xess_result_t (*pfn_create)(ID3D12Device*, xess_context_handle_t*);
typedef xess_result_t (*pfn_build)(xess_context_handle_t, ID3D12PipelineLibrary*, bool,
                                   uint32_t);
typedef xess_result_t (*pfn_init)(xess_context_handle_t, const xess_d3d12_init_params_t*);
typedef xess_result_t (*pfn_execute)(xess_context_handle_t, ID3D12GraphicsCommandList*,
                                     const xess_d3d12_execute_params_t*);

struct Slot
{
    HMODULE      module = nullptr;
    std::wstring path;
    pfn_create   create = nullptr;
    pfn_build    build = nullptr;
    pfn_init     init = nullptr;
    pfn_execute  execute = nullptr;
};

constexpr int kMaxModules = 2;
Slot             g_slots[kMaxModules];
std::atomic<int> g_slot_count{ 0 };

struct XessFrame
{
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* mv = nullptr;
    uint32_t render_w = 0, render_h = 0;
    uint32_t output_w = 0, output_h = 0;
    float jitter_x = 0.0f, jitter_y = 0.0f;
    float mv_scale_x = 1.0f, mv_scale_y = 1.0f;
    float frame_time_ms = 0.0f;
    uint64_t serial = 0;
    uint64_t tick_ms = 0;   // GetTickCount64() when the API call happened
};

XessFrame             g_reported;
std::atomic<bool>     g_valid{ false };
uint64_t              g_last_serial = 0;      // poll gate, same as the other sources
std::atomic<uint32_t> g_init_flags{ 0 };
std::atomic<uint32_t> g_out_w{ 0 }, g_out_h{ 0 };
std::atomic<bool>     g_logged_init{ false };
std::atomic<bool>     g_logged_exec{ false };

// What an execute means, independent of which module called it.
void on_execute(const xess_d3d12_execute_params_t* p)
{
    if (!p) { return; }
    auto* depth = static_cast<ID3D12Resource*>(p->pDepthTexture);
    auto* mv = static_cast<ID3D12Resource*>(p->pVelocityTexture);
    // Trace the entry point: PBZ was seen calling xessD3D12Init but never producing a
    // frame, and a silent `return` here would look exactly like "execute is never called".
    static std::atomic<uint32_t> calls{ 0 };
    const uint32_t n = ++calls;
    // HIGH_RES_MV mode makes the depth texture OPTIONAL (xess_d3d12.h: "Optional depth
    // texture. Required if XESS_INIT_FLAG_HIGH_RES_MV has not been specified"). PBZ leaves
    // it null and the frame is perfectly valid - the old `if (!depth) return;` threw those
    // frames away, which is why the XeSS mode produced nothing at all.
    if (!mv && !depth) {
        if (n == 1 || (n % 600) == 0) {
            mgpu::input::logf("xess: execute#%u has neither depth nor motion vectors - skipped\n", n);
        }
        return;
    }
    if (n == 1) {
        mgpu::input::logf("xess: first execute render=%ux%u output=%ux%u depth=%p mv=%p"
                          " reset=%u %s\n",
                          p->inputWidth, p->inputHeight, g_out_w.load(), g_out_h.load(),
                          (void*)depth, (void*)mv, static_cast<unsigned>(p->resetHistory),
                          depth ? "" : "(HIGH_RES_MV: no depth, publishing MV only)");
    }

    const uint32_t flags = g_init_flags.load(std::memory_order_relaxed);
    const bool hi_res_mv = (flags & XESS_INIT_FLAG_HIGH_RES_MV) != 0;
    const bool ndc_mv = (flags & XESS_INIT_FLAG_USE_NDC_VELOCITY) != 0;

    g_reported.depth = depth;
    g_reported.mv = mv;
    // The content size is the *render* resolution (inputWidth/Height): that is what the
    // depth stream is, and both streams must agree or XeFG rejects the tags. With
    // HIGH_RES_MV the velocity texture is output-sized instead - the downsample pass
    // rescales it (it samples with an SRV, so size does not have to match), which is why
    // the reported size stays the render size here.
    g_reported.render_w = p->inputWidth;
    g_reported.render_h = p->inputHeight;
    g_reported.output_w = g_out_w.load();
    g_reported.output_h = g_out_h.load();
    g_reported.jitter_x = p->jitterOffsetX;
    g_reported.jitter_y = p->jitterOffsetY;
    // XeSS MVs are pixel offsets, or NDC when USE_NDC_VELOCITY is set: the consumer then
    // needs (±w/2, ∓h/2) to get back to pixels (OptiScaler maps it the same way).
    g_reported.mv_scale_x = ndc_mv ? (static_cast<float>(g_reported.render_w) * 0.5f) : 1.0f;
    g_reported.mv_scale_y = ndc_mv ? (static_cast<float>(g_reported.render_h) * -0.5f) : 1.0f;
    g_reported.frame_time_ms = 0.0f;
    g_reported.tick_ms = GetTickCount64();
    ++g_reported.serial;

    if (!g_logged_exec.exchange(true) && depth) {
        D3D12_RESOURCE_DESC md{};
        if (mv) { md = mv->GetDesc(); }
        D3D12_RESOURCE_DESC dd = depth->GetDesc();
        mgpu::input::logf("xess: first execute render=%ux%u output=%ux%u depth=%ux%u fmt %u"
                          " mv=%ux%u %s%s\n",
                          p->inputWidth, p->inputHeight, g_out_w.load(), g_out_h.load(),
                          static_cast<unsigned>(dd.Width), dd.Height,
                          static_cast<unsigned>(dd.Format),
                          static_cast<unsigned>(md.Width), md.Height,
                          hi_res_mv ? "HIGH_RES_MV " : "",
                          ndc_mv ? "NDC_MV" : "");
    }

    mgpu::input::track(mv, depth, resource_usage::shader_resource,
                       resource_usage::depth_stencil);
    mgpu::input::hold(depth);
    mgpu::input::hold(mv);
    g_valid.store(true, std::memory_order_release);
}

void on_init(const xess_d3d12_init_params_t* ip)
{
    if (!ip) { return; }
    g_init_flags.store(ip->initFlags, std::memory_order_relaxed);
    g_out_w.store(ip->outputResolution.x, std::memory_order_relaxed);
    g_out_h.store(ip->outputResolution.y, std::memory_order_relaxed);
    if (!g_logged_init.exchange(true)) {
        mgpu::input::logf("xess: init flags=0x%X output=%ux%u quality=%d\n",
                          ip->initFlags, ip->outputResolution.x, ip->outputResolution.y,
                          static_cast<int>(ip->qualitySetting));
    }
}

// ------------------------------------------------------------ per-module thunks
// Same reason as in_ffx.cpp: a game may probe several builds, and MinHook returns one
// trampoline per hook site, so each hooked module gets its own fixed-index thunk.
template <int N>
xess_result_t create_thunk(ID3D12Device* dev, xess_context_handle_t* ctx)
{
    return g_slots[N].create(dev, ctx);
}

template <int N>
xess_result_t build_thunk(xess_context_handle_t h, ID3D12PipelineLibrary* lib, bool blocking,
                          uint32_t flags)
{
    return g_slots[N].build(h, lib, blocking, flags);
}

template <int N>
xess_result_t init_thunk(xess_context_handle_t h, const xess_d3d12_init_params_t* ip)
{
    on_init(ip);
    return g_slots[N].init(h, ip);
}

template <int N>
xess_result_t execute_thunk(xess_context_handle_t h, ID3D12GraphicsCommandList* list,
                            const xess_d3d12_execute_params_t* p)
{
    on_execute(p);
    return g_slots[N].execute(h, list, p);
}

struct Thunks { LPVOID create, build, init, execute; };
const Thunks kThunks[kMaxModules] = {
    { reinterpret_cast<LPVOID>(&create_thunk<0>), reinterpret_cast<LPVOID>(&build_thunk<0>),
      reinterpret_cast<LPVOID>(&init_thunk<0>), reinterpret_cast<LPVOID>(&execute_thunk<0>) },
    { reinterpret_cast<LPVOID>(&create_thunk<1>), reinterpret_cast<LPVOID>(&build_thunk<1>),
      reinterpret_cast<LPVOID>(&init_thunk<1>), reinterpret_cast<LPVOID>(&execute_thunk<1>) },
};


bool is_xess_module(const wchar_t* path)
{
    // libxess.dll only: libxess_fg.dll is the frame generator (our consumer side), and
    // igxess/igxess2 are older names some builds still ship.
    static const wchar_t* kNames[] = { L"libxess.dll", L"igxess.dll", L"igxess2.dll" };
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
    if (slot >= kMaxModules) { return false; }

    g_slots[slot].module = m;
    g_slots[slot].path = path;

    struct Target { const char* name; LPVOID detour; LPVOID* original; };
    Target targets[] = {
        { "xessD3D12CreateContext",  kThunks[slot].create,
          reinterpret_cast<LPVOID*>(&g_slots[slot].create) },
        { "xessD3D12BuildPipelines", kThunks[slot].build,
          reinterpret_cast<LPVOID*>(&g_slots[slot].build) },
        { "xessD3D12Init",           kThunks[slot].init,
          reinterpret_cast<LPVOID*>(&g_slots[slot].init) },
        { "xessD3D12Execute",        kThunks[slot].execute,
          reinterpret_cast<LPVOID*>(&g_slots[slot].execute) },
    };

    bool any = false;
    for (auto& t : targets) {
        LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(m, t.name));
        if (!addr) { continue; }
        if (MH_CreateHook(addr, t.detour, t.original) != MH_OK ||
            MH_EnableHook(addr) != MH_OK) {
            mgpu::input::logf("xess: hook %s FAILED\n", t.name);
            continue;
        }
        any = true;
    }
    if (!any) { return false; }

    g_slot_count.store(slot + 1);
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
                       GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(m), &pinned);
    mgpu::input::logf("xess: %ls hooked (slot %d)\n", path.c_str(), slot);
    return true;
}

class XessSource final : public mgpu::input::Source
{
public:
    const char* name() const override { return "xess"; }
    uint32_t api_id() const override { return mgpu::kInputApiXess; }

    void on_module(HMODULE module) override
    {
        if (!module) { return; }
        wchar_t full[MAX_PATH]{};
        if (!GetModuleFileNameW(module, full, MAX_PATH)) { return; }
        if (!is_xess_module(full)) { return; }
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
                if (is_xess_module(me.szModule)) { on_module(me.hModule); }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }

    bool poll(mgpu::input::FrameInputs& out) override
    {
        if (!g_valid.load(std::memory_order_acquire)) { return false; }
        const XessFrame f = g_reported;
        if (f.serial == g_last_serial) { return false; }    // nothing new (see in_ngx)
        g_last_serial = f.serial;

        const uint32_t flags = g_init_flags.load(std::memory_order_relaxed);

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
        // XeSS exports no camera: the bridge keeps its synthetic projection.
        out.near_plane = 0.0f;
        out.far_plane  = 0.0f;
        out.vfov_rad   = 0.0f;

        uint32_t fl = 0;
        if (flags & XESS_INIT_FLAG_INVERTED_DEPTH)   { fl |= mgpu::kInputFlagInvertedDepth; }
        if (flags & XESS_INIT_FLAG_HIGH_RES_MV)      { fl |= mgpu::kInputFlagHiResMv; }
        if (flags & XESS_INIT_FLAG_USE_NDC_VELOCITY) { fl |= mgpu::kInputFlagMvNdc; }
        if (flags & XESS_INIT_FLAG_JITTERED_MV)      { fl |= mgpu::kInputFlagMvJittered; }
        if (!(flags & XESS_INIT_FLAG_LDR_INPUT_COLOR)) { fl |= mgpu::kInputFlagHdr; }
        out.flags  = fl;
        out.serial = f.serial;
        out.tick_ms = f.tick_ms;
        // ---- normalized semantics -------------------------------------------------
        // XeSS is the one API that says out loud which space its velocities live in:
        // USE_NDC_VELOCITY => NDC, otherwise the SDK's default is PIXELS (not UV). The first
        // version declared "uv" for the non-NDC case and the probe caught it on 2026-11-10:
        // `probe: mv mean|v|=(44.848,3.113) -> unit=pixels  declared=uv  (DISAGREES ...)`,
        // with mvscale=(1,1) in the same log - 44 px cannot be UV.
        out.mv_unit = (flags & XESS_INIT_FLAG_USE_NDC_VELOCITY)
                          ? mgpu::input::MvUnit::ndc
                          : mgpu::input::MvUnit::pixels;
        // Every one of these APIs defines its vectors as previous-minus-current; a game that
        // flipped it would break its own upscaler. MvFlip is the explicit escape hatch.
        out.mv_sign        = mgpu::input::MvSign::prev_minus_cur;
        out.camera_source  = mgpu::input::CameraSource::none;
        out.invalid_marker = 0.0f;
        out.valid_w = f.render_w;
        out.valid_h = f.render_h;
        return true;
    }
};

XessSource g_xess_source;

} // namespace xess_impl

mgpu::input::Source* mgpu::input::make_xess_source()
{
    return &xess_impl::g_xess_source;
}

