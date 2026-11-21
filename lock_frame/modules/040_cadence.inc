// ============================================================================
//  lock_frame module: 040_cadence.inc
//
//  Cadence estimation, render-time feeds, MFG count controller, smoothness
//  helpers.
//
//  Included by lock_frame/lock_frame.cpp as part of the single-translation-unit build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

// ------------------------------------------------- cadence (smoothness) helpers
//
// Why this exists: XeSS-FG paces generated frames into the panel's vblanks, so the
// output is only *evenly* spaced when the output interval T_in/(N+1) is a whole
// number of vblanks. 4x of a locked 30 fps on a 120 Hz panel is exactly 8.33 ms =
// one vblank (smooth). A *dynamic* 38 fps gives 6.6 ms = 1.26 vblanks, and the
// pacer can only alternate 1,1,1,2 - no stutter, but visibly uneven. That is the
// "dynamic frame time looks unsmooth" symptom, and it is arithmetic, not MV/depth.
//
// Two knobs, both off by default (docs/findings/FINAL_CONFIG.md 4.2):
//   --fg-rt-mode ema|raw|median|quant   what we hand XeFG as frameRenderTime
//   --interp-auto [floor|round]         keep the output on a whole-vblank cadence
enum { kFgRtEma = 0, kFgRtRaw = 1, kFgRtMedian = 2, kFgRtQuant = 3, kFgRtAuto = 4 };
// v30: auto is the default and it is a CONTINUOUS feed, not a mode (see rt_auto_feed_ms):
//   - never below m*vblank, so the panel is never asked for more than one output per vblank
//     (asking for less is what makes the panel drop, which is worse than any hold);
//   - a 2% deadband keeps the 39-40 fps working point on the plan;
//   - when a scene is slower, the feed ramps up by at most 0.5 ms per batch, so a scene
//     change is a 0.15 s ramp instead of a visible step.
// Measured on Wuchang: individual intervals of 17-39 ms around a 25 ms median must never be
// fed raw (17 ms asks for 0.68 vblank per output); the median + ramp handles that.
static int g_fg_rt_mode = kFgRtAuto;   // see fg_render_time_ms / rt_* below
// The legacy WGC fast/slow pair, kept as auto's fallback until the game clock has a sample
// (and as the fast/slow flag in the report). Two EMAs; when they disagree by more than a
// quarter of a vblank the rate is moving (unlocked / GPU-bound).
static double g_rt_ema_fast = 0.0;
static double g_rt_ema_slow = 0.0;
static int    g_rt_auto_state = 0;      // 0 = rate is steady (quant), 1 = rate is moving (raw)
static int    g_rt_auto_agree = 0;      // consecutive frames agreeing to flip (hysteresis)
static UINT   g_rt_auto_flips = 0;
// --fg-out-hz N: cap the output rate (0 = the panel refresh, i.e. fill every vblank).
// Only meaningful together with --interp-cadence, where the per-frame frame count is ours
// to choose: "120 Hz panel but I only want 60 fps" then comes out as 2x for a 30 fps input
// instead of 4x. The floor is 2x (libxess_fg cannot generate zero frames); if the target
// needs fewer, bridge warns once and says what rate it will actually run at.
static double g_fg_out_hz = 0.0;
// --fg-cad-rt 0|1 (default 1): in --interp-cadence mode derive frameRenderTime from the
// batches themselves (k * vblank) instead of from the median interval.
//
// This is the fix for "the picture judders while the input rate fluctuates": cadence asks
// XeFG for k frames because k vblanks actually elapsed, but the pacer was still being told
// the *median* length. Whenever the two disagree (a 33.3 ms gap while the median is 25 ms)
// the pacer budgets the wrong time for the batch and has to hold or rush a frame, which is
// visible. Feeding k * vblank makes the two agree by construction - it is exactly the
// interval those k frames have to cover.
static UINT   g_cad_last_k = 0;
static double g_cad_last_tin = 0.0;   // the smoothed input interval behind that batch
// Measured in a heavily fluctuating scene (2..6 vblanks between inputs): feeding k*vblank
// raised the empty-refresh rate from 1.0% to 2.6% and produced one 37-vblank hole, while the
// median feed stayed at 1.0%. So this is OFF by default and exists only for A/B.
// OFF by default (2026-11-09 eye test): feeding k * vblank looks right on paper - the budget
// then matches the batch - but Present() blocks on the pacer's budget, and we measure the
// input interval at the *next* WGC arrival, so a longer budget inflates the very measurement
// that chooses k. With queue depth 1 the input rate is our own throttle, i.e. the loop has no
// fixed point. Measured: xN drifted 3.00 -> 3.05/3.38 while the input median stayed at
// 25.0 ms, and the picture surged. The median feed holds xN at exactly 3.00. Keep this as an
// A/B switch only (bin/6 - cadence batch feed (A-B).bat).
static bool   g_fg_cad_rt = false;
static bool   g_cad_rt_logged = false;
// --cadence-hold N (default 15 frames, 0/1 = off): how long to keep the interpolated-frame
// count before the cadence logic is allowed to change it again.
//
// Why: changing that count is expensive (the guide says it may reload the model / recompile
// shaders), and with a fluctuating input the phase accumulator asks for a different count
// almost every frame - measured 24-32 switches per 1 s window (40 fps input), which is
// exactly the "stutters even though no vblank is empty" the ear sees. Holding the count for
// ~15 frames and then stepping by at most one keeps the long-run average (the phase keeps
// accumulating) while making the picture's rhythm steady.
// --cadence-smooth N (default 8 frames, 1 = raw): smooth the arrival interval feeding the
// phase accumulator over ~N frames.
//
// Why: feeding the *raw* per-frame interval makes the batch size follow the jitter 1:1 - one
// late frame asks for 4-5 generated frames, the next fast one asks for 2. The long-run average
// stays right, but the picture's rhythm changes on almost every frame (measured 17-32 count
// switches per 1 s window while the input fluctuates 2..6 vblanks), and that is what reads as
// "一卡一卡".
//
// Default 0 since 2026-11-10: stability now comes from the *median* (arr_median_ms, 16 samples)
// alone. Stacking an EMA on top of a median added ~0.5-0.8 s of lag, so a real rate change
// (60 -> 40 fps) kept the count too low for about a second - visible as the picture running
// slightly slow before it caught up. Set >1 only to damp a title whose median still moves.
static int    g_cad_smooth = 0;
static double g_cad_arr_s = 0.0;
static int    g_cad_hold_frames = 0;
static int    g_cad_hold = 0;
static bool   g_out_hz_floor_warned = false;
// A longer ring for the "slow" rate. Per-sample EMAs could not tell a locked 40 fps (which
// arrives as 2/3/4 vblanks) from a rate that is actually moving: the +-1 vblank jitter of a
// locked input drives a fast EMA just as much as a real step does - measured, it flipped a
// locked 40 fps to "raw". Medians are immune to that jitter (it is exactly why quant uses
// one), so the decision compares a short median against a long one.
#define kArrSlowRing 64
static double g_arr_slow_ring[kArrSlowRing] = {};
static UINT   g_arr_slow_n = 0;
static UINT   g_arr_slow_i = 0;

static double arr_slow_median_ms()
{
    if (g_arr_slow_n == 0) { return 0.0; }
    static double v[kArrSlowRing];
    const UINT n = g_arr_slow_n;
    for (UINT i = 0; i < n; ++i) { v[i] = g_arr_slow_ring[i]; }
    for (UINT i = 1; i < n; ++i) {
        const double key = v[i];
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; --j; }
        v[j + 1] = key;
    }
    return v[n / 2];
}

// --fg-rt-spike 0/1 (default 1): when ONE input frame arrives much later than the plan
// (a fast camera turn, a streamed asset), report the real length to XeFG for that frame so
// the pacer spreads its batch over the gap instead of laying it out too short and leaving an
// empty refresh period. This is the difference between "the game dipped to 57 fps" and "the
// picture juddered". See fg_render_time_ms().
static bool   g_fg_rt_spike = true;
static const double kSpikeRatio = 1.35;   // last interval this much longer than the plan
static int    g_spike_logs_left = 8;      // log the first few, then stay quiet
static UINT   g_spike_count = 0;          // spikes this report window
// How often we processed two captured frames closer together than 0.6x the input interval.
// That happens after a stall: WGC has several frames queued and the loop drains them back to
// back (each one with its own Present). It is what made `show` read 148 fps on a 120 Hz panel,
// so it is worth counting explicitly instead of guessing.
static UINT   g_burst_count = 0;

// v30 auto: ONE continuous, slew-limited feed instead of a quant/raw mode.
//
// The physical constraint: XeFG lays m outputs over whatever we report, so asking for less
// than m*vblank makes the panel drop, and asking for more than the real gap leaves a hold.
// A mode switch (quant <-> raw) is itself a step, which is what a scene change made visible.
//
//   target = max(m*vblank, median)   // never below the plan: 1 output per vblank
//   a 2% deadband keeps a 39-40 fps working point on the plan
//   feed grows at most 0.5 ms per batch, shrinks immediately (a smaller feed never over-asks)
//
// So a scene that drops from 40 to 36 fps ramps 25.0 -> 27.8 ms over ~6 batches (0.15 s)
// instead of flipping mode: spacing stays <= 1 output per vblank the whole time, and the
// residual is a constant, bounded motion-speed error instead of per-batch jitter.
static double game_median_ms();   // defined in 140_frame.inc, declared before its first use
static double rt_quant_value();   // defined below, before fg_render_time_ms()

static double rt_auto_feed_ms()
{
    const double vb = refresh_ms();
    const int m = static_cast<int>(g_fg_interp_actual) + 1;
    if (!(vb > 0.5) || m < 2) { return rt_quant_value(); }
    const double plan = vb * static_cast<double>(m);
    const double med = game_median_ms();

    double target = plan;
    if (med > 0.5 && med < 200.0) { target = (med > plan) ? med : plan; }
    if (target < plan * 1.02) { target = plan; }        // deadband at the working point

    static double s_feed = 0.0;
    if (s_feed <= 0.0) { s_feed = plan; }
    if (target <= s_feed) { s_feed = target; }          // recover instantly
    else { s_feed = (target < s_feed + 0.5) ? target : s_feed + 0.5; }
    return s_feed;
}

static const char* fg_rt_mode_name()
{
    switch (g_fg_rt_mode) {
    case kFgRtRaw:    return "raw";
    case kFgRtMedian: return "median";
    case kFgRtAuto:   return "auto";
    case kFgRtQuant:  return "quant";
    default:          return "ema";
    }
}
// Last input intervals, for a median that is robust against WGC's own +-1 vblank
// quantization (a game at 35 fps on 120 Hz reaches us as 25/33/25 ms, not 28.6).
#define kArrRing 16
static double g_arr_ring[kArrRing] = {};
static UINT   g_arr_ring_n = 0;
static UINT   g_arr_ring_i = 0;
static double g_arr_last_raw = 0.0;

// Interval between WGC frame arrivals (i.e. the game's own frame time), EMA'd.
// Declared here because the cadence helpers need it; the frame-loop section below
// still owns updating it (note_arrival).
static double g_arrival_ms_ema = 26.0;

static void arr_push(double dt_ms)
{
    if (!(dt_ms > 0.0) || dt_ms > 1000.0) { return; }
    g_arr_ring[g_arr_ring_i] = dt_ms;
    g_arr_ring_i = (g_arr_ring_i + 1) % kArrRing;
    if (g_arr_ring_n < kArrRing) { ++g_arr_ring_n; }
    g_arr_last_raw = dt_ms;
}

