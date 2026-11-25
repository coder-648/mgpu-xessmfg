// ============================================================================
//  lock_frame module: 060_frame_io.inc
//
//  Output statistics, fences, publication queue, stream geometry, local
//  copies, dummy inputs.
//
//  Included by lock_frame/lock_frame.cpp as part of the single-translation-unit build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

// ------------------------------ output statistics ----------------------------
// The drawn-in FPS counter (seven-segment digits plus the dark plate behind them)
// has been removed: nothing is drawn on top of the picture any more. g_show_fps
// stays because the single-line OSD and the periodic log still report the rate.
static int  g_show_fps = 0;

// The colour sink has to match whatever fills it. WGC delivers scRGB FP16; with
// --fg-colour shared the add-on forwards the game's back buffer verbatim (HDR10 PQ in
// R10G10B10A2), and a D3D12 copy cannot change formats.
static DXGI_FORMAT colour_sink_format()
{
    return g_fg_colour_shared ? DXGI_FORMAT_R10G10B10A2_UNORM
                              : DXGI_FORMAT_R16G16B16A16_FLOAT;
}

static bool create_colour_sink(UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width  = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = colour_sink_format();
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // NTHANDLE is only legal together with KEYEDMUTEX (D3D11 requirement).
    // D3D12 cannot take a keyed mutex, so only this side acquires/releases it
    // and the D3D12 side is ordered by the shared fence instead.
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    const HRESULT ch = g_d3d11->CreateTexture2D(&d, nullptr, &g_shared11);
    if (FAILED(ch)) {
        logf_("[lock] colour sink CreateTexture2D FAILED hr=0x%08lX\n",
              static_cast<unsigned long>(ch));
        return false;
    }
    if (FAILED(g_shared11.As(&g_colour_mutex))) {
        logf_("[lock] colour sink keyed mutex unavailable\n");
        return false;
    }
    ComPtr<IDXGIResource1> dxgi_res;
    if (FAILED(g_shared11.As(&dxgi_res))) { return false; }
    HANDLE sh = nullptr;
    if (FAILED(dxgi_res->CreateSharedHandle(nullptr, GENERIC_ALL, L"mgpu_colour_sink", &sh))) {
        logf_("[lock] colour sink CreateSharedHandle FAILED\n");
        return false;
    }
    const HRESULT hr = g_device->OpenSharedHandle(sh, IID_PPV_ARGS(&g_shared12));
    CloseHandle(sh);
    if (FAILED(hr)) { logf_("[lock] colour sink OpenSharedHandle FAILED\n"); return false; }

    logf_("[lock] colour sink %ux%u fmt %u (%s) shared to D3D12\n", w, h,
          static_cast<unsigned>(colour_sink_format()),
          g_fg_colour_shared ? "HDR10 PQ, straight from the add-on" : "scRGB FP16, from WGC");
    return true;
}


// ------------------------------------------------------------ frame loop

static UINT64 g_fence11_value = 0;
static UINT64 g_present_id = 0;
static UINT   g_mv_slot = 0;
static UINT   g_slot_timeouts = 0;
static UINT64 g_frames = 0;
// Each back buffer carries its own state: with FLIP_DISCARD and BufferCount=2 the
// presented buffer alternates, and writing only buffer 0 left the other one stale
// - which is what flickered on screen.
static D3D12_RESOURCE_STATES g_bb_state[4] = {
    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
};
static double g_t0 = 0.0;
static ULONGLONG g_t_report = 0;   // start of the current 40-frame report window
// How long Present() blocked, and the measured frame interval handed to XeSS-FG as
// frameRenderTime (a hardcoded value misleads its pacing).
static DWORD g_present_ms = 0;
// Stage breakdown, all in milliseconds, exponential moving average.
static double g_ms_capture = 0.0;   // waiting for the next WGC frame
static double g_ms_work = 0.0;      // our CPU work up to submitting the command list
static double g_ms_present = 0.0;   // Present() blocking
static double g_ms_throttle = 0.0;  // waiting for the GPU to finish the previous frame
static double g_frame_ms_ema = 25.0;      // interval between our run_frame() calls
static double g_ms_wait = 0.0;            // waiting for the producer's MV/depth slot
static double g_max_wait = 0.0;           // worst such wait in this report window
static double g_ms_copywait = 0.0;        // waiting for the 118 MB colour copy
static double g_max_copywait = 0.0;
// The D3D11 side of the same hop (keyed-mutex acquire + CopyResource). Separate from
// `copy` because they measure different halves: `colour` is CPU + this card's copy
// submission, `copy` is the D3D12 queue waiting for it to land.
static double g_ms_colour = 0.0;
static double g_max_colour = 0.0;
static double g_ms_total = 0.0;           // whole run_frame() duration
static double g_max_total = 0.0;
static double g_ms_inputs = 0.0;          // recording the cross-adapter MV/depth copies
static double g_max_inputs = 0.0;
static double g_ms_xell = 0.0;            // xellSleep(): XeLL's own latency pacing delay
static double g_max_xell = 0.0;
static double g_ms_age = 0.0;             // age of the WGC frame when we receive it
static double g_max_age = 0.0;
static double g_ms_fg = 0.0;              // xefgSwapChain tagging calls
static double g_max_fg = 0.0;
static DWORD  g_wait_ms = 20;             // --wait-ms N: timeout for that wait
static ULONGLONG g_last_arrival = 0;
static double g_presented_sum = 0.0;      // frames XeSS-FG emitted per input frame
static UINT   g_presented_n   = 0;
// Arrival jitter diagnostics: how irregular the game's frames reach us, and how many gaps
// were longer than 1.6x the mean interval. NOT a frame-drop counter: with a 40 fps lock on
// a 120 Hz panel the intervals alternate 16.7/33.3/41.7 ms, and every 41.7 ms gap counts -
// so the number tracks the beat pattern, not lost frames (renamed to arr_gaps in the log
// because "wgc_drops" read as if frames were being dropped).
static double g_arr_min = 1e9, g_arr_max = 0.0;
static double g_arr_prev = 0.0;           // previous WGC timestamp in ms
// The last WGC SystemRelativeTime, in QPC milliseconds. frame_key/frame_qpc_ms in
// FrameShared is on the same clock, so this is what lets the bridge pick the producer
// slot that actually matches the late colour instead of walking slots blindly.
static double g_wgc_ts_ms = 0.0;
static bool   g_pair_frame_key = true;    // --no-pair-key disables
static UINT   g_arr_drops = 0;
static UINT   g_slow_frames = 0;   // frames whose GPU work did not fit the input interval
// Worst-case tracking inside a report window: averages hide the one-frame hitches
// that are exactly what the eye notices.
static double g_max_gpu = 0.0, g_max_present = 0.0, g_max_gap = 0.0;
static double g_last_present_qpc = 0.0;
static UINT   g_x2_miss = 0;       // presents where XeSS-FG emitted no interpolated frame
static UINT   g_hitch_logged = 0;  // hitches reported this window (rate limit)
// One-shot diagnostics: the present-size check only makes sense once the window is at
// its final size, and the "present is the bottleneck" note is worth saying once.
static bool   g_present_size_checked = false;
static bool   g_present_cap_warned = false;
static bool   g_diag_dumped = false;    // producer state printed once (wait_slot)
static UINT   g_report_every = 40; // --report N: frames per statistics window
static UINT   g_arr_count = 0;
static double  g_last_frame_tick = 0.0;   // qpc_ms() of the previous run_frame_ex (QPC, not GetTickCount64)

// Bucketed arrival histogram, in vblanks, reset every report window. This is the direct
// answer to "are we losing input frames, or is the game simply not delivering them?":
// a locked 40 fps game on a 120 Hz panel must land in bucket 3 (25.0 ms) almost every
// time; anything in bucket 4+ is a missing/gated frame, and a spread over 2/3/4 means the
// presenter itself is not holding its cadence.
static UINT   g_arr_hist[9] = {};         // index = vblanks rounded, 8 = "8 or more"
// The same bucketing for the interval the *producer* measured between the game's own two
// Present calls (FrameShared.frame_time_ms, published as a raw value - not an EMA).
// Comparing this with the composition-side histogram splits "the game wobbles" from
// "DWM / our own overlay coalesces our captures":
//   game_arr and arr both spread  -> the game's own presents are uneven
//   game_arr flat, arr spread     -> the frames leave the game evenly and are lost after
//                                    that (composition, our overlay, capture pool)
static UINT   g_game_hist[9] = {};
// The same median taken from two different clocks. `dwm` uses the capture frame's
// SystemRelativeTime (what the compositor stamped); `wc` uses the wall clock at the moment
// we dequeued it. They should agree. When they do not, the difference is our own pickup
// jitter, not lost input - which is exactly the mistake `input=` used to make.
static double g_arr_wc_ring[kArrRing] = {};
static UINT   g_arr_wc_n = 0;
static UINT   g_arr_wc_i = 0;

static double arr_wc_median_ms()
{
    if (g_arr_wc_n == 0) { return 0.0; }
    double v[kArrRing];
    const UINT n = g_arr_wc_n;
    for (UINT i = 0; i < n; ++i) { v[i] = g_arr_wc_ring[i]; }
    for (UINT i = 1; i < n; ++i) {
        const double k = v[i];
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; --j; }
        v[j + 1] = k;
    }
    return v[n / 2];
}

static double qpc_ms();   // defined next to the pacer below

// Pacing state. Declared here because run_frame_warp needs g_pace_ms for its own
// present timing (interpolated frame and real frame one half tick apart).
static bool   g_pace_auto = false;      // --pace auto: follow the measured source rate
static double g_pace_ms = 0.0;          // 0 = off
static double g_pace_next = 0.0;        // next tick in QPC milliseconds
static double g_represents = 0.0;       // how often we had to re-send the last frame
static UINT   g_pace_skips = 0;         // extra frames that arrived inside one tick
static bool   g_have_last = false;
static UINT   g_last_slot = 0;
static bool   g_reuse_frame = false;
static bool   g_no_throttle = false;    // --no-throttle: do not wait for the previous frame

