// ============================================================================
//  proxy_nvngx.cpp - route the driver's NGX core per feature, like OptiScaler's
//  NVNGX_Proxy does.
//
//  Measured on CP2077: Streamline's `sl.dlss_g` plugin resolves every NGX entry
//  point from the driver's `_nvngx.dll` (the live log shows the architecture
//  question coming from DriverStore\...\_nvngx.dll), so a `nvngx_dlssg.dll` next to
//  the exe is never loaded. The way in is GetProcAddress.
//
//  The first attempt replaced the core's CreateFeature/GetCapabilityParameters
//  outright and black-screened the game: DLSS-SR is created through the same
//  function, and its implementation does not exist in our DLSS-G fallback module.
//  OptiScaler's proxy is shaped differently, and that shape is what this file copies:
//
//      * one real function table per module the caller asked about,
//      * our returned entry points forward to that table,
//      * only `feature == NVSDK_NGX_Feature_FrameGeneration` is routed to us,
//      * GetCapabilityParameters keeps the real parameter object and only edits
//        the frame-generation fields on it.
//
//  Everything else (DLSS-SR, DLSS-D, RR, Init, Shutdown, scratch) is untouched.
// ============================================================================
#include "fg_internal.h"

#include <MinHook.h>
#include <psapi.h>

namespace fakerfg {
namespace {

typedef FARPROC(WINAPI* pfn_get_proc)(HMODULE, LPCSTR);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_create)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                  NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_evaluate)(ID3D12GraphicsCommandList*,
                                                    const NVSDK_NGX_Handle*,
                                                    const NVSDK_NGX_Parameter*, void*);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_release)(NVSDK_NGX_Handle*);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_requirements)(IDXGIAdapter*,
                                                        const NVSDK_NGX_FeatureDiscoveryInfo*,
                                                        NVSDK_NGX_FeatureRequirement*);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_caps)(NVSDK_NGX_Parameter**);

struct NgxModule
{
    HMODULE          mod          = nullptr;
    pfn_create       create       = nullptr;
    pfn_evaluate     evaluate     = nullptr;
    pfn_release      release      = nullptr;
    pfn_requirements requirements = nullptr;
    pfn_caps         caps         = nullptr;
};

constexpr int kMaxModules = 4;
NgxModule g_mods[kMaxModules]{};
int       g_mod_count = 0;

// The real parameter block we patched, kept so the answers can be taken back once the
// title's own inputs show XeFG cannot run there (see nvngx_withdraw_frame_generation).
NVSDK_NGX_Parameter* g_patched_params = nullptr;

pfn_get_proc g_real_get_proc = nullptr;      // kernel32!GetProcAddress trampoline
pfn_get_proc g_real_get_proc_kb = nullptr;   // kernelbase!GetProcAddress trampoline
bool         g_ngx_installed = false;

bool is_dlssg(NVSDK_NGX_Feature feature)
{
    return static_cast<int>(feature) == static_cast<int>(NVSDK_NGX_Feature_FrameGeneration);
}

NgxModule* module_entry(HMODULE m)
{
    for (int i = 0; i < g_mod_count; ++i) {
        if (g_mods[i].mod == m) { return &g_mods[i]; }
    }
    if (m == nullptr || g_mod_count >= kMaxModules || g_real_get_proc == nullptr) { return nullptr; }

    NgxModule& e = g_mods[g_mod_count++];
    e.mod      = m;
    e.create   = reinterpret_cast<pfn_create>(reinterpret_cast<void*>(
                     g_real_get_proc(m, "NVSDK_NGX_D3D12_CreateFeature")));
    e.evaluate = reinterpret_cast<pfn_evaluate>(reinterpret_cast<void*>(
                     g_real_get_proc(m, "NVSDK_NGX_D3D12_EvaluateFeature")));
    e.release  = reinterpret_cast<pfn_release>(reinterpret_cast<void*>(
                     g_real_get_proc(m, "NVSDK_NGX_D3D12_ReleaseFeature")));
    e.requirements = reinterpret_cast<pfn_requirements>(reinterpret_cast<void*>(
                     g_real_get_proc(m, "NVSDK_NGX_D3D12_GetFeatureRequirements")));
    e.caps     = reinterpret_cast<pfn_caps>(reinterpret_cast<void*>(
                     g_real_get_proc(m, "NVSDK_NGX_D3D12_GetCapabilityParameters")));
    logf("nvngx: module %p -> create=%p eval=%p caps=%p\n",
         (void*)m, (void*)e.create, (void*)e.evaluate, (void*)e.caps);
    return &e;
}