// ============ --interp-cadence: variable-rate (per-frame) generation ========
//
// The problem this solves: a *constant* multiplier only lands on whole vblanks when
// the input rate divides the refresh exactly, so with UE5's wandering frame time the
// choice is either "lock the game (throws GPU headroom away)" or "accept judder".
//
// The multiplier does not have to be constant. The pacer shows at most one frame per
// vblank, so if the number of vblanks between two real frames is k, asking XeFG for
// k-1 generated frames makes every output frame land on its own vblank - whatever the
// input rate is. The output then runs at the refresh rate and the input is free to
// wander. A phase accumulator (Bresenham) keeps the long-run average exact:
//
//     phase += T_in / vblank
//     k     = round(phase)                 frames to output for this real frame
//     phase -= k                           carry the remainder into the next frame
//     interp = clamp(k - 1, 1, max)
//
// So 26 fps on 120 Hz alternates 5x/5x/4x/5x... (avg 4.615 frames per input frame =
// 120 fps), 35 fps alternates 4x/3x, and nothing is locked: 20..60 fps input all
// land on exactly one vblank per output frame.
//
// Cost to watch (this is why the counter exists): the XeSS-FG guide says changing the
// count "may be expensive (may switch to a different model and trigger shader
// compilation)". In this build every generated frame uses the 2x model, so a count
// change may only change the number of dispatches - but that is an assumption, and
// `switch_fail`/`gpu`/`HITCH` in the log is how we find out. If it turns out to be
// expensive, use --interp-auto instead (it changes the count at most every 2 windows).
static bool   g_interp_cadence = false;
static double g_cad_phase = 0.0;
static UINT   g_cad_hist[8] = {};        // how often each count was asked for
static UINT   g_cad_switches = 0;        // count changes actually issued
static UINT   g_cad_fail = 0;            // ... that the DLL refused
// (g_dyn_mfg is declared above, next to fg_apply_interp.)

// Mean of the same ring, with each sample clamped to 3x the median so one alt-tab or asset
// stream does not buy a dozen generated frames.
//
// Why the cadence needs the *mean* and not the median (2026-11-10): the input distribution is
// right-skewed - mostly 25 ms with frequent 33-42 ms frames. The median therefore always reads
// 25.0 ms (3.00 vblanks) while the real average was 28.1 ms (3.375 vblanks), so the accumulator
// asked for 3 frames per input and left ~12% of refresh periods showing a duplicate:
//
//   out: presents=120  expected=120  vblanks=135      <- 40 inputs x 3 frames, but 135 vblanks
//
// XeFG emitted every frame it was asked for; the count was simply too low for the elapsed time.
// Averaging the ring gives "how many vblanks per input on average", which is exactly what
// "fill every vblank" needs. Stability still comes from the bounded phase + one-step slew.
static double arr_mean_ms()
{
    if (g_arr_ring_n == 0) { return g_arrival_ms_ema; }
    const double med = arr_median_ms();
    const double cap = (med > 0.5) ? (3.0 * med) : 0.0;
    double sum = 0.0;
    for (UINT i = 0; i < g_arr_ring_n; ++i) {
        double v = g_arr_ring[i];
        if (cap > 0.0 && v > cap) { v = cap; }
        sum += v;
    }
    return sum / static_cast<double>(g_arr_ring_n);
}

// --cad-batch-rt: in cadence mode, pace each batch over the vblanks it actually covers
// (k * vblank) instead of over the measured arrival interval, and take the input rate from the
// *producer's own present interval* (FrameShared.frame_time_ms) rather than from our WGC
// arrival times.
//
// Why both changes together (measured 2026-11-10): a batch of 3 frames spread over a 28.3 ms
// mean gives 9.4 ms per frame while a batch of 4 gives 7.1 ms - the spacing alternates instead
// of being one frame per vblank, which is the residual judder in dynamic mode. k*vblank fixes
// the spacing, but on 2026-11-08 it drifted (xN 3.00 -> 3.38) because our own Present blocking
// inflates the arrival interval we measure - a feedback loop. Measuring on the producer's clock
// removes that coupling: the game's present interval does not depend on what we do.
static bool g_cad_batch_rt = false;

// Newest published frame's present interval, in ms (0 = unusable). Slot-agnostic on purpose:
// the bridge picks its slot after this decision, and the interval is the same for all of them.
static double producer_interval_ms()
{
    if (!g_ctl) { return 0.0; }
    uint64_t newest = 0;
    double ms = 0.0;
    for (UINT i = 0; i < mgpu::kMaxSlots; ++i) {
        const mgpu::FrameShared& f = g_ctl->frames[i];
        if (f.published == 0) { continue; }
        if (f.frame_id >= newest) { newest = f.frame_id; ms = static_cast<double>(f.frame_time_ms); }
    }
    return (ms > 3.0 && ms < 100.0) ? ms : 0.0;
}

// Phase-accumulator step; returns the count to use for this real frame.

//
// This is the feature that exists for an **unlocked, fluctuating** frame rate, so the two
// things that matter are (a) one trustworthy estimate of "what rate is the input running at"
// and (b) never scheduling more output frames than the panel has vblanks to show.
//
// 2026-11-10 (Wuchang/PBZ, 120 Hz panel, 35-50 fps input) it did neither: it accumulated a
// *single-sample* interval (8-sample EMA) while the log line printed a 16-sample median, and
// the count was capped only by the DLL's ceiling. The log said it plainly:
//
//   input=25.0ms = 3.00 vblanks -> avg 5.00x, 200.0 fps out = 0.60 vblank/frame NOT-EXACT
//   input=16.7ms = 2.00 vblanks -> avg 2.98x, 178.5 fps out = 0.67 vblank/frame NOT-EXACT
//   input=25.0ms = 3.00 vblanks -> avg 3.00x, 120.0 fps out = 1.00 vblank/frame   (correct)
//
// 200 fps on a 120 Hz panel means XeFG spreads its batch over more time than it has: some
// refresh periods show two frames, others none. That is the judder.
static UINT cadence_next_slots()
{
    const double vb = refresh_ms();
    // The *mean*, not the median: this accumulator answers "how many output frames is the time
    // since the last input frame worth", and on a right-skewed distribution the median
    // under-counts by ~12% (see arr_mean_ms). A single long frame is clamped inside the mean so
    // it cannot buy a dozen frames.
    double t_in = arr_mean_ms();
    if (t_in < 0.5) { t_in = (g_arr_last_raw > 0.5) ? g_arr_last_raw : (1000.0 / 60.0); }
    // --cad-batch-rt: measure on the game's clock, not on ours (see producer_interval_ms).
    static bool prod_logged = false;
    if (g_cad_batch_rt) {
        const double p = producer_interval_ms();
        if (p > 0.0) {
            t_in = p;
            if (!prod_logged) {
                prod_logged = true;
                logf_("[lock] --cad-batch-rt: input rate from the producer's own present"
                      " interval (%.2f ms), batch paced over k vblanks\n", p);
            }
        }
    }
    // Optional extra smoothing (g_cad_smooth > 1) on top of the median.
    if (g_cad_smooth > 1) {
        if (g_cad_arr_s <= 0.5) { g_cad_arr_s = t_in; }
        else {
            const double a = 2.0 / (static_cast<double>(g_cad_smooth) + 1.0);
            g_cad_arr_s += (t_in - g_cad_arr_s) * a;
        }
        t_in = g_cad_arr_s;
    }

    // Target output rate. Without --fg-out-hz this is the panel refresh, i.e. "fill every
    // vblank". `want` is how many output frames this input interval is worth - fractional.
    const double panel_hz = (vb > 0.5) ? 1000.0 / vb : 120.0;
    const double out_hz = (g_fg_out_hz > 1.0) ? g_fg_out_hz : panel_hz;
    const double want = t_in * out_hz / 1000.0;

    g_cad_phase += want;
    int k = static_cast<int>(g_cad_phase + 0.5);

    // --vrr: stable count from the moving average, plus a one-shot fill for a genuinely
    // late input.
    //
    // History matters here, because two simpler versions were both worse:
    //   * k = round(mean/vb), rt = mean            -> smooth normally, but a dropped input
    //     (56 ms instead of 28.6) was told "this batch covers 28.6 ms" and the picture sat
    //     still for the rest = a stall ("一 drop 就跳").
    //   * k = floor(raw_gap/vb), rt = raw_gap      -> follows drops, but 40 fps sits exactly on
    //     25.0 ms = 3.000 vblanks, so 24.9 ms floors to 2 and 25.1 ms to 3: k and the budget
    //     wiggled every frame and it was visibly *worse* ("反而变得更卡").
    //
    // So: the normal path is driven by the mean (stable), and only a gap that is clearly
    // abnormal (> 1.4x the mean) is treated as a dropped frame and filled proportionally.
    if (g_vrr) {
        int k = static_cast<int>(t_in / vb + 0.5);
        if (k < 2) { k = 2; }
        const int vmax = static_cast<int>(g_fg_interp_max) + 1;
        if (k > vmax) { k = vmax; }

        // Over-refresh guard *only*. An earlier version capped k by floor(gap / vblank) from the
        // instantaneous gap, which is the wrong quantity: about half of the individual gaps are
        // shorter than the mean, so the cap kept pulling the count down and the picture fell
        // back to leaving holes ("变成和之前取中值的时候一样了", 2026-11-10). The guard now only
        // triggers when the batch would exceed the panel rate by more than 10% (e.g. 240 Hz panel,
        // 10.5 ms gap, k=3 -> 285 fps); normal alternation (3/4 at 28.1 ms on 120 Hz) is untouched.
        const double gap = (g_arr_last_raw > 1.0) ? g_arr_last_raw : t_in;
        auto over_refresh = [&](int kk) {
            return (static_cast<double>(kk) * vb) > (gap * 1.10);
        };
        if (over_refresh(k)) {
            int kf = static_cast<int>(gap / vb);
            if (kf < 2) { kf = 2; }
            k = kf;
        }

        double rt = t_in;                       // the paced budget for this batch
        const double ratio = (t_in > 0.5) ? (gap / t_in) : 1.0;
        // ---- fast path for a load change ------------------------------------------------
        // The count comes from a ~400 ms moving average, which is right for jitter but wrong
        // for a *step*: entering a heavy scene (40 -> 20 fps) or leaving it (20 -> 45) leaves
        // the average stale for about half a second, and every batch in that window is the
        // wrong size - too small (holes, the picture stalls) or too large (over-generated, the
        // picture surges). So when the last gap is clearly off (slower or *faster*), size this
        // one batch from the gap itself. In range (0.7..1.4x) nothing changes: that is the
        // hysteresis band that keeps 25.0 ms vs 25.1 ms from wiggling the count.
        // Without this a dropped input keeps the old count and the last generated frame is shown
        // for the whole extra gap - the stall reported as "一 drop 就跳". Measured again
        // 2026-11-10: switching it off only made it worse, so the fill is not optional.
        if (ratio > 1.4 || ratio < 0.7) {
            int kd = static_cast<int>(gap / vb + 0.5);
            if (kd < 2) { kd = 2; }
            if (kd > vmax) {
                static bool capped = false;
                if (!capped) {
                    capped = true;
                    logf_("[lock] --vrr: the input just needed %d frames (%.1f ms = %.1f vblanks)"
                          " but this DLL allows %d - the output will run slower for a moment"
                          " (raise --interp)\n", kd, gap, gap / vb, vmax);
                }
                kd = vmax;
            }
            // Same over-refresh guard as above, so a burst of fast frames cannot push the
            // output over the refresh rate either.
            if (over_refresh(kd)) {
                int kf = static_cast<int>(gap / vb);
                if (kf < 2) { kf = 2; }
                if (kf < kd) { kd = kf; }
            }
            if (kd != k) {
                static int logs = 8;
                if (logs > 0) {
                    --logs;
                    logf_("[lock] --vrr: load change (%.1f ms vs %.1f ms average, %.2fx) ->"
                          " %d frames over %.1f ms (%.2f ms apart)%s\n",
                          gap, t_in, ratio, kd, gap, gap / static_cast<double>(kd),
                          (logs == 0) ? " (further changes not logged)" : "");
                }
            }
            k = kd;
            rt = gap;                       // this batch really covers this gap
        }

        static bool once = false;
        if (!once) {
            once = true;
            logf_("[lock] --vrr: %d frame(s) per %.1f ms = %.2f ms apart (%.1f fps out);"
                  " vblank=%.2fms vmax=%d; late inputs get proportionally more frames\n",
                  k, t_in, t_in / static_cast<double>(k),
                  1000.0 * static_cast<double>(k) / t_in, vb, vmax);
        }
        g_cad_phase = 0.0;
        g_cad_last_k = static_cast<UINT>(k);
        g_cad_last_tin = rt;            // frameRenderTime = the interval this batch covers
        return static_cast<UINT>(k);
    }

    // Hard cap: never schedule more frames than the vblanks that actually elapsed (rounded up,
    // because a fraction of a vblank still needs one frame). Without this the accumulator ran
    // ahead and the output overshot the panel - the 0.60/0.67 vblank/frame lines above.
    int cap = static_cast<int>(want + 0.999);
    if (cap < 2) { cap = 2; }                   // libxess_fg cannot generate zero frames
    const int kmax = static_cast<int>(g_fg_interp_max) + 1;
    const bool capped_by_dll = (cap > kmax);
    if (capped_by_dll) { cap = kmax; }
    if (k > cap) { k = cap; }

    if (k < 2) {
        if (!g_out_hz_floor_warned) {
            g_out_hz_floor_warned = true;
            logf_("[lock] --fg-out-hz %.0f: a %.1f ms input needs %u generated frame(s),"
                  " but 2x is the minimum here - the output will run at ~%.0f Hz instead."
                  " Use --no-fg for 1:1.\n",
                  out_hz, t_in, (want < 1.0) ? 1u : static_cast<UINT>(want), 2000.0 / t_in);
        }
        k = 2;
    }

    // Bresenham carry, bounded to one frame: a bounded phase means the *next* k can only move
    // by one step, so a rate change walks 2->3->4 instead of jumping (the caller additionally
    // rate-limits the DLL re-configuration, see g_cad_hold_frames).
    g_cad_phase -= static_cast<double>(k);
    if (g_cad_phase > 0.5) { g_cad_phase = 0.5; }
    if (g_cad_phase < -0.5) { g_cad_phase = -0.5; }

    g_cad_last_k = static_cast<UINT>(k);      // frameRenderTime follows this (see fg_render_time_ms)
    g_cad_last_tin = t_in;
    return static_cast<UINT>(k);
}