// Windows defaults to a 15.6 ms timer, which makes any Sleep()-based pacing overshoot
// the tick by up to that much - the periodic 40 ms gap in a 25 ms cadence. Raising the
// resolution is done through winmm so the build needs no extra import library.
static void enable_fine_timer()
{
    HMODULE wm = LoadLibraryA("winmm.dll");
    if (!wm) { return; }
    using pfn_time_begin = UINT(WINAPI*)(UINT);
    auto begin = reinterpret_cast<pfn_time_begin>(GetProcAddress(wm, "timeBeginPeriod"));
    if (begin && begin(1) == 0) {
        logf_("[lock] timer resolution raised to 1ms\n");
    }
}

// Cross-adapter fences do not report a value on the consuming node: GetCompletedValue()
// returns UINT64_MAX here (PBZ showed exactly that), which used to poison g_fwd_last and
// make every later wait ask for value 0. Track our own expectation instead - the producer
// signals 1, 2, 3, ... per slot, and SetEventOnCompletion fires immediately when the fence
// is already past the value we ask for.
// Every slot expects the producer's first signal (value 1). Only initialising the first
// four was fine while kMaxSlots was 4; with 16 slots 4..15 would start at 0 and their
// first wait would return immediately without waiting for the publication.
static UINT64 g_fwd_expect[mgpu::kMaxSlots] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
};

// ---- waiting for a *local* fence without allocating a kernel object every frame ------
// gpu_throttle() runs once per real frame, so CreateEventW + CloseHandle there was 60-120
// object creations per second for no reason. One auto-reset event is reused forever; that
// is safe only because every wait here is on a fence whose GetCompletedValue() is
// meaningful, which lets the loop tell "the event fired" from "the event was still set
// from an earlier timeout":
//   * the fence value is re-checked after every wait, so a late signal can cost one extra
//     iteration but can never make us skip a wait that has not retired yet;
//   * the remaining timeout is recomputed each round, so a spurious wakeup does not turn
//     into a shorter timeout than the caller asked for.
// Everything calling this runs on the frame thread, so one event is shared.
//
// wait_slot() deliberately does NOT use it: its fences are SHARED_CROSS_ADAPTER, where
// GetCompletedValue() reads back UINT64_MAX on this node, so the check that makes the
// reused event safe is unavailable - and a stale signal there would advance
// g_fwd_expect[] past a publication we never saw, i.e. read one slot too old.
static bool wait_fence_value(ID3D12Fence* fence, UINT64 value, DWORD timeout_ms)
{
    if (!fence) { return false; }
    if (fence->GetCompletedValue() >= value) { return true; }
    static HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) {
        // No event: bounded spin. Should never happen, but never wait forever either.
        const ULONGLONG t0 = GetTickCount64();
        while (fence->GetCompletedValue() < value) {
            if (GetTickCount64() - t0 >= timeout_ms) { return false; }
            SwitchToThread();
        }
        return true;
    }
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        if (FAILED(fence->SetEventOnCompletion(value, ev))) { return false; }
        const ULONGLONG spent = GetTickCount64() - t0;
        if (spent >= timeout_ms) { return fence->GetCompletedValue() >= value; }
        WaitForSingleObject(ev, static_cast<DWORD>(timeout_ms - spent));
        if (fence->GetCompletedValue() >= value) { return true; }
        if (GetTickCount64() - t0 >= timeout_ms) { return false; }
    }
}

// Slot selection lives further down (round-robin, see run_frame_ex step 3). Protocol v5
// here used to add a "is this slot GPU-complete?" query on frames[].ready plus
// --mv-newest; both are gone, because publishing `ready` requires polling a
// SHARED_CROSS_ADAPTER fence and four GetCompletedValue() calls per present dragged the
// game from 33 to 60 ms (docs/findings/HANDOFF.md 8.6). The field is still in mgpu_shared.h so the
// add-on/bridge layouts stay identical - nothing reads it.
//
// --depth-lag N: pair the colour with a depth N frames older, applied on OUR side from a
// local ring. The WGC colour is 10-70 ms old (~0.3-2 frames at 30 fps), so the depth that
// belongs to it is usually one publication older - but measured worse anyway, hence 0.
static int g_depth_lag = 0;
// --exit-on-focus-loss N: quit when another application has been in the foreground
// for N seconds (default 5, 0 = never). Without this the topmost overlay survives a
// crashed/closed game and just sits on the desktop forever.
static UINT g_exit_after_s = 5;
// --run-sec N: quit after N seconds of running. Measurement runs need a reproducible
// length (30 s per segment) and an automatic exit, so the next segment can start.
static UINT g_run_sec = 0;
static ULONGLONG g_fg_lost_ms = 0;

static void wait_slot(UINT slot)
{
    if (slot >= mgpu::kMaxSlots || !g_fwd[slot]) { return; }
    {
        // One event per call, on purpose: this fence is SHARED_CROSS_ADAPTER, so the
        // GetCompletedValue() re-check that makes a reused event safe is not available
        // here (it reads UINT64_MAX on this node) - see wait_fence_value(). A stale
        // signal would advance g_fwd_expect[] without a publication and make us read a
        // slot one frame too old, which is worse than the ~1 us this costs.
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_fwd[slot]->SetEventOnCompletion(g_fwd_expect[slot], ev);
        // Blocking here shows up as a gap between presents that nothing else explains,
        // so the wait is measured. A timeout just means the producer has not published
        // yet and the previous contents are used.
        const double w0 = qpc_ms();
        const DWORD wr = WaitForSingleObject(ev, g_wait_ms);
        const double w1 = qpc_ms();
        g_ms_wait = g_ms_wait * 0.9 + (w1 - w0) * 0.1;
        if (w1 - w0 > g_max_wait) { g_max_wait = w1 - w0; }
        if (wr == WAIT_OBJECT_0) { ++g_fwd_expect[slot]; }
        else { ++g_slot_timeouts; }
        CloseHandle(ev);
    }
    if (!g_diag_dumped && g_ctl) {
        // One-shot: is the producer publishing at all? If the fences stay at 0 and
        // frame_published never moves, the add-on is not running its present path
        // (no ReShade technique, or its reshade_present callback never fires) and every
        // slot wait is just burning the --wait-ms timeout.
        g_diag_dumped = true;
        logf_("[lock] diag: frame_published=%llu  fwd=[%llu %llu %llu %llu]"
              "  frames[0]=(id %llu, render %ux%u, mvscale %.6f, %.6f)\n",
              static_cast<unsigned long long>(g_ctl->frame_published),
              static_cast<unsigned long long>(g_fwd[0] ? g_fwd[0]->GetCompletedValue() : 0),
              static_cast<unsigned long long>(g_fwd[1] ? g_fwd[1]->GetCompletedValue() : 0),
              static_cast<unsigned long long>(g_fwd[2] ? g_fwd[2]->GetCompletedValue() : 0),
              static_cast<unsigned long long>(g_fwd[3] ? g_fwd[3]->GetCompletedValue() : 0),
              static_cast<unsigned long long>(g_ctl->frames[0].frame_id),
              g_ctl->frames[0].render_w, g_ctl->frames[0].render_h,
              g_ctl->frames[0].mv_scale_x, g_ctl->frames[0].mv_scale_y);
    }
}

// Local (committed, DEFAULT heap) copies of the producer's MV/depth. XeSS-FG
// cannot work with the imported cross-adapter heap resources: it fails with
// "Failed to allocate memory for a copy of DEPTH" and then reports the motion
// vector resource as missing.
static ComPtr<ID3D12Resource> g_mv_local[mgpu::kMaxSlots];
static ComPtr<ID3D12Resource> g_ui_local[mgpu::kMaxSlots];
static bool                   g_ui_fresh[mgpu::kMaxSlots] = {};

// Local UI decode targets are indexed by UI PUBLICATION slot, not by MV/depth slot.
// The producer's UI ring is smaller (UiSlots, 8 by default: the 4K full-res UI is ~34 MB
// per slot) and `decode_ui_slot()` always reads codec slot `slot % UiSlots`, so one local
// texture per UI slot is enough. At 4K that is 8 x 33 MB instead of 32 x 33 MB
// (1.06 GB -> 265 MB). The decode -> unblend -> tag window is a single frame and
// gpu_throttle() keeps the queue at depth 1, so two MV slots that map to the same UI slot
// can never be in flight at the same time. With an older producer (slots == 32) this is
// the identity, i.e. exactly the old behaviour.
static UINT ui_slot_index(UINT slot)
{
    const UINT n = (g_ui_slots != 0) ? g_ui_slots : mgpu::kMaxSlots;
    return (n != 0) ? (slot % n) : slot;
}
static ComPtr<ID3D12Resource> g_depth_local[mgpu::kMaxSlots];
// --depth-lag N: our own ring of local depth copies. The shared ring must always be read
// at its newest completed slot (reading an older one collides with the producer's write),
// so the "depth from N frames ago" alignment is done here instead.
static ComPtr<ID3D12Resource> g_depth_ring[mgpu::kMaxSlots];
static UINT g_ring_write = 0;
static UINT g_ring_count = 0;
static UINT g_ring_need  = 1;

// Copy-on-publication (2026-11-14): every new producer publication is copied into a
// per-slot local cache, so the WGC lookup is a hit regardless of which slot the timestamp
// picks. `local_pub[s]` is the publication the local copy holds; the frame waits on
// `g_copy_fence` at `local_fence[s]` before tagging. The poll runs in the capture loop,
// which is idle (Sleep 1 ms) while the producer publishes, so no extra thread is needed.
static bool     g_pf_pub = false;           // --prefetch-publications / --no-prefetch-publications
                                            // Opt-in until the 100% cache is validated on hardware:
                                            // the first 4K A run showed 3-9/40 hits and the OF every-frame
                                            // fallback cost ~2-3 ms on the render card. Default is the
                                            // one-slot rotating prefetch (g_prefetch_inputs) + in-frame copy.
