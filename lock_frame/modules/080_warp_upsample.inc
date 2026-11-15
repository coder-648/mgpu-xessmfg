// ============================================================================
//  lock_frame module: 080_warp_upsample.inc
//
//  Experimental --fg warp interpolator and --upsample joint MV/depth
//  reconstruction.
//
//  Included by lock_frame/lock_frame.cpp as part of the single-translation-unit build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

// ------------------------- our own interpolator (--fg warp) -------------------
//
// XeSS-FG interpolates at the swap chain size, so presenting native 5K cost 12-13 ms
// on the 6750 GRE and dragged the input rate down to ~27 fps. Lossless Scaling avoids
// that by analysing motion at a low resolution and warping at native resolution. We
// can skip the analysis completely: the game already hands us per-pixel motion
// vectors, so a single warp of two native 5K frames is all that is needed. Nothing in
// this path is ever scaled, so the output keeps the render card's 5K detail.
//
// Per input frame: 5K copy + 5K warp + two 5K colour conversions, two presents.
// -----------------------------------------------------------------------------

static bool  g_fg_warp     = false;
static bool  g_warp_mv_logged = false;
static float g_mv_mul      = 0.0f;   // --mv-mul <f>: force the MV scale (UV units)
static float g_mv_sign     = 1.0f;   // --mv-flip: use -MV
static float g_warp_clamp  = 1.0f;   // --warp-clamp <f>: 0 disables the neighbour clamp
static float g_warp_conf   = 90.0f;  // --warp-conf <f>: MV-consistency sensitivity
static float g_warp_conf_on = 0.98f; // run the pixel search below this confidence
static float g_warp_search  = 2.0f;  // --warp-search <f>: search radius in MV texels

static ComPtr<ID3D12Resource> g_hist[2];
static D3D12_RESOURCE_STATES  g_hist_state[2] = { D3D12_RESOURCE_STATE_COMMON,
                                                  D3D12_RESOURCE_STATE_COMMON };
static ComPtr<ID3D12Resource> g_interp;
static D3D12_RESOURCE_STATES  g_interp_state = D3D12_RESOURCE_STATE_COMMON;
static ComPtr<ID3D12DescriptorHeap> g_interp_rtv;
static ComPtr<ID3D12DescriptorHeap> g_srv_interp;
static ComPtr<ID3D12DescriptorHeap> g_warp_srv;
static ComPtr<ID3D12RootSignature>  g_warp_root;
static ComPtr<ID3D12PipelineState>  g_warp_pso;
static UINT  g_hist_write = 0;
static bool  g_hist_valid = false;

// The warp path submits *two* lists per input frame (the interpolated midpoint, then the
// real frame), and the midpoint's Present() only queues it. Resetting one command
// allocator for the second list while the first is still executing is undefined
// behaviour in D3D12 - it was a latent hazard (it only ever showed up as a driver
// hiccup). Two (allocator, list) pairs are kept and the shared helpers flip between them
// once per frame; gpu_throttle() at the top of the next call waits for the fence both
// lists signalled, so a pair is always retired before it is reset again.
static ComPtr<ID3D12CommandAllocator>     g_alloc_alt;
static ComPtr<ID3D12GraphicsCommandList>  g_list_alt;

static void warp_ensure_alt_pair()
{
    if (g_list_alt) { return; }
    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&g_alloc_alt))) ||
        FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           g_alloc_alt.Get(), nullptr,
                                           IID_PPV_ARGS(&g_list_alt)))) {
        logf_("[lock] warp: second command list FAILED - the two lists keep sharing one"
              " allocator, which is not legal while the first one is in flight\n");
        g_alloc_alt.Reset();
        g_list_alt.Reset();
        return;
    }
    g_list_alt->Close();
}

static void warp_use_alt_pair()
{
    warp_ensure_alt_pair();
    if (!g_list_alt) { return; }      // creation failed: keep the old (unclean) behaviour
    // ComPtr::Swap only exchanges the pointers: the pair that was just submitted simply
    // becomes "the other pair" and is reset after the next gpu_throttle().
    g_alloc.Swap(g_alloc_alt);
    g_list.Swap(g_list_alt);
}