static double arr_median_ms()
{
    if (g_arr_ring_n == 0) { return g_arrival_ms_ema; }
    double v[kArrRing];
    const UINT n = g_arr_ring_n;
    for (UINT i = 0; i < n; ++i) { v[i] = g_arr_ring[i]; }
    for (UINT i = 1; i < n; ++i) {              // n <= 16: insertion sort is fine
        const double k = v[i];
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; --j; }
        v[j + 1] = k;
    }
    return v[n / 2];
}

static double refresh_ms()
{
    return (g_refresh_hz > 0) ? 1000.0 / static_cast<double>(g_refresh_hz) : 1000.0 / 120.0;
}

// What goes into xefg_swapchain_frame_constant_data_t::frameRenderTime. The guide:
// "On non-Intel GPUs, XeSS-FG can use frameRenderTime to sanity-check presentation
// pacing" - this is the one number that tells the pacer what we think the input
// cadence is, so an EMA with a ~330 ms tail (alpha 0.1 at 30 fps) is exactly wrong
// when the frame time moves. OptiScaler feeds the game's raw value and only
// auto-detects seconds vs milliseconds.
// --input-ms X: pretend the input interval is X ms instead of the measured arrival gap.
// Two uses: (a) a game whose frame pacing is erratic (22-28 ms jitter) can be treated as a
// steady 25 ms; (b) it decides when `slow`/`load` complain and what frameRenderTime XeFG
// sees. It does NOT create GPU time - if the work genuinely needs 27 ms in a 25 ms slot the
// frames still arrive late, they are just no longer reported as "slow".
static double g_input_ms_override = 0.0;

static double input_interval_ms()
{
    return (g_input_ms_override > 1.0) ? g_input_ms_override : g_arrival_ms_ema;
}

// Raw: the interval the producer actually delivered. Most responsive, and the noisiest -
// it feeds one sample of jitter straight into the pacer's plan.
//
// v29: "delivered" now means the *game's* clock (frames[].frame_qpc_ms, 1 ms resolution),
// not the WGC arrival (SystemRelativeTime, quantised to whole vblanks). The WGC value is
// only a fallback for the first frames and for the - no-producer case.
// The game-clock helpers live in 140_frame.inc (this module is earlier in the TU), so they
// are forward-declared here. They return 0 until a sample exists.
static double game_raw_ms();
static double game_ema_ms();
static double game_median_ms();
static int    game_rate_moving();   // 1 = rate moving, 0 = steady, -1 = no data yet

static double rt_raw_value()
{
    const double g = game_raw_ms();
    if (g > 0.5) { return g; }
    return (g_arr_last_raw > 0.0) ? g_arr_last_raw : g_arrival_ms_ema;
}

// Quant: 16-sample median snapped to whole vblanks (plus the single-frame spike handling
// below). This is the cadence the pacer can actually lay out, so its sanity check stops
// fighting the real pacing.
static double rt_quant_value()
{
    const double vb = refresh_ms();
    // v29: snap the *game's* median; the WGC median is vblank-quantised already, which
    // is exactly the information loss that made 39 fps look like a locked 40.
    const double gm = game_median_ms();
    const double base_ms = (gm > 0.5) ? gm : arr_median_ms();
    const double last_ms = (game_raw_ms() > 0.5) ? game_raw_ms() : g_arr_last_raw;
    int k = static_cast<int>(base_ms / vb + 0.5);
    if (k < 1) { k = 1; }
    const double plan = static_cast<double>(k) * vb;

    // ---- single-frame spikes -------------------------------------------------
    // A fast camera turn (or a streamed asset) makes the game miss its slot once: the
    // input arrives 2 vblanks late instead of 1. Telling XeFG the *plan* length for that
    // frame is what turns one late input into an empty refresh period - it lays its batch
    // out as if it had `plan` ms and the rest of the gap shows nothing (measured:
    // `out: vblanks > presents`, the picture hitches).
    // Feeding the real, vblank-snapped length instead lets the same batch be spread over
    // the longer gap: motion is a touch slower for that one interval, but there is no
    // hitch. That is the difference between "the game dipped to 57 fps" and "the picture
    // juddered".
    //
    // NOT in cadence mode (2026-11-10). There the batch size comes from the phase accumulator,
    // and this branch would hand XeFG a *different* interval than the one the batch covers -
    // the log showed exactly that:
    //     rt=41.7ms (5 vblanks)  input=25.0ms (3 vblanks)  interp=2 (batch of 3 frames)
    // i.e. 3 frames told to fill 5 vblanks: some refresh periods get two frames, others none,
    // and the eye sees the picture "jump". Under an unlocked, fluctuating frame rate this
    // branch fires on almost every frame (any single extra vblank), so it is not a rare-case
    // fix any more - the accumulator itself already absorbs rate changes (bounded phase, one
    // count step per frame). A genuinely isolated hitch in cadence mode is left to
    // `--fg-rt-spike 1 --fg-cad-rt 0`.
    if (g_fg_rt_spike && !g_interp_cadence && last_ms > plan * kSpikeRatio &&
        last_ms < 100.0) {
        int kr = static_cast<int>(last_ms / vb + 0.5);
        if (kr < k) { kr = k; }
        const int kcap = k * 3;              // never chase a real hitch (>3x the plan)
        if (kr > kcap) { kr = kcap; }
        if (kr > k) {
            const double rt = static_cast<double>(kr) * vb;
            if (g_spike_logs_left > 0) {
                --g_spike_logs_left;
                logf_("[lock] rt spike: last input %.1fms (%.1f vblanks) > plan %.1fms"
                      " - frameRenderTime %.1fms for this frame (%.1f vblanks)%s\n",
                      last_ms, last_ms / vb, plan, rt, rt / vb,
                      (g_spike_logs_left == 0) ? " (further spikes not logged)" : "");
            }
            ++g_spike_count;
            return rt;
        }
    }
    return plan;
}

// What the pacer is actually being fed (the breakdown line prints this). In cadence mode
// fg_render_time_ms() returns a batch-derived value, so the --fg-rt-mode name alone would
// read "quant" while something else is what XeFG got.
static const char* fg_rt_feed_name()
{
    if (g_interp_cadence && g_fg_cad_rt && g_cad_last_k >= 2) {
        const bool capped = (g_fg_out_hz > 1.0)
                         && (g_fg_out_hz < 0.999 * (1000.0 / refresh_ms()));
        return capped ? "cad:gap" : "cad:batch";
    }
    return fg_rt_mode_name();
}

static double fg_render_time_ms()
{
    if (g_fg_rt_zero) { return 0.0; }
    // Cadence mode: feed the *same* interval the batch size was derived from.
    //
    // This is the one invariant that matters: `k` frames are handed to XeFG, so the budget it
    // is told must be the interval those k frames cover - not a snapped plan that can be
    // several vblanks longer. The 2026-11-10 log had "rt=41.7ms (5 vblanks), input=25.0ms
    // (3 vblanks), batch of 3 frames": 3 frames told to fill 5 vblanks, so the pacer puts two
    // in one refresh period and leaves the next empty - the eye reads that as a jump.
    //
    // Note this is *not* k*vblank (which was measured to drift: Present() blocks on the
    // pacer's budget, which inflates the very interval we measure, and the loop has no fixed
    // point - see the g_fg_cad_rt comment). g_cad_last_tin is the measured median the count
    // was chosen from, so feeding it back changes nothing about the measurement.
    if (g_interp_cadence && g_cad_last_k >= 2 && g_cad_last_tin > 0.5) {
        // --cad-batch-rt: k frames fill exactly k vblanks, so tell XeFG exactly that. Safe now
        // because the count came from the *producer's* clock (g_cad_batch_rt), so this budget
        // cannot feed back into the measurement.
        if (g_cad_batch_rt) {
            static bool onceb = false;
            if (!onceb) {
                onceb = true;
                logf_("[lock] cadence: frameRenderTime = %u x %.2f ms (the vblanks this batch"
                      " fills) - --cad-batch-rt\n", g_cad_last_k, refresh_ms());
            }
            return static_cast<double>(g_cad_last_k) * refresh_ms();
        }
        static bool once = false;
        if (!once) {
            once = true;
            logf_("[lock] cadence: frameRenderTime = the interval the batch covers"
                  " (measured %.1fms for %u frames) - --fg-cad-rt 1 switches to k*vblank,"
                  " --fg-rt-mode applies when cadence is off\n",
                  g_cad_last_tin, g_cad_last_k);
        }
        return g_cad_last_tin;
    }
    if (g_interp_cadence && g_fg_cad_rt && g_cad_last_k >= 2) {
        // The batch must be paced over the time it actually covers. Filling every vblank
        // means k frames occupy k vblanks, so the budget is k * vblank - not the median
        // input interval, which contradicts the batch whenever the count is not the one the
        // median implies (a 4-frame batch told "25 ms" squeezes 4 frames into 3 vblanks and
        // then idles for one, i.e. a visible hitch on every such batch).
        //
        // With --fg-out-hz the frames do *not* fill every vblank (fewer frames than there
        // are vblanks), so the whole batch is spread over the real input gap instead.
        const bool capped = (g_fg_out_hz > 1.0) && (g_fg_out_hz < 0.999 * (1000.0 / refresh_ms()));
        const double rt = capped
            ? ((g_cad_last_tin > 0.5) ? g_cad_last_tin : g_cad_last_k * refresh_ms())
            : static_cast<double>(g_cad_last_k) * refresh_ms();
        if (!g_cad_rt_logged) {
            g_cad_rt_logged = true;
            logf_("[lock] cadence: frameRenderTime = batch * vblank (%.1fms for %u frames);"
                  " --fg-cad-rt 0 restores the median feed\n", rt, g_cad_last_k);
        }
        return rt;
    }
    switch (g_fg_rt_mode) {
    case kFgRtRaw:
        return rt_raw_value();
    case kFgRtEma:
        // The forwarded interval as an EMA (alpha 0.1, i.e. ~10 frames) - the historical
        // default before quant existed. Now only an A/B option: in fixed-multiplier mode it
        // measured *worse* than auto in all three paired rounds (0.2/0.4/0.5% empty vblanks
        // vs 0.1/0.1/0.3%, max hole 4-6 vblanks vs 2), because a smooth estimate lags the
        // step when the throttled input rate actually moves. See PERF_BASELINE 3.23.
        //
        // v29: prefer the game-clock EMA (accepted-to-accepted). The old source was
        // g_frame_ms_ema, which is measured with GetTickCount64 and quantised to 1 ms on a
        // 15.6 ms clock - it cannot see a 3.6 ms step at all.
        {
            const double ge = game_ema_ms();
            if (ge > 0.5) { return ge; }
            return (g_arrival_ms_ema > 0.5) ? g_arrival_ms_ema : rt_quant_value();
        }
    case kFgRtMedian:
        {
            const double gm = game_median_ms();
            return (gm > 0.5) ? gm : arr_median_ms();
        }
    case kFgRtAuto:
        // v30: no quant/raw decision any more - that switch was itself a step, and a scene
        // change (40 -> 36 fps) flipped it while the eye was watching.
        //
        // rt_auto_feed_ms() returns max(m*vblank, median), ramped: spacing is never worse
        // than one output per vblank (no drops), and a slower scene widens the batch over
        // ~0.15 s. The residual is a constant motion-speed error proportional to the deficit,
        // which reads as "the game is a bit slower", not as judder.
        return rt_auto_feed_ms();
    case kFgRtQuant:
    default:
        return rt_quant_value();
    }
}

