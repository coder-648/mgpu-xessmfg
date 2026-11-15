// ============================================================================
//  consumer module: stream_auto.cpp
//
//  --stream-auto per-stream StreamPercent controller and warp PSO setup.
//
//  Included by src/xess_mfg_consumer/unity.cpp as part of the unity build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

#include "../consumer_internal.h"
// -------------------- --stream-auto: slow per-stream StreamPercent controller ----------
// Budget comes from the WGC hop (`g_ms_age`'s low percentile), not its mean: a late WGC
// only adds slack, an early one is what breaks the copy. The model is deliberately
// conservative and rate-aware:
//   copy = bytes/13 GB/s (producer write) + bytes/17.6 GB/s (consumer read, measured)
//   bandwidth = bytes * input_fps <= 5.5 GB/s
// It only steps one rung at a time, needs 2 windows to go down and 4 to go back up, and
// never changes more than once per 2 s (a change clears the published frames).
static void age_push(double a)
{
    if (a > 0.0 && a < 500.0) {
        g_age_ring[g_age_i] = a;
        g_age_i = (g_age_i + 1) % 64;
        if (g_age_n < 64) { ++g_age_n; }
    }
}

// Median, not a low percentile. The WGC stream carries duplicate/re-composited frames
// whose SystemRelativeTime is fresh, so its low tail is not a deadline: measured on
// CP2077, p10 read 0.5-4.6 ms while the median was ~12 ms. The producer-side term below
// is what protects the frame rate; the age only has to cover the consumer's read.
static double age_median()
{
    if (g_age_n < 8) { return 0.0; }
    double v[64];
    memcpy(v, g_age_ring, sizeof(double) * g_age_n);
    for (unsigned i = 1; i < g_age_n; ++i) {
        const double key = v[i];
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; --j; }
        v[j + 1] = key;
    }
    return v[g_age_n / 2];
}

// Bytes for BOTH streams at the given depth percent. In asymmetric mode MV stays at 100%.
static double stream_pair_bytes(UINT percent, UINT rw, UINT rh, bool asym)
{
    const double one = 4.0 * static_cast<double>(rw) * static_cast<double>(rh);
    const double s = 100.0 / static_cast<double>(percent ? percent : 100u);
    return asym ? (one + one * s * s) : (2.0 * one * s * s);
}