static bool     g_of_miss_fallback = false; // --of-miss-fallback: on a prefetch miss run the FX flow
                                            // every frame instead of copying. 4K cost, opt-in.

// --pub-fifo (2026-11-14): the WGC colour is slower than the copy (age 10.6 ms vs 3.6 ms),
// so the natural model is a queue: every publication that has been copied locally is pushed
// in publication order, and each WGC frame pops the oldest entry. No slot guessing, no
// `local_pub == published` comparison, no miss. The queue depth is capped at kMaxSlots-1 so
// the producer cannot reuse a slot whose local copy is still queued; an overflow drops the
// oldest entry (that colour frame was skipped) and is counted.
struct PubQueueEntry {
    uint64_t pub;
    uint64_t fence;
    uint32_t slot;
    uint32_t gen;
};
static PubQueueEntry g_pub_q[mgpu::kMaxSlots] = {};
static UINT          g_pub_q_head = 0;
static UINT          g_pub_q_n = 0;
static bool          g_pub_fifo = false;    // --pub-fifo (implies --prefetch-publications)
static UINT          g_fifo_drops = 0;

static void pub_q_reset()
{
    g_pub_q_head = 0;
    g_pub_q_n = 0;
}

// The producer rotates over kMaxSlots textures, so a new publication for slot S overwrites
// the local copy that an older queued entry for S points at. Drop those entries first: the
// colour frame that would have consumed them was skipped. That is the only overflow.
static void pub_q_push(uint64_t pub, UINT slot, uint64_t fence, uint32_t gen)
{
    const UINT cap = mgpu::kMaxSlots;
    UINT m = 0;
    for (UINT i = 0; i < g_pub_q_n; ++i) {
        const PubQueueEntry& e = g_pub_q[(g_pub_q_head + i) % cap];
        if (e.slot == slot) { ++g_fifo_drops; continue; }
        g_pub_q[m++] = e;
    }
    if (m == cap) {                             // safety: never more than the ring size
        for (UINT i = 1; i < m; ++i) { g_pub_q[i - 1] = g_pub_q[i]; }
        --m;
        ++g_fifo_drops;
    }
    g_pub_q_head = 0;
    g_pub_q_n = m;
    if (g_pub_q_n != 0 && pub <= g_pub_q[g_pub_q_n - 1].pub) { return; }
    PubQueueEntry& e = g_pub_q[g_pub_q_n];
    e.pub = pub; e.fence = fence; e.slot = slot; e.gen = gen;
    ++g_pub_q_n;
}

static bool pub_q_pop(PubQueueEntry* out, uint32_t gen)
{
    while (g_pub_q_n != 0) {
        const PubQueueEntry e = g_pub_q[0];
        for (UINT i = 1; i < g_pub_q_n; ++i) { g_pub_q[i - 1] = g_pub_q[i]; }
        --g_pub_q_n;
        if (e.gen == gen) {
            if (out) { *out = e; }
            return true;
        }
    }
    return false;
}
static uint64_t g_local_pub[mgpu::kMaxSlots] = {};
static uint64_t g_local_fence[mgpu::kMaxSlots] = {};

// --upsample: consumer-side joint MV/depth reconstruction (docs/design/UPSAMPLE_QUALITY.md).
// The stream is transferred at StreamPercent size and rebuilt to the game's render size
// here. XeFG requires MV and depth to be the *same* size, so both are rebuilt by the same
// pass and tagged together; the switch is all-or-nothing by construction.
static bool  g_upsample = false;
static float g_upsample_k = 200.0f;         // --upsample-k: depth-similarity sharpeness
static ComPtr<ID3D12RootSignature>  g_up_rs;
static ComPtr<ID3D12PipelineState>  g_up_pso;
static ComPtr<ID3D12DescriptorHeap> g_up_heap;
static ComPtr<ID3D12Resource>       g_up_mv;
static ComPtr<ID3D12Resource>       g_up_depth;
static D3D12_RESOURCE_STATES        g_up_mv_state    = D3D12_RESOURCE_STATE_COMMON;
static D3D12_RESOURCE_STATES        g_up_depth_state = D3D12_RESOURCE_STATE_COMMON;
static UINT  g_up_w = 0, g_up_h = 0;
static UINT  g_up_slot = mgpu::kMaxSlots;
static bool  g_up_valid = false;
// The publication the current g_up_* pair was built from. A slot number alone is not a
// frame identity: the ring reuses the same slot a few frames later, and the rebuild pass
// may legitimately skip a frame (nothing to gain / --depth-lag / prefetch). Without this
// key the tag could pick up the pair of an older publication that happened to use the
// same slot (see docs/SELF_AUDIT_2026-11-25.md C).
static uint64_t g_up_pub = 0;
static bool  g_up_told = false;

// --stream-auto / --stream-asym (protocol v15). The consumer writes per-stream percent
// requests; the producer applies them on its next record and echoes `active_percent_*`.
// Auto is a slow controller (one decision per report window + hysteresis) because every
// change invalidates the 4 published frames and rebuilds the consumer's local copies.
// Asymmetric means MV 1:1 and depth downscaled; it needs --upsample so the tagged pair
// ends up the same size (XeFG requirement).
static bool      g_stream_auto = false;
static bool      g_stream_asym = false;
static UINT      g_auto_percent = 100;
static ULONGLONG g_auto_last_change = 0;      // GetTickCount64 of the last applied step
static UINT      g_auto_down = 0, g_auto_up = 0;
static double    g_age_ring[64] = {};
static unsigned  g_age_n = 0, g_age_i = 0;

// Local (consumer-side) copy of one input stream. Sized to the *content* rectangle, not
// to the slot capacity: XeFG is tagged with `resourceSize = w x h` and samples the whole
// texture, so handing it a padded 3840x2160 buffer would make it read the padding.
// Recreated when the producer publishes a new content size (protocol v9 - no handshake
// needed, the old texture simply stops being referenced).
// Live content size published by the producer (protocol v9): how much of each slot is
// valid. Snapshotted once per frame so MV and depth cannot disagree within one frame if
// the producer publishes a new rectangle mid-frame.
struct InputGeo { UINT mv_w = 0, mv_h = 0, depth_w = 0, depth_h = 0; };
static InputGeo g_geo;

// v10: read one stream's (width,height) through its seqlock. Without it a reader could
// take the new width and the old height (or vice versa) - which is what the old code
// papered over with a clamp.
static bool read_stream_geo(const mgpu::StreamShared& st, UINT* w, UINT* h)
{
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t s0 = st.seq;
        if (s0 & 1u) { continue; }                 // producer is mid-write
        MemoryBarrier();
        const UINT ww = st.width, hh = st.height;
        MemoryBarrier();
        if (st.seq == s0) { *w = ww; *h = hh; return true; }
    }
    return false;
}

static void refresh_input_geo()
{
    if (!g_ctl) { return; }
    // --no-upsample is the explicit opt-out from the reconstruction pass (an A/B for the
    // ported MFG model). Read once: the CLI does not change while we run.
    static int s_no_upsample = -1;
    if (s_no_upsample < 0) {
        s_no_upsample = (wcsstr(GetCommandLineW(), L"--no-upsample") != nullptr) ? 1 : 0;
    }
    // Clamp to the slot capacity: a stale/foreign header must never make us copy out of
    // bounds of the placed view (belt and braces on top of the seqlock).
    auto clamp = [](UINT v, UINT cap) { return (cap && v > cap) ? cap : v; };
    const mgpu::StreamShared& mv = g_ctl->streams[1];
    const mgpu::StreamShared& dp = g_ctl->streams[2];
    UINT mw = mv.width, mh = mv.height, dw = dp.width, dh = dp.height;
    if (!read_stream_geo(mv, &mw, &mh) || !read_stream_geo(dp, &dw, &dh)) {
        static bool logged = false;
        if (!logged) { logged = true; logf_("[lock] geometry seqlock kept changing - using the last good values\n"); }
        mw = g_geo.mv_w; mh = g_geo.mv_h; dw = g_geo.depth_w; dh = g_geo.depth_h;
    }
    mw = clamp(mw, mv.slot_w);
    mh = clamp(mh, mv.slot_h);
    dw = clamp(dw, dp.slot_w);
    dh = clamp(dh, dp.slot_h);
    if (mw != g_geo.mv_w || mh != g_geo.mv_h ||
        dw != g_geo.depth_w || dh != g_geo.depth_h) {
        logf_("[lock] input geometry: mv %ux%u, depth %ux%u\n", mw, mh, dw, dh);
        g_geo.mv_w = mw;
        g_geo.mv_h = mh;
        g_geo.depth_w = dw;
        g_geo.depth_h = dh;
    }

    // XeFG requires MV and depth at the SAME size. The two streams can legitimately differ
    // (a DLSS preset can hand the producer MV at output resolution and depth at render
    // resolution), and the codec keeps each stream at its own 1:1 size instead of forcing
    // both through one StreamPercent. The joint reconstruction pass is what normalises
    // them, so a mismatch has to switch it on - otherwise the tags would carry two
    // different sizes and XeFG refuses the frame.
    if (mw >= 256 && dw >= 256 && (mw != dw || mh != dh)) {
        static bool told = false;
        if (!told) {
            told = true;
            logf_("[lock] MV %ux%u and depth %ux%u differ - the reconstruction pass is"
                  " forced ON so both reach one size before tagging (XeFG requires it)\n",
                  mw, mh, dw, dh);
        }
        if (!g_upsample && s_no_upsample == 0) {
            g_upsample = true;
            logf_("[lock] --upsample: switched ON automatically (the two streams differ)\n");
        } else if (!g_upsample) {
            static bool told_off = false;
            if (!told_off) {
                told_off = true;
                logf_("[lock] WARNING: --no-upsample with mismatched MV/depth sizes - XeFG"
                      " will refuse the frame; drop it from the command line\n");
            }
        }
    }
}

