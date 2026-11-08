// ============================================================================
// shared_layout.h - layout of the cross-process stream slots (protocol v9)
//
// Included by game_capture.cpp *after* the capture-state globals (it uses them):
// it is only a slice of that translation unit, not a standalone module.
//
// Decoupling rule (v9): the add-on is a *dumb producer* - it copies the frame's
// MV/depth into slot k of a fixed, pre-placed buffer and publishes a fence. The bridge
// is a *dumb consumer* - it copies slot k out. Neither waits for the other, and neither
// tells the other how to work:
//
//   * a slot is always kStreamMaxW x kStreamMaxH, placed once, never moved;
//   * the live part is the top-left `width x height` rectangle, published in
//     StreamShared. Both sides copy exactly that rectangle, so the only thing a
//     resolution change touches is two integers in the mapping;
//   * `generation` tells the consumer "the rectangle changed" (informational), and
//     `magic` is only 0 while the mapping itself is being created.
//
// The v8 design re-placed the slots instead, which needed a magic/invalidate + ack
// handshake and a bounded stall on the render thread. None of that exists any more.
// ============================================================================
#pragma once

// w/h are the *content* size; slot_w/slot_h are what actually gets placed.
struct StreamSpec { DXGI_FORMAT format; UINT w, h, slot_w, slot_h; };

static void stream_specs(UINT w, UINT h, StreamSpec out[3])
{
    // Fixed capacity: every stream is placed at the maximum we support, once.
    const UINT sw = mgpu::kStreamMaxW;
    const UINT sh = mgpu::kStreamMaxH;
    // colour is unused with Color=0 (the bridge takes colour from WGC); its slot size
    // stays 0 in that case, exactly like before.
    out[0] = { DXGI_FORMAT_R10G10B10A2_UNORM, 5120, 2880, 5120, 2880 };
    out[1] = { DXGI_FORMAT_R16G16_FLOAT,     w, h, sw, sh };   // motion vectors
    // R32_TYPELESS: the only depth format that allocates on a cross-adapter heap here.
    out[2] = { DXGI_FORMAT_R32_TYPELESS,     w, h, sw, sh };   // depth
}

// Fills ctl->streams[] and returns the end offset (bytes) of the used area.
static uint64_t layout_streams(ID3D12Device* device, mgpu::SharedControl* ctl, UINT w, UINT h)
{
    StreamSpec specs[3];
    stream_specs(w, h, specs);

    uint64_t cursor = mgpu::kDataStart;
    for (int i = 0; i < 3; ++i) {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = specs[i].slot_w;
        rd.Height           = specs[i].slot_h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = specs[i].format;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

        const D3D12_RESOURCE_ALLOCATION_INFO ai = device->GetResourceAllocationInfo(0, 1, &rd);
        const bool needed = (i != 0) || g_capture_colour;
        const uint64_t slot_size = needed ? mgpu::align64k(ai.SizeInBytes) : 0;

        ctl->streams[i].width     = specs[i].w;
        ctl->streams[i].height    = specs[i].h;
        ctl->streams[i].format    = static_cast<uint32_t>(specs[i].format);
        ctl->streams[i].offset    = cursor;
        ctl->streams[i].slot_size = slot_size;
        ctl->streams[i].slot_w    = specs[i].slot_w;
        ctl->streams[i].slot_h    = specs[i].slot_h;
        cursor += slot_size * mgpu::kMaxSlots;
    }
    return cursor;
}