// ---------------------------------------------------------------- per-module thunks
// x64 has one calling convention; the module index is folded into a template so the
// returned pointer still identifies which real table to forward to.
template <int N>
NVSDK_NGX_Result NVSDK_CONV thunk_create(ID3D12GraphicsCommandList* list,
                                         NVSDK_NGX_Feature feature,
                                         NVSDK_NGX_Parameter* params,
                                         NVSDK_NGX_Handle** out)
{
    NgxModule& m = g_mods[N];
    if (is_dlssg(feature)) {
        static int told = 0;
        if (told < 8) {
            ++told;
            logf("nvngx: CreateFeature(FrameGeneration) owns=%d capture=%d fake=%d\n",
                 provider_owns_output() ? 1 : 0, provider_capture_dlssg() ? 1 : 0,
                 cfg().fake_dlssg_feature);
        }
    }
    if (is_dlssg(feature) && (provider_owns_output() || provider_capture_dlssg()) &&
        out != nullptr) {
        ID3D12Device* device = nullptr;
        if (list != nullptr) { list->GetDevice(IID_PPV_ARGS(&device)); }
        void* handle = nullptr;
        const bool ours = provider_create(device, list, params, &handle);
        if (device != nullptr) { device->Release(); }
        if (ours) {
            *out = static_cast<NVSDK_NGX_Handle*>(handle);
            return NVSDK_NGX_Result_Success;
        }
    }
    return (m.create != nullptr) ? m.create(list, feature, params, out)
                                 : NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

// The upscaler lane, shared by both Evaluate thunks. OptiScaler's Upscaler_Inputs_Dx12 tags
// MV/depth in UpscaleStart and the upscaler's output in UpscaleEnd - i.e. on the frame's own
// command list, before and after forwarding the Evaluate. With TagAtEvaluate=1 that is
// exactly what happens here; with it off xefg_tag_at_evaluate is a no-op and the present
// path keeps tagging as before.
void publish_upscaler_record(ID3D12GraphicsCommandList* list, bool tagged);   // below

NVSDK_NGX_Result forward_upscaler_evaluate(pfn_evaluate real, ID3D12GraphicsCommandList* list,
                                           const NVSDK_NGX_Handle* handle,
                                           const NVSDK_NGX_Parameter* params, void* callback,
                                           NVSDK_NGX_Result missing)
{
    // The entry may still be unresolved when the module scan ran before the real NGX core
    // was known (MinHook status 3 + `ngx_real_core() == nullptr`). Resolve it here rather
    // than answer Success without running the upscaler - that would leave the title's output
    // texture unwritten (black scene) with no error anywhere.
    if (real == nullptr) {
        if (HMODULE core = ngx_real_core()) {
            if (g_real_get_proc != nullptr) {
                real = reinterpret_cast<pfn_evaluate>(
                    reinterpret_cast<void*>(g_real_get_proc(core,
                        "NVSDK_NGX_D3D12_EvaluateFeature")));
                static bool told = false;
                if (!told && real != nullptr) {
                    told = true;
                    logf("nvngx: resolved the upscaler Evaluate lazily from the real core %p\n",
                         reinterpret_cast<void*>(real));
                }
            }
        }
    }

    // OptiScaler NVNGX_DLSS_Dx12.cpp:933, verbatim:
    //     evalCounter++;
    //     if (cfg.SkipFirstFrames.has_value() && evalCounter < cfg.SkipFirstFrames.value())
    //         return NVSDK_NGX_Result_Success;
    // Wuchang's GameConfigs entry is 10 ("Skip 1 frame use of upscaler which cause crash").
    static unsigned eval_counter = 0;
    ++eval_counter;
    const int skip_frames = cfg().skip_first_frames;
    if (skip_frames > 0 && eval_counter < static_cast<unsigned>(skip_frames)) {
        static bool told_skip = false;
        if (!told_skip) {
            told_skip = true;
            logf("sr: SkipFirstFrames=%d - the first %d upscaler evaluates are not"
                 " forwarded\n", skip_frames, skip_frames);
        }
        return NVSDK_NGX_Result_Success;
    }

    sr_note_command_list(list);          // OptiScaler: res.cmdList = InCmdList
    const unsigned harvests = sr_harvest_count();
    sr_harvest(const_cast<NVSDK_NGX_Parameter*>(params));
    const bool harvested = (sr_harvest_count() != harvests);

    const bool tagged = harvested && xefg_tag_at_evaluate(list, EvalTag_Inputs);

    const NVSDK_NGX_Result r = (real != nullptr) ? real(list, handle, params, callback) : missing;

    if (tagged) { xefg_tag_at_evaluate(list, EvalTag_Color); }

    // v2 seam: the capture's record, published while `list` is still open.
    if (harvested) { publish_upscaler_record(list, tagged); }
    return r;
}

// The capture's view of this frame, published whether or not we are the DLSS-G provider.
//
// The shipping configuration is the LINK mode (FakeDlssgFeature=0): the title keeps its
// own frame generation off, no DLSS-G Evaluate ever reaches us, and the upscaler lane is
// the only place the inputs exist. Without this publish fakerFG produces no record at all
// in that mode, and the capture has to fall back to the hook-sniffing lanes the new
// architecture exists to remove (docs/design/XEFG_CHAIN_STANDARD.md 3A).
void publish_upscaler_record(ID3D12GraphicsCommandList* list, bool tagged)
{
    // Only the link mode publishes from here: with FakeDlssgFeature=1 the DLSS-G
    // provider owns the contract and provider_evaluate() publishes the record.
    // (provider_owns_output() is NOT the right test: it is true for any
    // Idle/Local/Bridge session that took the chain, including the link mode.)
    if (cfg().fg_input != static_cast<int>(FgInputSource::Upscaler) ||
        cfg().fake_dlssg_feature != 0) { return; }

    // sr_apply() does not state the frame semantics (the upscaler parameter block does not
    // carry them), so they come from the same configuration XeFG is running with.
    uint32_t base_flags = MGPU_FG_FRAME_MV_ACTIVE | MGPU_FG_FRAME_FROM_FG;
    if (cfg().depth_inverted) { base_flags |= MGPU_FG_FRAME_DEPTH_INVERTED; }
    if (cfg().mv_jittered)    { base_flags |= MGPU_FG_FRAME_MV_JITTERED; }
    if (cfg().high_res_mv != 0 && cfg().mv_resample == 0) { base_flags |= MGPU_FG_FRAME_HI_RES_MV; }

    MgpuFgFrame f{};
    f.flags = base_flags;                     // sr_apply() swaps near/far on DEPTH_INVERTED
    const bool have = tagged ? xefg_last_eval_frame(&f) : sr_apply(f);
    if (!have) { return; }

    if (tagged) {
        // xefg_tag_at_evaluate() ran sr_apply() with a zeroed flag set, so it did not apply
        // the reversed-Z swap; do it here, once, and take the flags from the configuration.
        if (cfg().depth_inverted && f.camera_near > 0.0f) {
            std::swap(f.camera_near, f.camera_far);
        }
        f.flags |= base_flags;
    }
    // OptiScaler's UpscalerInputs::GetCameraValues declares the near/far pair; with
    // DepthInverted the pair is stored reversed, which is what the DEPTH_INVERTED flag says.
    if (f.camera_near > 0.0f && f.camera_far > 0.0f) {
        f.flags |= MGPU_FG_FRAME_HAS_CAMERA;
    }
    // The upscaler parameter block may simply not carry a velocity texture. Do not claim
    // MV_ACTIVE in that case: a depth-only record is valid, and the consumer must be able
    // to tell "no MV" from "MV we forgot to copy" (the OSD/log depends on it).
    if (f.stream[MGPU_FG_STREAM_MV].resource == nullptr) {
        f.flags &= ~MGPU_FG_FRAME_MV_ACTIVE;
    }

    f.cmd_list     = list;                    // still OPEN: this is the copy window
    for (int i = 0; i < MGPU_FG_STREAM_COUNT; ++i) { f.stream_cmd[i] = list; }
    f.flags       |= MGPU_FG_FRAME_CMD_OPEN;  // ...so the transport may record on it
    f.mode         = api_mode();
    // The record needs the device too: the transport places its slots on it. In the
    // upscaler lane nothing set it (provider's harvest does), so take the chain's.
    if (f.device == nullptr) { f.device = xefg_device(); }
    f.frame_qpc_ms = now_qpc_ms();
    if (f.declared_frames == 0) {
        f.declared_frames = (cfg().interpolated >= 1) ? cfg().interpolated : 1;
    }

    static int told = 0;
    if (told < 3) {
        ++told;
        logf("record: upscaler lane %s mv=%p depth=%p render=%ux%u out=%ux%u flags=0x%X\n",
             tagged ? "(tagged, post-MvResample)" : "(untagged)",
             f.stream[MGPU_FG_STREAM_MV].resource, f.stream[MGPU_FG_STREAM_DEPTH].resource,
             f.render_w, f.render_h, f.output_w, f.output_h, f.flags);
    }
    publish_record(f);
}

template <int N>
NVSDK_NGX_Result NVSDK_CONV thunk_evaluate(ID3D12GraphicsCommandList* list,
                                            const NVSDK_NGX_Handle* handle,
                                            const NVSDK_NGX_Parameter* params, void* callback)
{
    NgxModule& m = g_mods[N];
    if (provider_ours(handle)) {
        const unsigned ok = provider_evaluate(list, handle,
                                              const_cast<NVSDK_NGX_Parameter*>(params));
        return (ok != 0) ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
    }
    // Not ours: this is DLSS-SR (or DLSS-D/RR). Record the MV/depth it declares -
    // OptiScaler's CP2077 profile feeds XeFG from here, not from DLSS-G's block.
    // A null target must never pass silently: we would answer Success without running the
    // upscaler, and the title would present an output texture nobody wrote (black scene,
    // healthy swapchain, no error anywhere).
    if (m.evaluate == nullptr) {
        static bool told = false;
        if (!told) {
            told = true;
            logf("sr: module #%d has NO real EvaluateFeature - DLSS output would stay"
                 " unwritten (black scene with a healthy swapchain)\n", N);
        }
    }
    return forward_upscaler_evaluate(m.evaluate, list, handle, params, callback,
                                     NVSDK_NGX_Result_FAIL_FeatureNotFound);
}

template <int N>
NVSDK_NGX_Result NVSDK_CONV thunk_release(NVSDK_NGX_Handle* handle)
{
    NgxModule& m = g_mods[N];
    if (provider_ours(handle)) { provider_release(handle); return NVSDK_NGX_Result_Success; }
    return (m.release != nullptr) ? m.release(handle) : NVSDK_NGX_Result_Success;
}

template <int N>
NVSDK_NGX_Result NVSDK_CONV thunk_requirements(IDXGIAdapter* adapter,
                                               const NVSDK_NGX_FeatureDiscoveryInfo* info,
                                               NVSDK_NGX_FeatureRequirement* out)
{
    NgxModule& m = g_mods[N];
    const NVSDK_NGX_Result r = (m.requirements != nullptr) ? m.requirements(adapter, info, out)
                                                           : NVSDK_NGX_Result_Fail;
    if (out == nullptr || info == nullptr || !is_dlssg(info->FeatureID) || !provider_answers()) {
        return r;
    }
    out->FeatureSupported  = static_cast<NVSDK_NGX_Feature_Support_Result>(0);
    out->MinHWArchitecture = 0;   // NVNGX_Proxy.h:102/121/141 - no minimum
    return NVSDK_NGX_Result_Success;
}

// Keep the real parameter object - the game uses it for far more than FG - and edit
// only the frame-generation answers on it.
template <int N>
NVSDK_NGX_Result NVSDK_CONV thunk_caps(NVSDK_NGX_Parameter** out)
{
    NgxModule& m = g_mods[N];
    const NVSDK_NGX_Result r = (m.caps != nullptr) ? m.caps(out) : NVSDK_NGX_Result_Fail;
    if (out == nullptr || *out == nullptr || !provider_answers()) { return r; }

    NVSDK_NGX_Parameter* p = *out;
    g_patched_params = p;
    // NVNGX_Parameter.cpp:801-825 - the same field set, applied to the REAL parameter block
    // (it stays theirs; only the frame-generation answers are edited).
    p->Set(NVSDK_NGX_Parameter_FrameGeneration_Available, 1);
    p->Set(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, 0);
    p->Set("FrameGeneration.FeatureInitResult", 1);
    p->Set("FrameInterpolation.Available", 1);
    p->Set(NVSDK_NGX_Parameter_FrameInterpolation_NeedsUpdatedDriver, 0);
    p->Set(NVSDK_NGX_Parameter_FrameInterpolation_FeatureInitResult, 1);
    {
        int countMax = 0;
        if (p->Get("DLSSG.MultiFrameCountMax", &countMax) != NVSDK_NGX_Result_Success ||
            countMax <= 0) {
            countMax = 1;
        }
        // The dual-GPU input lane must keep the game at 2x. A 4x game-side setting
        // reduces NVNGX Evaluate to 15/s on a 30fps title (measured on PBZ), which
        // makes a 30x4 consumer output only 60fps.
        if (cfg().fg_input == static_cast<int>(FgInputSource::NVNGX)) { countMax = 1; }
        p->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, countMax);
    }
    static bool told = false;
    if (!told) {
        told = true;
        logf("nvngx: real capability block patched: FrameGeneration.Available=1\n");
    }
    return r;
}

