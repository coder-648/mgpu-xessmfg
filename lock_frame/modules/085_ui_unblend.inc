// ============================================================================
//  lock_frame module: 085_ui_unblend.inc
//
//  B mode 4: consumer-side UI unblend.
//
//  The colour that arrives through WGC is the presented backbuffer (UI included). The
//  producer transports one downsampled UI stream. This pass rebuilds a HUDless frame:
//
//      premultiplied: H = (P - UI.rgb) / max(1 - UI.a, eps)
//      straight:      H = (P - UI.rgb * UI.a) / max(1 - UI.a, eps)
//
//  The result is tagged as XEFG_SWAPCHAIN_RES_HUDLESS_COLOR while the chain buffer keeps
//  the UI-bearing image. XeFG's BACKBUFFER_HUDLESS mode then interpolates H and extracts
//  the UI from the backbuffer. If this pass cannot run, the caller tags UI instead and
//  falls back to BACKBUFFER_UITEXTURE.
//
//  Included by lock_frame/lock_frame.cpp after 080 (make_srv/make_uav/up_transition).
// ============================================================================

static ComPtr<ID3D12RootSignature> g_unblend_rs;
static ComPtr<ID3D12PipelineState> g_unblend_pso;
static ComPtr<ID3D12DescriptorHeap> g_unblend_heap;

// A small ring, not one texture per publication slot: 32 x 4K R10 would be ~1 GB. Four
// entries cover the swap-chain depth while the tagged HUDless is still in flight.
static const UINT kUnblendRing = 4;
static ComPtr<ID3D12Resource> g_hudless_ring[kUnblendRing];
static D3D12_RESOURCE_STATES  g_hudless_state[kUnblendRing] = {
    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
static UINT g_hudless_w = 0, g_hudless_h = 0;
static UINT g_hudless_ring_i = 0;
static ID3D12Resource* g_hudless_current = nullptr;   // valid for this frame only

static bool ensure_unblend_objects()
{
    if (g_unblend_rs && g_unblend_pso && g_unblend_heap) { return true; }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;                 // present, UI
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;                 // HUDless
    ranges[1].BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.ShaderRegister = 0;
    p[0].Constants.Num32BitValues = 8;            // DstW/DstH/UiW/UiH + 4 floats
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable.NumDescriptorRanges = 1;
    p[2].DescriptorTable.pDescriptorRanges = &ranges[1];
    p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC smp{};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    smp.ShaderRegister = 0;
    smp.RegisterSpace = 0;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3;
    rs.pParameters = p;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &smp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sig, err;
    const HRESULT shr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                                    &sig, &err);
    if (FAILED(shr) || !sig) {
        logf_("[lock] ui-unblend: root signature FAILED hr=0x%08lX\n",
              static_cast<unsigned long>(shr));
        return false;
    }
    if (FAILED(g_device->CreateRootSignature(0, sig->GetBufferPointer(),
                                             sig->GetBufferSize(),
                                             IID_PPV_ARGS(&g_unblend_rs)))) {
        logf_("[lock] ui-unblend: CreateRootSignature FAILED\n");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_unblend_rs.Get();
    pd.CS.pShaderBytecode = kUnblendUICSO;
    pd.CS.BytecodeLength = sizeof(kUnblendUICSO);
    if (FAILED(g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_unblend_pso)))) {
        logf_("[lock] ui-unblend: CreateComputePipelineState FAILED\n");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 3;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_unblend_heap)))) {
        logf_("[lock] ui-unblend: descriptor heap FAILED\n");
        return false;
    }
    logf_("[lock] ui-unblend: UI -> HUDless pass ready (B mode 4)\n");
    return true;
}

