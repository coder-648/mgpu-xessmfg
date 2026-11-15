// ============================================================================
//  lock_frame module: 110_pair_dump.inc
//
//  WGC timestamp/slot pairing, --dump recording, colour probe.
//
//  Included by lock_frame/lock_frame.cpp as part of the single-translation-unit build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

static double qpc_ms()
{
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// frame_key pairing: WGC hands over colour after composition, so its
// SystemRelativeTime is normally a little *after* the producer's evaluate timestamp.
// Pick the newest producer slot whose frame_qpc_ms is at or just before that stamp;
// fall back to the old round-robin when no slot is in the window (old add-on, a
// stalled producer, or a clock that is not on the same QPC base).
static UINT pair_slot_for_wgc(double wgc_ms, UINT slots)
{
    if (!g_pair_frame_key || !g_ctl || wgc_ms <= 0.0) { return mgpu::kMaxSlots; }
    UINT best = mgpu::kMaxSlots;
    double best_dt = 1.0e9;
    for (UINT s = 0; s < slots && s < mgpu::kMaxSlots; ++s) {
        const uint64_t t = g_ctl->frames[s].frame_qpc_ms;
        if (t == 0) { continue; }
        const double dt = wgc_ms - static_cast<double>(t);
        if (dt < -2.0 || dt > 250.0) { continue; }
        if (dt < best_dt) { best_dt = dt; best = s; }
    }
    return best;
}

// ====================== --dump: raw frame export for analysis ================
// Writes the exact colour / depth / MV resources we hand to XeSS-FG to disk so
// the pairing question ("does this depth/MV belong to the same game frame as
// this colour?") can be answered offline instead of by eye.
//
//     <dir>\f<NN>_colour.raw   RGBA16F, the captured (WGC) colour
//     <dir>\f<NN>_depth.raw    R32_FLOAT, the depth we tag
//     <dir>\f<NN>_mv.raw       R16G16_FLOAT, the MV we tag
//     <dir>\f<NN>_meta.txt     sizes/format/slot/frame_id/jitter/...
//
// Header of every .raw = 4 x uint32 { width, height, dxgiFormat, rowPitchBytes }
// then the rows at their natural pitch (copy padding stripped), so the file is
// exactly width*height*bytesPerPixel + 16 bytes.
//
// The dump waits for the GPU, so the frames it writes have distorted timings -
// that is intentional, only the data matters.
static int  g_dump_frames = 0;                              // --dump N
static int  g_dump_index  = 0;
static int  g_dump_after  = 60;    // --dump-after N: skip the settle-in frames
static char g_dump_dir[MAX_PATH] = "dump";   // --dump-dir DIR (relative to the CWD by default)

// One stream's readback target. The copy is recorded inside the frame's own command
// list (so it is ordered with the MV/depth copies that feed XeSS-FG) and only
// mapped once the whole dump is complete. The first version waited for the GPU
// inside the frame loop instead, which put five game frames between consecutive
// dumped frames - far too coarse to answer "one frame ahead or one behind".
struct DumpStream
{
    ComPtr<ID3D12Resource> rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT   rows = 0;
    UINT64 row_size = 0;
    D3D12_RESOURCE_DESC desc{};
};

struct DumpEntry
{
    DumpStream s[3];                 // 0 = colour, 1 = depth, 2 = mv
    bool have[3] = { false, false, false };   // which streams had a resource
    bool recorded = false;
    // The shared per-frame record changes every frame, so snapshot it now.
    uint64_t bridge_frame = 0, present_id = 0, tick_ms = 0, producer_frame_id = 0;
    uint32_t slot = 0, render_w = 0, render_h = 0;
    float    jitter_x = 0.0f, jitter_y = 0.0f, mv_scale_x = 0.0f, mv_scale_y = 0.0f;
    uint32_t stream_mv_w = 0, stream_mv_h = 0, stream_depth_w = 0, stream_depth_h = 0;
    float    fov = 0.0f;
    uint32_t no_mv = 0, no_depth = 0, inputs_on = 1, proj_rz = 0;
};

static std::vector<DumpEntry> g_dump_entries;
static bool g_dump_flushed = false;

static bool dump_prepare(DumpStream& s, ID3D12Resource* res, int which)
{
    static bool warned[3] = { false, false, false };
    auto bail = [&](const char* why) {
        if (!warned[which & 3]) {
            warned[which & 3] = true;
            logf_("[lock] dump: stream %d skipped (%s)\n", which, why);
        }
        return false;
    };
    if (!res) { return bail("no resource this frame"); }
    s.desc = res->GetDesc();
    if (s.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) { return bail("not a texture2D"); }
    UINT64 total = 0;
    g_device->GetCopyableFootprints(&s.desc, 0, 1, 0, &s.fp, &s.rows, &s.row_size, &total);
    if (total == 0 || s.rows == 0) { return bail("GetCopyableFootprints returned nothing"); }

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    const HRESULT hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&s.rb));
    if (FAILED(hr)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "readback buffer failed hr=0x%08lX (%.1f MB)",
                 static_cast<unsigned long>(hr), static_cast<double>(total) / (1024.0 * 1024.0));
        return bail(msg);
    }
    return true;
}

