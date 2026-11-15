// ============================================================================
//  consumer module: input_cache.cpp
//
//  Frame entry, retired inputs, ACK, shared-layout service, copy queue,
//  prefetch/FIFO.
//
//  Included by src/xess_mfg_consumer/unity.cpp as part of the unity build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

#include "../consumer_internal.h"
static void run_frame(ID3D11Texture2D* colour11)
{
    run_frame_ex(colour11);
}

// XeFG keeps a raw reference to whatever we tagged last on its own queue, so a texture
// we stop using must NOT be destroyed the moment the producer publishes a new content
// size - that removed the consumer's device (0x887A0005) the first time the size changed.
// Retire list: hold it for a few presents, far beyond any in-flight window.
static std::vector<ComPtr<ID3D12Resource>> g_retired_inputs;
static std::vector<UINT64> g_retired_at;
static const UINT64 kRetirePresents = 16;

static void retire_input(ComPtr<ID3D12Resource>& r)
{
    if (!r) { return; }
    g_retired_inputs.push_back(r);
    g_retired_at.push_back(g_present_id);
    r.Reset();
}

static void collect_retired_inputs()
{
    // Swap-remove instead of erase(): the vectors hold at most a handful of entries, but
    // this runs once per frame and the ordering of the retire list carries no meaning.
    size_t i = 0;
    while (i < g_retired_inputs.size()) {
        if (g_present_id - g_retired_at[i] >= kRetirePresents) {
            g_retired_inputs[i] = g_retired_inputs.back();
            g_retired_at[i] = g_retired_at.back();
            g_retired_inputs.pop_back();
            g_retired_at.pop_back();
        } else {
            ++i;
        }
    }
}

// ---- v10 credit window (P1) -------------------------------------------------
// We tell the producer which slot we are done reading, and only then may it overwrite it.
// "Done" = the GPU work that read those textures has completed, i.e. after gpu_throttle()
// waited for the *previous* frame's fence. The value we report is the publication stored in
// frames[slot].published (cross-adapter fences cannot be queried from this node, so the
// header field is the only way to name it - docs/findings/HANDOFF.md 8.6).
static UINT g_ack_slot = mgpu::kMaxSlots;
// v22: the publication this frame consumed. Written at the end of the frame, ACKed at the
// start of the next one - by then gpu_throttle() has waited for this frame's GPU work, so
// the producer may safely overwrite the slot.
static UINT     g_ack_pub_slot  = mgpu::kMaxSlots;
static uint64_t g_ack_pub_value = 0;

static void ack_publication_previous()
{
    if (!g_ctl || g_ack_pub_value == 0) { return; }
    // v22.1: watermark ACK. The old code ACKed only the slot this frame read, so the
    // producer's "first free slot from g_next_slot" always found THAT slot free and kept
    // reusing it: the ring collapsed to depth 1 and the consumer locked itself to ~8 fps
    // (measured: pick slot=0 forever, slots=[56 55 55 55]). A publication at or below the
    // one we just consumed can never be needed again, so acknowledge every such slot.
    // v23: order by the global publication sequence. `published` is a per-slot fence and
    // repeats across slots, so a watermark made of it both skipped publications and could
    // ACK a slot whose same-numbered publication had not been consumed yet.
    for (UINT s = 0; s < mgpu::kMaxSlots; ++s) {
        const uint64_t seq = g_ctl->frames[s].pub_seq;
        const uint64_t pub = g_ctl->frames[s].published;
        if (seq != 0 && seq <= g_ack_pub_value && pub != 0 && g_ctl->consumed[s] < pub) {
            MemoryBarrier();
            g_ctl->consumed[s] = pub;
        }
    }
    g_ack_pub_slot  = mgpu::kMaxSlots;
    g_ack_pub_value = 0;
}

static void ack_publication_record(UINT slot)
{
    if (!g_ctl || slot >= mgpu::kMaxSlots) { return; }
    g_ack_pub_slot  = slot;
    g_ack_pub_value = g_ctl->frames[slot].pub_seq;   // v23: global order watermark
}
// Publication value of the slot that frame actually used, snapshotted when the slot was
// selected. The old ACK path read frames[slot].published at ACK time, which may already
// name a NEWER publication the consumer never copied - the producer was then free to
// overwrite data still in flight. This is the value we really consumed.
static uint64_t g_ack_pub = 0;
// Defined here (used by the ACK below); the copy ring itself is set up further down.
static ComPtr<ID3D12Fence> g_copy_fence;