// --interp-auto: choose N so that (N+1) output frames per input frame land on whole
// vblanks, i.e. N + 1 ~= T_in / vblank.
//
// "round" (default) sits closest to a whole vblank count; "floor" never asks for
// more frames than the panel can show in one input interval. Measured 2026-11-07
// (PBZ, 120 Hz, input 26/32/47 ms): floor dropped to 3x at 32 ms (3.84 vblanks,
// 93.8 fps = 1.28 vblank/frame) while round kept 4x (125 fps, 4% over-subscribed
// but only ~0.16 vblanks of error) - the round answer was measurably smoother and
// stopped the 3x/4x churn. Keep floor only for A/B.
static bool g_interp_auto = false;
static bool g_interp_auto_round = true;
static int  g_auto_pending = -1;
static UINT g_auto_hits = 0;
static bool g_auto_capped_logged = false;

static double dabs_(double v) { return (v < 0.0) ? -v : v; }

// ================= --dyn-mfg: OptiScaler 的 Dynamic MFG（完整控制器）===========
//
// Uses the verbatim ported state machine in src/xefg_dynamic_count.h (819 lines, from
// OptiScaler, MIT). Unlike my earlier hand-written version - which computed
// floor(target / input fps), oscillated 2x<->3x every 2 s on this game and could not
// recover from a 6x start (docs/findings/FINAL_CONFIG.md 9.17/9.18) - this one:
//   * keeps the presented rate nearest the target from the measured frame time and a
//     LEARNED cost of one generated frame, instead of the (poisoned) input fps;
//   * holds every step (0.3 s up, 1 s down) and verifies the 1.5 s after a change,
//     undoing a step that did not pay off;
//   * doubles its back-off while a step stays blocked, and requires every generated
//     frame to earn 30% of the rate a free frame would add;
//   * supports 1X (generation off) as a normal state.
//
// Called once per REAL frame (its holds count frames, not report windows), before the
// resources are tagged. now_ms/wait_ms come from the caller, where both are in scope.
static double g_dyn_target = 0.0;        // --fg-target N (0 = refresh - 4)
static int    g_dyn_rounding = 1;        // --fg-round 0 at most / 1 nearest / 2 at least
static bool   g_dyn_allow1x = true;      // --fg-allow-1x off => 2x is the floor
static bool   g_dyn_fg_off = false;      // is FG currently switched off by us?
static XeFG_DynamicCount g_dyn_ctrl;     // the ported controller
static double g_dyn_log_ms = 0.0;        // last state log

static double dyn_target_fps()
{
    if (g_dyn_target > 0.0) { return g_dyn_target; }
    const double r = static_cast<double>(g_refresh_hz);
    if (r < 1.0) { return 0.0; }
    return (r > 60.0) ? (r - 4.0) : r;    // 116 at 120 Hz, as OptiScaler defaults to
}

// count = interpolated frames (1 = 2x ... 5 = 6x); 0 = 1X, generation off.
static void fg_dyn_mfg_apply(int count)
{
    if (count <= 0) {
        if (!g_dyn_fg_off) {
            const xefg_swapchain_result_t r = g_api.SetEnabled(g_fg, 0);
            logf_("[lock] dyn-mfg: 1X (frame generation off): %s (%d)\n", xefg_str(r), (int)r);
            if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS) { g_dyn_fg_off = true; }
        }
        return;
    }
    if (g_dyn_fg_off) {
        const xefg_swapchain_result_t r = g_api.SetEnabled(g_fg, 1);
        logf_("[lock] dyn-mfg: back on: %s (%d)\n", xefg_str(r), (int)r);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { return; }
        g_dyn_fg_off = false;
    }
    fg_set_interp(static_cast<UINT>(count), "dyn-mfg");
}

static void fg_dyn_mfg_frame(double now_ms, double wait_ms)
{
    if (!g_dyn_mfg || g_refresh_hz < 1) { return; }
    const double target = dyn_target_fps();
    if (target <= 0.0) { return; }

    XeFG_DynamicCount::Sample s{};
    s.nowMs = now_ms;
    // The game's own frame interval: the raw last arrival (the controller keeps its own
    // EMA and rejects anything above 250 ms as a hitch).
    s.frameMs = (g_arr_last_raw > 0.5) ? g_arr_last_raw : g_arrival_ms_ema;
    // The part of the interval spent waiting inside the present path.
    s.waitMs = wait_ms;
    s.targetFps = target;
    s.refreshFps = static_cast<double>(g_refresh_hz);
    s.limitFps = 0.0;                       // no in-process limiter
    s.maxCount = static_cast<int>(g_fg_interp_max);
    s.currentCount = g_dyn_fg_off ? 0 : static_cast<int>(g_fg_interp_actual);
    s.rounding = g_dyn_rounding;
    s.minCount = g_dyn_allow1x ? 0 : 1;

    const int want = g_dyn_ctrl.Update(s);
    const int have = s.currentCount;
    if (want == have) {
        // A state line every 5 s, so a log always shows what the controller thinks.
        if (now_ms - g_dyn_log_ms >= 5000.0) {
            g_dyn_log_ms = now_ms;
            logf_("[lock] dyn-mfg: %dx held: presented %.1f fps, target %.1f, lower %.1f,"
                  " wait %.0f%%, frame cost %.2f ms, back-off %.0f s, %s\n",
                  have + 1, g_dyn_ctrl.PresentedFps(), g_dyn_ctrl.TargetFps(),
                  g_dyn_ctrl.LowerFps(), g_dyn_ctrl.WaitShare() * 100.0,
                  g_dyn_ctrl.FrameCostMs(), g_dyn_ctrl.BackoffMs() / 1000.0,
                  g_dyn_ctrl.Uncapped() ? "uncapped" : "capped");
        }
        return;
    }

    static const char* reasons[] = { "", "up", "down", "try down", "undo", "no gain, back" };
    const auto reason = g_dyn_ctrl.LastReason();
    const char* why = (reason != XeFG_DynamicCount::Reason::None)
                    ? reasons[static_cast<int>(reason)] : "";
    logf_("[lock] dyn-mfg: %dx -> %dx (%s): presented %.1f fps, target %.1f, lower %.1f,"
          " wait %.0f%%, frame cost %.2f ms, back-off %.0f s\n",
          have + 1, want + 1, why, g_dyn_ctrl.PresentedFps(), g_dyn_ctrl.TargetFps(),
          g_dyn_ctrl.LowerFps(), g_dyn_ctrl.WaitShare() * 100.0,
          g_dyn_ctrl.FrameCostMs(), g_dyn_ctrl.BackoffMs() / 1000.0);
    fg_dyn_mfg_apply(want);
    g_dyn_log_ms = now_ms;
}


static void fg_auto_interp_tick(const char* why)
{
    if (!g_interp_auto || g_refresh_hz == 0 || g_fg_interp_max < 2) { return; }
    const double t_in = arr_median_ms();
    if (t_in < 1.0) { return; }
    const double slots = t_in / refresh_ms();          // vblanks per input frame
    int k = g_interp_auto_round ? static_cast<int>(slots + 0.5)
                                : static_cast<int>(slots);
    if (k < 2) { k = 2; }                              // 1 frame = no generation
    const int kmax = static_cast<int>(g_fg_interp_max) + 1;
    bool capped = false;
    if (k > kmax) { k = kmax; capped = true; }
    const int want = k - 1;

    if (capped && !g_auto_capped_logged) {
        g_auto_capped_logged = true;
        logf_("[lock] --interp-auto: input %.1fms wants %dx for a whole-vblank cadence"
              " but this DLL caps at %ux - staying at %ux (VRR or a faster game frame"
              " rate are the only ways to 120 here)\n",
              t_in, static_cast<int>(slots + 0.5), g_fg_interp_max + 1,
              g_fg_interp_actual + 1);
    }

    if (want == static_cast<int>(g_fg_interp_actual)) {
        g_auto_pending = -1;
        g_auto_hits = 0;
        return;
    }
    // Only move when the new count is meaningfully closer to a whole number of
    // vblanks than the current one: a 0.1 vblank difference is not worth a model
    // switch (and the switch costs a visible hitch).
    const double err_want = dabs_(slots - static_cast<double>(k));
    const double err_have = dabs_(slots - static_cast<double>(g_fg_interp_actual + 1));
    if (err_want + 0.15 > err_have) {
        g_auto_pending = -1;
        g_auto_hits = 0;
        return;
    }

    if (want == g_auto_pending) {
        if (++g_auto_hits >= 2) {                      // stable for two windows
            g_auto_pending = -1;
            g_auto_hits = 0;
            fg_set_interp(static_cast<UINT>(want), why);
        }
    } else {
        g_auto_pending = want;
        g_auto_hits = 1;
    }
}

// The overlay has to fill the monitor the game is on, NOT the capture size.
//
// Those differ as soon as VSR/DSR is in play: the game renders and presents at
// 5120x2880 while the panel is 3840x2160. Sizing the window from the capture
// pixels would make a 4K output get stretched to 5K by DXGI and then downscaled
// to 4K by the driver - two resamples for nothing. Sized to the monitor, a 4K
// output lands exactly 1:1 on a 4K desktop (and a 5K output still fills a 5K
// desktop as before).
static void overlay_size(UINT* x, UINT* y, UINT* w, UINT* h)
{
    RECT r{};
    overlay_rect(&r);
    *x = static_cast<UINT>(r.left);
    *y = static_cast<UINT>(r.top);
    *w = static_cast<UINT>(r.right - r.left);
    *h = static_cast<UINT>(r.bottom - r.top);

    logf_("[lock] overlay fits the monitor %ux%u at (%u,%u) (capture %ux%u)\n",
          *w, *h, *x, *y, g_full_w, g_full_h);
    if (*w != g_full_w || *h != g_full_h) {
        logf_("[lock]   captured %ux%u will be resampled once by us to the output size;"
              " pick it with --scale/--den so it matches the panel for a 1:1 present\n",
              g_full_w, g_full_h);
    }
}

