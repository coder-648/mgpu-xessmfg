// ============================================================================
//  consumer module: main.cpp
//
//  main: argument parsing, startup, capture/present loops.
//
//  Included by src/xess_mfg_consumer/unity.cpp as part of the unity build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

#include "../consumer_internal.h"
// ============================================================================
//  v22: the colour arrives through the producer's publication (stream 0).
//
//  There is no WGC session, frame pool, capture thread or colour queue: one
//  publication is one frame, and the main loop below waits for its fence before
//  running it. See docs/design/COLOUR_TRANSPORT_V22.md.

// 精确匹配一个“不带值”的开关：名字后面必须是结尾、空白或引号。
//
// 反例（2026-11-15 实测）：`--no-fg-pace` 里包含 `--no-fg`，用
// `wcsstr(L"--no-fg")` 会连帧生成一起关掉 —— `run_consumer_carry_nopace.bat`
// 因此整轮跑成 1X（日志 `mode: fg=off`，xN=1.00），A/B 结论完全无效。
// 历史上 `run_consumer_dynamic_nopace.bat` 的“no pacer”实验也受此影响。
static bool flag_alone(const wchar_t* name)
{
    const wchar_t* cmd = GetCommandLineW();
    const size_t n = wcslen(name);
    for (const wchar_t* p = wcsstr(cmd, name); p != nullptr; p = wcsstr(p + 1, name)) {
        const wchar_t c = p[n];
        if (c == L'\0' || c == L' ' || c == L'\t' || c == L'"') { return true; }
    }
    return false;
}