static void ack_previous_slot()
{
    if (!g_ctl) { return; }
    // 2026-11-15: acknowledge only publications whose LOCAL COPY has completed, and only
    // with the value that was copied. Never write the slot's current `published` for
    // slots we did not touch: that was an over-ACK (it told the producer "I have the
    // latest" for every idle slot) and it is what let a publication be overwritten
    // between the colour arriving and the copy being recorded.
    // 2026-11-15 flow control: a publication may be recycled only after its COLOUR was
    // consumed (consumer.published colour_seq), or after a timeout when WGC dropped that
    // colour (otherwise the producer would stall behind un-consumed slots). ACKing on
    // "copy retired" let the producer run arbitrarily ahead of the colour stream, which is
    // what produced the 43 fps `no_slot=332 / lag=14` runs.
    const double now_ms = qpc_ms();
    const uint64_t colour_seq = g_ctl->colour_seq;
    for (UINT k = 0; k < g_input_ring; ++k) {
        const uint64_t lp = g_local_pub[k];
        if (lp == 0) { continue; }
        const UINT prod = g_local_prod[k];
        if (prod >= mgpu::kMaxSlots) { continue; }
        if (lp <= g_ctl->consumed[prod]) { continue; }
        // The data lives in OUR ring now, so the transit slot may be reused as soon as the
        // copy fence retires. No need to wait for the colour.
        if (g_copy_fence && g_copy_fence->GetCompletedValue() >= g_local_fence[k]) {
            g_ctl->consumed[prod] = lp;
            ++g_ring_freed;
        }
    }
    g_ack_pub = 0;
}

// Drops our local (consumer-side) copies of the input streams. Only their *size* can
// change now: the placed views are fixed for the life of the process (protocol v9).
static void prefetch_invalidate();          // --prefetch-inputs, defined below
static void release_local_inputs()
{
    prefetch_invalidate();
    for (UINT s = 0; s < mgpu::kMaxSlots; ++s) {
        retire_input(g_mv_local[s]);
        retire_input(g_depth_local[s]);
        retire_input(g_depth_ring[s]);
#if MGPU_FG_OF
        // The flow pass caches a descriptor per ring slot; a rebuilt ring texture would
        // otherwise leave the pass reading a destroyed resource.
        g_of_depth_bound[s] = false;
#endif
    }
    g_ring_count = 0;
    g_ring_need  = 1;
    g_ring_write = 0;
    // A new geometry retires the local copies; the publication keys and the FIFO entries
    // describe those retired textures, so they must not survive the generation change.
    for (UINT s = 0; s < kInputRing; ++s) {
        g_local_pub[s]   = 0;
        g_local_fence[s] = 0;
        g_local_id[s]    = 0;
        g_local_t[s]     = 0.0;
    }
    pub_q_reset();
}

// Drops every view we placed inside the shared heap (plus the local copies).
// v9: only used on teardown / when the producer's block becomes invalid.
static void release_shared_views()
{
    prefetch_invalidate();
    for (UINT s = 0; s < mgpu::kMaxSlots; ++s) {
        g_mv[s].Reset();
        g_depth[s].Reset();
        g_scar[s].Reset();
        g_mv_local[s].Reset();
        g_depth_local[s].Reset();
        g_depth_ring[s].Reset();
        // g_fwd_expect[] is deliberately NOT reset: the producer keeps incrementing the
        // same fences, so zeroing our side would make us read slots nobody wrote.
    }
    g_ring_count = 0;
    g_ring_need  = 1;
    g_ring_write = 0;
    g_shared_slot = 0;
    // --upsample: the reconstruction targets belong to the old geometry, so a tag must not
    // reuse them after a generation change (a reuse frame would otherwise publish them).
    g_up_valid = false;
    g_up_slot  = mgpu::kMaxSlots;
    // copy-on-publication: the cache describes the old geometry/publications
    for (UINT s = 0; s < kInputRing; ++s) {
        g_local_pub[s]   = 0;
        g_local_fence[s] = 0;
        g_local_id[s]    = 0;
        g_local_t[s]     = 0.0;
    }
    pub_q_reset();
}

