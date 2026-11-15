// ============================================================================
//  proxy_ngx.cpp - we are `nvngx_dlssg.dll`.
//
//  Why this file and not a hook (measured on CP2077, 2026-11-12): Streamline's
//  `sl.dlss_g` plugin resolves the DLSS-G entry points with GetProcAddress while
//  it loads - long before a ReShade add-on exists - and keeps those pointers.
//  OptiScaler sidesteps it by being that DLL; so do we. The real runtime is
//  loaded from `nvngx_dlssg_real.dll` and forwarded to whenever we are not the
//  ones answering (mode off/observe, or when the XeFG presenter could not start).
//
//  Four entry points decide whether frame generation exists at all, and they are
//  the only ones with logic: GetFeatureRequirements, CreateFeature,
//  EvaluateFeature, ReleaseFeature. Everything else (the D3D11/CUDA/Vulkan halves
//  of the same module, version helpers) forwards through one generic thunk, so a
//  module that probes 50 exports finds all 50.
//
//  Provenance: same arrangement as OptiScaler's framegen/nvngx/Nvngx_DllProxy.h +
//  Nvngx_FG.cpp (GPL-3.0, docs/design/FAKER_FG.md 7).
// ============================================================================
#include "fg_internal.h"

namespace fakerfg {
namespace {

// The surface the real DLSS-G module exports (measured on 310.9.16).
#define FG_NGX_EXPORTS(X)                                            \
    X(NVSDK_NGX_D3D12_Init)                                          \
    X(NVSDK_NGX_D3D12_Init_Ext)                                      \
    X(NVSDK_NGX_D3D12_CreateFeature)                                 \
    X(NVSDK_NGX_D3D12_EvaluateFeature)                               \
    X(NVSDK_NGX_D3D12_ReleaseFeature)                                \
    X(NVSDK_NGX_D3D12_GetFeatureRequirements)                        \
    X(NVSDK_NGX_D3D12_GetScratchBufferSize)                          \
    X(NVSDK_NGX_D3D12_Shutdown)                                      \
    X(NVSDK_NGX_D3D12_Shutdown1)                                     \
    X(NVSDK_NGX_D3D12_PopulateParameters_Impl)                       \
    X(NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl)                 \
    X(NVSDK_NGX_D3D11_Init)                                          \
    X(NVSDK_NGX_D3D11_Init_Ext)                                      \
    X(NVSDK_NGX_D3D11_CreateFeature)                                 \
    X(NVSDK_NGX_D3D11_EvaluateFeature)                               \
    X(NVSDK_NGX_D3D11_ReleaseFeature)                                \
    X(NVSDK_NGX_D3D11_GetFeatureRequirements)                        \
    X(NVSDK_NGX_D3D11_GetScratchBufferSize)                          \
    X(NVSDK_NGX_D3D11_Shutdown)                                      \
    X(NVSDK_NGX_D3D11_Shutdown1)                                     \
    X(NVSDK_NGX_D3D11_PopulateParameters_Impl)                       \
    X(NVSDK_NGX_D3D11_PopulateDeviceParameters_Impl)                 \
    X(NVSDK_NGX_GetAPIVersion)                                       \
    X(NVSDK_NGX_GetApplicationId)                                    \
    X(NVSDK_NGX_GetDriverVersionEx)                                  \
    X(NVSDK_NGX_GetGPUArchitecture)                                  \
    X(NVSDK_NGX_GetSnippetVersion)                                   \
    X(NVSDK_NGX_SetOverrideStatusCallback)                           \
    X(NVSDK_NGX_SetTelemetryEvaluateCallback)                        \
    X(NVSDK_NGX_CUDA_Init)                                           \
    X(NVSDK_NGX_CUDA_Init_Ext)                                       \
    X(NVSDK_NGX_CUDA_CreateFeature)                                  \
    X(NVSDK_NGX_CUDA_EvaluateFeature)                                \
    X(NVSDK_NGX_CUDA_ReleaseFeature)                                 \
    X(NVSDK_NGX_CUDA_GetScratchBufferSize)                           \
    X(NVSDK_NGX_CUDA_Shutdown)                                       \
    X(NVSDK_NGX_CUDA_PopulateParameters_Impl)                        \
    X(NVSDK_NGX_VULKAN_Init)                                         \
    X(NVSDK_NGX_VULKAN_Init_Ext)                                     \
    X(NVSDK_NGX_VULKAN_Init_Ext2)                                    \
    X(NVSDK_NGX_VULKAN_CreateFeature)                                \
    X(NVSDK_NGX_VULKAN_CreateFeature1)                               \
    X(NVSDK_NGX_VULKAN_EvaluateFeature)                              \
    X(NVSDK_NGX_VULKAN_ReleaseFeature)                               \
    X(NVSDK_NGX_VULKAN_GetFeatureRequirements)                       \
    X(NVSDK_NGX_VULKAN_GetScratchBufferSize)                         \
    X(NVSDK_NGX_VULKAN_Shutdown)                                     \
    X(NVSDK_NGX_VULKAN_Shutdown1)                                    \
    X(NVSDK_NGX_VULKAN_PopulateParameters_Impl)                      \
    X(NVSDK_NGX_VULKAN_PopulateDeviceParameters_Impl)

enum : int {
#define X(n) kExp_##n,
    FG_NGX_EXPORTS(X)
#undef X
    kExportCount
};

void* g_export[kExportCount]{};

// x64 has one calling convention: a six-pointer thunk forwards any entry point in
// this list, which is what lets the .def alias the real names to us.
typedef void* (WINAPI* pfn_any6)(void*, void*, void*, void*, void*, void*);

template <int I>
void* WINAPI thunk6(void* a, void* b, void* c, void* d, void* e, void* f)
{
    auto fn = reinterpret_cast<pfn_any6>(g_export[I]);
    return (fn != nullptr) ? fn(a, b, c, d, e, f) : nullptr;
}

// The entry points that carry behaviour.
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_init)(unsigned long long, const wchar_t*,
                                               ID3D12Device*, NVSDK_NGX_Version);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_init_ext)(unsigned long long, const wchar_t*,
                                                   ID3D12Device*, NVSDK_NGX_Version,
                                                   const NVSDK_NGX_Parameter*);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_shutdown1)(ID3D12Device*);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_scratch)(NVSDK_NGX_Feature,
                                                   const NVSDK_NGX_Parameter*, size_t*);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_requirements)(IDXGIAdapter*,
                                                        const NVSDK_NGX_FeatureDiscoveryInfo*,
                                                        NVSDK_NGX_FeatureRequirement*);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_create)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                  NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_evaluate)(ID3D12GraphicsCommandList*,
                                                    const NVSDK_NGX_Handle*,
                                                    const NVSDK_NGX_Parameter*, void*);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_release)(NVSDK_NGX_Handle*);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_populate)(NVSDK_NGX_Parameter*);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_params)(NVSDK_NGX_Parameter**);

