// ============================================================================
//  lock_frame module: 100_convert_warp.inc
//
//  Resize kernel constants, convert draw, warp present path.
//
//  Included by lock_frame/lock_frame.cpp as part of the single-translation-unit build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

// ------------------------------------------------------- resize filter constants
// Taps that cover 2*support at the current scale, so the same kernel works for
// 1.33x (5K -> 4K) and 2x (5K -> 1440p) without touching the shader.
static void conv_constants(float* cc, UINT dst_w, UINT dst_h)
{
    float taps = 1.0f;
    float down = static_cast<float>(g_down);
    float support = g_down_support;
    if (g_down_support > 0.0f && dst_w > 0 && dst_h > 0 && g_width > 0 && g_height > 0) {
        const float sx = static_cast<float>(g_width) / static_cast<float>(dst_w);
        const float sy = static_cast<float>(g_height) / static_cast<float>(dst_h);
        if (fabsf(sx - 1.0f) < 1e-3f && fabsf(sy - 1.0f) < 1e-3f) {
            // 1:1 (4K capture -> 4K present): the resize is an identity copy, so the
            // single tap is exact. Paying for a 7x7 kernel here would only cost time,
            // and any kernel rounding would soften a picture that needs no resampling.
            down = 0.0f;
            support = 0.0f;
            static bool logged = false;
            if (!logged) {
                logged = true;
                logf_("[lock] resize filter: not needed - %ux%u -> %ux%u is 1:1\n",
                      g_width, g_height, dst_w, dst_h);
            }
        } else {
            const float s = (sx > sy) ? sx : sy;
            taps = ceilf(2.0f * g_down_support * s) + 1.0f;
            if (taps < 2.0f) { taps = 2.0f; }
            if (taps > 16.0f) { taps = 16.0f; }
        }
    }
    cc[0] = 80.0f;                                   // scRGB 1.0 = 80 nits
    cc[1] = static_cast<float>(g_colour_mode);
    cc[2] = g_sharpen;
    cc[3] = down;
    cc[4] = support;
    cc[5] = taps;
    cc[6] = static_cast<float>(dst_w);
    cc[7] = static_cast<float>(dst_h);
    cc[8] = cc[9] = cc[10] = 0.0f;
    // 四角 marker 回填（锁帧版由 marker 配对驱动；只有最近真的解出 marker 才开）。
    cc[11] = lock_marker_should_repair() ? 1.0f : 0.0f;
}

static void warp_convert(ID3D12Resource* bb, UINT bbi, ID3D12DescriptorHeap* srv_heap)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(bbi & 3) * g_rtv_stride;
    if (!g_rtv_res[bbi & 3]) {
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_device->CreateRenderTargetView(bb, &rd, rtv);
        g_rtv_res[bbi & 3] = bb;
    }

    D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(g_full_w),
                       static_cast<float>(g_full_h), 0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(g_full_w), static_cast<LONG>(g_full_h) };
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &scissor);
    g_list->SetGraphicsRootSignature(g_conv_root.Get());
    g_list->SetPipelineState(g_conv_pso.Get());
    ID3D12DescriptorHeap* heaps[] = { srv_heap };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetGraphicsRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
    float cc[12];
    conv_constants(cc, g_full_w, g_full_h);
    g_list->SetGraphicsRoot32BitConstants(1, 12, cc, 0);
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->DrawInstanced(3, 1, 0, 0);
}

static bool service_shared_layout();