static bool place_stream_slots(ID3D12Device* device, ID3D12Heap* heap,
                               const mgpu::StreamShared& st,
                               ComPtr<ID3D12Resource>(&dst)[mgpu::kMaxSlots])
{
    // Place at the slot capacity, not at the content size: the pitch must not depend on
    // the game's current upscaler preset (that is the whole point of v9).
    const UINT pw = st.slot_w ? st.slot_w : st.width;
    const UINT ph = st.slot_h ? st.slot_h : st.height;
    for (UINT s = 0; s < mgpu::kMaxSlots; ++s) {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = pw;
        rd.Height           = ph;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = static_cast<DXGI_FORMAT>(st.format);
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

        const uint64_t off = st.offset + st.slot_size * s;
        if (FAILED(device->CreatePlacedResource(heap, off, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                nullptr, IID_PPV_ARGS(&dst[s])))) {
            logf_("shared: CreatePlacedResource FAILED (%ux%u fmt %u slot %u)\n",
                  pw, ph, st.format, s);
            return false;
        }
    }
    return true;
}

// The mapping is created once with room for kStreamMaxW x kStreamMaxH: a rebuild only
// re-places resources inside it, so the consumer's mapping/heap/fence handles stay
// valid (recreating the mapping would leave the consumer holding the old object).
static uint64_t reserve_bytes(ID3D12Device* device)
{
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = mgpu::kStreamMaxW;
    rd.Height           = mgpu::kStreamMaxH;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_R32_TYPELESS;   // 4 B/px, same as R16G16_FLOAT
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    const D3D12_RESOURCE_ALLOCATION_INFO ai = device->GetResourceAllocationInfo(0, 1, &rd);
    const uint64_t slot = mgpu::align64k(ai.SizeInBytes);

    uint64_t total = mgpu::kDataStart + 2ull * slot * mgpu::kMaxSlots;   // MV + depth
    if (g_capture_colour) { total += slot * mgpu::kMaxSlots; }
    return total;
}

// Target stream size, straight from the same rules ensure_shared() uses.
static void stream_target_size(UINT* w, UINT* h)
{
    UINT mw = 2560, mh = 1440;
    if (g_inputs.render_w >= 256 && g_inputs.render_h >= 256) {
        mw = g_inputs.render_w;
        mh = g_inputs.render_h;
    }
    if (g_render_w_ini >= 256 && g_render_h_ini >= 256) {
        mw = g_render_w_ini;
        mh = g_render_h_ini;
    }
    // StreamPercent + StreamHeight, in the same order and with the same rounding as
    // stream_target_from() in game_capture.cpp - the slots here and the downsample targets
    // there have to agree exactly, or the copy reads the wrong rectangle.
    if (g_stream_percent > 100 || g_stream_height > 0) {
        UINT w = mw, h = mh;
        stream_target_from(mw, mh, &w, &h);
        mw = w;
        mh = h;
    }
    if (g_stream_align > 1) {
        const UINT a = static_cast<UINT>(g_stream_align);
        if (mw % a) { mw -= mw % a; }
        if (mh % a) { mh -= mh % a; }
    }
    *w = mw;
    *h = mh;
}

// Blocks the render thread (bounded) until everything we recorded on the game queue has
// finished. Producer-internal only: ds_prepare() recreates our own pass targets when the
// render size changes. v9 never asks the consumer for anything.
static ComPtr<ID3D12Fence> g_idle_fence;
static HANDLE g_idle_event = nullptr;
static UINT64 g_idle_value = 0;

static bool wait_game_queue_idle(UINT timeout_ms)
{
    if (!g_game_queue || !g_game_device) { return false; }
    if (!g_idle_fence) {
        if (FAILED(g_game_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                              IID_PPV_ARGS(&g_idle_fence)))) {
            return false;
        }
        g_idle_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_idle_event) { return false; }
    }
    const UINT64 v = ++g_idle_value;
    if (FAILED(g_game_queue->Signal(g_idle_fence.Get(), v))) { return false; }
    if (g_idle_fence->GetCompletedValue() >= v) { return true; }
    if (FAILED(g_idle_fence->SetEventOnCompletion(v, g_idle_event))) { return false; }
    return WaitForSingleObject(g_idle_event, timeout_ms) == WAIT_OBJECT_0;
}