static bool ensure_hudless_ring(UINT w, UINT h)
{
    if (w < 64 || h < 64) { return false; }
    if (g_hudless_ring[0] && g_hudless_w == w && g_hudless_h == h) { return true; }

    for (UINT i = 0; i < kUnblendRing; ++i) {
        g_hudless_ring[i].Reset();
        g_hudless_state[i] = D3D12_RESOURCE_STATE_COMMON;
    }
    g_hudless_current = nullptr;
    g_hudless_w = g_hudless_h = 0;

    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    for (UINT i = 0; i < kUnblendRing; ++i) {
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&g_hudless_ring[i])))) {
            logf_("[lock] ui-unblend: HUDless ring allocation FAILED (%ux%u)\n", w, h);
            for (UINT k = 0; k < kUnblendRing; ++k) { g_hudless_ring[k].Reset(); }
            return false;
        }
    }
    g_hudless_w = w;
    g_hudless_h = h;
    g_hudless_ring_i = 0;
    logf_("[lock] ui-unblend: HUDless ring %ux%u x%u (R10 PQ)\n", w, h, kUnblendRing);
    return true;
}

// Records the unblend pass and returns the HUDless texture to tag for this frame, or
// nullptr when B cannot run (no UI stream, no local UI, unsupported state). On success
// `present` is left in RENDER_TARGET and the returned texture is in NON_PIXEL_SRV.
static ID3D12Resource* record_ui_unblend(UINT slot, ID3D12Resource* present, UINT w, UINT h)
{
    g_hudless_current = nullptr;
    if (!g_ui_unblend || g_ctl == nullptr || present == nullptr || slot >= mgpu::kMaxSlots) {
        return nullptr;
    }
    if (g_ui_local[slot] == nullptr) { return nullptr; }
    if (!g_ui_fresh[slot]) { return nullptr; }
    if ((g_ctl->active_mask & mgpu::kRequestUi) == 0) { return nullptr; }

    const mgpu::StreamShared& su = g_ctl->streams[3];
    if (su.width < 64 || su.height < 64 || su.format == 0) { return nullptr; }
    if (!ensure_unblend_objects() || !ensure_hudless_ring(w, h)) { return nullptr; }

    const UINT ri = g_hudless_ring_i;
    g_hudless_ring_i = (g_hudless_ring_i + 1u) % kUnblendRing;
    ID3D12Resource* hudless = g_hudless_ring[ri].Get();

    const UINT inc = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_unblend_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_unblend_heap->GetGPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = gpu;
    make_srv(present, cpu, DXGI_FORMAT_R10G10B10A2_UNORM);
    cpu.ptr += inc; gpu.ptr += inc;
    make_srv(g_ui_local[slot].Get(), cpu, static_cast<DXGI_FORMAT>(su.format));
    cpu.ptr += inc; gpu.ptr += inc;
    const D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu = gpu;
    make_uav(hudless, cpu, DXGI_FORMAT_R10G10B10A2_UNORM);

    // Present came in as RENDER_TARGET from the convert draw. UI and HUDless are COMMON.
    up_transition(present, D3D12_RESOURCE_STATE_RENDER_TARGET,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    up_transition(g_ui_local[slot].Get(), D3D12_RESOURCE_STATE_COMMON,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    up_transition(hudless, g_hudless_state[ri], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    struct UnblendConsts { UINT dst_w, dst_h, ui_w, ui_h; float premul, opaque, pad0, pad1; }
        c{ w, h, su.width, su.height, 1.0f, 0.99f, 0.0f, 0.0f };

    g_list->SetComputeRootSignature(g_unblend_rs.Get());
    g_list->SetPipelineState(g_unblend_pso.Get());
    ID3D12DescriptorHeap* heaps[] = { g_unblend_heap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetComputeRoot32BitConstants(0, 8, &c, 0);
    g_list->SetComputeRootDescriptorTable(1, srv_gpu);
    g_list->SetComputeRootDescriptorTable(2, uav_gpu);
    g_list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = hudless;
    g_list->ResourceBarrier(1, &ub);

    up_transition(present, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_RENDER_TARGET);
    up_transition(g_ui_local[slot].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_COMMON);
    up_transition(hudless, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_hudless_state[ri] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    g_hudless_current = hudless;
    if (!g_ui_unblend_ready_logged) {
        g_ui_unblend_ready_logged = true;
        logf_("[lock] ui-unblend: HUDless rebuilt from %ux%u present + %ux%u UI\n",
              w, h, su.width, su.height);
    }
    return hudless;
}