static void barrier_to(ID3D12Resource* res, D3D12_RESOURCE_STATES& state,
                       D3D12_RESOURCE_STATES next)
{
    if (!res || state == next) { return; }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = next;
    g_list->ResourceBarrier(1, &b);
    state = next;
}

static const char* k_warp_hlsl = R"HLSL(
Texture2D<float4> tA   : register(t0);      // one history frame
Texture2D<float4> tB   : register(t1);      // the other history frame
Texture2D<float2> tMv  : register(t2);
SamplerState      smp  : register(s0);
// aIsCurrent: 1 when tA holds the newer frame. Both frames stay at fixed descriptors
// so nothing has to be rewritten while a previous list may still be running.
cbuffer P : register(b0) {
    float2 mvScale;     // MV units -> UV delta
    float  aIsCurrent;  // 1 when tA holds the newer frame
    float  clampK;      // neighbourhood clamp strength
    float  confGain;    // per-UV forward/backward error that zeroes the history
    float  confOn;      // below this confidence the local pixel search runs
    float  searchR;     // search radius in motion-vector texels
    float  pad0;
};

static const float2 kOffsets[8] = {
    float2( 1.0,  0.0), float2(-1.0,  0.0), float2( 0.0,  1.0), float2( 0.0, -1.0),
    float2( 1.0,  1.0), float2(-1.0,  1.0), float2( 1.0, -1.0), float2(-1.0, -1.0),
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut vs_main(uint id : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((id == 2) ? 2.0 : 0.0, (id == 1) ? 2.0 : 0.0);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// The conditional operator cannot select between texture objects, so the branch
// has to happen on the sampled value instead.
float3 cur_at(float2 uv)
{
    return (aIsCurrent > 0.5) ? tA.SampleLevel(smp, uv, 0).rgb
                              : tB.SampleLevel(smp, uv, 0).rgb;
}
float3 prev_at(float2 uv)
{
    return (aIsCurrent > 0.5) ? tB.SampleLevel(smp, uv, 0).rgb
                              : tA.SampleLevel(smp, uv, 0).rgb;
}

float4 ps_main(VSOut i) : SV_Target
{
    // The game's motion vectors become a UV delta through the scale it hands NGX.
    float2 mv = tMv.SampleLevel(smp, i.uv, 0) * mvScale;
    // A wrong scale or a hard camera cut would smear the whole screen; a quarter of
    // the screen per frame is already far beyond anything real.
    mv = clamp(mv, -0.25, 0.25);

    float2 pPrev = i.uv - mv * 0.5;     // where this pixel was half a frame ago
    float2 pCur  = i.uv + mv * 0.5;

    float3 a = prev_at(pPrev);
    float3 b = cur_at(pCur);

    // Forward/backward consistency. The MV sampled where the history landed should
    // point back at this pixel; when it does not, the history is untrustworthy - the
    // surface was just revealed, or the content has no motion vector at all (particles,
    // reflections, screen-space effects).
    float2 mvBack = tMv.SampleLevel(smp, pPrev, 0) * mvScale;
    float  fbErr  = length(mvBack + mv);
    float  conf   = saturate(1.0 - fbErr * confGain);

    uint tw = 1u, th = 1u;
    tA.GetDimensions(tw, th);
    float2 tx = float2(1.0, 1.0) / float2(tw, th);

    // Pixel-domain fallback: where the engine's motion vector is not trustworthy the
    // content has no motion vector at all, so search the previous frame locally for the
    // patch that actually matches the current one. This is the cheap version of what a
    // learned optical-flow net does, and it is applied per pixel only when MV fails, so
    // geometry keeps the exact engine motion. Without it, rain, smoke, water, reflections
    // and screen-space effects smear.
    if (conf < confOn) {
        float3 best    = a;
        float  bestErr = length(a - b);
        [unroll]
        for (int k = 0; k < 8; ++k) {
            float2 o = kOffsets[k] * tx * searchR;
            float3 cand = prev_at(pPrev + o);
            float  e    = length(cand - b);
            if (e < bestErr) { bestErr = e; best = cand; }
        }
        a    = best;
        // a good match restores confidence in the history
        conf = saturate(1.0 - bestErr * confGain * 0.5);
    }

    // Clamp the reprojected history to the current neighbourhood. Without this a
    // background pixel revealed by a moving object smears its old colour forward.
    float3 n0 = cur_at(pCur + float2( tx.x, 0.0));
    float3 n1 = cur_at(pCur + float2(-tx.x, 0.0));
    float3 n2 = cur_at(pCur + float2(0.0,  tx.y));
    float3 n3 = cur_at(pCur + float2(0.0, -tx.y));
    float3 lo = min(min(n0, n1), min(n2, n3));
    float3 hi = max(max(n0, n1), max(n2, n3));
    a = lerp(a, clamp(a, min(lo, b), max(hi, b)), clampK);

    // Photometric disagreement the clamp could not explain: the history is simply
    // wrong here, so take it out of the blend.
    float3 rel   = abs(a - b) / (0.05 + abs(a) + abs(b));
    float  photo = saturate(max(rel.r, max(rel.g, rel.b)));
    conf *= 1.0 - photo;

    return float4(lerp(b, a, 0.5 * conf), 1.0);   // midpoint of the two real frames
}
)HLSL";

static void make_srv(ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE where, DXGI_FORMAT fmt)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = fmt;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(res, &srv, where);
}

