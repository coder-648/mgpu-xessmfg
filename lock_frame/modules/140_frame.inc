// ============================================================================
//  lock_frame module: 140_frame.inc
//
//  run_frame_ex: the per-WGC-frame recording path.
//
//  Included by lock_frame/lock_frame.cpp as part of the single-translation-unit build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

// Case-insensitive "does the name end with this suffix" - used by the Unreal MV-source policy
// in the game-window setup (an Unreal shipping build is <Project>-Win64-Shipping.exe).
static bool wcs_ends_with_ci(const wchar_t* s, const wchar_t* suffix)
{
    if (!s || !suffix) { return false; }
    const size_t ls = wcslen(s), lf = wcslen(suffix);
    return ls >= lf && _wcsicmp(s + (ls - lf), suffix) == 0;
}

// 加载/菜单/暂停时没有新的发布，fakerFG 仍以 120 Hz 重画同一帧的 marker。
// 这些帧按 seq12 去重后不再 Present；如果覆盖窗口一直留着，用户会看到冻结的
// 旧画面。所以超过 250 ms 没有新发布就把覆盖窗口隐藏，让游戏自己的窗口
// （菜单/加载画面）露出来；第一条新发布到达时再显示。
static uint64_t g_lock_seen_pub = 0;
static double   g_lock_last_pub_change_ms = -1.0;
static bool     g_lock_overlay_hidden = false;

static void lock_overlay_stall_gate()
{
    if (g_ctl == nullptr) { return; }
    // frame_published is written by the producer for every successful publication and is
    // monotonic in the game's frame sequence. Scanning frames[].published (a per-slot
    // fence value) produced false "no publication" windows on PBZ while pairing was
    // still exact=40/40; hiding the overlay there made FG drop for 250 ms (visible
    // flicker). Use the publication counter that the watchdog already trusts.
    const uint64_t newest = g_ctl->frame_published;
    if (newest == 0) { return; }
    const double now = qpc_ms();
    if (newest != g_lock_seen_pub) {
        g_lock_seen_pub = newest;
        g_lock_last_pub_change_ms = now;
        if (g_lock_overlay_hidden && g_bridge_wnd != nullptr) {
            ShowWindow(g_bridge_wnd, SW_SHOWNOACTIVATE);
            g_lock_overlay_hidden = false;
            logf_("[lock] new publication -> overlay shown, FG resumes\n");
        }
        return;
    }
    if (!g_lock_overlay_hidden && g_lock_last_pub_change_ms > 0.0 &&
        now - g_lock_last_pub_change_ms > 250.0 && g_bridge_wnd != nullptr) {
        uint64_t slot_newest = 0;
        const UINT sc = (g_ctl->slots != 0) ? g_ctl->slots : 1;
        for (UINT s = 0; s < sc && s < mgpu::kMaxSlots; ++s) {
            if (g_ctl->frames[s].published > slot_newest) {
                slot_newest = g_ctl->frames[s].published;
            }
        }
        ShowWindow(g_bridge_wnd, SW_HIDE);
        g_lock_overlay_hidden = true;
        logf_("[lock] no new publication for 250 ms -> overlay hidden (menu/loading)"
              " (frame_published=%llu slot_published=%llu)\n",
              (unsigned long long)newest, (unsigned long long)slot_newest);
    }
}

// Rebuild the marker's FULL frame id near the producer's newest publication.
//
// The marker only carries seq12 and ms12. Deriving a forward distance with
// `(marker_seq12 - newest_seq12) & 0xFFF` makes a frame that is two publications
// BEHIND look like it is 4094 ahead. It also makes the ack jump to `newest` and
// lets the producer drop the frames the colour is actually waiting for. Pick the
// candidate (same 4K block, previous, next) closest to `newest` instead, and
// acknowledge exactly that frame; the producer already knows how to hold from it.
static void lock_reanchor_colour_watermark(uint32_t payload)
{
    if (g_ctl == nullptr) { return; }
    uint64_t newest = 0;
    const UINT sc = (g_ctl->slots != 0) ? g_ctl->slots : 1;
    for (UINT s = 0; s < sc && s < mgpu::kMaxSlots; ++s) {
        if (g_ctl->frames[s].frame_id > newest) {
            newest = g_ctl->frames[s].frame_id;
        }
    }
    if (newest == 0) { return; }

    const uint32_t seq12 = mgpu::marker_seq_from_payload(payload);
    const uint64_t block = newest & ~0xFFFull;
    uint64_t best = 0;
    int64_t best_delta = 0x7FFFFFFFFFFFFFFFll;
    for (int step = -1; step <= 1; ++step) {
        const int64_t cand = static_cast<int64_t>(block) +
                             static_cast<int64_t>(step) * 4096 +
                             static_cast<int64_t>(seq12);
        if (cand <= 0) { continue; }
        const int64_t delta = cand - static_cast<int64_t>(newest);
        const int64_t mag = (delta < 0) ? -delta : delta;
        if (mag < best_delta) {
            best_delta = mag;
            best = static_cast<uint64_t>(cand);
        }
    }
    // A marker this far from the newest publication is not a useful watermark;
    // it is a stale/duplicate WGC frame. Leave the current ack alone.
    if (best == 0 || best_delta > 512) { return; }
    // A fresh consumer starts after the producer has already counted records. Its
    // marker seq and the producer's frame_id can therefore be offset by > kMaxSlots,
    // and a stale colour_seq left by a previous consumer can be ABOVE the new
    // producer's newest id. In that case the normal `best > colour_seq` rule can never
    // move the ack down, so the producer skips every frame forever. Detect that
    // condition explicitly and allow the marker's true frame to reset the ack.
    const bool ack_far_ahead = (g_ctl->colour_seq > newest) &&
                               (g_ctl->colour_seq - newest > mgpu::kMaxSlots);
    if (best != 0 && (best > g_ctl->colour_seq ||
                      (ack_far_ahead && best < g_ctl->colour_seq))) {
        static UINT told = 0;
        if (told < 12) {
            ++told;
            logf_("[lock] pair miss: re-anchor watermark %llu -> %llu"
                  " (marker_seq12=%u newest=%llu delta=%lld%s)\n",
                  (unsigned long long)g_ctl->colour_seq, (unsigned long long)best,
                  seq12, (unsigned long long)newest,
                  static_cast<long long>(best_delta),
                  ack_far_ahead ? ", stale ack" : "");
        }
        g_ctl->colour_seq = best;
    }
}

// ============================================================================
//  v29: game-clock cadence + slot decimation (the default path)
//
//  Why: WGC's arrival timestamp (SystemRelativeTime) is quantised to whole vblanks, so a
//  42 fps input and a 39 fps input both read as "3 vblanks" (25 ms). The producer publishes
//  the game's own present clock in frames[].frame_qpc_ms (1 ms resolution); one sample per
//  colour frame is enough to measure what the game actually did.
//
//  Two consumers:
//    * frameRenderTime (040_cadence.inc) - the gap between ACCEPTED frames, so a dropped
//      frame is reported as the 2T gap it really is and XeFG spreads its batch over it.
//    * the leaky-bucket gate below - drops the surplus before any copy, so the forwarded
//      rate is panel/(interp+1) and the panel is not left to DWM's mercy.
// ============================================================================
static double game_raw_ms() { return (g_game_last_ms > 0.5) ? g_game_last_ms : 0.0; }
static double game_ema_ms() { return (g_game_acc_ema > 0.5) ? g_game_acc_ema : 0.0; }

static double game_median_ms()
{
    if (g_game_ring_n == 0) { return 0.0; }
    double v[16];
    for (UINT i = 0; i < g_game_ring_n; ++i) { v[i] = g_game_ring[i]; }
    for (UINT i = 1; i < g_game_ring_n; ++i) {
        const double k = v[i];
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; --j; }
        v[j + 1] = k;
    }
    return v[g_game_ring_n / 2];
}

// 1 = the rate is moving (raw tracks it better), 0 = steady (quant removes the +-1 vblank
// jitter), -1 = not enough samples yet (caller falls back to the WGC-based decision).
static int game_rate_moving()
{
    if (g_game_in_fast <= 0.0 || g_game_in_slow <= 0.0) { return -1; }
    const double vb = refresh_ms();
    if (!(vb > 0.5)) { return -1; }
    return (dabs_(g_game_in_fast - g_game_in_slow) > 0.25 * vb) ? 1 : 0;
}

// One sample per incoming colour frame (accepted or dropped): the input rate.
static void note_game_input(uint64_t t)
{
    if (t == 0) { return; }
    double dt = 0.0;
    if (g_game_prev_qpc != 0 && t > g_game_prev_qpc) {
        dt = static_cast<double>(t - g_game_prev_qpc);
        // 1 ms is the field's resolution; anything bigger than 200 ms is a stall, not
        // a cadence sample (loading / alt-tab / a pair miss).
        if (dt < 1.0 || dt > 200.0) { dt = 0.0; }
    }
    g_game_prev_qpc = t;
    if (dt <= 0.0) { return; }
    if (g_game_in_ema <= 0.0) { g_game_in_ema = dt; }
    else { g_game_in_ema = g_game_in_ema * 0.95 + dt * 0.05; }        // ~20 frames
    if (g_game_in_fast <= 0.0 || g_game_in_slow <= 0.0) {
        g_game_in_fast = dt;
        g_game_in_slow = dt;
    } else {
        g_game_in_fast = g_game_in_fast * 0.70 + dt * 0.30;
        g_game_in_slow = g_game_in_slow * 0.95 + dt * 0.05;
    }
}

// Accepted frame: the accepted-to-accepted gap. This is the interval frameRenderTime must
// describe - when the gate dropped the frame in between it is 2T, and XeFG has to spread
// its m outputs over that, not over T.
static void note_game_accepted(uint64_t t)
{
    if (t == 0) { return; }
    double dt = 0.0;
    if (g_game_prev_acc_qpc != 0 && t > g_game_prev_acc_qpc) {
        dt = static_cast<double>(t - g_game_prev_acc_qpc);
        if (dt < 1.0 || dt > 400.0) { dt = 0.0; }
    }
    g_game_prev_acc_qpc = t;
    if (dt <= 0.0) { return; }
    g_game_last_ms = dt;
    g_game_acc_ema = (g_game_acc_ema > 0.5) ? (g_game_acc_ema * 0.9 + dt * 0.1) : dt;
    g_game_ring[g_game_ring_i] = dt;
    g_game_ring_i = (g_game_ring_i + 1) % 16;
    if (g_game_ring_n < 16) { ++g_game_ring_n; }
    const double vb = refresh_ms();
    if (vb > 0.5) {
        int k = static_cast<int>(dt / vb * 2.0 + 0.5);   // 0.5-vblank buckets
        if (k < 1) { k = 1; }
        if (k > 16) { k = 16; }
        ++g_in_step_hist[k];
    }
}

// True when this colour frame must not be used: keeping it would push the forwarded rate
// above panel/(interp+1). A leaky bucket spreads the drops (1 in 21 at 42 fps, never a
// burst), which is what keeps the content steps even.
// Another limiter owns the input when --lock or --pace is set: the gate then stays out.
static bool slot_gate_drop()
{
    if (!g_slot_gate || g_ctl == nullptr) { return false; }
    // Another owner: an explicit limiter, or a controller that already picks the count
    // per frame (a gate on top would fight it - it drops inputs the controller asked for).
    if (g_lock_fps > 1.0 || g_pace_ms > 0.0) { return false; }
    if (g_interp_cadence || g_interp_auto || g_dyn_mfg) { return false; }
    // No generated frames to absorb the drop: dropping would lower the shown rate.
    if (g_no_fg || g_fg_warp || g_fg_interp_actual < 1) { return false; }
    const int m = static_cast<int>(g_fg_interp_actual) + 1;
    if (m < 2 || g_game_in_ema < 1.0) { return false; }
    const double vb = refresh_ms();
    if (!(vb > 0.5)) { return false; }
    const double f_in = 1000.0 / g_game_in_ema;                    // measured input rate
    const double target = 1000.0 / (vb * static_cast<double>(m));  // panel / m
    double q = target / f_in;
    if (q > 1.0) { q = 1.0; }        // under-supplied: keep everything
    if (q < 0.2) { q = 0.2; }        // never drop more than 4 of 5
    // A 2% overshoot is not worth a dropped frame. The measured working point of this title
    // is 39-40 fps against a 40 fps target, where the interval jitter already produces 2.5/
    // 3.5 vblank batches; one drop there costs a full 2x content gap for nothing. Only start
    // decimating when the surplus is real.
    if (q > 0.98) { g_slot_credit = 1.0; return false; }
    g_slot_credit += q;
    if (g_slot_credit < 1.0) { return true; }
    g_slot_credit -= 1.0;
    if (g_slot_credit > 2.0) { g_slot_credit = 2.0; }
    return false;
}