// The render size changed: publish the new content rectangle. That is ALL the producer
// does - nothing is re-placed, nothing is invalidated, nobody is waited for, and the
// render thread never stalls (the pass targets are fixed-capacity, see ds_prepare()).
static bool update_content_size(ID3D12Device* device, UINT w, UINT h)
{
    auto* ctl = reinterpret_cast<mgpu::SharedControl*>(g_map_view);
    if (!ctl || !g_shared_heap || !g_shared_ready) { return false; }
    (void)device;

    // v10 seqlock: odd while the (width,height) pair is being rewritten, even when it is
    // consistent. The consumer retries while it is odd or changed underneath it, so it can
    // never mix an old width with a new height (the reason the old code needed a clamp).
    for (int i = 1; i <= 2; ++i) {
        ctl->streams[i].seq += 1;               // odd: being written
        MemoryBarrier();
        ctl->streams[i].width  = w;
        ctl->streams[i].height = h;
        MemoryBarrier();
        ctl->streams[i].seq += 1;               // even: stable
    }
    ctl->producer.render_w = w;
    ctl->producer.render_h = h;
    MemoryBarrier();
    ctl->generation += 1;          // informational: "the rectangle changed"
    MemoryBarrier();

    g_stream_w = w;
    g_stream_h = h;
    g_mv_w = w;
    g_mv_h = h;
    // The MV/depth pass keeps rendering into the same (capacity-sized) targets; only the
    // viewport/scissor that says how much of them is live moves.
    g_ds_w = w;
    g_ds_h = h;

    // Drop the half-paired slot bookkeeping; the fence values stay monotonic (the
    // consumer keeps its own expect counter).
    memset(ctl->frames, 0, sizeof(ctl->frames));
    g_slot_index = 0;

    logf_("shared: content size is now %ux%u (gen %u) - slots stay %ux%u, nothing rebuilt\n",
          w, h, ctl->generation, mgpu::kStreamMaxW, mgpu::kStreamMaxH);
    return true;
}

// Called every frame from on_begin_effects: did the upscaler move to a new render size?
static void maybe_update_content_size(ID3D12Device* device)
{
    if (!g_shared_ready || !device) { return; }

    // Test hook: [capture] TestSize=<frames> toggles the published rectangle between the
    // real size and 90% of it every N frames, so this path can be exercised (and
    // regression tested) without touching the game's upscaler settings.
    static bool s_test_logged = false;
    if (g_test_rebuild > 0 && !s_test_logged) {
        s_test_logged = true;
        logf_("shared: TEST hook TestSize=%d frames - forcing content size changes\n",
              g_test_rebuild);
    }
    if (g_test_rebuild > 0 && g_frames_seen > 0 &&
        (g_frames_seen % static_cast<UINT>(g_test_rebuild)) == 0) {
        static bool s_small = false;
        s_small = !s_small;
        UINT w = 0, h = 0;
        stream_target_size(&w, &h);
        const UINT tw = s_small ? ((w * 9u / 10u) & ~1u) : w;
        const UINT th = s_small ? ((h * 9u / 10u) & ~1u) : h;
        if (tw == g_stream_w && th == g_stream_h) { return; }
        logf_("shared: TEST %s %ux%u -> %ux%u\n", s_small ? "shrink" : "restore",
              g_stream_w, g_stream_h, tw, th);
        update_content_size(device, tw, th);
        return;
    }

    UINT w = 0, h = 0;
    stream_target_size(&w, &h);
    if (w == 0 || h == 0 || (w == g_stream_w && h == g_stream_h)) { return; }
    if (w > mgpu::kStreamMaxW || h > mgpu::kStreamMaxH) {
        if (!g_logged_size_mismatch) {
            g_logged_size_mismatch = true;
            logf_("shared: %ux%u is above the %ux%u reserve - keeping the %ux%u layout"
                  " (the pass rescales)\n",
                  w, h, mgpu::kStreamMaxW, mgpu::kStreamMaxH, g_stream_w, g_stream_h);
        }
        return;
    }
    update_content_size(device, w, h);
}