static void run_frame_warp(ID3D11Texture2D* colour11)
{
    if (!service_shared_layout()) { return; }
    ++g_present_id;
    if (g_no_throttle) { g_ms_throttle = 0.0; } else { gpu_throttle(); }
    // Create the second (allocator, list) pair up front, so the swap in the middle of the
    // frame is always a plain pointer exchange - not on the first frame only.
    warp_ensure_alt_pair();
    const double t_work0 = qpc_ms();

    if (g_colour_mutex) { g_colour_mutex->AcquireSync(0, 20); }
    g_ctx11->CopyResource(g_shared11.Get(), colour11);
    if (g_colour_mutex) { g_colour_mutex->ReleaseSync(0); }
    const UINT64 fv = ++g_fence11_value;
    g_ctx11_4->Signal(g_fence11.Get(), fv);
    g_queue->Wait(g_fence12.Get(), fv);

    const UINT slots = (g_ctl->slots != 0) ? g_ctl->slots : 1;
    const UINT slot = g_mv_slot % slots;
    wait_slot(slot);

    // NGX's MV.Scale is the number of motion-vector units per screen UV, i.e. the
    // game reports its render size here (2560x1440 for a 5K DLSS Performance run), so
    // the delta is mv / scale. A zero value means "no scale given", in which case the
    // same formula falls back to 1/render-size.
    const mgpu::FrameShared& fr = g_ctl->frames[slot];
    const float rw = static_cast<float>(g_ctl->streams[1].width ? g_ctl->streams[1].width : 1);
    const float rh = static_cast<float>(g_ctl->streams[1].height ? g_ctl->streams[1].height : 1);
    float msx = (fr.mv_scale_x > 0.0f) ? (1.0f / fr.mv_scale_x) : (1.0f / rw);
    float msy = (fr.mv_scale_y > 0.0f) ? (1.0f / fr.mv_scale_y) : (1.0f / rh);
    if (g_mv_mul > 0.0f) { msx = g_mv_mul; msy = g_mv_mul; }
    msx *= g_mv_sign;
    msy *= g_mv_sign;
    if (!g_warp_mv_logged) {
        g_warp_mv_logged = true;
        logf_("[lock] warp: mv scale=(%.8f, %.8f) (game said %.8f, %.8f) clamp=%.2f\n",
              msx, msy, fr.mv_scale_x, fr.mv_scale_y, g_warp_clamp);
    }

    const UINT wi = g_hist_write;
    const UINT pi = 1u - wi;
    const bool interp_ok = g_hist_valid;

    g_alloc->Reset();
    g_list->Reset(g_alloc.Get(), nullptr);

    // --- list 1: history update, then the interpolated (midpoint) frame --------
    D3D12_RESOURCE_STATES shared_state = D3D12_RESOURCE_STATE_COMMON;
    barrier_to(g_shared12.Get(), shared_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier_to(g_hist[wi].Get(), g_hist_state[wi], D3D12_RESOURCE_STATE_COPY_DEST);
    g_list->CopyResource(g_hist[wi].Get(), g_shared12.Get());
    barrier_to(g_hist[wi].Get(), g_hist_state[wi], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barrier_to(g_shared12.Get(), shared_state, D3D12_RESOURCE_STATE_COMMON);

    ID3D12Resource* bb1 = nullptr;
    UINT bbi1 = 0;
    bool have_bb1 = false;

    if (interp_ok) {
        if (g_no_inputs) { ensure_dummy_inputs(slot); } else { copy_inputs_local(slot); }

        // Motion vectors occupy slot 2 of the warp heap. That slot was last used two
        // frames ago and we waited for the previous frame, so rewriting it is safe.
        const UINT srv_stride = g_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE mv_where = g_warp_srv->GetCPUDescriptorHandleForHeapStart();
        mv_where.ptr += 2ull * srv_stride;
        if (g_mv_local[slot]) {
            make_srv(g_mv_local[slot].Get(), mv_where, DXGI_FORMAT_R16G16_FLOAT);
        } else {
            make_srv(g_hist[0].Get(), mv_where, DXGI_FORMAT_R16G16B16A16_FLOAT);
        }

        barrier_to(g_interp.Get(), g_interp_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE irtv = g_interp_rtv->GetCPUDescriptorHandleForHeapStart();
        D3D12_VIEWPORT wvp{ 0.0f, 0.0f, static_cast<float>(g_width),
                            static_cast<float>(g_height), 0.0f, 1.0f };
        D3D12_RECT wsc{ 0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height) };
        g_list->OMSetRenderTargets(1, &irtv, FALSE, nullptr);
        g_list->RSSetViewports(1, &wvp);
        g_list->RSSetScissorRects(1, &wsc);
        g_list->SetGraphicsRootSignature(g_warp_root.Get());
        g_list->SetPipelineState(g_warp_pso.Get());
        ID3D12DescriptorHeap* wheaps[] = { g_warp_srv.Get() };
        g_list->SetDescriptorHeaps(1, wheaps);
        g_list->SetGraphicsRootDescriptorTable(0, g_warp_srv->GetGPUDescriptorHandleForHeapStart());
        // hist[wi] is the newest frame; it sits in descriptor 0 only when wi == 0
        const float wc[8] = { msx, msy, (wi == 0) ? 1.0f : 0.0f, g_warp_clamp,
                              g_warp_conf, g_warp_conf_on, g_warp_search, 0.0f };
        g_list->SetGraphicsRoot32BitConstants(1, 8, wc, 0);
        g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_list->DrawInstanced(3, 1, 0, 0);
        barrier_to(g_interp.Get(), g_interp_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Present the midpoint first so the display order is prev -> mid -> current.
        bbi1 = g_swapchain->GetCurrentBackBufferIndex();
        have_bb1 = back_buffer(bbi1, &bb1);
        if (have_bb1) {
            barrier_to(bb1, g_bb_state[bbi1 & 3], D3D12_RESOURCE_STATE_RENDER_TARGET);
            warp_convert(bb1, bbi1, g_srv_interp.Get());
            barrier_to(bb1, g_bb_state[bbi1 & 3], D3D12_RESOURCE_STATE_PRESENT);
        }
    }

    g_list->Close();
    {
        ID3D12CommandList* lists[] = { g_list.Get() };
        g_queue->ExecuteCommandLists(1, lists);
        g_queue->Signal(g_frame_fence.Get(), ++g_frame_fence_value);
    }

    if (have_bb1) {
        // QPC here: g_present_ms is reported and used by the "display paces the loop"
        // warning, and GetTickCount64's 15.6 ms granularity made it read 0 or 16.
        const double t_p = qpc_ms();
        g_swapchain->Present(0, g_present_flags());
        const double pres_ms = qpc_ms() - t_p;
        g_present_ms = static_cast<DWORD>(pres_ms + 0.5);
        g_ms_present = g_ms_present * 0.9 + pres_ms * 0.1;

        // Half a tick later the real frame goes out. XeSS-FG decides this timing
        // internally for its own path (and gets it wrong on non-Intel GPUs), but here
        // we own Present, so the interpolated frame and the real frame land one half
        // tick apart - exactly where they belong in time.
        if (g_pace_ms > 2.0) {
            const double mid = qpc_ms() + g_pace_ms * 0.5;
            while (qpc_ms() < mid) { Sleep(0); }
        }
    }

    // --- list 2: the real frame, which has to go out after the midpoint -----
    // Its own (allocator, list) pair: list 1 may still be executing and resetting its
    // allocator now would be undefined behaviour (see warp_use_alt_pair()). This is
    // deliberately outside the have_bb1 branch - without a first present the list is
    // still closed here and the second list has to be opened either way.
    warp_use_alt_pair();
    g_alloc->Reset();
    g_list->Reset(g_alloc.Get(), nullptr);

    const UINT bbi2 = g_swapchain->GetCurrentBackBufferIndex();
    ID3D12Resource* bb2 = nullptr;
    const bool have_bb2 = back_buffer(bbi2, &bb2);
    if (have_bb2) {
        barrier_to(bb2, g_bb_state[bbi2 & 3], D3D12_RESOURCE_STATE_RENDER_TARGET);
        warp_convert(bb2, bbi2, g_srv_heap.Get());
        barrier_to(bb2, g_bb_state[bbi2 & 3], D3D12_RESOURCE_STATE_PRESENT);
    }
    g_list->Close();
    {
        ID3D12CommandList* lists[] = { g_list.Get() };
        g_queue->ExecuteCommandLists(1, lists);
        g_queue->Signal(g_frame_fence.Get(), ++g_frame_fence_value);
    }
    if (have_bb2) {
        const double t_p = qpc_ms();
        g_swapchain->Present(0, g_present_flags());
        const double pres_ms = qpc_ms() - t_p;
        g_present_ms = static_cast<DWORD>(pres_ms + 0.5);
        g_ms_present = g_ms_present * 0.9 + pres_ms * 0.1;
    }

    g_hist_write = pi;
    g_hist_valid = true;
    g_mv_slot = (g_mv_slot + 1) % slots;
    ++g_frames;
    g_ms_work = g_ms_work * 0.9 + (qpc_ms() - t_work0) * 0.1;

    if ((g_frames % g_report_every) == 1) {
        // What did we actually read out of the shared frame record? "jitter=(0.000, 0.000)"
        // in the line below can mean "the producer sent zero" or "we paired with the wrong
        // slot", and those need opposite fixes - so print the raw record once per window.
        if (g_ctl) {
            const mgpu::FrameShared& raw_fr = g_ctl->frames[slot];
            logf_("[lock] slot %u record: jitter=(%.4f, %.4f) mvscale=(%.1f, %.1f)"
                  " render=%ux%u frame_id=%llu key=0x%llX pub=%llu active=0x%X\n",
                  slot, raw_fr.jitter_x, raw_fr.jitter_y, raw_fr.mv_scale_x, raw_fr.mv_scale_y,
                  raw_fr.render_w, raw_fr.render_h,
                  (unsigned long long)raw_fr.frame_id, (unsigned long long)raw_fr.frame_key,
                  (unsigned long long)raw_fr.published, g_ctl->active_mask);
        }
        const ULONGLONG now_ms = GetTickCount64();
        const double win = (g_t_report && now_ms > g_t_report)
                         ? (now_ms - g_t_report) / 1000.0 : 0.0;
        g_t_report = now_ms;
        const double secs = (now_ms / 1000.0) - g_t0;
        const double in_fps = (secs > 0) ? g_frames / secs : 0.0;
        g_show_fps = static_cast<int>(in_fps * 2.0 + 0.5);
        logf_("[lock] warp frame %llu  slot %u  (%.1f fps in)  present=%ums"
              "  bb=%ux%u  out=2x  show=%d\n",
              (unsigned long long)g_frames, slot, in_fps,
              g_present_ms, g_full_w, g_full_h, g_show_fps);
        logf_("[lock]   breakdown: capture=%.1fms work=%.1fms present=%.1fms gpu=%.1fms"
              " timeouts=%u mode=%d\n", g_ms_capture, g_ms_work, g_ms_present,
              g_ms_throttle, g_slot_timeouts, g_colour_mode);
    }

    assert_topmost();
}

// --pace: re-time our presents to a fixed cadence. The game hands us frames at an
// alternating 16.7/33.3 ms pattern (RTSS lands off the 120 Hz grid), and XeSS-FG
// assumes an even input cadence, so the output judders even though nothing is dropped.
// Sampling "the newest frame that exists at tick N" turns that into a steady 80 Hz.
// (The globals live above run_frame_warp, which needs g_pace_ms for its own presents.)