HMODULE g_real = nullptr;
bool    g_real_tried = false;

void resolve_exports(HMODULE m)
{
    const char* names[kExportCount] = {
#define X(n) #n,
        FG_NGX_EXPORTS(X)
#undef X
    };
    int found = 0;
    for (int i = 0; i < kExportCount; ++i) {
        // The real entry points: GetProcAddress itself is hooked, so going through it here
        // would store our own thunks as forward targets (one hop of infinite regress).
        g_export[i] = ngx_real_get_proc(m, names[i]);
        if (g_export[i] != nullptr) { ++found; }
    }
    logf("ngx proxy: %d/%d exports resolved from nvngx_dlssg_real.dll\n", found, kExportCount);
}

// One capability override for both the real parameter block and our own.
void answer_capability(NVSDK_NGX_Parameter* p)
{
    if (p == nullptr) { return; }
    // NVNGX_Parameter.cpp:801-825, the same field set they set for
    // `activeFgInput == DLSSG || activeFgNvngx != None`.
    p->Set(NVSDK_NGX_Parameter_FrameGeneration_Available, 1);
    p->Set(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, 0);
    p->Set("FrameGeneration.FeatureInitResult", 1);
    p->Set("FrameInterpolation.Available", 1);
    p->Set(NVSDK_NGX_Parameter_FrameInterpolation_NeedsUpdatedDriver, 0);
    p->Set(NVSDK_NGX_Parameter_FrameInterpolation_FeatureInitResult, 1);

    // "Streamline handles the max interpolated frame count": keep what the driver table
    // reported, only defaulting it when the key is missing.
    int countMax = 0;
    if (p->Get("DLSSG.MultiFrameCountMax", &countMax) != NVSDK_NGX_Result_Success ||
        countMax <= 0) {
        countMax = 1;                                             // 2x
    }
    p->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, countMax);

    static bool told = false;
    if (!told) {
        told = true;
        logf("ngx proxy: reported FrameGeneration.Available=1 MultiFrameCountMax=%d\n", countMax);
    }
}


} // namespace