int main()
{
    // Must happen before any window is created: otherwise Windows virtualizes the
    // window size by the display DPI (175% here) and the client rect no longer
    // matches the 5120x2880 swap chain we ask XeSS-FG for.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Announce ourselves before waiting for the producer's mapping. The producer only
    // creates the 90-540 MB transport when this event exists, so a game launch without
    // the consumer never pays for it during startup.
    {
        static HANDLE s_present_ev =
            CreateEventW(nullptr, TRUE, FALSE, mgpu::kConsumerPresentEvent);
        if (s_present_ev != nullptr) {
            SetEvent(s_present_ev);
        } else {
            logf_("[consumer] CreateEvent(%ls) FAILED err=%lu - the producer may not"
                  " create the transport\n", mgpu::kConsumerPresentEvent, GetLastError());
        }
    }

    // 必须用精确匹配：--no-fg-pace 不能把帧生成关掉（见 flag_alone）。
    if (flag_alone(L"--no-fg")) { g_no_fg = true; }

    // v22: --no-pair-key / --pair-bias-ms are accepted but ignored (the publication slot
    // is the pairing key; there is no timestamp matching any more).
    if (wcsstr(GetCommandLineW(), L"--no-inputs")) { g_no_inputs = true; }
    if (wcsstr(GetCommandLineW(), L"--streams=")) {
        // --streams=mv,depth   select exactly the streams the producer should transfer.
        // `--streams=none` then a later flag is not supported on purpose: one list wins.
        const wchar_t* p = wcsstr(GetCommandLineW(), L"--streams=") + 10;
        wchar_t buf[128] = {};
        int n = 0;
        while (p[n] != 0 && p[n] != L' ' && p[n] != L'"' && n < 127) { buf[n] = p[n]; ++n; }
        buf[n] = 0;
        uint32_t mask = 0;
        if (wcsicmp(buf, L"all") == 0) { mask = mgpu::kRequestAll; }
        else if (wcsicmp(buf, L"none") == 0) { mask = 0; }
        else {
            wchar_t* ctx = nullptr;
            for (wchar_t* t = wcstok(buf, L",", &ctx); t != nullptr; t = wcstok(nullptr, L",", &ctx)) {
                if (wcsicmp(t, L"mv") == 0)         { mask |= mgpu::kRequestMv; }
                else if (wcsicmp(t, L"depth") == 0) { mask |= mgpu::kRequestDepth; }
                else if (wcsicmp(t, L"color") == 0) { mask |= mgpu::kRequestColor; }
                else if (wcsicmp(t, L"ui") == 0)    { mask |= mgpu::kRequestUi; }
                else if (wcsicmp(t, L"meta") == 0)  { mask |= mgpu::kRequestMeta; }
                // v15 dumb-producer switches: the producer copies 1:1 at source resolution
                // and we do the resample/convert here instead (section 11 of TRANSFER_V2).
                else if (wcsicmp(t, L"rawmv") == 0)    { mask |= mgpu::kRequestRawMv; }
                else if (wcsicmp(t, L"rawdepth") == 0) { mask |= mgpu::kRequestRawDepth; }
            }
        }
        g_stream_mask = mask;
        g_stream_mask_explicit = true;
        logf_("[consumer] --streams: mask=0x%X (mv=%d depth=%d color=%d ui=%d meta=%d)\n", mask,
              (mask & mgpu::kRequestMv) ? 1 : 0, (mask & mgpu::kRequestDepth) ? 1 : 0,
              (mask & mgpu::kRequestColor) ? 1 : 0, (mask & mgpu::kRequestUi) ? 1 : 0,
              (mask & mgpu::kRequestMeta) ? 1 : 0);
        logf_("[consumer] --streams raw: mv=%d depth=%d\n",
              (mask & mgpu::kRequestRawMv) ? 1 : 0, (mask & mgpu::kRequestRawDepth) ? 1 : 0);
    }
    if (wcsstr(GetCommandLineW(), L"--link=")) {
        // Effective one-way bandwidth of the GPU-to-GPU link, in GB/s. Only used by the OSD's
        // occupancy figure; nothing throttles on it (a wrong value is a wrong readout, not a
        // wrong frame). 13.0 = PCIe 4.0 x8 after bifurcation.
        const wchar_t* p = wcsstr(GetCommandLineW(), L"--link=") + 7;
        const double v = _wtof(p);
        if (v > 0.5 && v < 200.0) {
            g_link_gbps = v;
            logf_("[consumer] link budget declared as %.1f GB/s (OSD occupancy)\n", v);
        }
    }
    if (wcsstr(GetCommandLineW(), L"--no-depth")) { g_no_depth = true; }
    if (wcsstr(GetCommandLineW(), L"--no-mv")) { g_no_mv = true; }
    if (wcsstr(GetCommandLineW(), L"--mv-shadow")) { g_mv_shadow = true; }
    // --log-flush: fflush after every line (pre-change behaviour). Useful when watching
    // for a crash, where the last lines before the process dies are the interesting ones.
    if (wcsstr(GetCommandLineW(), L"--log-flush")) { g_log_flush_each = 1; }
    if (wcsstr(GetCommandLineW(), L"--capture-monitor")) { g_capture_monitor = true; }
    if (wcsstr(GetCommandLineW(), L"--marker-probe")) {
        g_marker_probe = true;
        logf_("[consumer] --marker-probe: verbose marker diagnostics\n");
    }
    // v17: fakerFG draws the exact-seq marker by default (opt-out marker_disable.on)
    // and the 16x16 pre-marker tile rides the MV/depth publication. Decode/repair by
    // default so the scar never depends on a command-line flag; --no-marker opts out.
    if (!wcsstr(GetCommandLineW(), L"--no-marker")) {
        g_marker_probe = true;
    } else {
        g_marker_probe = false;
        logf_("[consumer] --no-marker: colour/MV pairing uses timestamps, no scar repair\n");
    }
    // v22: the WGC capture options (--no-capture-thread / --legacy-poll / --colour-queue /
    // --wgc-minint) were removed with the capture path. Old .bat files that still pass them
    // are ignored rather than breaking startup.

    // --overlay-monitor N: present on the Nth display instead of the game window's display.
    // Parsed before the game window is found, because the consumer adapter is chosen by
    // "which adapter drives the overlay monitor".
    if (const wchar_t* om = wcsstr(GetCommandLineW(), L"--overlay-monitor ")) {
        const int n = _wtoi(om + 18);
        if (n >= 0) { g_overlay_monitor = n; }
    }
    // --overlay-hidden: keep presenting, never show the window (measurement only).
    if (wcsstr(GetCommandLineW(), L"--overlay-hidden")) { g_overlay_hidden = true; }

    // --pid N: target a process by id (the GUI knows the window's pid).
    if (const wchar_t* pp = wcsstr(GetCommandLineW(), L"--pid ")) {
        const int n = _wtoi(pp + 6);
        if (n > 0) { g_game_pid = static_cast<DWORD>(n); }
    }
    // --hwnd H: target this exact window (decimal or 0x...). Most precise.
    if (const wchar_t* hp = wcsstr(GetCommandLineW(), L"--hwnd ")) {
        const wchar_t* p = hp + 7;
        while (*p == L' ') { ++p; }
        const unsigned long long v = wcstoull(p, nullptr, 0);
        if (v != 0) { g_game_hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(v)); }
    }
    // --game <exe>: the process whose window we capture and present over.
    if (const wchar_t* gp = wcsstr(GetCommandLineW(), L"--game ")) {
        const wchar_t* p = gp + 7;
        while (*p == L' ' || *p == L'"') { ++p; }
        const wchar_t* end = p;
        const bool quoted = (*(gp + 7) == L'"');
        while (*end && ((quoted && *end != L'"') || (!quoted && *end != L' '))) { ++end; }
        if (end > p) {
            g_game_exe.assign(p, end);
            // accept "PBZ" as well as "PBZ.exe"
            if (g_game_exe.find(L'.') == std::wstring::npos) { g_game_exe += L".exe"; }
            g_game_exe_set = true;
        }
    }
    // --gpu <spec>: which adapter runs XeSS-FG. Default is "the one that drives the
    // game window's monitor", which is also the only one the overlay can present on.
    //   --gpu 0 / 1                          adapter index (EnumAdapters1 order)
    //   --gpu nv / amd / intel               vendor alias
    //   --gpu 10de / 1002 / 8086 / 0x10de    vendor id
    //   --gpu luid:1:2A3B                    exact LUID
    //   --gpu 6750 / 3080                    description substring
    if (const wchar_t* gs = wcsstr(GetCommandLineW(), L"--gpu ")) {
        const wchar_t* p = gs + 6;
        while (*p == L' ') { ++p; }
        const wchar_t* end = p;
        while (*end && *end != L' ') { ++end; }
        if (end > p) { g_gpu_spec.assign(p, end); }
    }
    // --producer-luid HI:LO (hex): the GPU the game renders on, so the pick can avoid
    // it. Protocol v6 publishes this from the add-on; until then it is passed by hand.
    if (const wchar_t* pl = wcsstr(GetCommandLineW(), L"--producer-luid ")) {
        unsigned long long hi = 0, lo = 0;
        if (swscanf_s(pl + 16, L"%llx:%llx", &hi, &lo) == 2) {
            g_producer_luid = (hi << 32) | (lo & 0xFFFFFFFFull);
            g_have_producer = true;
        }
    }
    if (!g_gpu_spec.empty()) {
        logf_("[consumer] --gpu %ls: consumer adapter forced by spec\n", g_gpu_spec.c_str());
    }
    if (g_have_producer) {
        logf_("[consumer] --producer-luid %08lX:%08lX: the pick will avoid the producer's"
              " GPU\n",
              static_cast<unsigned long>(g_producer_luid >> 32),
              static_cast<unsigned long>(g_producer_luid & 0xFFFFFFFFull));
    }
    if (wcsstr(GetCommandLineW(), L"--xell")) { g_no_xell = false; }
    if (wcsstr(GetCommandLineW(), L"--no-xell")) { g_no_xell = true; }
    if (wcsstr(GetCommandLineW(), L"--fg-jitter-flip")) { g_fg_jitter_flip = true; }
    if (wcsstr(GetCommandLineW(), L"--fg-no-proj")) { g_fg_no_proj = true; }
    if (wcsstr(GetCommandLineW(), L"--fg-use-matrix")) { g_fg_use_matrix = true; }
    if (wcsstr(GetCommandLineW(), L"--fg-proj-rz")) { g_fg_proj_rz = true; }
    if (const wchar_t* fv = wcsstr(GetCommandLineW(), L"--fg-fov ")) {
        const float v = static_cast<float>(_wtof(fv + 9));
        if (v > 20.0f && v < 140.0f) { g_fg_fov_y = v; }
    }
    if (const wchar_t* nz = wcsstr(GetCommandLineW(), L"--fg-zn ")) {
        const float v = static_cast<float>(_wtof(nz + 8));
        if (v > 0.001f && v < 1000.0f) { g_fg_near_z = v; }
    }
    // MFG. --interp N is the number of *interpolated* frames per real frame:
    // 1 = 2x, 2 = 3x, 3 = 4x. 0 (default) = the most the loaded DLL allows, which
    // is what we always did (XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES).
    if (const wchar_t* it = wcsstr(GetCommandLineW(), L"--interp ")) {
        const int n = _wtoi(it + 9);
        if (n >= 0 && n <= 7) { g_fg_interp = n; g_fg_interp_set = (n > 0); }
    }
    if (const wchar_t* fl = wcsstr(GetCommandLineW(), L"--fg-lib ")) {
        const wchar_t* p = fl + 9;
        while (*p == L' ' || *p == L'"') { ++p; }
        const wchar_t* end = p;
        bool quoted = (*(fl + 9) == L'"');
        while (*end && ((quoted && *end != L'"') || (!quoted && *end != L' '))) { ++end; }
        if (end > p) { g_fg_lib.assign(p, static_cast<size_t>(end - p)); }
    }
    if (wcsstr(GetCommandLineW(), L"--fg-probe")) { g_fg_probe = true; }
    if (wcsstr(GetCommandLineW(), L"--unlock-mfg")) { g_unlock_mfg = true; }
    // XeFG tuning (see the globals above).
    if (const wchar_t* rt = wcsstr(GetCommandLineW(), L"--fg-rt ")) {
        if (wcsncmp(rt + 8, L"0", 1) == 0) { g_fg_rt_zero = true; }
    }
    if (wcsstr(GetCommandLineW(), L"--fg-ui none")) { g_fg_ui_none = true; }
    if (const wchar_t* sc = wcsstr(GetCommandLineW(), L"--fg-scene ")) {
        const float v = static_cast<float>(_wtof(sc + 11));
        if (v > 0.0f && v <= 10.0f) { g_fg_scene = v; }
    }
    if (wcsstr(GetCommandLineW(), L"--fg-depth-inv")) { g_fg_depth_inv = true; }
    // --fg-jittered-mv / --fg-no-jittered-mv: XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV ("remove
    // jitter from input velocity"). The add-on publishes the game's own DLSS MVJittered
    // declaration, so normally this turns itself on; these two force it for an A/B run.
    if (wcsstr(GetCommandLineW(), L"--fg-jittered-mv")) {
        g_fg_jittered_mv = true;
        g_fg_jittered_mv_forced = true;
    }
    if (wcsstr(GetCommandLineW(), L"--fg-no-jittered-mv")) {
        g_fg_jittered_mv = false;
        g_fg_jittered_mv_forced = true;
    }
    // --fg-mv-scale raw|px|<float>: see the globals above.
    if (const wchar_t* mvs = wcsstr(GetCommandLineW(), L"--fg-mv-scale ")) {
        const wchar_t* p = mvs + 14;
        while (*p == L' ') { ++p; }
        if (wcsncmp(p, L"px", 2) == 0) {
            g_fg_mv_scale_mode = 1;
        } else if (wcsncmp(p, L"raw", 3) == 0) {
            g_fg_mv_scale_mode = 0;
        } else {
            const float v = static_cast<float>(_wtof(p));
            if (v > 0.0f && v < 100000.0f) {
                g_fg_mv_scale_mode = 2;
                g_fg_mv_scale_force = v;
            }
        }
    }
    // --probe-colour N: log whether the captured colour is black every N frames.
    if (const wchar_t* pcx = wcsstr(GetCommandLineW(), L"--probe-colour ")) {
        const int n = _wtoi(pcx + 15);
        if (n > 0) { g_probe_colour = n; }
    }
    if (wcsstr(GetCommandLineW(), L"--tearing")) { g_tearing = true; }
    if (wcsstr(GetCommandLineW(), L"--overlay-layered")) { g_overlay_layered = true; }
    if (wcsstr(GetCommandLineW(), L"--relay-input")) { g_relay_input = true; }
    if (wcsstr(GetCommandLineW(), L"--no-osd")) { g_no_osd = true; }
    // --cpu-low: the bridge is a consumer; when the game's render thread is the limiter,
    // competing with it for CPU shows up as input jitter (arr 2/3/4) long before it shows
    // in any GPU counter. Below-normal still gets scheduled; it just yields first.
    if (wcsstr(GetCommandLineW(), L"--cpu-low")) {
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        logf_("[consumer] --cpu-low: bridge process below normal, main thread below normal"
              " (does not touch the game/producer process)\n");
    }
    if (const wchar_t* el = wcsstr(GetCommandLineW(), L"--exit-on-focus-loss ")) {
        const int n = _wtoi(el + 22);
        if (n >= 0 && n <= 3600) { g_exit_after_s = static_cast<UINT>(n); }
    }
    if (const wchar_t* dl = wcsstr(GetCommandLineW(), L"--depth-lag ")) {
        const int n = _wtoi(dl + 12);
        if (n >= 0 && n < static_cast<int>(mgpu::kMaxSlots)) { g_depth_lag = n; }
    }
    if (wcsstr(GetCommandLineW(), L"--fg-jitter-zero")) {
        g_fg_jitter_zero = true;
        logf_("[consumer] --fg-jitter-zero: passing jitter = 0. Our colour is the post-upscaler"
              " image (the jitter is already resolved), so this is the A/B control for"
              " 'is XeFG shifting its history by a jitter that is no longer there?'\n");
    }
    if (wcsstr(GetCommandLineW(), L"--fg-debug-tag-interp")) { g_fg_tag_interp = true; }
    if (wcsstr(GetCommandLineW(), L"--fg-debug-only-interp")) { g_fg_only_interp = true; }
    // --list-modes: print the refresh rates this output supports and the exact input
    // rate for every output multiplier, then exit. No game or shared memory needed.
    if (wcsstr(GetCommandLineW(), L"--list-modes")) { return list_display_modes(); }
    // --fg-rt-mode ema|raw|median|quant: which estimate of the input frame time goes
    // into frameRenderTime (XeFG's presentation-pacing sanity check).
    if (const wchar_t* rm = wcsstr(GetCommandLineW(), L"--fg-rt-mode ")) {
        const wchar_t* p = rm + 13;
        while (*p == L' ') { ++p; }
        if (wcsncmp(p, L"raw", 3) == 0)         { g_fg_rt_mode = kFgRtRaw; }
        else if (wcsncmp(p, L"median", 6) == 0) { g_fg_rt_mode = kFgRtMedian; }
        else if (wcsncmp(p, L"auto", 4) == 0)   { g_fg_rt_mode = kFgRtAuto; }
        else if (wcsncmp(p, L"quant", 5) == 0)  { g_fg_rt_mode = kFgRtQuant; }
        else                                    { g_fg_rt_mode = kFgRtEma; }
    }
    // --fg-rt-spike 0|1: absorb single-frame input spikes by reporting the real interval to
    // XeFG for that frame (default on; --fg-rt-spike 0 disables it for A/B).
    if (const wchar_t* sp = wcsstr(GetCommandLineW(), L"--fg-rt-spike ")) {
        g_fg_rt_spike = (_wtoi(sp + 14) != 0);
    }
    // --cadence-source tick|mean: where the per-frame count comes from. `tick` (default)
    // quantises the real arrival timestamp to the vblank grid and uses the tick difference;
    // `mean` is the old mean + phase accumulator (stable but lags a moving rate).
    if (const wchar_t* csrc = wcsstr(GetCommandLineW(), L"--cadence-source ")) {
        const wchar_t* v = csrc + 17;
        if (wcsncmp(v, L"mean", 4) == 0) { g_cad_source = kCadSourceMean; }
        else if (wcsncmp(v, L"tick", 4) == 0) { g_cad_source = kCadSourceTick; }
        else if (wcsncmp(v, L"governor", 8) == 0) { g_cad_source = kCadSourceGovernor; }
    }
    // --cadence-smooth N: EMA window (frames) for the interval that drives the cadence math.
    if (const wchar_t* cs = wcsstr(GetCommandLineW(), L"--cadence-smooth ")) {
        const int n = _wtoi(cs + 17);
        if (n >= 1 && n <= 240) { g_cad_smooth = n; }
    }
    // --cadence-hold N: keep the DLL count for N input frames before changing it (then
    // step by at most one). 0/1 = change whenever the phase says so. Default 12.
    // One count change costs about one output frame on this pacer (2026-11-14 log), so
    // holding removes the "missing frames" judder while the phase keeps the average exact.
    if (const wchar_t* ch = wcsstr(GetCommandLineW(), L"--cadence-hold ")) {
        const int n = _wtoi(ch + 15);
        if (n >= 0 && n <= 240) { g_cad_hold_frames = n; }
    }
    // --no-fg-pace: disable the FG-input vblank pacer (dynamic cadence only).
    if (wcsstr(GetCommandLineW(), L"--no-fg-pace")) { g_fg_pace = false; }
    // --fg-cad-rt 0|1: see fg_render_time_ms(). Default 1 (batch-derived frameRenderTime).
    if (const wchar_t* cr = wcsstr(GetCommandLineW(), L"--fg-cad-rt ")) {
        g_fg_cad_rt = (_wtoi(cr + 12) != 0);
    }
    // --fg-out-hz N: cap the output frame rate (only with --interp-cadence). 0 = panel.
    if (const wchar_t* oh = wcsstr(GetCommandLineW(), L"--fg-out-hz ")) {
        const int n = _wtoi(oh + 12);
        if (n >= 5 && n <= 1000) { g_fg_out_hz = n; }
    }
    // --interp-auto [floor|round]: pick the interpolation count from the measured
    // input cadence so the output stays on whole vblanks (dynamic frame times).
    if (wcsstr(GetCommandLineW(), L"--interp-auto")) {
        g_interp_auto = true;
        if (const wchar_t* ia = wcsstr(GetCommandLineW(), L"--interp-auto ")) {
            const wchar_t* p = ia + 14;
            while (*p == L' ') { ++p; }
            g_interp_auto_round = (wcsncmp(p, L"round", 5) == 0);
        }
    }
    // --interp-cadence: variable-rate generation - pick the multiplier per real frame
    // from the elapsed vblanks so the output keeps one frame per vblank (no lock, no VRR).
    if (wcsstr(GetCommandLineW(), L"--interp-cadence")) { g_interp_cadence = true; }
    // --cad-policy optimal|even|fill: 动态逐帧的取值策略。
    //   optimal = (m,k,paced) 代价最小（默认，见 src/cadence_core.h）
    //   even    = 恒定 k 优先（旧 governor 语义）
    //   fill    = 逐帧相位补满（仅当切换成本实测为 0 时更好）
    if (const wchar_t* cp = wcsstr(GetCommandLineW(), L"--cad-policy ")) {
        const wchar_t* v = cp + 13;
        if (wcsncmp(v, L"even", 4) == 0)      { g_cad_policy = 1; }
        else if (wcsncmp(v, L"fill", 4) == 0) { g_cad_policy = 2; }
        else if (wcsncmp(v, L"locked", 6) == 0) { g_cad_policy = 3; }
        else                                  { g_cad_policy = 0; }

    }
    // --cad-latency-budget N: 机会式输入量化的最大等待（ms，默认 2）。
    if (const wchar_t* lb = wcsstr(GetCommandLineW(), L"--cad-latency-budget ")) {
        const double v = _wtof(lb + 21);
        if (v >= 0.0 && v <= 50.0) { g_cad_latency_budget_ms = v; }
    }
    // --cad-epoch N: 秒级动态倍率（毫秒）。epoch 内禁止换 k；需 --interp-cadence。
    if (const wchar_t* ce = wcsstr(GetCommandLineW(), L"--cad-epoch ")) {
        const double v = _wtof(ce + 12);
        if (v >= 200.0 && v <= 10000.0) {
            g_cad_epoch_ms = v;
            g_interp_cadence = true;
        }
    }
    // --cad-round 0|1|2: floor / nearest / ceil（epoch 模式的倍率取整）。
    if (const wchar_t* cr = wcsstr(GetCommandLineW(), L"--cad-round ")) {
        const int n = _wtoi(cr + 12);
        if (n >= 0 && n <= 2) { g_cad_rounding = n; }
    }
    // --cad-allow-pacing off: 不主动量化输入（最低延迟；接受残差）。
    if (wcsstr(GetCommandLineW(), L"--cad-allow-pacing off")) { g_cad_allow_pacing = false; }
    // --cad-cost-cap N: FG 卡单张输出的实测成本（ms）；>0 时把它作为第二上限。
    if (const wchar_t* ccap = wcsstr(GetCommandLineW(), L"--cad-cost-cap ")) {
        const double v = _wtof(ccap + 15);
        if (v >= 0.0 && v <= 100.0) { g_cad_cost_cap_ms = v; }
    }
    // --cad-switch-loss measure|auto: 每次计数切换实测丢几张（expected 与
    // GetLastPresentCount 对账）。默认 measure = 只写日志；auto = 让控制器按
    // 实测结果增减驻留（并在 fill 明显亏本时退回 even）。真机确认前不要 auto。
    if (const wchar_t* csl = wcsstr(GetCommandLineW(), L"--cad-switch-loss ")) {
        const wchar_t* v = csl + 18;
        g_cad_switch_loss_auto = (wcsncmp(v, L"auto", 4) == 0);
    }
    // --cad-input producer|arrival: 动态逐帧用哪个时钟判断输入速率。
    //   producer（默认）= 游戏自己的 present 间隔，不受我们捕获/拷贝停顿影响；
    //   arrival        = WGC 到达间隔（旧行为，暂停/丢帧会污染估计）。
    if (const wchar_t* ci = wcsstr(GetCommandLineW(), L"--cad-input ")) {
        const wchar_t* v = ci + 12;
        g_cad_input_producer = (wcsncmp(v, L"arrival", 7) != 0);
    }
    // --cad-budget input|vblank: 每批预算 = 实测输入间隔 T（默认）或 k×vblank。
    //   vblank 让输出间距严格 1 vblank（节奏误差 0），代价是非整除输入下丢多余的输入帧。
    if (const wchar_t* cb = wcsstr(GetCommandLineW(), L"--cad-budget ")) {
        const wchar_t* v = cb + 13;
        g_cad_budget_vblank = (wcsncmp(v, L"vblank", 6) == 0);
    }

    // --out-clock [base_fps]: M6 固定倍率输出时钟。
    //   QPC 定时、queue=2、coast<=2、固定 k；不逐帧改 k。默认关。
    //   例：--interp 2 --out-clock 40  -> P=25ms, 3x, 120Hz 输出。
    if (const wchar_t* oc = wcsstr(GetCommandLineW(), L"--out-clock")) {
        g_out_clock = true;
        const wchar_t* p = oc + 11;
        while (*p == L' ' || *p == L'=') { ++p; }
        if (*p >= L'0' && *p <= L'9') {
            const double v = _wtof(p);
            (void)v;
        }
        // A fixed output clock is incompatible with per-frame k decisions.
        g_interp_cadence = false;
    }
    // --dyn-mfg: OptiScaler-style Dynamic MFG (auto 1x..6x to hit a presented-fps target).
    if (wcsstr(GetCommandLineW(), L"--dyn-mfg")) { g_dyn_mfg = true; }
    if (const wchar_t* ft = wcsstr(GetCommandLineW(), L"--fg-target ")) {
        const double v = _wtof(ft + 12);
        if (v >= 10.0 && v <= 1000.0) { g_dyn_target = v; }
    }
    if (const wchar_t* dh = wcsstr(GetCommandLineW(), L"--fg-round ")) {
        const int n = _wtoi(dh + 11);
        if (n >= 0 && n <= 2) { g_dyn_rounding = n; }   // 0 at most, 1 nearest, 2 at least
    }
    if (wcsstr(GetCommandLineW(), L"--fg-allow-1x off")) { g_dyn_allow1x = false; }
    // --lock N: enforce the input rate at N fps by dropping captured frames.
    if (const wchar_t* lk = wcsstr(GetCommandLineW(), L"--lock ")) {
        const double v = _wtof(lk + 7);
        if (v >= 5.0 && v <= 1000.0) { g_lock_fps = v; }
    }
    // --lock-auto: solve (rate, multiplier) so that rate*(interp+1) divides the refresh.
    if (wcsstr(GetCommandLineW(), L"--lock-auto")) { g_lock_auto = true; }
    // Echo the cadence knobs before anything can fail, so the log always says which
    // cadence model ran (both default to the old behaviour and stay silent).
    if (g_fg_rt_mode != kFgRtEma) {
        const char* what = (g_fg_rt_mode == kFgRtQuant) ? "median, snapped to whole vblanks"
                          : (g_fg_rt_mode == kFgRtRaw)  ? "the raw last input interval"
                                                        : "median of the last 16 input intervals";
        logf_("[consumer] --fg-rt-mode %s: frameRenderTime = %s\n", fg_rt_mode_name(), what);
    }
    if (g_interp_auto) {
        logf_("[consumer] --interp-auto (%s): the interpolation count follows the measured"
              " input cadence so the output stays on whole vblanks\n",
              g_interp_auto_round ? "round" : "floor");
    }
    // Cadence + motion vectors are not a good pair on this DLL, and the two are easy to
    // combine by accident (the UI's MV dropdown sits right above the mode split).
    // Per FINAL_CONFIG 5.17 a 3x/4x count is "the 2x model run twice", and the second
    // generated frame reprojects with the same vectors for a different time fraction - the
    // result is self-inconsistent and shows as flicker/ghosting. Cadence makes it worse:
    // it changes the count *between* batches, so the fractions change per frame and XeFG's
    // per-count state is re-entered 20-30 times a second (measured switches/window: 29 raw,
    // 12 with --cadence-smooth). A constant count (fixed mode) does not do this - users
    // report MV fine there, cadence + MV not smooth at all.
    if (g_interp_cadence && (!g_no_mv || g_fg_of)) {
        if (g_fg_of) {
            logf_("[consumer] NOTE: --interp-cadence with --fg-of. Optical flow is one more full"
                  " GPU pass on the same card that runs the generation, and cadence already"
                  " keeps it at 84-90%% load - measured 2026-11-09: drops become clearly more"
                  " frequent with it and almost disappear without it. Prefer --no-mv (smoothest)"
                  " or the game's vectors (cheaper, but flickers at 3x/4x - FINAL_CONFIG 5.17)\n");
        } else {
            logf_("[consumer] NOTE: --interp-cadence with the game's motion vectors. This DLL's"
                  " 3x/4x path reuses one MV set for several time fractions and cadence changes"
                  " the count between batches, so expect flicker/ghosting that --no-mv does not"
                  " show. --no-mv is the smoothest of the three (FINAL_CONFIG 5.17.1);"
                  " --cadence-hold (default 12) limits how often the count changes\n");
        }
    }
    if (g_lock_auto) {
        logf_("[consumer] --lock-auto: solving (input lock, multiplier) so that"
              " input*(interp+1) divides the refresh - no RTSS, no VRR needed\n");
    }
    if (g_dyn_mfg) {
        static const char* roundings[] = { "at most", "nearest", "at least" };
        logf_("[consumer] --dyn-mfg: OptiScaler's Dynamic MFG - target %s, rounding %s,"
              " 1X %s; the count follows the measured frame time and the learned cost of"
              " a generated frame\n",
              (g_dyn_target > 0.0) ? "from --fg-target" : "just under the refresh",
              roundings[(g_dyn_rounding < 0 || g_dyn_rounding > 2) ? 1 : g_dyn_rounding],
              g_dyn_allow1x ? "allowed" : "not allowed");
    }
    if (g_lock_fps > 1.0) {
        logf_("[consumer] --lock %.2f fps: captured frames that arrive early are dropped so"
              " the forwarded input rate is exactly this\n", g_lock_fps);
    }
    if (g_interp_cadence) {
        logf_("[consumer] --interp-cadence: variable-rate generation - the multiplier follows"
              " the vblanks between real frames (output runs at the panel refresh, no lock"
              " needed; the refresh is detected during FG init)\n");
        logf_("[consumer]   cadence core: policy=%s, latency budget %.1f ms, input pacing %s;"
              " MFG assumes a FIXED refresh (disable VRR/Adaptive-Sync)\n",
              (g_cad_policy == 1) ? "even" : ((g_cad_policy == 2) ? "fill" : "optimal"),
              g_cad_latency_budget_ms, g_cad_allow_pacing ? "on" : "off");
        // The 2026-11-07 "cadence is worse" result (FINAL_CONFIG 9.18) was retracted: it
        // was the arrival-interval bug (GetTickCount64 truncated the 0 ms samples, so the
        // median read 31 ms instead of 25 ms and the count snapped to 4). With the WGC
        // SystemRelativeTime timestamp cadence measured 1.0-1.5% empty vblanks.
        if (g_cad_source == kCadSourceTick) {
            logf_("[consumer]   cadence: source arrival-tick (k = vblank difference),"
                  " frameRenderTime = k*vblank\n");
        } else if (g_cad_source == kCadSourceGovernor) {
            logf_("[consumer]   cadence: source governor (fixed integer k per epoch,"
                  " output floor 96%% of target), frameRenderTime %s\n",
                  g_fg_cad_rt ? "batch*vblank" : "median");
        } else {
            logf_("[consumer]   cadence: source mean+phase (EMA %d frame(s), hold %d),"
                  " frameRenderTime %s\n",
                  g_cad_smooth, g_cad_hold_frames,
                  g_fg_cad_rt ? "batch*vblank" : "median");
        }
    }