// v9: nobody rebuilds anything. `generation` only says "the live rectangle inside the
// slots changed size", which costs us one log line and a lazily re-created local copy -
// the placed views, the fences and the mapping all stay exactly as they are. `magic` is
// still checked because a *new* producer process would recreate the block; that is the
// only case where this frame has to be skipped.
static bool service_shared_layout()
{
    if (!g_ctl) { return true; }

    if (g_ctl->magic != mgpu::kSharedMagic) {
        if (!g_layout_pending) {
            g_layout_pending = true;
            logf_("[consumer] the producer's block is invalid (restarting?) - pausing\n");
            release_shared_views();
        }
        return false;
    }
    g_layout_pending = false;

    if (g_ctl->generation != g_seen_generation) {
        g_seen_generation = g_ctl->generation;
        // Content size changed: the local copies are the only things to rebuild, and
        // ensure_local_copy() does that on demand.
        release_local_inputs();
        logf_("[consumer] producer published gen %u: mv %ux%u, depth %ux%u (slots stay %ux%u)\n",
              g_seen_generation, g_ctl->streams[1].width, g_ctl->streams[1].height,
              g_ctl->streams[2].width, g_ctl->streams[2].height,
              mgpu::kStreamMaxW, mgpu::kStreamMaxH);
    }
    // The geometry fields only change together with `generation` (the producer bumps it
    // from update_content_size() on every width/height change), so re-reading the seqlock
    // every frame was a handful of uncached PCIe reads for nothing. The seqlock still
    // makes a *concurrent* change safe - it just no longer runs when nothing changed.
    // The `== 0` half keeps one edge case honest: if we attached before the producer
    // published any size, we keep looking until it does.
    if (g_ctl->generation != g_seen_geo_generation ||
        g_geo.mv_w == 0 || g_geo.depth_w == 0) {
        g_seen_geo_generation = g_ctl->generation;
        refresh_input_geo();
    }
    // The producer rewrites its input flags every frame, and the add-on can only learn
    // MVJittered once the game has created its DLSS feature - which may well be after the
    // bridge opened the mapping. Adopting a late flag still fixes the per-frame uses
    // (camera presence, depth sign), but the XeFG init flags were fixed by
    // InitFromSwapChainDesc, so say it out loud instead of silently testing the wrong thing.
    if (g_ctl->producer.input_flags != g_producer_flags) {
        const UINT before = g_producer_flags;
        g_producer_flags = g_ctl->producer.input_flags;
        if (((g_producer_flags & ~before) & mgpu::kInputFlagMvJittered) != 0) {
            logf_("[consumer] WARNING: the producer only now reported that its motion vectors"
                  " still carry the jitter. The XeFG swap chain was created before that, so"
                  " the flag did NOT take effect - restart the bridge after the game is"
                  " in-world, or pass --fg-jittered-mv\n");
        }
    }
    return true;
}

