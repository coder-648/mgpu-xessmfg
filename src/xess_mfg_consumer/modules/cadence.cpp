// ============================================================================
//  consumer module: cadence.cpp
//
//  Cadence estimation, render-time feeds, MFG count controller, smoothness
//  helpers.
//
//  Included by src/xess_mfg_consumer/unity.cpp as part of the unity build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

#include "../consumer_internal.h"
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
static int g_fg_rt_mode = kFgRtQuant;   // quant: median snapped to whole vblanks (see fg_render_time_ms)
// --fg-rt-mode auto: pick quant or raw from whether the *input rate* is moving.
// Two EMAs of the interval, one fast, one slow; when they disagree by more than a quarter
// of a vblank the rate is changing (unlocked / GPU-bound) and raw tracks it better, because
// the median lags a step by a couple of windows. A locked rate keeps them together even
// though individual frames jitter by +-1 vblank, which is exactly the distinction we want
// (a locked 40 fps also arrives as 2/3/4 vblanks and belongs to quant).
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
static bool   g_fg_cad_rt = false;  // default: feed the input median (old bridge behaviour)
static bool   g_cad_rt_logged = false;
// --cadence-hold N: RETIRED. The phase accumulator's carry is bounded to +-0.5, so the
// count already moves by at most one step per frame; an explicit hold only makes the
// applied count disagree with the value the phase just subtracted (measured: empty
// vblanks 7.5% -> 15.1%). The switch is still accepted for old scripts but ignored.
// --cadence-smooth N (default 8 frames, 1 = raw): smooth the arrival interval feeding the
// phase accumulator over ~N frames.
//
// Why: feeding the *raw* per-frame interval makes the batch size follow the jitter 1:1 - one
// late frame asks for 4-5 generated frames, the next fast one asks for 2. The long-run average
// stays right, but the picture's rhythm changes on almost every frame (measured 17-32 count
// switches per 1 s window while the input fluctuates 2..6 vblanks), and that is what reads as
// "一卡一卡".
//
// Default 8 (docs/findings/FINAL_CONFIG.md 4, PERF_BASELINE 3.22): the mean of a
// right-skewed arrival distribution still moves frame to frame; without this EMA the
// accumulator follows that jitter 1:1. 8 frames halves the count switches (29 -> 12 per
// window) and lowers empty vblanks (3.9% -> 1.3%). Set 1 only to reproduce the old jitter
// for an A/B.
static int    g_cad_smooth = 8;
static double g_cad_arr_s = 0.0;
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

static const char* fg_rt_mode_name()
{
    switch (g_fg_rt_mode) {
    case kFgRtRaw:    return "raw";
    case kFgRtMedian: return "median";
    case kFgRtAuto:   return (g_rt_auto_state == 1) ? "auto:raw" : "auto:quant";
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
// Capacity for cadence: --interp N is the maximum number of interpolated frames, so
// kmax = N+1. It is min(--interp, DLL max); without --interp the DLL max is used.
// (The tick/mean schedulers must not ask for 5/6 when the test explicitly caps at 4x.)
static UINT   g_cad_kmax = 0;
static double g_cad_phase = 0.0;
// --cadence-hold N: keep the DLL count for at least N input frames before changing it,
// and then change by at most one step. Changing SetNumInterpolatedFrames is not free on
// this pacer: the 2026-11-14 CP2077 log shows one output frame lost per change
// (switches ~30/40 -> presents ~95/120). Holding makes the picture's rhythm steady while
// the phase accumulator keeps the long-run average exact (it subtracts the *applied* k).
static int    g_cad_hold_frames = 0;   // 0/1 = change as soon as the present queue is drained
static int    g_cad_hold = 0;
static int    g_cad_applied_k = 0;
// Where the per-frame count comes from:
//   tick = arrival-timestamp / vblank difference (default; follows jitter exactly)
//   mean = the historical mean + phase accumulator (stable, lags a moving rate)
enum { kCadSourceTick = 0, kCadSourceMean = 1, kCadSourceGovernor = 2 };
// Default is the measured-stable mean+phase path (smooth=8, direct apply, no hold).
// `--cadence-source tick` is kept as an A/B: it follows every 2/3/4 vblank difference,
// but on this pacer the per-frame count switches themselves produced visible hitches
// (2026-11-14 CP2077 test: switches 12-38/40, SLOW frame colour 38-46 ms).
static int    g_cad_source = kCadSourceGovernor;
static bool   g_cad_governor_logged = false;
static UINT   g_cad_hist[8] = {};        // how often each count was asked for
static UINT   g_cad_switches = 0;        // count changes actually issued
static UINT   g_cad_deferred = 0;        // changes postponed until the present queue drained
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

// Newest published frame's present interval, in ms (0 = unusable).
//
// 注意：单次原始样本会被 WGC/合成器的 2/3/4 vblank 量化带偏（实测中位数 25.7ms
// 而"最新一帧"读到 23.4ms，差 9% → k 被算小、输出欠供）。所以这里对一个
// 16 样本环取中位数再返回：调用方（cadence core / --cad-batch-rt）需要的是
// "输入速率"，不是"上一帧恰好多长"。
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
    if (!(ms > 3.0 && ms < 100.0)) { return 0.0; }

    static double ring[16] = {};
    static UINT n = 0, w = 0;
    ring[w] = ms;
    w = (w + 1) % 16u;
    if (n < 16) { ++n; }
    double v[16];
    for (UINT i = 0; i < n; ++i) { v[i] = ring[i]; }
    for (UINT i = 1; i < n; ++i) {
        const double key = v[i];
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; --j; }
        v[j + 1] = key;
    }
    return v[n / 2];
}


// Changing SetNumInterpolatedFrames while XeFG still has generated frames queued for the
// compositor throws one of them away: the 2026-11-14 log shows presents ~= expected minus
// one per switch. So a switch is allowed only when GetLastPresentCount has caught up with
// everything submitted so far (one in-flight allowed for the compositor's own lag).
static uint64_t g_cad_expected_out = 0;
static bool cadence_present_drained()
{
    if (!g_swapchain) { return true; }
    UINT last = 0;
    if (FAILED(g_swapchain->GetLastPresentCount(&last))) { return true; }
    return static_cast<uint64_t>(last) + 2 >= g_cad_expected_out;
}

// Rate-limit the expensive part: keep the count for `g_cad_hold_frames` input frames and
// only ever step by one. The phase accumulator keeps running, so the long-run average is
// unaffected; this only controls *when* the DLL is reconfigured.
static int cadence_hold_count(int k)
{
    if (g_cad_hold <= 0) {
        // Keep in sync with anything applied outside the cadence path (init, --interp).
        g_cad_applied_k = static_cast<int>(g_fg_interp_actual) + 1;
    }
    // Never reconfigure while previous generated frames are still queued for presentation.
    if (k != g_cad_applied_k && !cadence_present_drained()) {
        ++g_cad_deferred;
        return g_cad_applied_k;
    }
    if (g_cad_hold_frames <= 1) {
        g_cad_applied_k = k;
        return k;
    }
    if (g_cad_applied_k < 2) {
        g_cad_applied_k = k;
        g_cad_hold = g_cad_hold_frames;
        return k;
    }
    if (g_cad_hold > 0) {
        --g_cad_hold;
        return g_cad_applied_k;
    }
    if (k != g_cad_applied_k) {
        k = (k > g_cad_applied_k) ? (g_cad_applied_k + 1) : (g_cad_applied_k - 1);
        g_cad_applied_k = k;
        g_cad_hold = g_cad_hold_frames;
    }
    return k;
}