#if MGPU_FG_OF
    if (wcsstr(GetCommandLineW(), L"--fg-of")) { g_fg_of = true; }
    // Any explicit MV switch means manual control; otherwise the vector source is decided
    // per frame from the producer's record (the UI no longer offers the choice).
    if (wcsstr(GetCommandLineW(), L"--no-mv") ||
        wcsstr(GetCommandLineW(), L"--fg-of") ||
        wcsstr(GetCommandLineW(), L"--mv-shadow") ||
        wcsstr(GetCommandLineW(), L"--mv-flip") ||
        wcsstr(GetCommandLineW(), L"--mvscale")) {
        g_mv_auto = false;
    }
    if (wcsstr(GetCommandLineW(), L"--game-mv")) { g_game_mv_forced = true; }
    if (wcsstr(GetCommandLineW(), L"--vrr")) { g_vrr = true; }
    if (wcsstr(GetCommandLineW(), L"--cad-batch-rt")) { g_cad_batch_rt = true; }
    if (wcsstr(GetCommandLineW(), L"--no-of-fallback")) { g_of_fallback = false; }
    if (wcsstr(GetCommandLineW(), L"--of-miss-fallback")) { g_of_miss_fallback = true; }
    if (const wchar_t* fc = wcsstr(GetCommandLineW(), L"--fg-colour ")) {
        const wchar_t* p = fc + 12;
        while (*p == L' ') { ++p; }
        if (wcsncmp(p, L"shared", 6) == 0) {
            g_fg_colour_shared = true;      // v22 default; kept for old .bat files
        }
    }
    // v22: --fg-colour wgc no longer exists. The colour always comes from stream 0; the
    // WGC capture path was deleted with the old pairing machinery.
    g_fg_colour_shared = true;
    if (const wchar_t* os_ = wcsstr(GetCommandLineW(), L"--fg-of-sign ")) {
        const double v = _wtof(os_ + 13);
        if (v == 1.0 || v == -1.0) { g_of_sign = static_cast<float>(v); }
    }
    if (const wchar_t* osc = wcsstr(GetCommandLineW(), L"--fg-of-scale ")) {
        const double v = _wtof(osc + 14);
        if (v > 0.0 && v < 1000.0) { g_of_scale = static_cast<float>(v); }
    }
    if (const wchar_t* ou = wcsstr(GetCommandLineW(), L"--fg-of-units ")) {
        const wchar_t* p = ou + 14;
        while (*p == L' ') { ++p; }
        if (wcsncmp(p, L"mvpx", 4) == 0) { g_of_units = 3; }
        else if (wcsncmp(p, L"uv", 2) == 0) { g_of_units = 1; }
        else if (wcsncmp(p, L"ndc", 3) == 0) { g_of_units = 2; }
        else { g_of_units = 0; }             // px
    }
    if (const wchar_t* oc = wcsstr(GetCommandLineW(), L"--fg-of-const ")) {
        double a = 0.0, b = 0.0;
        if (swscanf_s(oc + 14, L"%lf %lf", &a, &b) == 2) {
            g_of_const_x = static_cast<float>(a);
            g_of_const_y = static_cast<float>(b);
            g_of_const_on = true;
        }
    }
    if (const wchar_t* dk = wcsstr(GetCommandLineW(), L"--fg-of-depth-k ")) {
        const double v = _wtof(dk + 15);
        if (v >= 0.0 && v <= 100000.0) { g_of_depth_k = static_cast<float>(v); }
    }
    if (const wchar_t* od = wcsstr(GetCommandLineW(), L"--fg-of-deadzone ")) {
        const double v = _wtof(od + 17);
        if (v >= 0.0 && v <= 64.0) { g_of_deadzone = static_cast<float>(v); }
    }
    if (const wchar_t* og = wcsstr(GetCommandLineW(), L"--fg-of-guide ")) {
        const double v = _wtof(og + 14);
        if (v >= 0.0 && v < 10000.0) { g_of_guide = static_cast<float>(v); }
    }