static bool init_fg()
{
    // --overlay-monitor must name a display the *consumer* adapter drives, otherwise every
    // CreateSwapChainForHwnd attempt returns E_ACCESSDENIED and bridge would exit with no
    // picture - the opposite of a useful fallback. Validate it here and fall back to the
    // game window's display instead.
    if (g_overlay_monitor >= 0 && g_consumer_adapter) {
        const HMONITOR want = overlay_monitor(g_game_wnd);
        if (!drives_monitor(g_consumer_adapter.Get(), want)) {
            logf_("[lock] WARNING: --overlay-monitor %d is not driven by the consumer"
                  " adapter (%ls), so the swap chain cannot be created there - presenting"
                  " on the game window's display instead. Put both displays on one GPU, or"
                  " use --gpu to move frame generation to the card that drives that"
                  " display.\n", g_overlay_monitor, g_consumer_desc.Description);
            g_overlay_monitor = -1;
        }
    }

    UINT ox = 0, oy = 0, ow = 0, oh = 0;
    overlay_size(&ox, &oy, &ow, &oh);
    g_bridge_wnd = create_bridge_window(ow, oh);
    if (!g_bridge_wnd) { logf_("[lock] window creation FAILED\n"); return false; }

    xefg_swapchain_result_t r = g_api.D3D12CreateContext(g_device.Get(), &g_fg);
    logf_("[lock] xefgSwapChainD3D12CreateContext: %s (%d)\n", xefg_str(r), (int)r);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { return false; }

    // Let the library explain itself. Without this the only symptom of a refused
    // interpolation is frameGenResult = ERR_INVALID_ARGUMENT.
    g_api.SetLoggingCallback(g_fg, XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG, fg_log_cb, nullptr);

    // How many interpolated frames this DLL adds on THIS device. 1 means the
    // vendor check won: 2x only, whatever --interp says.
    {
        xefg_swapchain_properties_t props{};
        if (g_api.GetProperties(g_fg, &props) == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            g_fg_interp_max = props.maxSupportedInterpolations ? props.maxSupportedInterpolations : 1;
            logf_("[lock] maxSupportedInterpolations = %u (descriptors=%u)\n",
                  g_fg_interp_max, props.requiredDescriptorCount);
        } else {
            g_fg_interp_max = 1;
            logf_("[lock] GetProperties FAILED - assuming 1 interpolated frame\n");
        }
    }

    // Only a *request* above what the DLL allows means the patch failed. With --interp 1
    // (2x) the patch code deliberately does nothing ("nothing to patch"), so warning here
    // was a false alarm - it fired on every plain 2x run and made people distrust the log.
    if (g_unlock_mfg && g_fg_interp_max <= 1 &&
        (g_fg_interp > 1 || g_interp_cadence || g_interp_auto || g_dyn_mfg)) {
        logf_("[lock] WARNING: --unlock-mfg is on and more than 2x was requested, but the"
              " DLL still reports 1 interpolated frame. The patch did not take - do not"
              " expect MFG.\n");
    }

    UINT want_interp = (g_fg_interp > 0) ? static_cast<UINT>(g_fg_interp) : g_fg_interp_max;
    // --interp-cadence starts at the maximum so the count can go either way per frame
    // without a re-init; cadence_next_slots() then sets 1..max every frame.
    if (g_interp_cadence) { want_interp = g_fg_interp_max; }
    // --dyn-mfg starts at 4x even when the ceiling is 6x: starting at 6x at 4K measured
    // 45 ms of GPU wait per real frame and collapsed the loop to 13 fps
    // (bridge_pbz_dynmfg.txt). The controller now only climbs with evidence.
    if (g_dyn_mfg) { want_interp = (g_fg_interp_max >= 3) ? 3u : g_fg_interp_max; }
    if (want_interp < 1) { want_interp = 1; }
    if (want_interp > g_fg_interp_max) {
        logf_("[lock] --interp %d above this DLL's limit (%u) - using %u\n",
              g_fg_interp, g_fg_interp_max, g_fg_interp_max);
        want_interp = g_fg_interp_max;
    }
    g_fg_interp_actual = want_interp;

    if (g_fg_interp_max <= 1) {
        logf_("[lock] MFG unavailable: libxess_fg.dll allows 1 interpolated frame here,"
              " so the output is 2x.\n");
        logf_("[lock]   a community MFG build (plus a matching libxell.dll) raises this;"
              " select it with --fg-lib <path>\n");
        if (g_fg_interp > 1) {
            logf_("[lock]   --interp %d ignored, running 2x\n", g_fg_interp);
        }
    } else {
        logf_("[lock] MFG available: up to %ux (%u interpolated frames per real frame)\n",
              g_fg_interp_max + 1, g_fg_interp_max);
        logf_("[lock]   running %ux (%u interpolated)\n", want_interp + 1, want_interp);
    }
    if (!g_api.has_mfg()) {
        logf_("[lock] note: DLL has no SetNumInterpolatedFrames, so the count cannot be"
              " changed at runtime\n");
    }

    // XeLL must be attached BEFORE the proxy swap chain is initialized: without
    // it InitFromSwapChain fails with ERR_LATENCY_REDUCTION_UNSUPPORTED (-15).
    const xell_result_t xr = xellD3D12CreateContext(g_device.Get(), &g_xell);
    logf_("[lock] xellD3D12CreateContext: %d\n", (int)xr);
    if (xr == XELL_RESULT_SUCCESS) {
        // Creating the context is not enough - XeLL has to be *enabled*, and
        // xefgSwapChainSetEnabled returns ERR_LATENCY_REDUCTION_UNSUPPORTED (-15)
        // until bLowLatencyMode is on.
        xell_sleep_params_t sp{};
        sp.minimumIntervalUs = 0;
        sp.bLowLatencyMode = g_xell_mode;
        const xell_result_t sr = xellSetSleepMode(g_xell, &sp);
        logf_("[lock] xellSetSleepMode(lowLatency): %d\n", (int)sr);

        const xefg_swapchain_result_t lr =
            g_api.SetLatencyReduction(g_fg, reinterpret_cast<void*>(g_xell));
        logf_("[lock] SetLatencyReduction: %s (%d)\n", xefg_str(lr), (int)lr);
    } else {
        logf_("[lock] no XeLL context - XeSS-FG cannot initialize\n");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 sc{};
    if (g_scale > 0.0f) {
        sc.Width  = (static_cast<UINT>(g_full_w * g_scale)) & ~1u;
        sc.Height = (static_cast<UINT>(g_full_h * g_scale)) & ~1u;
    } else if (g_out_den > 1) {
        sc.Width  = g_full_w / static_cast<UINT>(g_out_den);
        sc.Height = g_full_h / static_cast<UINT>(g_out_den);
    } else {
        sc.Width  = g_full_w;
        sc.Height = g_full_h;
    }
    sc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;      // HDR10, the only format XeSS-FG accepts
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    if (g_tearing) { sc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING; }
    sc.BufferCount = g_swap_buffers;    // two presents per input frame out of this ring
    sc.Scaling = DXGI_SCALING_STRETCH;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs{};
    fs.Windowed = TRUE;

    // D3D12 devices do not implement IDXGIDevice (E_NOINTERFACE), so the factory
    // must be found through the device adapter LUID.
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGIFactory4> f4;
    {
        const LUID luid = g_device->GetAdapterLuid();
        logf_("[lock] device LUID %08lX:%08lX\n",
              static_cast<unsigned long>(luid.HighPart),
              static_cast<unsigned long>(luid.LowPart));

        HRESULT h = CreateDXGIFactory2(0, IID_PPV_ARGS(&f4));
        logf_("[lock] CreateDXGIFactory2: hr=0x%08lX\n", static_cast<unsigned long>(h));
        if (FAILED(h)) { return false; }

        ComPtr<IDXGIAdapter1> ad;
        h = f4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad));
        logf_("[lock] EnumAdapterByLuid: hr=0x%08lX\n", static_cast<unsigned long>(h));
        if (SUCCEEDED(h) && ad) {
            h = ad->GetParent(IID_PPV_ARGS(&factory));
            logf_("[lock] Adapter->GetParent(IDXGIFactory2): hr=0x%08lX\n",
                  static_cast<unsigned long>(h));
        }
        if (!factory) { f4.As(&factory); }
        if (!factory) { logf_("[lock] no DXGI factory\n"); return false; }

        // Which adapter drives the monitor our window sits on? A mismatch here is
        // one way CreateSwapChainForHwnd ends in E_ACCESSDENIED.
        const HMONITOR mon = MonitorFromWindow(g_bridge_wnd, MONITOR_DEFAULTTOPRIMARY);
        logf_("[lock] window monitor %p\n", reinterpret_cast<void*>(mon));
        for (unsigned i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (f4->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) { break; }
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            for (unsigned j = 0;; ++j) {
                ComPtr<IDXGIOutput> out;
                if (a->EnumOutputs(j, &out) == DXGI_ERROR_NOT_FOUND) { break; }
                DXGI_OUTPUT_DESC od{};
                out->GetDesc(&od);
                const bool ours = (od.Monitor == mon);
                logf_("[lock]   '%ls' output %u %p%s\n", d.Description, j,
                      reinterpret_cast<void*>(od.Monitor), ours ? "  <== our window" : "");

                // HDR state decides how the frame has to be handed to the display.
                ComPtr<IDXGIOutput6> o6;
                if (ours && SUCCEEDED(out.As(&o6))) {
                    DXGI_OUTPUT_DESC1 od1{};
                    if (SUCCEEDED(o6->GetDesc1(&od1))) {
                        DEVMODEW dm{};
                        // The mode of the display we present on, not of the primary one:
                        // with --overlay-monitor they are different monitors, and quant
                        // would otherwise snap frameRenderTime to the wrong vblank.
        if (overlay_display_mode(&dm)) {
            logf_("[lock] display mode: %lux%lu @ %lu Hz%s\n",
                  dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency,
                  (g_overlay_monitor >= 0) ? " (overlay monitor)" : "");

            g_refresh_hz = dm.dmDisplayFrequency;
            // A fixed multiplier needs the panel to actually run at the target output
            // rate. 4K HDR over a link without DSC lands on 59/60 Hz, and then 4x can
            // never show more than ~60 fps no matter what the generator does.
            if (g_refresh_hz > 0 && g_refresh_hz < 100) {
                const unsigned mul = static_cast<unsigned>(g_fg_interp + 1);
                logf_("[lock] WARNING: panel runs at %lu Hz - fixed %ux needs %lu Hz for"
                      " %.1f fps input; the output will cap near %lu fps. Check the display"
                      " mode and the cable (4K HDR 120 Hz needs DP 1.4 + DSC or HDMI 2.1).\n",
                      dm.dmDisplayFrequency, mul,
                      static_cast<unsigned long>(dm.dmDisplayFrequency * mul),
                      static_cast<double>(dm.dmDisplayFrequency) / static_cast<double>(mul),
                      dm.dmDisplayFrequency);
            }
        }
        logf_("[lock]   colour space %d (%s) bits=%u maxLum=%.0f maxFull=%.0f\n",
                              (int)od1.ColorSpace,
                              (od1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
                                  ? "HDR10 active" : "SDR (no HDR10)",
                              od1.BitsPerColor, od1.MaxLuminance, od1.MaxFullFrameLuminance);
                    }
                }
            }
        }
    }

    xefg_swapchain_d3d12_init_params_t ip{};
    // Last chance to learn what the game declared: the add-on only knows MVJittered (and the
    // depth sign) once the game has created its DLSS feature, which can easily be after
    // peek_producer()/open_shared() ran. Re-read the freshest flags here - after this point
    // the init flags are frozen for the whole run.
    if (g_ctl != nullptr) {
        g_producer_flags = g_ctl->producer.input_flags;
        adopt_producer_hints(g_producer_flags);
    }
    // The count asked for here is what the DLL will actually add per real frame;
    // want_interp was already clamped to maxSupportedInterpolations above.
    ip.maxInterpolatedFrames = want_interp;
    ip.initFlags = g_fg_depth_inv ? XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH
                                  : XEFG_SWAPCHAIN_INIT_FLAG_NONE;
    // NDC velocity must be *declared*: without the flag XeFG reads the field as UV (0..1) and
    // a [-1,1] source comes out wrong. OptiScaler does exactly this pair - the flag plus
    // mvScaleY *= size.y * -0.5 (inputs/FG/XeFG_Inputs_Dx12.cpp:521); our producer publishes
    // the matching (w/2, -h/2) scale along with the kInputFlagMvNdc bit
    // (docs/findings/OPTISCALER_QUIRKS.md 3.1).
    const bool ndc_mv = (g_producer_flags & mgpu::kInputFlagMvNdc) != 0;
    if (ndc_mv) { ip.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY; }
    // JITTERED_MV makes the library take the frame's jitter back out of the velocity
    // ("Remove jitter from input velocity", xefg_swapchain.h:95). OptiScaler sets it from
    // the game's DLSS MVJittered flag; UE5 titles do report that flag, so their vectors keep
    // the +-0.5 px Halton offset and XeFG read it as real motion (= model-edge smear).
    // HIGH_RES_MV deliberately stays out: the SDK marks it
    // "Deprecated. Setting this flag has no effect." (xefg_swapchain.h:91).
    if (g_fg_jittered_mv) { ip.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV; }
    if (ip.initFlags != XEFG_SWAPCHAIN_INIT_FLAG_NONE) {
        logf_("[lock] initFlags=0x%x (inverted_depth=%d jittered_mv=%d ndc_velocity=%d)\n",
              static_cast<unsigned>(ip.initFlags), g_fg_depth_inv ? 1 : 0,
              g_fg_jittered_mv ? 1 : 0, ndc_mv ? 1 : 0);
    }
    // AUTO probes for a UI texture we never provide; NONE says it outright.
    // AUTO only when the UI really travels: XeFG composits a UI texture we tag, and with
    // none tagged "AUTO" is the same as promising something that never arrives.
    // slot_size alone is NOT evidence: the producer reserves stream 3 at output size even
    // when its UiStream=1 source is still Off (no UI/HUDless yet). Require a selected
    // producer source, or an explicit local opt-in (--ui-source + --ui-unblend).
    const bool ui_source_on = g_ctl != nullptr && g_ctl->ui_source != mgpu::kUiSourceOff;
    const bool ui_cli_optin = g_ui_unblend && g_ui_source_mode != 0;
    const bool have_ui_stream = g_ctl && g_ctl->streams[3].slot_size != 0 &&
                                g_ctl->streams[3].width >= 64 &&
                                (ui_source_on || ui_cli_optin);
    if (g_ctl && g_ctl->streams[3].slot_size != 0 && g_ctl->streams[3].width >= 64 &&
        !have_ui_stream) {
        logf_("[lock] note: stream 3 is reserved but the producer UI source is Off -"
              " UI composition stays disabled (start lock_frame after the game has presented"
              " a few frames, or check fakerFG.ini UiStream/UiSource)\n");
    }
    ip.uiMode = (!g_fg_ui_none && have_ui_stream) ? XEFG_SWAPCHAIN_UI_MODE_AUTO
                                                  : XEFG_SWAPCHAIN_UI_MODE_NONE;
    ip.creationNodeMask = 1;
    ip.visibleNodeMask  = 1;

    if (g_out_den > 1 || g_scale > 0.0f) {
        // XeSS-FG creates the native swap chain from our description but does not keep
        // DXGI_SCALING_STRETCH, so a smaller back buffer is shown 1:1 in the corner.
        // Creating the swap chain here (with STRETCH) and handing it over fixes that.
        r = XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        logf_("[lock] InitFromSwapChainDesc: skipped (output %ux%u), creating it ourselves\n",
              sc.Width, sc.Height);
    } else {
        r = g_api.D3D12InitFromSwapChainDesc(g_fg, g_bridge_wnd, &sc, &fs,
                                             g_queue.Get(), factory.Get(), &ip);
        logf_("[lock] InitFromSwapChainDesc: %s (%d)\n", xefg_str(r), (int)r);
    }

    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        // The description route was rejected, so create the swap chain ourselves
        // and hand it over. Every attempt is logged, so one run shows exactly what
        // this machine accepts. app_swap is the global g_app_swap so the reference
        // survives past this block.
        ComPtr<IDXGISwapChain1>& app_swap = g_app_swap;

        struct Combo { DXGI_FORMAT format; DXGI_SWAP_EFFECT effect; const char* name; };
        const Combo combos[] = {
            { DXGI_FORMAT_R10G10B10A2_UNORM,   DXGI_SWAP_EFFECT_FLIP_DISCARD,    "10bit FLIP_DISCARD" },
            { DXGI_FORMAT_R10G10B10A2_UNORM,   DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, "10bit FLIP_SEQ" },
            { DXGI_FORMAT_R8G8B8A8_UNORM,      DXGI_SWAP_EFFECT_FLIP_DISCARD,    "8bit FLIP_DISCARD" },
            { DXGI_FORMAT_R16G16B16A16_FLOAT,  DXGI_SWAP_EFFECT_FLIP_DISCARD,    "FP16 FLIP_DISCARD" },
        };

        auto hwnd_attempts = [&](HWND wnd, const char* where) {
            for (const Combo& c : combos) {
                DXGI_SWAP_CHAIN_DESC1 d = sc;
                d.Format = c.format;
                d.SwapEffect = c.effect;
                ComPtr<IDXGISwapChain1> tmp;
                const HRESULT h = factory->CreateSwapChainForHwnd(g_queue.Get(), wnd, &d, &fs,
                                                                 nullptr, &tmp);
                logf_("[lock] hwnd %-8s %-18s hr=0x%08lX\n", where, c.name,
                      static_cast<unsigned long>(h));
                if (SUCCEEDED(h)) { app_swap = tmp; g_sc_wnd = wnd; return true; }
            }
            return false;
        };

        // 1) our own overlay window
        if (hwnd_attempts(g_bridge_wnd, "popup")) {
            logf_("[lock] using hwnd swap chain on the overlay window\n");
        }

        // 2) a plain window in case the popup style is the reason for E_ACCESSDENIED.
        //    It must carry the same click-through / never-activate styles: without them
        //    this fallback swallows the mouse and the keyboard of the game (measured on
        //    a UE5 title where route 1 failed - the overlay is a separate process, so
        //    HTTRANSPARENT alone does not pass input through).
        HWND plain = nullptr;
        if (!app_swap) {
            plain = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                        WS_EX_TOOLWINDOW,
                                    L"STATIC", L"MGPU Bridge SC", WS_POPUP,
                                    0, 0, static_cast<int>(g_full_w), static_cast<int>(g_full_h),
                                    nullptr, nullptr, nullptr, nullptr);
            logf_("[lock] probe window %p (click-through)\n", reinterpret_cast<void*>(plain));
            if (plain) { hwnd_attempts(plain, "plain"); }
        }

        // 3) composition swap chain + DirectComposition: needs no hwnd ownership
        if (!app_swap) {
            DXGI_SWAP_CHAIN_DESC1 d = sc;
            d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            ComPtr<IDXGISwapChain1> tmp;
            const HRESULT h = factory->CreateSwapChainForComposition(g_queue.Get(), &d, nullptr,
                                                                     &tmp);
            logf_("[lock] CreateSwapChainForComposition: hr=0x%08lX\n",
                  static_cast<unsigned long>(h));
            if (SUCCEEDED(h)) {
                ComPtr<IDXGIDevice> d11_dxgi;
                HRESULT hc = g_d3d11.As(&d11_dxgi);
                if (SUCCEEDED(hc)) { hc = DCompositionCreateDevice(d11_dxgi.Get(),
                                                                    IID_PPV_ARGS(&g_dcomp)); }
                if (SUCCEEDED(hc)) { hc = g_dcomp->CreateTargetForHwnd(g_bridge_wnd, TRUE,
                                                                       &g_dcomp_target); }
                if (SUCCEEDED(hc)) { hc = g_dcomp->CreateVisual(&g_dcomp_visual); }
                if (SUCCEEDED(hc)) { hc = g_dcomp_visual->SetContent(tmp.Get()); }
                if (SUCCEEDED(hc)) { hc = g_dcomp_target->SetRoot(g_dcomp_visual.Get()); }
                if (SUCCEEDED(hc)) { hc = g_dcomp->Commit(); }
                logf_("[lock] DirectComposition attach: hr=0x%08lX\n",
                      static_cast<unsigned long>(hc));
                if (SUCCEEDED(hc)) { app_swap = tmp; }
            }
        }

        if (!app_swap) {
            logf_("[lock] no swap chain could be created on this system\n");
            return false;
        }

        xefg_swapchain_d3d12_init_params_t ip2{};
        // Ownership is transferred: the guide says the passed swap chain is
        // released, and any extra outstanding reference makes re-creating the
        // native swap chain fail.
        ip2.pApplicationSwapChain = app_swap.Detach();
        ip2.maxInterpolatedFrames = want_interp;
        ip2.initFlags = ip.initFlags;
        ip2.uiMode = ip.uiMode;
        ip2.creationNodeMask = 1;
        ip2.visibleNodeMask  = 1;

        r = g_api.D3D12InitFromSwapChain(g_fg, g_queue.Get(), &ip2);
        logf_("[lock] InitFromSwapChain: %s (%d)\n", xefg_str(r), (int)r);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { return false; }
    }

    // What the library really applied (it may clamp the request).
    if (g_api.D3D12GetInitParams) {
        xefg_swapchain_d3d12_init_params_t got{};
        if (g_api.D3D12GetInitParams(g_fg, &got) == XEFG_SWAPCHAIN_RESULT_SUCCESS &&
            got.maxInterpolatedFrames > 0) {
            g_fg_interp_actual = got.maxInterpolatedFrames;
            logf_("[lock] applied maxInterpolatedFrames = %u -> %ux output\n",
                  g_fg_interp_actual, g_fg_interp_actual + 1);
        }
    }

    r = g_api.D3D12GetSwapChainPtr(g_fg, IID_PPV_ARGS(&g_swapchain));
    logf_("[lock] GetSwapChainPtr: %s (%d) ptr=%p\n", xefg_str(r), (int)r,
          reinterpret_cast<void*>(g_swapchain.Get()));
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS || !g_swapchain) {
        logf_("[lock] GetSwapChainPtr FAILED\n");
        return false;
    }

    {
        DXGI_SWAP_CHAIN_DESC1 real{};
        if (SUCCEEDED(g_swapchain->GetDesc1(&real))) {
            logf_("[lock] FG swap chain: %ux%u format=%u buffers=%u flags=0x%X\n",
                  real.Width, real.Height, static_cast<unsigned>(real.Format),
                  real.BufferCount, real.Flags);
        }
    }

    // Present was blocking 31-47 ms, which capped the loop at ~24 fps (below the
    // game's own rate) and let stale frames pile up: that is where the huge input
    // latency came from. One queued frame instead of three.
    {
        ComPtr<IDXGISwapChain2> sc2;
        if (SUCCEEDED(g_swapchain.As(&sc2))) {
            const HRESULT h = sc2->SetMaximumFrameLatency(g_max_latency);
            logf_("[lock] SetMaximumFrameLatency(%u): hr=0x%08lX\n", g_max_latency,
                  static_cast<unsigned long>(h));
        }
    }

    // which reports HDR10 active. scRGB cannot be used here - XeSS-FG rejects it.
    const DXGI_COLOR_SPACE_TYPE want_cs =
        (g_colour_mode == 2) ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
                             : DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const HRESULT cs = g_swapchain->SetColorSpace1(want_cs);
    logf_("[lock] SetColorSpace1(%s): hr=0x%08lX\n",
          (g_colour_mode == 2) ? "SDR" : "HDR10", static_cast<unsigned long>(cs));

    // The swap chain exists now, so the window that owns it becomes the visible
    // overlay. Our own window stays hidden unless it was the one that won, which
    // avoids a blank white window sitting on top of the picture.
    make_borderless(overlay_wnd(), static_cast<int>(ox), static_cast<int>(oy), ow, oh);
    if (g_overlay_layered) {
        const LONG_PTR ex = GetWindowLongPtrW(overlay_wnd(), GWL_EXSTYLE);
        SetWindowLongPtrW(overlay_wnd(), GWL_EXSTYLE, ex | WS_EX_LAYERED);
        const BOOL ok = SetLayeredWindowAttributes(overlay_wnd(), 0, 255, LWA_ALPHA);
        logf_("[lock] --overlay-layered: WS_EX_LAYERED + alpha 255 (ok=%d)\n",
              static_cast<int>(ok));
    }

    if (g_no_fg) {
        logf_("[lock] --no-fg: proxy swap chain only, frame generation left off\n");
        return true;
    }

    if (g_fg_scene > 0.0f) {
        if (g_api.SetSceneChangeThreshold) {
            const xefg_swapchain_result_t sc = g_api.SetSceneChangeThreshold(g_fg, g_fg_scene);
            logf_("[lock] SetSceneChangeThreshold(%.2f): %s (%d)\n",
                  g_fg_scene, xefg_str(sc), static_cast<int>(sc));
        } else {
            logf_("[lock] --fg-scene ignored: DLL has no SetSceneChangeThreshold\n");
        }
    }

    r = g_api.SetEnabled(g_fg, 1);
    logf_("[lock] SetEnabled: %s (%d)\n", xefg_str(r), (int)r);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { return false; }

    // B mode 4 gate: the UI stream existence is the only switch for XeFG UI composition.
    // unblend success tags HUDless (mode 4); failure tags UI (mode 2). If the producer has
    // no UI stream there is nothing to compose, so keep the library default (DISABLED).
    {
        const bool ui_source_on = g_ctl != nullptr && g_ctl->ui_source != mgpu::kUiSourceOff;
        const bool ui_cli_optin = g_ui_unblend && g_ui_source_mode != 0;
        const bool have_ui_stream = g_ctl && g_ctl->streams[3].slot_size != 0 &&
                                    g_ctl->streams[3].width >= 64 &&
                                    (ui_source_on || ui_cli_optin);
        if (g_api.has_ui_composition()) {
            const auto st = (have_ui_stream && !g_fg_ui_none)
                                ? XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED
                                : XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_DISABLED;
            const xefg_swapchain_result_t ur = g_api.SetUiCompositionState(g_fg, st);
            logf_("[lock] SetUiCompositionState(%s): %s (%d) (ui_stream=%d unblend=%d)\n",
                  st == XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED ? "ENABLED" : "DISABLED",
                  xefg_str(ur), (int)ur, have_ui_stream ? 1 : 0, g_ui_unblend ? 1 : 0);
        } else if (have_ui_stream) {
            logf_("[lock] UI stream present but this libxess_fg has no SetUiCompositionState -"
                  " composition stays off\n");
        }
    }

    // If the init parameter could not express the request (older DLL, or a
    // mismatch after the library clamped it) try the runtime call as well.
    fg_apply_interp("after SetEnabled");
    return true;
}