static bool ensure_local_copy(ComPtr<ID3D12Resource>& dst, ID3D12Resource* src,
                              UINT w, UINT h, bool uav = false)
{
    if (w == 0 || h == 0) { return false; }
    if (dst) {
        const D3D12_RESOURCE_DESC have = dst->GetDesc();
        if (have.Width == w && have.Height == h &&
            ((have.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) == uav) {
            return true;
        }
        dst.Reset();
    }
    D3D12_RESOURCE_DESC d = src->GetDesc();
    d.Width  = w;
    d.Height = h;
    d.Flags  = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                   : D3D12_RESOURCE_FLAG_NONE;
    // The producer's textures are row-major because they live on a cross-adapter
    // heap; committed local resources must use the usual (unknown/swizzled) layout.
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    const HRESULT hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(&dst));
    if (FAILED(hr)) {
        logf_("[lock] local input copy FAILED hr=0x%08lX (format=%u %ux%u)\n",
              static_cast<unsigned long>(hr), static_cast<unsigned>(d.Format),
              static_cast<unsigned>(d.Width), d.Height);
        return false;
    }
    return true;
}

// Which command list the *input* copies are recorded into. Null means "the frame's own
// list" (g_list), which is what every mode except --input-copy-queue uses. See the copy
// queue block next to run_frame_ex(): only plain CopyTextureRegion is ever redirected, so
// the experimental modes (clears, barriers, --dump readbacks) keep working unchanged.
static ID3D12GraphicsCommandList* g_copy_target = nullptr;
// Number of CopyTextureRegion calls recorded so far. --prefetch-inputs uses it to tell a
// real prefetch from an empty list (a slot whose streams had no view this frame).
static UINT g_copy_count = 0;

// Copies the live content rectangle out of one of our placed views into a local texture.
// Both sides agree on the rectangle through StreamShared.width/height; the slot capacity
// (and therefore the row pitch) is fixed for the lifetime of the process.
static void copy_content(ID3D12Resource* dst, ID3D12Resource* src, UINT w, UINT h)
{
    if (!dst || !src || w == 0 || h == 0) { return; }
    ID3D12GraphicsCommandList* list = g_copy_target ? g_copy_target : g_list.Get();
    if (!list) { return; }
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = dst;
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box{};
    box.left = 0;
    box.top = 0;
    box.front = 0;
    box.right = w;
    box.bottom = h;
    box.back = 1;
    list->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
    ++g_copy_count;
}

// --------------------------------------------------- --mv-verify (bit-exact round trip)
// The producer encoded `source` into the bytes sitting in the slot. This re-encodes the
// freshly decoded MV and compares the two byte streams: if they are identical then
// decode(bytes) == source bit for bit, because the encoder is injective (lossless - 32
// real 4K frames bit-exact against the CPU reference plus all 65536 fp16 bit patterns).
// A few samples are enough; each one reads back ~34 MB twice, so this is opt-in only.
static bool                          g_mv_verify = false;
static ComPtr<ID3D12RootSignature>   g_ver_rs;
static ComPtr<ID3D12DescriptorHeap>  g_ver_heap;      // t0 texture, u0 out, u1 TileLen
static ComPtr<ID3D12PipelineState>   g_ver_pso;
static ComPtr<ID3D12Resource>        g_ver_tl, g_ver_out, g_ver_rb_enc, g_ver_rb_slot;
static uint64_t                      g_ver_bytes = 0;
static uint32_t                      g_ver_tiles = 0;
static ULONGLONG                     g_ver_ms = 0;
static int                           g_ver_samples = 0;
static uint64_t                      g_ver_slot_bytes = 0;
static D3D12_RESOURCE_STATES         g_ver_out_state = D3D12_RESOURCE_STATE_COPY_DEST;

static bool init_mv_verify()
{
    static int state = 0;
    if (state != 0) { return state > 0; }
    state = -1;
    D3D12_DESCRIPTOR_RANGE rg[3]{};
    for (int i = 0; i < 3; ++i) {
        rg[i].NumDescriptors     = 1;
        rg[i].BaseShaderRegister = static_cast<UINT>(i == 0 ? 0 : (i - 1));
        rg[i].RangeType          = (i == 0) ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                                            : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    }
    D3D12_ROOT_PARAMETER rp[4]{};
    rp[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 6;
    rp[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
    for (int i = 0; i < 3; ++i) {
        rp[i + 1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[i + 1].DescriptorTable.NumDescriptorRanges = 1;
        rp[i + 1].DescriptorTable.pDescriptorRanges   = &rg[i];
        rp[i + 1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
    rsd.pParameters   = rp;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(g_device->CreateRootSignature(0, blob->GetBufferPointer(),
                                             blob->GetBufferSize(), IID_PPV_ARGS(&g_ver_rs)))) {
        logf_("[lock] --mv-verify: root signature FAILED\n");
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 3;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_ver_heap)))) { return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature     = g_ver_rs.Get();
    pd.CS.pShaderBytecode = kFrEncodeCSO;
    pd.CS.BytecodeLength  = sizeof(kFrEncodeCSO);
    if (FAILED(g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_ver_pso)))) {
        logf_("[lock] --mv-verify: encode PSO FAILED\n");
        return false;
    }
    state = 1;
    logf_("[lock] --mv-verify: round-trip check ready (%zu B encode blob)\n",
          sizeof(kFrEncodeCSO));
    return true;
}

static ID3D12Resource* ver_buffer(ComPtr<ID3D12Resource>& r, uint64_t bytes, bool readback)
{
    if (r && r->GetDesc().Width >= bytes) { return r.Get(); }
    r.Reset();
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = readback ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = bytes;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags            = readback ? D3D12_RESOURCE_FLAG_NONE
                                  : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&r)))) {
        return nullptr;
    }
    return r.Get();
}

// Records one sample (re-encode + two readbacks) and, on the next call, compares the two
// byte streams from the *previous* sample - two seconds later, so nothing has to wait.
static void mv_verify_tick(UINT slot, UINT w, UINT h)
{
    if (!g_mv_verify || slot >= mgpu::kMaxSlots || !g_mv_local[slot] || !g_mv[slot]) { return; }
    const ULONGLONG now = GetTickCount64();

    if (g_ver_bytes != 0 && g_ver_rb_enc && g_ver_rb_slot) {
        void* a = nullptr;
        void* b = nullptr;
        const D3D12_RANGE rr{ 0, static_cast<SIZE_T>(g_ver_bytes) };
        if (SUCCEEDED(g_ver_rb_enc->Map(0, &rr, &a)) && a != nullptr &&
            SUCCEEDED(g_ver_rb_slot->Map(0, &rr, &b)) && b != nullptr) {
            const uint8_t* pa = static_cast<const uint8_t*>(a);
            const uint8_t* pb = static_cast<const uint8_t*>(b);
            uint64_t diff = 0, first = 0;
            for (uint64_t i = 0; i < g_ver_bytes; ++i) {
                if (pa[i] != pb[i]) { if (diff == 0) { first = i; } ++diff; }
            }
            if (diff == 0) {
                logf_("[lock] --mv-verify: sample %d BIT-EXACT - the re-encoded %llu B match"
                      " the producer's slot byte for byte\n",
                      g_ver_samples, static_cast<unsigned long long>(g_ver_bytes));
            } else {
                logf_("[lock] --mv-verify: sample %d MISMATCH - %llu of %llu bytes differ,"
                      " first at +%llu\n",
                      g_ver_samples, static_cast<unsigned long long>(diff),
                      static_cast<unsigned long long>(g_ver_bytes),
                      static_cast<unsigned long long>(first));
            }
            const D3D12_RANGE none{ 0, 0 };
            g_ver_rb_enc->Unmap(0, &none);
            g_ver_rb_slot->Unmap(0, &none);
        }
        g_ver_bytes = 0;
    }
    if (g_ver_samples >= 3) { return; }
    if (g_ver_ms != 0 && (now - g_ver_ms) < 2000) { return; }
    if (!init_mv_verify()) { g_mv_verify = false; return; }

    // Only the written part of the slot is comparable: the encoder fills every tile's full
    // 264-word stride, so the tail beyond the last tile is untouched in both buffers.
    const uint32_t tx = (w + 31u) / 32u;
    const uint32_t ty = (h + 7u) / 8u;
    const uint32_t tiles = tx * ty;
    const uint64_t bytes = static_cast<uint64_t>(tiles) * 1056ull;
    if (bytes == 0 || g_mv[slot]->GetDesc().Width < bytes) { return; }
    const int next_sample = g_ver_samples + 1;

    ID3D12GraphicsCommandList* list = g_list.Get();
    ID3D12Resource* out = ver_buffer(g_ver_out, bytes, false);
    ID3D12Resource* rbe = ver_buffer(g_ver_rb_enc, bytes, true);
    ID3D12Resource* rbs = ver_buffer(g_ver_rb_slot, bytes, true);
    ID3D12Resource* tl  = ver_buffer(g_ver_tl, 65536ull * 4ull, false);
    if (list == nullptr || !out || !rbe || !rbs || !tl) {
        logf_("[lock] --mv-verify: resources FAILED - check disabled\n");
        g_mv_verify = false;
        return;
    }
    ID3D12Resource* mv = g_mv_local[slot].Get();
    ID3D12Resource* slot_buf = g_mv[slot].Get();

    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource   = mv;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &b);
    b.Transition.pResource   = out;
    b.Transition.StateBefore = g_ver_out_state;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->ResourceBarrier(1, &b);
    g_ver_out_state = D3D12_RESOURCE_STATE_COPY_SOURCE;

    const UINT inc = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_ver_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_ver_heap->GetGPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = DXGI_FORMAT_R16G16_FLOAT;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    g_device->CreateShaderResourceView(mv, &srv, cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format                     = DXGI_FORMAT_R32_TYPELESS;   // raw buffer view
    uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.StructureByteStride = 0;
    uav.Buffer.Flags               = D3D12_BUFFER_UAV_FLAG_RAW;
    uav.Buffer.NumElements         = static_cast<UINT>(out->GetDesc().Width / 4ull);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav = cpu;
    cpu_uav.ptr += inc;
    g_device->CreateUnorderedAccessView(out, nullptr, &uav, cpu_uav);
    // TileLen is RWStructuredBuffer<uint> in the shader: the view MUST be structured
    // (stride 4); a raw view silently drops every write.
    uav.Format                     = DXGI_FORMAT_UNKNOWN;
    uav.Buffer.StructureByteStride = 4;
    uav.Buffer.Flags               = D3D12_BUFFER_UAV_FLAG_NONE;
    uav.Buffer.NumElements = static_cast<UINT>(tl->GetDesc().Width / 4ull);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_tl = cpu_uav;
    cpu_tl.ptr += inc;
    g_device->CreateUnorderedAccessView(tl, nullptr, &uav, cpu_tl);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
    gpu_uav.ptr += inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_tl = gpu_uav;
    gpu_tl.ptr += inc;

    uint32_t params[6] = { w, h, 1u, tx, ty, 264u };
    list->SetComputeRootSignature(g_ver_rs.Get());
    list->SetPipelineState(g_ver_pso.Get());
    ID3D12DescriptorHeap* heaps[1] = { g_ver_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRoot32BitConstants(0, 6, params, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->SetComputeRootDescriptorTable(2, gpu_uav);
    list->SetComputeRootDescriptorTable(3, gpu_tl);
    list->Dispatch(tiles, 1, 1);

    D3D12_RESOURCE_BARRIER ub{};
    ub.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = out;
    list->ResourceBarrier(1, &ub);
    b.Transition.pResource   = out;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    list->CopyBufferRegion(rbe, 0, out, 0, bytes);

    // The producer's bytes, exactly as they arrived (the decode left the slot in COPY_DEST).
    b.Transition.pResource   = slot_buf;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    list->CopyBufferRegion(rbs, 0, slot_buf, 0, bytes);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &b);

    b.Transition.pResource   = mv;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1, &b);
    b.Transition.pResource   = out;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &b);
    g_ver_out_state = D3D12_RESOURCE_STATE_COPY_DEST;

    g_ver_bytes = bytes;
    g_ver_ms = now;
    ++g_ver_samples;
    logf_("[lock] --mv-verify: sample %d recorded (%u tiles, %llu B, MV %ux%u)\n",
          next_sample, tiles, static_cast<unsigned long long>(bytes), w, h);
}