FARPROC thunk_for(const NgxModule* m, const char* name)
{
    const int n = static_cast<int>(m - g_mods);
    struct Map { const char* name; FARPROC f[4]; };
    static const Map map[] = {
        { "NVSDK_NGX_D3D12_CreateFeature",
          { (FARPROC)&thunk_create<0>, (FARPROC)&thunk_create<1>,
            (FARPROC)&thunk_create<2>, (FARPROC)&thunk_create<3> } },
        { "NVSDK_NGX_D3D12_EvaluateFeature",
          { (FARPROC)&thunk_evaluate<0>, (FARPROC)&thunk_evaluate<1>,
            (FARPROC)&thunk_evaluate<2>, (FARPROC)&thunk_evaluate<3> } },
        { "NVSDK_NGX_D3D12_ReleaseFeature",
          { (FARPROC)&thunk_release<0>, (FARPROC)&thunk_release<1>,
            (FARPROC)&thunk_release<2>, (FARPROC)&thunk_release<3> } },
        { "NVSDK_NGX_D3D12_GetFeatureRequirements",
          { (FARPROC)&thunk_requirements<0>, (FARPROC)&thunk_requirements<1>,
            (FARPROC)&thunk_requirements<2>, (FARPROC)&thunk_requirements<3> } },
        { "NVSDK_NGX_D3D12_GetCapabilityParameters",
          { (FARPROC)&thunk_caps<0>, (FARPROC)&thunk_caps<1>,
            (FARPROC)&thunk_caps<2>, (FARPROC)&thunk_caps<3> } },
    };
    for (const Map& e : map) {
        if (strcmp(name, e.name) == 0) { return e.f[n]; }
    }
    return nullptr;
}
bool is_ngx_core(HMODULE m)
{
    if (m == nullptr) { return false; }
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(m, path, MAX_PATH) == 0) { return false; }
    const wchar_t* base = wcsrchr(path, L'\\');
    base = (base != nullptr) ? base + 1 : path;
    return _wcsicmp(base, L"nvngx.dll") == 0 || _wcsicmp(base, L"_nvngx.dll") == 0;
}

// The upscaler runtimes: nvngx_dlss.dll (DLSS-SR), nvngx_dlssd.dll (ray
// reconstruction), nvngx_dlssnr.dll (denoiser). OptiScaler's Upscaler_Inputs_Dx12
// takes MV/depth from their Evaluate for exactly this reason - on CP2077 those are
// the R32/RG16F pair XeFG wants, while the DLSS-G block declares a D32 depth.
// (The name test lives in name_is_upscaler() below, where the export scan uses it.)


// ---------------------------------------------------------------- upscaler lane
// OptiScaler's Upscaler_Inputs_Dx12 hooks the *upscaler's* EvaluateFeature and reads
// MV/depth from that parameter block. Only EvaluateFeature is replaced, and it is
// forwarded unchanged - the upscaler keeps working, we just look at the inputs.
//
// One table, one counter: the two entry points that can register a module (the
// GetProcAddress interposition and the export scan) must agree on the slot, because
// the slot number *is* the thunk identity. The first version indexed the same array
// with two counters, so the scan could overwrite the entry a live thunk pointed at -
// which forwards a foreign handle into another module's Evaluate (an access
// violation, not an error code).
struct SrModule
{
    HMODULE      mod      = nullptr;
    pfn_evaluate evaluate = nullptr;
    void*        hooked   = nullptr;   // the export address MinHook patched, once
};