#else
    // The flow pass is compiled out - say so instead of silently ignoring the switch
    // (a silent no-op is exactly how a dead input wastes a whole experiment round).
    if (wcsstr(GetCommandLineW(), L"--fg-of")) {
        logf_("[consumer] --fg-of ignored: not compiled in (MV is a dead input for"
              " libxess_fg 1.3.1; rebuild with /DMGPU_FG_OF=1 to re-test)\n");
    }
#endif
    if (const wchar_t* dp = wcsstr(GetCommandLineW(), L"--dump ")) {
        const int n = _wtoi(dp + 7);
        if (n > 0 && n <= 64) { g_dump_frames = n; }
    }
    if (const wchar_t* da = wcsstr(GetCommandLineW(), L"--dump-after ")) {
        const int n = _wtoi(da + 13);
        if (n >= 0 && n <= 100000) { g_dump_after = n; }
    }
    if (const wchar_t* dd = wcsstr(GetCommandLineW(), L"--dump-dir ")) {
        const wchar_t* p = dd + 11;
        while (*p == L' ' || *p == L'"') { ++p; }
        const wchar_t* end = p;
        const bool quoted = (*(dd + 11) == L'"');
        while (*end && ((quoted && *end != L'"') || (!quoted && *end != L' '))) { ++end; }
        if (end > p) {
            const int len = WideCharToMultiByte(CP_ACP, 0, p, static_cast<int>(end - p),
                                                g_dump_dir, MAX_PATH - 1, nullptr, nullptr);
            if (len > 0) { g_dump_dir[len] = '\0'; }
        }
    }
    if (g_dump_frames > 0) {
        CreateDirectoryA(g_dump_dir, nullptr);
        logf_("[consumer] --dump %d frame(s) to %s (colour/depth/MV raw + meta)\n",
              g_dump_frames, g_dump_dir);
        logf_("[consumer]   run with real inputs (no --no-mv/--no-depth) or the dump"
              " cannot answer the pairing question\n");
    }
    if (wcsstr(GetCommandLineW(), L"--no-throttle")) { g_no_throttle = true; }
    // --input-copy-queue: MV/depth copies on their own COPY queue so they run on the copy
    // engine at the same time as the colour convert draw (see the block next to
    // run_frame_ex()). Off by default: it is the first structural change to the submit
    // order, and its failure mode (XeFG refusing frames because the tags lost their
    // command list) has to be seen in an A/B run before it can be a default.
    if (wcsstr(GetCommandLineW(), L"--input-copy-queue")) {
        g_input_copy_queue = true;
        logf_("[consumer] --input-copy-queue: MV/depth copies will use a COPY queue\n");
    }
    // --prefetch-inputs: copy the *next* frame's MV/depth during this frame's Present.
    // Implies --input-copy-queue (it needs the second queue to get the copies off the frame
    // in the first place). Off by default: it changes which frame each slot is read for.
    if (wcsstr(GetCommandLineW(), L"--prefetch-inputs")) {
        g_prefetch_inputs = true;
        g_input_copy_queue = true;
        logf_("[consumer] --prefetch-inputs: MV/depth are copied one frame ahead (implies"
              " --input-copy-queue; this is the default)\n");
    }
    if (wcsstr(GetCommandLineW(), L"--no-prefetch-inputs")) {
        g_prefetch_inputs = false;
        g_pf_pub = false;
        g_pub_fifo = false;
        g_input_copy_queue = false;
        logf_("[consumer] --no-prefetch-inputs: copies run after WGC again (old order)\n");
    }
    if (wcsstr(GetCommandLineW(), L"--no-prefetch-publications")) {
        g_pf_pub = false;
        g_pub_fifo = false;
        logf_("[consumer] --no-prefetch-publications: one-slot-ahead prefetch instead of the"
              " publication cache\n");
    } else if (wcsstr(GetCommandLineW(), L"--prefetch-publications")) {
        g_pf_pub = true;
        logf_("[consumer] --prefetch-publications: every new publication is copied into the"
              " local cache\n");
    }
    if (wcsstr(GetCommandLineW(), L"--no-pub-fifo")) {
        g_pub_fifo = false;
        logf_("[consumer] --no-pub-fifo: publication cache is matched by publication value"
              " instead of queue order\n");
    } else if (wcsstr(GetCommandLineW(), L"--pub-fifo")) {
        g_pub_fifo = true;
        g_pf_pub = true;
        logf_("[consumer] --pub-fifo: copied publications are consumed in FIFO order (no slot"
              " key, no miss); queue holds at most %u, a reused slot drops its stale entry\n",
              mgpu::kMaxSlots);
    }
    logf_("[consumer] input path: copy-on-publication=%d fifo=%d rotating-prefetch=%d\n",
          g_pf_pub ? 1 : 0, g_pub_fifo ? 1 : 0, g_prefetch_inputs ? 1 : 0);