// Records the three readback copies for this frame into the caller's command list.
static void dump_record(ID3D12GraphicsCommandList* list, UINT slot, UINT64 present_id)
{
    if (g_dump_frames <= 0 || g_dump_index >= g_dump_frames || g_dump_flushed) { return; }
    // Loading screens and menus come first; only export settled gameplay frames.
    if (g_frames < static_cast<UINT64>(g_dump_after)) { return; }
    if (g_dump_entries.empty()) { g_dump_entries.resize(static_cast<size_t>(g_dump_frames)); }

    DumpEntry& e = g_dump_entries[static_cast<size_t>(g_dump_index)];
    // Depth normally lives in the local RING (the newest entry is what was handed to XeFG),
    // and g_depth_local[slot] is only used as a fallback when the ring could not be built -
    // reading the wrong one made every dump report depth=0 even with a healthy depth stream.
    ID3D12Resource* depth_res = nullptr;
    if (g_ring_count > 0 && g_depth_ring[g_ring_write]) {
        depth_res = g_depth_ring[g_ring_write].Get();
    } else if (slot < mgpu::kMaxSlots && g_depth_local[slot]) {
        depth_res = g_depth_local[slot].Get();
    }
    ID3D12Resource* res[3] = { g_shared12.Get(),
                               depth_res,
                               (slot < mgpu::kMaxSlots) ? g_mv_local[slot].Get() : nullptr };

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    for (int i = 0; i < 3; ++i) {
        // A missing stream (XeSS publishes no depth, --no-depth, --no-mv before the dummy
        // exists) must not throw the whole frame away - that is why `--dump` used to leave
        // an empty directory with no explanation at all.
        if (!dump_prepare(e.s[i], res[i], i)) { continue; }
        e.have[i] = true;
        b.Transition.pResource = res[i];
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = res[i];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = e.s[i].rb.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = e.s[i].fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &b);
    }

    const mgpu::FrameShared& fr = g_ctl->frames[slot];
    e.recorded = (e.have[0] || e.have[1] || e.have[2]);   // at least one usable stream
    e.bridge_frame = g_frames;
    e.present_id = present_id;
    e.tick_ms = GetTickCount64();
    e.slot = slot;
    e.producer_frame_id = fr.frame_id;
    e.render_w = fr.render_w;
    e.render_h = fr.render_h;
    e.jitter_x = fr.jitter_x;
    e.jitter_y = fr.jitter_y;
    e.mv_scale_x = fr.mv_scale_x;
    e.mv_scale_y = fr.mv_scale_y;
    e.stream_mv_w = g_ctl->streams[1].width;
    e.stream_mv_h = g_ctl->streams[1].height;
    e.stream_depth_w = g_ctl->streams[2].width;
    e.stream_depth_h = g_ctl->streams[2].height;
    e.fov = g_fg_fov_y;
    e.no_mv = g_no_mv ? 1 : 0;
    e.no_depth = g_no_depth ? 1 : 0;
    e.inputs_on = g_no_inputs ? 0 : 1;
    e.proj_rz = g_fg_proj_rz ? 1 : 0;
    ++g_dump_index;
}

static void run_frame_ex(ID3D11Texture2D* colour11);