// The producer's encode pass wrote the MV bytes into the slot buffer (streams[1].codec=1);
// this pass turns them back into the exact fp16 bit patterns XeFG expects, straight into
// g_mv_local[slot]. Same tiling as the encoder: 32x8 pixels per tile, one 256-thread group
// per tile.
static ComPtr<ID3D12RootSignature>  g_fr_rs;
static ComPtr<ID3D12DescriptorHeap> g_fr_heap;    // 0 = slot SRV (bytes), 1 = MV UAV
static ComPtr<ID3D12PipelineState>  g_fr_pso;

static bool init_fr_decode()
{
    static int state = 0;                        // 0 = untried, 1 = ready, -1 = failed
    if (state != 0) { return state > 0; }
    state = -1;
    D3D12_DESCRIPTOR_RANGE rg[2]{};
    rg[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rg[0].NumDescriptors     = 1;
    rg[0].BaseShaderRegister = 0;
    rg[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rg[1].NumDescriptors     = 1;
    rg[1].BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER rp[3]{};
    rp[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 6;
    rp[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
    for (int i = 0; i < 2; ++i) {
        rp[i + 1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[i + 1].DescriptorTable.NumDescriptorRanges = 1;
        rp[i + 1].DescriptorTable.pDescriptorRanges   = &rg[i];
        rp[i + 1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 3;
    rsd.pParameters   = rp;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(g_device->CreateRootSignature(0, blob->GetBufferPointer(),
                                             blob->GetBufferSize(), IID_PPV_ARGS(&g_fr_rs)))) {
        logf_("[lock] fr: root signature FAILED\n");
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_fr_heap)))) {
        logf_("[lock] fr: descriptor heap FAILED\n");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature     = g_fr_rs.Get();
    pd.CS.pShaderBytecode = kFrDecodeCSO;
    pd.CS.BytecodeLength  = sizeof(kFrDecodeCSO);
    if (FAILED(g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_fr_pso)))) {
        logf_("[lock] fr: decode PSO FAILED (%zu bytes)\n", sizeof(kFrDecodeCSO));
        return false;
    }
    state = 1;
    logf_("[lock] fr: MV decode pass ready (tile 32x8, 264 words per slot tile)\n");
    return true;
}

// Local MV sink for the codec path: R16G16_FLOAT with UAV access, because the decode
// pass writes it. Same lifecycle as ensure_local_copy (recreated on geometry change).
static bool ensure_mv_local_codec(ComPtr<ID3D12Resource>& dst, UINT w, UINT h)
{
    if (w < 16 || h < 16) { return false; }
    if (dst) {
        const D3D12_RESOURCE_DESC have = dst->GetDesc();
        if (have.Width == w && have.Height == h &&
            have.Format == DXGI_FORMAT_R16G16_FLOAT &&
            (have.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
            return true;
        }
        dst.Reset();
    }
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
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&dst)))) {
        logf_("[lock] fr: MV local %ux%u FAILED\n", w, h);
        return false;
    }
    return true;
}

// Decodes this slot's MV bytes into g_mv_local[slot]. The read of the shared slot is ordered
// after the producer's publication by order_publication_read() (queue-side wait on fwd[slot],
// issued by run_frame_ex before copy_inputs_local()); the barriers below are state
// bookkeeping for our own view of the shared memory.
static bool decode_mv_slot(UINT slot, UINT w, UINT h)
{
    if (slot >= mgpu::kMaxSlots || !g_mv[slot]) { return false; }
    ID3D12Resource* src = g_mv[slot].Get();
    if (src->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) { return false; }
    ID3D12GraphicsCommandList* list = g_list.Get();
    if (!list || !init_fr_decode()) { return false; }
    if (!ensure_mv_local_codec(g_mv_local[slot], w, h)) { return false; }
    ID3D12Resource* dst = g_mv_local[slot].Get();

    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource   = src;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &b);
    b.Transition.pResource   = dst;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->ResourceBarrier(1, &b);

    const UINT inc = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_fr_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_fr_heap->GetGPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                     = DXGI_FORMAT_R32_TYPELESS;   // raw ByteAddressBuffer
    srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement        = 0;
    srv.Buffer.NumElements         = static_cast<UINT>(src->GetDesc().Width / 4ull);
    srv.Buffer.StructureByteStride = 0;
    srv.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_RAW;
    g_device->CreateShaderResourceView(src, &srv, cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format             = DXGI_FORMAT_R16G16_FLOAT;
    uav.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Texture2D.MipSlice = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav = cpu;
    cpu_uav.ptr += inc;
    g_device->CreateUnorderedAccessView(dst, nullptr, &uav, cpu_uav);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
    gpu_uav.ptr += inc;

    const UINT tx = (w + 31u) / 32u;
    const UINT ty = (h + 7u) / 8u;
    uint32_t params[6] = { w, h, 1u, tx, ty, 264u };
    list->SetComputeRootSignature(g_fr_rs.Get());
    list->SetPipelineState(g_fr_pso.Get());
    ID3D12DescriptorHeap* heaps[1] = { g_fr_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRoot32BitConstants(0, 6, params, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->SetComputeRootDescriptorTable(2, gpu_uav);
    list->Dispatch(tx * ty, 1, 1);

    D3D12_RESOURCE_BARRIER ub{};
    ub.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = dst;
    list->ResourceBarrier(1, &ub);
    b.Transition.pResource   = dst;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1, &b);
    b.Transition.pResource   = src;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &b);

    d3d_drain_msgs("fr-decode");
    mv_verify_tick(slot, w, h);
    static bool told = false;
    if (!told) {
        told = true;
        logf_("[lock] fr: decoded MV slot %u -> %ux%u (%u tiles, %llu B slot)\n",
              slot, w, h, tx * ty, static_cast<unsigned long long>(src->GetDesc().Width));
        // The two streams are independent, so spell out the plan the producer chose.
        logf_("[lock] fr: transport plan: mv=codec %ux%u, depth=%s %ux%u, upsample %s\n",
              g_geo.mv_w, g_geo.mv_h,
              depth_codec_shared() ? "codec" : "texture",
              g_geo.depth_w, g_geo.depth_h,
              (g_geo.mv_w >= g_full_w && g_geo.depth_w >= g_full_w) ? "not needed"
                                                                  : "runs (depth)");
    }
    return true;
}

// Depth sink for the codec path. ensure_local_copy() copies the source's DESCRIPTION, which
// on the codec path is a byte buffer - so this builds a proper R32_FLOAT texture instead
// (UAV-capable: the decode pass writes it).
static bool ensure_depth_sink_codec(ComPtr<ID3D12Resource>& dst, UINT w, UINT h)
{
    if (w < 16 || h < 16) { return false; }
    if (dst) {
        const D3D12_RESOURCE_DESC have = dst->GetDesc();
        if (have.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            have.Width == w && have.Height == h &&
            have.Format == DXGI_FORMAT_R32_FLOAT &&
            (have.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
            return true;
        }
        dst.Reset();
    }
    D3D12_RESOURCE_DESC d{};
    d.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width            = w;
    d.Height           = h;
    d.DepthOrArraySize = 1;
    d.MipLevels        = 1;
    d.Format           = DXGI_FORMAT_R32_FLOAT;
    d.SampleDesc.Count = 1;
    d.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&dst)))) {
        logf_("[lock] fr: depth sink %ux%u FAILED\n", w, h);
        return false;
    }
    return true;
}