#if MGPU_FG_OF
    // Prefetch misses use the flow instead of a fallback copy. The FFX context keeps its
    // own history, so it must be fed every frame; the output is only tagged on a miss.
    // Opt-in: at 4K the flow costs ~2-3 ms on the render card, which alone exceeds the
    // 25 ms budget on this chain. The default miss path is the in-frame copy.
    if (g_of_miss_fallback && g_prefetch_inputs && g_of_fallback && !g_no_mv && !g_no_inputs) {
        g_of_track = true;
        logf_("[consumer] of: tracking every frame for prefetch misses (no copy on miss,"
              " opt-in; expect ~2-3 ms at 4K)\n");
    }
#endif
    // --stream-auto: the consumer picks MV/depth StreamPercent per report window from the
    // measured WGC hop (p10) + the copy model, and writes it through protocol v15.
    // --stream-asym: MV stays 1:1, depth is scaled (ini value, or the auto choice). It
    // implies --upsample because XeFG needs the tagged MV/depth pair to be the same size.
    if (wcsstr(GetCommandLineW(), L"--stream-auto")) {
        g_stream_auto = true;
        logf_("[consumer] --stream-auto: per-stream StreamPercent is chosen from the measured"
              " WGC age (p10) and the copy model, one step per window\n");
    }
    if (wcsstr(GetCommandLineW(), L"--stream-asym")) {
        g_stream_asym = true;
        logf_("[consumer] --stream-asym: MV stays 1:1, depth is scaled; --upsample is implied"
              " (XeFG needs one size for both)\n");
    }
    // --upsample: rebuild MV+depth to the game's render size on this GPU after the
    // cross-adapter copy. Off by default (A/B): it costs one compute pass and changes the
    // tagged resources, so it has to be seen in a real run before it can be a default.
    // docs/design/UPSAMPLE_QUALITY.md. It records into the frame's own list, so the two
    // copy-queue experiments are turned off when it is enabled.
    if (wcsstr(GetCommandLineW(), L"--upsample") || g_stream_asym) {
        g_upsample = true;
        if (const wchar_t* uk = wcsstr(GetCommandLineW(), L"--upsample-k ")) {
            const double v = _wtof(uk + 13);
            if (v > 0.0 && v < 1.0e6) { g_upsample_k = static_cast<float>(v); }
        }
        logf_("[consumer] --upsample: ON - MV/depth are rebuilt from the stream size to the"
              " game's render size on this GPU (depth K=%.0f)\n", g_upsample_k);
    }
    if (const wchar_t* wm = wcsstr(GetCommandLineW(), L"--wait-ms ")) {
        const int n = _wtoi(wm + 10);
        if (n >= 0 && n <= 200) { g_wait_ms = static_cast<DWORD>(n); }
    }
    if (const wchar_t* im = wcsstr(GetCommandLineW(), L"--input-ms ")) {
        const double v = _wtof(im + 11);
        if (v >= 1.0 && v <= 200.0) {
            g_input_ms_override = v;
            logf_("[consumer] --input-ms %.2f: the pacing model treats the input interval as"
                  " %.2f ms (measured cadence is still reported as game=. This does not"
                  " create GPU time - it only changes the slow/load accounting and what"
                  " frameRenderTime XeFG sees)\n", v, v);
        }
    }
    if (const wchar_t* rp = wcsstr(GetCommandLineW(), L"--report ")) {
        const int n = _wtoi(rp + 9);
        if (n >= 5 && n <= 400) { g_report_every = static_cast<UINT>(n); }
    }
    if (const wchar_t* pc = wcsstr(GetCommandLineW(), L"--pace ")) {
        if (wcsncmp(pc + 7, L"auto", 4) == 0) {
            // Follow the source: pacing below the real arrival rate throws frames away,
            // which is worse than no pacing at all.
            g_pace_auto = true;
            g_pace_ms = 1.0;
        } else {
            const int hz = _wtoi(pc + 7);
            if (hz >= 10 && hz <= 240) { g_pace_ms = 1000.0 / static_cast<double>(hz); }
        }
    }
    if (const wchar_t* lm = wcsstr(GetCommandLineW(), L"--latency ")) {
        const int n = _wtoi(lm + 10);
        if (n >= 1 && n <= 4) { g_max_latency = static_cast<UINT>(n); }
    }
    if (const wchar_t* xm = wcsstr(GetCommandLineW(), L"--xell-mode ")) {
        const int n = _wtoi(xm + 12);
        if (n >= 0 && n <= 1) { g_xell_mode = n; }
    }
    if (const wchar_t* bu = wcsstr(GetCommandLineW(), L"--buffers ")) {
        const int n = _wtoi(bu + 10);
        if (n >= 2 && n <= 4) { g_swap_buffers = static_cast<UINT>(n); }
    }
    if (wcsstr(GetCommandLineW(), L"--sdr")) { g_colour_mode = 2; }
    if (wcsstr(GetCommandLineW(), L"--no-primaries")) { g_colour_mode = 1; }
    if (wcsstr(GetCommandLineW(), L"--half")) { g_half = true; g_out_den = 2; }
    if (wcsstr(GetCommandLineW(), L"--fg")) {
        g_fg_warp = wcsstr(GetCommandLineW(), L"--fg warp") != nullptr;
    }
    if (const wchar_t* mm = wcsstr(GetCommandLineW(), L"--mv-mul ")) {
        g_mv_mul = static_cast<float>(_wtof(mm + 9));
    }
    if (wcsstr(GetCommandLineW(), L"--mv-flip")) { g_mv_sign = -1.0f; }
    if (const wchar_t* wcf = wcsstr(GetCommandLineW(), L"--warp-conf ")) {
        g_warp_conf = static_cast<float>(_wtof(wcf + 12));
        if (g_warp_conf < 0.0f) { g_warp_conf = 0.0f; }
        if (g_warp_conf > 4000.0f) { g_warp_conf = 4000.0f; }
    }
    if (const wchar_t* wsr = wcsstr(GetCommandLineW(), L"--warp-search ")) {
        g_warp_search = static_cast<float>(_wtof(wsr + 14));
        if (g_warp_search < 0.0f) { g_warp_search = 0.0f; }
        if (g_warp_search > 16.0f) { g_warp_search = 16.0f; }
    }
    if (const wchar_t* wcl = wcsstr(GetCommandLineW(), L"--warp-clamp ")) {
        g_warp_clamp = static_cast<float>(_wtof(wcl + 13));
        if (g_warp_clamp < 0.0f) { g_warp_clamp = 0.0f; }
        if (g_warp_clamp > 1.0f) { g_warp_clamp = 1.0f; }
    }
    if (g_fg_warp) {
        // Our interpolator works at the captured size: no scaling anywhere.
        g_out_den = 1;
        g_scale = 0.0f;
        g_no_fg = true;   // XeSS-FG is replaced by the warp pass
    }
    if (const wchar_t* sp = wcsstr(GetCommandLineW(), L"--sharp")) {
        g_sharpen = static_cast<float>(_wtof(sp + 7));
        if (g_sharpen < 0.0f) { g_sharpen = 0.0f; }
        if (g_sharpen > 2.0f) { g_sharpen = 2.0f; }
    }
    // --down N: kernel used by the resize that runs before frame generation.
    if (const wchar_t* dn = wcsstr(GetCommandLineW(), L"--down ")) {
        const int n = _wtoi(dn + 7);
        if (n >= 0 && n <= 3) { g_down = n; }
    }
    if (const wchar_t* sp2 = wcsstr(GetCommandLineW(), L"--scale ")) {
        g_scale = static_cast<float>(_wtof(sp2 + 8));
        if (g_scale < 0.25f) { g_scale = 0.0f; }
        if (g_scale > 1.0f)  { g_scale = 1.0f; }
    }
    if (const wchar_t* d = wcsstr(GetCommandLineW(), L"--den ")) {
        const int n = _wtoi(d + 6);
        if (n >= 1 && n <= 8) { g_out_den = n; }
    }
    // Kernel support per --down mode (in output-pixel units). support 0 keeps the
    // single bilinear tap; the shader stretches whatever support it is given.
    switch (g_down) {
        case 1: g_down_support = 2.0f; break;   // Catmull-Rom      (support 2)
        case 2: g_down_support = 2.0f; break;   // Mitchell B=C=1/3 (support 2)
        case 3: g_down_support = 3.0f; break;   // Lanczos-3        (support 3)
        default: g_down = 0; g_down_support = 0.0f; break;
    }

    logf_("[consumer] mode: fg=%s den=%d scale=%.2f buffers=%u latency=%u tearing=%d"
          " xell=%s(mode %d) inputs=%s sharp=%.2f colour=%d interp=%d unlock=%d down=%d\n",
          g_fg_warp ? "warp" : (g_no_fg ? "off" : "xess-fg"), g_out_den, g_scale,
          g_swap_buffers, g_max_latency, g_tearing ? 1 : 0,
          g_no_xell ? "no-sleep" : "on", g_xell_mode,
          g_no_inputs ? "off" : "on", g_sharpen, g_colour_mode, g_fg_interp,
          g_unlock_mfg ? 1 : 0, g_down);
    if (g_overlay_monitor >= 0 || g_overlay_hidden) {
        logf_("[consumer] overlay: monitor=%s hidden=%d (capture stays %s)\n",
              (g_overlay_monitor >= 0) ? "explicit" : "game window",
              g_overlay_hidden ? 1 : 0,
              g_capture_monitor ? "monitor" : "window");
    }

    if (g_game_hwnd != nullptr || g_game_pid != 0) {
        if (g_game_hwnd != nullptr) {
            logf_("[consumer] target window %p (--hwnd)\n", (void*)g_game_hwnd);
            if (!IsWindow(g_game_hwnd)) { g_game_hwnd = nullptr; }
        } else {
            logf_("[consumer] target process %lu (--pid)\n", (unsigned long)g_game_pid);
        }
    } else if (g_game_exe_set) {
        logf_("[consumer] game process: %ls (--game)\n", g_game_exe.c_str());
    } else {
        logf_("[consumer] no --game/--hwnd/--pid: using the foreground window\n");
    }

    {
        static const char* k_down_names[] = { "bilinear (1 tap)",
                                              "catmull-rom (support 2)",
                                              "mitchell B=C=1/3 (support 2)",
                                              "lanczos-3 (support 3)" };
        const int i = (g_down >= 0 && g_down <= 3) ? g_down : 0;
        if (g_down == 0) {
            logf_("[consumer] resize filter: %s - add --down 1 (catmull-rom) for a real"
                  " low-pass; --down 2 soft/no ringing, --down 3 sharpest\n", k_down_names[i]);
        } else {
            logf_("[consumer] resize filter: %s, taps follow the scale at draw time\n",
                  k_down_names[i]);
        }
    }

    if (g_fg_rt_zero || g_fg_ui_none || g_fg_scene > 0.0f || g_fg_depth_inv ||
        g_fg_jittered_mv_forced) {
        logf_("[consumer] XeFG tuning: rt=%s ui=%s scene=%.2f depthInv=%d jitteredMV=%d%s\n",
              g_fg_rt_zero ? "zero" : "input",
              g_fg_ui_none ? "none" : "auto",
              g_fg_scene, g_fg_depth_inv ? 1 : 0, g_fg_jittered_mv ? 1 : 0,
              g_fg_jittered_mv_forced ? (g_fg_jittered_mv ? " (forced ON)" : " (forced OFF)")
                                      : " (automatic: follows the producer's DLSS flag)");
    }
    // v22: --slack/--jbuf belonged to the WGC loop and are gone. Old .bat files that pass
    // them are ignored.

    // libxess_fg.dll is loaded here rather than through an import library, so the
    // file can be chosen (--fg-lib) and its real version and MFG capability can be
    // reported before anything is initialised.
    //
    // This happens even for --fg warp: that path replaces the interpolator but still
    // presents through XeSS-FG's proxy swap chain, so the DLL is required either way.
    {
        std::string fg_err;
        if (xefg_dyn::Load(g_api, g_fg_lib, &fg_err)) {
            logf_("[consumer] libxess_fg: %s\n", narrow_utf8(g_api.path).c_str());
            xefg_swapchain_version_t v{};
            if (g_api.GetVersion && g_api.GetVersion(&v) == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                logf_("[consumer] libxess_fg version %u.%u.%u, SetNumInterpolatedFrames=%s\n",
                      v.major, v.minor, v.patch, g_api.has_mfg() ? "yes" : "NO");
            }
        } else {
            logf_("[consumer] libxess_fg.dll NOT loaded: %s\n", fg_err.c_str());
            logf_("[consumer]   the proxy swap chain needs it even with --fg warp;"
                  " pick a build with --fg-lib <path>\n");
            if (!g_fg_probe) { return 11; }
        }
    }

    // --unlock-mfg used to be applied right here. It moved after adapter selection
    // (apply_unlock_mfg(), see main()) because the patch must NOT run on an Intel
    // adapter: there the clamp is not in the way and the native MFG model exists, so
    // patching would replace real multi-frame generation with repeated 2x frames
    // (docs/findings/MFG.md 1). The vendor is only known once the consumer adapter is picked.

    if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) {
        logf_("[consumer] RoInitialize FAILED\n");
        return 1;
    }
    enable_fine_timer();
    g_t0 = GetTickCount64() / 1000.0;

    // --fg-probe only asks the loaded DLL what it supports, so it needs neither the
    // game nor the shared memory - it can be run on its own to find out whether the
    // community MFG build is really in place. --fg-of-selftest is the same idea for
    // the optical-flow bring-up (only exists when MGPU_FG_OF=1).