static void run_frame_ex(ID3D11Texture2D* colour11)
{
    if (!service_shared_layout()) { return; }
    // --run-sec: a measurement run has a fixed length and exits by itself.
    if (g_run_sec != 0 && g_t0 > 0.0 &&
        (static_cast<double>(GetTickCount64()) / 1000.0) - g_t0 >=
            static_cast<double>(g_run_sec)) {
        logf_("[lock] --run-sec %u reached - exiting for the next measurement segment\n",
              g_run_sec);
        hard_exit("--run-sec reached");
    }
    collect_retired_inputs();
    probe_colour_black();
    lock_overlay_stall_gate();

#if MGPU_FG_OF
    // The flow field is a per-frame output: never let a failed dispatch inherit the
    // previous frame's "fresh" flag.
    g_of_fresh = false;
    // Resolve the vector source BEFORE pairing. A depth-only title must be able to
    // initialize optical flow even if the very first marker pair fails; the old code
    // only did this inside the pair-success branch, so a no-slot start could never
    // reach OF and stayed at 1X forever.
    if (!g_reuse_frame && colour11 != nullptr && g_ctl != nullptr) {
        if (g_mv_auto) {
            const bool known = g_ctl->frame_published != 0;
            if (known) {
                g_auto_use_mv = !g_fg_of && producer_has_mv_stream();
            }
            static int8_t told_mv_src = -1;
            const int8_t now_src = g_auto_use_mv ? 1 : 0;
            if (known && now_src != told_mv_src) {
                told_mv_src = now_src;
                logf_("[lock] mv: auto -> %s (active_mask=0x%X mv slot %ux%u%s,"
                      " input_flags=0x%X)\n",
                      g_auto_use_mv ? "the producer's motion vectors"
                                    : (g_fg_of
                                           ? "optical flow (forced: Unreal/no-MV policy)"
                                           : "optical flow / zeroed (no MV received)"),
                      g_ctl->active_mask,
                      g_ctl->streams[mgpu::kStreamMv].width,
                      g_ctl->streams[mgpu::kStreamMv].height,
                      producer_has_mv_stream() ? "" : " (no MV data)",
                      g_ctl->producer.input_flags);
            }
        }
        g_of_active = mv_from_flow();
        // A real depth stream is required: g_of_mv is created at the depth stream size
        // (XeFG rejects a MV/depth size mismatch). If the layout is not live yet, retry
        // on the next colour frame instead of freezing g_of_out_w/h at the capture size.
        if (g_of_active && !g_of_ready && g_ctl->frame_published != 0 &&
            g_ctl->streams[mgpu::kStreamDepth].width >= 256) {
            if (!of_init()) {
                static bool told_of_fail = false;
                if (!told_of_fail) {
                    told_of_fail = true;
                    logf_("[lock] of: initialization FAILED - no vector source this session\n");
                }
            }
        }
    }
#endif

    // 0. 身份：先从这一帧 WGC 颜色的四角 marker 解出 seq12+ms12，再到 producer 的
    //    发布里精确匹配。matched == false 时本帧只上屏真实颜色（1X），不打 tag。
    bool pair_ok = g_reuse_frame;           // 复用帧沿用上一帧已验证的 slot
    bool fallback_pair = false;             // reset/degraded: current colour + last good slot + OF
    int  pair_slot = -1;
    int  pair_drain_slot = -1;              // 未配对时用来解除 producer 的槽位阻塞
    if (!g_reuse_frame && colour11 != nullptr && g_ctl != nullptr) {
        const LockPairResult lp = lock_pair_colour(colour11);
        // Loading/menu/pause：同一帧身份被 WGC 重复交付。直接丢掉，不 Present，
        // 也不参与命中率统计（记在 pair: 的 dup 里）。
        if (lp.duplicate) { return; }
        // Every real pairing attempt contributes to the sliding health window. A
        // duplicate is not a new attempt, so it is deliberately excluded. When the
        // producer has published nothing at all (depth-only lane not feeding us yet),
        // that is a transport problem, not a marker mismatch: do not poison the health
        // window and do not trigger a session reset loop.
        const bool publishing = lock_producer_publishing();
        if (publishing) {
            lock_note_pair_outcome(!lp.verified);
            if (lock_should_hard_reset() && lock_request_session_reset()) {
                return;
            }
        } else {
            static ULONGLONG told_stall_ms = 0;
            const ULONGLONG now_ms = GetTickCount64();
            if (told_stall_ms == 0 || now_ms - told_stall_ms > 2000) {
                told_stall_ms = now_ms;
                logf_("[lock] no producer publication yet (frame_published=%llu) -"
                      " waiting for the depth-only record; pairing is not attempted\n",
                      static_cast<unsigned long long>(g_ctl ? g_ctl->frame_published : 0));
            }
        }
        pair_ok = lp.verified;
        pair_slot = lp.slot;
        if (!pair_ok) {
            static UINT told = 0;
            if (told < 8) {
                ++told;
                logf_("[lock] pair miss (%s) - draining the publication window\n", lp.reason);
            }
            // The colour will never be used (no exact publication). Release the whole
            // window: the producer may fill it with CURRENT frames immediately, so the next
            // WGC colour can match again. Without this the window freezes on old frames and
            // every later colour misses forever (the "once behind, always behind" queue).
            // Nothing was copied/tagged for this colour, so ACKing is safe.
            //
            // Do not "wait for the watermark": PBZ showed the other deadlock. The producer
            // could not publish because every slot was still un-ACKed, so it skipped
            // (no ACKed slot), the colour ran ahead, and no later colour could ever match.
            // The last verified slot was already ACKed by ack_previous_slot() before this
            // frame, so releasing the rest cannot invalidate a frame we are still reading.
            // Re-anchor immediately from the marker's full-id neighbourhood. The
            // producer is allowed to advance to this colour frame, while the window
            // is still held for the retry.
            lock_reanchor_colour_watermark(lp.payload);
            // After the timeout, release the unread window so the producer can publish
            // current frames immediately. The watermark above is independent of this.
            const ULONGLONG miss_now = GetTickCount64();
            if (g_lock_miss_since_ms == 0) { g_lock_miss_since_ms = miss_now; }
            const bool release_window = (miss_now - g_lock_miss_since_ms) >= 100;
            if (release_window) {
                ack_window_except(mgpu::kMaxSlots);
                g_lock_miss_since_ms = 0;
                static UINT told_release = 0;
                if (told_release < 3) {
                    ++told_release;
                    logf_("[lock] pair miss: released the unread publication window after timeout\n");
                }
            } else {
                static UINT told_hold = 0;
                if (told_hold < 3) {
                    ++told_hold;
                    logf_("[lock] pair miss: holding publication window for retry\n");
                }
            }
            // The colour frames arrive but the producer publishes nothing: this is not a
            // pairing failure, the game's DLSS-G Evaluate / fakerFG record path stopped
            // (PBZ does this across a frame-limiter or display reset). Keep it visible in
            // the log instead of only reporting no_slot.
            if (g_ctl) {
                static uint64_t s_pub_seen = 0;
                static ULONGLONG s_pub_ms = 0;
                static bool s_pub_stalled = false;
                const uint64_t pub = g_ctl->frame_published;
                const ULONGLONG now_ms = GetTickCount64();
                if (pub != s_pub_seen) {
                    if (s_pub_stalled) {
                        logf_("[lock] producer record resumed: frame_published %llu -> %llu"
                              " after %llums\n",
                              (unsigned long long)s_pub_seen, (unsigned long long)pub,
                              (unsigned long long)(s_pub_ms ? now_ms - s_pub_ms : 0));
                    }
                    s_pub_seen = pub;
                    s_pub_ms = now_ms;
                    s_pub_stalled = false;
                } else if (pub != 0 && s_pub_ms != 0 && now_ms - s_pub_ms > 800) {
                    if (!s_pub_stalled) {
                        s_pub_stalled = true;
                        logf_("[lock] producer record stalled: frame_published=%llu unchanged"
                              " for %llums - the game stopped feeding DLSS-G (toggle frame"
                              " generation off/on or restart the game)\n",
                              (unsigned long long)pub,
                              (unsigned long long)(now_ms - s_pub_ms));
                    }
                }
            }
            // The watermark was already re-anchored above on the first miss; doing it
            // again after the drain only repeated the same movement.
            // DEGRADED, or any no-MV/OF lane with a ready optical-flow field: keep the
            // current colour moving with the last verified slot. Wuchang deliberately
            // publishes depth only (mask=0x2) and uses OF, so a marker miss must not
            // immediately become a visible 1X frame.
            const bool of_ready_fallback =
#if MGPU_FG_OF
                g_of_ready && mv_from_flow();
#else
                false;
#endif
            if ((g_pair_degraded || of_ready_fallback) && g_have_last &&
                g_last_slot < ((g_ctl->slots != 0) ? g_ctl->slots : 1)) {
                // DEGRADED: keep output alive with the last verified MV/depth slot and
                // the optical-flow field computed from THIS colour. No guessed marker
                // slot is used; the fallback is explicit and marked in the OSD.
                pair_ok = true;
                pair_slot = static_cast<int>(g_last_slot);
                fallback_pair = true;
                g_lock_no_slot_streak = 0;
            } else {
                g_lock_no_slot_streak++;
                if (g_lock_no_slot_streak > 8) {
                    // Keep the picture alive if the link stays broken: one 1X frame, then try
                    // to re-anchor again. Normal gameplay never gets here.
                    g_lock_no_slot_streak = 0;
                    pair_ok = false;
                } else {
                    return;
                }
            }
        } else {
            g_lock_no_slot_streak = 0;
            // Watermark for the producer: this is the last colour identity we can actually
            // use. The producer stays within kMaxSlots/2 of it instead of running ahead at
            // the game's publication rate.
            if (g_ctl && pair_slot >= 0 && pair_slot < static_cast<int>(mgpu::kMaxSlots)) {
                g_ctl->colour_seq = g_ctl->frames[pair_slot].frame_id;
            }
        }
    }

    // ---- v29 default-path pacing: decimate the surplus on the GAME clock --------------
    // Decided here, before the colour copy / MV copies / tag / present: a dropped frame
    // costs nothing but the WGC capture we already paid for. The signal is
    // frames[].frame_qpc_ms (the game's own present clock, 1 ms) because WGC's
    // SystemRelativeTime is quantised to whole vblanks - a 42 fps input and a 39 fps
    // input are indistinguishable there.
    //
    // `fallback_pair` (DEGRADED, last verified slot) carries a stale timestamp and is
    // excluded; a pair miss resets the clocks so the next interval cannot span the hole.
    if (!g_reuse_frame && !fallback_pair && pair_ok &&
        pair_slot >= 0 && pair_slot < static_cast<int>(mgpu::kMaxSlots) && g_ctl != nullptr) {
        const uint64_t game_t = g_ctl->frames[pair_slot].frame_qpc_ms;
        note_game_input(game_t);
        if (slot_gate_drop()) {
            ++g_slot_drops;
            // Nothing was read from this slot. The ACK is deliberately NOT written here:
            // ack_previous_slot() (next accepted frame) and ack_older_than() (the accepted
            // frame's own path) both run *after* gpu_throttle(), which is what guarantees
            // the previous frame's GPU work has retired before a slot is handed back.
            // The dropped publication is older than the next accepted one, so the normal
            // watermark releases it there.
            return;
        }
        ++g_slot_keeps;
        note_game_accepted(game_t);
    } else if (!g_reuse_frame) {
        g_game_prev_qpc = 0;
        g_game_prev_acc_qpc = 0;
    }

    // 1. colour: capture frame -> shared sink (D3D11, on the AMD card)
    // One frame id for XeLL markers and the XeSS-FG present id, as the guide
    // requires. XeLL sleeps at simulation start and needs the markers to pace.
    const uint32_t present_id = static_cast<uint32_t>(++g_present_id);
    const double t_r0 = qpc_ms();
    // Catch-up burst detector: after a stall, WGC hands over several queued frames and the
    // loop processes them back to back (each with its own Present). This is the "the missing
    // inputs get dumped into the next batch" effect - measurable instead of guessed.
    {
        static double s_prev_proc = 0.0;
        if (s_prev_proc > 0.0) {
            const double dt = t_r0 - s_prev_proc;
            const double nom = input_interval_ms();
            if (nom > 2.0 && dt < nom * 0.6) { ++g_burst_count; }
        }
        s_prev_proc = t_r0;
    }
    // Protocol v7: tell the add-on whether its motion-vector stream is wanted. It is
    // ~14 MB/frame across PCIe, so it is produced only when we will tag it.
    if (g_ctl) {
        // --mv-shadow needs the producer to keep feeding real vectors even though we tag
        // zeroed ones, otherwise the copy would transfer stale data and measure nothing.
        // Auto (default): always ask for MV so the producer sends it when it can; the
        // per-frame decision below picks between it, our flow, and zeros.
        bool want_producer_mv = true;
        if (!g_mv_auto) {
            want_producer_mv = !(g_no_mv && !g_mv_shadow);
#if MGPU_FG_OF
            if (g_fg_of) { want_producer_mv = false; }
#endif
        }
#if MGPU_FG_OF
        // --fg-of builds its own vectors from the colour, so the producer stream (and its
        // ~14.7 MB/frame across PCIe) would be pure waste.
        if (g_fg_of) { want_producer_mv = false; }
#endif
        // Auto mode: once the producer has published depth-only for a while, stop asking
        // for MV so it cannot start copying a stale/dummy texture and flip the source back.
        // Re-probe every ~120 frames so a title that begins feeding vectors is found.
        if (g_mv_auto && g_ctl && g_ctl->frame_published != 0 &&
            !producer_has_mv_stream()) {
            static uint32_t probe = 0;
            const bool allow_probe = ((++probe % 120u) == 0u);
            if (!allow_probe) { want_producer_mv = false; }
        }
        g_ctl->consumer_request = want_producer_mv ? mgpu::kRequestMv : 0u;
        // v14: the full switch board. --no-mv/--no-depth/--fg-of/--no-colour fold into it, so
        // there is exactly one place that decides what crosses the bus.
        uint32_t mask = g_stream_mask;
        if (!want_producer_mv)          { mask &= ~mgpu::kRequestMv; }
        if (g_no_depth)                 { mask &= ~mgpu::kRequestDepth; }
        g_ctl->request_mask = mask;
        if (mask != g_stream_mask_last) {
            g_stream_mask_last = mask;
            logf_("[lock] streams -> mv=%d depth=%d color=%d ui=%d meta=%d\n",
                  (mask & mgpu::kRequestMv) ? 1 : 0, (mask & mgpu::kRequestDepth) ? 1 : 0,
                  (mask & mgpu::kRequestColor) ? 1 : 0, (mask & mgpu::kRequestUi) ? 1 : 0,
                  (mask & mgpu::kRequestMeta) ? 1 : 0);
        }
    }
    // --dyn-mfg: OptiScaler's controller, once per real frame (before anything is
    // tagged, so a new count applies to this present).
    if (g_dyn_mfg && !g_interp_cadence) { fg_dyn_mfg_frame(t_r0, g_ms_present); }
    // --interp-cadence: pick this frame's multiplier from the vblanks that elapsed
    // since the previous real frame (see cadence_next_slots). Done before anything is
    // tagged, so the count applies to this present.
    if (g_interp_cadence && g_refresh_hz > 0) {
        const UINT slots = cadence_next_slots();     // what the phase accumulator wants
        UINT want = slots - 1;
        // Rate-limit the expensive part: see g_cad_hold_frames. While holding we tag the
        // count the DLL is already configured for, so what we ask for and what it produces
        // always agree; the phase accumulator keeps the long-run average correct anyway.
        if (g_cad_hold > 0) {
            --g_cad_hold;
            want = g_fg_interp_actual;
        } else if (want != g_fg_interp_actual) {
            want = (want > g_fg_interp_actual) ? (g_fg_interp_actual + 1)
                                              : (g_fg_interp_actual - 1);
            if (want < 1) { want = 1; }
            g_cad_hold = (g_cad_hold_frames > 0) ? g_cad_hold_frames : 0;
        }
        const UINT before = g_fg_interp_actual;
        fg_set_interp(want, "interp-cadence", /*quiet=*/true);
        if (g_fg_interp_actual != before) { ++g_cad_switches; }
        if (g_fg_interp_actual != want) { ++g_cad_fail; }
        if (want < 8) { ++g_cad_hist[want]; }
        g_cad_last_k = want + 1;      // frameRenderTime follows what we actually tagged
    }
    // Waiting for the previous frame keeps queue depth at 1: latency stays bounded and
    // "gpu" is a real measurement, but our submissions cannot overlap XeSS-FG's work.
    if (g_no_throttle) { g_ms_throttle = 0.0; } else { gpu_throttle(); }
    // v10: the previous frame's GPU work (which read its MV/depth slot) has completed, so
    // hand that slot back to the producer.
    ack_previous_slot();
    const double t_work0 = qpc_ms();

    // Real frame interval for XeSS-FG's pacing model. QPC: GetTickCount64 ticks in 15.6 ms
    // steps, so this EMA could only ever read 0 / 16 / 31 ms.
    {
        const double now_tick = qpc_ms();
        if (g_last_frame_tick > 0.0) {
            g_frame_ms_ema = g_frame_ms_ema * 0.9 + (now_tick - g_last_frame_tick) * 0.1;
        }
        g_last_frame_tick = now_tick;
    }
    if (g_xell && !g_reuse_frame) {
        const uint32_t fid = present_id;
        xellAddMarkerData(g_xell, fid, XELL_SIMULATION_START);
        // XeLL's whole job is to delay work to reduce latency, so its sleep is timed:
        // it is the one component inside run_frame that could hide tens of ms.
        const double x0 = qpc_ms();
        xellSleep(g_xell, fid);
        const double x1 = qpc_ms();
        g_ms_xell = g_ms_xell * 0.9 + (x1 - x0) * 0.1;
        if (x1 - x0 > g_max_xell) { g_max_xell = x1 - x0; }
        xellAddMarkerData(g_xell, fid, XELL_SIMULATION_END);
    }

    const double t_c0 = qpc_ms();
    if (g_colour_mutex) { g_colour_mutex->AcquireSync(0, 20); }
    if (!g_reuse_frame && !g_fg_colour_shared) {
        // CopyResource is silent about mismatches and a failed copy leaves the sink at its
        // initial (black) content - which looks exactly like "the pipeline runs but the
        // screen is black" (protocol-independent, every counter healthy). Say it once.
        static bool copy_checked = false;
        if (!colour11) {
            if (!copy_checked) {
                copy_checked = true;
                logf_("[lock] colour copy: colour11 is NULL on a non-reuse frame"
                      " - nothing can reach the sink\n");
            }
        } else if (!copy_checked) {
            copy_checked = true;
            D3D11_TEXTURE2D_DESC sd{}, dd{};
            colour11->GetDesc(&sd);
            if (g_shared11) { g_shared11->GetDesc(&dd); }
            logf_("[lock] colour copy: src %ux%u fmt %u -> sink %ux%u fmt %u %s\n",
                  (unsigned)sd.Width, sd.Height, (unsigned)sd.Format,
                  (unsigned)dd.Width, dd.Height, (unsigned)dd.Format,
                  (sd.Width == dd.Width && sd.Height == dd.Height &&
                   sd.Format == dd.Format) ? "(match)" : "<== MISMATCH");
        }
        if (colour11) { g_ctx11->CopyResource(g_shared11.Get(), colour11); }
    }
    if (g_colour_mutex) { g_colour_mutex->ReleaseSync(0); }
    // The D3D11 half of the colour hop: keyed-mutex acquire + the full-frame CopyResource
    // (66 MB at 4K, 118 MB at 5K, both on this card). It used to be invisible - `work`
    // only starts after it and `copy` only covers the D3D12 wait - so the largest single
    // per-frame transfer had no line of its own in the breakdown.
    {
        const double c1 = qpc_ms();
        g_ms_colour = g_ms_colour * 0.9 + (c1 - t_c0) * 0.1;
        if (c1 - t_c0 > g_max_colour) { g_max_colour = c1 - t_c0; }
    }

    if (!g_reuse_frame && !g_fg_colour_shared) {
        const UINT64 fv = ++g_fence11_value;
        g_ctx11_4->Signal(g_fence11.Get(), fv);
        // 2. D3D12 waits for that copy. This is a 118 MB transfer on the same GPU
        //    XeSS-FG is using, and it was never measured - it is now.
        const double f0 = qpc_ms();
        g_queue->Wait(g_fence12.Get(), fv);
        const double f1 = qpc_ms();
        g_ms_copywait = g_ms_copywait * 0.9 + (f1 - f0) * 0.1;
        if (f1 - f0 > g_max_copywait) { g_max_copywait = f1 - f0; }
    }

    // 3. MV/depth slot: the marker match above is the only pairing key. WGC delivers
    //    the colour one frame behind the producer's publication, so colour and motion
    //    data are never assumed to be the same index - they are matched by frame_id.
    const UINT slots = (g_ctl->slots != 0) ? g_ctl->slots : 1;
    UINT slot = g_mv_slot % slots;
    if (g_reuse_frame) {
        slot = g_last_slot;
    } else if (pair_ok && pair_slot >= 0 && pair_slot < static_cast<int>(slots)) {
        slot = static_cast<UINT>(pair_slot);
        static bool logged_pair = false;
        if (!logged_pair) {
            logged_pair = true;
            logf_("[lock] marker pair: WGC %.1fms -> slot %u (frame_id=%llu, age %.1fms)\n",
                  g_wgc_ts_ms, slot,
                  static_cast<unsigned long long>(g_ctl->frames[slot].frame_id),
                  g_wgc_ts_ms - static_cast<double>(g_ctl->frames[slot].frame_qpc_ms));
        }
        // A-v3: pick this frame's vector source before anything is tagged or dispatched.
        // The producer marks its own vectors unusable in `semantics`; we then run our optical
        // flow for this frame (and keep receiving the producer's MV, so we can switch back).
        // Auto MV: what did the producer actually publish? Refreshed once per captured
        // frame - this is the "handle what we received" decision, not a UI option.
        if (g_mv_auto && g_ctl) {
            g_auto_use_mv = !g_fg_of && producer_has_mv_stream();
        }
        g_of_active = mv_from_flow();
        g_of_fresh = false;
#if MGPU_FG_OF
        if (!g_of_active && g_of_fallback && !g_no_mv && g_ctl) {
            const uint32_t sem = g_ctl->frames[slot].semantics;
            if ((sem & mgpu::kSemQualityKnown) != 0 && (sem & mgpu::kSemMvUsable) == 0) {
                g_of_active = true;
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    logf_("[lock] producer marked its motion vectors UNUSABLE (quality=%u/255)"
                          " - falling back to our optical flow until it says otherwise\n",
                          (sem >> mgpu::kSemMvQualityShift) & 0xFFu);
                }
            }
        }
        if (g_of_active && !g_of_ready) { of_init(); }
#endif
        if (fallback_pair) { g_of_active = true; }
        lock_note_mv_source(!g_of_active && !fallback_pair, g_of_active);
        // Round-robin slot walking. Protocol v5 was meant to let us pick the newest
        // GPU-complete slot (frames[].ready), but publishing that "ready" needs a
        // GetCompletedValue() poll on a SHARED_CROSS_ADAPTER fence, which stalls the
        // game's render thread (33 -> 60 ms, picture flickers - docs/findings/HANDOFF.md 8.6,
        // "两个必须记住的坑"). The producer therefore never advertises it, the poll and
        // --mv-newest are gone, and the slots are consumed in order - which is the
        // behaviour that always ran at full rate.
        g_last_slot = slot;
        g_have_last = true;
        // Release every publication older than the one we just matched; their colour has
        // already been consumed and they only pin the producer's credit window.
        ack_older_than(g_ctl->frames[slot].frame_id);
    }

    // Prefetch lookup happens BEFORE the OF/colour list: a miss selects the flow as this
    // frame's vector source and suppresses the fallback copy entirely. (The copy for the
    // next slot is already running on the copy queue; the wait is a GPU-side queue op.)
    g_of_miss = fallback_pair;
    bool pf_hit = false;
    if (g_pf_pub) {
        if (g_pub_fifo) {
            // FIFO: the queue head is the oldest publication that has already been copied
            // locally. No key comparison and no slot guessing; the colour is `age` behind,
            // so the head is normally the frame the WGC capture just delivered.
            PubQueueEntry qe{};
            if (pub_q_pop(&qe, g_ctl->generation)) {
                slot = qe.slot;
                pf_hit = true;
                ++g_pf_hits;
                g_queue->Wait(g_copy_fence.Get(), qe.fence);
            } else {
                ++g_pf_misses;
            }
        } else {
            // Copy-on-publication: the local cache is keyed by the exact publication value.
            const uint64_t pub = g_ctl->frames[slot].published;
            if (pub != 0 && g_local_pub[slot] == pub && g_local_fence[slot] != 0) {
                pf_hit = true;
                ++g_pf_hits;
                // Order this frame's list after the copy that produced the cache entry.
                g_queue->Wait(g_copy_fence.Get(), g_local_fence[slot]);
            } else {
                ++g_pf_misses;
            }
        }
    } else if (g_prefetch_inputs) {
        if (g_pf_valid && g_pf_slot == slot && g_pf_gen == g_ctl->generation) {
            pf_hit = true;
            g_pf_valid = false;                     // consumed; g_pf_fence keeps the value
            ++g_pf_hits;
        } else {
            if (g_pf_valid) { ++g_pf_misses; }      // a prefetch existed but for another slot
            g_pf_valid = false;
        }
        if (pf_hit) { prefetch_wait(); }
    }
    if (pair_ok && !pf_hit && g_of_track && !g_no_mv && !g_no_inputs && !g_reuse_frame &&
        g_ctl && (g_ctl->active_mask & mgpu::kRequestMv) != 0) {
        g_of_miss = true;
        g_of_active = true;                         // tag the flow for this frame
        ++g_of_miss_frames;
        static uint32_t told = 0;
        if (told < 3) {
            ++told;
            logf_("[lock] of: prefetch miss -> optical flow for this frame (no copy)\n");
        }
    }
    // v10: 只 ACK 真正读过的槽。配对失败时读的是"没有"，所以 ACK 最旧的一个发布，
    // 只为让 producer 继续往前走（见上面的 pair_drain_slot）。
    if (pair_ok) {
        g_ack_slot = slot;
    } else if (pair_drain_slot >= 0) {
        g_ack_slot = static_cast<UINT>(pair_drain_slot);
    } else {
        g_ack_slot = mgpu::kMaxSlots;
    }

    // Cached: see back_buffer(). The buffer identity is fixed for the life of the swap
    // chain, so this also removes the per-frame QueryInterface/refcount traffic.
    ID3D12Resource* bbres = nullptr;
    const UINT bbi = g_swapchain->GetCurrentBackBufferIndex();
    if (!back_buffer(bbi, &bbres)) { return; }
    D3D12_RESOURCE_STATES& bb_state = g_bb_state[bbi & 3];

    g_alloc->Reset();
    g_list->Reset(g_alloc.Get(), nullptr);

    // A miss with no depth history at all (startup / right after a generation change) needs
    // a valid depth resource for the tag and for the flow's guide: use the flat dummy at
    // the stream size. Steady-state misses use the last depth from the ring.
    if (g_of_miss && g_ring_count == 0) {
        ensure_dummy_depth(slot);
        if (g_depth_local[slot]) { of_bind_depth(0, g_depth_local[slot].Get()); }
    }

    D3D12_RESOURCE_BARRIER bb[2]{};
    bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bb[0].Transition.pResource = bbres;
    bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bb[0].Transition.StateBefore = bb_state;
    bb[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    bb[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bb[1].Transition.pResource = g_shared12.Get();
    bb[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bb[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g_list->ResourceBarrier(2, bb);

    // --fg-of: motion vectors from the colour we just captured. Recorded before the
    // convert draw so the colour is still in COMMON for the flow dispatch; of_record
    // restores COMMON on the way out, which is what the barrier above assumes.
#if MGPU_FG_OF
    if (g_of_active || g_of_track) {
        const double o0 = qpc_ms();
        if (!g_of_ready) { of_init(); }
        if (of_record(g_list.Get())) {
            g_of_fresh = true;
            const double o1 = qpc_ms();
            g_ms_of = g_ms_of * 0.9 + (o1 - o0) * 0.1;
            if (o1 - o0 > g_max_of) { g_max_of = o1 - o0; }
        }
    }
#endif

    // scRGB (FP16) -> BT.2020 + PQ in R10G10B10A2: the format XeSS-FG requires.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(bbi & 3) * g_rtv_stride;
    if (!g_rtv_res[bbi & 3]) {
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_device->CreateRenderTargetView(bbres, &rtv_desc, rtv);
        g_rtv_res[bbi & 3] = bbres;
    }

    // Draw to the real back buffer size. XeSS-FG creates the native swap chain
    // itself, so its size need not equal the captured size; a viewport that is too
    // small leaves an uncovered border of uninitialised pixels around the picture.
    const D3D12_RESOURCE_DESC bb_desc = bbres->GetDesc();
    const UINT draw_w = static_cast<UINT>(bb_desc.Width);
    const UINT draw_h = bb_desc.Height;

    // Checked on the first frame, not during init: the window is only resized to the
    // borderless size after the swap chain exists, so checking earlier reported a
    // bogus mismatch. A present size that differs from the client area means DXGI
    // stretches the picture - that is the "everything looks scaled" symptom.
    if (!g_present_size_checked) {
        g_present_size_checked = true;
        RECT cr{};
        if (g_bridge_wnd && GetClientRect(g_bridge_wnd, &cr)) {
            const UINT cw = static_cast<UINT>(cr.right - cr.left);
            const UINT ch = static_cast<UINT>(cr.bottom - cr.top);
            if (cw && ch && (cw != draw_w || ch != draw_h)) {
                logf_("[lock] WARNING: present size %ux%u != window client %ux%u - DXGI will"
                      " stretch the picture. Align the capture, the output and the monitor"
                      " (see --den/--scale).\n", draw_w, draw_h, cw, ch);
            } else {
                RECT wr{};
                GetWindowRect(g_bridge_wnd, &wr);
                logf_("[lock] present %ux%u == window client %ux%u (1:1, no stretch)"
                      " window rect=(%ld,%ld)-(%ld,%ld) monitor=%ux%u\n",
                      draw_w, draw_h, cw, ch, wr.left, wr.top, wr.right, wr.bottom,
                      g_full_w, g_full_h);
            }
        }
    }

    D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(draw_w), static_cast<float>(draw_h),
                       0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(draw_w), static_cast<LONG>(draw_h) };
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &scissor);
    g_list->SetGraphicsRootSignature(g_conv_root.Get());
    g_list->SetPipelineState(g_conv_pso.Get());
    ID3D12DescriptorHeap* heaps[] = { g_srv_heap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetGraphicsRootDescriptorTable(0, g_srv_heap->GetGPUDescriptorHandleForHeapStart());
    float conv_consts[12];
    conv_constants(conv_consts, draw_w, draw_h);
    // In-pass marker repair: 本帧 marker 若刚解码成功，就把四角覆盖掉。
    conv_consts[11] = lock_marker_should_repair() ? 1.0f : 0.0f;
    {
        static float told_repair = -1.0f;
        if (conv_consts[11] != told_repair) {
            told_repair = conv_consts[11];
            logf_("[lock] convert marker repair constant = %.0f (draw %ux%u)\n",
                  conv_consts[11], draw_w, draw_h);
        }
    }
    g_list->SetGraphicsRoot32BitConstants(1, 12, conv_consts, 0);
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->DrawInstanced(3, 1, 0, 0);

    // UI unblend: the chain buffer now contains the presented backbuffer (UI included).
    // The unblend pass needs this frame's local UI copy, so it is recorded after
    // copy_inputs_local() below, not here. Keep the chain buffer in RENDER_TARGET until
    // then; it is transitioned to PRESENT after the pass.
    ID3D12Resource* hudless_res = nullptr;

    bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bb[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_list->ResourceBarrier(1, &bb[1]);

    // 4. tag the FG inputs. The producer's resources live on a heap imported from a file
    //    mapping with ALLOW_CROSS_ADAPTER; XeSS-FG fails to copy those ("Failed to
    //    allocate memory for a copy of DEPTH"), so plain local copies are tagged instead.
    //    Those copies pull data that physically lives on the NVIDIA card across to the
    //    AMD one, so both the recording time and the GPU wait for the previous frame are
    //    worth watching.
    //
    //    --input-copy-queue: they are recorded into the copy queue's list here and start
    //    executing while this list's convert draw is still being recorded; the direct
    //    queue is made to wait for them *after* the submit below, so the two overlap and
    //    XeSS-FG's work (submitted at Present) still sees finished data.
    //    --prefetch-inputs: normally they were already recorded during the *previous*
    //    frame's Present, so this frame only queues the wait.
    // The lookup itself happened before the OF/colour list (see above): a prefetch hit
    // needs no copy, and a miss now uses the flow instead of a fallback copy.
    // v30: the MV decode is a Dispatch, which a COPY list cannot record, so the codec keeps
    // the inputs on this list (same reason as --upsample).
    const bool cq = (pf_hit || g_of_miss || g_upsample || !pair_ok || mv_codec_shared())
                        ? false : input_copy_begin();
    if (pair_ok && !pf_hit && !g_of_miss) {
        const double m0 = qpc_ms();
        if (g_no_inputs) { ensure_dummy_inputs(slot); } else { copy_inputs_local(slot); }
        const double m1 = qpc_ms();
        g_ms_inputs = g_ms_inputs * 0.9 + (m1 - m0) * 0.1;
        if (m1 - m0 > g_max_inputs) { g_max_inputs = m1 - m0; }
        input_copy_submit(cq);
        // --upsample: record the reconstruction into the same list, after the copies it
        // reads and before the tags below. The copy-queue variants are off (see the
        // argument handling), so the copies are guaranteed to be in this list.
        if (g_upsample && !cq) { record_upsample(slot); }
    }
    // Copy-on-publication: on a hit the reconstruction pass has the prefetched local
    // copies available (the copy-fence wait is already queued before this list).
    if (g_upsample && pf_hit) { record_upsample(slot); }

    // UI unblend: only after this frame's UI copy exists. A failed/missing UI stream leaves
    // hudless_res null, so the tag section below falls back to tagging UI (mode 2).
    D3D12_RESOURCE_STATES hudless_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // v34: the support patch lives on stream 4. It is composed on this card (H' = present
    // outside the support, patch inside); stream 4 itself is never tagged directly.
    const bool patch_stream = g_ctl != nullptr &&
        (g_ctl->active_mask & mgpu::kRequestHudless) != 0 &&
        (g_ctl->streams[4].ui_flags & mgpu::kHudlessFlagPatchPayload) != 0;
    if (!patch_stream && g_hudless_stream && pair_ok && g_hudless_fresh[slot] &&
        g_hudless_local[slot] && draw_w >= 64 && draw_h >= 64) {
        // v32: clean HUDless from stream 4. No unblend, no division by (1-a).
        hudless_res = g_hudless_local[slot].Get();
        hudless_state = D3D12_RESOURCE_STATE_COMMON;
        static bool told_clean = false;
        if (!told_clean) {
            told_clean = true;
            logf_("[lock] hudless: using clean stream 4 (no unblend), state=COMMON\n");
        }
    } else if (pair_ok && !g_of_miss && !g_no_inputs && !cq) {
        hudless_res = record_ui_unblend(slot, bbres, draw_w, draw_h);
    }

    // The chain buffer stayed in RENDER_TARGET through the unblend pass so the pass could
    // read it as an SRV; hand it to the presenter now.
    bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bb[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_list->ResourceBarrier(1, &bb[0]);
    bb_state = D3D12_RESOURCE_STATE_PRESENT;

    // Policy (2026-11-11): no motion vectors -> no frame generation. Menus, loading screens and
    // transitions legitimately carry no MV, and the capture (correctly) refuses to invent a
    // state for a texture the title never transitions. Presenting the real frame there is the
    // right answer; generating from a stale field is what produced artefacts.
    //
    // 2026-11-22 (--fg-gate): one frame without the MV stream used to cost a full
    // SetEnabled(0) -> SetEnabled(1) cycle, which resets XeFG's history (and, for the 1<->2
    // crossing, rebuilds inside the library). With `--fg-gate N` up to N consecutive such
    // frames are tolerated: generation stays ON, but this frame hands XeFG no inputs and no
    // present id - OptiScaler's Dispatch early-out (XeFG_Dx12.cpp:696-703). `--fg-gate-up M`
    // needs M consecutive MV frames before switching back on. Both default to the old
    // behaviour (0 / 1); a missing *pairing* is never tolerated.
    {
        static bool gated_off = false;
        static int miss_run = 0;
        static int ok_run = 0;
        static bool tolerated_run = false;
        // 只有"marker 配对成功 + producer 这一帧确实发布了 MV"才允许插帧。
        // Wuchang/Unreal uses optical flow and deliberately requests no producer MV
        // (streams -> mv=0 depth=1). The gate must accept the OF field in that case:
        // otherwise pair is exact but XeFG stays at 1X forever.
        const bool producer_mv = producer_has_mv_stream();
#if MGPU_FG_OF
        // XeFG rejects a frame whose MV and depth sizes differ. g_of_mv is created at
        // the depth stream size at init; a later resolution/preset change can invalidate
        // that. Refuse to tag (1X) and say why instead of feeding a mismatched pair.
        const bool of_size_ok =
            g_ctl != nullptr &&
            g_of_out_w == g_ctl->streams[mgpu::kStreamDepth].width &&
            g_of_out_h == g_ctl->streams[mgpu::kStreamDepth].height;
        if (g_of_active && g_of_ready && g_of_fresh && !of_size_ok) {
            static bool told_size = false;
            if (!told_size) {
                told_size = true;
                logf_("[lock] of: tagged size %ux%u != depth stream %ux%u - OF disabled"
                      " (resolution changed after init)\n",
                      g_of_out_w, g_of_out_h,
                      g_ctl ? g_ctl->streams[mgpu::kStreamDepth].width : 0u,
                      g_ctl ? g_ctl->streams[mgpu::kStreamDepth].height : 0u);
            }
        }
        const bool flow_mv = g_of_active && g_of_ready && g_of_fresh && of_size_ok;
#else
        const bool flow_mv = false;
#endif
        const bool mv_available = pair_ok && g_ctl && (producer_mv || flow_mv);
        const bool hard_miss = !mv_available && !pair_ok;   // pairing unverified: never tolerate

        if (mv_available) { if (ok_run < 1000) { ++ok_run; } miss_run = 0; }
        else { if (miss_run < 1000) { ++miss_run; } ok_run = 0; }

        g_mv_gate_tolerated = false;
        if (!mv_available && !gated_off) {
            // Tolerate a short run of MV-missing frames (menu blip, one dropped publication)
            // instead of resetting XeFG's history for it.
            if (!hard_miss && g_fg_gate_hold > 0 && miss_run <= g_fg_gate_hold) {
                g_mv_gate_tolerated = true;
                if (!tolerated_run) {
                    tolerated_run = true;
                    logf_("[lock] MV missing this frame -> holding FG on (%d/%d tolerated)"
                          " [pair=%d producer_mv=%d of=%d/%d]\n",
                          miss_run, g_fg_gate_hold, pair_ok ? 1 : 0, producer_mv ? 1 : 0,
#if MGPU_FG_OF
                          g_of_ready ? 1 : 0, g_of_fresh ? 1 : 0
#else
                          0, 0
#endif
                          );
                }
            } else {
                const xefg_swapchain_result_t r = g_api.SetEnabled(g_fg, 0);
                logf_("[lock] %s -> 1X (%s, %d) [pair=%d producer_mv=%d of=%d/%d]\n",
                      pair_ok ? "MV missing this frame" : "colour/motion pairing unverified",
                      xefg_str(r), (int)r, pair_ok ? 1 : 0, producer_mv ? 1 : 0,
#if MGPU_FG_OF
                      g_of_ready ? 1 : 0, g_of_fresh ? 1 : 0
#else
                      0, 0
#endif
                      );
                if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS) { gated_off = true; }
                tolerated_run = false;
            }
        } else if (mv_available && gated_off) {
            // --fg-gate-up: require a short run of good frames, so an alternating hit/miss
            // pattern cannot make SetEnabled flap every frame.
            if (ok_run >= g_fg_gate_up) {
                const xefg_swapchain_result_t r = g_api.SetEnabled(g_fg, 1);
                logf_("[lock] MV back -> FG on (%s, %d) [%d consecutive]\n",
                      xefg_str(r), (int)r, ok_run);
                if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS) { gated_off = false; }
            } else if (ok_run == 1 && g_fg_gate_up > 1) {
                logf_("[lock] MV back - waiting for %d consecutive frames before FG on\n",
                      g_fg_gate_up);
            }
        }
        if (mv_available) { tolerated_run = false; }
        // Nothing to tag while the gate is closed: the swap chain is in 1X and the real frame
        // goes straight out. (An early return here would skip Close()/Present.)
        // `|| !pair_ok` keeps the tag guard closed even if SetEnabled(0) itself failed.
        g_mv_gate_off = gated_off || !pair_ok || g_mv_gate_tolerated;
    }


    xefg_swapchain_d3d12_resource_data_t rd{};
    rd.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
    rd.resourceBase = { 0, 0 };
    // --input-copy-queue: the copies are not in this list, so neither are the tags. The
    // library only needs the list for resources that stop being valid before the next
    // present; ours live until then (see the switch's comment).
    ID3D12CommandList* tag_list = (cq || pf_hit) ? nullptr : g_list.Get();
    // MV and depth can be different sizes (UE5: MVs at render size, depth at full res), so
    // each is tagged with its own stream size - the old code used streams[1] for both.
#if MGPU_FG_OF
    if (g_of_active && g_of_ready && g_of_fresh &&
        g_ctl && g_of_out_w == g_ctl->streams[mgpu::kStreamDepth].width &&
        g_of_out_h == g_ctl->streams[mgpu::kStreamDepth].height) {
        // Our own field: 1/8 resolution, screen-UV units, computed from this frame's
        // colour on this GPU (the producer's MV stream measured as garbage). Requiring
        // g_of_fresh keeps a stale field from being tagged when the flow did not run.
        rd.resourceSize = { g_of_out_w, g_of_out_h };
        rd.pResource = g_of_mv.Get();
    } else
#endif
    {
        // --upsample: both streams were rebuilt to the render size by the joint pass, so
        // the pair below is same-size by construction (XeFG rejects mismatches).
        if (g_upsample && g_up_valid && g_up_slot == slot && g_up_mv) {
            rd.resourceSize = { g_up_w, g_up_h };
            rd.pResource = g_up_mv.Get();
        } else {
            rd.resourceSize = { g_ctl->streams[1].width, g_ctl->streams[1].height };
            rd.pResource = g_mv_local[slot].Get();
        }
    }
    rd.incomingState = D3D12_RESOURCE_STATE_COMMON;
    rd.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
    if (!g_mv_gate_off) { g_api.D3D12TagFrameResource(g_fg, tag_list, present_id, &rd); }

    if (g_upsample && g_up_valid && g_up_slot == slot && g_up_depth) {
        rd.resourceSize = { g_up_w, g_up_h };
        rd.pResource = g_up_depth.Get();
    } else if (g_pf_pub && g_depth_local[slot]) {
        // Copy-on-publication cache: this is the exact publication the lookup selected.
        rd.resourceSize = { g_ctl->streams[2].width, g_ctl->streams[2].height };
        rd.pResource = g_depth_local[slot].Get();
    } else {
        rd.resourceSize = { g_ctl->streams[2].width, g_ctl->streams[2].height };
        rd.pResource = g_depth_local[slot].Get();
        if (!g_no_depth && g_ring_need > 0 && g_ring_count > 0 && g_depth_ring[0]) {
            UINT back = static_cast<UINT>((g_depth_lag > 0) ? g_depth_lag : 0);
            if (back >= g_ring_count) { back = g_ring_count - 1; }
            const UINT idx = (g_ring_write + g_ring_need - back) % g_ring_need;
            if (g_depth_ring[idx]) {
                rd.pResource = g_depth_ring[idx].Get();
#if MGPU_FG_OF
                g_of_depth_desc = idx;          // guide next frame's upsample with it
#endif
            }
        }
    }
    rd.type = XEFG_SWAPCHAIN_RES_DEPTH;
    if (!g_mv_gate_off) { g_api.D3D12TagFrameResource(g_fg, tag_list, present_id, &rd); }

    // Tag HUDless + UI together whenever both exist: XeFG's AUTO then selects
    // BACKBUFFER_HUDLESS_UITEXTURE (mode 5) for BOTH a game-supplied and a render-card
    // synthesised UI. Mode 5 composites our UI back over the HUDless (and refines with the
    // full-resolution backbuffer), so the heuristic mask no longer has to be divided out of
    // the interpolation base - mode 4's (P-U)/(1-a) unblend is exactly what left the ring
    // around UI edges. 2026-11-23: default for every source; CP2077 A/B measured mode 5 as
    // the better one for the derived UI too.
    // Hard condition: XeFG requires HUDless/UI/backbuffer to share resourceSize, so a
    // half-res UI stream (UiFullRes=0) still has to stay mode 4.
    // --no-ui-mode5 restores mode 4 for every source. Without a HUDless this frame the
    // fallback below still tags a game UI alone (mode 2); a derived UI is never tagged
    // alone.
    // UI resources are per UI publication slot (see ui_slot_index in 060), not per MV slot:
    // 8 local decode targets instead of 32 at 4K (~1.06 GB -> 265 MB).
    const UINT uil = ui_slot_index(slot);
    const bool ui_active = g_ui_local[uil] && g_ui_fresh[uil] && pair_ok && !g_of_miss &&
                           (g_ctl->active_mask & mgpu::kRequestUi) != 0 &&
                           g_ctl->streams[3].width >= 64 && g_ctl->streams[3].height >= 64;
    const uint32_t ui_src = ui_source_now();
    const bool ui_derived = (g_ctl->streams[3].ui_flags & mgpu::kUiFlagDerived) != 0;
    const bool ui_patch = (g_ctl->streams[4].ui_flags & mgpu::kHudlessFlagPatchPayload) != 0;
    const uint32_t ui_q = g_ctl->streams[3].ui_quality;
    const bool ui_full_res = g_ctl->streams[3].width == draw_w &&
                             g_ctl->streams[3].height == draw_h;
    // mode 5 = HUDless + UI, for every UI source (game tag or render-card synth). A patch
    // is only the HUDless source; when stream 3 carries a game UI plane, tag both and
    // AUTO selects mode 5. Without a UI plane the same H' goes alone -> mode 4.
    // 0/1 both mean "use it when allowed"; only --no-ui-mode5 (2) forces mode 4.
    // Full-res is a hard condition: XeFG requires HUDless/UI/backbuffer to share
    // resourceSize. The old 50..6000 bp coverage gate is gone: an empty derived UI in
    // mode 5 composites back to the HUDless, so it is harmless.
    bool mode5 = false;
    // v34 patch path: stream 4 is the reconstruction material and the consumer tags H'
    // only. The UI stream is disabled by default; if a stale UiStream=1 still publishes
    // a game UI plane, do not combine it here (mode 5 costs another UI encode/decode and
    // is intentionally sealed off).
    if (!ui_patch && hudless_res != nullptr && ui_active) {
        mode5 = (g_ui_mode5 != 2) && ui_full_res;
    }
    if (hudless_res != nullptr) {
        rd.resourceSize = { draw_w, draw_h };
        rd.pResource = hudless_res;
        rd.incomingState = hudless_state;
        rd.type = XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
        if (!g_mv_gate_off) { g_api.D3D12TagFrameResource(g_fg, tag_list, present_id, &rd); }
        if (mode5) {
            rd.resourceSize = { draw_w, draw_h };
            rd.pResource = g_ui_local[uil].Get();
            rd.incomingState = D3D12_RESOURCE_STATE_COMMON;
            rd.type = XEFG_SWAPCHAIN_RES_UI;
            if (!g_mv_gate_off) {
                g_api.D3D12TagFrameResource(g_fg, tag_list, present_id, &rd);
            }
            static int told_mode5 = -1;
            const int k = ui_derived ? 1 : 0;
            if (told_mode5 != k) {
                told_mode5 = k;
                logf_("[lock] ui: mode 5 (HUDless + UI), source=%s coverage=%u bp\n",
                      ui_derived ? "render-synth" : "game", ui_q);
            }
        }
    } else if (ui_active && !ui_patch && ui_src == mgpu::kUiSourceGame) {
        rd.resourceSize = { g_ctl->streams[3].width, g_ctl->streams[3].height };
        rd.pResource = g_ui_local[uil].Get();
        rd.incomingState = D3D12_RESOURCE_STATE_COMMON;
        rd.type = XEFG_SWAPCHAIN_RES_UI;
        if (!g_mv_gate_off) { g_api.D3D12TagFrameResource(g_fg, tag_list, present_id, &rd); }
    } else if (ui_active && !ui_patch) {
        // Derived UI is only a means to rebuild HUDless; without a HUDless this frame it
        // must not be tagged as UI (that would freeze scene colour into the overlay).
        static bool told_derived = false;
        if (!told_derived) {
            told_derived = true;
            logf_("[lock] ui-unblend: derived UI has no HUDless this frame -"
                  " UI composition stays off (no UI tag fallback)\n");
        }
    } else if (ui_patch && ui_active) {
        static bool told_patch = false;
        if (!told_patch) {
            told_patch = true;
            logf_("[lock] ui-patch: patch stream present but H' compose did not run -"
                  " no tag this frame (composition stays off)\n");
        }
    }

    // --tag-dump: copy exactly what XeFG is about to receive (backbuffer/HUDless/UI).
    if (g_tag_dump_frames > 0) {
        ID3D12Resource* tag_ui = g_ui_used_current;
        D3D12_RESOURCE_STATES tag_ui_state = g_ui_used_state;
        // v33/v34: in patch mode the third dump is the reconstruction material (the
        // decoded HUDless patch), not a UI plane. stream 4 is the patch stream.
        if (ui_patch) {
            const UINT hslot = g_hudless_slots ? (slot % g_hudless_slots) : slot;
            if (hslot < mgpu::kMaxSlots && g_hudless_local[hslot]) {
                tag_ui = g_hudless_local[hslot].Get();
                tag_ui_state = D3D12_RESOURCE_STATE_COMMON;
            }
        } else if (tag_ui == nullptr && ui_active) {
            tag_ui = g_ui_local[uil].Get();
            tag_ui_state = D3D12_RESOURCE_STATE_COMMON;
        }
        record_tag_dump(g_list.Get(), slot, mode5 ? 1u : 0u,
                        bbres, D3D12_RESOURCE_STATE_RENDER_TARGET,
                        hudless_res, hudless_state,
                        tag_ui, tag_ui_state);
    }
    // --dump: record the readback copies in this list, i.e. after everything that
    // feeds XeSS-FG and before Close, so they see exactly what was tagged.
    if (g_dump_frames > 0) { dump_record(g_list.Get(), slot, present_id); }

    g_list->Close();
    if (g_xell) { xellAddMarkerData(g_xell, present_id,
                                    XELL_RENDERSUBMIT_START); }
    ID3D12CommandList* lists[] = { g_list.Get() };
    g_queue->ExecuteCommandLists(1, lists);
    // --input-copy-queue: our list (convert draw) is queued first, so the MV/depth copies
    // on the copy queue run *concurrently* with it. This wait sits between our list and
    // Present(), which is where XeSS-FG submits the work that reads those resources.
    if (cq) { input_copy_wait(); }
    if (g_xell) { xellAddMarkerData(g_xell, present_id,
                                    XELL_RENDERSUBMIT_END); }
    g_ms_work = g_ms_work * 0.9 + (qpc_ms() - t_work0) * 0.1;

    // 5. frame constants: jitter from the producer, approximate matrices
    // v11: what the producer *measured* about its own inputs, logged on change. The
    // switches (--mv-flip / --fg-mv-scale / --fg-depth-inv) still win for now; the next
    // step is to let these drive them, which is why they are transported at all
    // (docs/findings/INPUT_SEMANTICS.md).
    {
        static uint32_t logged_sem = 0xFFFFFFFFu;
        const uint32_t sem = g_ctl->frames[slot].semantics;
        if (sem != logged_sem) {
            logged_sem = sem;
            const char* unit = "unknown";
            switch ((sem >> mgpu::kSemMvUnitShift) & 3u) {
            case 1:  unit = "pixels"; break;
            case 2:  unit = "uv"; break;
            case 3:  unit = "ndc"; break;
            default: break;
            }
            const char* sgn = "unknown";
            switch ((sem >> mgpu::kSemMvSignShift) & 3u) {
            case 1:  sgn = "prev-cur"; break;
            case 2:  sgn = "cur-prev"; break;
            default: break;
            }
            const char* dep = "unknown";
            switch ((sem >> mgpu::kSemDepthShift) & 3u) {
            case 1:  dep = "reversed"; break;
            case 2:  dep = "forward"; break;
            default: break;
            }
            const unsigned q = (sem >> mgpu::kSemMvQualityShift) & 0xFFu;
            const bool q_known = (sem & mgpu::kSemQualityKnown) != 0;
            // "(declared)" is not "(unknown)": since 2026-11-10 the producer publishes the
            // API's own declaration for the sign as well (it is the contract), so the probe
            // suffix now distinguishes *measured* from *declared* and from neither.
            const char* sign_src = (sem & mgpu::kSemSignMeasured) ? "(measured)"
                                   : (sgn[0] != 'u' ? "(declared)" : "(unknown)");
            if (q_known) {
                logf_("[lock] producer semantics: mv=%s%s sign=%s%s depth=%s%s quality=%u/255%s"
                      " valid=%ux%u\n",
                      unit, (sem & mgpu::kSemMvMeasured) ? "(measured)" : "(declared)",
                      sgn, sign_src,
                      dep, (sem & mgpu::kSemDepthMeasured) ? "(measured)" : "(declared)",
                      q, (sem & mgpu::kSemMvUsable) ? "  [MV usable]" : "  [MV NOT usable]",
                      g_ctl->frames[slot].valid_w, g_ctl->frames[slot].valid_h);
            } else {
                // No sign test has run yet (needs two consecutive frames plus depth):
                // say so instead of pretending the quality is zero.
                logf_("[lock] producer semantics: mv=%s%s sign=%s%s depth=%s%s"
                      " quality=unknown (sign test has not run yet) valid=%ux%u\n",
                      unit, (sem & mgpu::kSemMvMeasured) ? "(measured)" : "(declared)",
                      sgn, sign_src, dep, (sem & mgpu::kSemDepthMeasured) ? "(measured)" : "(declared)",
                      g_ctl->frames[slot].valid_w, g_ctl->frames[slot].valid_h);
            }
        }
    }

    xefg_swapchain_frame_constant_data_t cd{};
    // The projection has to match the aspect the frame was RENDERED at, not the one we
    // capture/present at. They are almost always identical, but the render size is the
    // honest source (and it is the only one that also matches the MV/depth field).
    UINT proj_w = g_ctl->frames[slot].render_w;
    UINT proj_h = g_ctl->frames[slot].render_h;
    if (proj_w < 64 || proj_h < 64) { proj_w = g_width; proj_h = g_height; }
    const float const_aspect = static_cast<float>(proj_w) / static_cast<float>(proj_h);
    // v6: a real camera beats --fg-fov. FSR 3.1 exports near/far/vfov; DLSS-SR does
    // not, so those games keep the --fg-fov / --fg-zn constants.
    const mgpu::FrameShared& fr = g_ctl->frames[slot];
    // Bucket the producer's own present interval exactly like the composition-side arrival
    // histogram, so the two can be compared row by row (see g_game_hist).
    if (fr.frame_time_ms > 0.5f && fr.frame_time_ms < 1000.0f) {
        const double vb_g = refresh_ms();
        if (vb_g > 0.5) {
            int kg = static_cast<int>(static_cast<double>(fr.frame_time_ms) / vb_g + 0.5);
            if (kg < 1) { kg = 1; }
            if (kg > 8) { kg = 8; }
            ++g_game_hist[kg];
        }
    }
    const bool cam_ok = ((g_producer_flags & mgpu::kInputFlagHasCamera) != 0) &&
                        std::isfinite(fr.near_plane) && fr.near_plane > 0.0f &&
                        fr.far_plane > fr.near_plane &&
                        fr.vfov_rad > 0.05f && fr.vfov_rad < 2.5f;
    // ---- v12: the game's own view-to-clip, when the producer had one ----
    // DLSS-SR exports no camera, so every NGX title used to get a synthesised projection from
    // --fg-fov (60 deg, 16:9-ish aspect). The add-on now forwards Streamline's sl::Constants,
    // which is documented as jitter-free, so this is the game's real aspect/FOV/far-plane form
    // including the infinite reversed-Z. Sanity checks: the matrix must look like a projection
    // (same shape test OptiScaler uses) and must not be stale.
    bool have_matrix = false;
    float mtx_fov = 0.0f;                       // derived vertical FOV, radians
    float mtx_aspect = const_aspect;            // derived aspect (b/a)
    {
        const float* m = fr.view_to_clip;
        bool finite = true;
        for (int i = 0; i < 16; ++i) { if (!std::isfinite(m[i])) { finite = false; break; } }
        const bool shape = std::fabs(m[15]) < 1.0e-2f &&
                           std::fabs(m[1]) < 1.0e-3f && std::fabs(m[4]) < 1.0e-3f &&
                           (std::fabs(std::fabs(m[11]) - 1.0f) < 1.0e-3f ||
                            std::fabs(std::fabs(m[14]) - 1.0f) < 1.0e-3f);
        const uint64_t now = GetTickCount64();
        const bool fresh = fr.matrix_tick_ms != 0 && now >= fr.matrix_tick_ms &&
                           (now - fr.matrix_tick_ms) < 500ull;
        have_matrix = finite && shape && fresh &&
                      (g_producer_flags & mgpu::kInputFlagHasMatrix) != 0 && !g_fg_no_proj;
        if (have_matrix) {
            // Derive the numbers OptiScaler derives (2*atan(1/b) is the vertical FOV; b/a is the
            // aspect), and keep near/far from whatever the source declared - the matrix-derived
            // near/far are the part their code distrusts on UE titles.
            const float a = std::fabs(fr.view_to_clip[0]);
            const float b = std::fabs(fr.view_to_clip[5]);
            if (b > 1.0e-6f) {
                mtx_fov = 2.0f * std::atan(1.0f / b);
            }
            if (a > 1.0e-6f && b > 1.0e-6f) {
                const float asp = b / a;
                if (asp > 0.4f && asp < 4.0f) { mtx_aspect = asp; }
            }
        }
        static bool logged_matrix = false;
        if (have_matrix && !logged_matrix) {
            logged_matrix = true;
            logf_("[lock] projection: from the game's own view-to-clip (v12): fovY=%.2fdeg"
                  " aspect=%.3f%s\n"
                  "[lock]   [%+.4f %+.4f %+.4f %+.4f]\n"
                  "[lock]   [%+.4f %+.4f %+.4f %+.4f]\n"
                  "[lock]   [%+.4f %+.4f %+.4f %+.4f]\n"
                  "[lock]   [%+.4f %+.4f %+.4f %+.4f]\n",
                  mtx_fov * 57.2957795f, mtx_aspect,
                  g_fg_use_matrix ? " (used verbatim: --fg-use-matrix)" : " (derived; --fg-fov retired)",
                  m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                  m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        }
    }
    // ---- projection cache (2026-11-22) ------------------------------------------
    // The producer publishes TWO records per game frame (evaluate-time and the
    // present-time fallback) and only one of them carries sl::Constants, so
    // `have_matrix` alternates every frame (measured: 50/50 over every ab_1x log).
    // Falling back to the synthetic 60deg/10/10000 on the other half would flip FOV,
    // near/far and the whole matrix at half the input rate - a far worse input than
    // either value being slightly off. Keep the last matrix-derived projection and
    // reuse it while a record without a matrix arrives.
    static bool     pc_matrix = false;      // a real matrix has been seen
    static float    pc_m[16]{};
    static float    pc_fov = 0.0f, pc_aspect = 0.0f;
    static bool     pc_planes = false;
    static float    pc_zn = 0.0f, pc_zf = 0.0f;
    static bool     pc_inf = false;
    static uint64_t pc_tick = 0;
    const uint64_t proj_now = GetTickCount64();
    const bool pc_fresh = pc_matrix && proj_now >= pc_tick && (proj_now - pc_tick) < 2000ull;
    if (have_matrix) {
        memcpy(pc_m, fr.view_to_clip, sizeof(pc_m));
        if (mtx_fov > 0.05f) { pc_fov = mtx_fov; }
        pc_aspect = mtx_aspect;
        pc_matrix = true;
        pc_tick = proj_now;
    } else if (pc_fresh) {
        static bool told_cache = false;
        if (!told_cache) {
            told_cache = true;
            logf_("[lock] note: record without a view-to-clip - keeping the last derived"
                  " projection (two publications per frame, cache <2 s)\n");
        }
    }
    // 0 = no matrix, 1 = this frame's own matrix, 2 = cached (this record had none)
    const int mtx_src = have_matrix ? 1 : (pc_fresh ? 2 : 0);
    const bool mtx_eff = mtx_src != 0;
    const float eff_fov = have_matrix ? mtx_fov : pc_fov;
    const float eff_aspect = have_matrix ? mtx_aspect : pc_aspect;
    const float proj_fov = (mtx_eff && eff_fov > 0.05f)
                               ? eff_fov
                               : (cam_ok ? fr.vfov_rad : (g_fg_fov_y * 3.14159265f / 180.0f));
    const float proj_aspect = (mtx_eff && eff_aspect > 0.1f && eff_aspect < 10.0f)
                                  ? eff_aspect
                                  : const_aspect;
    // The game's own projection wins, unconditionally (decision 2026-11-10). We do *not*
    // second-guess it against --fg-fov: some integrations hand XeFG a plain 60 deg placeholder,
    // and that is still the game's own answer for its own FG - overriding it with our guess was
    // the worse bet. The line below exists so a mismatch is visible instead of silent.
    if (have_matrix && std::fabs(mtx_fov * 57.2957795f - g_fg_fov_y) > 5.0f) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logf_("[lock] note: the game's projection says fovY=%.2fdeg, --fg-fov says %.2f -"
                  " keeping the game's (its own FG uses it)\n",
                  mtx_fov * 57.2957795f, g_fg_fov_y);
        }
    }
    // ---- near/far --------------------------------------------------------------
    // 1) the producer's declared camera wins (FSR 3.1 exports near/far/vfov);
    // 2) otherwise derive them from the game's own view-to-clip, when that matrix is fresh
    //    and looks like a projection. This replaces the old hard-coded `--fg-zn` / 10000
    //    fallback, which is wrong for every reversed-Z title (and is where the far plane
    //    of a UE game silently became 10000 instead of infinity);
    // 3) --fg-far-finite then replaces an infinite far plane with a finite one, exactly what
    //    OptiScaler hands UE titles (75000; XeFG_Inputs_Dx12.cpp:464-478).
    float proj_zn = cam_ok ? fr.near_plane : g_fg_near_z;
    float proj_zf = cam_ok ? fr.far_plane : 10000.0f;
    bool far_infinite = !std::isfinite(proj_zf) || proj_zf <= 0.0f;
    bool zn_zf_derived = false;
    if (!cam_ok) {
        if (have_matrix) {
            float dn = 0.0f, df = 0.0f;
            bool dinf = false;
            if (derive_near_far_from_matrix(fr.view_to_clip, dn, df, dinf)) {
                pc_zn = dn;
                pc_zf = df;
                pc_inf = dinf;
                pc_planes = true;
            }
        }
        if (pc_planes) {          // this record's matrix, or the cached one
            proj_zn = pc_zn;
            proj_zf = pc_zf;
            far_infinite = pc_inf;
            zn_zf_derived = true;
        }
    }
    if (g_fg_far_finite > 0.0f && far_infinite) {
        proj_zf = g_fg_far_finite;
        far_infinite = false;
    }
    // An infinite far plane only makes sense in the reversed-Z form; the finite form
    // divides by infinity and produces NaNs.
    const bool use_rz = g_fg_proj_rz || far_infinite;
    {
        // One line per change: which of the three sources produced the planes, and whether
        // an infinite far was kept or replaced. This is the first thing to read when the
        // edge artifacts are blamed on the projection.
        static bool told_planes = false;
        static float told_zn = -1.0f, told_zf = -1.0f;
        static bool told_inf = false;
        if (!told_planes || told_zn != proj_zn || told_zf != proj_zf || told_inf != far_infinite) {
            told_planes = true;
            told_zn = proj_zn;
            told_zf = proj_zf;
            told_inf = far_infinite;
            logf_("[lock] near/far: zn=%.3f zf=%.0f%s%s%s (camera=%d matrix=%d%s"
                  " farFinite=%.0f)\n",
                  proj_zn, proj_zf,
                  far_infinite ? " (infinite)" : "",
                  zn_zf_derived ? " (derived from the game's view-to-clip)" : "",
                  g_game_is_unreal ? ", Unreal shipping build" : "",
                  cam_ok ? 1 : 0, mtx_src, (mtx_src == 2) ? " cached" : "",
                  g_fg_far_finite);
        }
    }
    if (mtx_eff && g_fg_use_matrix) {
        const float* src = have_matrix ? fr.view_to_clip : pc_m;
        for (int i = 0; i < 16; ++i) { cd.projectionMatrix[i] = src[i]; }
    } else if (use_rz) { approx_projection_rz(cd.projectionMatrix, proj_aspect, proj_fov, proj_zn); }
    else { approx_projection(cd.projectionMatrix, proj_aspect, proj_fov, proj_zn, proj_zf); }
    cd.viewMatrix[0] = 1.0f;
    cd.viewMatrix[5] = 1.0f;
    cd.viewMatrix[10] = 1.0f;
    cd.viewMatrix[15] = 1.0f;
    if (g_fg_no_proj) {
        // Test switch: hand XeFG a plain identity instead of our aspect guess.
        for (int i = 0; i < 16; ++i) { cd.projectionMatrix[i] = 0.0f; }
        cd.projectionMatrix[0] = cd.projectionMatrix[5] = cd.projectionMatrix[10] = 1.0f;
        cd.projectionMatrix[15] = 1.0f;
    }
    // Always the producer's jitter: the game's motion vectors are computed on the jittered
    // render, so this is what XeFG needs to align its history (see g_fg_jitter_zero above).
    cd.jitterOffsetX = g_ctl->frames[slot].jitter_x;
    cd.jitterOffsetY = g_ctl->frames[slot].jitter_y;
    if (g_fg_jitter_zero) {
        // A/B: our colour is the post-upscaler image, i.e. the jitter is already resolved.
        cd.jitterOffsetX = 0.0f;
        cd.jitterOffsetY = 0.0f;
    }
    if (g_fg_jitter_flip) {
        cd.jitterOffsetX = -cd.jitterOffsetX;
        cd.jitterOffsetY = -cd.jitterOffsetY;
    }
    // The producer knows how its vectors are scaled (NGX MV.Scale.X/Y; FSR's
    // motionVectorScale is in pixels, e.g. (2880, 1620)). Pass it through instead of
    // assuming 1.0 and use --mv-flip's sign for the (previous-current) convention.
    cd.motionVectorScaleX = g_ctl->frames[slot].mv_scale_x;
    cd.motionVectorScaleY = g_ctl->frames[slot].mv_scale_y;
    if (cd.motionVectorScaleX == 0.0f && cd.motionVectorScaleY == 0.0f) {
        cd.motionVectorScaleX = 1.0f;
        cd.motionVectorScaleY = 1.0f;
    }
    // --fg-mv-scale: the two conventions seen in the field. CP2077 reports (2560, 1440) and
    // its texels are UV; PBZ reports (1, 1) while XeFG documents its vectors as
    // "screen-space motion in pixels", so 'px' turns the latter into pixels and the two can
    // be compared instead of argued about (docs/findings/UE5_MV_OPTISCALER.md 5.2).
    if (g_fg_mv_scale_mode == 1) {
        const float sx = static_cast<float>(g_ctl->streams[1].width ? g_ctl->streams[1].width : 1u);
        const float sy = static_cast<float>(g_ctl->streams[1].height ? g_ctl->streams[1].height : 1u);
        cd.motionVectorScaleX *= sx;
        cd.motionVectorScaleY *= sy;
    } else if (g_fg_mv_scale_mode == 2) {
        cd.motionVectorScaleX = g_fg_mv_scale_force;
        cd.motionVectorScaleY = g_fg_mv_scale_force;
    }
    // --fg-of: the producers' MV.Scale describes THEIR texture, not ours. Our optical-flow
    // pass writes screen UV of the captured frame (480x270 SINT -> UV), so with the
    // producer's scale - 1.0 on PBZ - the library multiplied those UV values by 1 and saw
    // ~0.009 px of motion instead of ~35 px. That is exactly why "optical flow" looked
    // identical to "no motion vectors" (PBZ, 2026-11-09).
    // The space is the render/MV one, not the 4K output: every game that reports a usable
    // MV.Scale reports its render size there (CP2077: 2560x1440), and XeFG's definition of
    // "pixels" has to agree with the depth/MV grid it gets.
    if (g_of_active && g_of_fresh) {
        // XeFG multiplies each velocity texel by this value. g_of_mv is the resource we
        // actually tag, so the scale is ITS size - not the producer's MV stream size.
        // They differ on depth-only titles (there is no MV stream at all) and whenever
        // MV and depth have different source resolutions.
        cd.motionVectorScaleX = static_cast<float>(g_of_out_w ? g_of_out_w : 1u);
        cd.motionVectorScaleY = static_cast<float>(g_of_out_h ? g_of_out_h : 1u);
    }
    if (g_mv_sign < 0.0f) {
        cd.motionVectorScaleX = -cd.motionVectorScaleX;
        cd.motionVectorScaleY = -cd.motionVectorScaleY;
    }
    // v13: the producer's own FG declaration can invalidate XeFG history on scene cuts
    // and teleports. This is metadata from sl::Constants.reset / DLSSG.Reset; it costs
    // nothing and is safer than guessing from a frame-time spike.
    cd.resetHistory = (fr.fg_flags & mgpu::kInputFlagResetHistory) ? 1u : 0u;
    if (!g_fg_consts_logged) {
        g_fg_consts_logged = true;
        char fov_buf[96]{};
        if (cam_ok) {
            _snprintf_s(fov_buf, sizeof(fov_buf), _TRUNCATE,
                        "camera=producer fov=%.1fdeg zn=%.2f zf=%.0f",
                        proj_fov * 180.0f / 3.14159265f, proj_zn, proj_zf);
        } else if (zn_zf_derived) {
            _snprintf_s(fov_buf, sizeof(fov_buf), _TRUNCATE,
                        "camera=derived fov=%.1fdeg zn=%.3f zf=%.0f%s",
                        proj_fov * 180.0f / 3.14159265f, proj_zn, proj_zf,
                        far_infinite ? " (infinite)" : "");
        } else {
            _snprintf_s(fov_buf, sizeof(fov_buf), _TRUNCATE,
                        "camera=synthetic (--fg-fov %.1f, --fg-zn %.2f)",
                        g_fg_fov_y, g_fg_near_z);
        }
        logf_("[lock] frame constants: jitter=(%.4f, %.4f)%s proj=%s %s"
              " aspect=%.5f (%ux%u) mvscale=(%.4f, %.4f) game_dt=%.2fms\n",
              cd.jitterOffsetX, cd.jitterOffsetY,
              g_fg_jitter_flip ? " (flipped)" : "",
              g_fg_no_proj ? "identity"
                           : (use_rz ? "reversed-Z approximation" : "aspect approximation"),
              fov_buf, const_aspect, proj_w, proj_h,
              cd.motionVectorScaleX, cd.motionVectorScaleY,
              static_cast<double>(fr.frame_time_ms));
    }
    // XeFG's pacing model eats this. OptiScaler lets it be the input frame time,
    // our present interval, or zero ([FrameGen] FTInput). We measured that the
    // input frame time works best for 2x; --fg-rt 0 is there to test whether 0
    // stops the library from budgeting the whole batch.
    // --fg-rt-mode picks *which* estimate of the input frame time: the historical
    // EMA (default), the raw last interval, a 16-sample median that ignores WGC's
    // +-1 vblank quantization, or that median snapped to whole vblanks.
    const double rt_ms = fg_render_time_ms();
    cd.frameRenderTime = static_cast<float>(rt_ms);
    // v6 cross-check: the add-on measures the game's own present interval. A large
    // mismatch means our capture cadence is not the game's (WGC batching, a limiter
    // in between), and frameRenderTime comes from *our* estimate - worth one line.
    //
    // The producer publishes a RAW single interval (FrameShared.frame_time_ms, not an EMA),
    // so comparing it against our *median* fired on a perfectly healthy beat pattern: a game
    // at ~44 fps on a 120 Hz panel alternates 2/3 vblank presents (16.7 / 25.0 ms), our
    // median reads 16.7 and one raw 3-vblank sample reads 25-26 ms -> "WGC sees 60 fps but
    // the game only renders 38". That is a false positive and it cost a debugging session on
    // 2026-11-09 (the game's own frame generation was in fact OFF). Both sides now use a
    // median over the same window, so only a real rate mismatch can trigger it.
    {
        static unsigned dt_frames = 0;
        static bool dt_warned = false;
        // 16-sample medians, one per frame, of the producer's own present interval.
        static float s_ring[16] = {};
        static UINT  s_n = 0, s_i = 0;
        if (fr.frame_time_ms > 0.5f && fr.frame_time_ms < 1000.0f) {
            s_ring[s_i] = fr.frame_time_ms;
            s_i = (s_i + 1) % 16u;
            if (s_n < 16) { ++s_n; }
        }
        double producer_ms = 0.0;
        if (s_n > 0) {
            float v[16];
            for (UINT i = 0; i < s_n; ++i) { v[i] = s_ring[i]; }
            for (UINT i = 1; i < s_n; ++i) {          // n <= 16: insertion sort is fine
                const float k = v[i];
                int j = static_cast<int>(i) - 1;
                while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; --j; }
                v[j + 1] = k;
            }
            producer_ms = v[s_n / 2];
        }
        ++dt_frames;
        if (!dt_warned && dt_frames > 120 && producer_ms > 1.0 && rt_ms > 1.0 &&
            (producer_ms > rt_ms * 1.35 || producer_ms < rt_ms * 0.74)) {
            dt_warned = true;
            logf_("[lock] note: producer frame time %.2f ms (median) vs our capture cadence"
                  " %.2f ms - WGC batching or a limiter between them (frameRenderTime"
                  " uses ours, docs/findings/FINAL_CONFIG.md 5.4)\n",
                  producer_ms, rt_ms);
            // The dangerous direction: WGC delivers MORE frames than the game renders.
            // Real causes: the game's own frame generation (DLSS-G / FSR FG), or DWM
            // re-compositing the same content on the composition clock while the window has
            // lost independent flip (our topmost overlay does exactly that - HANDOFF 8.6).
            if (producer_ms > rt_ms * 1.35) {
                logf_("[lock]   WARNING: WGC sees %.1f fps but the game renders %.1f fps."
                      " Either the game's OWN frame generation is on (DLSS-G / FSR FG - turn it"
                      " off, FINAL_CONFIG 2.1.2), or DWM is re-compositing duplicates because"
                      " the game window lost independent flip (our overlay: try"
                      " --overlay-hidden). Duplicates are dropped, not interpolated\n",
                      1000.0 / rt_ms, 1000.0 / producer_ms);
            }
        }
    }
    const double fg0 = qpc_ms();
    // A tolerated MV-missing frame sends nothing downstream: this is OptiScaler's Dispatch
    // early-out (XeFG_Dx12.cpp:696-703), which returns before TagFrameConstants/SetPresentId.
    // Calling SetPresentId with no inputs for that id is a different (unproven) shape, so the
    // id gaps by one here - a not-ready dispatch upstream does exactly the same.
    if (!g_mv_gate_tolerated) {
        g_api.TagFrameConstants(g_fg, present_id, &cd);
        g_api.SetPresentId(g_fg, present_id);
    }
    const double fg1 = qpc_ms();
    g_ms_fg = g_ms_fg * 0.9 + (fg1 - fg0) * 0.1;
    if (fg1 - fg0 > g_max_fg) { g_max_fg = fg1 - fg0; }
    if (g_xell) { xellAddMarkerData(g_xell, present_id,
                                    XELL_PRESENT_START); }
    const double t_present = qpc_ms();
    // Present(1,0) blocked for ~31 ms here, capping the whole loop at ~31 fps and
    // competing with the game's own presents. The guide uses sync interval 0, and
    // XeSS-FG does its own pacing anyway.
    g_swapchain->Present(0, g_present_flags());
    const double pres_ms = qpc_ms() - t_present;
    g_present_ms = static_cast<DWORD>(pres_ms + 0.5);
    g_ms_present = g_ms_present * 0.9 + pres_ms * 0.1;
    // Signal after Present so this also covers XeSS-FG's own submissions.
    g_queue->Signal(g_frame_fence.Get(), ++g_frame_fence_value);
    if (g_xell) { xellAddMarkerData(g_xell, present_id,
                                    XELL_PRESENT_END); }

    // --dump: export this frame's colour/depth/MV so the pairing can be checked
    // offline. Placed after Present so the resources a dump reads are the exact
    // ones XeSS-FG was tagged with for this present id.
    if (g_dump_frames > 0) { dump_flush(); }
    if (g_ui_dump_frames > 0) { ui_dump_flush(); }
    if (g_tag_dump_frames > 0) { tag_dump_flush(); }

    // Did this frame's GPU work fit inside the interval the game gives us? When the sum
    // exceeds it we cannot consume frames as fast as they arrive, the WGC pool backs up
    // and frames get lost - which is what a "dip" looks like on screen.
    if (input_interval_ms() > 1.0 &&
        (g_ms_throttle + g_ms_present) > input_interval_ms()) {
        ++g_slow_frames;
    }
    // Per-frame version of the test above (g_slow_frames is a per-window counter): the main
    // loop's --slack-auto uses this to decide whether to absorb a queued frame instead of
    // dropping it. Same criterion, so the two never disagree about what "overran" means.
    g_last_overran = (input_interval_ms() > 1.0) &&
                     ((g_ms_throttle + g_ms_present) > input_interval_ms());

    // Continue from the slot that was actually consumed; with frame-key pairing this
    // is not necessarily the old round-robin successor.
    if (!g_reuse_frame) { g_mv_slot = (slot + 1) % slots; }
    ++g_frames;
    // Remember which producer publication this frame consumed, for --slack-auto's
    // duplicate-vs-new-content test in the main loop.
    if (g_ctl) { g_last_content_pub = g_ctl->frame_published; }

    // --prefetch-inputs: submit the NEXT frame's MV/depth copies now. They run on the copy
    // queue during this frame's Present()/vblank wait, i.e. off the next frame's critical
    // path; the next frame only queues a wait for them. g_mv_slot has already advanced, so
    // it names exactly the slot the next frame will read.
    if (g_prefetch_inputs && !g_pf_pub && !g_reuse_frame) { prefetch_next_inputs(g_mv_slot % slots); }

    // Re-assert our z-order every frame batch: the game raises itself on activation.
    assert_topmost();

    // Whole-function duration: whatever is not attributed to a component above shows up
    // here, which is how the "40 ms gap that nothing explains" gets pinned down.
    {
        const double tot = qpc_ms() - t_r0;
        g_ms_total = g_ms_total * 0.9 + tot * 0.1;
        if (tot > g_max_total) { g_max_total = tot; }
    }

    // Every frame, so the on-screen counter is smooth: framesPresented > 1 means
    // XeSS-FG really emitted interpolated frames for that Present, 1 = passthrough.
    xefg_swapchain_present_status_t ps{};
    const xefg_swapchain_result_t sr = g_api.GetLastPresentStatus(g_fg, &ps);
    g_presented_sum += ps.framesPresented;
    g_presented_n   += 1;
    // "xNmiss" counts real frames that produced fewer frames than the configured
    // multiplier, which is how a refused MFG step (old XeLL, model limit) shows up.
    // In --interp-cadence mode the multiplier changes every frame on purpose, so the
    // comparison is meaningless there; the histogram below is the equivalent check.
    if (!g_interp_cadence && ps.framesPresented < g_fg_interp_actual + 1) { ++g_x2_miss; }

    // Did somebody else take over presentation? With N+1 frames per real frame on
    // a vblank-paced display the batch cannot honestly cost much less than the
    // input interval; when it does, Present is returning without waiting (an OSD
    // or latency tool hooking it, or our overlay being occluded) and the picture
    // judders even though xN still reads 4.00.
    //
    // One window is not enough evidence: a catch-up burst (two frames processed
    // back to back after a stall) drops the EMA for a single window and used to
    // trigger this on a perfectly healthy run. Require three in a row.
    //
    // v30: a fast `total` alone no longer means anything - with frameRenderTime pinned to
    // m vblanks XeFG paces asynchronously, so Present returns early and total is 4-8 ms by
    // design (Wuchang logged total=5.7ms, input=25.6ms on a perfectly paced run). Only warn
    // when the *displayed* rate is also short of what the batches should produce.
    const double expected_out = (g_arrival_ms_ema > 0.5)
        ? static_cast<double>(g_fg_interp_actual + 1) * 1000.0 / g_arrival_ms_ema : 0.0;
    if (!g_nopace_warned && g_frames > 200 && ps.framesPresented >= 2 &&
        g_arrival_ms_ema > 8.0 && g_ms_total > 0.0 &&
        g_ms_total < 0.35 * g_arrival_ms_ema &&
        expected_out > 1.0 && static_cast<double>(g_show_fps) < expected_out - 10.0) {
        if (++g_nopace_windows >= 3) {
            g_nopace_warned = true;
            logf_("[lock] WARNING: presentation is not vblank-paced - total=%.1fms but the input"
                  " interval is %.1fms while %u frames are presented per real frame.\n",
                  g_ms_total, g_arrival_ms_ema, ps.framesPresented);
            logf_("[lock]   something else hooks Present or our overlay is occluded"
                  " (OSD / latency tool / FLM, RTSS, Steam overlay, a topmost window).\n");
            logf_("[lock]   MFG still produces %u frames, but they go out in bursts, so the"
                  " picture judders while the counter reads %.0f fps.\n",
                  ps.framesPresented, 1000.0 / std::max(g_ms_total, 0.001) * ps.framesPresented);
        }
    } else {
        g_nopace_windows = 0;
    }
    {
        const double nq = qpc_ms();
        double gap = 0.0;
        if (g_last_present_qpc > 0.0) {
            gap = nq - g_last_present_qpc;
            if (gap > g_max_gap) { g_max_gap = gap; }
        }
        g_last_present_qpc = nq;
        if (g_ms_throttle > g_max_gpu) { g_max_gpu = g_ms_throttle; }
        if (g_ms_present > g_max_present) { g_max_present = g_ms_present; }

        // Immediate hitch report. A 40-frame average hides exactly the one-frame stalls
        // the eye notices, so anything 45% over the expected spacing is logged at once
        // (rate limited, so a burst cannot flood the file).
        const double expect = (g_pace_ms > 0.0) ? g_pace_ms : g_arrival_ms_ema;
        // Without pacing the gap simply follows the source, and RTSS makes that
        // alternate between 16.7 and 41.7 ms on a 120 Hz panel - a 1.45x threshold
        // would flag that as a hitch every second. Only report real outliers there.
        const double hitch_limit = expect * ((g_pace_ms > 0.0) ? 1.45 : 2.0);
        if (expect > 1.0 && g_hitch_logged < 60) {
            double worst = gap;
            if (g_ms_throttle > worst) { worst = g_ms_throttle; }
            if (g_ms_present > worst) { worst = g_ms_present; }
            if (worst > hitch_limit) {
                ++g_hitch_logged;
                logf_("[lock] HITCH gap=%.1fms gpu=%.1fms pres=%.1fms wait=%.1fms"
                      " expect=%.1fms xN=%u slot=%u drop=%u\n",
                      gap, g_ms_throttle, g_ms_present, g_ms_wait, expect,
                      ps.framesPresented, slot, g_arr_drops);
            }
        }
    }
    // `show` is what the OSD and the per-frame log line print as "output fps". It used to be
    // loop_fps * framesPresented, i.e. the *loop* rate (measured with GetTickCount64!) times
    // the last batch size. The moment the loop catches up after a stall - WGC hands us two or
    // three queued frames back to back - that reads 148 or even 164 fps on a 120 Hz panel,
    // which is a number the display can never show and which sent people hunting for a bug
    // that does not exist. The swap chain is the only honest source: count what it really gave
    // the compositor per second, in the report window below (see `out:`).
    if (g_show_fps == 0 && g_frame_ms_ema > 0.1) {
        const double loop_fps = 1000.0 / g_frame_ms_ema;
        const double presented = (ps.framesPresented > 0) ? ps.framesPresented : 1;
        g_show_fps = static_cast<int>(loop_fps * presented + 0.5);   // first window only
    }

    if (g_frames == 30 || g_frames == 200 || g_frames == 600) { dump_zorder(); }

    if ((g_frames % g_report_every) == 1) {
        const ULONGLONG now_ms = GetTickCount64();
        const double win = (g_t_report && now_ms > g_t_report)
                         ? (now_ms - g_t_report) / 1000.0 : 0.0;
        g_t_report = now_ms;
        // --stream-auto / --stream-asym: one decision per window, with hysteresis.
        stream_controller_tick(slot);
        const double secs = (now_ms / 1000.0) - g_t0;
        logf_("[lock] frame %llu  slot %u  jitter=(%.3f, %.3f)  (%.1f fps avg, %.1f recent)"
              "  presented=%u fg=%u %s  hittest=%ld fgwin=%d present=%ums bb=%ux%u show=%d\n",
              (unsigned long long)g_frames, slot,
              cd.jitterOffsetX, cd.jitterOffsetY,
              (secs > 0) ? g_frames / secs : 0.0,
              (win > 0.0) ? static_cast<double>(g_report_every) / win : 0.0,
              ps.framesPresented, ps.isFrameGenEnabled,
              (sr == XEFG_SWAPCHAIN_RESULT_SUCCESS) ? xefg_str(ps.frameGenResult) : "status?",
              g_hittest_count,
              (GetForegroundWindow() == g_game_wnd) ? 1 : 0,
              g_present_ms, draw_w, draw_h, g_show_fps);
        lock_pair_report();     // marker 配对命中率 / lag（每个统计窗口清零）
        const double xn = (g_presented_n > 0) ? g_presented_sum / g_presented_n : 0.0;
        g_presented_sum = 0.0;
        g_presented_n = 0;

        // v29: the default-path pacing numbers. `in(vb)` is the interval between ACCEPTED
        // frames in vblanks (0.5-vblank buckets): at a locked 40 fps with 3x it is 3.0,
        // and a 4.0 bucket means the gate dropped the frame in between. `slot` is what the
        // gate did this window (it stays idle when --lock / --pace owns the input).
        if (g_slot_keeps || g_slot_drops || g_game_in_ema > 0.5) {
            const double vb = refresh_ms();
            const double target = (vb > 0.5)
                ? 1000.0 / (vb * static_cast<double>(g_fg_interp_actual + 1)) : 0.0;
            const double in_fps = (g_game_in_ema > 0.5) ? 1000.0 / g_game_in_ema : 0.0;
            const unsigned gate_n = g_slot_keeps + g_slot_drops;
            logf_("[lock]   game clock: in %.1f fps (%.2f ms) target %.1f fps%s"
                  " -> kept=%u dropped=%u (%.1f%%) gate=%s rate=%s\n",
                  in_fps, g_game_in_ema, target,
                  (target > 0.0 && in_fps > target * 1.02) ? " OVER" : "",
                  g_slot_keeps, g_slot_drops,
                  (gate_n > 0) ? 100.0 * g_slot_drops / gate_n : 0.0,
                  g_slot_gate ? "on" : "off",
                  (game_rate_moving() < 0) ? "?" : (game_rate_moving() ? "moving" : "steady"));
            logf_("[lock]   in(vb):");
            for (int i = 1; i <= 16; ++i) {
                if (g_in_step_hist[i]) { logf_(" %.1f=%u", i * 0.5, g_in_step_hist[i]); }
            }
            logf_("  (accepted-to-accepted, 0.5-vblank buckets)\n");
            for (int i = 0; i <= 16; ++i) { g_in_step_hist[i] = 0; }
            g_slot_keeps = 0;
            g_slot_drops = 0;
        }

        // Cadence control (both flags off by default, so neither the call nor the log
        // line appears in a normal run):
        //   --interp-auto  re-pick the interpolation count so the output lands on whole
        //                  vblanks (hysteresis lives in fg_auto_interp_tick).
        //   --fg-rt-mode   report which frameRenderTime estimate is being fed.
        if (g_interp_auto && !g_lock_auto) { fg_auto_interp_tick("auto (vblank cadence)"); }
        if (g_lock_auto) { fg_solve_cadence("lock-auto"); }
        // --dyn-mfg runs per real frame in run_frame_ex (its holds count frames).
        if (g_fg_rt_mode != kFgRtEma || g_interp_auto || g_lock_auto || g_lock_fps > 1.0 ||
            g_interp_cadence) {
            // Same estimator the accumulator used (mean of the ring), so this line and `want`
            // can never disagree again - they did, and it read as "input=3 vblanks but 5x".
            //
            // v29: prefer the GAME clock. arr_mean_ms() rejects any sample above 1.6x its
            // EMA (its spike filter), so a 26.7 ms input with 2-7% double intervals reads as
            // 25.0 ms - and then `out` below claims 120 fps while the real output is ~112.
            const double gm = game_median_ms();
            const double t_in = (gm > 0.5) ? gm : arr_mean_ms();

            const double vb = refresh_ms();
            // In cadence mode the multiplier changes per frame, so report the average
            // this window actually used instead of the last frame's count.
            double mul = static_cast<double>(g_fg_interp_actual + 1);
            if (g_interp_cadence) {
                // g_cad_hist[k] counts the frames that asked for k *interpolated* frames, so
                // the number of output frames per input frame is (k + 1). Forgetting the +1
                // made this line print "avg 1.00x, 60.0 fps out" for a run that was actually
                // doing 2.0x and 120 fps - the most misleading log in the program.
                double sum = 0.0, cnt = 0.0;
                for (int i = 0; i < 8; ++i) {
                    sum += static_cast<double>(i + 1) * g_cad_hist[i];
                    cnt += static_cast<double>(g_cad_hist[i]);
                }
                if (cnt > 0.0) { mul = sum / cnt; }
            }
            const double out_ms = (t_in > 0.0 && mul > 0.0) ? t_in / mul : 0.0;
            const double out_vb = (vb > 0.0) ? out_ms / vb : 0.0;
            // 1.00 / 2.00 / 3.00 vblank per output frame = perfectly even motion;
            // anything else is the pacer alternating short and long holds.
            const double whole = static_cast<double>(static_cast<int>(out_vb + 0.5));
            const double frac = dabs_(out_vb - whole);
            logf_("[lock]   cadence: rt=%.1fms (%s) input=%.1fms = %.2f vblanks ->"
                  " interp=%u (avg %.2fx, %.1f fps out = %.2f vblank/frame%s)"
                  " refresh=%uHz lock=%.2f drops=%u%s\n",
                  fg_render_time_ms(), fg_rt_feed_name(), t_in, t_in / vb,
                  // interp = generated frames for the last real frame (k-1). It used to print
                  // g_fg_interp_actual, i.e. the *ceiling* (--interp N), which read as "we are
                  // asking for 2" while the average below was 5x - the most misleading pair of
                  // numbers in the log (2026-11-10).
                  // v29: outside --interp-cadence the count is fixed, so print the applied one
                  // (the line used to show interp=0 for a 3x run, which reads as "1X").
                  (g_interp_cadence && g_cad_last_k > 0)
                      ? static_cast<unsigned>(g_cad_last_k - 1)
                      : g_fg_interp_actual,
                  mul,
                  (out_ms > 0.0) ? 1000.0 / out_ms : 0.0, out_vb,
                  (frac > 0.06) ? " NOT-EXACT" : "", g_refresh_hz,
                  g_lock_fps, g_lock_drops,
                  (g_fg_out_hz > 1.0) ? " (out-hz cap)" : "");
            g_lock_drops = 0;
            if (g_interp_cadence) {
                logf_("[lock]   cadence hist:");
                for (int i = 1; i < 8; ++i) {
                    if (g_cad_hist[i] != 0) { logf_(" %ux=%u", i + 1, g_cad_hist[i]); }
                }
                logf_("  switches=%u refused=%u\n", g_cad_switches, g_cad_fail);
                for (int i = 0; i < 8; ++i) { g_cad_hist[i] = 0; }
                g_cad_switches = 0;
                g_cad_fail = 0;
            } else if (frac > 0.06 && g_refresh_hz > 0) {
                // No VRR: the only fix is locking the input to a divisor of the refresh.
                logf_("[lock]     whole-vblank at %u Hz needs input %.2f fps (4x) or"
                      " %.2f (3x) or %.2f (2x) - see --list-modes\n",
                      g_refresh_hz, g_refresh_hz / 4.0, g_refresh_hz / 3.0,
                      g_refresh_hz / 2.0);
            }
        }
        const double jit_min = (g_arr_count > 0) ? g_arr_min : 0.0;
        const double jit_max = (g_arr_count > 0) ? g_arr_max : 0.0;
        g_arr_min = 1e9; g_arr_max = 0.0; g_arr_count = 0;
        const UINT drops = g_arr_drops; g_arr_drops = 0;
        const UINT slow = g_slow_frames; g_slow_frames = 0;
        const double budget = (input_interval_ms() > 1.0)
                            ? (g_ms_throttle + g_ms_present) / input_interval_ms() * 100.0 : 0.0;
        const double mx_gpu = g_max_gpu, mx_pres = g_max_present, mx_gap = g_max_gap;
        const double mx_wait = g_max_wait;
        const double mx_cw = g_max_copywait, mx_in = g_max_inputs, mx_tot = g_max_total;
        const double mx_cq = g_max_cq; g_max_cq = 0.0;
        const double mx_xe = g_max_xell, mx_fg = g_max_fg;
        const double mx_col = g_max_colour; g_max_colour = 0.0;
        const double mx_age = g_max_age; g_max_age = 0.0;
        // Everything run_frame() spends that no other field accounts for: the slot walk,
        // the descriptor/barrier setup, the present bookkeeping and this report itself.
        // If this grows, the next thing to look at is ours, not XeSS-FG's.
        const double accounted = g_ms_colour + g_ms_copywait + g_ms_work + g_ms_wait +
                                 g_ms_inputs + g_ms_cq + g_ms_of + g_ms_xell + g_ms_fg +
                                 g_ms_throttle + g_ms_present;
        const double cpu_only = (g_ms_total > accounted) ? (g_ms_total - accounted) : 0.0;
        const UINT x2m = g_x2_miss;
        g_hitch_logged = 0;
        g_max_gpu = 0.0; g_max_present = 0.0; g_max_gap = 0.0; g_x2_miss = 0;
        g_max_wait = 0.0; g_max_copywait = 0.0; g_max_total = 0.0; g_max_inputs = 0.0;
        g_max_xell = 0.0; g_max_fg = 0.0;
        const UINT pace_rep = static_cast<UINT>(g_represents); g_represents = 0.0;
        const UINT pace_sk = g_pace_skips; g_pace_skips = 0;

        // Factual one-shot note about who paces the loop: XeFG can only push
        // (interp+1) frames per input frame into the panel's vblanks, so the input rate
        // is topped out at refresh/(interp+1). On a 59 Hz panel at 4x that is ~15 fps
        // no matter how fast the game itself runs.
        // Only worth saying when the panel itself caps the profile (e.g. 59/60 Hz with
        // 4x): on a healthy 120 Hz run the input is *meant* to be 30 fps, so Present
        // blocking ~10 ms of 33 ms is the design, not a problem.
        if (!g_present_cap_warned && g_refresh_hz > 0 && g_refresh_hz < 100 &&
            g_ms_total > 1.0 && g_arrival_ms_ema > 1.0 &&
            g_ms_present > g_ms_total * 0.35 && g_ms_total > g_arrival_ms_ema * 0.85) {
            g_present_cap_warned = true;
            const double in_fps = 1000.0 / g_arrival_ms_ema;
            const double cap = (g_refresh_hz > 0)
                             ? g_refresh_hz / static_cast<double>(g_fg_interp_actual + 1) : 0.0;
            logf_("[lock] note: Present blocks %.1f of %.1f ms (%.0f%%), so the display side"
                  " paces the loop.", g_ms_present, g_ms_total,
                  g_ms_present / g_ms_total * 100.0);
            if (cap > 0.0) {
                logf_(" %ux output on a %lu Hz panel tops the input out at %.1f fps"
                      " (measured %.1f fps) - lower --interp to go higher, or raise the"
                      " refresh rate.", static_cast<unsigned>(g_fg_interp_actual + 1),
                      static_cast<unsigned long>(g_refresh_hz), cap, in_fps);
            }
            logf_("\n");
        }
        // Cross-adapter budget: the only per-frame traffic that leaves the render card.
        // MV is R16G16 (4 B/px) and depth R32 (4 B/px), and XeSS-FG wants both at the
        // stream size, so this line is exactly what StreamPercent / --no-depth change -
        // it is the number to watch when the goal is "less bus traffic", not `total`.
        {
            const double mv_mb  = static_cast<double>(g_geo.mv_w) * g_geo.mv_h * 4.0 / (1024.0 * 1024.0);
            const double dep_mb = static_cast<double>(g_geo.depth_w) * g_geo.depth_h * 4.0 / (1024.0 * 1024.0);
            const bool send_mv  = mv_from_producer();
            const bool send_dep = !g_no_depth && !g_no_inputs;
            const double per_frame = (send_mv ? mv_mb : 0.0) + (send_dep ? dep_mb : 0.0);
            const double rate = (g_arrival_ms_ema > 0.1)
                              ? per_frame * (1000.0 / g_arrival_ms_ema) : 0.0;
            logf_("[lock]   bus: mv %ux%u%s %.1fMB + depth %ux%u%s %.1fMB = %.1fMB/frame"
                  " (%.1f GB/s at %.1f fps; StreamPercent and --no-depth are the levers)\n",
                  g_geo.mv_w, g_geo.mv_h, send_mv ? "" : " (not sent)", send_mv ? mv_mb : 0.0,
                  g_geo.depth_w, g_geo.depth_h, send_dep ? "" : " (not sent)",
                  send_dep ? dep_mb : 0.0, per_frame, rate * 0.001,
                  (g_arrival_ms_ema > 0.1) ? 1000.0 / g_arrival_ms_ema : 0.0);
            // Render-card head room. There is no way to *reserve* a slice of the GPU (WDDM has
            // no per-process budget), so the only two levers are "make the vectors cheaper"
            // and "leave the card some time". Say so when the budget looks tight: 48 MB/frame
            // is ~1.5 GB/s at 30 fps and ~2 GB/s at 40 fps, i.e. a fifth of a frame period
            // spent moving vectors and depth around, on top of two full-screen passes.
            {
                const double share = (g_ms_total > 0.5)
                    ? (g_ms_inputs + g_ms_copywait) / g_ms_total : 0.0;
                if (per_frame >= 48.0 || share >= 0.25) {
                    logf_("[lock]   headroom: %.0f MB/frame of vectors+depth (%.0f%% of the"
                          " loop is inputs+copy). Lower [capture] StreamPercent (150 halves"
                          " it), drop depth with --no-depth, and cap the *game* frame rate"
                          " (driver Max Frame Rate) so the conversion pass always has a slot"
                          " - dropping frames after render (--lock) does not free GPU time\n",
                          per_frame, share * 100.0);
                }
            }
        }
        // Arrival histogram: where the input actually comes from. `need` is what the current
        // multiplier requires (interp+1 vblanks per input frame), so a bucket below it means
        // the presenter ran faster than we consume, and a bucket above it means frames were
        // late or gated. `n` is the number of arrivals in this window - if `n` is ~20% short
        // of `frames`, the input itself is not being delivered, not merely reprocessed.
        {
            UINT total_arr = 0;
            for (int k = 1; k <= 8; ++k) { total_arr += g_arr_hist[k]; }
            logf_("[lock]   arr(vb):");
            for (int k = 1; k <= 8; ++k) {
                if (g_arr_hist[k]) { logf_(" %dx=%u", k, g_arr_hist[k]); }
            }
            const UINT need = g_fg_interp_actual + 1;
            logf_("  n=%u  need %ux (%.1f ms)  frames=%llu  med dwm=%.1f wc=%.1f\n", total_arr,
                  need, need * refresh_ms(), static_cast<unsigned long long>(g_frames),
                  arr_median_ms(), arr_wc_median_ms());
            for (int k = 1; k <= 8; ++k) { g_arr_hist[k] = 0; }
            if (g_fg_rt_mode == kFgRtAuto) {
                const double vb = refresh_ms();
                // 64-sample median, once per report window (it is only used right here).
                g_rt_ema_slow = (g_arr_slow_n >= 32) ? arr_slow_median_ms() : g_rt_ema_fast;
                const double diff = dabs_(g_rt_ema_fast - g_rt_ema_slow);
                const bool moving = diff >= vb * 0.60;
                const int want = moving ? 1 : 0;
                // Decide once per report window (~1 s) and require three windows in a row:
                // the per-frame version flapped the pacer's input 4 times in 18 s.
                if (want != g_rt_auto_state) {
                    if (++g_rt_auto_agree >= 3) {
                        g_rt_auto_state = want;
                        g_rt_auto_agree = 0;
                        ++g_rt_auto_flips;
                        logf_("[lock] --fg-rt-mode auto: input rate is %s (median %.1f vs"
                              " %.1f ms, %.2f vblanks apart) -> feeding %s\n",
                              moving ? "moving" : "steady", g_rt_ema_fast, g_rt_ema_slow,
                              (vb > 0.5) ? diff / vb : 0.0, moving ? "raw" : "quant");
                    }
                } else {
                    g_rt_auto_agree = 0;
                }
                logf_("[lock]   rt auto: median %.1f vs %.1f (%.2f vblanks apart) -> %s"
                      "  flips=%u\n", g_rt_ema_fast, g_rt_ema_slow,
                      (vb > 0.5) ? diff / vb : 0.0,
                      (g_rt_auto_state == 1) ? "raw" : "quant", g_rt_auto_flips);
            }
            if (g_spike_count) {
                logf_("[lock]   rt spikes: %u frame(s) reported at their real length this"
                      " window (--fg-rt-spike %d)\n", g_spike_count, g_fg_rt_spike ? 1 : 0);
                g_spike_count = 0;
            }
            if (g_burst_count) {
                logf_("[lock]   bursts: %u frame(s) processed faster than 0.6x the input"
                      " interval this window (queued captures drained back to back)\n",
                      g_burst_count);
                g_burst_count = 0;
            }
            // The same buckets for the interval the producer measured between the game's own
            // Present calls. If this is flat while `arr(vb)` is not, the game is presenting
            // evenly and the unevenness appears after it (composition / our overlay).
            UINT game_total = 0;
            for (int k = 1; k <= 8; ++k) { game_total += g_game_hist[k]; }
            logf_("[lock]   game(vb):");
            for (int k = 1; k <= 8; ++k) {
                if (g_game_hist[k]) { logf_(" %dx=%u", k, g_game_hist[k]); }
            }
            logf_("  n=%u (producer measured)\n", game_total);
            for (int k = 1; k <= 8; ++k) { g_game_hist[k] = 0; }
        }
        // ---- "interpolation cannot help here" -------------------------------------------
        // With the WGC throttle gone (2026-11-10) an unlocked title can run well above half the
        // panel rate. XeFG cannot generate fewer than one frame per input, so 2x of an 80 fps
        // input asks a 120 Hz panel for 160 - it cannot show that, and the surplus becomes
        // duplicates: the picture is *worse* than a plain pass-through. Say so once instead of
        // silently mis-pacing (measured after the throttle fix: 1:1 felt clearly better).
        {
            static bool warned = false;
            const double vb_now = refresh_ms();
            const double tin_now = arr_mean_ms();
            if (!warned && tin_now > 0.5 && vb_now > 0.5) {
                const double per_input = tin_now / vb_now;      // frames needed to fill the panel
                if (per_input < 1.6) {
                    warned = true;
                    logf_("[lock] note: input ~%.1f fps is already %.0f%% of the %.0f Hz panel"
                          " (%.2f frames per input) - frame generation cannot help at this rate;"
                          " its extra frames only become duplicates. Switch the UI mode to"
                          " \"关闭 (1:1 直通)\" (or add --no-fg).\n",
                          1000.0 / tin_now, 100.0 / per_input, static_cast<double>(g_refresh_hz),
                          per_input);
                }
            }
        }

        // ---- output side: are the generated frames actually reaching the display? -----
        // Nothing above can see this: `xN` is XeFG's own accounting and `cadence` is derived
        // from the input median. Only the swap chain knows what it really handed to the
        // compositor, so count it. `expected` = frames this window * (interp+1) and `vblanks`
        // = refresh periods actually elapsed; presents << expected means XeFG is not
        // emitting the frames it charges us for (the picture judders while every other
        // counter looks perfect), presents > vblanks means they go out in bursts.
        {
            UINT last = 0;
            static UINT s_prev_last = 0;
            if (g_swapchain && SUCCEEDED(g_swapchain->GetLastPresentCount(&last))) {
                const UINT d = (s_prev_last && last >= s_prev_last) ? (last - s_prev_last) : 0;
                s_prev_last = last;
                const double vbs = (win > 0.0) ? win * static_cast<double>(g_refresh_hz) : 0.0;
                const double expect = static_cast<double>(g_report_every) *
                                      static_cast<double>(g_fg_interp_actual + 1);
                logf_("[lock]   out: presents=%u  expected=%.0f  vblanks=%.0f%s\n",
                      d, expect, vbs,
                      (d > 0 && static_cast<double>(d) + 0.5 < expect)
                          ? "  <== OUTPUT FRAMES MISSING" : "");
                // The honest output rate: what the swap chain actually handed to the
                // compositor per second of this window. This is what `show=` reports now.
                if (win > 0.05 && d > 0) {
                    g_show_fps = static_cast<int>(static_cast<double>(d) / win + 0.5);
                }
            }
            static bool s_stats_failed = false;
            DXGI_FRAME_STATISTICS fs{};
            if (!s_stats_failed) {
                if (g_swapchain && SUCCEEDED(g_swapchain->GetFrameStatistics(&fs))) {
                    static UINT s_pc = 0, s_prc = 0, s_src = 0;
                    const UINT pc  = (fs.PresentCount >= s_pc) ? fs.PresentCount - s_pc : 0;
                    const UINT prc = (fs.PresentRefreshCount >= s_prc)
                                   ? fs.PresentRefreshCount - s_prc : 0;
                    const UINT src = (fs.SyncRefreshCount >= s_src)
                                   ? fs.SyncRefreshCount - s_src : 0;
                    s_pc = fs.PresentCount; s_prc = fs.PresentRefreshCount;
                    s_src = fs.SyncRefreshCount;
                    // presentRefresh lags syncRefresh when the compositor had to skip a
                    // vblank: a *displayed* drop, invisible to every other counter.
                    logf_("[lock]   out2: count+%u  presentRefresh+%u  syncRefresh+%u%s\n",
                          pc, prc, src, (src > prc + 1) ? "  <== VBLANK MISSED" : "");
                } else {
                    s_stats_failed = true;
                    logf_("[lock]   out2: GetFrameStatistics unsupported on this swap chain\n");
                }
            }
        }
        // Process CPU for this report window: user+kernel time of *all* bridge threads
        // (WGC, the D3D driver's workers, our recording), not just the frame loop. This is
        // the number to compare with Task Manager; the per-stage fields below only cover
        // what run_frame() itself timed.
        {
            static ULONGLONG s_cpu_prev = 0, s_wall_prev = 0;
            FILETIME ct{}, et{}, kt{}, ut{};
            ULONGLONG cpu = 0;
            if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
                cpu = (static_cast<ULONGLONG>(kt.dwHighDateTime) << 32 | kt.dwLowDateTime) +
                      (static_cast<ULONGLONG>(ut.dwHighDateTime) << 32 | ut.dwLowDateTime);
            }
            const ULONGLONG wall = GetTickCount64();
            if (s_cpu_prev != 0 && wall > s_wall_prev) {
                const double cpu_ms = static_cast<double>(cpu - s_cpu_prev) / 10000.0;
                const double wall_ms = static_cast<double>(wall - s_wall_prev);
                const double pct1 = (wall_ms > 0.0) ? (cpu_ms / wall_ms * 100.0) : 0.0;
                const double per_frame = (g_report_every > 0)
                                       ? (cpu_ms / static_cast<double>(g_report_every)) : 0.0;
                logf_("[lock]   proc: cpu %.1f ms / %.0f ms wall = %.1f%% of one core,"
                      " %.2f ms/frame (all bridge threads)\n",
                      cpu_ms, wall_ms, pct1, per_frame);
            }
            s_cpu_prev = cpu;
            s_wall_prev = wall;
        }
        logf_("[lock]   breakdown: capture=%.1fms work=%.1fms present=%.1fms gpu=%.1fms"
              " wait=%.1fms copy=%.1fms colour=%.1fms mvs=%.1fms cq=%.1fms of=%.1fms age=%.1fms"
              " xell=%.1fms fg=%.1fms total=%.1fms cpu=%.1fms timeouts=%u mode=%d  xN=%.2f"
              "  game=%.1fms  jitter=%.1f..%.1fms  arr_gaps=%u  load=%.0f%%  slow=%u"
              "  pace=%.1fms repres=%u skip=%u"
              "  worst(gpu %.1f/pres %.1f/wait %.1f/copy %.1f/colour %.1f/mvs %.1f/cq %.1f"
              "/of %.1f/age %.1f"
              "/xell %.1f/fg %.1f/total %.1f/gap %.1f) xNmiss=%u\n",
              g_ms_capture, g_ms_work, g_ms_present, g_ms_throttle, g_ms_wait,
              g_ms_copywait, g_ms_colour, g_ms_inputs, g_ms_cq, g_ms_of, g_ms_age, g_ms_xell,
              g_ms_fg, g_ms_total, cpu_only,
              g_slot_timeouts, g_colour_mode, xn, g_arrival_ms_ema, jit_min, jit_max,
              drops, budget, slow, g_pace_ms, pace_rep, pace_sk,
              mx_gpu, mx_pres, mx_wait, mx_cw, mx_col, mx_in, mx_cq, g_max_of, mx_age, mx_xe,
              mx_fg, mx_tot, mx_gap, x2m);
        // Codec budget watchdog: the decode runs on THIS GPU, so a machine that is already
        // saturated pays for it. Switching streams mid-session is impossible (the slot type
        // is part of the layout), so this only reports and recommends the ini change.
        if (mv_codec_shared() || depth_codec_shared()) {
            static int hot_windows = 0;
            static bool told_hot = false;
            hot_windows = (budget >= 85.0) ? hot_windows + 1 : 0;
            if (hot_windows >= 3 && !told_hot) {
                told_hot = true;
                logf_("[lock] WARNING: load stayed >=85%% with the fr codec on - this GPU is"
                      " paying for the decode. Set MvCodec=0 / DepthCodec=0 in fakerFG.ini"
                      " and restart to fall back to the cross-adapter copy path\n");
            }
        }
        // --prefetch-inputs hit/miss for this window (only worth a line when it is on).
        if (g_pf_pub) {
            static UINT s_pf_h = 0, s_pf_m = 0, s_pf_d = 0;
            logf_("[lock]   pf: %u frame(s) served from the publication cache, %u missed"
                  " (empty queue), %u dropped by overflow; queue now %u (this window)%s\n",
                  g_pf_hits - s_pf_h, g_pf_misses - s_pf_m,
                  g_fifo_drops - s_pf_d, g_pub_q_n,
                  g_pub_fifo ? "" : " [key mode]");
            s_pf_h = g_pf_hits;
            s_pf_m = g_pf_misses;
            s_pf_d = g_fifo_drops;
        } else if (g_prefetch_inputs) {
            static UINT s_pf_h = 0, s_pf_m = 0;
            logf_("[lock]   pf: %u frame(s) served from a prefetched copy, %u had to copy"
                  " in-frame (this window)\n", g_pf_hits - s_pf_h, g_pf_misses - s_pf_m);
            s_pf_h = g_pf_hits;
            s_pf_m = g_pf_misses;
        }
        // --slack-auto: how many frames were absorbed instead of dropped this window, and how
        // many were still dropped - split into "carried new content" (a real lost input frame)
        // and "duplicate" (free). Only printed when it is on.
        if (g_slack_auto) {
            static UINT s_hold = 0, s_drop = 0, s_real = 0;
            const UINT dh = g_slack_auto_holds - s_hold;
            const UINT dd = g_slack_auto_drops - s_drop;
            const UINT dr = g_slack_auto_real - s_real;
            logf_("[lock]   slack-auto: %u held back (would have been dropped), %u dropped"
                  " [%u real loss / %u duplicate]  (this window)\n", dh, dd, dr, dd - dr);
            s_hold = g_slack_auto_holds;
            s_drop = g_slack_auto_drops;
            s_real = g_slack_auto_real;
        }

        // ---- end-to-end latency estimate, measured from inside the process -----
        //
        // Everything below comes from timestamps we already collect, so nothing has
        // to hook Present (an external tool would disturb presentation and change
        // what it measures - see docs/findings/MFG.md §3.3):
        //
        //   age            WGC's own SystemRelativeTime -> the moment we pick the
        //                  frame up. That is the game compose -> our process hop.
        //   total          our whole run_frame, ending when Present() returned.
        //   scanout        0 .. one refresh interval: the newest frame of the batch
        //                  reaches the panel within one vblank.
        //
        // The game's own input->compose latency is NOT visible here (it needs a
        // click timestamp), so it is stated separately as ">= one input frame".
        if (g_ms_total > 0.0) {
            const double scanout = (g_refresh_hz > 0) ? 1000.0 / g_refresh_hz : 8.33;
            const double input_ms = (g_arrival_ms_ema > 1.0) ? g_arrival_ms_ema : 0.0;
            // --slack holds frames back on purpose; that is latency we added, so it
            // has to show up here instead of being hidden.
            const double queue_ms = (input_ms > 0.0) ? g_slack * input_ms : 0.0;
            const double compose_to_present = g_ms_age + g_ms_total + queue_ms;
            if (queue_ms > 0.0) {
                logf_("[lock] LATENCY: compose->present %.1fms (game->wgc %.1f + bridge %.1f"
                      " + slack %.1f) | +scanout 0..%.1f => **compose->photons %.1f..%.1fms**\n",
                      compose_to_present, g_ms_age, g_ms_total, queue_ms, scanout,
                      compose_to_present, compose_to_present + scanout);
            } else {
                logf_("[lock] LATENCY: compose->present %.1fms (game->wgc %.1f + bridge %.1f)"
                      " | +scanout 0..%.1f => **compose->photons %.1f..%.1fms**\n",
                      compose_to_present, g_ms_age, g_ms_total, scanout,
                      compose_to_present, compose_to_present + scanout);
            }
            if (input_ms > 0.0) {
                logf_("[lock]          + game input->compose >= %.1fms (1 input frame,"
                      " not measurable here) => **est. click->photons >= %.1fms**\n",
                      input_ms, compose_to_present + input_ms);
            }
        }
    }
}


// ------------------------------------------------------------------ fg probe