constexpr int kMaxSrModules = 8;
SrModule g_sr_mods[kMaxSrModules]{};
int      g_sr_count = 0;

SrModule* sr_entry(HMODULE m)
{
    if (m == nullptr || g_real_get_proc == nullptr) { return nullptr; }
    for (int i = 0; i < g_sr_count; ++i) {
        if (g_sr_mods[i].mod == m) { return &g_sr_mods[i]; }
    }
    if (g_sr_count >= kMaxSrModules) { return nullptr; }

    SrModule& e = g_sr_mods[g_sr_count++];
    e.mod      = m;
    e.evaluate = reinterpret_cast<pfn_evaluate>(reinterpret_cast<void*>(
                     g_real_get_proc(m, "NVSDK_NGX_D3D12_EvaluateFeature")));
    return &e;
}

// The thunk dispatches on the *handle* first: whichever module the caller reached us
// through, a handle we created is ours. That is what makes a mixed-up module identity
// harmless instead of fatal.
template <int N>
NVSDK_NGX_Result NVSDK_CONV thunk_sr_evaluate(ID3D12GraphicsCommandList* list,
                                               const NVSDK_NGX_Handle* handle,
                                               const NVSDK_NGX_Parameter* params,
                                               void* callback)
{
    // One-shot trace: proves the hooked export is the function the upscaler really calls.
    static int told_called = 0;
    if (told_called < 3) {
        ++told_called;
        logf("sr: hooked Evaluate called (module #%d)\n", N);
    }
    if (provider_ours(handle)) {
        const unsigned ok = provider_evaluate(list, handle,
                                              const_cast<NVSDK_NGX_Parameter*>(params));
        return (ok != 0) ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
    }
    SrModule& m = g_sr_mods[N];
    if (m.evaluate == nullptr) {
        static bool told = false;
        if (!told) {
            told = true;
            logf("sr: module #%d has NO real EvaluateFeature - DLSS output would stay"
                 " unwritten (black scene with a healthy swapchain)\n", N);
        }
    }
    return forward_upscaler_evaluate(m.evaluate, list, handle, params, callback,
                                     NVSDK_NGX_Result_Success);
}

FARPROC sr_thunk_for(const SrModule* m)
{
    switch (static_cast<int>(m - g_sr_mods)) {
    case 0:  return reinterpret_cast<FARPROC>(&thunk_sr_evaluate<0>);
    case 1:  return reinterpret_cast<FARPROC>(&thunk_sr_evaluate<1>);
    case 2:  return reinterpret_cast<FARPROC>(&thunk_sr_evaluate<2>);
    case 3:  return reinterpret_cast<FARPROC>(&thunk_sr_evaluate<3>);
    case 4:  return reinterpret_cast<FARPROC>(&thunk_sr_evaluate<4>);
    case 5:  return reinterpret_cast<FARPROC>(&thunk_sr_evaluate<5>);
    case 6:  return reinterpret_cast<FARPROC>(&thunk_sr_evaluate<6>);
    default: return reinterpret_cast<FARPROC>(&thunk_sr_evaluate<7>);
    }
}


bool is_own_module(HMODULE m)
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&nvngx_install_if_possible), &self) ||
        self == nullptr) {
        return false;
    }
    return m == self;
}

// Kernel_Hooks.cpp hooks kernel32 *and* kernelbase for the same APIs (hk_K32_ /
// hk_KB_ pairs, each with its own original). Both matter: kernel32's export is only a
// jump thunk into kernelbase, so a caller that imports kernelbase!GetProcAddress
// directly never reaches the kernel32 patch. NVIDIA's own modules do exactly that, and
// once we were the NGX core module that meant NULL for every entry point we do not
// export ourselves - the DLSS runtime silently dropped out (measured: no SR evaluate
// for the whole session).
static FARPROC WINAPI get_proc_impl(HMODULE module, LPCSTR name, pfn_get_proc real)
{
    if (module == nullptr || name == nullptr) {
        return (real != nullptr) ? real(module, name) : nullptr;
    }

    // Ordinal exports pass a small integer as the "name" (Kernel_Hooks.cpp:115): strcmp on
    // it reads address ~5 and faults exactly like the GetModuleHandleExW mistake did.
    if (reinterpret_cast<uintptr_t>(name) < 0xF000u) {
        return (real != nullptr) ? real(module, name) : nullptr;
    }

    // Our own module was handed out for the NGX core name (LibraryLoad_Hooks: `return
    // dllModule`). Serve what we export (the provider's entries), and for everything else
    // - Init_ProjectID, D3D11/CUDA halves, helpers we do not implement - hand the driver's
    // real core function through, exactly like NVNGX_Proxy resolves its forward targets
    // from the real module.
    if (is_own_module(module)) {
        // Diagnostic: which NGX entries the SR runtime resolves from us. "DLSS is off in
        // the menu" and "DLSS is on but died inside our core" look identical from the FG
        // side; the init sequence reaching us is the only thing that tells them apart.
        if (strncmp(name, "NVSDK_NGX_D3D12_", 16) == 0) {
            static char asked[12][72]{};
            static int  asked_n = 0;
            bool known = false;
            for (int i = 0; i < asked_n; ++i) {
                if (strcmp(asked[i], name) == 0) { known = true; break; }
            }
            if (!known && asked_n < 12) {
                strncpy_s(asked[asked_n++], name, _TRUNCATE);
                logf("kernel: the NGX runtime asked us for %s\n", name);
            }
        }
        if (FARPROC own = (real != nullptr) ? real(module, name) : nullptr) { return own; }
        if (HMODULE core = ngx_real_core()) {
            return (real != nullptr) ? real(core, name) : nullptr;
        }
        return nullptr;
    }

    // The driver's NGX core comes first, and it has to: on this machine the DLSS-G
    // plugin and the DLSS-SR runtime both resolve their entry points from the same
    // `_nvngx.dll`, so one module answers for both features. Only the handle tells
    // them apart, and thunk_evaluate already dispatches on it (ours -> provider,
    // anything else -> harvest + forward). Routing the core into the upscaler lane
    // instead (the first version did, depending on call order) hands our own handle
    // to the real runtime.
    if (is_ngx_core(module)) {
        if (NgxModule* m = module_entry(module)) {
            if (FARPROC f = thunk_for(m, name)) {
                static int told = 0;
                if (told < 10) {
                    ++told;
                    logf("nvngx: GetProcAddress(%p, %s) -> our thunk\n", (void*)module, name);
                }
                return f;
            }
        }
        return (real != nullptr) ? real(module, name) : nullptr;
    }

    // OptiScaler's rule: the module that really exports NVSDK_NGX_D3D12_EvaluateFeature
    // is the one to watch. Resolve for real first - probes for modules that do not
    // export it (exe, ntdll, kernel32 ...) must fall through untouched. Without this
    // check the four slots filled up with non-exporters and the SR runtime never got
    // one (measured: the SR runtime here is an NGX model blob, not nvngx_dlss.dll).
    if (strcmp(name, "NVSDK_NGX_D3D12_EvaluateFeature") == 0) {
        FARPROC resolved = (real != nullptr) ? real(module, name) : nullptr;
        if (resolved == nullptr) { return nullptr; }

        // FgInput=DLSSG (PBZ): DLSS-SR must run untouched. Its Evaluate is what writes the
        // colour texture; intercepting it here without a resolvable real target leaves the
        // output unwritten (black scene). The DLSS-G lane harvests MV/depth instead.
        if (cfg().fg_input != static_cast<int>(FgInputSource::Upscaler)) { return resolved; }

        if (!is_own_module(module)) {
            if (SrModule* e = sr_entry(module)) {
                // Do NOT touch e->evaluate here: once MinHook has replaced the export,
                // that slot holds the trampoline. Overwriting it with the (now hooked)
                // export address makes the thunk call itself until the stack dies.
                static int told_any = 0;
                if (told_any < 6) {
                    ++told_any;
                    wchar_t path[MAX_PATH]{};
                    GetModuleFileNameW(module, path, MAX_PATH);
                    logf("nvngx: EvaluateFeature of %ls -> harvest thunk (real %p)\n",
                         path, (void*)resolved);
                }
                return sr_thunk_for(e);
            }
        }
        return resolved;
    }

    return (real != nullptr) ? real(module, name) : nullptr;
}