// ------------------------------------------------- v30: depth decode (fr codec, D32)
// Same kernel, R32 variant: one 32-bit value per pixel, so the depth stream can run at
// 1:1 through the same codec instead of being downsampled to 150% and resampled here.
// A SEPARATE descriptor heap from the MV pass: both dispatches are recorded into the same
// command list, so sharing one heap would make the second descriptor write win at
// execution time for both.
static ComPtr<ID3D12DescriptorHeap> g_fr_d_heap;
static ComPtr<ID3D12PipelineState>  g_fr_d_pso;

static bool init_fr_decode_depth()
{
    static int state = 0;
    if (state != 0) { return state > 0; }
    state = -1;
    if (!init_fr_decode()) { return false; }        // root signature is shared
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;                          // slot SRV (bytes) | depth UAV
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_fr_d_heap)))) {
        logf_("[lock] fr: depth descriptor heap FAILED\n");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature     = g_fr_rs.Get();
    pd.CS.pShaderBytecode = kFrDecodeD32CSO;
    pd.CS.BytecodeLength  = sizeof(kFrDecodeD32CSO);
    if (FAILED(g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_fr_d_pso)))) {
        logf_("[lock] fr: depth decode PSO FAILED (%zu bytes)\n", sizeof(kFrDecodeD32CSO));
        return false;
    }
    state = 1;
    logf_("[lock] fr: depth decode pass ready (D32 variant, %zu B blob)\n",
          sizeof(kFrDecodeD32CSO));
    return true;
}

static bool decode_depth_slot(UINT slot, ID3D12Resource* dst, UINT w, UINT h)
{
    if (dst == nullptr || slot >= mgpu::kMaxSlots || !g_depth[slot]) { return false; }
    ID3D12Resource* src = g_depth[slot].Get();
    if (src == nullptr || src->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        return false;
    }
    ID3D12GraphicsCommandList* list = g_list.Get();
    if (list == nullptr || !init_fr_decode_depth()) { return false; }

    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource   = src;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &b);
    b.Transition.pResource   = dst;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->ResourceBarrier(1, &b);

    const UINT inc = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_fr_d_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_fr_d_heap->GetGPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                     = DXGI_FORMAT_R32_TYPELESS;   // raw ByteAddressBuffer
    srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement        = 0;
    srv.Buffer.NumElements         = static_cast<UINT>(src->GetDesc().Width / 4ull);
    srv.Buffer.StructureByteStride = 0;
    srv.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_RAW;
    g_device->CreateShaderResourceView(src, &srv, cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format             = DXGI_FORMAT_R32_FLOAT;
    uav.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Texture2D.MipSlice = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav = cpu;
    cpu_uav.ptr += inc;
    g_device->CreateUnorderedAccessView(dst, nullptr, &uav, cpu_uav);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
    gpu_uav.ptr += inc;

    const UINT tx = (w + 31u) / 32u;
    const UINT ty = (h + 7u) / 8u;
    uint32_t params[6] = { w, h, 1u, tx, ty, 264u };
    list->SetComputeRootSignature(g_fr_rs.Get());
    list->SetPipelineState(g_fr_d_pso.Get());
    ID3D12DescriptorHeap* heaps[1] = { g_fr_d_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRoot32BitConstants(0, 6, params, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->SetComputeRootDescriptorTable(2, gpu_uav);
    list->Dispatch(tx * ty, 1, 1);

    D3D12_RESOURCE_BARRIER ub{};
    ub.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = dst;
    list->ResourceBarrier(1, &ub);
    b.Transition.pResource   = dst;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1, &b);
    b.Transition.pResource   = src;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &b);

    static bool told = false;
    if (!told) {
        told = true;
        logf_("[lock] fr: decoded DEPTH -> %ux%u (%u tiles, %llu B slot, codec 1:1)\n",
              w, h, tx * ty, static_cast<unsigned long long>(src->GetDesc().Width));
    }
    return true;
}

static ComPtr<ID3D12DescriptorHeap> g_fr_u_heap;
static ComPtr<ID3D12DescriptorHeap> g_fr_u_heap_patch;   // stream 4, same PSOs
static ComPtr<ID3D12PipelineState>  g_fr_u_pso_r10;
static ComPtr<ID3D12PipelineState>  g_fr_u_pso_rgba8;

static bool init_fr_decode_ui();
static bool ensure_ui_sink_codec(ComPtr<ID3D12Resource>& dst, UINT w, UINT h,
                                 DXGI_FORMAT fmt);

static bool decode_ui_slot_from(ID3D12Resource* src, UINT slot, ID3D12Resource* dst,
                                UINT w, UINT h, DXGI_FORMAT fmt, bool patch_slot)
{
    if (dst == nullptr || src == nullptr || slot >= mgpu::kMaxSlots) { return false; }
    if (src->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        return false;
    }
    ID3D12GraphicsCommandList* list = g_list.Get();
    if (list == nullptr || !init_fr_decode_ui()) { return false; }

    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource   = src;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &b);
    b.Transition.pResource   = dst;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->ResourceBarrier(1, &b);

    const UINT inc = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = (patch_slot ? g_fr_u_heap_patch : g_fr_u_heap)
                                          ->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = (patch_slot ? g_fr_u_heap_patch : g_fr_u_heap)
                                          ->GetGPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                     = DXGI_FORMAT_R32_TYPELESS;   // raw ByteAddressBuffer
    srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement        = 0;
    srv.Buffer.NumElements         = static_cast<UINT>(src->GetDesc().Width / 4ull);
    srv.Buffer.StructureByteStride = 0;
    srv.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_RAW;
    g_device->CreateShaderResourceView(src, &srv, cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format             = fmt;
    uav.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Texture2D.MipSlice = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav = cpu;
    cpu_uav.ptr += inc;
    g_device->CreateUnorderedAccessView(dst, nullptr, &uav, cpu_uav);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
    gpu_uav.ptr += inc;

    const UINT tx = (w + 31u) / 32u;
    const UINT ty = (h + 7u) / 8u;
    uint32_t params[6] = { w, h, 1u, tx, ty, 264u };
    list->SetComputeRootSignature(g_fr_rs.Get());
    list->SetPipelineState(fmt == DXGI_FORMAT_R10G10B10A2_UNORM
                               ? g_fr_u_pso_r10.Get() : g_fr_u_pso_rgba8.Get());
    ID3D12DescriptorHeap* heaps[1] = { patch_slot ? g_fr_u_heap_patch.Get()
                                                  : g_fr_u_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRoot32BitConstants(0, 6, params, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->SetComputeRootDescriptorTable(2, gpu_uav);
    list->Dispatch(tx * ty, 1, 1);

    D3D12_RESOURCE_BARRIER ub{};
    ub.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = dst;
    list->ResourceBarrier(1, &ub);
    b.Transition.pResource   = dst;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1, &b);
    b.Transition.pResource   = src;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &b);

    static bool told = false;
    if (!told) {
        told = true;
        logf_("[lock] fr: decoded UI -> %ux%u fmt %u (%u tiles, %llu B slot, codec 1:1)\n",
              w, h, static_cast<unsigned>(fmt), tx * ty,
              static_cast<unsigned long long>(src->GetDesc().Width));
    }
    return true;
}

static bool decode_ui_slot(UINT slot, ID3D12Resource* dst, UINT w, UINT h,
                           DXGI_FORMAT fmt)
{
    return decode_ui_slot_from(g_ui[slot].Get(), slot, dst, w, h, fmt, false);
}


// ------------------------------------------------- v31/P2: UI decode (fr codec, packed)
// Same kernel, UI variants: R10G10B10A2 / R8G8B8A8 packed to one u32 per pixel on the
// producer, unpacked into the local UI texture here. SEPARATE heap from MV/depth: all
// decodes are recorded into the same list, so a shared descriptor write would make the
// last one win at execution time.

static bool init_fr_decode_ui()
{
    static int state = 0;
    if (state != 0) { return state > 0; }
    state = -1;
    if (!init_fr_decode()) { return false; }        // root signature is shared
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;                          // slot SRV (bytes) | UI UAV
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_fr_u_heap)))) {
        logf_("[lock] fr: UI descriptor heap FAILED\n");
        return false;
    }
    // Stream 4 patch decode shares the PSOs but needs its own descriptor slots: both
    // decodes are recorded into the same command list before submit.
    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_fr_u_heap_patch)))) {
        logf_("[lock] fr: patch descriptor heap FAILED\n");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature     = g_fr_rs.Get();
    pd.CS.pShaderBytecode = kFrDecodeUiR10CSO;
    pd.CS.BytecodeLength  = sizeof(kFrDecodeUiR10CSO);
    if (FAILED(g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_fr_u_pso_r10)))) {
        logf_("[lock] fr: UI R10 decode PSO FAILED (%zu bytes)\n", sizeof(kFrDecodeUiR10CSO));
        return false;
    }
    pd.CS.pShaderBytecode = kFrDecodeUiRgba8CSO;
    pd.CS.BytecodeLength  = sizeof(kFrDecodeUiRgba8CSO);
    if (FAILED(g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_fr_u_pso_rgba8)))) {
        logf_("[lock] fr: UI RGBA8 decode PSO FAILED (%zu bytes)\n",
              sizeof(kFrDecodeUiRgba8CSO));
        return false;
    }
    state = 1;
    logf_("[lock] fr: UI decode pass ready (R10 %zu B, RGBA8 %zu B)\n",
          sizeof(kFrDecodeUiR10CSO), sizeof(kFrDecodeUiRgba8CSO));
    return true;
}