// ---------------- --input-copy-queue: MV/depth copies on a COPY queue ---------
// The frame's own list is a DIRECT list, and everything on it runs in order on the 3D
// engine: our colour convert draw, then the MV/depth copies that read host memory over
// PCIe (~14 MB/frame each at 1440p, 33 MB at 4K), and XeSS-FG's own work at Present().
// Those copies are pure transfers, so a COPY queue can move them on the copy/DMA engine
// *at the same time* as the convert draw instead of queueing behind it. Measured share of
// the loop: `mvs` + PCIe wait is the one stage that scales with the stream size, and
// docs/findings/PERF_BASELINE.md §3.3 puts 1440p at ~1.8 ms/frame and 4K at ~3.6 ms.
//
// Ordering, and why the tags do not use our list here:
//   copy queue: [mv+depth CopyTextureRegion] -> Signal(copy fence)
//   direct    : [our list: convert draw]      -> Wait(copy fence) -> Present() -> XeFG
// The Wait is queued *after* our list, so the two overlap; XeSS-FG's work is submitted at
// Present() and therefore starts only after the copies have landed. What makes that legal
// is that the tagged resources are no longer referenced by our list: their validity is
// XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT, and xefg_swapchain_d3d12.h says pCmdList "is only
// required if the resource is not valid until the next present call". Passing NULL keeps
// the copy queue and the direct queue from ever touching the same resource at once (the
// implicit COMMON promotion on each side would otherwise race). If XeFG starts refusing
// frames with this switch on, it needs the list after all - then the copies have to go
// back to the direct queue, i.e. drop the switch.
//
// Not used with the experimental modes: --no-inputs/--no-mv/--no-depth record clears
// (ClearRenderTargetView is illegal on a COPY list), --dump records readback copies in our
// own list that must be ordered after the input copies, and --fg-of owns depth through its
// own descriptor heap.
static bool g_input_copy_queue = true;      // --input-copy-queue (implied by prefetch)
static bool g_cq_reported = false;
static ComPtr<ID3D12CommandQueue>         g_copy_queue;
// Several allocators/lists so the prefetch never has to wait on the CPU for the previous
// batch. The old single-list version blocked in input_copy_begin() until the copy queue had
// finished the previous 28 MB batch; when the producer's fwd is late (render card at 100%)
// that wait grew to 17.5 ms and showed up as an unstable `gpu`/`present`. With a ring the
// copy queue orders the batches on the GPU; the CPU only waits if all slots are in flight.
static const UINT kCopyRing = 8;
static ComPtr<ID3D12CommandAllocator>     g_copy_alloc[kCopyRing];
static ComPtr<ID3D12GraphicsCommandList>  g_copy_list[kCopyRing];
static UINT64                             g_copy_list_fence[kCopyRing] = {};
static UINT                               g_copy_ring = 0;   // next slot to record
static UINT                               g_copy_cur = 0;    // slot being recorded now
static UINT64                             g_copy_value = 0;
static double                             g_ms_cq = 0.0;   // queueing the cross-queue wait
static double                             g_max_cq = 0.0;

static bool input_copy_queue_usable()
{
    if (!g_input_copy_queue) { return false; }
    if (g_no_inputs || g_no_mv || g_no_depth || g_dump_frames > 0) { return false; }
    // --no-throttle removes the fence wait that retires the previous frame's copy list, and
    // resetting a command allocator that is still executing is undefined behaviour. The
    // switch exists to measure the depth-1 cost, not to run with a second queue in flight.
    if (g_no_throttle) { return false; }
#if MGPU_FG_OF
    if (g_fg_of) { return false; }
#endif
    if (g_copy_list[0]) { return true; }
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    bool ok = SUCCEEDED(g_device->CreateCommandQueue(&q, IID_PPV_ARGS(&g_copy_queue)));
    for (UINT i = 0; ok && i < kCopyRing; ++i) {
        ok = SUCCEEDED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                       IID_PPV_ARGS(&g_copy_alloc[i]))) &&
             SUCCEEDED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
                                                   g_copy_alloc[i].Get(), nullptr,
                                                   IID_PPV_ARGS(&g_copy_list[i]))) &&
             SUCCEEDED(g_copy_list[i]->Close());
        g_copy_list_fence[i] = 0;
    }
    if (ok) {
        ok = SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                             IID_PPV_ARGS(&g_copy_fence)));
    }
    if (!ok) {
        logf_("[consumer] --input-copy-queue: copy queue FAILED - falling back to the direct"
              " queue (the MV/depth copies then queue behind the convert draw)\n");
        g_copy_queue.Reset();
        for (UINT i = 0; i < kCopyRing; ++i) {
            g_copy_alloc[i].Reset();
            g_copy_list[i].Reset();
            g_copy_list_fence[i] = 0;
        }
        g_copy_fence.Reset();
        g_copy_ring = 0;
        g_copy_cur = 0;
        g_input_copy_queue = false;
        return false;
    }
    g_copy_ring = 0;
    g_copy_cur = 0;
    return true;
}