bool load_real_ngx()
{
    if (g_real != nullptr) { return true; }
    if (g_real_tried) { return false; }
    g_real_tried = true;

    // OptiScaler (proxies/NVNGX_Proxy.h:549) resolves the real entry points from the
    // driver's own core, which the LoadLibrary hook loaded for us.
    if (HMODULE core = ngx_real_core()) {
        g_real = core;
        resolve_exports(core);
        logf("ngx proxy: real NGX core (forward target) = %p\n", (void*)core);
        return true;
    }

    wchar_t dir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(dir, L'\\')) { *(slash + 1) = L'\0'; }

    const wchar_t* names[] = { L"nvngx_dlssg_real.dll", L"nvngx_dlssg.dll.orig",
                               L"nvngx_dlssg_real.orig.dll" };
    for (const wchar_t* n : names) {
        wchar_t path[MAX_PATH]{};
        wcscpy_s(path, dir);
        wcscat_s(path, n);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) { continue; }
        HMODULE m = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m == nullptr) {
            logf("ngx proxy: LoadLibrary(%ls) failed, GetLastError=%lu\n", path, GetLastError());
            continue;
        }
        g_real = m;
        resolve_exports(m);
        logf("ngx proxy: real DLSS-G runtime = %ls\n", path);
        return true;
    }
    logf("ngx proxy: no real runtime next to the exe (nvngx_dlssg_real.dll) -"
         " observe/forward modes are unavailable, the provider must stand alone\n");
    return false;
}

void* real_ngx(const char* name)
{
    if (!load_real_ngx() || name == nullptr) { return nullptr; }
    return reinterpret_cast<void*>(GetProcAddress(g_real, name));
}

} // namespace fakerfg

using namespace fakerfg;