// ---- --probe-colour N: is the captured picture black? -----------------------
// Copies a 64x36 window of the shared colour texture (what the present path is about to
// draw) into a readback buffer and counts non-zero samples. This is the one measurement
// that separates "the capture is black" from "the presentation is black": every other
// counter in this program (SUCCESS, show, gpu) stays healthy in both cases.
static int  g_probe_colour = 0;          // --probe-colour N (0 = off)
static ComPtr<ID3D12Resource>            g_probe_rb;
static ComPtr<ID3D12CommandAllocator>    g_probe_alloc;
static ComPtr<ID3D12GraphicsCommandList> g_probe_list;
static ComPtr<ID3D12Fence>               g_probe_fence;
static UINT64                            g_probe_value = 0;

static void probe_colour_black()
{
    if (g_probe_colour <= 0 || !g_shared12 || !g_queue) { return; }
    // --fg-colour shared: the sink is filled by the *previous* frame's cross-adapter
    // copy, so probing frame 0 samples an untouched (black) texture and reports a
    // BLACK CAPTURE that says nothing. Give the pump a couple of windows first.
    if (g_frames < static_cast<UINT64>(g_probe_colour)) { return; }
    if ((g_frames % static_cast<UINT64>(g_probe_colour)) != 0) { return; }

    const UINT W = 64, H = 36;                 // 64 * 8 B = 512 B/row: 256-aligned
    const UINT rowpitch = W * 8;
    if (!g_probe_rb) {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = static_cast<UINT64>(rowpitch) * H;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_probe_rb)))) {
            logf_("[lock] probe: readback buffer FAILED - probe disabled\n");
            g_probe_colour = 0; return;
        }
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&g_probe_alloc))) ||
            FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               g_probe_alloc.Get(), nullptr,
                                               IID_PPV_ARGS(&g_probe_list))) ||
            FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                         IID_PPV_ARGS(&g_probe_fence)))) {
            logf_("[lock] probe: command objects FAILED - probe disabled\n");
            g_probe_colour = 0; return;
        }
        g_probe_list->Close();
    }

    const D3D12_RESOURCE_DESC cd = g_shared12->GetDesc();
    if (cd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) { g_probe_colour = 0; return; }
    D3D12_BOX box{};
    box.right  = (W < static_cast<UINT>(cd.Width))  ? W : static_cast<UINT>(cd.Width);
    box.bottom = (H < cd.Height) ? H : cd.Height;
    box.back = 1;

    g_probe_alloc->Reset();
    g_probe_list->Reset(g_probe_alloc.Get(), nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = g_shared12.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_probe_list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g_shared12.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_probe_rb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = cd.Format;
    dst.PlacedFootprint.Footprint.Width = box.right;
    dst.PlacedFootprint.Footprint.Height = box.bottom;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = rowpitch;
    g_probe_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_probe_list->ResourceBarrier(1, &b);
    g_probe_list->Close();
    ID3D12CommandList* lists[] = { g_probe_list.Get() };
    g_queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++g_probe_value;
    if (FAILED(g_queue->Signal(g_probe_fence.Get(), v))) { return; }
    wait_fence_value(g_probe_fence.Get(), v, 200);

    void* mapped = nullptr;
    if (FAILED(g_probe_rb->Map(0, nullptr, &mapped)) || !mapped) { return; }
    const uint8_t* p = static_cast<const uint8_t*>(mapped);
    UINT nonzero = 0, samples = 0;
    for (UINT y = 0; y < box.bottom; ++y) {
        const uint8_t* row = p + static_cast<size_t>(y) * rowpitch;
        for (UINT x = 0; x < box.right; ++x) {
            const uint8_t* px = row + static_cast<size_t>(x) * 8;   // R16G16B16A16
            ++samples;
            if (px[0] | px[1] | px[2] | px[3] | px[4] | px[5] | px[6] | px[7]) { ++nonzero; }
        }
    }
    g_probe_rb->Unmap(0, nullptr);
    logf_("[lock] probe: captured colour %ux%u fmt %u -> %u/%u samples non-zero (%.1f%%)"
          " %s\n", static_cast<unsigned>(cd.Width), cd.Height,
          static_cast<unsigned>(cd.Format), nonzero, samples,
          100.0 * nonzero / (samples ? samples : 1),
          (nonzero == 0) ? "<== BLACK CAPTURE" : "ok");
}