// Re-opens the copy list and points copy_content() at it. Returns false when the copies
// must stay on the frame's own list.
static bool input_copy_begin()
{
    if (!input_copy_queue_usable()) { return false; }
    if (!g_cq_reported) {
        g_cq_reported = true;
        logf_("[consumer] --input-copy-queue: MV/depth copies go on a COPY queue and overlap"
              " the colour convert draw (tagged without our command list, see the switch)\n");
    }
    // v23.2: never block the presenter on the copy ring. Pick a slot whose batch has
    // already retired; if every slot is still in flight, fall back to this frame's own
    // list. The old code waited up to 50 ms for the next ring slot and showed up as
    // `SLOW frame colour=59.6` / HITCH 53-76 ms whenever the FG card was near 100%.
    UINT i = g_copy_ring;
    bool found = false;
    for (UINT n = 0; n < kCopyRing; ++n) {
        const UINT cand = (g_copy_ring + n) % kCopyRing;
        const UINT64 f = g_copy_list_fence[cand];
        if (f == 0 || !g_copy_fence || g_copy_fence->GetCompletedValue() >= f) {
            i = cand;
            found = true;
            break;
        }
    }
    if (!found) {
        static UINT full = 0;
        if (++full <= 3 || (full % 300u) == 0u) {
            logf_("[consumer] input-copy-queue: all %u ring slots still in flight -"
                  " recording this frame's copies on the frame list (%u so far)\n",
                  kCopyRing, full);
        }
        return false;
    }
    g_copy_cur = i;
    g_copy_ring = (i + 1) % kCopyRing;
    g_copy_alloc[i]->Reset();
    g_copy_list[i]->Reset(g_copy_alloc[i].Get(), nullptr);
    g_copy_target = g_copy_list[i].Get();
    return true;
}

// Closes and submits the copy list. Nothing else may record into it afterwards, so the
// redirection is cleared here even on the error paths.
static void input_copy_submit(bool worked)
{
    ID3D12GraphicsCommandList* list = g_copy_target;
    g_copy_target = nullptr;
    if (!worked || !list) {
        if (list) { list->Close(); }   // keep the allocator reset-safe next time around
        return;
    }
    const UINT i = g_copy_cur;
    list->Close();
    ID3D12CommandList* lists[] = { list };
    g_copy_queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++g_copy_value;
    g_copy_queue->Signal(g_copy_fence.Get(), v);
    g_copy_list_fence[i] = v;
}

// Queues the cross-queue wait on the direct queue. Must be called *after* our own list has
// been submitted (so the convert draw and the copies overlap) and *before* Present(), which
// is when XeSS-FG's work enters the queue and starts reading the tagged resources.
static void input_copy_wait()
{
    const double t0 = qpc_ms();
    g_queue->Wait(g_copy_fence.Get(), g_copy_value);
    const double dt = qpc_ms() - t0;
    g_ms_cq = g_ms_cq * 0.9 + dt * 0.1;
    if (dt > g_max_cq) { g_max_cq = dt; }
}

// v22: copy one publication's colour + MV/depth. The colour, the vectors and the depth
// live in the SAME producer slot, so one call covers the whole frame; the copy queue
// waits for that slot's fence on the GPU. Returns true when the copies are on the copy
// queue (the caller must call input_copy_wait() before Present); false means the caller
// has to record them on its own list.
static bool copy_publication_inputs(UINT slot)
{
    if (!g_ctl || slot >= mgpu::kMaxSlots) { return false; }
    if (!input_copy_begin()) { return false; }
    const double t0 = qpc_ms();
    const uint64_t published = g_ctl->frames[slot].published;
    if (g_fwd[slot] && published != 0) {
        g_copy_queue->Wait(g_fwd[slot].Get(), published);
    }
    const UINT before = g_copy_count;
    copy_inputs_local(slot);
    const bool recorded = (g_copy_count != before);
    input_copy_submit(recorded);
    const double dt = qpc_ms() - t0;
    g_ms_colour = g_ms_colour * 0.9 + dt * 0.1;
    if (dt > g_max_colour) { g_max_colour = dt; }
    return recorded;
}