static HANDLE g_map = nullptr;

// Places our views of the producer's MV/depth slots inside the shared heap.
//
// Protocol v9: the slots are placed at their *capacity* (StreamShared.slot_w/h) and never
// move, so this runs exactly once. The pitch comes from the capacity - placing with the
// live content size instead would read a different memory layout. What the producer
// publishes per frame is only the live rectangle (`width x height`), which copy_inputs_local
// copies out of these views.
static bool place_shared_views()
{
    if (!g_ctl || !g_shared_heap) { return false; }
    for (UINT s = 0; s < mgpu::kMaxSlots; ++s) {
        for (int k = 0; k <= 3; ++k) {                 // 0 = colour/hudless, 1 = MV, 2 = depth, 3 = UI
            const mgpu::StreamShared& st = g_ctl->streams[k];
            if (st.slot_size == 0) { continue; }
            ComPtr<ID3D12Resource>& dst = (k == 0) ? g_colour[s]
                                          : (k == 1) ? g_mv[s]
                                          : (k == 2) ? g_depth[s]
                                                     : g_ui[s];
            if (dst) { continue; }                     // already placed (v9: never moves)
            const UINT pw = st.slot_w ? st.slot_w : st.width;
            const UINT ph = st.slot_h ? st.slot_h : st.height;
            D3D12_RESOURCE_DESC rd{};
            if (st.codec != 0) {
                // v30: the stream carries fr-codec bytes in a raw buffer. slot_size is the
                // byte size; slot_w/slot_h stay the content capacity, so tile counts and the
                // TileLen[] layout are derivable on both sides.
                rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width            = st.slot_size;
                rd.Height           = 1;
                rd.DepthOrArraySize = 1;
                rd.MipLevels        = 1;
                rd.Format           = DXGI_FORMAT_UNKNOWN;
                rd.SampleDesc.Count = 1;
                rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                // The two adapters place the SAME memory: the resource description (size,
                // format and flags) must be identical on both sides, otherwise the driver
                // treats the view as mismatched and removes the device. The producer needs
                // ALLOW_UNORDERED_ACCESS for the encode pass, so this side carries it too
                // (measured 2026-11-21: producer had it, this side did not).
                rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER |
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                const UINT64 boff = st.offset + st.slot_size * s;
                // A buffer cannot be created in COMMON (E_INVALIDARG, measured in
                // tests/gpu_codec); the decode pass transitions it back afterwards.
                if (FAILED(g_device->CreatePlacedResource(
                        g_shared_heap.Get(), boff, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                        nullptr, IID_PPV_ARGS(&dst)))) {
                    logf_("[lock] placed BUFFER FAILED (stream %d slot %u, %llu B)\n", k, s,
                          static_cast<unsigned long long>(st.slot_size));
                    return false;
                }
                continue;
            }
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width            = pw;
            rd.Height           = ph;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.Format           = static_cast<DXGI_FORMAT>(st.format);
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

            const UINT64 off = st.offset + st.slot_size * s;
            if (FAILED(g_device->CreatePlacedResource(g_shared_heap.Get(), off, &rd,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&dst)))) {
                logf_("[lock] placed resource FAILED (stream %d slot %u)\n", k, s);
                return false;
            }
        }
    }
    return true;
}