FARPROC WINAPI hook_get_proc(HMODULE module, LPCSTR name)
{
    return get_proc_impl(module, name, g_real_get_proc);
}

FARPROC WINAPI hook_get_proc_kb(HMODULE module, LPCSTR name)
{
    return get_proc_impl(module, name, g_real_get_proc_kb);
}

} // namespace

// Take back "frame generation is available". Called when the title's own inputs prove that
// XeFG cannot run there (DlssG_Dx12::Activate()'s low-res-MV / native-AA condition): a title
// that believes FG is on hands the picture to the provider and only clears the swap chain,
// which is a black screen when nothing generates. With the answers back at 0 the title
// composites its own frame and keeps presenting real ones at 1X.
void nvngx_withdraw_frame_generation()
{
    NVSDK_NGX_Parameter* p = g_patched_params;      // anon-namespace global, still alive
    if (p == nullptr) { return; }

    p->Set(NVSDK_NGX_Parameter_FrameGeneration_Available, 0);
    p->Set("FrameGeneration.FeatureInitResult", 0);
    p->Set("FrameInterpolation.Available", 0);
    p->Set(NVSDK_NGX_Parameter_FrameInterpolation_FeatureInitResult, 0);
    logf("nvngx: capability block withdrawn - FrameGeneration.Available=0\n");
}

void nvngx_install_if_possible()
{
    if (g_ngx_installed) { return; }
    // Not gated on NgxCoreProxy: the same hook also registers the upscaler lane (it is the
    // GetProcAddress layer OptiScaler's Upscaler_Inputs_Dx12 needs) - only the
    // "our own module" branch inside the hook belongs to the core impersonation.
    if (cfg().passthrough != 0) { return; }

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        logf("nvngx: MinHook init failed\n");
        return;
    }

    // kernel32 first, then kernelbase: they are separate code addresses, each needs its
    // own patch and its own trampoline. The kernel32 trampoline lands on the kernelbase
    // jump target, i.e. on the second patch, which terminates in the kernelbase
    // trampoline - one extra hop, never a loop (Kernel_Hooks.cpp does the same).
    int hooked = 0;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 != nullptr) {
        LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(k32, "GetProcAddress"));
        if (addr != nullptr && MH_CreateHook(addr, reinterpret_cast<LPVOID>(&hook_get_proc),
                                            reinterpret_cast<LPVOID*>(&g_real_get_proc)) == MH_OK) {
            ++hooked;
        }
    }
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    if (kb != nullptr) {
        LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(kb, "GetProcAddress"));
        if (addr != nullptr && MH_CreateHook(addr, reinterpret_cast<LPVOID>(&hook_get_proc_kb),
                                            reinterpret_cast<LPVOID*>(&g_real_get_proc_kb)) == MH_OK) {
            ++hooked;
        }
    }
    // One suspension for the batch (see hook_one in proxy_kernel.cpp).
    if (hooked > 0 && MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        logf("nvngx: MH_EnableHook(all) failed\n");
        hooked = 0;
    }
    if (hooked == 0) {
        logf("nvngx: could not hook GetProcAddress\n");
        return;
    }
    g_ngx_installed = true;
    logf("nvngx: GetProcAddress interposed on %d module(s) (kernel32/kernelbase, per-module"
         " thunks, DLSS-G only)\n", hooked);
}


bool nvngx_installed() { return g_ngx_installed; }

// The real GetProcAddress: used to resolve forward targets, never our own thunks.
void* ngx_real_get_proc(HMODULE m, const char* name)
{
    if (m == nullptr || name == nullptr || g_real_get_proc == nullptr) { return nullptr; }
    return reinterpret_cast<void*>(g_real_get_proc(m, name));
}

} // namespace fakerfg