static void stream_controller_tick(UINT slot)
{
    if (!g_ctl) { return; }
    if (g_ctl->version < 15) {
        static bool told = false;
        if (!told && (g_stream_auto || g_stream_asym)) {
            told = true;
            logf_("[consumer] --stream-auto/--stream-asym need producer protocol v15;"
                  " this producer is v%u - the request is ignored\n", g_ctl->version);
        }
        return;
    }
    if (!g_stream_auto && !g_stream_asym) {
        if (g_ctl->requested_percent_mv != 0 || g_ctl->requested_percent_depth != 0) {
            g_ctl->requested_percent_mv = 0;
            g_ctl->requested_percent_depth = 0;
        }
        return;
    }
    if (!g_stream_auto) {
        // Asymmetric only: MV 1:1, depth keeps the producer's ini value.
        g_ctl->requested_percent_mv = 100;
        g_ctl->requested_percent_depth = 0;
        return;
    }

    const double age = age_median();
    const UINT rw = g_ctl->producer.render_w, rh = g_ctl->producer.render_h;
    if (age <= 0.0 || rw < 256 || rh < 256) { return; }
    const double fps = (g_arrival_ms_ema > 0.5) ? 1000.0 / g_arrival_ms_ema : 0.0;
    const double t_in = (fps > 0.0) ? 1000.0 / fps : 0.0;
    const double margin = 1.0;
    const double age_budget = age - margin;

    const double frame_ms = (slot < mgpu::kMaxSlots) ? g_ctl->frames[slot].frame_time_ms : 0.0;
    static bool s_ctrl_diag = false;
    if (!s_ctrl_diag) {
        s_ctrl_diag = true;
        logf_("[consumer] autoscale: first window v=%u frame=%.2fms published=%llu"
              " bytes=%u/%u active=%u/%u age=%.1f slot=%u\n",
              g_ctl->version, frame_ms,
              static_cast<unsigned long long>(
                  (slot < mgpu::kMaxSlots) ? g_ctl->frames[slot].published : 0),
              g_ctl->stream_bytes[1], g_ctl->stream_bytes[2],
              g_ctl->active_percent_mv, g_ctl->active_percent_depth, age, slot);
    }
    if (frame_ms < 1.0) { return; }

    // Start from what the producer is actually using (ini 150, or the previous bridge's
    // request) instead of assuming 100; otherwise the first decision "improves" to 100
    // without knowing the current state.
    if (g_auto_percent == 100 && g_ctl->active_percent_depth >= 100) {
        g_auto_percent = g_ctl->active_percent_depth;
    }

    // The producer copy is on the DMA engine and happens after present, so it is NOT part
    // of the frame's critical path any more. The budget that matters is the consumer's:
    //   gpu + present <= T_in - margin        (what actually drops frames)
    // plus the transport checks (age for the read, PCIe bandwidth, source lifetime).
    const double gpu_present = g_ms_throttle + g_ms_present;

    static const UINT kLadder[] = { 100, 125, 150, 175, 200, 250, 300, 400 };
    constexpr UINT kLadderN = sizeof(kLadder) / sizeof(kLadder[0]);
    UINT best = kLadder[kLadderN - 1];       // no rung fits -> lowest quality
    for (UINT p : kLadder) {
        const double bytes = stream_pair_bytes(p, rw, rh, g_stream_asym);
        const double tp = bytes / 13.0e6;     // producer write, ms (source-lifetime check)
        const double tc = bytes / 17.6e6;     // consumer read (measured at 4K/1:1)
        const bool fits = (tp <= 10.0) && (tc <= age_budget);
        const bool bw_ok = (fps <= 0.0) || (bytes * fps <= 5.5e9);
        if (fits && bw_ok) { best = p; break; }   // ascending: first = best quality
    }

    UINT next_worse = kLadder[kLadderN - 1];
    for (UINT p : kLadder) { if (p > g_auto_percent) { next_worse = p; break; } }

    if (gpu_present > t_in - 1.0) {
        // Over budget: force at least one rung worse. Do not wait for the transport model.
        if (next_worse > best) { best = next_worse; }
    } else if (best < g_auto_percent && gpu_present > t_in - 1.5) {
        // Under budget but without 1.5 ms of headroom: do not improve (the extra MV detail
        // costs XeFG GPU time and would put us back over).
        best = g_auto_percent;
    }

    const ULONGLONG now = GetTickCount64();
    const bool first_decision = (g_auto_last_change == 0);
    if (best == g_auto_percent) {
        g_auto_down = g_auto_up = 0;
    } else if (!first_decision && now - g_auto_last_change < 2000) {
        // dwell: a change is expensive (frames are cleared + local copies rebuilt)
    } else if (best < g_auto_percent) {
        // improving (lower percent) needs 4 windows of headroom
        ++g_auto_down;
        g_auto_up = 0;
        if (g_auto_down >= (first_decision ? 1u : 4u)) {
            const double b = stream_pair_bytes(best, rw, rh, g_stream_asym);
            logf_("[consumer] autoscale: age_med %.1f, gpu+present %.1f, frame %.1f, T_in %.1f"
                  " -> %u (was %u, %.1f MB/frame, read %.1f ms, %.1f fps)\n",
                  age, gpu_present, frame_ms, t_in, best, g_auto_percent, b / 1.0e6,
                  b / 17.6e6, fps);
            g_auto_percent = best;
            g_auto_last_change = now;
            g_auto_down = g_auto_up = 0;
        }
    } else {
        // degrading (higher percent) needs 2 windows; over-budget is already forced above
        ++g_auto_up;
        g_auto_down = 0;
        if (g_auto_up >= (first_decision ? 1u : 2u)) {
            const double b = stream_pair_bytes(best, rw, rh, g_stream_asym);
            logf_("[consumer] autoscale: age_med %.1f, gpu+present %.1f, frame %.1f, T_in %.1f"
                  " -> %u (was %u, %.1f MB/frame)\n",
                  age, gpu_present, frame_ms, t_in, best, g_auto_percent, b / 1.0e6);
            g_auto_percent = best;
            g_auto_last_change = now;
            g_auto_down = g_auto_up = 0;
        }
    }

    g_ctl->requested_percent_mv    = g_stream_asym ? 100u : g_auto_percent;
    g_ctl->requested_percent_depth = g_auto_percent;
}