static const char* input_api_name(UINT api)
{
    switch (api) {
        case mgpu::kInputApiNgx:   return "ngx/dlss";
        case mgpu::kInputApiFfx:   return "ffx/fsr";
        case mgpu::kInputApiXess:  return "xess";
        case mgpu::kInputApiSl:    return "sl/streamline";
        case mgpu::kInputApiNvngxFg: return "ngx/dlssg";
        case mgpu::kInputApiXefg:  return "xefg/xess-fg";
        case mgpu::kInputApiFfxFg: return "ffx/fsr-fg";
        case mgpu::kInputApiFaker: return "fakerFG";
        case mgpu::kInputApiDepth: return "reshade-depth";
        default:                   return "none";
    }
}

// Protocol v6 peek: read the producer's identity out of an *existing* mapping
// without waiting for it. The bridge is normally started after the game, so this
// usually answers immediately and the adapter pick can avoid the producer's GPU
// without --producer-luid. When the mapping is not there yet, the pick falls back to
// the monitor rule and open_shared() logs the same information later.
static bool peek_producer()
{
    HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, mgpu::kMappingName);
    if (!m) { return false; }

    bool ok = false;
    auto* c = static_cast<const mgpu::SharedControl*>(
        MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(mgpu::SharedControl)));
    if (c) {
        if (c->magic == mgpu::kSharedMagic && c->version == mgpu::kSharedVersion &&
            c->producer.device_luid != 0) {
            g_producer_luid     = c->producer.device_luid;
            g_producer_api      = c->producer.input_api;
            g_producer_flags    = c->producer.input_flags;
            g_producer_render_w = c->producer.render_w;
            g_producer_render_h = c->producer.render_h;
            g_producer_output_w = c->producer.output_w;
            g_producer_output_h = c->producer.output_h;
            g_have_producer     = true;
            ok = true;
            adopt_producer_hints(g_producer_flags);

            int32_t hi = 0;
            uint32_t lo = 0;
            mgpu::unpack_luid(g_producer_luid, &hi, &lo);
            logf_("[lock] shared block present: producer pid=%u luid=%08lX:%08lX"
                  " api=%s flags=0x%X render=%ux%u output=%ux%u\n",
                  c->producer_pid, static_cast<unsigned long>(static_cast<uint32_t>(hi)),
                  static_cast<unsigned long>(lo), input_api_name(g_producer_api),
                  g_producer_flags, g_producer_render_w, g_producer_render_h,
                  g_producer_output_w, g_producer_output_h);
        }
        UnmapViewOfFile(const_cast<mgpu::SharedControl*>(c));
    }
    CloseHandle(m);
    return ok;
}

