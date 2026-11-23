// ============================================================================
//  passes.cpp - OptiScaler's three present/evaluate shader passes, ported 1:1.
//
//    RenderUI        shaders/render_ui/RUI_Dx12.cpp      (FGDrawUIOverFG)
//    HudlessCompare  shaders/hudless_compare/HC_Dx12.cpp (fgHudlessCompare)
//    HudCopy         shaders/hud_copy/HudCopy_Dx12.cpp   (FGHudCutoff)
//
//  Same shaders (verbatim, see src/faker_fg/shaders), same resource flow, same
//  state transitions and the same config names/defaults as OptiScaler:
//
//    RenderUI:        copy chain[now] -> internal, draw a fullscreen triangle
//                     with t0 = UI, t1 = internal, RTV = chain[now].
//    HudlessCompare:  same shape, but the PS paints pink where hudless and the
//                     presented frame disagree (diagnostic).
//    HudCopy:         compute, hudless + (present - hudless) * uiMask -> present,
//                     dispatched on the TITLE's command list inside the DLSS-G
//                     Evaluate (Nvngx_FG.cpp:419-459).
//
//  Shaders are precompiled by fxc at build time and embedded as bytes.
// ============================================================================
#include "fg_internal.h"

#include <cstring>

namespace fakerfg {
namespace {

#include "render_ui_VSMain_cso.h"        // kRenderUiVSCSO
#include "render_ui_PSMain_cso.h"        // kRenderUiPSCSO
#include "render_ui_pm_PSMain_cso.h"     // kRenderUiPmPSCSO
#include "hudless_compare_PSMain_cso.h"  // kHudlessComparePSCSO
#include "hud_copy_CSMain_cso.h"         // kHudCopyCSO
#include "hudless_to_pq_CSMain_cso.h"    // kHudlessToPQCSO
#include "marker_PSMain_cso.h"           // kMarkerPSCSO

ID3D12RootSignature*        g_pass_gfx_root = nullptr;
ID3D12RootSignature*        g_pass_cs_root = nullptr;
ID3D12PipelineState*        g_pass_rui_pso[2]{};     // [0] premultiplied, [1] straight
ID3D12PipelineState*        g_pass_hc_pso = nullptr;
ID3D12PipelineState*        g_pass_hudcopy_pso = nullptr;
ID3D12PipelineState*        g_pass_hudpq_pso = nullptr;
ID3D12DescriptorHeap*       g_pass_heap = nullptr;   // SRV, SRV, CBV
ID3D12DescriptorHeap*       g_pass_sampler_heap = nullptr;
ID3D12DescriptorHeap*       g_pass_rtv_heap = nullptr;
UINT                        g_pass_desc_size = 0;
UINT                        g_pass_rtv_size = 0;
ID3D12Device*               g_pass_device = nullptr;

struct PassBuffer
{
    ID3D12Resource*       res = nullptr;
    UINT                  w = 0;
    UINT                  h = 0;
    DXGI_FORMAT           format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};
PassBuffer g_pass_copy[3];      // one per frame index, like HC_NUM_OF_HEAPS
int g_pass_copy_next = 0;

struct PassCb
{
    float diff_threshold = 0.03f;
    float pink_amount    = 1.0f;
    float inv_w          = 0.0f;
    float inv_h          = 0.0f;
};
ID3D12Resource* g_pass_cb = nullptr;
void*           g_pass_cb_mapped = nullptr;

bool pass_ensure(ID3D12Device* device)
{
    if (device == nullptr) { return false; }
    if (g_pass_device != device) {
        g_pass_device = device;
        g_pass_gfx_root = nullptr;
        g_pass_cs_root = nullptr;
        g_pass_rui_pso[0] = g_pass_rui_pso[1] = nullptr;
        g_pass_hc_pso = nullptr;
        g_pass_hudcopy_pso = nullptr;
        g_pass_hudpq_pso = nullptr;
    }
    if (g_pass_hudcopy_pso != nullptr && g_pass_hudpq_pso != nullptr &&
        g_pass_heap != nullptr) { return true; }

    auto make_root = [&](bool compute, ID3D12RootSignature** out) -> bool {
        D3D12_DESCRIPTOR_RANGE ranges[4]{};
        UINT count = 0;
        ranges[count].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[count].NumDescriptors = 1;
        ranges[count].BaseShaderRegister = 0;              // t0
        ranges[count].OffsetInDescriptorsFromTableStart = 0;
        ++count;
        ranges[count].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[count].NumDescriptors = 1;
        ranges[count].BaseShaderRegister = 1;              // t1
        ranges[count].OffsetInDescriptorsFromTableStart = 1;
        ++count;
        ranges[count].RangeType = compute ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                                          : D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        ranges[count].NumDescriptors = 1;
        ranges[count].BaseShaderRegister = 0;              // u0 / b0
        ranges[count].OffsetInDescriptorsFromTableStart = 2;
        ++count;
        if (compute) {
            ranges[count].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
            ranges[count].NumDescriptors = 1;
            ranges[count].BaseShaderRegister = 0;          // b0
            ranges[count].OffsetInDescriptorsFromTableStart = 3;
            ++count;
        }

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = count;
        params[0].DescriptorTable.pDescriptorRanges = ranges;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // b1: marker code / norm params as root constants (8 x 32 bit). The marker
        // pass uses the first four, the norm pass all eight. Root constants cannot
        // be clobbered by another pass rewriting the descriptor heap.
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 1;
        params[1].Constants.Num32BitValues = 8;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;                        // s0
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2;
        rsd.pParameters = params;
        rsd.NumStaticSamplers = compute ? 0 : 1;
        rsd.pStaticSamplers = compute ? nullptr : &sampler;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* sig = nullptr;
        ID3DBlob* err = nullptr;
        const HRESULT sr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                                       &sig, &err);
        if (FAILED(sr) || sig == nullptr) {
            logf("pass: root signature (%s) failed 0x%08X\n", compute ? "cs" : "gfx",
                 static_cast<unsigned>(sr));
            if (err != nullptr) { err->Release(); }
            return false;
        }
        const HRESULT cr = device->CreateRootSignature(0, sig->GetBufferPointer(),
                                                       sig->GetBufferSize(),
                                                       IID_PPV_ARGS(out));
        sig->Release();
        if (err != nullptr) { err->Release(); }
        return SUCCEEDED(cr);
    };