// ------------------------- --upsample: joint MV/depth reconstruction -----------
// One pass reads the low-res MV+depth copies and writes full-res MV+depth. The inputs are
// both local (the cross-card copy already happened), so this never waits for WGC colour.
// It is recorded on the frame's own DIRECT list, after the copies and before the tags,
// which is why --upsample disables --input-copy-queue / --prefetch-inputs at startup.
static void make_uav(ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE where, DXGI_FORMAT fmt)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = fmt;
    u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(res, nullptr, &u, where);
}

static void up_transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                          D3D12_RESOURCE_STATES after)
{
    if (!res || before == after) { return; }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    g_list->ResourceBarrier(1, &b);
}

static bool ensure_upsample_objects()
{
    if (g_up_rs && g_up_pso && g_up_heap) { return true; }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.ShaderRegister = 0;
    p[0].Constants.Num32BitValues = 7;          // DstW, DstH, MvW, MvH, DepW, DepH, DepthK
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable.NumDescriptorRanges = 1;
    p[2].DescriptorTable.pDescriptorRanges = &ranges[1];
    p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3;
    rs.pParameters = p;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> sig, err;
    const HRESULT shr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                                    &sig, &err);
    if (FAILED(shr) || !sig) {
        logf_("[lock] --upsample: root signature FAILED hr=0x%08lX\n",
              static_cast<unsigned long>(shr));
        return false;
    }
    if (FAILED(g_device->CreateRootSignature(0, sig->GetBufferPointer(),
                                             sig->GetBufferSize(),
                                             IID_PPV_ARGS(&g_up_rs)))) {
        logf_("[lock] --upsample: CreateRootSignature FAILED\n");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_up_rs.Get();
    pd.CS.pShaderBytecode = kUpsampleJointCSO;
    pd.CS.BytecodeLength = sizeof(kUpsampleJointCSO);
    if (FAILED(g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_up_pso)))) {
        logf_("[lock] --upsample: CreateComputePipelineState FAILED\n");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4;                       // mv SRV, depth SRV, mv UAV, depth UAV
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_up_heap)))) {
        logf_("[lock] --upsample: descriptor heap FAILED\n");
        return false;
    }
    logf_("[lock] --upsample: joint MV/depth reconstruction ready (depth K=%.0f)\n",
          g_upsample_k);
    return true;
}