static bool init_warp()
{
    const UINT w = g_width, h = g_height;

    auto make_tex = [&](ComPtr<ID3D12Resource>& res) -> HRESULT {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1;
        hp.VisibleNodeMask = 1;
        return g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&res));
    };

    if (FAILED(make_tex(g_hist[0])) || FAILED(make_tex(g_hist[1])) ||
        FAILED(make_tex(g_interp))) {
        logf_("[consumer] --fg warp: history textures FAILED (%ux%u)\n", w, h);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = 3;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_warp_srv)))) {
        return false;
    }
    const UINT srv_stride = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE base = g_warp_srv->GetCPUDescriptorHandleForHeapStart();
    make_srv(g_hist[0].Get(), base, DXGI_FORMAT_R16G16B16A16_FLOAT);
    D3D12_CPU_DESCRIPTOR_HANDLE d1 = base;
    d1.ptr += srv_stride;
    make_srv(g_hist[1].Get(), d1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    // Slot 2 (motion vectors) is written each frame for whichever slot is in use.

    sh.NumDescriptors = 1;
    if (FAILED(g_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srv_interp)))) {
        return false;
    }
    make_srv(g_interp.Get(), g_srv_interp->GetCPUDescriptorHandleForHeapStart(),
             DXGI_FORMAT_R16G16B16A16_FLOAT);

    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = 1;
    if (FAILED(g_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_interp_rtv)))) {
        return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_device->CreateRenderTargetView(g_interp.Get(), &rtv,
                                     g_interp_rtv->GetCPUDescriptorHandleForHeapStart());

    logf_("[consumer] --fg warp: %ux%u history + interpolation targets ready\n", w, h);
    return true;
}

static void warp_convert(ID3D12Resource* bb, UINT bbi, ID3D12DescriptorHeap* srv_heap);
static bool init_warp_pso()
{
    ComPtr<ID3DBlob> vs, ps, err;
    HRESULT hr = D3DCompile(k_warp_hlsl, strlen(k_warp_hlsl), "warp.hlsl", nullptr, nullptr,
                            "vs_main", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) {
        logf_("[consumer] warp VS FAILED: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    hr = D3DCompile(k_warp_hlsl, strlen(k_warp_hlsl), "warp.hlsl", nullptr, nullptr,
                    "ps_main", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr)) {
        logf_("[consumer] warp PS FAILED: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 3;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 12;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;

    ComPtr<ID3DBlob> sig, serr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &serr);
    if (FAILED(hr)) { logf_("[consumer] warp SerializeRootSignature FAILED\n"); return false; }
    hr = g_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                       IID_PPV_ARGS(&g_warp_root));
    if (FAILED(hr)) { logf_("[consumer] warp CreateRootSignature FAILED\n"); return false; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_warp_root.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = 0xFFFFFFFFu;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.SampleDesc.Count = 1;
    hr = g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_warp_pso));
    if (FAILED(hr)) {
        logf_("[consumer] warp CreateGraphicsPipelineState FAILED hr=0x%08lX\n",
              static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