    if (g_pass_gfx_root == nullptr && !make_root(false, &g_pass_gfx_root)) { return false; }
    if (g_pass_cs_root == nullptr && !make_root(true, &g_pass_cs_root)) { return false; }

    if (g_pass_heap == nullptr) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 4;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_pass_heap)))) {
            g_pass_heap = nullptr;
            return false;
        }
        g_pass_desc_size = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (g_pass_rtv_heap == nullptr) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_pass_rtv_heap)))) {
            g_pass_rtv_heap = nullptr;
            return false;
        }
        g_pass_rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    return true;
}

struct PassPso
{
    DXGI_FORMAT          format = DXGI_FORMAT_UNKNOWN;
    int                  kind = -1;
    ID3D12PipelineState* pso = nullptr;
};
PassPso g_pass_pso_cache[8];
int     g_pass_pso_count = 0;

// The RTV format is the chain's, which is only known at run time, so the graphics PSOs are
// cached per (format, variant) - the same reason OptiScaler builds them in the constructor of
// RUI_Dx12 once the swap chain exists.
ID3D12PipelineState* pass_gfx_pso(DXGI_FORMAT fmt, int kind, const unsigned char* ps,
                                  size_t ps_size)
{
    for (int i = 0; i < g_pass_pso_count; ++i) {
        if (g_pass_pso_cache[i].format == fmt && g_pass_pso_cache[i].kind == kind) {
            return g_pass_pso_cache[i].pso;
        }
    }
    if (g_pass_pso_count >= static_cast<int>(_countof(g_pass_pso_cache))) { return nullptr; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_pass_gfx_root;
    pd.VS.pShaderBytecode = kRenderUiVSCSO;
    pd.VS.BytecodeLength = sizeof(kRenderUiVSCSO);
    pd.PS.pShaderBytecode = ps;
    pd.PS.BytecodeLength = ps_size;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
    pd.SampleDesc.Count = 1;

    ID3D12PipelineState* pso = nullptr;
    if (FAILED(g_pass_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
        logf("pass: graphics PSO (fmt=%u kind=%d) failed\n", static_cast<unsigned>(fmt), kind);
        return nullptr;
    }
    g_pass_pso_cache[g_pass_pso_count].format = fmt;
    g_pass_pso_cache[g_pass_pso_count].kind = kind;
    g_pass_pso_cache[g_pass_pso_count].pso = pso;
    ++g_pass_pso_count;
    return pso;
}

// Invisible marker: alpha-blended fullscreen triangle, scissored to a small block.
// The PS outputs black with alpha=1/255 on "1" checker pixels, which multiplies the
// destination by ~0.996 (1 LSB darkening); no colour block is written.
ID3D12PipelineState* pass_marker_pso(DXGI_FORMAT fmt)
{
    static DXGI_FORMAT cached_fmt = DXGI_FORMAT_UNKNOWN;
    static ID3D12PipelineState* cached = nullptr;
    if (cached != nullptr && cached_fmt == fmt) { return cached; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_pass_gfx_root;
    pd.VS.pShaderBytecode = kRenderUiVSCSO;
    pd.VS.BytecodeLength = sizeof(kRenderUiVSCSO);
    pd.PS.pShaderBytecode = kMarkerPSCSO;
    pd.PS.BytecodeLength = sizeof(kMarkerPSCSO);
    auto& bl = pd.BlendState.RenderTarget[0];
    // Full-contrast code; original tile is restored after the consumer's HDR
    // conversion. Preserve alpha (DWM/UI), write RGB only.
    bl.BlendEnable = FALSE;
    bl.SrcBlend = D3D12_BLEND_ONE;
    bl.DestBlend = D3D12_BLEND_ZERO;
    bl.BlendOp = D3D12_BLEND_OP_ADD;
    bl.SrcBlendAlpha = D3D12_BLEND_ZERO;
    bl.DestBlendAlpha = D3D12_BLEND_ONE;
    bl.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    bl.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED |
                               D3D12_COLOR_WRITE_ENABLE_GREEN |
                               D3D12_COLOR_WRITE_ENABLE_BLUE;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
    pd.SampleDesc.Count = 1;

    ID3D12PipelineState* pso = nullptr;
    if (FAILED(g_pass_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
        logf("pass: marker PSO (fmt=%u) failed\n", static_cast<unsigned>(fmt));
        return nullptr;
    }
    cached_fmt = fmt;
    cached = pso;
    return pso;
}

static bool pass_marker_impl(ID3D12CommandList* list, ID3D12Resource* sc_buffer,
                        UINT w, UINT h, uint32_t code)
{
    if (list == nullptr || sc_buffer == nullptr) { return false; }
    if (!pass_ensure(g_pass_device)) { return false; }
    ID3D12GraphicsCommandList* cl = static_cast<ID3D12GraphicsCommandList*>(list);
    const D3D12_RESOURCE_DESC sd = sc_buffer->GetDesc();
    ID3D12PipelineState* pso = pass_marker_pso(sd.Format);
    if (pso == nullptr) { return false; }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_pass_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g_pass_device->CreateRenderTargetView(sc_buffer, nullptr, rtv);
    cl->SetGraphicsRootSignature(g_pass_gfx_root);
    // code (32-bit short snowflake), width, height, side, cells  (the PS places the
    // four blocks itself and paints the always-white cell).
    const uint32_t mc[8] = { code, w, h, mgpu::kMarkerSide, mgpu::kMarkerCells,
                             0u, 0u, 0u };
    cl->SetGraphicsRoot32BitConstants(1, 8, mc, 0);
    cl->SetPipelineState(pso);
    cl->OMSetRenderTargets(1, &rtv, TRUE, nullptr);

    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<FLOAT>(w);
    vp.Height = static_cast<FLOAT>(h);
    vp.MaxDepth = 1.0f;
    cl->RSSetViewports(1, &vp);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // v32 marker cost fix: the full-screen triangle only needs to rasterize 4 x 6x6
    // blocks. A full-screen scissor still runs the PS over the whole 4K frame; four tiny
    // viewport+scissor draws reduce pixel coverage from 8.3M to 144. The marker PS uses
    // absolute SV_Position, so the viewport origin is transparent to it.
    const uint32_t side = mgpu::kMarkerSide;
    for (uint32_t i = 0; i < mgpu::kMarkerCorners; ++i) {
        uint32_t x = 0, y = 0;
        mgpu::marker_corner(i, w, h, &x, &y);
        D3D12_VIEWPORT bvp{};
        bvp.TopLeftX = static_cast<FLOAT>(x);
        bvp.TopLeftY = static_cast<FLOAT>(y);
        bvp.Width = static_cast<FLOAT>(side);
        bvp.Height = static_cast<FLOAT>(side);
        bvp.MaxDepth = 1.0f;
        cl->RSSetViewports(1, &bvp);
        D3D12_RECT rc{ static_cast<LONG>(x), static_cast<LONG>(y),
                       static_cast<LONG>(x + side), static_cast<LONG>(y + side) };
        cl->RSSetScissorRects(1, &rc);
        cl->DrawInstanced(3, 1, 0, 0);
    }
    return true;
}

bool pass_ensure_cs()
{
    if (g_pass_hudcopy_pso != nullptr && g_pass_hudpq_pso != nullptr) { return true; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_pass_cs_root;
    pd.CS.pShaderBytecode = kHudCopyCSO;
    pd.CS.BytecodeLength = sizeof(kHudCopyCSO);
    if (FAILED(g_pass_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pass_hudcopy_pso)))) {
        g_pass_hudcopy_pso = nullptr;
        return false;
    }
    pd.CS.pShaderBytecode = kHudlessToPQCSO;
    pd.CS.BytecodeLength = sizeof(kHudlessToPQCSO);
    if (FAILED(g_pass_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pass_hudpq_pso)))) {
        g_pass_hudpq_pso = nullptr;
        return false;
    }
    return true;
}

bool pass_ensure_cb()
{
    if (g_pass_cb != nullptr) { return true; }
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = 256;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    if (FAILED(g_pass_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      IID_PPV_ARGS(&g_pass_cb)))) {
        g_pass_cb = nullptr;
        return false;
    }
    if (FAILED(g_pass_cb->Map(0, nullptr, &g_pass_cb_mapped))) {
        g_pass_cb_mapped = nullptr;
        return false;
    }
    return true;
}

} // namespace