namespace fakerfg {
namespace {

// ---------------------------------------------------------------- upscaler lane
// OptiScaler's CP2077 profile is FGInput = Upscaler: the MV/depth that reach XeFG are
// the DLSS-SR ones (two-channel MV, R32 depth, render resolution), not the DLSS-G
// parameter block's textures. Recorded on every forwarded Evaluate and used by the
// DLSS-G one that follows in the same frame.
// OptiScaler's Upscaler_Inputs_Dx12::UpscaleStart, which is the FGInput=Upscaler lane:
// the upscaler's own parameter block carries MV, depth and the frame's camera values -
// the MV/depth pair there is what XeFG wants (R32 depth, two-channel MV, render size).
struct SrInputs
{
    MgpuFgResource mv{};
    MgpuFgResource depth{};
    MgpuFgResource color{};   // NVSDK_NGX_Parameter_Output - the upscaled frame
    // IFeature::RenderWidth()/TargetWidth(): the upscaler's own declared sizes, from the
    // NGX parameter block (IFeature.cpp:120-165). Render is min(Width, OutWidth), the
    // target/display size is the larger of the two.
    unsigned       render_w   = 0;
    unsigned       render_h   = 0;
    unsigned       target_w   = 0;
    unsigned       target_h   = 0;
    float          jitter_x   = 0.0f;
    float          jitter_y   = 0.0f;
    float          mv_scale_x = 0.0f;
    float          mv_scale_y = 0.0f;
    uint32_t       reset      = 0;
    uint64_t       tick       = 0;
    uint32_t       stamp      = 0;
    void*          cmd        = nullptr;   // present counter at harvest time (frame-based gate)
    bool           valid      = false;
};

SrInputs g_sr{};
uint32_t g_sr_harvests = 0;   // monotonic: how often the upscaler actually evaluated

// OptiScaler [Hotfix] MotionVectorResourceBarrier / DepthResourceBarrier:
// `.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)`.
uint32_t mv_state()
{
    return static_cast<uint32_t>(cfg().mv_state != 0 ? cfg().mv_state
                                                    : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

uint32_t depth_state()
{
    return static_cast<uint32_t>(cfg().depth_state != 0 ? cfg().depth_state
                                                       : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// The upscaler's output has no declared state anywhere in its parameter block, and a
// wrong StateBefore is not cosmetic (it removes the device). UINT_MAX is what OptiScaler
// hands the library when a tag carries no state at all, so that is the default; ColorState
// exists for a title where that is not enough.
uint32_t color_state()
{
    return (cfg().color_state != 0) ? static_cast<uint32_t>(cfg().color_state) : 0xFFFFFFFFu;
}

void fill_from(NVSDK_NGX_Parameter* p, const char* name, MgpuFgResource& out,
               uint32_t declared_state)
{
    ID3D12Resource* r = nullptr;
    if (p->Get(name, &r) != NVSDK_NGX_Result_Success || r == nullptr) { return; }
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    out.resource = r;
    out.width    = static_cast<uint32_t>(d.Width);
    out.height   = d.Height;
    out.format   = static_cast<uint32_t>(d.Format);
    // Upscaler lane: the parameter block carries no state, so OptiScaler declares
    // MVResourceBarrier / DepthResourceBarrier (`.value_or(NON_PIXEL_SHADER_RESOURCE)`).
    out.state    = declared_state;
    // ... and validity `ValidNow` for both inputs.
    out.validity = MGPU_FG_RV_ONLY_NOW;
}

float get_f32(NVSDK_NGX_Parameter* p, const char* name)
{
    float v = 0.0f;
    if (p->Get(name, &v) == NVSDK_NGX_Result_Success) { return v; }
    return 0.0f;
}

uint32_t get_u32(NVSDK_NGX_Parameter* p, const char* name)
{
    unsigned v = 0;
    if (p->Get(name, &v) == NVSDK_NGX_Result_Success) { return v; }
    return 0;
}

} // namespace

void sr_harvest(void* ngx_params)
{
    auto* p = static_cast<NVSDK_NGX_Parameter*>(ngx_params);
    if (p == nullptr) { return; }

    MgpuFgResource mv{}, depth{};
    fill_from(p, NVSDK_NGX_Parameter_MotionVectors, mv, mv_state());
    fill_from(p, NVSDK_NGX_Parameter_Depth, depth, depth_state());
    if (mv.resource == nullptr || depth.resource == nullptr) {
        static int misses = 0;
        if (misses < 3) {
            ++misses;
            logf("sr: evaluate seen but mv=%p depth=%p - not an upscaler block?\n",
                 mv.resource, depth.resource);
        }
        return;
    }

    g_sr.mv         = mv;
    g_sr.depth      = depth;

    // The colour for this lane: OptiScaler's Upscaler_Inputs_Dx12::UpscaleEnd reads
    // NVSDK_NGX_Parameter_Output into `info.type = UAV` and hands it to the FG feature.
    // It has to: with FGInput=Upscaler the DLSS-G block carries no usable colour (that
    // block's HUDless is the pre-upscale image), the upscaled frame *is* this texture.
    // Without it XeFG interpolates from a buffer nobody writes - a black picture at a
    // doubled frame rate, which is exactly what was measured.
    fill_from(p, NVSDK_NGX_Parameter_Output, g_sr.color, color_state());
    if (g_sr.color.resource == nullptr) { g_sr.color = MgpuFgResource{}; }
    xefg_set_marker_color(g_sr.color.resource, g_sr.color.format, g_sr.color.state,
                          g_sr.color.width, g_sr.color.height);

    g_sr.jitter_x   = get_f32(p, NVSDK_NGX_Parameter_Jitter_Offset_X);
    g_sr.jitter_y   = get_f32(p, NVSDK_NGX_Parameter_Jitter_Offset_Y);
    g_sr.mv_scale_x = get_f32(p, NVSDK_NGX_Parameter_MV_Scale_X);
    g_sr.mv_scale_y = get_f32(p, NVSDK_NGX_Parameter_MV_Scale_Y);
    g_sr.reset      = get_u32(p, NVSDK_NGX_Parameter_Reset);

    // IFeature.cpp:120-165, verbatim: the sizes come from the upscaler's parameter block,
    // not from the resource descriptions. Render is the smaller of (Width, OutWidth), the
    // target/display size the larger one; values outside 20..16384 are dropped (Crytek's
    // checks) and Out* missing means there is no upscaling in this block.
    {
        unsigned w  = get_u32(p, NVSDK_NGX_Parameter_Width);
        unsigned h  = get_u32(p, NVSDK_NGX_Parameter_Height);
        unsigned ow = get_u32(p, NVSDK_NGX_Parameter_OutWidth);
        unsigned oh = get_u32(p, NVSDK_NGX_Parameter_OutHeight);
        if (w  > 16384 || w  < 20) { w  = 0; }
        if (h  > 16384 || h  < 20) { h  = 0; }
        if (ow > 16384 || ow < 20) { ow = 0; }
        if (oh > 16384 || oh < 20) { oh = 0; }
        if (ow != 0 && oh != 0) {
            g_sr.target_w = (w > ow) ? w : ow;
            g_sr.target_h = (h > oh) ? h : oh;
            g_sr.render_w = (w < ow) ? w : ow;
            g_sr.render_h = (h < oh) ? h : oh;
        } else {
            // No Out* in this block: no upscaling to describe. Fall back to the resources
            // (depth = render, upscaler output = target) so the lane still has sizes.
            g_sr.render_w = g_sr.depth.width;
            g_sr.render_h = g_sr.depth.height;
            g_sr.target_w = g_sr.color.width;
            g_sr.target_h = g_sr.color.height;
        }
    }

    ++g_sr_harvests;
    g_sr.tick       = GetTickCount64();
    g_sr.stamp      = g_present_counter;
    g_sr.valid      = true;

    // IFeature.cpp:109-116: with FGInput=Upscaler the three XeFG init flags are the
    // UPSCALER's create flags, not our ini defaults -
    //     FGXeFGDepthInverted = _initFlags.DepthInverted;
    //     FGXeFGJitteredMV    = _initFlags.JitteredMV;
    //     FGXeFGHighResMV     = !_initFlags.LowResMV;
    // and OptiScaler saves them (Config::SaveXeFG) because XeFG's context already exists
    // here. Guessing them instead (MvJittered=0/HighResMv=0) hands XeFG motion vectors in
    // the wrong space and jitter it does not expect - the picture it produces is black.
    unsigned long long create_flags = 0;
    if (p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &create_flags) ==
        NVSDK_NGX_Result_Success) {
        static bool told_flags = false;
        if (!told_flags) {
            told_flags = true;
            const int depth_inverted =
                (create_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) ? 1 : 0;
            const int mv_jittered =
                (create_flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) ? 1 : 0;
            const int high_res_mv =
                (create_flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) ? 0 : 1;
            logf("sr: upscaler create flags 0x%llX -> DepthInverted=%d MvJittered=%d"
                 " HighResMv=%d (HDR=%d)\n",
                 create_flags, depth_inverted, mv_jittered, high_res_mv,
                 (create_flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) ? 1 : 0);
            cfg_save_upscaler_flags(depth_inverted, mv_jittered, high_res_mv);
        }
    }

    static int told = 0;
    if (told < 3) {
        ++told;
        logf("sr: upscaler lane render=%ux%u target=%ux%u mv %ux%u fmt=%u depth %ux%u fmt=%u"
             " color %p (%ux%u fmt=%u) jitter=(%.3f,%.3f) mvScale=(%.3f,%.3f) reset=%u\n",
             g_sr.render_w, g_sr.render_h, g_sr.target_w, g_sr.target_h,
             mv.width, mv.height, mv.format, depth.width, depth.height, depth.format,
             g_sr.color.resource, g_sr.color.width, g_sr.color.height, g_sr.color.format,
             g_sr.jitter_x, g_sr.jitter_y, g_sr.mv_scale_x, g_sr.mv_scale_y, g_sr.reset);
    }
}

// Upscaler_Inputs_Dx12::UpscaleStart's effect, applied to the frame the DLSS-G Evaluate
// is about to hand to XeFG: inputs, render size and the camera values come from the
// upscaler, everything else stays as the DLSS-G block declared it.
bool sr_lane_ready()
{
    // Frame-based, exactly like XeFG_Dx12::Present()'s `> 3` tolerance: the harvest is
    // accepted for the frame it arrived on plus the next three presents, so a title that
    // evaluates the upscaler on its render thread (and skips a present here and there)
    // keeps generation on, while a genuinely dead lane falls out after four presents.
    if (!g_sr.valid) { return false; }
    return (g_present_counter - g_sr.stamp) <= 3;
}

bool sr_apply(MgpuFgFrame& f)
{
    // "Fresh" is the whole point: Upscaler_Inputs_Dx12 tags MV/depth inside the upscaler's
    // own Evaluate (UpscaleStart/UpscaleEnd), so a frame without one simply has no inputs
    // and XeFG_Dx12::Dispatch's `IsReady(Depth) && IsReady(Velocity)` gate skips it.
    // Keeping the last successful harvest instead (this was a plain `g_sr.valid` flag)
    // hands the library resources from the previous scene - after a level transition they
    // are destroyed or reused, and the interpolated frames come out black. That is exactly
    // the loading-screen black screen.
    if (!g_sr.valid || !sr_lane_ready()) {
        static bool told = false;
        if (g_sr.valid && !told) {
            told = true;
            logf("sr: upscaler lane is stale - no inputs this frame, XeFG stays at 1X\n");
        }
        return false;
    }

    f.stream[MGPU_FG_STREAM_MV]    = g_sr.mv;
    f.stream[MGPU_FG_STREAM_DEPTH] = g_sr.depth;

    // NO colour in this lane, on purpose: Upscaler_Inputs_Dx12 hands the upscaler's Output
    // over only in UpscaleEnd and only when FGHUDFix is on (Config.h:671, default false);
    // its CP2077 profile disables the HUD fix outright. Without it XeFG interpolates its own
    // back buffer, which is where the title renders because it was handed our chain.

    // Sizes, IFeature::RenderWidth()/TargetWidth(): the upscaler's own numbers. These are
    // what Upscaler_Inputs_Dx12 uses for the velocity and depth tags.
    if (g_sr.render_w >= 8) {
        f.render_w = g_sr.render_w;
        f.render_h = g_sr.render_h;
    } else if (g_sr.depth.width >= 8) {
        f.render_w = g_sr.depth.width;
        f.render_h = g_sr.depth.height;
    }
    if (g_sr.target_w >= 8) {
        f.output_w = g_sr.target_w;
        f.output_h = g_sr.target_h;
    } else if (g_sr.color.width >= 8) {
        f.output_w = g_sr.color.width;
        f.output_h = g_sr.color.height;
    }
    f.jitter_x   = g_sr.jitter_x;
    f.jitter_y   = g_sr.jitter_y;
    f.mv_scale_x = g_sr.mv_scale_x;
    f.mv_scale_y = g_sr.mv_scale_y;

    if (g_sr.reset != 0) { f.flags |= MGPU_FG_FRAME_RESET_HISTORY; }

    // UpscalerInputs::GetCameraValues: the camera comes from the configuration, not from
    // the title, and `if (feature->DepthInverted()) std::swap(near, far)`. The aspect is
    // DisplayWidth / DisplayHeight (UpscaleStart).
    f.camera_near = cfg().camera_near;
    f.camera_far  = cfg().camera_far;
    if (f.flags & MGPU_FG_FRAME_DEPTH_INVERTED) { std::swap(f.camera_near, f.camera_far); }
    f.camera_fov_y = cfg().vertical_fov_deg * 3.14159265358979f / 180.0f;
    const float out_w = static_cast<float>(g_width);
    const float out_h = static_cast<float>(g_height);
    if (out_w > 0.0f && out_h > 0.0f) { f.aspect = out_w / out_h; }
    return true;
}

} // namespace fakerfg

namespace fakerfg {
namespace {

// ---------------------------------------------------------------- upscaler export hooks
// GetProcAddress interposition is not enough: `sl.dlss.dll` loads its NGX runtime
// implicitly (import table), so the Evaluate we need to watch is never resolved through
// GetProcAddress. OptiScaler's Upscaler_Inputs_Dx12 hooks the *export* of each upscaler
// module instead; this is the same move.
//
// It runs on the loader thread ONLY. Installing a hook suspends every other thread
// (MinHook does that so the patch is atomic); doing it from inside Present, while
// another thread sits in a D3D12/DXGI call holding runtime locks, is a deadlock - and
// if the patch lands under an executing thread it is a crash. That was one of the two
// crash sites in the first live run.
bool name_is_upscaler(const wchar_t* path)
{
    if (path == nullptr) { return false; }
    return wcsstr(path, L"nvngx_dlss.dll") != nullptr ||
           wcsstr(path, L"nvngx_dlssd.dll") != nullptr ||
           wcsstr(path, L"nvngx_dlssnr.dll") != nullptr;
}

bool in_dlssg_table(HMODULE m)
{
    for (int i = 0; i < g_mod_count; ++i) {
        if (g_mods[i].mod == m) { return true; }
    }
    return false;
}

} // namespace

// Called periodically from the loader thread (and once from fg_init). Never from the
// present path.
// Hook one module's NVSDK_NGX_D3D12_EvaluateFeature export. Called for every module the
// scan sees, and for every module LoadLibraryExW brings in (so an NGX runtime is hooked
// while the loader is still returning, before its caller can cache the pointer).
void hook_module_exports(HMODULE m)
{
    if (m == nullptr || is_own_module(m)) { return; }
    // DLSSG lane: never hook DLSS-SR modules. PBZ's own nvngx_dlss.dll cannot be
    // MinHook-ed (status 3) and the fallback real pointer is null -> its output texture
    // was never written, which is a black picture with a healthy swapchain.
    if (cfg().fg_input != static_cast<int>(FgInputSource::Upscaler)) { return; }
    if (g_sr_count >= kMaxSrModules) { return; }

    // A module already in the table may still be unhooked: sl.dlss.dll imports
    // NVSDK_NGX_D3D12_EvaluateFeature through its import table, so the thunks we hand out
    // through GetProcAddress are never used for DLSS-SR. Hooking the export is the only way
    // to see those calls (OptiScaler hooks module exports for exactly this reason).
    SrModule* known = nullptr;
    for (int k = 0; k < g_sr_count; ++k) {
        if (g_sr_mods[k].mod == m) { known = &g_sr_mods[k]; }
    }
    if (known != nullptr && known->hooked != nullptr) { return; }

    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(m, path, MAX_PATH) == 0) { return; }

    void* addr = (g_real_get_proc != nullptr)
                     ? reinterpret_cast<void*>(g_real_get_proc(m, "NVSDK_NGX_D3D12_EvaluateFeature"))
                     : nullptr;
    if (addr == nullptr) { return; }

    SrModule* entry = sr_entry(m);
    if (entry == nullptr) { return; }

    // A module may FORWARD its NGX exports to us - we are the module deployed as
    // nvngx_dlssg.dll, so GetProcAddress answers with one of our own thunks. Hooking that
    // (or keeping it as "the real function") makes our thunk call itself: measured on
    // Wuchang as STACK_OVERFLOW (0xC00000FD) with cfg() in the failing frame, right after
    // `MinHook could not hook ... (status 3 / MH_ERROR_ALREADY_CREATED)`. Such a module
    // needs no hook at all; the call is already ours, and the real implementation lives in
    // the driver's NGX core.
    {
        HMODULE owner = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(addr), &owner) &&
            owner != nullptr && is_own_module(owner)) {
            static bool told_self = false;
            if (!told_self) {
                told_self = true;
                logf("nvngx: %ls forwards Evaluate to us - no hook, forwarding to the real"
                     " NGX core\n", path);
            }
            if (g_real_get_proc != nullptr) {
                if (HMODULE core = ngx_real_core()) {
                    entry->evaluate = reinterpret_cast<pfn_evaluate>(
                        reinterpret_cast<void*>(g_real_get_proc(core,
                            "NVSDK_NGX_D3D12_EvaluateFeature")));
                }
            }
            return;
        }
    }