#if MGPU_FG_OF
    const bool of_selftest_mode = (wcsstr(GetCommandLineW(), L"--fg-of-selftest") != nullptr);
#else
    const bool of_selftest_mode = false;
#endif
    HWND game_wnd = nullptr;
    if (!g_fg_probe && !of_selftest_mode) {
        if (g_game_hwnd != nullptr && IsWindow(g_game_hwnd)) {
            game_wnd = g_game_hwnd;
        } else if (g_game_pid != 0) {
            game_wnd = find_window_of_pid(g_game_pid);
        } else if (g_game_exe_set) {
            game_wnd = find_game_window(g_game_exe.c_str());
        } else {
            // Nothing was named: take whatever the user is looking at. This is what makes
            // a bare `xess_mfg_consumer.exe` (or a double-clicked recipe) work; the old code fell
            // back to a hard-coded Cyberpunk2077.exe, which only worked on one machine.
            game_wnd = GetForegroundWindow();
            logf_("[consumer] target: foreground window %p\n", (void*)game_wnd);
        }
        if (!game_wnd) {
            if (g_game_pid != 0) {
                logf_("[consumer] no visible window for pid %lu\n", (unsigned long)g_game_pid);
            } else if (g_game_exe_set) {
                logf_("[consumer] game window not found - is %ls running?\n", g_game_exe.c_str());
            } else {
                logf_("[consumer] no foreground window - click into the game first,"
                      " or pass --game <exe> / --hwnd <hex>\n");
            }
            return 2;
        }
        logf_("[consumer] game window %p\n", (void*)game_wnd);
        g_game_wnd = game_wnd;

        // ---- MV source policy for Unreal titles ----
        // Measured on 2026-11-10 on this machine: with the game's own motion vectors both
        // Wuchang: Fallen Feathers and Phantom Blade Zero show edge ghosting that survives
        // --fg-jittered-mv, while our optical flow on the same frames is clean (the A/B bats
        // "A - jittered MV" vs "B - optical flow"). So an Unreal shipping build (exe name
        // *-Win64-Shipping.exe) defaults to optical flow; anything else keeps the game's MV,
        // which is free and usually better (docs/ops/USAGE.md 6). Explicit --fg-of / --game-mv
        // always wins.
        if (!g_fg_of && !g_game_mv_forced) {
            DWORD gpid = 0;
            GetWindowThreadProcessId(game_wnd, &gpid);
            wchar_t image[MAX_PATH]{};
            DWORD len = MAX_PATH;
            HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, gpid);
            if (hp != nullptr) {
                if (QueryFullProcessImageNameW(hp, 0, image, &len)) {
                    const wchar_t* bn = wcsrchr(image, L'\\');
                    bn = (bn != nullptr) ? bn + 1 : image;
                    if (wcs_ends_with_ci(bn, L"-Win64-Shipping.exe")) {
                        g_fg_of = true;
                        logf_("[consumer] %ls is an Unreal shipping build - using our optical flow"
                              " for the motion vectors (its own MVs ghosted here; override with"
                              " --game-mv)\n", bn);
                    }
                }
                CloseHandle(hp);
            }
        }

        {
            // If the game window itself is topmost, that explains why it covered us.
            const LONG_PTR ex = GetWindowLongPtrW(g_game_wnd, GWL_EXSTYLE);
            RECT gr{};
            GetWindowRect(g_game_wnd, &gr);
            logf_("[consumer] game window style=0x%08lX ex=0x%08lX topmost=%d rect=(%ld,%ld)-(%ld,%ld)%s\n",
                  static_cast<unsigned long>(GetWindowLongPtrW(g_game_wnd, GWL_STYLE)),
                  static_cast<unsigned long>(ex), (ex & WS_EX_TOPMOST) ? 1 : 0,
                  gr.left, gr.top, gr.right, gr.bottom,
                  (ex & WS_EX_TOPMOST)
                      ? "   (already topmost: possibly left over from an earlier run,"
                        " cleared on exit)"
                      : "");
        }
    } else {
        logf_("[consumer] --fg-probe: no game window needed\n");
    }

    // Protocol v6: if the add-on is already running it publishes the adapter it
    // renders on, so "pick the other GPU" works without --producer-luid. A command
    // line --producer-luid still wins (peek only runs when it was not given).
    if (!g_have_producer && !g_fg_probe) { peek_producer(); }

    // Which GPU runs XeSS-FG. Not "the AMD card" any more: XeFG is cross-vendor and
    // the only hard constraint is that the adapter drives the monitor the overlay
    // sits on (docs/design/VENDOR_COMPAT_PLAN.md M6.1). --gpu forces one; --producer-luid
    // says which card to avoid.
    const AdapterPick pick = find_consumer_adapter(game_wnd);
    if (!pick.adapter) {
        logf_("[consumer] no usable consumer adapter (only software adapters, or --gpu"
              " matched nothing)\n");
        logf_("[consumer]   choose one explicitly: --gpu <index|nv|amd|intel|"
              "luid:HI:LO|name substring>\n");
        return 3;
    }
    g_consumer_desc   = pick.desc;
    g_consumer_vendor = pick.desc.VendorId;
    g_consumer_adapter = pick.adapter;

    if (!init_devices(pick.adapter.Get())) { return 4; }

    // The vendor is known now, so the MFG patch can run - or deliberately not run on
    // an Intel adapter, where the native MFG model exists (docs/findings/MFG.md 1).
    if (g_unlock_mfg && !apply_unlock_mfg() && !g_fg_probe) { return 13; }

    if (g_fg_probe) { return fg_probe() ? 0 : 12; }
    // --fg-of-selftest: exercise the optical-flow bring-up without the game, so a
    // failure in ffxGetInterfaceDX12/ContextCreate can be diagnosed in one run.
