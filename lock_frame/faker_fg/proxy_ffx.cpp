// ============================================================================
//  proxy_ffx.cpp - the FSR (FFX API) input lane.
//
//  FSR 3.1 titles call `ffxDispatch` with
//  FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE. That descriptor carries the
//  render-size motion vectors and depth (and camera/jitter), i.e. exactly the frame
//  record the producer needs. We only observe that dispatch: the original FFX call is
//  always forwarded unchanged, so the title's own FSR pipeline keeps working.
// ============================================================================
#include "fg_internal.h"

#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_api_types.h"
#include "ffx_api/ffx_framegeneration.h"
#include "ffx_api/dx12/ffx_api_dx12.h"
#include "FidelityFX/host/ffx_frameinterpolation.h"

#include <MinHook.h>
#include <psapi.h>

namespace fakerfg {
namespace {

using PFN_ffxDispatch = ffxReturnCode_t (*)(ffxContext*, const ffxDispatchDescHeader*);

PFN_ffxDispatch g_ffx_dispatch = nullptr;
PFN_ffxDispatch o_ffx_dispatch = nullptr;
bool g_ffx_hook_tried = false;

bool ffx_module_name(const wchar_t* path)
{
    if (path == nullptr) { return false; }
    const wchar_t* base = wcsrchr(path, L'\\');
    base = (base != nullptr) ? base + 1 : path;
    const wchar_t* names[] = {
        L"amd_fidelityfx_framegeneration_dx12.dll",
        L"amd_fidelityfx_loader_dx12.dll",
        L"ffx_framegeneration_dx12.dll",
        L"amd_fidelityfx_framegeneration.dll",
    };
    for (const wchar_t* n : names) {
        if (_wcsicmp(base, n) == 0) { return true; }
    }
    return false;
}

uint32_t map_state(uint32_t state)
{
    switch (state) {
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_READ: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_COPY_SRC: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case FFX_API_RESOURCE_STATE_COPY_DEST: return D3D12_RESOURCE_STATE_COPY_DEST;
    case FFX_API_RESOURCE_STATE_GENERIC_READ: return D3D12_RESOURCE_STATE_GENERIC_READ;
    case FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT:
        return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET:
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

void fill_resource(MgpuFgResource& out, const FfxApiResource& res)
{
    out = MgpuFgResource{};
    out.resource = res.resource;
    out.state = map_state(res.state);
    out.validity = MGPU_FG_RV_ONLY_NOW;
    out.width = res.description.width;
    out.height = res.description.height;
    if (res.resource == nullptr) { return; }
    const D3D12_RESOURCE_DESC d =
        static_cast<ID3D12Resource*>(res.resource)->GetDesc();
    out.width = static_cast<uint32_t>(d.Width);
    out.height = d.Height;
    out.format = static_cast<uint32_t>(d.Format);
}

ID3D12Device* resource_device(const MgpuFgResource& r)
{
    if (r.resource == nullptr) { return nullptr; }
    ID3D12Device* device = nullptr;
    if (FAILED(static_cast<ID3D12Resource*>(r.resource)->GetDevice(
            IID_PPV_ARGS(&device)))) {
        return nullptr;
    }
    return device;   // released after publish_record
}

void capture_prepare(const ffxDispatchDescFrameGenerationPrepare* p)
{
    if (p == nullptr) { return; }
    MgpuFgFrame f = make_empty_record();
    fill_resource(f.stream[MGPU_FG_STREAM_MV], p->motionVectors);
    fill_resource(f.stream[MGPU_FG_STREAM_DEPTH], p->depth);
    if (f.stream[MGPU_FG_STREAM_MV].resource == nullptr &&
        f.stream[MGPU_FG_STREAM_DEPTH].resource == nullptr) {
        return;
    }


    f.cmd_list = p->commandList;
    f.mode = api_mode();
    f.frame_qpc_ms = now_qpc_ms();
    f.declared_frames = 1;
    f.render_w = p->renderSize.width;
    f.render_h = p->renderSize.height;
    if (f.render_w == 0) { f.render_w = f.stream[MGPU_FG_STREAM_MV].width; }
    if (f.render_h == 0) { f.render_h = f.stream[MGPU_FG_STREAM_MV].height; }
    f.output_w = f.render_w;
    f.output_h = f.render_h;

    f.jitter_x = p->jitterOffset.x;
    f.jitter_y = p->jitterOffset.y;
    f.mv_scale_x = p->motionVectorScale.x;
    f.mv_scale_y = p->motionVectorScale.y;
    f.camera_near = p->cameraNear;
    f.camera_far = p->cameraFar;
    f.camera_fov_y = p->cameraFovAngleVertical;

    uint32_t flags = MGPU_FG_FRAME_FROM_FG;
    if (f.stream[MGPU_FG_STREAM_MV].resource != nullptr) { flags |= MGPU_FG_FRAME_MV_ACTIVE; }
    if (p->cameraNear > 0.0f && p->cameraFar > p->cameraNear) {
        flags |= MGPU_FG_FRAME_HAS_CAMERA;
    }
    if (cfg().depth_inverted != 0) { flags |= MGPU_FG_FRAME_DEPTH_INVERTED; }
    if (cfg().mv_jittered != 0)    { flags |= MGPU_FG_FRAME_MV_JITTERED; }
    // Dispatch-desc command list: the title is still recording into it.
    f.flags = flags | MGPU_FG_FRAME_CMD_OPEN;

    const uint32_t present_id = xefg_next_frame_id();
    f.present_id = present_id;
    ID3D12Device* dev = resource_device(
        f.stream[MGPU_FG_STREAM_MV].resource != nullptr
            ? f.stream[MGPU_FG_STREAM_MV] : f.stream[MGPU_FG_STREAM_DEPTH]);
    f.device = dev;
    publish_record(f);
    if (cfg().fg_output == static_cast<int>(FgOutputSink::XeFG)) { fg_output_submit(f); }
    if (dev != nullptr) { dev->Release(); }

    static unsigned logged = 0;
    if (logged < 3 || (++logged % 300) == 0) {
        if (logged <= 3) { ++logged; }
        logf("ffx: prepare id=%llu render=%ux%u mv=%p fmt=%u depth=%p fmt=%u list=%p\n",
             static_cast<unsigned long long>(p->frameID), f.render_w, f.render_h,
             f.stream[MGPU_FG_STREAM_MV].resource, f.stream[MGPU_FG_STREAM_MV].format,
             f.stream[MGPU_FG_STREAM_DEPTH].resource, f.stream[MGPU_FG_STREAM_DEPTH].format,
             f.cmd_list);
    }
}

ffxReturnCode_t hk_ffx_dispatch(ffxContext* context, const ffxDispatchDescHeader* desc)
{
    if (desc != nullptr && cfg().fg_input == static_cast<int>(FgInputSource::FSR)) {
        if (desc->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE) {
            capture_prepare(reinterpret_cast<const ffxDispatchDescFrameGenerationPrepare*>(desc));
        }
    }
    return (o_ffx_dispatch != nullptr) ? o_ffx_dispatch(context, desc)
                                       : FFX_API_RETURN_ERROR;
}

// FSR 3.0 / early 3.1 titles use the older frame-interpolation dispatch instead of the
// FFX API prepare descriptor. The fields are the same inputs under different names.
using PFN_ffx_interp = FfxErrorCode (*)(FfxFrameInterpolationContext*,
                                        const FfxFrameInterpolationDispatchDescription*);
PFN_ffx_interp o_ffx_interp = nullptr;

uint32_t map_legacy_state(FfxResourceStates state)
{
    switch (state) {
    case FFX_RESOURCE_STATE_UNORDERED_ACCESS: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_RESOURCE_STATE_COMPUTE_READ: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_RESOURCE_STATE_PIXEL_READ: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_RESOURCE_STATE_COPY_SRC: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case FFX_RESOURCE_STATE_COPY_DEST: return D3D12_RESOURCE_STATE_COPY_DEST;
    case FFX_RESOURCE_STATE_GENERIC_READ: return D3D12_RESOURCE_STATE_GENERIC_READ;
    case FFX_RESOURCE_STATE_INDIRECT_ARGUMENT:
        return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case FFX_RESOURCE_STATE_RENDER_TARGET:
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

void fill_legacy_resource(MgpuFgResource& out, const FfxResource& res)
{
    out = MgpuFgResource{};
    out.resource = res.resource;
    out.state = map_legacy_state(res.state);
    out.validity = MGPU_FG_RV_ONLY_NOW;
    out.width = res.description.width;
    out.height = res.description.height;
    if (res.resource == nullptr) { return; }
    const D3D12_RESOURCE_DESC d =
        static_cast<ID3D12Resource*>(res.resource)->GetDesc();
    out.width = static_cast<uint32_t>(d.Width);
    out.height = d.Height;
    out.format = static_cast<uint32_t>(d.Format);
}

void capture_legacy(const FfxFrameInterpolationDispatchDescription* p)
{
    if (p == nullptr) { return; }
    MgpuFgFrame f = make_empty_record();
    fill_legacy_resource(f.stream[MGPU_FG_STREAM_MV], p->dilatedMotionVectors);
    fill_legacy_resource(f.stream[MGPU_FG_STREAM_DEPTH], p->dilatedDepth);
    fill_legacy_resource(f.stream[MGPU_FG_STREAM_BACKBUFFER], p->currentBackBuffer);
    fill_legacy_resource(f.stream[MGPU_FG_STREAM_HUDLESS], p->currentBackBuffer_HUDLess);
    if (f.stream[MGPU_FG_STREAM_MV].resource == nullptr &&
        f.stream[MGPU_FG_STREAM_DEPTH].resource == nullptr) {
        return;
    }


    f.cmd_list = p->commandList;
    f.mode = api_mode();
    f.frame_qpc_ms = now_qpc_ms();
    f.declared_frames = 1;
    f.render_w = p->renderSize.width;
    f.render_h = p->renderSize.height;
    f.output_w = p->displaySize.width ? p->displaySize.width : f.render_w;
    f.output_h = p->displaySize.height ? p->displaySize.height : f.render_h;
    f.camera_near = p->cameraNear;
    f.camera_far = p->cameraFar;
    f.camera_fov_y = p->cameraFovAngleVertical;

    uint32_t flags = MGPU_FG_FRAME_FROM_FG;
    if (f.stream[MGPU_FG_STREAM_MV].resource != nullptr) { flags |= MGPU_FG_FRAME_MV_ACTIVE; }
    if (p->cameraNear > 0.0f && p->cameraFar > p->cameraNear) {
        flags |= MGPU_FG_FRAME_HAS_CAMERA;
    }
    if (cfg().depth_inverted != 0) { flags |= MGPU_FG_FRAME_DEPTH_INVERTED; }
    if (p->reset) { flags |= MGPU_FG_FRAME_RESET_HISTORY; }
    // Legacy interpolation desc: same open-list promise as the Dispatch desc above.
    f.flags = flags | MGPU_FG_FRAME_CMD_OPEN;

    const uint32_t present_id = xefg_next_frame_id();
    f.present_id = present_id;
    ID3D12Device* dev = resource_device(
        f.stream[MGPU_FG_STREAM_MV].resource != nullptr
            ? f.stream[MGPU_FG_STREAM_MV] : f.stream[MGPU_FG_STREAM_DEPTH]);
    f.device = dev;
    publish_record(f);
    if (cfg().fg_output == static_cast<int>(FgOutputSink::XeFG)) { fg_output_submit(f); }
    if (dev != nullptr) { dev->Release(); }
}

FfxErrorCode hk_ffx_interp(FfxFrameInterpolationContext* context,
                           const FfxFrameInterpolationDispatchDescription* desc)
{
    if (desc != nullptr && cfg().fg_input == static_cast<int>(FgInputSource::FSR)) { capture_legacy(desc); }
    return (o_ffx_interp != nullptr) ? o_ffx_interp(context, desc) : FFX_ERROR_INVALID_POINTER;
}

} // namespace

void ffx_module_loaded(HMODULE m)
{
    if (m == nullptr || g_ffx_hook_tried) { return; }
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(m, path, MAX_PATH) == 0 || !ffx_module_name(path)) { return; }

    auto* fn = reinterpret_cast<PFN_ffxDispatch>(GetProcAddress(m, "ffxDispatch"));
    auto* interp = reinterpret_cast<PFN_ffx_interp>(
        GetProcAddress(m, "ffxFrameInterpolationDispatch"));
    if (fn == nullptr && interp == nullptr) {
        logf("ffx: %ls has neither ffxDispatch nor ffxFrameInterpolationDispatch\n", path);
        return;
    }
    g_ffx_hook_tried = true;
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        logf("ffx: MH_Initialize failed %d\n", static_cast<int>(st));
        return;
    }
    if (fn != nullptr) {
        o_ffx_dispatch = fn;
        const MH_STATUS c = MH_CreateHook(fn, reinterpret_cast<void*>(&hk_ffx_dispatch),
                                          reinterpret_cast<void**>(&o_ffx_dispatch));
        if (c == MH_OK) {
            MH_EnableHook(fn);
            g_ffx_dispatch = fn;
            logf("ffx: hooked %ls!ffxDispatch\n", path);
        } else {
            logf("ffx: MH_CreateHook(ffxDispatch) failed %d\n", static_cast<int>(c));
        }
    }
    if (interp != nullptr) {
        o_ffx_interp = interp;
        const MH_STATUS c = MH_CreateHook(interp, reinterpret_cast<void*>(&hk_ffx_interp),
                                          reinterpret_cast<void**>(&o_ffx_interp));
        if (c == MH_OK) {
            MH_EnableHook(interp);
            logf("ffx: hooked %ls!ffxFrameInterpolationDispatch\n", path);
        } else {
            logf("ffx: MH_CreateHook(ffxFrameInterpolationDispatch) failed %d\n",
                 static_cast<int>(c));
        }
    }
}

void ffx_scan_loaded_modules()
{
    HMODULE mods[256]{};
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) { return; }
    const DWORD n = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < n && i < 256; ++i) { ffx_module_loaded(mods[i]); }
}

} // namespace fakerfg