static bool ensure_upsample_targets(UINT w, UINT h)
{
    if (g_up_mv && g_up_depth && g_up_w == w && g_up_h == h) { return true; }
    g_up_mv.Reset();
    g_up_depth.Reset();
    g_up_valid = false;
    g_up_slot = mgpu::kMaxSlots;

    auto make = [&](ComPtr<ID3D12Resource>& r, DXGI_FORMAT fmt) -> HRESULT {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1;
        hp.VisibleNodeMask = 1;
        return g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&r));
    };
    if (FAILED(make(g_up_mv, DXGI_FORMAT_R16G16_FLOAT)) ||
        FAILED(make(g_up_depth, DXGI_FORMAT_R32_FLOAT))) {
        logf_("[lock] --upsample: target allocation FAILED (%ux%u)\n", w, h);
        g_up_mv.Reset();
        g_up_depth.Reset();
        return false;
    }
    g_up_mv_state = D3D12_RESOURCE_STATE_COMMON;
    g_up_depth_state = D3D12_RESOURCE_STATE_COMMON;
    g_up_w = w;
    g_up_h = h;
    logf_("[lock] --upsample: reconstruction targets %ux%u\n", w, h);
    return true;
}

// Records the pass for `slot`. Preconditions are checked here rather than at the call
// site so every fallback is a no-op (the tags then use the stream-size resources).
static void record_upsample(UINT slot)
{
    // One-shot diagnostic: when the pass does not run, the reason has to be visible in
    // the log instead of being guessed from a missing "targets" line.
    static bool s_diag = false;
    if (!s_diag) {
        s_diag = true;
        const UINT tw = (g_ctl && slot < mgpu::kMaxSlots) ? g_ctl->frames[slot].render_w : 0;
        const UINT th = (g_ctl && slot < mgpu::kMaxSlots) ? g_ctl->frames[slot].render_h : 0;
        logf_("[lock] --upsample: check up=%d reuse=%d noin=%d nodep=%d of=%d lag=%d"
              " mvp=%d mv=%d dep=%d mvloc=%d deploc=%d ring=%d(%u/%u) geo=%ux%u/%ux%u"
              " target=%ux%u slot=%u\n",
              static_cast<int>(g_upsample), static_cast<int>(g_reuse_frame),
              static_cast<int>(g_no_inputs), static_cast<int>(g_no_depth),
              static_cast<int>(g_fg_of), g_depth_lag,
              static_cast<int>(mv_from_producer()),
              static_cast<int>(g_mv[slot] != nullptr),
              static_cast<int>(g_depth[slot] != nullptr),
              static_cast<int>(g_mv_local[slot] != nullptr),
              static_cast<int>(g_depth_local[slot] != nullptr),
              static_cast<int>(g_depth_ring[g_ring_write] != nullptr),
              g_ring_need, g_ring_count,
              g_geo.mv_w, g_geo.mv_h, g_geo.depth_w, g_geo.depth_h, tw, th, slot);
    }
    if (!g_upsample || g_reuse_frame || !g_ctl) { return; }
    if (g_no_inputs || g_no_depth || g_fg_of || g_depth_lag > 0) { return; }
    if (!mv_from_producer() || !g_mv[slot] || !g_depth[slot]) { return; }
    if (g_ctl->request_mask & mgpu::kRequestRawMv) { return; }   // prepare_stream path
    if (!g_mv_local[slot]) { return; }

    // Depth: copy_inputs_local() writes g_depth_ring[] even with --depth-lag 0 (the tag
    // path reads that ring), so g_depth_local[slot] may legitimately never exist. Use the
    // same resource the tag will use, otherwise this pass is a permanent no-op and the
    // tags end up with MV at render size + depth at stream size (XeFG: ERR_INVALID_ARGUMENT).
    ID3D12Resource* depth_src = nullptr;
    if (g_pf_pub && g_depth_local[slot]) {
        depth_src = g_depth_local[slot].Get();
    } else if (g_ring_need > 0 && g_ring_count > 0 && g_depth_ring[g_ring_write]) {
        depth_src = g_depth_ring[g_ring_write].Get();
    } else if (g_depth_local[slot]) {
        depth_src = g_depth_local[slot].Get();
    }
    if (!depth_src) { return; }

    const UINT mw = g_geo.mv_w, mh = g_geo.mv_h;
    const UINT dw = g_geo.depth_w, dh = g_geo.depth_h;
    if (mw < 256 || mh < 256 || dw < 256 || dh < 256) { return; }

    // Target = the picture XeFG actually receives (the capture/present size), not the
    // game's internal render size. On a 1080p-render -> 4K-output title those differ,
    // and the user wants MV/depth matched to the input picture.
    UINT tw = g_full_w, th = g_full_h;
    if (tw < 256 || th < 256) { tw = g_width; th = g_height; }
    if (tw < 256 || th < 256) {
        tw = g_ctl->frames[slot].render_w;
        th = g_ctl->frames[slot].render_h;
    }
    if (tw < 256 || th < 256) {
        tw = g_ctl->producer.render_w;
        th = g_ctl->producer.render_h;
    }
    if (tw > 8192 || th > 8192) { return; }
    // Nothing to gain when both sources are already at the target size.
    if (tw <= mw && th <= mh && tw <= dw && th <= dh) { return; }
    // The pass only upsamples. A source larger than the target would mean the render size
    // shrank after the stream was sized - skip rather than downsample the wrong way.
    if (mw > tw || mh > th || dw > tw || dh > th) { return; }
    if (!ensure_upsample_objects() || !ensure_upsample_targets(tw, th)) { return; }

    const UINT inc = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_up_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_up_heap->GetGPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = gpu;
    make_srv(g_mv_local[slot].Get(), cpu, DXGI_FORMAT_R16G16_FLOAT);
    cpu.ptr += inc;
    gpu.ptr += inc;
    make_srv(depth_src, cpu, DXGI_FORMAT_R32_FLOAT);
    cpu.ptr += inc;
    gpu.ptr += inc;
    const D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu = gpu;
    make_uav(g_up_mv.Get(), cpu, DXGI_FORMAT_R16G16_FLOAT);
    cpu.ptr += inc;
    make_uav(g_up_depth.Get(), cpu, DXGI_FORMAT_R32_FLOAT);

    // The copies left both local textures in COPY_DEST; the targets start in COMMON.
    up_transition(g_mv_local[slot].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    up_transition(depth_src, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier_to(g_up_mv.Get(), g_up_mv_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier_to(g_up_depth.Get(), g_up_depth_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    struct UpConsts { UINT dst_w, dst_h, mv_w, mv_h, dep_w, dep_h; float k; }
        c{ tw, th, mw, mh, dw, dh, g_upsample_k };
    g_list->SetComputeRootSignature(g_up_rs.Get());
    g_list->SetPipelineState(g_up_pso.Get());
    ID3D12DescriptorHeap* heaps[] = { g_up_heap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetComputeRoot32BitConstants(0, 7, &c, 0);
    g_list->SetComputeRootDescriptorTable(1, srv_gpu);
    g_list->SetComputeRootDescriptorTable(2, uav_gpu);
    g_list->Dispatch((tw + 7) / 8, (th + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = g_up_mv.Get();
    g_list->ResourceBarrier(1, &ub);
    ub.UAV.pResource = g_up_depth.Get();
    g_list->ResourceBarrier(1, &ub);

    barrier_to(g_up_mv.Get(), g_up_mv_state, D3D12_RESOURCE_STATE_COMMON);
    barrier_to(g_up_depth.Get(), g_up_depth_state, D3D12_RESOURCE_STATE_COMMON);
    up_transition(g_mv_local[slot].Get(),
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_COMMON);
    up_transition(depth_src,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_COMMON);

    g_up_slot = slot;
    g_up_valid = true;
    if (!g_up_told) {
        g_up_told = true;
        logf_("[lock] --upsample: MV/depth rebuilt from %ux%u / %ux%u to %ux%u on this GPU"
              " (tags use the reconstructed pair)\n", mw, mh, dw, dh, tw, th);
    }
}