// ------------------------------------------------------------------ tick cadence
// The theoretically clean mapping for a jittery input: every real frame has the game's own
// timestamp (FrameShared.frame_qpc_ms from the paired publication; WGC SystemRelativeTime
// only as a fallback). Quantise it to a vblank index and use the *difference* between
// consecutive indices as the number of output frames:
//
//     tick_n = round((arrival_n - anchor) / vblank)
//     k_n    = tick_n - tick_{n-1}          (clamped to 2..kmax)
//     interp = k_n - 1
//     frameRenderTime = k_n * vblank
//
// No mean, no EMA, no phase lag: 35/45 fps produce 3/4/3/4 exactly as their gaps demand.
// The budget is the interval those k frames actually fill, so the pacer is not asked to
// squeeze a 4-frame batch into 3 vblanks (or leave a hole after a 3-frame batch).
static double g_tick_anchor_ms = 0.0;       // arrival qpc of the first frame
static double g_tick_last_arrival_ms = 0.0;
static int    g_tick_last = 0;              // target tick of the previous real frame
static bool   g_tick_clamp_logged = false;

static UINT cadence_tick_slots()
{
    const double vb = refresh_ms();
    // --fg-out-hz: the tick grid is the target output period, not necessarily the panel.
    // (Without the cap it is exactly one vblank.)
    const double period = (g_fg_out_hz > 1.0) ? (1000.0 / g_fg_out_hz) : vb;
    const double now = g_cad_input_ts_ms;
    if (!(period > 0.5) || !(now > 0.0)) { return 0; }

    const int kmax = static_cast<int>(g_cad_kmax ? g_cad_kmax : g_fg_interp_max) + 1;
    if (g_tick_anchor_ms <= 0.0 || now < g_tick_last_arrival_ms ||
        (now - g_tick_last_arrival_ms) > 500.0) {
        // First frame, a clock reset, or a long pause: re-anchor. The first batch size
        // comes from the mean estimate (2..kmax); subsequent batches are tick differences.
        g_tick_anchor_ms = now;
        g_tick_last_arrival_ms = now;
        g_tick_last = 0;
        const double est = (arr_mean_ms() > 0.5) ? arr_mean_ms() : (3.0 * period);
        int k = static_cast<int>(est / period + 0.5);
        if (k < 2) { k = 2; }
        if (k > kmax) { k = kmax; }
        k = cadence_hold_count(k);
        g_cad_last_k = static_cast<UINT>(k);
        g_cad_last_tin = static_cast<double>(k) * period;
        return static_cast<UINT>(k);
    }

    const int tick = static_cast<int>(llround((now - g_tick_anchor_ms) / period));
    int k = tick - g_tick_last;          // previous real frame's target tick
    g_tick_last = tick;
    if (k < 2) {
        k = 2;
        if (!g_tick_clamp_logged) {
            g_tick_clamp_logged = true;
            logf_("[consumer] cadence tick: input gap is below 2 vblanks - clamping to 2x"
                  " (dynamic cadence is not meant for >panel/2 input; use --no-fg)\n");
        }
    } else if (k > kmax) {
        k = kmax;
        if (!g_tick_clamp_logged) {
            g_tick_clamp_logged = true;
            logf_("[consumer] cadence tick: gap needs more than %d frames - clamped; raise"
                  " --interp / --unlock-mfg if this repeats\n", kmax);
        }
    }
    k = cadence_hold_count(k);
    g_cad_last_k = static_cast<UINT>(k);
    g_cad_last_tin = static_cast<double>(k) * period;
    return static_cast<UINT>(k);
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
    // Count the outputs the previous real frame submitted; cadence_present_drained()
    // compares this with the swap chain's real present count before allowing a switch.
    g_cad_expected_out += static_cast<uint64_t>(g_fg_interp_actual) + 1u;
    if (g_cad_source == kCadSourceGovernor) {
        const double vb = refresh_ms();
        const double panel_hz = (vb > 0.5) ? (1000.0 / vb) : 120.0;
        const double out_hz = (g_fg_out_hz > 1.0) ? g_fg_out_hz : panel_hz;
        double t_in = arr_mean_ms();
        if (t_in < 0.5) { t_in = (g_arr_last_raw > 0.5) ? g_arr_last_raw : (1000.0 / 60.0); }
        if (g_cad_smooth > 1) {
            if (g_cad_arr_s <= 0.5) { g_cad_arr_s = t_in; }
            else {
                const double a = 2.0 / (static_cast<double>(g_cad_smooth) + 1.0);
                g_cad_arr_s += (t_in - g_cad_arr_s) * a;
            }
            t_in = g_cad_arr_s;
        }
        const int kmax = static_cast<int>(g_cad_kmax ? g_cad_kmax : g_fg_interp_max) + 1;
        // Floor the *output* at 96% of the target. k = ceil(floor / input_rate) means the
        // multiplier is chosen so the panel never sees fewer than ~115 frames/s; when the
        // input is faster than the exact divisor the output goes over the refresh and the
        // panel simply drops the surplus. That is the old bridge's core trick: a stable
        // integer multiplier, not a fractional one.
        const double floor_hz = out_hz * 0.96;
        int want_k = static_cast<int>(std::ceil((floor_hz * t_in / 1000.0) - 1e-9));
        if (want_k < 2) { want_k = 2; }
        if (want_k > kmax) { want_k = kmax; }

        static int s_down_frames = 0;
        int k = static_cast<int>(g_fg_interp_actual) + 1;
        if (k < 2) { k = want_k; }
        if (want_k > k) {
            k = want_k;                 // go up immediately: never let the output sag
            s_down_frames = 0;
        } else if (want_k < k) {
            // Only step down after a sustained excess; a short dip must not flap the count.
            if (++s_down_frames >= 24) { k = want_k; s_down_frames = 0; }
        } else {
            s_down_frames = 0;
        }
        k = cadence_hold_count(k);
        g_cad_last_k = static_cast<UINT>(k);
        g_cad_last_tin = t_in;
        if (!g_cad_governor_logged) {
            g_cad_governor_logged = true;
            logf_("[consumer] cadence governor: input %.2f ms -> %dx (target %.0f Hz, floor"
                  " %.0f Hz, kmax %d); fixed integer multiplier per epoch\n",
                  t_in, k, out_hz, floor_hz, kmax);
        }
        return static_cast<UINT>(k);
    }
    if (g_cad_source == kCadSourceTick) {
        const UINT k = cadence_tick_slots();
        if (k >= 2) { return k; }
        // No arrival timestamp yet (first frames / timestamp unavailable): fall through to
        // the mean phase accumulator for this frame only.
    }
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
                logf_("[consumer] --cad-batch-rt: input rate from the producer's own present"
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
                    logf_("[consumer] --vrr: the input just needed %d frames (%.1f ms = %.1f vblanks)"
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
                    logf_("[consumer] --vrr: load change (%.1f ms vs %.1f ms average, %.2fx) ->"
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
            logf_("[consumer] --vrr: %d frame(s) per %.1f ms = %.2f ms apart (%.1f fps out);"
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
    const int kmax = static_cast<int>(g_cad_kmax ? g_cad_kmax : g_fg_interp_max) + 1;
    const bool capped_by_dll = (cap > kmax);
    if (capped_by_dll) { cap = kmax; }
    if (k > cap) { k = cap; }

    if (k < 2) {
        if (!g_out_hz_floor_warned) {
            g_out_hz_floor_warned = true;
            logf_("[consumer] --fg-out-hz %.0f: a %.1f ms input needs %u generated frame(s),"
                  " but 2x is the minimum here - the output will run at ~%.0f Hz instead."
                  " Use --no-fg for 1:1.\n",
                  out_hz, t_in, (want < 1.0) ? 1u : static_cast<UINT>(want), 2000.0 / t_in);
        }
        k = 2;
    }

    // Rate-limit the DLL reconfiguration, then account the *applied* count in the phase.
    // Holding does not change the long-run average: any under/over-shoot stays in
    // g_cad_phase and is repaid at the next allowed step.
    k = cadence_hold_count(k);

    // Keep the held count inside the band this frame actually needs: at 35 fps that is
    // {3,4}, at 45 fps {2,3}. Without this a deep phase swing could step one past the band
    // (e.g. 2x at 35 fps), which is a visible slow patch even though the long-run average
    // is still correct.
    {
        const int klo = (want > 2.0) ? static_cast<int>(want) : 2;
        int khi = static_cast<int>(want) + 1;
        if (khi < 2) { khi = 2; }
        if (khi > kmax) { khi = kmax; }
        if (k < klo) { k = klo; }
        if (k > khi) { k = khi; }
    }

    // Bresenham carry. The clamp is deliberately wider than +-0.5 when holding is on:
    // the error has to survive N frames of a deliberately held count, otherwise clamping
    // would silently discard it and the average would drift.
    g_cad_phase -= static_cast<double>(k);
    const double phase_limit = (g_cad_hold_frames > 1)
        ? (2.0 * static_cast<double>(g_cad_hold_frames) + 1.0) : 0.5;
    if (g_cad_phase > phase_limit) { g_cad_phase = phase_limit; }
    if (g_cad_phase < -phase_limit) { g_cad_phase = -phase_limit; }

    g_cad_last_k = static_cast<UINT>(k);      // frameRenderTime follows this (see fg_render_time_ms)
    g_cad_last_tin = t_in;
    return static_cast<UINT>(k);
}

static double arr_median_ms()
{
    // v23.3: no capture arrivals any more. The producer's interval is the truth; the ring
    // is only kept so the old call sites do not have to change.
    if (g_arr_ring_n == 0) {
        if (g_prod_interval_ms > 3.0 && g_prod_interval_ms < 100.0) { return g_prod_interval_ms; }
        return g_arrival_ms_ema;
    }
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

// 真实刷新周期（由 vblank_init 用 IDXGIOutput 校准；定义见下文）。
static bool   g_vb_ok = false;
static double g_vb_period_ms = 0.0;
static double g_vb_t0_ms = 0.0;

static double refresh_ms()
{
    // 优先用实测周期。面板很少是精确 120.000 Hz（本机实测 119.67–119.89 Hz）：
    // 用假设值铺网格会让提交相位相对面板缓慢打滑（实测 ~2.7 ms/s → 几秒一次的顿挫），
    // 而这种漂移再怎么调 hold/k/预算都修不掉，只能用真实周期。
    if (g_vb_ok && g_vb_period_ms > 2.0) { return g_vb_period_ms; }
    return (g_refresh_hz > 0) ? 1000.0 / static_cast<double>(g_refresh_hz) : 1000.0 / 120.0;
}


// --out-clock period: P = k*vblank, or the explicit base rate from --out-clock N.
static double oc_period_ms()
{
    if (g_oc_period_ms <= 0.5) {
        g_oc_period_ms = (g_oc_base_fps > 1.0)
            ? (1000.0 / g_oc_base_fps)
            : (static_cast<double>(g_fg_interp_actual + 1) * refresh_ms());
    }
    return g_oc_period_ms;
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
    // v23.3: v22 deleted the WGC arrival path, so g_arrival_ms_ema froze at its last value
    // (measured: a constant 26.0 ms in every run, regardless of the real input). The
    // producer's per-presentation interval is the only live source; keep the override for
    // deliberate A/B and fall back to the (possibly stale) arrival EMA last.
    if (g_input_ms_override > 1.0) { return g_input_ms_override; }
    if (g_prod_interval_ms > 3.0 && g_prod_interval_ms < 100.0) { return g_prod_interval_ms; }
    return g_arrival_ms_ema;
}

// Raw: the interval the producer actually delivered. Most responsive, and the noisiest -
// it feeds one sample of jitter straight into the pacer's plan.
static double rt_raw_value()
{
    return (g_arr_last_raw > 0.0) ? g_arr_last_raw : g_arrival_ms_ema;
}

// Quant: 16-sample median snapped to whole vblanks (plus the single-frame spike handling
// below). This is the cadence the pacer can actually lay out, so its sanity check stops
// fighting the real pacing.
static double rt_quant_value()
{
    const double vb = refresh_ms();
    int k = static_cast<int>(arr_median_ms() / vb + 0.5);
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
    if (g_fg_rt_spike && !g_interp_cadence && g_arr_last_raw > plan * kSpikeRatio &&
        g_arr_last_raw < 100.0) {
        int kr = static_cast<int>(g_arr_last_raw / vb + 0.5);
        if (kr < k) { kr = k; }
        const int kcap = k * 3;              // never chase a real hitch (>3x the plan)
        if (kr > kcap) { kr = kcap; }
        if (kr > k) {
            const double rt = static_cast<double>(kr) * vb;
            if (g_spike_logs_left > 0) {
                --g_spike_logs_left;
                logf_("[consumer] rt spike: last input %.1fms (%.1f vblanks) > plan %.1fms"
                      " - frameRenderTime %.1fms for this frame (%.1f vblanks)%s\n",
                      g_arr_last_raw, g_arr_last_raw / vb, plan, rt, rt / vb,
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
    if (g_interp_cadence && g_cad_source == kCadSourceTick && g_cad_last_k >= 2) {
        return "cad:tick";
    }
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
    // --out-clock: the batch is laid out over exactly P = k*vblank; the presenter
    // submits at that fixed tick regardless of capture arrival.
    if (g_out_clock) {
        return oc_period_ms();
    }
    // cadence core：本帧预算由 (m,k,paced) 决策给出（paced = k*m*vblank，
    // 否则 = 实测 T）。它替代下面所有 cadence 分支。
    if (g_interp_cadence && g_cad_render_ms > 0.5) { return g_cad_render_ms; }
    // Cadence mode: feed the interval the applied batch covers.
    //
    // Default is the batch budget k*vblank: the count applied to this present covers
    // exactly that many vblanks, so the pacer never has to squeeze k frames into k-1
    // vblanks (or leave a hole). The input median (g_cad_last_tin) remains available as
    // --fg-cad-rt 0 for A/B; it follows game jitter and made dynamic mode stutter.
    // --fg-cad-rt / --cad-batch-rt must be checked *before* the default.
    if (g_interp_cadence && g_cad_last_k >= 2) {
        // --cad-batch-rt: k frames fill exactly k vblanks; the count came from the
        // producer's own present interval, so this budget cannot feed back into it.
        if (g_cad_batch_rt) {
            static bool onceb = false;
            if (!onceb) {
                onceb = true;
                logf_("[consumer] cadence: frameRenderTime = %u x %.2f ms (the vblanks this batch"
                      " fills) - --cad-batch-rt\n", g_cad_last_k, refresh_ms());
            }
            return static_cast<double>(g_cad_last_k) * refresh_ms();
        }
        // --fg-cad-rt 1: k * vblank normally; with --fg-out-hz the batch is spread over the
        // real input gap instead (fewer frames than vblanks).
        if (g_fg_cad_rt) {
            const bool capped = (g_fg_out_hz > 1.0) &&
                                (g_fg_out_hz < 0.999 * (1000.0 / refresh_ms()));
            const double rt = capped
                ? ((g_cad_last_tin > 0.5) ? g_cad_last_tin : g_cad_last_k * refresh_ms())
                : static_cast<double>(g_cad_last_k) * refresh_ms();
            if (!g_cad_rt_logged) {
                g_cad_rt_logged = true;
                logf_("[consumer] cadence: frameRenderTime = batch * vblank (%.1fms for %u frames);"
                      " --fg-cad-rt 0 restores the median feed\n", rt, g_cad_last_k);
            }
            return rt;
        }
        static bool once = false;
        if (!once) {
            once = true;
            logf_("[consumer] cadence: frameRenderTime = the interval the batch covers"
                  " (measured %.1fms for %u frames) - --fg-cad-rt 1 switches to k*vblank,"
                  " --fg-rt-mode applies when cadence is off\n",
                  g_cad_last_tin, g_cad_last_k);
        }
        if (g_cad_last_tin > 0.5) { return g_cad_last_tin; }
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
        return (g_arrival_ms_ema > 0.5) ? g_arrival_ms_ema : rt_quant_value();
    case kFgRtMedian:
        return arr_median_ms();
    case kFgRtAuto:
        // Measured both ends (PERF_BASELINE 3.19): with --interp-cadence, "raw" wins when the
        // input rate is actually moving (0.7% empty vs 1.0%), while "quant" wins - or ties -
        // when the rate is held by a limiter, because then the per-frame jitter is pure noise.
        // Decide from the *rate*, not from the per-frame spread: a locked 40 fps also arrives
        // as 2/3/4 vblanks, and that case belongs to quant.
        return (g_rt_auto_state == 1) ? rt_raw_value() : rt_quant_value();
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

// ---- cadence core（最优动态逐帧）------------------------------------------
// ---- 真实 vblank（DXGI output）--------------------------------------------
// 我们一直用"120 Hz -> 8.333 ms"这个假设去铺网格和判断 presents/vblanks。
// 但面板的真实周期（119.88 Hz = 8.342 ms 之类）与假设值不同时，两者的相位会慢慢走开，
// 表现为"持续轻微顿挫"；而且我们从来没有显示端的时间基准。
// consumer 自己驱动 overlay 那块屏，所以可以直接对它对应的 IDXGIOutput 做校准。
static ComPtr<IDXGIOutput> g_vb_out;
static const char* g_vb_source = "none";   // dwm / wfb120 / none
static double qpc_ms();               // defined in pair_dump.cpp




static bool vblank_init()
{
    if (g_vb_ok) { return true; }
    if (!g_consumer_adapter) { return false; }

    // 首选 DWM 的合成时钟：qpcRefreshPeriod 由合成器直接给出，不受我们线程唤醒
    // 抖动影响。之前用 9 次 WaitForVBlank 的中位数，三次运行分别得到
    // 119.674 / 119.890 / 120.291 Hz —— 面板不可能这样变，说明那个估计的噪声
    // (~±0.3%) 与我们要消除的漂移同量级，直接拿它铺网格反而会引入漂移。
    {
        DWM_TIMING_INFO ti{};
        ti.cbSize = sizeof(ti);
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) &&
            ti.qpcRefreshPeriod > 0 && f.QuadPart > 0) {
            const double period = static_cast<double>(ti.qpcRefreshPeriod) * 1000.0 /
                                  static_cast<double>(f.QuadPart);
            if (period > 2.0 && period < 40.0) {
                g_vb_period_ms = period;
                g_vb_t0_ms = static_cast<double>(ti.qpcVBlank) * 1000.0 /
                             static_cast<double>(f.QuadPart);
                g_vb_ok = true;
                g_vb_source = "dwm";
            }
        }
    }

    // 回退：对 overlay 显示器对应的 IDXGIOutput 做长基线校准（120 个 vblank 的
    // 平均，而不是 9 个的中位数），只用于 DWM 不可用的环境。
    if (!g_vb_ok) {
        IDXGIAdapter* ad = g_consumer_adapter.Get();
        const HMONITOR want = (g_overlay_monitor >= 0)
                            ? overlay_monitor(g_game_wnd)
                            : MonitorFromWindow(g_bridge_wnd, MONITOR_DEFAULTTOPRIMARY);
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIOutput> out;
            if (ad->EnumOutputs(i, &out) == DXGI_ERROR_NOT_FOUND) { break; }
            DXGI_OUTPUT_DESC d{};
            if (FAILED(out->GetDesc(&d))) { continue; }
            if (g_overlay_monitor >= 0 && d.Monitor != want) { continue; }
            g_vb_out = out;
            break;
        }
        if (!g_vb_out) { return false; }
        const int n = 120;
        double t0 = 0.0, t1 = 0.0;
        for (int i = 0; i <= n; ++i) {
            if (FAILED(g_vb_out->WaitForVBlank())) { return false; }
            const double t = qpc_ms();
            if (i == 0) { t0 = t; t1 = t; } else { t1 = t; }
        }
        g_vb_period_ms = (t1 - t0) / static_cast<double>(n);
        g_vb_t0_ms = t1;
        g_vb_ok = (g_vb_period_ms > 2.0 && g_vb_period_ms < 40.0);
        if (g_vb_ok) { g_vb_source = "wfb120"; }
    }

    if (g_vb_ok) {
        const double assumed = (g_refresh_hz > 0) ? 1000.0 / static_cast<double>(g_refresh_hz) : 0.0;
        logf_("[consumer] vblank: measured period %.4f ms (%.3f Hz) via %s; assumed %.4f ms"
              " (%.3f Hz) - diff %.4f ms per vblank%s\n",
              g_vb_period_ms, 1000.0 / g_vb_period_ms, g_vb_source, assumed,
              (assumed > 0.5) ? 1000.0 / assumed : 0.0,
              (assumed > 0.5) ? (g_vb_period_ms - assumed) : 0.0,
              (assumed > 0.5 && std::fabs(g_vb_period_ms - assumed) > 0.02)
                  ? "  <== our grid differs from the panel" : "");
    }
    return g_vb_ok;
}


// 距上一次 vblank 的相位（ms，0..period）；vblank 未校准返回 -1。
static double vblank_phase_ms(double qpc)
{
    if (!g_vb_ok) { return -1.0; }
    double ph = std::fmod(qpc - g_vb_t0_ms, g_vb_period_ms);
    if (ph < 0.0) { ph += g_vb_period_ms; }
    return ph;
}
static const char* cad_policy_name(int p);   // 定义在本文件后面

// 把 CLI 开关翻译成控制器配置。在 init_fg() 末尾调用：那里 g_refresh_hz 与
// g_cad_kmax 都已确定。optimal 的 (m,k,paced) 选择见 src/cadence_core.h。
static void cad_core_configure()
{
    // 先校准真实 vblank，再用它做控制器的时间基准（V、目标、预算）。
    // 顺序很重要：Configure 里的 refreshHz 决定 V = 1000/refreshHz。
    vblank_init();
    mgpu_cad::Config cc;
    cc.refreshHz = g_vb_ok ? (1000.0 / g_vb_period_ms)
                           : ((g_refresh_hz > 0) ? static_cast<double>(g_refresh_hz) : 120.0);

    cc.maxOutputFrames = static_cast<int>(g_cad_kmax ? g_cad_kmax : g_fg_interp_max) + 1;
    cc.outHz = g_fg_out_hz;
    cc.policy = (g_cad_epoch_ms > 0.5) ? mgpu_cad::kPolicyEven
              : (g_cad_policy == 1) ? mgpu_cad::kPolicyEven
              : (g_cad_policy == 2) ? mgpu_cad::kPolicyFill
              : (g_cad_policy == 3) ? mgpu_cad::kPolicyLocked
                                    : mgpu_cad::kPolicyOptimal;

    cc.epochMs = g_cad_epoch_ms;
    cc.rounding = g_cad_rounding;
    cc.smooth = g_cad_smooth;
    cc.minDwell = (g_cad_hold_frames > 0) ? g_cad_hold_frames : 4;
    cc.deadband = 0.60;
    cc.latencyBudgetMs = g_cad_latency_budget_ms;
    cc.allowInputPacing = g_cad_allow_pacing;
    cc.costPerOutputMs = g_cad_cost_cap_ms;
    cc.autoTuneDwell = g_cad_switch_loss_auto;
    // 每批预算：input = 实测 T（现状）；vblank = k×vblank（节奏误差 0，多余输入帧被丢）。
    cc.feedWholeVblanks = g_cad_budget_vblank;

    g_cad_core.Configure(cc);
    g_cad_core_ready = true;
    // 真实 vblank：校准一次（阻塞约 9 个刷新周期），之后每帧只做相位计算。
    vblank_init();
    logf_("[consumer] cadence core: policy=%s J=%d panel=%uHz outHz=%.0f "
          "latency-budget=%.1fms pacing=%d cost-cap=%.1fms dwell=%d switch-loss=%s budget=%s\n",
          cad_policy_name(g_cad_policy),
          cc.maxOutputFrames, g_refresh_hz, cc.outHz, cc.latencyBudgetMs,
          cc.allowInputPacing ? 1 : 0, cc.costPerOutputMs, cc.minDwell,
          cc.autoTuneDwell ? "auto" : "measure-only",
          (cc.policy == mgpu_cad::kPolicyLocked || cc.feedWholeVblanks) ? "k*vblank" : "input-T");



    logf_("[consumer] cadence core: MFG assumes a FIXED refresh - disable VRR/"
          "Adaptive-Sync; a variable refresh adds a second pacing controller\n");
    // 默认是 optimal（带死区/代价函数）——真实锁帧一定有 ±1–2 帧抖动，
    // 纯 R/f 的整数切分（locked）在边界上会翻。这里只提示另一条路，不报警。
    if (cc.policy == mgpu_cad::kPolicyLocked) {
        logf_("[consumer] cadence core: policy=locked (idealised fixed rate);"
              " optimal is the default because real locking always wobbles\n");
    }


}

// ---- cadence core: 名字 + regime 迁移日志 --------------------------------
// reason 在日志里一直是裸数字（"reason=2"），排查时得翻头文件才知道是降档还是
// 被排空挡住。这里给出名字，并只在 regime 真正变化时打一行：cap-limited 是被
// "输入低于 target/J"压低的状态，必须说清楚它现在的真实输出率。
static const char* cad_reason_name(int r)
{
    switch (r)
    {
    case mgpu_cad::kHeld:      return "held";
    case mgpu_cad::kUp:        return "up";
    case mgpu_cad::kDown:      return "down";
    case mgpu_cad::kEnterCap:  return "enter-cap";
    case mgpu_cad::kExitCap:   return "exit-cap";
    case mgpu_cad::kCapEven:   return "cap-even";
    case mgpu_cad::kCostCapped:return "cost-capped";
    case mgpu_cad::kDeferred:  return "deferred";
    case mgpu_cad::kReuse:     return "reuse";
    case mgpu_cad::kHitch:     return "hitch";
    default:                   return "?";
    }
}

static const char* cad_policy_name(int p)
{
    switch (p)
    {
    case 1: return "even";
    case 2: return "fill";
    case 3: return "locked";
    default: return "optimal";
    }
}

static const char* cad_regime_name(int regime)
{
    switch (regime)
    {
    case mgpu_cad::kRegimeCapLimited:   return "cap-limited";
    case mgpu_cad::kRegimeInputLimited: return "input-limited";
    default:                            return "fillable";
    }
}

static void cad_note_regime(int regime, int jEff, double outHz, double targetHz)
{
    static int s_regime = -1;
    if (regime == s_regime) { return; }
    const bool first = (s_regime < 0);
    s_regime = regime;
    if (regime == mgpu_cad::kRegimeCapLimited) {
        // The actionable form: the panel only accepts integer k, so the game has to be
        // locked to R/k for the requested multiplier to actually fill the panel.
        const int kNow = (jEff > 0) ? jEff : 4;
        const double lockHz = (targetHz > 1.0 && kNow > 0) ? targetHz / static_cast<double>(kNow) : 0.0;
        logf_("[consumer] cadence core: cap-limited - the input needs more than J=%d"
              " frames per real frame, so the output tops out near %.0f Hz (target"
              " %.0f Hz). Raise --interp / --unlock-mfg, or lock the game to ~%.2f fps"
              " (%.0f/%d). Other exact locks: 60 (2x), 40 (3x), 30 (4x), 24 (5x).\n",
              jEff, outHz, targetHz, lockHz, targetHz, kNow);
    } else if (!first && regime == mgpu_cad::kRegimeFillable) {
        // input-limited 由 frame.cpp 的一次性提示负责，这里不重复。
        logf_("[consumer] cadence core: back to fillable (output %.0f of %.0f Hz)\n",
              outHz, targetHz);
    }
}



// count = interpolated frames (1 = 2x ... 5 = 6x); 0 = 1X, generation off.
static void fg_dyn_mfg_apply(int count)
{
    if (count <= 0) {
        if (!g_dyn_fg_off) {
            const xefg_swapchain_result_t r = g_api.SetEnabled(g_fg, 0);
            logf_("[consumer] dyn-mfg: 1X (frame generation off): %s (%d)\n", xefg_str(r), (int)r);
            if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS) { g_dyn_fg_off = true; }
        }
        return;
    }
    if (g_dyn_fg_off) {
        const xefg_swapchain_result_t r = g_api.SetEnabled(g_fg, 1);
        logf_("[consumer] dyn-mfg: back on: %s (%d)\n", xefg_str(r), (int)r);
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
            logf_("[consumer] dyn-mfg: %dx held: presented %.1f fps, target %.1f, lower %.1f,"
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
    logf_("[consumer] dyn-mfg: %dx -> %dx (%s): presented %.1f fps, target %.1f, lower %.1f,"
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
        logf_("[consumer] --interp-auto: input %.1fms wants %dx for a whole-vblank cadence"
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

    logf_("[consumer] overlay fits the monitor %ux%u at (%u,%u) (capture %ux%u)\n",
          *w, *h, *x, *y, g_full_w, g_full_h);
    if (*w != g_full_w || *h != g_full_h) {
        logf_("[consumer]   captured %ux%u will be resampled once by us to the output size;"
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
            logf_("[consumer] WARNING: --overlay-monitor %d is not driven by the consumer"
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
    if (!g_bridge_wnd) { logf_("[consumer] window creation FAILED\n"); return false; }

    xefg_swapchain_result_t r = g_api.D3D12CreateContext(g_device.Get(), &g_fg);
    logf_("[consumer] xefgSwapChainD3D12CreateContext: %s (%d)\n", xefg_str(r), (int)r);
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
            logf_("[consumer] maxSupportedInterpolations = %u (descriptors=%u)\n",
                  g_fg_interp_max, props.requiredDescriptorCount);
        } else {
            g_fg_interp_max = 1;
            logf_("[consumer] GetProperties FAILED - assuming 1 interpolated frame\n");
        }
    }

    // Only a *request* above what the DLL allows means the patch failed. With --interp 1
    // (2x) the patch code deliberately does nothing ("nothing to patch"), so warning here
    // was a false alarm - it fired on every plain 2x run and made people distrust the log.
    if (g_unlock_mfg && g_fg_interp_max <= 1 &&
        (g_fg_interp > 1 || g_interp_cadence || g_interp_auto || g_dyn_mfg)) {
        logf_("[consumer] WARNING: --unlock-mfg is on and more than 2x was requested, but the"
              " DLL still reports 1 interpolated frame. The patch did not take - do not"
              " expect MFG.\n");
    }

    UINT want_interp = (g_fg_interp > 0) ? static_cast<UINT>(g_fg_interp) : g_fg_interp_max;
    // --interp-cadence: start at 2x and let the phase accumulator climb. Starting at the
    // DLL maximum (the old behaviour) meant the first frames over-generated (6x against a
    // 3.375-vblank input), which is a visible surge before the count settles. The count is
    // allowed to change every frame, so starting high buys nothing.
    if (g_interp_cadence) { want_interp = 1; }
    // --interp is the capacity ceiling in cadence mode (--interp 3 => max 4 frames).
    g_cad_kmax = g_fg_interp_max;
    if (g_interp_cadence && g_fg_interp > 0) {
        const UINT cap = static_cast<UINT>(g_fg_interp);
        if (cap >= 1 && cap < g_cad_kmax) { g_cad_kmax = cap; }
    }
    if (g_cad_kmax < 1) { g_cad_kmax = 1; }
    if (g_interp_cadence) {
        logf_("[consumer] cadence capacity: %ux (--interp %d, kmax %u)\n",
              g_cad_kmax + 1, g_fg_interp, g_cad_kmax);
    }
    // --dyn-mfg starts at 4x even when the ceiling is 6x: starting at 6x at 4K measured
    // 45 ms of GPU wait per real frame and collapsed the loop to 13 fps
    // (bridge_pbz_dynmfg.txt). The controller now only climbs with evidence.
    if (g_dyn_mfg) { want_interp = (g_fg_interp_max >= 3) ? 3u : g_fg_interp_max; }
    if (want_interp < 1) { want_interp = 1; }
    if (want_interp > g_fg_interp_max) {
        logf_("[consumer] --interp %d above this DLL's limit (%u) - using %u\n",
              g_fg_interp, g_fg_interp_max, g_fg_interp_max);
        want_interp = g_fg_interp_max;
    }
    g_fg_interp_actual = want_interp;

    if (g_fg_interp_max <= 1) {
        logf_("[consumer] MFG unavailable: libxess_fg.dll allows 1 interpolated frame here,"
              " so the output is 2x.\n");
        logf_("[consumer]   a community MFG build (plus a matching libxell.dll) raises this;"
              " select it with --fg-lib <path>\n");
        if (g_fg_interp > 1) {
            logf_("[consumer]   --interp %d ignored, running 2x\n", g_fg_interp);
        }
    } else {
        logf_("[consumer] MFG available: up to %ux (%u interpolated frames per real frame)\n",
              g_fg_interp_max + 1, g_fg_interp_max);
        logf_("[consumer]   running %ux (%u interpolated)\n", want_interp + 1, want_interp);
    }
    if (!g_api.has_mfg()) {
        logf_("[consumer] note: DLL has no SetNumInterpolatedFrames, so the count cannot be"
              " changed at runtime\n");
        if (g_interp_cadence) {
            g_interp_cadence = false;
            logf_("[consumer] --interp-cadence disabled: the DLL cannot change the count;"
                  " running fixed 2x instead\n");
        }
    }

    // XeLL must be attached BEFORE the proxy swap chain is initialized: without
    // it InitFromSwapChain fails with ERR_LATENCY_REDUCTION_UNSUPPORTED (-15).
    const xell_result_t xr = xellD3D12CreateContext(g_device.Get(), &g_xell);
    logf_("[consumer] xellD3D12CreateContext: %d\n", (int)xr);
    if (xr == XELL_RESULT_SUCCESS) {
        // Creating the context is not enough - XeLL has to be *enabled*, and
        // xefgSwapChainSetEnabled returns ERR_LATENCY_REDUCTION_UNSUPPORTED (-15)
        // until bLowLatencyMode is on.
        xell_sleep_params_t sp{};
        sp.minimumIntervalUs = 0;
        sp.bLowLatencyMode = g_xell_mode;
        const xell_result_t sr = xellSetSleepMode(g_xell, &sp);
        logf_("[consumer] xellSetSleepMode(lowLatency): %d\n", (int)sr);

        const xefg_swapchain_result_t lr =
            g_api.SetLatencyReduction(g_fg, reinterpret_cast<void*>(g_xell));
        logf_("[consumer] SetLatencyReduction: %s (%d)\n", xefg_str(lr), (int)lr);
    } else {
        logf_("[consumer] no XeLL context - XeSS-FG cannot initialize\n");
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
        logf_("[consumer] device LUID %08lX:%08lX\n",
              static_cast<unsigned long>(luid.HighPart),
              static_cast<unsigned long>(luid.LowPart));

        HRESULT h = CreateDXGIFactory2(0, IID_PPV_ARGS(&f4));
        logf_("[consumer] CreateDXGIFactory2: hr=0x%08lX\n", static_cast<unsigned long>(h));
        if (FAILED(h)) { return false; }

        ComPtr<IDXGIAdapter1> ad;
        h = f4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad));
        logf_("[consumer] EnumAdapterByLuid: hr=0x%08lX\n", static_cast<unsigned long>(h));
        if (SUCCEEDED(h) && ad) {
            h = ad->GetParent(IID_PPV_ARGS(&factory));
            logf_("[consumer] Adapter->GetParent(IDXGIFactory2): hr=0x%08lX\n",
                  static_cast<unsigned long>(h));
        }
        if (!factory) { f4.As(&factory); }
        if (!factory) { logf_("[consumer] no DXGI factory\n"); return false; }

        // Which adapter drives the monitor our window sits on? A mismatch here is
        // one way CreateSwapChainForHwnd ends in E_ACCESSDENIED.
        const HMONITOR mon = MonitorFromWindow(g_bridge_wnd, MONITOR_DEFAULTTOPRIMARY);
        logf_("[consumer] window monitor %p\n", reinterpret_cast<void*>(mon));
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
                logf_("[consumer]   '%ls' output %u %p%s\n", d.Description, j,
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
            logf_("[consumer] display mode: %lux%lu @ %lu Hz%s\n",
                  dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency,
                  (g_overlay_monitor >= 0) ? " (overlay monitor)" : "");

            g_refresh_hz = dm.dmDisplayFrequency;
            // A 30 fps x4 profile needs the panel to actually run at 120 Hz. 4K HDR over
            // a link without DSC lands on 59/60 Hz, and then 4x can never show more than
            // ~60 fps no matter what the frame generator does.
            if (g_refresh_hz > 0 && g_refresh_hz < 100) {
                logf_("[consumer] WARNING: panel runs at %lu Hz - 30 fps x%d needs %lu Hz; the"
                      " output will cap near %lu fps. Check the display mode and the cable"
                      " (4K HDR 120 Hz needs DP 1.4 + DSC or HDMI 2.1).\n",
                      dm.dmDisplayFrequency, g_fg_interp + 1,
                      static_cast<unsigned long>(30 * (g_fg_interp + 1)),
                      dm.dmDisplayFrequency);
            }
        }
        logf_("[consumer]   colour space %d (%s) bits=%u maxLum=%.0f maxFull=%.0f\n",
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
        logf_("[consumer] initFlags=0x%x (inverted_depth=%d jittered_mv=%d ndc_velocity=%d)\n",
              static_cast<unsigned>(ip.initFlags), g_fg_depth_inv ? 1 : 0,
              g_fg_jittered_mv ? 1 : 0, ndc_mv ? 1 : 0);
    }
    // AUTO probes for a UI texture we never provide; NONE says it outright.
    // AUTO only when the UI really travels: XeFG composits a UI texture we tag, and with
    // none tagged "AUTO" is the same as promising something that never arrives.
    const bool have_ui_stream = g_ctl && g_ctl->streams[3].slot_size != 0 &&
                                g_ctl->streams[3].width >= 64;
    ip.uiMode = (!g_fg_ui_none && have_ui_stream) ? XEFG_SWAPCHAIN_UI_MODE_AUTO
                                                  : XEFG_SWAPCHAIN_UI_MODE_NONE;
    ip.creationNodeMask = 1;
    ip.visibleNodeMask  = 1;

    if (g_out_den > 1 || g_scale > 0.0f) {
        // XeSS-FG creates the native swap chain from our description but does not keep
        // DXGI_SCALING_STRETCH, so a smaller back buffer is shown 1:1 in the corner.
        // Creating the swap chain here (with STRETCH) and handing it over fixes that.
        r = XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        logf_("[consumer] InitFromSwapChainDesc: skipped (output %ux%u), creating it ourselves\n",
              sc.Width, sc.Height);
    } else {
        r = g_api.D3D12InitFromSwapChainDesc(g_fg, g_bridge_wnd, &sc, &fs,
                                             g_queue.Get(), factory.Get(), &ip);
        logf_("[consumer] InitFromSwapChainDesc: %s (%d)\n", xefg_str(r), (int)r);
        logf_("[consumer] startup: +%llu ms XeFG init\n", startup_ms());
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
                logf_("[consumer] hwnd %-8s %-18s hr=0x%08lX\n", where, c.name,
                      static_cast<unsigned long>(h));
                if (SUCCEEDED(h)) { app_swap = tmp; g_sc_wnd = wnd; return true; }
            }
            return false;
        };

        // 1) our own overlay window
        if (hwnd_attempts(g_bridge_wnd, "popup")) {
            logf_("[consumer] using hwnd swap chain on the overlay window\n");
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
            logf_("[consumer] probe window %p (click-through)\n", reinterpret_cast<void*>(plain));
            if (plain) { hwnd_attempts(plain, "plain"); }
        }

        // 3) composition swap chain + DirectComposition: needs no hwnd ownership
        if (!app_swap) {
            DXGI_SWAP_CHAIN_DESC1 d = sc;
            d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            ComPtr<IDXGISwapChain1> tmp;
            const HRESULT h = factory->CreateSwapChainForComposition(g_queue.Get(), &d, nullptr,
                                                                     &tmp);
            logf_("[consumer] CreateSwapChainForComposition: hr=0x%08lX\n",
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
                logf_("[consumer] DirectComposition attach: hr=0x%08lX\n",
                      static_cast<unsigned long>(hc));
                if (SUCCEEDED(hc)) { app_swap = tmp; }
            }
        }

        if (!app_swap) {
            logf_("[consumer] no swap chain could be created on this system\n");
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
        logf_("[consumer] InitFromSwapChain: %s (%d)\n", xefg_str(r), (int)r);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { return false; }
    }

    // What the library really applied (it may clamp the request).
    if (g_api.D3D12GetInitParams) {
        xefg_swapchain_d3d12_init_params_t got{};
        if (g_api.D3D12GetInitParams(g_fg, &got) == XEFG_SWAPCHAIN_RESULT_SUCCESS &&
            got.maxInterpolatedFrames > 0) {
            g_fg_interp_actual = got.maxInterpolatedFrames;
            logf_("[consumer] applied maxInterpolatedFrames = %u -> %ux output\n",
                  g_fg_interp_actual, g_fg_interp_actual + 1);
        }
    }

    r = g_api.D3D12GetSwapChainPtr(g_fg, IID_PPV_ARGS(&g_swapchain));
    logf_("[consumer] GetSwapChainPtr: %s (%d) ptr=%p\n", xefg_str(r), (int)r,
          reinterpret_cast<void*>(g_swapchain.Get()));
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS || !g_swapchain) {
        logf_("[consumer] GetSwapChainPtr FAILED\n");
        return false;
    }

    {
        DXGI_SWAP_CHAIN_DESC1 real{};
        if (SUCCEEDED(g_swapchain->GetDesc1(&real))) {
            logf_("[consumer] FG swap chain: %ux%u format=%u buffers=%u flags=0x%X\n",
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
            logf_("[consumer] SetMaximumFrameLatency(%u): hr=0x%08lX\n", g_max_latency,
                  static_cast<unsigned long>(h));
        }
    }

    // which reports HDR10 active. scRGB cannot be used here - XeSS-FG rejects it.
    const DXGI_COLOR_SPACE_TYPE want_cs =
        (g_colour_mode == 2) ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
                             : DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const HRESULT cs = g_swapchain->SetColorSpace1(want_cs);
    logf_("[consumer] SetColorSpace1(%s): hr=0x%08lX\n",
          (g_colour_mode == 2) ? "SDR" : "HDR10", static_cast<unsigned long>(cs));

    // The swap chain exists now, so the window that owns it becomes the visible
    // overlay. Our own window stays hidden unless it was the one that won, which
    // avoids a blank white window sitting on top of the picture.
    make_borderless(overlay_wnd(), static_cast<int>(ox), static_cast<int>(oy), ow, oh);
    if (g_overlay_layered) {
        const LONG_PTR ex = GetWindowLongPtrW(overlay_wnd(), GWL_EXSTYLE);
        SetWindowLongPtrW(overlay_wnd(), GWL_EXSTYLE, ex | WS_EX_LAYERED);
        const BOOL ok = SetLayeredWindowAttributes(overlay_wnd(), 0, 255, LWA_ALPHA);
        logf_("[consumer] --overlay-layered: WS_EX_LAYERED + alpha 255 (ok=%d)\n",
              static_cast<int>(ok));
    }

    if (g_no_fg) {
        logf_("[consumer] --no-fg: proxy swap chain only, frame generation left off\n");
        return true;
    }

    if (g_fg_scene > 0.0f) {
        if (g_api.SetSceneChangeThreshold) {
            const xefg_swapchain_result_t sc = g_api.SetSceneChangeThreshold(g_fg, g_fg_scene);
            logf_("[consumer] SetSceneChangeThreshold(%.2f): %s (%d)\n",
                  g_fg_scene, xefg_str(sc), static_cast<int>(sc));
        } else {
            logf_("[consumer] --fg-scene ignored: DLL has no SetSceneChangeThreshold\n");
        }
    }

    r = g_api.SetEnabled(g_fg, 1);
    logf_("[consumer] SetEnabled: %s (%d)\n", xefg_str(r), (int)r);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { return false; }

    // If the init parameter could not express the request (older DLL, or a
    // mismatch after the library clamped it) try the runtime call as well.
    fg_apply_interp("after SetEnabled");

    // Configure the (m,k,paced) controller now: g_refresh_hz and g_cad_kmax are final.
    if (g_interp_cadence) { cad_core_configure(); }
    // 真实 vblank 校准不依赖 cadence：固定倍率基线也要有 period/vphase，
    // 否则我们用来对照"顺"的那套配置反而没有显示端诊断。
    vblank_init();
    if (g_out_clock) {
        const double ocp = oc_period_ms();
        const double batch = static_cast<double>(g_fg_interp_actual + 1) * refresh_ms();
        logf_("[consumer] out-clock: P=%.3f ms (%.2f vblank), k=%u, QPC schedule;"
              " queue=2 coast<=2; fixed multiplier (no per-frame count change)\n",
              ocp, ocp / refresh_ms(), g_fg_interp_actual + 1);
        if (ocp < batch - 0.15 * refresh_ms() ||
            ocp > batch + 0.15 * refresh_ms()) {
            logf_("[consumer] out-clock: WARNING P=%.2fms != k*vblank=%.2fms;"
                  " choose --out-clock %u or --interp %u to match\n",
                  ocp, batch,
                  static_cast<unsigned>(g_refresh_hz / std::max(1u, g_fg_interp_actual + 1)),
                  g_fg_interp_actual);
        }
    }
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
        for (int k = 0; k < static_cast<int>(mgpu::kMaxStreams); ++k) {
            // 0 = colour/hudless, 1 = MV, 2 = depth, 3 = UI, 4 = scar (v17)
            const mgpu::StreamShared& st = g_ctl->streams[k];
            if (st.slot_size == 0) { continue; }
            ComPtr<ID3D12Resource>& dst = (k == 0) ? g_colour[s]
                                          : (k == 1) ? g_mv[s]
                                          : (k == 2) ? g_depth[s]
                                          : (k == static_cast<int>(mgpu::kStreamScar))
                                                    ? g_scar[s]
                                                    : g_ui[s];
            if (dst) { continue; }                     // already placed (v9: never moves)
            const UINT pw = st.slot_w ? st.slot_w : st.width;
            const UINT ph = st.slot_h ? st.slot_h : st.height;
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

            const UINT64 off = st.offset + st.slot_size * s;
            if (FAILED(g_device->CreatePlacedResource(g_shared_heap.Get(), off, &rd,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&dst)))) {
                logf_("[consumer] placed resource FAILED (stream %d slot %u)\n", k, s);
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
            logf_("[consumer] shared block present: producer pid=%u luid=%08lX:%08lX"
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
    logf_("[consumer] waiting for '%s' ...\n", mgpu::kMappingName);

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
                        break;
                    }
                    // A live mapping that speaks another protocol: waiting cannot
                    // fix that, and silently reading it would pair the wrong fields
                    // (v5 had no `producer`, so the LUID would be garbage).
                    logf_("[consumer] shared block version %u, this bridge speaks %u -"
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
            logf_("[consumer] still waiting after 10 s. The mapping is created by the add-on on"
                  " its first captured frame, so if it never appears the add-on is not running:"
                  " check (1) ReShade loaded into the game, (2) at least one enabled technique"
                  " (ReShadePreset.ini), (3) DLSS on in-game, (4) %ls is the process you expect."
                  " See docs/findings/FINAL_CONFIG.md 2.1.1.\n",
                  g_game_exe_set ? g_game_exe.c_str() : L"(foreground window)");
        }
        Sleep(100);
    }
    if (version_mismatch) { return false; }
    if (!g_ctl) { logf_("[consumer] shared block never appeared\n"); return false; }

    g_width  = g_ctl->streams[0].width;
    g_height = g_ctl->streams[0].height;
    logf_("[consumer] shared: pid=%u %ux%u slots=%u\n",
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
        logf_("[consumer] shared: producer luid=%08lX:%08lX api=%s flags=0x%X"
              " render=%ux%u output=%ux%u\n",
              static_cast<unsigned long>(static_cast<uint32_t>(hi)),
              static_cast<unsigned long>(lo), input_api_name(g_producer_api),
              g_producer_flags, g_producer_render_w, g_producer_render_h,
              g_producer_output_w, g_producer_output_h);

        if (declared_frames != 0 || harvest_flags != 0) {
            static bool logged_v14 = false;
            if (!logged_v14) {
                logged_v14 = true;
                logf_("[consumer]   producer declaration: framesToGenerate=%u%s harvest=0x%X"
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
            logf_("[consumer]   producer supplies the camera (near/far/vfov) - the"
                  " projection no longer depends on --fg-fov\n");
        }
        adopt_producer_hints(g_producer_flags);
        if (g_have_producer) {
            const uint64_t consumer_luid =
                mgpu::pack_luid(g_consumer_desc.AdapterLuid.HighPart,
                                static_cast<uint32_t>(g_consumer_desc.AdapterLuid.LowPart));
            if (consumer_luid == g_producer_luid) {
                logf_("[consumer] WARNING: producer and consumer are the SAME GPU - there is"
                      " no cross-adapter transfer, frame generation competes with the game\n");
            }
        }
    }
    // The colour stream is a placeholder (the add-on does not allocate it with Color=0);
    // MV and depth are the ones that actually matter. Their size follows the game's
    // render resolution - DLSS Quality at 4K is 1440p, DLAA is 2160p - narrowed by the
    // add-on's [capture] StreamPercent (100 = full, 200 = half, 400 = quarter).
    logf_("[consumer] MV %ux%u, depth %ux%u (stream size)\n",
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
            logf_("[consumer] NOTE: the stream is %ux%u (%.0f MB/frame with depth). The same"
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
        logf_("[consumer] shared block declares %ux%u, present at scale %.2f\n",
              g_width, g_height, g_scale);
    } else if (g_out_den > 1) {
        logf_("[consumer] shared block declares %ux%u, present at 1/%d\n",
              g_width, g_height, g_out_den);
    } else {
        logf_("[consumer] shared block declares %ux%u, present at the same size\n",
              g_width, g_height);
    }

    if (g_no_inputs) {
        logf_("[consumer] --no-inputs: not importing the cross-adapter heap\n");
        return true;
    }
    if (g_no_depth) {
        logf_("[consumer] --no-depth: depth stream skipped, XeFG gets a dummy depth"
              " (disocclusion quality will drop, transfer halves)\n");
    }
    if (g_no_mv) {
        logf_("[consumer] --no-mv: motion vectors are zeroed, the real depth is kept\n");
    }
#if MGPU_FG_OF
    if (g_fg_of) {
        logf_("[consumer] --fg-of: motion vectors come from FidelityFX optical flow on the"
              " captured colour (the producer's MV stream is not copied)\n");
        if (g_of_const_on) {
            logf_("[consumer]   DIAGNOSTIC: constant MV (%.1f, %.1f) px on every frame\n",
                  static_cast<double>(g_of_const_x), static_cast<double>(g_of_const_y));
        }
        logf_("[consumer]   units=%s sign=%.0f scale=%.3f guide=%.0f deadzone=%.2fpx max=%.0fpx\n",
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
        logf_("[consumer] OpenExistingHeapFromFileMapping FAILED\n");
        return false;
    }

    if (!place_shared_views()) { return false; }

    // v22: pick the convert mode from the colour stream's own encoding. R10 is already
    // display-encoded PQ (HDR10 swap chain) and must pass through; FP16 is scRGB (linear,
    // 1.0 = 80 nits) and goes through the scRGB -> BT.2020 PQ path. --sdr/--no-primaries
    // still win for SDR output and for A/B.
    if (g_ctl->streams[mgpu::kStreamColour].format ==
        static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM)) {
        if (g_colour_mode != 2) { g_colour_mode = 3; }
        logf_("[consumer] colour stream is HDR10 PQ (R10G10B10A2): convert is a pass-through\n");
    } else if (g_colour_mode == 3) {
        g_colour_mode = 0;
    }
    // Read the published stream geometry once here: service_shared_layout() only re-reads
    // it when `generation` changes, so this is the first and possibly only time. If the
    // producer has not published a size yet, that path keeps retrying (mv_w == 0).
    g_seen_generation = g_ctl->generation;
    g_seen_geo_generation = 0xFFFFFFFFu;    // force the geometry read below
    refresh_input_geo();
    g_seen_geo_generation = g_ctl->generation;

    HANDLE producer = OpenProcess(PROCESS_DUP_HANDLE, FALSE, g_ctl->producer_pid);
    if (!producer) {
        const DWORD err = GetLastError();
        logf_("[consumer] OpenProcess(PROCESS_DUP_HANDLE, pid=%u) FAILED: %lu%s\n",
              g_ctl->producer_pid, static_cast<unsigned long>(err),
              (err == ERROR_ACCESS_DENIED)
                  ? "  <== ACCESS DENIED: run the consumer with the SAME elevation as"
                    " the game (right-click the .bat -> Run as administrator if Steam/"
                    " the game is elevated), then start it again"
                  : "  <== the producer may have exited; restart the game and consumer");
        // A diagnostic open without DUP_HANDLE distinguishes "gone" from "denied".
        HANDLE q = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_ctl->producer_pid);
        if (q) {
            logf_("[consumer]   the process exists; it is only the handle duplication"
                  " that is denied (elevation mismatch)\n");
            CloseHandle(q);
        }
        return false;
    }
    for (UINT s = 0; s < g_ctl->slots && s < mgpu::kMaxSlots; ++s) {
        HANDLE lh = nullptr;
        if (!DuplicateHandle(producer, reinterpret_cast<HANDLE>(g_ctl->fence_forward[s]),
                             GetCurrentProcess(), &lh, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            logf_("[consumer] DuplicateHandle FAILED\n");
            CloseHandle(producer);
            return false;
        }
        const HRESULT hr = g_device->OpenSharedHandle(lh, IID_PPV_ARGS(&g_fwd[s]));
        CloseHandle(lh);
        if (FAILED(hr)) {
            logf_("[consumer] fence OpenSharedHandle FAILED\n");
            CloseHandle(producer);
            return false;
        }
    }
    CloseHandle(producer);

    logf_("[consumer] MV/depth placed, fences duplicated\n");
    return true;
}


// v22: the present ring (fakerFG -> consumer, WGC timestamp -> seq) is deleted with the
// WGC colour path. A publication carries colour + MV + depth, so the slot is the key.
// One keyed mutex per sink (the D3D11 side is the only acquirer; D3D12 is ordered by the
// shared fence). Triple buffered together with g_shared11/g_shared12.
static ComPtr<IDXGIKeyedMutex> g_colour_mutex_pair[kColourSinks];
#define g_colour_mutex (g_colour_mutex_pair[g_sink])

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
static UINT g_srv_stride = 0;   // SRV descriptor size (two colour-sink descriptors)
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
            logf_("[consumer] swap chain GetBuffer(%u) FAILED\n", index);
            return false;
        }
    }
    if (out) { *out = g_bb_cache[index].Get(); }
    return true;
}
static UINT g_rtv_stride = 0;