static bool open_shared()
{
    logf_("[lock] waiting for '%s' ...\n", mgpu::kMappingName);

    void* view = nullptr;
    bool version_mismatch = false;
    for (int i = 0; i < 1200 && !view && !version_mismatch; ++i) {
        g_map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mgpu::kMappingName);
        if (g_map) {
            view = MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            if (view) {
                const auto* c = reinterpret_cast<const mgpu::SharedControl*>(view);
                if (c->magic == mgpu::kSharedMagic) {
                    if (c->version == mgpu::kSharedVersion) {
                        g_ctl = const_cast<mgpu::SharedControl*>(c);
                        // A new consumer process owns a new session. The producer
                        // compares this token and clears all per-session ACK/frame
                        // state before publishing, so an old colour_seq can never
                        // hold the new session at a stale watermark.
                        const uint64_t session =
                            (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
                            static_cast<uint64_t>(GetTickCount64());
                        g_ctl->consumer_session = session != 0 ? session : 1;
                        MemoryBarrier();
                        logf_("[lock] new consumer session 0x%llX - producer state will reset\n",
                              static_cast<unsigned long long>(g_ctl->consumer_session));
                        break;
                    }
                    // A live mapping that speaks another protocol: waiting cannot
                    // fix that, and silently reading it would pair the wrong fields
                    // (v5 had no `producer`, so the LUID would be garbage).
                    logf_("[lock] shared block version %u, this bridge speaks %u -"
                          " rebuild/redeploy BOTH sides (add-on + bridge) so they match\n",
                          c->version, mgpu::kSharedVersion);
                    version_mismatch = true;
                }
                UnmapViewOfFile(view);
                view = nullptr;
            }
            CloseHandle(g_map);
            g_map = nullptr;
        }
        if (i == 100) {
            // 10 seconds in: the game window exists, so the missing piece is the add-on.
            logf_("[lock] still waiting after 10 s. The mapping is created by the add-on on"
                  " its first captured frame, so if it never appears the add-on is not running:"
                  " check (1) ReShade loaded into the game, (2) at least one enabled technique"
                  " (ReShadePreset.ini), (3) DLSS on in-game, (4) %ls is the process you expect."
                  " See docs/findings/FINAL_CONFIG.md 2.1.1.\n",
                  g_game_exe_set ? g_game_exe.c_str() : L"(foreground window)");
        }
        Sleep(100);
    }
    if (version_mismatch) { return false; }
    if (!g_ctl) { logf_("[lock] shared block never appeared\n"); return false; }

    g_width  = g_ctl->streams[0].width;
    g_height = g_ctl->streams[0].height;
    logf_("[lock] shared: pid=%u %ux%u slots=%u\n",
          g_ctl->producer_pid, g_width, g_height, g_ctl->slots);

    // v6: producer identity and input semantics. `peek_producer()` usually filled
    // these before the adapter pick; when the mapping appeared only now, adopt it here.
    // kInputFlagInvertedDepth has to be known before init_fg(), which is why this is
    // read here rather than per frame.
    {
        g_producer_luid     = g_ctl->producer.device_luid;
        g_producer_api      = g_ctl->producer.input_api;
        g_producer_flags    = g_ctl->producer.input_flags;
        g_producer_render_w = g_ctl->producer.render_w;
        g_producer_render_h = g_ctl->producer.render_h;
        g_producer_output_w = g_ctl->producer.output_w;
        g_producer_output_h = g_ctl->producer.output_h;
        // v14: pacing/shape of the FG declaration (append-only producer fields).
        const uint32_t declared_frames = g_ctl->producer.declared_frames;
        const uint32_t harvest_flags   = g_ctl->producer.harvest_flags;
        if (g_producer_luid != 0) { g_have_producer = true; }

        int32_t hi = 0;
        uint32_t lo = 0;
        mgpu::unpack_luid(g_producer_luid, &hi, &lo);
        logf_("[lock] shared: producer luid=%08lX:%08lX api=%s flags=0x%X"
              " render=%ux%u output=%ux%u\n",
              static_cast<unsigned long>(static_cast<uint32_t>(hi)),
              static_cast<unsigned long>(lo), input_api_name(g_producer_api),
              g_producer_flags, g_producer_render_w, g_producer_render_h,
              g_producer_output_w, g_producer_output_h);

        if (declared_frames != 0 || harvest_flags != 0) {
            static bool logged_v14 = false;
            if (!logged_v14) {
                logged_v14 = true;
                logf_("[lock]   producer declaration: framesToGenerate=%u%s harvest=0x%X"
                      " (mv=%c depth=%c cam=%c clip=%c token=%c reset=%c hudless=%c ui=%c)\n",
                      declared_frames,
                      declared_frames >= 1 ? "  <== the game itself is generating frames" : "",
                      harvest_flags,
                      (harvest_flags & (1u << 0)) ? 'y' : 'n',
                      (harvest_flags & (1u << 1)) ? 'y' : 'n',
                      (harvest_flags & (1u << 2)) ? 'y' : 'n',
                      (harvest_flags & (1u << 3)) ? 'y' : 'n',
                      (harvest_flags & (1u << 4)) ? 'y' : 'n',
                      (harvest_flags & (1u << 5)) ? 'y' : 'n',
                      (harvest_flags & (1u << 6)) ? 'y' : 'n',
                      (harvest_flags & (1u << 7)) ? 'y' : 'n');
            }
        }

        if (g_producer_flags & mgpu::kInputFlagHasCamera) {
            logf_("[lock]   producer supplies the camera (near/far/vfov) - the"
                  " projection no longer depends on --fg-fov\n");
        }
        adopt_producer_hints(g_producer_flags);
        if (g_have_producer) {
            const uint64_t consumer_luid =
                mgpu::pack_luid(g_consumer_desc.AdapterLuid.HighPart,
                                static_cast<uint32_t>(g_consumer_desc.AdapterLuid.LowPart));
            if (consumer_luid == g_producer_luid) {
                logf_("[lock] WARNING: producer and consumer are the SAME GPU - there is"
                      " no cross-adapter transfer, frame generation competes with the game\n");
            }
        }
    }
    // The colour stream is a placeholder (the add-on does not allocate it with Color=0);
    // MV and depth are the ones that actually matter. Their size follows the game's
    // render resolution - DLSS Quality at 4K is 1440p, DLAA is 2160p - narrowed by the
    // add-on's [capture] StreamPercent (100 = full, 200 = half, 400 = quarter).
    logf_("[lock] MV %ux%u, depth %ux%u (stream size)\n",
          g_ctl->streams[1].width, g_ctl->streams[1].height,
          g_ctl->streams[2].width, g_ctl->streams[2].height);
    // Read-only advice for the add-on's side of the bus. The add-on and the bridge are
    // separate deployments (the add-on lives in the game folder, possibly for several games,
    // and reads its ini only at load), so the bridge must not touch that file - but it can
    // see the stream size, so it can say whether narrowing is in effect at all. Traffic
    // grows with the square of the height, and the measured cost is bytes / ~13 GB/s.
    {
        const UINT sh = g_ctl->streams[1].height;
        if (sh > 1440u) {
            const double mb = static_cast<double>(g_ctl->streams[1].width) * sh * 4.0 / (1024.0 * 1024.0) * 2.0;
            const double capped = mb * static_cast<double>(1440u) * static_cast<double>(1440u)
                                / (static_cast<double>(sh) * static_cast<double>(sh));
            logf_("[lock] NOTE: the stream is %ux%u (%.0f MB/frame with depth). The same"
                  " session with a smaller stream would send ~%.0f MB/frame - the new producer sends"
                  " 1:1 full size (StreamPercent has not been ported to xess_mfg_producer yet)\n",
                  g_ctl->streams[1].width, sh, mb, capped);
        }
    }

    g_full_w = g_width;
    g_full_h = g_height;

    // Capture stays at the full window size and only the presented image is divided:
    // a smaller WGC frame pool crops instead of scaling, which zoomed the picture.
    // The size printed here is what the *shared block declares*; adopt_capture_size()
    // replaces it with the real window size before anything is built from it.
    if (g_scale > 0.0f) {
        logf_("[lock] shared block declares %ux%u, present at scale %.2f\n",
              g_width, g_height, g_scale);
    } else if (g_out_den > 1) {
        logf_("[lock] shared block declares %ux%u, present at 1/%d\n",
              g_width, g_height, g_out_den);
    } else {
        logf_("[lock] shared block declares %ux%u, present at the same size\n",
              g_width, g_height);
    }

    if (g_no_inputs) {
        logf_("[lock] --no-inputs: not importing the cross-adapter heap\n");
        return true;
    }
    if (g_no_depth) {
        logf_("[lock] --no-depth: depth stream skipped, XeFG gets a dummy depth"
              " (disocclusion quality will drop, transfer halves)\n");
    }
    if (g_no_mv) {
        logf_("[lock] --no-mv: motion vectors are zeroed, the real depth is kept\n");
    }
#if MGPU_FG_OF
    if (g_fg_of) {
        logf_("[lock] --fg-of: motion vectors come from FidelityFX optical flow on the"
              " captured colour (the producer's MV stream is not copied)\n");
        if (g_of_const_on) {
            logf_("[lock]   DIAGNOSTIC: constant MV (%.1f, %.1f) px on every frame\n",
                  static_cast<double>(g_of_const_x), static_cast<double>(g_of_const_y));
        }
        logf_("[lock]   units=%s sign=%.0f scale=%.3f guide=%.0f deadzone=%.2fpx max=%.0fpx\n",
              (g_of_units == 3) ? "mvpx" : ((g_of_units == 0) ? "px"
                                           : ((g_of_units == 1) ? "uv" : "ndc")),
              static_cast<double>(g_of_sign), static_cast<double>(g_of_scale),
              static_cast<double>(g_of_guide), static_cast<double>(g_of_deadzone),
              static_cast<double>(g_of_maxlen));
    }
#endif

    ComPtr<ID3D12Device3> device3;
    if (FAILED(g_device.As(&device3))) { return false; }

    if (FAILED(device3->OpenExistingHeapFromFileMapping(g_map, IID_PPV_ARGS(&g_shared_heap)))) {
        logf_("[lock] OpenExistingHeapFromFileMapping FAILED\n");
        return false;
    }

    if (!place_shared_views()) { return false; }
    // Read the published stream geometry once here: service_shared_layout() only re-reads
    // it when `generation` changes, so this is the first and possibly only time. If the
    // producer has not published a size yet, that path keeps retrying (mv_w == 0).
    g_seen_generation = g_ctl->generation;
    g_seen_geo_generation = 0xFFFFFFFFu;    // force the geometry read below
    refresh_input_geo();
    g_seen_geo_generation = g_ctl->generation;

    HANDLE producer = OpenProcess(PROCESS_DUP_HANDLE, FALSE, g_ctl->producer_pid);
    if (!producer) { logf_("[lock] OpenProcess FAILED\n"); return false; }
    for (UINT s = 0; s < g_ctl->slots && s < mgpu::kMaxSlots; ++s) {
        HANDLE lh = nullptr;
        if (!DuplicateHandle(producer, reinterpret_cast<HANDLE>(g_ctl->fence_forward[s]),
                             GetCurrentProcess(), &lh, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            logf_("[lock] DuplicateHandle FAILED\n");
            CloseHandle(producer);
            return false;
        }
        const HRESULT hr = g_device->OpenSharedHandle(lh, IID_PPV_ARGS(&g_fwd[s]));
        CloseHandle(lh);
        if (FAILED(hr)) {
            logf_("[lock] fence OpenSharedHandle FAILED\n");
            CloseHandle(producer);
            return false;
        }
    }
    CloseHandle(producer);

    logf_("[lock] MV/depth placed, fences duplicated\n");
    return true;
}


static ComPtr<IDXGIKeyedMutex> g_colour_mutex;

// ============================ HDR10 output path =============================
//
// XeSS-FG refuses an FP16 back buffer ("Backbuffer format is not valid. Found
// 40"), so the output side must be R10G10B10A2_UNORM in HDR10 / BT.2100 colour
// space. The capture stays FP16 scRGB and is converted here: BT.709 -> BT.2020
// primaries, then PQ (SMPTE ST 2084) encoding. scRGB 1.0 is defined as 80 nits,
// hence the sdrWhiteNits constant.

static ComPtr<ID3D12RootSignature> g_conv_root;
static ComPtr<ID3D12PipelineState> g_conv_pso;
static ComPtr<ID3D12DescriptorHeap> g_srv_heap;
static ComPtr<ID3D12DescriptorHeap> g_rtv_heap;
static ComPtr<ID3D12Resource>       g_rtv_res[4];

// XeSS-FG's swap chain is created once at a fixed size - nothing in here calls
// ResizeBuffers - so the buffers behind it never change identity. GetBuffer() per frame is
// a QueryInterface plus an AddRef/Release round trip for nothing; cache them. One COM
// reference per buffer is held for the life of the process, which is exactly what a
// FLIP_DISCARD swap chain allows.
static ComPtr<ID3D12Resource>       g_bb_cache[4];

static bool back_buffer(UINT index, ID3D12Resource** out)
{
    if (out) { *out = nullptr; }
    if (index > 3 || !g_swapchain || !g_device) { return false; }
    if (!g_bb_cache[index]) {
        if (FAILED(g_swapchain->GetBuffer(index, IID_PPV_ARGS(&g_bb_cache[index])))) {
            logf_("[lock] swap chain GetBuffer(%u) FAILED\n", index);
            return false;
        }
    }
    if (out) { *out = g_bb_cache[index].Get(); }
    return true;
}
static UINT g_rtv_stride = 0;