// ============================================================================
//  The exported surface. `fg_*` are the entries with behaviour; `fwd_*` is
//  generated for every other symbol so this module looks exactly like the one it
//  displaced.
// ============================================================================
extern "C" {

NVSDK_NGX_Result NVSDK_CONV fg_init(unsigned long long app_id,
                                                          const wchar_t* data_path,
                                                          ID3D12Device* device,
                                                          NVSDK_NGX_Version version)
{
    start_loader_thread();          // ReShade comes up here, before any swap chain
    sl_install_if_possible();
    load_real_ngx();                // resolve once, so a later Evaluate can forward

    auto real = reinterpret_cast<pfn_init>(g_export[kExp_NVSDK_NGX_D3D12_Init]);
    const NVSDK_NGX_Result r = (real != nullptr) ? real(app_id, data_path, device, version)
                                                 : NVSDK_NGX_Result_Success;
    logf("ngx: Init -> 0x%X (answers=%d)\n", static_cast<unsigned>(r),
         provider_answers() ? 1 : 0);
    return r;
}

NVSDK_NGX_Result NVSDK_CONV fg_init_ext(
    unsigned long long app_id, const wchar_t* data_path, ID3D12Device* device,
    NVSDK_NGX_Version version, const NVSDK_NGX_Parameter* params)
{
    start_loader_thread();
    load_real_ngx();
    auto real = reinterpret_cast<pfn_init_ext>(g_export[kExp_NVSDK_NGX_D3D12_Init_Ext]);
    return (real != nullptr) ? real(app_id, data_path, device, version, params)
                             : NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV fg_shutdown()
{
    auto real = reinterpret_cast<NVSDK_NGX_Result(NVSDK_CONV*)()>(
        g_export[kExp_NVSDK_NGX_D3D12_Shutdown]);
    if (real != nullptr) { real(); }
    logf("ngx: Shutdown\n");
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV fg_shutdown1(ID3D12Device* device)
{
    auto real = reinterpret_cast<pfn_shutdown1>(g_export[kExp_NVSDK_NGX_D3D12_Shutdown1]);
    if (real != nullptr) { real(device); }
    logf("ngx: Shutdown1\n");
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV fg_scratch(
    NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* params, size_t* size)
{
    auto real = reinterpret_cast<pfn_scratch>(
        g_export[kExp_NVSDK_NGX_D3D12_GetScratchBufferSize]);
    if (real != nullptr) { return real(feature, params, size); }
    if (size != nullptr) { *size = 0; }
    return NVSDK_NGX_Result_Success;
}

// "Is DLSS-G available on this adapter?" - the gate the menu is built on. We answer
// it when we are the provider; otherwise the real runtime's answer stands.
NVSDK_NGX_Result NVSDK_CONV fg_requirements_impl(
    IDXGIAdapter* adapter, const NVSDK_NGX_FeatureDiscoveryInfo* info,
    NVSDK_NGX_FeatureRequirement* out)
{
    auto real = reinterpret_cast<pfn_requirements>(
        g_export[kExp_NVSDK_NGX_D3D12_GetFeatureRequirements]);
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    if (real != nullptr) { r = real(adapter, info, out); }

    if (out == nullptr || info == nullptr ||
        static_cast<int>(info->FeatureID) != NVSDK_NGX_Feature_FrameGeneration ||
        !provider_answers()) {
        return r;
    }

    const unsigned was = static_cast<unsigned>(out->FeatureSupported);
    out->FeatureSupported  = static_cast<NVSDK_NGX_Feature_Support_Result>(0);
    out->MinHWArchitecture = 0;   // NVNGX_Proxy.h:102/121/141 - no minimum
    static bool told = false;
    if (!told) {
        told = true;
        logf("ngx: DLSS-G supported (GetFeatureRequirements, was supported=%u)\n", was);
    }
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV fg_capability_impl(NVSDK_NGX_Parameter** out)
{
    if (out == nullptr) { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    // The DLSS-G module does not export the capability entry points (they live in the
    // nvngx core), so there is nothing to forward to: answer ourselves.
    if (*out == nullptr) { *out = new_parameter_block(); }
    if (provider_answers()) { answer_capability(*out); }
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV fg_parameters(NVSDK_NGX_Parameter** out)
{
    if (out == nullptr) { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    *out = new_parameter_block();
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV fg_allocate(NVSDK_NGX_Parameter** out)
{
    if (out == nullptr) { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    *out = new_parameter_block();
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV fg_destroy(NVSDK_NGX_Parameter* p)
{
    destroy_parameter_block(p);
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV fg_populate_impl(NVSDK_NGX_Parameter* params)
{
    auto real = reinterpret_cast<pfn_populate>(
        g_export[kExp_NVSDK_NGX_D3D12_PopulateParameters_Impl]);
    const NVSDK_NGX_Result r = (real != nullptr) ? real(params) : NVSDK_NGX_Result_Success;
    // Without a real runtime, still report the one thing callers read from this.
    if (params != nullptr) {
        // NVNGX uses this to decide the game-side DLSS-FG multiplier. Keep it at 2x
        // for the dual-GPU input lane; see proxy_nvngx.cpp thunk_caps.
        params->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, 1);
    }
    return r;
}

NVSDK_NGX_Result NVSDK_CONV fg_populate_device(NVSDK_NGX_Parameter* params)
{
    auto real = reinterpret_cast<pfn_populate>(
        g_export[kExp_NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl]);
    return (real != nullptr) ? real(params) : NVSDK_NGX_Result_Success;
}

// Creation: ours to keep. This is where "we are the FG pipeline" starts.
NVSDK_NGX_Result NVSDK_CONV fg_create_impl(
    ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* params,
    NVSDK_NGX_Handle** out)
{
    start_loader_thread();
    if (out == nullptr) { return NVSDK_NGX_Result_FAIL_InvalidParameter; }

    ID3D12Device* device = nullptr;
    if (list != nullptr) { list->GetDevice(IID_PPV_ARGS(&device)); }

    unsigned w = 0, h = 0;
    if (params != nullptr) {
        params->Get(NVSDK_NGX_Parameter_Width, &w);
        params->Get(NVSDK_NGX_Parameter_Height, &h);
    }
    logf("ngx: CreateFeature feature=%d %ux%u FgOutput=%d owns_output=%d\n",
         static_cast<int>(feature), w, h, cfg().fg_output,
         provider_owns_output() ? 1 : 0);

    void* ours = nullptr;
    const bool ours_created =
        static_cast<int>(feature) == NVSDK_NGX_Feature_FrameGeneration &&
        provider_create(device, list, params, &ours);
    if (ours_created) {
        *out = static_cast<NVSDK_NGX_Handle*>(ours);
        if (device != nullptr) { device->Release(); }
        return NVSDK_NGX_Result_Success;
    }
    if (device != nullptr) { device->Release(); }

    // The forwarding decision is Passthrough's, not ours per-feature. Only a session that
    // deliberately observes the title's own FG may hand the contract to
    // `nvngx_dlssg_real.dll`. In every owning session forwarding would mean the game keeps
    // generating frames - on the render GPU, at full cost - while we either generate as
    // well or only transport. Measured on CP2077: `FakeDlssgFeature=0` used to reach the
    // real runtime through this path, which is exactly the "the game is still running FG"
    // symptom.
    const bool forward_to_real = cfg().passthrough != 0;
    load_real_ngx();
    auto real = reinterpret_cast<pfn_create>(g_export[kExp_NVSDK_NGX_D3D12_CreateFeature]);
    if (forward_to_real) {
        if (real != nullptr) { return real(list, feature, params, out); }
        logf("ngx: CreateFeature: no real runtime and the provider is not answering -> Fail\n");
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }
    static bool told_decline = false;
    if (!told_decline) {
        told_decline = true;
        logf("ngx: DLSS-G CreateFeature refused (Passthrough=0) - the title turns its OWN"
             " frame generation off; fakerFG owns the FG path\n");
    }
    *out = nullptr;
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

// Evaluate: harvest + publish + tag for our own handle; forward (and still harvest)
// for the real runtime's.
NVSDK_NGX_Result NVSDK_CONV fg_evaluate_impl(
    ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* params, void* callback)
{
    if (provider_ours(handle)) {
        const unsigned ok = provider_evaluate(list, handle,
                                              const_cast<NVSDK_NGX_Parameter*>(params));
        return (ok != 0) ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
    }

    // A handle that is not ours can only exist in a passthrough session (the real runtime
    // created it). Anywhere else it is a leftover and forwarding it would let the title's
    // own FG generate behind our back.
    const bool observer = cfg().passthrough != 0;
    if (!observer) {
        static bool told = false;
        if (!told) {
            told = true;
            logf("ngx: Evaluate of a foreign DLSS-G handle ignored - the title's own frame"
                 " generation stays off\n");
        }
        return NVSDK_NGX_Result_Success;
    }

    // Not ours: DLSS-SR (or DLSS-D/RR) - the Upscaler lane's only source of MV/depth.
    // Record it while forwarding (Upscaler_Inputs_Dx12 does the same in UpscaleStart).
    sr_harvest(const_cast<NVSDK_NGX_Parameter*>(params));

    load_real_ngx();
    auto real = reinterpret_cast<pfn_evaluate>(g_export[kExp_NVSDK_NGX_D3D12_EvaluateFeature]);
    const NVSDK_NGX_Result r = (real != nullptr) ? real(list, handle, params, callback)
                                                 : NVSDK_NGX_Result_Success;

    // Passthrough lane: the real runtime produced the picture, but the record is still
    // ours - this keeps A/B against the old path useful.
    MgpuFgFrame f{};
    harvest(const_cast<NVSDK_NGX_Parameter*>(params), list, f);
    f.mode       = api_mode();
    f.present_id = xefg_next_frame_id();
    publish_record(f);
    return r;
}

NVSDK_NGX_Result NVSDK_CONV fg_release_impl(NVSDK_NGX_Handle* handle)
{
    if (provider_ours(handle)) {
        provider_release(handle);
        return NVSDK_NGX_Result_Success;
    }
    const bool observer = cfg().passthrough != 0;
    if (!observer) { return NVSDK_NGX_Result_Success; }
    load_real_ngx();
    auto real = reinterpret_cast<pfn_release>(g_export[kExp_NVSDK_NGX_D3D12_ReleaseFeature]);
    return (real != nullptr) ? real(handle) : NVSDK_NGX_Result_Success;
}

} // extern "C"

// ---------------------------------------------------------------- instance bridge
namespace fakerfg {
namespace {

const NgxBridge* g_bridge = nullptr;
bool g_bridge_tried = false;

// The primary is the instance loaded as dxgi.dll. A DLSS-G instance forwards to it.
const NgxBridge* primary()
{
    if (g_bridge_tried) { return g_bridge; }
    g_bridge_tried = true;

    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&fg_create_impl), &self)) {
        return nullptr;
    }
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (dxgi == nullptr || dxgi == self) { return nullptr; }

    typedef const NgxBridge* (__cdecl* pfn_bridge)();
    auto get = reinterpret_cast<pfn_bridge>(
        reinterpret_cast<void*>(GetProcAddress(dxgi, "MgpuFgNgxBridge")));
    if (get == nullptr) { return nullptr; }

    const NgxBridge* b = get();
    if (b != nullptr && b->size == sizeof(NgxBridge)) {
        g_bridge = b;
        logf("ngx proxy: this is the DLSS-G instance - the contract is handled by the"
             " dxgi instance (%p)\n", (void*)dxgi);
    }
    return g_bridge;
}

} // namespace
} // namespace fakerfg

// The table the DLSS-G instance calls. It always runs in the dxgi instance, where
// the swap chain, the XeFG context and the record live.
extern "C" const NgxBridge* __cdecl MgpuFgNgxBridge()
{
    static const NgxBridge table = {
        sizeof(NgxBridge),
        &fg_create_impl,
        &fg_evaluate_impl,
        &fg_release_impl,
        &fg_requirements_impl,
        &fg_capability_impl,
        &fg_populate_impl,
    };
    return &table;
}

// Thin exports: delegate when this is the DLSS-G instance, otherwise run here.
extern "C" {

NVSDK_NGX_Result NVSDK_CONV fg_create(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature f,
                                      NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out)
{
    if (const NgxBridge* b = fakerfg::primary()) { return b->create(list, f, p, out); }
    return fg_create_impl(list, f, p, out);
}

NVSDK_NGX_Result NVSDK_CONV fg_evaluate(ID3D12GraphicsCommandList* list,
                                        const NVSDK_NGX_Handle* h,
                                        const NVSDK_NGX_Parameter* p, void* cb)
{
    if (const NgxBridge* b = fakerfg::primary()) { return b->evaluate(list, h, p, cb); }
    return fg_evaluate_impl(list, h, p, cb);
}

NVSDK_NGX_Result NVSDK_CONV fg_release(NVSDK_NGX_Handle* h)
{
    if (const NgxBridge* b = fakerfg::primary()) { return b->release(h); }
    return fg_release_impl(h);
}

NVSDK_NGX_Result NVSDK_CONV fg_requirements(IDXGIAdapter* a,
                                            const NVSDK_NGX_FeatureDiscoveryInfo* i,
                                            NVSDK_NGX_FeatureRequirement* o)
{
    if (const NgxBridge* b = fakerfg::primary()) { return b->requirements(a, i, o); }
    return fg_requirements_impl(a, i, o);
}

NVSDK_NGX_Result NVSDK_CONV fg_capability(NVSDK_NGX_Parameter** out)
{
    if (const NgxBridge* b = fakerfg::primary()) { return b->capability(out); }
    return fg_capability_impl(out);
}

NVSDK_NGX_Result NVSDK_CONV fg_populate(NVSDK_NGX_Parameter* p)
{
    if (const NgxBridge* b = fakerfg::primary()) { return b->populate(p); }
    return fg_populate_impl(p);
}

} // extern "C"

extern "C" {

// ---------------------------------------------------------------- forwarders
// One C function per remaining symbol; the .def maps the real names onto these.
#define X(n)                                                                           \
    void* __cdecl fwd_##n(void* a, void* b, void* c, void* d,     \
                                                void* e, void* f)                      \
    {                                                                                  \
        return thunk6<kExp_##n>(a, b, c, d, e, f);                                     \
    }
FG_NGX_EXPORTS(X)
#undef X

} // extern "C"