// ---------------------- --prefetch-inputs: copy one frame ahead ---------------
// The round-robin slot walk makes the *next* frame's slot known while the current one is
// still being presented, so its MV/depth can be copied during that Present() block instead
// of at the start of the next frame. Two things make this worth it and one makes it safe:
//
//   worth it : the copies are 28 MB/frame of PCIe *read* at 1440p (63 MB at 4K). Measured
//              with --input-copy-queue, overlapping them with the convert draw already
//              bought ~1 ms; here they leave the frame entirely.
//   safe     : the copy runs on the COPY queue and the producer's cross-adapter fence is
//              waited on *on that queue* (GPU side, no CPU polling), so the copy cannot
//              read a slot the producer has not finished publishing. That is stricter than
//              the main path, which never waits for the producer at all.
//   --depth-lag still works: copy_inputs_local() advances g_ring_write here, which is
//              exactly the entry the next frame's tag picks up (nothing advances it in
//              between because the frame's own copy stage is skipped on a prefetch hit).
//
// Requires --input-copy-queue (it is implied). Off by default: it changes which frame each
// slot is read for, and that is the kind of change that has to be seen in a real run.
// Default ON since 2026-11-14: this is the normal path now, not an experiment. The copy
// for the *next* slot is recorded during this frame's Present, on the copy queue, and the
// queue waits for the producer's fwd on the GPU. The CPU then blocks in TryGetNextFrame,
// so the transfer runs in parallel with WGC; when the colour arrives the bridge only does
// a local lookup (pair_slot_for_wgc) and uses the copy already in VRAM. A miss falls back
// to an in-frame copy (and is counted in `pf: hits/misses`).
// --no-prefetch-inputs restores the old "copy after WGC" order.
// --prefetch-inputs / --no-prefetch-inputs
static bool   g_prefetch_inputs = true;
static bool   g_pf_valid = false;           // a prefetch is pending on the copy queue
static UINT   g_pf_slot = 0;                // ... for this slot
static UINT   g_pf_gen = 0xFFFFFFFFu;       // ... with this producer geometry generation
static UINT64 g_pf_fence = 0;               // copy-fence value that retires it
static UINT   g_pf_hits = 0;                // frames served from a prefetch
static UINT   g_pf_misses = 0;              // frames that had to copy in-frame
static bool   g_pf_reported = false;

static bool prefetch_usable()
{
    return g_prefetch_inputs && !g_reuse_frame && input_copy_queue_usable();
}

static void prefetch_invalidate()
{
    g_pf_valid = false;
    g_pf_gen   = 0xFFFFFFFFu;
}

// Queues the cross-queue wait for a *prefetched* copy. Same position as input_copy_wait()
// (after our list, before Present), but it waits on the value the prefetch signalled.
static void prefetch_wait()
{
    const double t0 = qpc_ms();
    g_queue->Wait(g_copy_fence.Get(), g_pf_fence);
    const double dt = qpc_ms() - t0;
    g_ms_cq = g_ms_cq * 0.9 + dt * 0.1;
    if (dt > g_max_cq) { g_max_cq = dt; }
}

// Records + submits the copies for `next_slot` and leaves them running while this frame is
// presented. Called once per frame, after Present + Signal, so nothing it does can delay
// the frame that is already on its way to the panel.
static void prefetch_next_inputs(UINT next_slot)
{
    if (g_pf_pub) { return; }       // copy-on-publication owns the local cache
    if (!prefetch_usable()) { return; }
    if (!g_ctl || next_slot >= mgpu::kMaxSlots) { return; }
    if (!g_fwd[next_slot]) { return; }                 // no fence: cannot order it
    const uint64_t published = g_ctl->frames[next_slot].published;
    const uint64_t copy_id   = g_ctl->frames[next_slot].frame_id;
    if (published == 0) { return; }                    // the producer never filled it
    if (!input_copy_begin()) { return; }
    if (!g_cq_reported) { g_cq_reported = true; }
    if (!g_pf_reported) {
        g_pf_reported = true;
        logf_("[consumer] --prefetch-inputs: MV/depth for the next slot are copied during this"
              " frame's Present (copy queue waits for the producer's fence on %u)\n", next_slot);
    }
    // GPU-side wait for the producer's publication of that slot - the only place in the
    // main path where our reads are ordered against the producer at all.
    g_copy_queue->Wait(g_fwd[next_slot].Get(), published);
    const UINT before = g_copy_count;
    copy_inputs_local(next_slot);
    const bool recorded = (g_copy_count != before);
    input_copy_submit(recorded);
    if (!recorded) { return; }
    g_local_id[next_slot] = copy_id;
    g_local_t[next_slot]  = qpc_ms();
    // This path is a cache entry too: ACK uses g_local_pub + the copy fence, so without
    // these the credit window would never advance with --no-prefetch-publications.
    g_local_pub[next_slot]   = published;
    g_local_fence[next_slot] = g_copy_value;
    g_pf_valid = true;
    g_pf_slot  = next_slot;
    g_pf_gen   = g_ctl->generation;
    g_pf_fence = g_copy_value;
}