static bool ensure_ui_sink_codec(ComPtr<ID3D12Resource>& dst, UINT w, UINT h,
                                 DXGI_FORMAT fmt)
{
    if (w < 16 || h < 16) { return false; }
    if (dst) {
        const D3D12_RESOURCE_DESC have = dst->GetDesc();
        if (have.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            have.Width == w && have.Height == h && have.Format == fmt &&
            (have.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
            return true;
        }
        dst.Reset();
    }
    D3D12_RESOURCE_DESC d{};
    d.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width            = w;
    d.Height           = h;
    d.DepthOrArraySize = 1;
    d.MipLevels        = 1;
    d.Format           = fmt;
    d.SampleDesc.Count = 1;
    d.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&dst)))) {
        logf_("[lock] fr: UI sink %ux%u fmt %u FAILED\n", w, h,
              static_cast<unsigned>(fmt));
        return false;
    }
    return true;
}

#if MGPU_FG_OF
static void of_bind_depth(UINT i, ID3D12Resource* res);   // defined with the OF state below
static void of_bind_hist();                              // ditto (history colours)
#endif

static void ensure_dummy_mv(UINT slot);      // defined below (used by --no-mv)
static void ensure_dummy_depth(UINT slot);   // defined below (used by --no-depth)

static void copy_inputs_local(UINT slot)
{
    if (slot >= mgpu::kMaxSlots) { return; }
    // UI state is tracked per UI publication slot (see ui_slot_index), so a slot reuses the
    // local texture of the UI slot it maps to.
    const UINT uislot = ui_slot_index(slot);
    g_ui_fresh[uislot] = false;
    g_hudless_fresh[slot] = false;
    if (g_hudless_slots) { g_hudless_fresh[slot % g_hudless_slots] = false; }
    // --fg-colour shared: the colour rides the same path as MV/depth - one cross-adapter
    // CopyTextureRegion of the live rectangle into the sink the convert draw reads.
    if (g_fg_colour_shared && g_colour[slot] && g_shared12) {
        const mgpu::StreamShared& sc = g_ctl->streams[0];
        const UINT cw = sc.width;
        const UINT ch = sc.height;
        if (cw && ch) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                logf_("[lock] --fg-colour shared: copying %ux%u from the producer's colour"
                      " slot (slot size %ux%u)\n", cw, ch,
                      g_colour[slot]->GetDesc().Width,
                      g_colour[slot]->GetDesc().Height);
            }
            copy_content(g_shared12.Get(), g_colour[slot].Get(), cw, ch);
        }
    }
    // COMMON is fine on both sides: D3D12 promotes/demotes implicitly for copies.
    // Auto (default): the producer's record decides. It says kInputFlagMvActive while it is
    // feeding vectors; when it stops, this frame's vectors come from our flow pass (if
    // compiled in) or are zeroed. Explicit --no-mv / --fg-of / --mv-shadow keep their old
    // meaning and turn the auto choice off.
    if (mv_from_flow()) {
        // Vectors come from our own flow pass; the producer's MV stream is neither copied
        // nor trusted (and its cross-adapter request is dropped, see g_stream_mask).
    } else if (mv_from_producer() && g_mv[slot] && mv_codec_shared()) {
        // v30: the slot holds fr-codec bytes, not pixels - reconstruct the exact fp16 MV
        // input on this GPU. No copy and no upsample: the codec runs at 1:1.
        static bool told_fail = false;
        if (!decode_mv_slot(slot, g_geo.mv_w, g_geo.mv_h) && !told_fail) {
            told_fail = true;
            logf_("[lock] fr: decode failed - MV keeps its previous contents\n");
        }
    } else if (mv_from_producer() && g_mv[slot]) {
        // Dumb producer: with kRequestRawMv the slot holds the source's own resolution, so
        // the resample happens here (see prepare_stream). The target size is the producer's
        // published render size - that is what the frame generator tags motion vectors at.
        const bool raw_mv = g_ctl && (g_ctl->request_mask & mgpu::kRequestRawMv) != 0;
        const mgpu::FrameShared& fr_mv = g_ctl ? g_ctl->frames[slot] : mgpu::FrameShared{};
        UINT rw = fr_mv.render_w, rh = fr_mv.render_h;
        if (rw < 256 || rh < 256) { rw = g_geo.mv_w; rh = g_geo.mv_h; }
        if (raw_mv && (rw != g_geo.mv_w || rh != g_geo.mv_h) &&
            ensure_local_copy(g_mv_local[slot], g_mv[slot].Get(), rw, rh)) {
            static bool logged = false;
            if (prepare_stream(g_mv[slot].Get(), g_geo.mv_w, g_geo.mv_h,
                               g_mv_local[slot].Get(), rw, rh, 0)) {
                if (!logged) {
                    logged = true;
                    logf_("[lock] raw MV %ux%u -> %ux%u resampled on this GPU (prepare pass)\n",
                          g_geo.mv_w, g_geo.mv_h, rw, rh);
                }
            } else {
                copy_content(g_mv_local[slot].Get(), g_mv[slot].Get(), g_geo.mv_w, g_geo.mv_h);
            }
        } else if (ensure_local_copy(g_mv_local[slot], g_mv[slot].Get(),
                                     g_geo.mv_w, g_geo.mv_h)) {
            copy_content(g_mv_local[slot].Get(), g_mv[slot].Get(), g_geo.mv_w, g_geo.mv_h);
        }
    } else {
        // No producer vectors and no flow pass: real depth, zeroed motion vectors.
        // --mv-shadow still copies the producer's stream into a sink texture for A/B.
        ensure_dummy_mv(slot);
        if (mv_codec_shared()) {
            // MvCodec=1 with a title that does not publish MV (measured on Wuchang: it tags
            // MV for the first few frames, then the record's mask drops to depth-only). The
            // codec slot exists but is never written, so it must NOT be decoded - this branch
            // is what keeps that case working. Nothing else changes: dummy vectors, real depth.
            static bool told_nomv = false;
            if (!told_nomv) {
                told_nomv = true;
                logf_("[lock] fr: codec MV slot exists but the game publishes no MV - using"
                      " dummy vectors (depth still travels through the codec)\n");
            }
        } else if (g_mv_shadow && g_mv[slot]) {
            // The shadow sink needs a TEXTURE source; a codec slot is a byte buffer.
            if (ensure_local_copy(g_mv_shadow_tex[slot], g_mv[slot].Get(),
                                  g_geo.mv_w, g_geo.mv_h)) {
                copy_content(g_mv_shadow_tex[slot].Get(), g_mv[slot].Get(),
                             g_geo.mv_w, g_geo.mv_h);
            }
        }
    }
    // --no-depth: skip the depth stream. Depth is the bigger half of the cross-adapter
    // traffic at high render resolutions (2160p R32 = 33 MB/frame), and XeFG only uses
    // it for disocclusion clamping - motion vectors carry the interpolation. The tagged
    // resource stays the right size/format, so this isolates the transfer cost.
    if (g_no_depth || !g_depth[slot]) {
        // The producer decides per session what it can transfer (format + command-list type).
        // When it did not lay out a depth stream, there is no slot to read - so give XeFG a
        // dummy surface instead of leaving its input undefined. Motion vectors carry the
        // interpolation; depth only feeds disocclusion clamping.
        if (!g_no_depth && !g_depth[slot]) {
            static bool logged_absent = false;
            if (!logged_absent) {
                logged_absent = true;
                logf_("[lock] the producer does not transfer depth this session (its format"
                      " cannot cross the adapter) - using a dummy depth; the game's motion"
                      " vectors carry the interpolation\n");
            }
        }
        ensure_dummy_depth(slot);
    } else if (g_depth[slot]) {
        // Local ring: copy the *newest completed* shared slot (never an older one - the
        // producer would be rewriting it, and two adapters touching the same memory
        // halved both the game and our loop), then hand XeFG a depth from `--depth-lag`
        // frames ago out of our own textures.
        const UINT need = 1u + static_cast<UINT>((g_depth_lag > 0) ? g_depth_lag : 0);
        const bool dec_depth = depth_codec_shared();
        bool ok = true;
        for (UINT i = 0; i < need; ++i) {
            // The codec slot is a byte BUFFER, so its desc cannot be copied into a texture.
            const bool ring_ok = dec_depth
                ? ensure_depth_sink_codec(g_depth_ring[i], g_geo.depth_w, g_geo.depth_h)
                : ensure_local_copy(g_depth_ring[i], g_depth[slot].Get(),
                                    g_geo.depth_w, g_geo.depth_h);
            if (!ring_ok) {
                ok = false;
                break;
            }
        }
        if (ok) {
            g_ring_write = (g_ring_write + 1) % need;
#if MGPU_FG_OF
            // Only the flow pass reads depth through our own descriptor heap; without it
            // the ring exists purely for --depth-lag.
            if (!g_of_depth_bound[g_ring_write]) {
                of_bind_depth(g_ring_write, g_depth_ring[g_ring_write].Get());
                g_of_depth_bound[g_ring_write] = true;
            }
#endif
            if (dec_depth) {
                if (!decode_depth_slot(slot, g_depth_ring[g_ring_write].Get(),
                                       g_geo.depth_w, g_geo.depth_h)) {
                    static bool told_dd = false;
                    if (!told_dd) {
                        told_dd = true;
                        logf_("[lock] fr: depth decode failed - depth keeps its previous"
                              " contents\n");
                    }
                }
            } else {
                copy_content(g_depth_ring[g_ring_write].Get(), g_depth[slot].Get(),
                             g_geo.depth_w, g_geo.depth_h);
            }
            if (g_ring_count < need) { ++g_ring_count; }
            g_ring_need = need;
        } else if (!depth_codec_shared() &&
                   ensure_local_copy(g_depth_local[slot], g_depth[slot].Get(),
                                     g_geo.depth_w, g_geo.depth_h)) {
            // A 8-byte depth slot means the producer sent the field raw; convert it here
            // instead of asking the game's GPU to do it.
            const DXGI_FORMAT df = g_depth[slot]->GetDesc().Format;
            const bool wide_depth = (df == DXGI_FORMAT_R32G8X24_TYPELESS ||
                                     df == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
            if (!wide_depth || !prepare_stream(g_depth[slot].Get(), g_geo.depth_w, g_geo.depth_h,
                                               g_depth_local[slot].Get(), g_geo.depth_w,
                                               g_geo.depth_h, 1)) {
                copy_content(g_depth_local[slot].Get(), g_depth[slot].Get(),
                             g_geo.depth_w, g_geo.depth_h);
            }
        }
    }
    // stream 3: UI (colour+alpha). With UiFullRes it is a full-resolution UI (game or
    // render-card synth); with UiCodec it travels as packed bytes and is decoded here.
    // `uislot` comes from the top of the function: it is both the shared slot index and the
    // index of this slot's local decode target.
    if (g_ui[uislot]) {
        const mgpu::StreamShared& su = g_ctl->streams[3];
        if (su.width >= 64 && su.height >= 64) {
            if (su.codec != 0) {
                const DXGI_FORMAT fmt =
                    (su.format == static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM))
                        ? DXGI_FORMAT_R10G10B10A2_UNORM
                        : DXGI_FORMAT_R8G8B8A8_UNORM;
                if (ensure_ui_sink_codec(g_ui_local[uislot], su.width, su.height, fmt) &&
                    decode_ui_slot(uislot, g_ui_local[uislot].Get(), su.width, su.height, fmt)) {
                    g_ui_fresh[uislot] = true;
                } else {
                    static bool told_dec = false;
                    if (!told_dec) {
                        told_dec = true;
                        logf_("[lock] fr: UI decode failed - stream 3 not fresh this frame\n");
                    }
                }
            } else if (ensure_local_copy(g_ui_local[uislot], g_ui[uislot].Get(),
                                         su.width, su.height)) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    logf_("[lock] stream 3: copying %ux%u (fmt %u) from the producer's"
                          " slot 3\n", su.width, su.height, su.format);
                }
                copy_content(g_ui_local[uislot].Get(), g_ui[uislot].Get(),
                             su.width, su.height);
                g_ui_fresh[uislot] = true;
            }
        }
    }

    // stream 4: v34 support patch (codec, sparse) or legacy clean HUDless (texture).
    // The patch is decoded into a local R10 texture; the consumer composes H' from it.
    if ((g_hudless_stream || (g_ctl->streams[4].ui_flags & mgpu::kHudlessFlagPatchPayload) != 0) &&
        g_ctl->streams[4].slot_size != 0) {
        const UINT hslot = g_hudless_slots ? (slot % g_hudless_slots) : slot;
        const mgpu::StreamShared& sh = g_ctl->streams[4];
        const bool patch4 = (sh.ui_flags & mgpu::kHudlessFlagPatchPayload) != 0;
        if (g_hudless[hslot] && sh.width >= 64 && sh.height >= 64) {
            if (patch4 && sh.codec != 0) {
                const DXGI_FORMAT fmt =
                    (sh.format == static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM))
                        ? DXGI_FORMAT_R10G10B10A2_UNORM
                        : DXGI_FORMAT_R8G8B8A8_UNORM;
                if (ensure_ui_sink_codec(g_hudless_local[hslot], sh.width, sh.height, fmt) &&
                    decode_ui_slot_from(g_hudless[hslot].Get(), hslot,
                                        g_hudless_local[hslot].Get(), sh.width, sh.height,
                                        fmt, true)) {
                    g_hudless_fresh[hslot] = true;
                    static bool told_p4 = false;
                    if (!told_p4) {
                        told_p4 = true;
                        logf_("[lock] stream 4: support patch %ux%u fmt %u (sparse)\n",
                              sh.width, sh.height, sh.format);
                    }
                }
            } else if (!patch4 && sh.codec == 0 &&
                       ensure_local_copy(g_hudless_local[slot], g_hudless[hslot].Get(),
                                         sh.width, sh.height)) {
                copy_content(g_hudless_local[slot].Get(), g_hudless[hslot].Get(),
                             sh.width, sh.height);
                g_hudless_fresh[slot] = true;
                static bool told_h = false;
                if (!told_h) {
                    told_h = true;
                    logf_("[lock] stream 4: clean HUDless %ux%u fmt %u\n",
                          sh.width, sh.height, sh.format);
                }
            }
        }
    }

}
// but with zero motion vectors and cleared depth, so the peer-access code path is
// completely absent. Interpolation then looks wrong, which is expected.
static void ensure_dummy_mv(UINT slot)
{
    if (slot >= mgpu::kMaxSlots) { return; }
    if (g_mv_local[slot]) { return; }

    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = g_ctl->streams[1].width;
    d.Height = g_ctl->streams[1].height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&g_mv_local[slot])))) {
        logf_("[lock] dummy MV resource FAILED\n");
        return;
    }
    if (!g_rtv_heap) { return; }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(5) * g_rtv_stride;      // slot 5: dummy MV
    D3D12_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_R16G16_FLOAT;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_device->CreateRenderTargetView(g_mv_local[slot].Get(), &rd, rtv);

    const float zeros[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = g_mv_local[slot].Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_list->ResourceBarrier(1, &b);
    g_list->ClearRenderTargetView(rtv, zeros, 0, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_list->ResourceBarrier(1, &b);
}

static void ensure_dummy_depth(UINT slot)
{
    if (slot >= mgpu::kMaxSlots) { return; }
    if (g_depth_local[slot]) { return; }
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = g_ctl->streams[2].width;
    d.Height = g_ctl->streams[2].height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R32_FLOAT;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;   // so it can be cleared
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&g_depth_local[slot])))) {
        logf_("[lock] dummy depth resource FAILED\n");
        return;
    }

    // Clear it once. A fresh committed resource is not guaranteed to be zeroed, and
    // uninitialised depth makes XeFG's disocclusion logic reject motion vectors at random -
    // a flat cleared depth behaves like "no depth", which is what this mode means.
    if (g_rtv_heap) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(4) * g_rtv_stride;   // slot 4: reserved for this
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = DXGI_FORMAT_R32_FLOAT;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_device->CreateRenderTargetView(g_depth_local[slot].Get(), &rd, rtv);

        const float zeros[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.pResource = g_depth_local[slot].Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_list->ResourceBarrier(1, &b);
        g_list->ClearRenderTargetView(rtv, zeros, 0, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        g_list->ResourceBarrier(1, &b);
    }
}

