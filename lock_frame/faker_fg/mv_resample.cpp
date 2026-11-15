// ============================================================================
//  mv_resample.cpp - full-resolution motion vectors onto the interpolation size.
//
//  Wuchang hands the upscaler `R16G16_FLOAT` motion vectors sized like the *output*
//  (its DLSS create flags leave MVLowRes clear: 0x69, while CP2077's 0x4B sets it),
//  with depth at the upscaler's render size. XeFG 1.3.1 rejects that pair
//  ("motion vector and depth resource resolutions must match") and its HIGH_RES_MV
//  init flag is deprecated, so one compute pass per frame converts the field into
//  exactly what a title that declares low-res MV would send: same size as depth,
//  vectors in render pixels.
//
//  The pass is `shaders/mv_resample.hlsl`, precompiled by fxc at build time and
//  embedded as bytes (no d3dcompiler at runtime). It runs on the TITLE's command list,
//  inside the upscaler's Evaluate, right before the inputs are tagged - the same place
//  OptiScaler's own shader passes run (Upscaler_Inputs_Dx12 / CameraMotion).
// ============================================================================
#include "fg_internal.h"

namespace fakerfg {
namespace {

#include "mv_resample_CSMain_cso.h"      // kMvResampleCSO (generated at build time)

ID3D12RootSignature*    g_mvr_root = nullptr;
ID3D12PipelineState*    g_mvr_pso = nullptr;
ID3D12DescriptorHeap*   g_mvr_heap = nullptr;
UINT                    g_mvr_desc_size = 0;
ID3D12Device*           g_mvr_device = nullptr;

struct MvrOut
{
    ID3D12Resource*       res = nullptr;
    UINT                  w = 0;
    UINT                  h = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};
MvrOut g_mvr_out[2];
int g_mvr_next = 0;

bool mvr_ensure_pipeline(ID3D12Device* device)
{
    if (g_mvr_pso != nullptr && g_mvr_device == device) { return true; }
    g_mvr_device = device;

    if (g_mvr_root == nullptr) {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType     = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;                  // t0
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType     = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;                  // u0
        ranges[1].OffsetInDescriptorsFromTableStart = 1;

        D3D12_ROOT_PARAMETER param{};
        param.ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 2;
        param.DescriptorTable.pDescriptorRanges   = ranges;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1;
        rsd.pParameters   = &param;
        rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ID3DBlob* sig = nullptr;
        ID3DBlob* err = nullptr;
        const HRESULT sr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                                       &sig, &err);
        if (FAILED(sr) || sig == nullptr) {
            logf("mvr: root signature failed 0x%08X\n", static_cast<unsigned>(sr));
            if (err != nullptr) { err->Release(); }
            return false;
        }
        const HRESULT cr = device->CreateRootSignature(0, sig->GetBufferPointer(),
                                                       sig->GetBufferSize(),
                                                       IID_PPV_ARGS(&g_mvr_root));
        sig->Release();
        if (err != nullptr) { err->Release(); }
        if (FAILED(cr)) {
            logf("mvr: CreateRootSignature failed 0x%08X\n", static_cast<unsigned>(cr));
            g_mvr_root = nullptr;
            return false;
        }
    }

    if (g_mvr_pso == nullptr) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature     = g_mvr_root;
        pd.CS.pShaderBytecode = kMvResampleCSO;
        pd.CS.BytecodeLength  = sizeof(kMvResampleCSO);
        const HRESULT hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_mvr_pso));
        if (FAILED(hr)) {
            logf("mvr: CreateComputePipelineState failed 0x%08X\n", static_cast<unsigned>(hr));
            g_mvr_pso = nullptr;
            return false;
        }
        logf("mvr: resample pipeline ready (%u bytes of CS)\n",
             static_cast<unsigned>(sizeof(kMvResampleCSO)));
    }

    if (g_mvr_heap == nullptr) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_mvr_heap)))) {
            g_mvr_heap = nullptr;
            return false;
        }
        g_mvr_desc_size = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    return g_mvr_pso != nullptr && g_mvr_heap != nullptr;
}