// ---------------------- copy-on-publication (100% hit) ------------------------
// Copies every new producer publication into this slot's local cache, on the copy queue,
// after a GPU-side wait on that slot's fwd fence. Called from the capture loop while it
// is polling for WGC (idle Sleep(1)), i.e. exactly during the producer's publication
// window. No guess: the slot is read from frames[s].published.
// Returns true only when BOTH streams were recorded. A partial copy must not be published
// as a cache entry: the frame tags MV and depth together, so a stale half would pair the
// new MV with an older depth. The old code gated the whole batch on one counter, which a
// single failing slot could keep at zero (observed: pf hits 3-9/40 with the 100% cache).
static bool copy_slot_streams(UINT s, UINT ring)
{
    if (s >= mgpu::kMaxSlots) { return false; }
    bool mv_ok = false, depth_ok = false;
    if (g_mv[s] && g_geo.mv_w >= 256 && g_geo.mv_h >= 256 &&
        ensure_local_copy(g_mv_local[ring], g_mv[s].Get(), g_geo.mv_w, g_geo.mv_h)) {
        copy_content(g_mv_local[ring].Get(), g_mv[s].Get(), g_geo.mv_w, g_geo.mv_h);
        mv_ok = true;
    }
    if (g_depth[s] && g_geo.depth_w >= 256 && g_geo.depth_h >= 256 &&
        ensure_local_copy(g_depth_local[ring], g_depth[s].Get(), g_geo.depth_w, g_geo.depth_h)) {
        copy_content(g_depth_local[ring].Get(), g_depth[s].Get(), g_geo.depth_w, g_geo.depth_h);
        depth_ok = true;
    }
    if (!mv_ok || !depth_ok) {
        static int told = 0;
        if (told < 4) {
            ++told;
            logf_("[consumer] pf: slot %u copy FAILED (mv=%d depth=%d; mv %p %ux%u, depth %p %ux%u"
                  " fmt=%d, local mv=%p depth=%p)\n",
                  s, mv_ok ? 1 : 0, depth_ok ? 1 : 0,
                  static_cast<void*>(g_mv[s].Get()), g_geo.mv_w, g_geo.mv_h,
                  static_cast<void*>(g_depth[s].Get()), g_geo.depth_w, g_geo.depth_h,
                  g_depth[s] ? static_cast<int>(g_depth[s]->GetDesc().Format) : -1,
                  static_cast<void*>(g_mv_local[s].Get()),
                  static_cast<void*>(g_depth_local[s].Get()));
        }
    }
    return mv_ok && depth_ok;
}