    const bool named = name_is_upscaler(path);
    entry->evaluate = reinterpret_cast<pfn_evaluate>(addr);

    // Two modules may forward to the same implementation, which means the same export
    // address. MinHook answers MH_ERROR_ALREADY_CREATED then and does NOT hand back a
    // trampoline - keeping `addr` would make our thunk jump into the already-installed
    // detour (recursion until the stack dies).
    void* other = nullptr;
    for (int k = 0; k < g_sr_count; ++k) {
        if (&g_sr_mods[k] != entry && g_sr_mods[k].hooked == addr) {
            other = reinterpret_cast<void*>(g_sr_mods[k].evaluate);
        }
    }
    if (other != nullptr) {
        entry->evaluate = reinterpret_cast<pfn_evaluate>(other);
        logf("nvngx: %ls shares its Evaluate with an already hooked module - forwarding\n", path);
        return;
    }

    void* trampoline = addr;
    const MH_STATUS created =
        MH_CreateHook(addr, reinterpret_cast<LPVOID>(sr_thunk_for(entry)), &trampoline);
    if (created != MH_OK) {
        // Never keep `addr` here: MinHook's MH_ERROR_ALREADY_CREATED means that address is
        // already patched, and calling it would land in a detour (ours, most likely) instead
        // of the upscaler. The driver's NGX core is the only implementation that is never
        // patched by us.
        entry->evaluate = nullptr;
        if (g_real_get_proc != nullptr) {
            if (HMODULE core = ngx_real_core()) {
                entry->evaluate = reinterpret_cast<pfn_evaluate>(reinterpret_cast<void*>(
                    g_real_get_proc(core, "NVSDK_NGX_D3D12_EvaluateFeature")));
            }
        }
        // PBZ ships its own nvngx_dlss.dll and the driver core lookup can come back empty.
        // Returning null made our thunk answer Success without running DLSS-SR (black
        // picture). Fall back to the module's own export; if that address is already
        // patched by another entry's hook, use that entry's trampoline instead.
        if (entry->evaluate == nullptr) {
            for (int k = 0; k < g_sr_count; ++k) {
                if (&g_sr_mods[k] != entry && g_sr_mods[k].hooked == addr &&
                    g_sr_mods[k].evaluate != nullptr) {
                    entry->evaluate = g_sr_mods[k].evaluate;
                    break;
                }
            }
        }
        if (entry->evaluate == nullptr) {
            entry->evaluate = reinterpret_cast<pfn_evaluate>(addr);
        }
        logf("nvngx: MinHook could not hook Evaluate in %ls (status %d) - forwarding to the"
             " real module export %p\n", path, static_cast<int>(created),
             reinterpret_cast<void*>(entry->evaluate));
        return;
    }
    entry->evaluate = reinterpret_cast<pfn_evaluate>(trampoline);
    if (MH_EnableHook(addr) != MH_OK) {
        logf("nvngx: could not enable the upscaler Evaluate hook in %ls\n", path);
        return;
    }
    entry->hooked = addr;
    logf("nvngx: export hooked (%s, evaluate @ %p): %ls\n", named ? "upscaler" : "other",
         addr, path);
}

void nvngx_scan_upscalers()
{
    if (!g_ngx_installed) { return; }

    static uint64_t last_scan = 0;
    const uint64_t now = GetTickCount64();
    if (last_scan != 0 && (now - last_scan) < 500) { return; }
    last_scan = now;
    if (g_sr_count >= kMaxSrModules) { return; }

    HMODULE mods[256]{};
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        static bool told_fail = false;
        if (!told_fail) {
            told_fail = true;
            logf("nvngx scan: EnumProcessModules failed (%lu)\n", GetLastError());
        }
        return;
    }
    {
        static unsigned scans = 0;
        if (scans < 6) {
            ++scans;
            const size_t n = needed / sizeof(HMODULE);
            unsigned exporters = 0;
            for (size_t i = 0; i < n; ++i) {
                if (GetProcAddress(mods[i], "NVSDK_NGX_D3D12_EvaluateFeature") != nullptr) {
                    ++exporters;
                }
            }
            logf("nvngx scan #%u: %u modules, %u export EvaluateFeature\n", scans,
                 static_cast<unsigned>(n), exporters);
        }
    }
    const size_t count = needed / sizeof(HMODULE);

    for (size_t i = 0; i < count; ++i) { hook_module_exports(mods[i]); }
}

// Called from the module-load hook (proxy_kernel.cpp) for every loaded module: OptiScaler
// hooks a runtime's NGX exports when that module loads (LibraryLoad_Hooks), it does not
// scan the process on a timer.
void nvngx_module_loaded(HMODULE m)
{
    if (!g_ngx_installed) { return; }
    hook_module_exports(m);
}

// Diagnostic: the number of upscaler Evaluates seen so far. Comparing its growth with the
// present count is what tells "the lane is sparse" (and the time window is wrong) from
// "the lane is dead" without guessing.
unsigned sr_harvest_count() { return g_sr_harvests; }

// The command list the upscaler evaluated on. OptiScaler tags MV/depth with it and ValidNow
// validity (Upscaler_Inputs_Dx12.cpp:167-199: `res.cmdList = InCmdList;
// res.validity = ValidNow;`). Without a list only UntilPresent is legal, which is what we
// were doing - and that is the difference that leaves XeFG with nothing to composite.
void sr_note_command_list(void* cmd_list)
{
    g_sr.cmd = cmd_list;
}

void* sr_command_list() { return g_sr.cmd; }

} // namespace fakerfg