bool pass_marker(ID3D12CommandList* list, ID3D12Resource* sc_buffer,
                 UINT w, UINT h, uint32_t code)
{
    return pass_marker_impl(list, sc_buffer, w, h, code);
}

// pass_norm_tile() removed with the 16x16 scar tile transport.


// ---------------------------------------------------------------- RenderUI (RUI_Dx12)
// RUI_Dx12::Dispatch(), step for step: copy the chain buffer, draw UI over it into the chain
// buffer, restore both states. The UI alpha convention follows FGUIPremultipliedAlpha.
bool pass_render_ui(ID3D12CommandList* list, ID3D12Resource* sc_buffer, UINT w, UINT h,
                    ID3D12Resource* ui, uint32_t ui_state)
{
    if (list == nullptr || sc_buffer == nullptr || ui == nullptr) { return false; }
    if (!pass_ensure(g_pass_device)) { return false; }
    ID3D12GraphicsCommandList* cl = static_cast<ID3D12GraphicsCommandList*>(list);

    const int slot = g_pass_copy_next;
    g_pass_copy_next = (g_pass_copy_next + 1) % 3;
    PassBuffer& copy = g_pass_copy[slot];
    const D3D12_RESOURCE_DESC sd = sc_buffer->GetDesc();
    if (copy.res == nullptr || copy.w != w || copy.h != h || copy.format != sd.Format) {
        if (copy.res != nullptr) { copy.res->Release(); copy.res = nullptr; }
        D3D12_RESOURCE_DESC d = sd;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        if (FAILED(g_pass_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                          D3D12_RESOURCE_STATE_COPY_DEST,
                                                          nullptr, IID_PPV_ARGS(&copy.res)))) {
            copy.res = nullptr;
            return false;
        }
        copy.w = w; copy.h = h; copy.format = sd.Format;
        copy.state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if (!pass_ensure_cb()) { return false; }
    const int kind = cfg().fg_ui_premultiplied != 0 ? 0 : 1;
    ID3D12PipelineState* pso = pass_gfx_pso(sd.Format, kind,
                                            kind == 0 ? kRenderUiPSCSO : kRenderUiPmPSCSO,
                                            kind == 0 ? sizeof(kRenderUiPSCSO)
                                                      : sizeof(kRenderUiPmPSCSO));
    if (pso == nullptr) { return false; }

    D3D12_RESOURCE_BARRIER b{};
    // chain: PRESENT -> COPY_SOURCE, then -> RENDER_TARGET (RUI_Dx12.cpp:207-214)
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = sc_buffer;
    b.Transition.Subresource = 0;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    if (copy.state != D3D12_RESOURCE_STATE_COPY_DEST) {
        b.Transition.pResource = copy.res;
        b.Transition.StateBefore = copy.state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cl->ResourceBarrier(1, &b);
        copy.state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    cl->CopyResource(copy.res, sc_buffer);
    b.Transition.pResource = sc_buffer;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cl->ResourceBarrier(1, &b);
    b.Transition.pResource = copy.res;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &b);
    copy.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    if (ui_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        b.Transition.pResource = ui;
        b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(ui_state);
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cl->ResourceBarrier(1, &b);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_pass_heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_pass_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels = 1;
    srvd.Format = sd.Format;
    g_pass_device->CreateShaderResourceView(ui, &srvd, cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu;
    cpu1.ptr += g_pass_desc_size;
    g_pass_device->CreateShaderResourceView(copy.res, &srvd, cpu1);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_pass_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g_pass_device->CreateRenderTargetView(sc_buffer, nullptr, rtv);

    ID3D12DescriptorHeap* heaps[] = { g_pass_heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootSignature(g_pass_gfx_root);
    cl->SetPipelineState(pso);
    cl->SetGraphicsRootDescriptorTable(0, gpu);
    cl->OMSetRenderTargets(1, &rtv, TRUE, nullptr);

    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<FLOAT>(w);
    vp.Height = static_cast<FLOAT>(h);
    vp.MaxDepth = 1.0f;
    cl->RSSetViewports(1, &vp);
    D3D12_RECT rc{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
    cl->RSSetScissorRects(1, &rc);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);

    b.Transition.pResource = sc_buffer;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cl->ResourceBarrier(1, &b);
    if (ui_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        b.Transition.pResource = ui;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(ui_state);
        cl->ResourceBarrier(1, &b);
    }
    return true;
}

// ---------------------------------------------------------------- HudCopy (HudCopy_Dx12)
// HudCopy.hlsl: hudless + (present - hudless) * uiMask -> present. Runs on the title's own
// command list inside the DLSS-G Evaluate (Nvngx_FG.cpp:419-459), like the original.
bool pass_hud_copy(ID3D12CommandList* list, ID3D12Resource* hudless, ID3D12Resource* present,
                   uint32_t hudless_state, uint32_t present_state, float threshold)
{
    if (list == nullptr || hudless == nullptr || present == nullptr) { return false; }
    if (!pass_ensure(g_pass_device) || !pass_ensure_cs() || !pass_ensure_cb()) { return false; }
    ID3D12GraphicsCommandList* cl = static_cast<ID3D12GraphicsCommandList*>(list);

    const D3D12_RESOURCE_DESC pd = present->GetDesc();
    const UINT w = static_cast<UINT>(pd.Width);
    const UINT h = pd.Height;

    // PresentCopy: the shader reads the frame it writes, so one internal copy (same as the
    // `_buffer` HudCopy_Dx12 keeps).
    const int slot = g_pass_copy_next;
    g_pass_copy_next = (g_pass_copy_next + 1) % 3;
    PassBuffer& copy = g_pass_copy[slot];
    if (copy.res == nullptr || copy.w != w || copy.h != h || copy.format != pd.Format) {
        if (copy.res != nullptr) { copy.res->Release(); copy.res = nullptr; }
        D3D12_RESOURCE_DESC d = pd;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        if (FAILED(g_pass_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                          D3D12_RESOURCE_STATE_COPY_DEST,
                                                          nullptr, IID_PPV_ARGS(&copy.res)))) {
            copy.res = nullptr;
            return false;
        }
        copy.w = w; copy.h = h; copy.format = pd.Format;
        copy.state = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = 0;
    b.Transition.pResource = present;
    b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(present_state);
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    if (copy.state != D3D12_RESOURCE_STATE_COPY_DEST) {
        b.Transition.pResource = copy.res;
        b.Transition.StateBefore = copy.state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cl->ResourceBarrier(1, &b);
        copy.state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    cl->CopyResource(copy.res, present);
    b.Transition.pResource = copy.res;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &b);
    copy.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.pResource = present;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->ResourceBarrier(1, &b);

    PassCb cb{};
    cb.diff_threshold = threshold;
    cb.pink_amount = 0.0f;
    cb.inv_w = 1.0f / static_cast<float>(w);
    cb.inv_h = 1.0f / static_cast<float>(h);
    if (g_pass_cb_mapped != nullptr) { std::memcpy(g_pass_cb_mapped, &cb, sizeof(cb)); }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_pass_heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_pass_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels = 1;
    srvd.Format = DXGI_FORMAT_UNKNOWN;                 // keep the resource's own format
    g_pass_device->CreateShaderResourceView(hudless, &srvd, cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu;
    cpu1.ptr += g_pass_desc_size;
    g_pass_device->CreateShaderResourceView(copy.res, &srvd, cpu1);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu2 = cpu1;
    cpu2.ptr += g_pass_desc_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavd.Format = DXGI_FORMAT_UNKNOWN;
    g_pass_device->CreateUnorderedAccessView(present, nullptr, &uavd, cpu2);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu3 = cpu2;
    cpu3.ptr += g_pass_desc_size;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
    cbv.BufferLocation = g_pass_cb->GetGPUVirtualAddress();
    cbv.SizeInBytes = 256;
    g_pass_device->CreateConstantBufferView(&cbv, cpu3);

    ID3D12DescriptorHeap* heaps[] = { g_pass_heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_pass_cs_root);
    cl->SetPipelineState(g_pass_hudcopy_pso);
    cl->SetComputeRootDescriptorTable(0, gpu);
    cl->Dispatch((w + 15) / 16, (h + 15) / 16, 1);

    b.Transition.pResource = present;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(present_state);
    cl->ResourceBarrier(1, &b);
    static bool told = false;
    if (!told) {
        told = true;
        logf("pass: HudCopy %ux%u threshold=%.3f\n", w, h, static_cast<double>(threshold));
    }
    return true;
}

// ---------------------------------------------------------------- HUDless -> PQ
// The snapshot is recorded on the tag's own list, so the HUDless resource is still valid.
// One compute pass replaces the full-resolution CopyResource with a smaller R10 write.
bool pass_hudless_to_pq(ID3D12CommandList* list, ID3D12Resource* hudless,
                        uint32_t hudless_state, ID3D12Resource* out_r10,
                        UINT w, UINT h)
{
    if (list == nullptr || hudless == nullptr || out_r10 == nullptr) { return false; }
    if (!pass_ensure(g_pass_device) || !pass_ensure_cs() || !pass_ensure_cb()) { return false; }
    ID3D12GraphicsCommandList* cl = static_cast<ID3D12GraphicsCommandList*>(list);

    D3D12_CPU_DESCRIPTOR_HANDLE cpu =
        g_pass_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu =
        g_pass_heap->GetGPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_pass_device->CreateShaderResourceView(hudless, &srv, cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu;
    cpu1.ptr += g_pass_desc_size;
    g_pass_device->CreateShaderResourceView(hudless, &srv, cpu1);  // t1 is unused

    D3D12_CPU_DESCRIPTOR_HANDLE cpu2 = cpu1;
    cpu2.ptr += g_pass_desc_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    g_pass_device->CreateUnorderedAccessView(out_r10, nullptr, &uavd, cpu2);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu3 = cpu2;
    cpu3.ptr += g_pass_desc_size;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
    cbv.BufferLocation = g_pass_cb->GetGPUVirtualAddress();
    cbv.SizeInBytes = 256;
    g_pass_device->CreateConstantBufferView(&cbv, cpu3);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = hudless;
    b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(hudless_state);
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &b);

    b.Transition.pResource = out_r10;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->ResourceBarrier(1, &b);

    ID3D12DescriptorHeap* heaps[] = { g_pass_heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_pass_cs_root);
    cl->SetPipelineState(g_pass_hudpq_pso);
    cl->SetComputeRootDescriptorTable(0, gpu);
    cl->Dispatch((w + 15) / 16, (h + 15) / 16, 1);

    b.Transition.pResource = out_r10;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);

    b.Transition.pResource = hudless;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(hudless_state);
    cl->ResourceBarrier(1, &b);
    return true;
}

void pass_set_device(ID3D12Device* device)
{
    g_pass_device = device;
    pass_ensure(device);
}

} // namespace fakerfg