static void prefetch_publications()
{
    if (!g_pf_pub || !g_ctl) { return; }
    // Deliberately NOT gated on g_reuse_frame: a reused colour does not consume the new
    // publications, but the NEXT colour will, and stopping here during/after a stall was
    // one of the ways the cache ran empty (misses then arrived in bursts).
    UINT     ss[mgpu::kMaxSlots] = {};
    UINT     rr[mgpu::kMaxSlots] = {};
    uint64_t pp[mgpu::kMaxSlots] = {};
    uint64_t ids[mgpu::kMaxSlots] = {};
    UINT n = 0;
    for (UINT s = 0; s < mgpu::kMaxSlots; ++s) {
        const uint64_t pub = g_ctl->frames[s].published;
        if (pub == 0 || pub == g_prod_seen[s] || !g_fwd[s]) { continue; }
        if (!g_mv[s] || !g_depth[s]) { continue; }       // both streams must exist
        const DXGI_FORMAT df = g_depth[s]->GetDesc().Format;
        if (df == DXGI_FORMAT_R32G8X24_TYPELESS ||
            df == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
            continue;                                 // wide depth: this slot uses the old path
        }
        ss[n] = s;
        pp[n] = pub;
        // Snapshot the record id with the publication value. Reading it after the
        // copies below can pick up the producer's NEXT publication in the same slot,
        // which made pub_q_pop_seq(seq) miss and fall back to a wrong queue head.
        ids[n] = g_ctl->frames[s].frame_id;
        ++n;
    }
    if (n == 0) { return; }
    // Publication order, not slot order: the producer publishes round-robin, so a plain
    // 0..3 scan can hand the FIFO 100,101,98,99. The queue must be strictly increasing.
    for (UINT i = 1; i < n; ++i) {
        const UINT s = ss[i];
        const uint64_t p = pp[i];
        const uint64_t id = ids[i];
        UINT j = i;
        while (j > 0 && pp[j - 1] > p) {
            ss[j] = ss[j - 1];
            pp[j] = pp[j - 1];
            ids[j] = ids[j - 1];
            --j;
        }
        ss[j] = s;
        pp[j] = p;
        ids[j] = id;
    }
    if (!input_copy_begin()) { return; }
    if (!g_cq_reported) {
        g_cq_reported = true;
        logf_("[consumer] --prefetch-publications: copying every new publication into the"
              " local cache (copy queue waits on fwd)\n");
    }
    const UINT before = g_copy_count;
    bool ok[mgpu::kMaxSlots] = {};
    for (UINT i = 0; i < n; ++i) {
        g_copy_queue->Wait(g_fwd[ss[i]].Get(), pp[i]);    // GPU-side publication wait
        rr[i] = g_ring_next % g_input_ring;
        g_ring_next = (rr[i] + 1u) % g_input_ring;
        ok[i] = copy_slot_streams(ss[i], rr[i]);
    }
    const bool recorded = (g_copy_count != before);
    input_copy_submit(recorded);
    if (!recorded) { return; }
    // Publish a cache entry only for the slots that copied BOTH streams. A slot whose copy
    // failed keeps its previous key, so the frame falls back instead of pairing a new half
    // with an old one.
    for (UINT i = 0; i < n; ++i) {
        if (!ok[i]) { continue; }
        g_local_pub[rr[i]]   = pp[i];
        g_local_fence[rr[i]] = g_copy_value;
        g_local_id[rr[i]]    = ids[i];
        g_local_t[rr[i]]     = qpc_ms();
        g_local_prod[rr[i]]  = ss[i];
        g_local_qpc[rr[i]]   = g_ctl->frames[ss[i]].frame_qpc_ms;
        g_prod_seen[ss[i]]   = pp[i];
        if (g_pub_fifo) {
            pub_q_push(pp[i], rr[i], g_copy_value, g_ctl->generation, ids[i]);
        }
    }
}

// Arm the producer's shared fwd fences for the NEXT publication of each slot. The shared
// fence's GetCompletedValue reads UINT64_MAX on this node (see wait_slot), so the armed
// value is tracked explicitly. SetEventOnCompletion is already used by wait_slot on these
// fences, so it is known to work across the adapter.
static void arm_pub_events()
{
    if (!g_pub_event || !g_pub_event_ok || !g_ctl) { return; }
    static LONGLONG freq = 0;
    if (freq == 0) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    for (UINT s = 0; s < mgpu::kMaxSlots; ++s) {
        if (!g_fwd[s]) { continue; }
        const uint64_t want = g_ctl->frames[s].published + 1;
        if (want == g_pub_armed[s]) { continue; }
        g_pub_armed[s] = want;
        LARGE_INTEGER a{}, b{};
        QueryPerformanceCounter(&a);
        const HRESULT hr = g_fwd[s]->SetEventOnCompletion(want, g_pub_event);
        QueryPerformanceCounter(&b);
        if (freq > 0) {
            g_arm_ms += static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 /
                        static_cast<double>(freq);
        }
        ++g_arm_calls;
        if (FAILED(hr)) {
            g_pub_event_ok = false;
            logf_("[consumer] M1: SetEventOnCompletion FAILED - publication poll fallback\n");
            return;
        }
    }
}