static void ensure_dummy_inputs(UINT slot)
{
    if (slot >= mgpu::kMaxSlots) { return; }
    const UINT w = g_ctl->streams[1].width;
    const UINT h = g_ctl->streams[1].height;

    auto make = [&](ComPtr<ID3D12Resource>& res, DXGI_FORMAT fmt) {
        if (res) { return; }
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1;
        hp.VisibleNodeMask = 1;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&res)))) {
            logf_("[lock] dummy input resource FAILED\n");
        }
    };
    make(g_mv_local[slot], DXGI_FORMAT_R16G16_FLOAT);
    ensure_dummy_depth(slot);
}

// The game (or Windows' fullscreen optimisation) pushes its own window above our
// topmost overlay when it is activated, which hides the frame-generated picture
// behind the game's own. Re-asserting the topmost position puts us back on top.
static ULONGLONG g_last_topmost = 0;
static bool g_zorder_logged = false;

static bool g_clickthrough_logged = false;
static int  g_click_probe = 0;
static ULONGLONG g_click_probe_ms = 0;

// Is that window one of ours (the overlay, the swap-chain window, a helper)?
static bool fg_is_ours(HWND fg)
{
    if (!fg) { return false; }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Is that window part of the shell's own UI (Task View / Alt+Tab / the desktop)?
//
// These take the foreground while the user is still looking at the game. Counting them as
// "the user left the game" made bridge quit after --exit-on-focus-loss seconds whenever
// Win+Tab, Alt+Tab or Win+D was used - which is exactly how a mirror tool must NOT behave.
// Real applications (another game, a browser, the desktop's own windows) are still
// counted, so the "don't sit on the desktop forever" protection keeps working.
static bool fg_is_shell_ui(HWND fg)
{
    if (!fg) { return false; }
    wchar_t cls[64]{};
    if (!GetClassNameW(fg, cls, 63)) { return false; }
    static const wchar_t* const kShellClasses[] = {
        L"Windows.UI.Core.CoreWindow",          // Task View (Win+Tab)
        L"MultitaskingViewFrame",               // Task View, older builds
        L"TaskSwitcherWnd",                     // Alt+Tab
        L"ForegroundStaging",                   // the transient window Alt+Tab/Win+Tab uses
        L"XamlExplorerHostIslandWindow",        // taskbar flyouts / Task View host
        L"Progman",                             // Win+D: "show desktop"
        L"WorkerW",                             // the desktop's wallpaper host
        L"Shell_TrayWnd",                       // the taskbar itself
    };
    for (const wchar_t* c : kShellClasses) {
        if (_wcsicmp(cls, c) == 0) { return true; }
    }
    return false;
}

// Is that window the launcher UI? Opening mgpu_ui.exe to change a setting must not
// count as "the user left the game", otherwise frame generation dies every time.
static bool fg_is_launcher(HWND fg)
{
    if (!fg) { return false; }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) { return false; }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { return false; }
    wchar_t path[MAX_PATH]{};
    DWORD len = MAX_PATH;
    bool match = false;
    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        match = _wcsicmp(base, L"mgpu_ui.exe") == 0;
    }
    CloseHandle(h);
    return match;
}