#if MGPU_FG_OF
    if (wcsstr(GetCommandLineW(), L"--fg-of-selftest")) {
        const bool ok = of_selftest();
        logf_("[consumer] --fg-of-selftest: %s (%ux%u field)\n", ok ? "OK" : "FAILED", g_of_w, g_of_h);
        return ok ? 0 : 13;
    }
#else
    if (wcsstr(GetCommandLineW(), L"--fg-of-selftest")) {
        logf_("[consumer] --fg-of-selftest: not compiled in (MGPU_FG_OF=0)\n");
        return 13;
    }
#endif
    if (!open_shared()) { return 5; }
    // v22: announce the request board before waiting for the first publication. The
    // producer publishes colour-only when the mask is empty, so this is not strictly
    // required to break the deadlock, but it makes MV/depth available from frame 1.
    if (g_ctl != nullptr) {
        g_ctl->request_mask    = mgpu::kRequestColor | mgpu::kRequestMv | mgpu::kRequestDepth;
        g_ctl->consumer_request = mgpu::kRequestMv;
        MemoryBarrier();
    }

    // v22: the colour stream defines the frame size. Its live rectangle is the frame's
    // OUTPUT size (the slot capacity is what the producer placed); fall back to the
    // producer's declared output/render size until the first publication.
    if (g_ctl->streams[mgpu::kStreamColour].width >= 256 &&
        g_ctl->streams[mgpu::kStreamColour].height >= 256) {
        g_width  = g_ctl->streams[mgpu::kStreamColour].width;
        g_height = g_ctl->streams[mgpu::kStreamColour].height;
    } else if (g_producer_output_w >= 256 && g_producer_output_h >= 256) {
        g_width  = g_producer_output_w;
        g_height = g_producer_output_h;
    } else if (g_producer_render_w >= 256 && g_producer_render_h >= 256) {
        g_width  = g_producer_render_w;
        g_height = g_producer_render_h;
    }
    logf_("[consumer] colour transport: frame %ux%u from the producer's stream 0"
          " (WGC removed)\n", g_width, g_height);
    if (!create_colour_sink(g_width, g_height)) { return 6; }
    if (!init_convert()) { return 9; }
    if (g_pace_ms > 1.0 && !g_fg_warp) {
        logf_("[consumer] note: --pace is ignored by the publication-driven loop\n");
    }
    if (!init_fg()) { return 8; }
    if (g_fg_warp && (!init_warp() || !init_warp_pso())) { return 10; }

    logf_("\n[consumer] running - close the window or press Ctrl+C to stop\n\n");
    logf_("[consumer] startup: +%llu ms running (colour transport + XeFG + overlay ready)\n",
          startup_ms());

    // ---------------------------------------------------------------- main loop
    // One publication = one frame. Pick the OLDEST unprocessed publication (not the
    // newest): when we fall behind, skipping in publication order is what keeps the
    // cadence controller seeing real per-presentation input. wait_slot() is the synchronisation
    // point - by the time run_frame() reads the slot, colour, MV and depth are all done.
    // ---- hotkeys (v22: kept, they only change the stream switch board / exit) ----
    constexpr int kEscapeHotkeyId = 0x4D47;   // 'MG'
    constexpr int kStreamHkBase   = 0x4D50;
    const uint32_t hk_bits[5] = { mgpu::kRequestMv, mgpu::kRequestDepth, mgpu::kRequestUi,
                                  mgpu::kRequestColor, mgpu::kRequestRawMv };
    bool stream_hk[6] = {};
    stream_hk[0] = RegisterHotKey(nullptr, kStreamHkBase + 0, MOD_CONTROL | MOD_ALT, '1') != FALSE;
    stream_hk[1] = RegisterHotKey(nullptr, kStreamHkBase + 1, MOD_CONTROL | MOD_ALT, '2') != FALSE;
    stream_hk[2] = RegisterHotKey(nullptr, kStreamHkBase + 2, MOD_CONTROL | MOD_ALT, '3') != FALSE;
    stream_hk[3] = RegisterHotKey(nullptr, kStreamHkBase + 3, MOD_CONTROL | MOD_ALT, '4') != FALSE;
    stream_hk[4] = RegisterHotKey(nullptr, kStreamHkBase + 4, MOD_CONTROL | MOD_ALT, '5') != FALSE;
    stream_hk[5] = RegisterHotKey(nullptr, kStreamHkBase + 5, MOD_CONTROL | MOD_ALT, '0') != FALSE;
    logf_("[consumer] hotkeys: Ctrl+Alt+1 mv, +2 depth, +3 ui, +4 color, +5 rawmv, +0 all"
          " (reg %d%d%d%d%d%d)\n", stream_hk[0], stream_hk[1], stream_hk[2], stream_hk[3],
          stream_hk[4], stream_hk[5]);
    auto handle_bridge_hotkey = [&](int id) -> bool {
        if (id == kStreamHkBase + 5) {
            const bool idle = (g_stream_mask & (mgpu::kRequestMv | mgpu::kRequestDepth)) == 0;
            apply_stream_mask(idle ? (mgpu::kRequestMv | mgpu::kRequestDepth | mgpu::kRequestMeta)
                                   : mgpu::kRequestMeta,
                              idle ? "hotkey: on" : "hotkey: off");
            return true;
        }
        for (int i = 0; i < 5; ++i) {
            if (id == kStreamHkBase + i) {
                apply_stream_mask(g_stream_mask ^ hk_bits[i], "hotkey");
                return true;
            }
        }
        return false;
    };
    const bool escape_hotkey =
        RegisterHotKey(nullptr, kEscapeHotkeyId, MOD_CONTROL | MOD_ALT, VK_END) != FALSE;
    logf_("[consumer] escape hotkey Ctrl+Alt+End registered=%d\n",
          static_cast<int>(escape_hotkey));

    UINT64 last_pub = 0;
    bool   overlay_hidden = false;
    // v22 diagnostics: where the main loop spends the gap that `total` does not cover.
    double t_assert_ms = 0.0, t_focus_ms = 0.0, t_scan_ms = 0.0, t_wait_ms = 0.0, t_msg_ms = 0.0;
    uint64_t loop_iters = 0, loop_none = 0, loop_frames = 0, loop_last_frames = g_frames;
    uint64_t loop_last_iters = 0;
    // ---- v23.3: start at the newest publication --------------------------------
    // XeFG init takes ~3 s while the producer is already publishing. Starting the loop at
    // pub_seq 0 replayed that whole backlog (measured: first logged pick already at 127,
    // 0.3-7.7 fps for the first windows, and the screen showed old/dirty frames until it
    // caught up). Fast-forward to the newest publication minus one, so the first frame has
    // a valid previous frame for FG history but no stale backlog.
    if (g_ctl != nullptr) {
        const UINT slots0 = (g_ctl->slots != 0) ? g_ctl->slots : 1;
        uint64_t newest = 0;
        for (UINT s = 0; s < slots0 && s < mgpu::kMaxSlots; ++s) {
            if (g_ctl->frames[s].pub_seq > newest) { newest = g_ctl->frames[s].pub_seq; }
        }
        if (newest > 1) {
            const uint64_t start = newest - 1;
            logf_("[consumer] startup: newest publication=%llu, starting at %llu"
                  " (dropped %llu stale)\n",
                  static_cast<unsigned long long>(newest),
                  static_cast<unsigned long long>(start),
                  static_cast<unsigned long long>(start));
            last_pub = start;
        }
    }

    for (;;) {
        ++loop_iters;
        MSG msg{};
        {
            const double tm = qpc_ms();
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { hard_exit("WM_QUIT (launcher request)"); }
            if (msg.message == WM_HOTKEY && static_cast<int>(msg.wParam) == kEscapeHotkeyId) {
                UnregisterHotKey(nullptr, kEscapeHotkeyId);
                hard_exit("escape hotkey Ctrl+Alt+End");
            }
            if (msg.message == WM_HOTKEY &&
                handle_bridge_hotkey(static_cast<int>(msg.wParam))) { continue; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            }
            t_msg_ms += qpc_ms() - tm;
        }

        if (!g_ctl || g_ctl->magic != mgpu::kSharedMagic) {
            Sleep(1);
            keep_game_focus();
            continue;
        }

        const double ts0 = qpc_ms();
        const UINT slots = (g_ctl->slots != 0) ? g_ctl->slots : 1;
        UINT   pick = mgpu::kMaxSlots;
        UINT64 best = UINT64_MAX;
        for (UINT s = 0; s < slots && s < mgpu::kMaxSlots; ++s) {
            const uint64_t seq = g_ctl->frames[s].pub_seq;
            if (seq > last_pub && seq < best) { best = seq; pick = s; }
        }
        t_scan_ms += qpc_ms() - ts0;

        if (pick >= mgpu::kMaxSlots) {
            ++loop_none;
            static uint64_t s_spin = 0;
            if ((++s_spin % 300u) == 0u) {
                logf_("[consumer] spin: last=%llu seq=[%llu %llu %llu %llu] fence=[%llu %llu %llu %llu]"
                      " global=%llu none=%llu\n",
                      static_cast<unsigned long long>(last_pub),
                      static_cast<unsigned long long>(g_ctl->frames[0].pub_seq),
                      static_cast<unsigned long long>(g_ctl->frames[1].pub_seq),
                      static_cast<unsigned long long>(g_ctl->frames[2].pub_seq),
                      static_cast<unsigned long long>(g_ctl->frames[3].pub_seq),
                      static_cast<unsigned long long>(g_ctl->frames[0].published),
                      static_cast<unsigned long long>(g_ctl->frames[1].published),
                      static_cast<unsigned long long>(g_ctl->frames[2].published),
                      static_cast<unsigned long long>(g_ctl->frames[3].published),
                      static_cast<unsigned long long>(g_ctl->frame_published),
                      static_cast<unsigned long long>(s_spin));
            }
            // The title stopped publishing (loading screen, pause, a transition). Our
            // overlay would freeze the last picture over the game's own window, so hide
            // it until publications resume.
            if (!overlay_hidden && g_bridge_wnd &&
                (qpc_ms() - g_last_publish_ms) > 250.0) {
                ShowWindow(g_bridge_wnd, SW_HIDE);
                overlay_hidden = true;
                logf_("[consumer] no publications for %.0f ms - overlay hidden until the"
                      " game evaluates again\n", qpc_ms() - g_last_publish_ms);
            }
            const double tf = qpc_ms();
            keep_game_focus();
            t_focus_ms += qpc_ms() - tf;
            const double ta = qpc_ms();
            assert_topmost();
            t_assert_ms += qpc_ms() - ta;
            Sleep(1);
            continue;
        }

        if (overlay_hidden) {
            ShowWindow(g_bridge_wnd, SW_SHOWNOACTIVATE);
            overlay_hidden = false;
            // Also discard the backlog accumulated while we were hidden.
            const UINT slots0 = (g_ctl->slots != 0) ? g_ctl->slots : 1;
            uint64_t newest = 0;
            for (UINT s = 0; s < slots0 && s < mgpu::kMaxSlots; ++s) {
                if (g_ctl->frames[s].pub_seq > newest) { newest = g_ctl->frames[s].pub_seq; }
            }
            if (newest > last_pub + 1) {
                logf_("[consumer] publications resumed - fast-forward %llu -> %llu\n",
                      static_cast<unsigned long long>(last_pub),
                      static_cast<unsigned long long>(newest - 1));
                last_pub = newest - 1;
            }
            logf_("[consumer] publications resumed - overlay shown\n");
        }

        const uint64_t prev_last = last_pub;
        last_pub  = best;
        g_pub_slot = pick;
        {
            static uint64_t s_picks = 0;
            if ((++s_picks % 40u) == 0u) {
                logf_("[consumer] pick: last=%llu best=%llu slot=%u seq=[%llu %llu %llu %llu]"
                      " fence=[%llu %llu %llu %llu] global=%llu\n",
                      static_cast<unsigned long long>(prev_last),
                      static_cast<unsigned long long>(best), pick,
                      static_cast<unsigned long long>(g_ctl->frames[0].pub_seq),
                      static_cast<unsigned long long>(g_ctl->frames[1].pub_seq),
                      static_cast<unsigned long long>(g_ctl->frames[2].pub_seq),
                      static_cast<unsigned long long>(g_ctl->frames[3].pub_seq),
                      static_cast<unsigned long long>(g_ctl->frames[0].published),
                      static_cast<unsigned long long>(g_ctl->frames[1].published),
                      static_cast<unsigned long long>(g_ctl->frames[2].published),
                      static_cast<unsigned long long>(g_ctl->frames[3].published),
                      static_cast<unsigned long long>(g_ctl->frame_published));
            }
        }
        {
            const double tw = qpc_ms();
            wait_slot(pick);
            t_wait_ms += qpc_ms() - tw;
        }
        run_frame(nullptr);
        {
            const double ta = qpc_ms();
            assert_topmost();
            t_assert_ms += qpc_ms() - ta;
        }
        // v22: per-40-frame main-loop profile. `iter/frame` > 1 means the loop spun
        // without a publication; the times explain where a HITCH gap goes.
        if (g_frames - loop_last_frames >= 40) {
            const uint64_t df = g_frames - loop_last_frames;
            const uint64_t di = loop_iters - loop_last_iters;
            logf_("[consumer] loop: frames=%llu iters=%llu (%.2f/frame) none=%llu"
                  " msg=%.2fms scan=%.2fms wait=%.2fms assert=%.2fms focus=%.2fms\n",
                  static_cast<unsigned long long>(df),
                  static_cast<unsigned long long>(di),
                  static_cast<double>(di) / static_cast<double>(df ? df : 1),
                  static_cast<unsigned long long>(loop_none),
                  t_msg_ms / static_cast<double>(df ? df : 1),
                  t_scan_ms / static_cast<double>(df ? df : 1),
                  t_wait_ms / static_cast<double>(df ? df : 1),
                  t_assert_ms / static_cast<double>(df ? df : 1),
                  t_focus_ms / static_cast<double>(df ? df : 1));
            loop_last_frames = g_frames;
            loop_last_iters = loop_iters;
            loop_frames += df;
            loop_none = 0;
            t_msg_ms = t_scan_ms = t_wait_ms = t_assert_ms = t_focus_ms = 0;
        }
    }
}