bool mvr_ensure_output(ID3D12Device* device, MvrOut& out, UINT w, UINT h)
{
    if (out.res != nullptr && out.w == w && out.h == h) { return true; }
    if (out.res != nullptr) { out.res->Release(); out.res = nullptr; }

    D3D12_RESOURCE_DESC d{};
    d.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width            = w;
    d.Height           = h;
    d.DepthOrArraySize = 1;
    d.MipLevels        = 1;
    d.Format           = DXGI_FORMAT_R16G16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type             = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask  = 1;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&out.res)))) {
        out.res = nullptr;
        return false;
    }
    out.w     = w;
    out.h     = h;
    out.state = D3D12_RESOURCE_STATE_COMMON;
    logf("mvr: output %ux%u R16G16_FLOAT created\n", w, h);
    return true;
}

} // namespace

void mv_resample_set_device(ID3D12Device* device) { g_mvr_device = device; }

bool mv_resample_frame(ID3D12CommandList* list, MgpuFgFrame* f)
{
    if (cfg().mv_resample == 0 || list == nullptr || f == nullptr || g_mvr_device == nullptr) {
        return false;
    }
    MgpuFgResource& mv = f->stream[MGPU_FG_STREAM_MV];
    MgpuFgResource& dp = f->stream[MGPU_FG_STREAM_DEPTH];
    if (mv.resource == nullptr || dp.width < 8 || dp.height < 8) { return false; }
    if (mv.width == dp.width && mv.height == dp.height) { return false; }   // already render-sized
    const UINT sw = mv.width, sh = mv.height;

    if (!mvr_ensure_pipeline(g_mvr_device)) { return false; }
    MvrOut& out = g_mvr_out[g_mvr_next];
    g_mvr_next = (g_mvr_next + 1) % 2;
    if (!mvr_ensure_output(g_mvr_device, out, dp.width, dp.height)) { return false; }

    ID3D12GraphicsCommandList* cl = static_cast<ID3D12GraphicsCommandList*>(list);

    D3D12_RESOURCE_BARRIER b{};
    if (out.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = out.res;
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = out.state;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cl->ResourceBarrier(1, &b);
        out.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_mvr_heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_mvr_heap->GetGPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format                  = DXGI_FORMAT_R16G16_FLOAT;
    sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels     = 1;
    g_mvr_device->CreateShaderResourceView(static_cast<ID3D12Resource*>(mv.resource), &sd, cpu);

    D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu = cpu;
    uav_cpu.ptr += g_mvr_desc_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format        = DXGI_FORMAT_R16G16_FLOAT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_mvr_device->CreateUnorderedAccessView(out.res, nullptr, &ud, uav_cpu);

    ID3D12DescriptorHeap* heaps[] = { g_mvr_heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_mvr_root);
    cl->SetPipelineState(g_mvr_pso);
    cl->SetComputeRootDescriptorTable(0, gpu);
    cl->Dispatch((dp.width + 7) / 8, (dp.height + 7) / 8, 1);

    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = out.res;
    b.Transition.Subresource = 0;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &b);
    out.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Hand XeFG the resampled texture: render-sized, vectors in render pixels, valid until
    // this slot is reused (two of them, so nothing still in flight is touched).
    mv.resource = out.res;
    mv.width    = dp.width;
    mv.height   = dp.height;
    mv.format   = static_cast<uint32_t>(DXGI_FORMAT_R16G16_FLOAT);
    mv.state    = static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    mv.validity = MGPU_FG_RV_UNTIL_PRESENT;

    static bool told = false;
    if (!told) {
        told = true;
        logf("mvr: motion vectors resampled %ux%u -> %ux%u (values x%.4f x%.4f)\n", sw, sh,
             dp.width, dp.height, static_cast<double>(dp.width) / static_cast<double>(sw),
             static_cast<double>(dp.height) / static_cast<double>(sh));
    }
    return true;
}

} // namespace fakerfg
