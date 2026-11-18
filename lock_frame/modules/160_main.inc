// ============================================================================
//  lock_frame module: 160_main.inc
//
//  main: argument parsing, startup, capture/present loops.
//
//  Included by lock_frame/lock_frame.cpp as part of the single-translation-unit build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

int main()
{
    // Must happen before any window is created: otherwise Windows virtualizes the
    // window size by the display DPI (175% here) and the client rect no longer
    // matches the 5120x2880 swap chain we ask XeSS-FG for.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    logf_("[lock] lock_frame %s (shared protocol v%u)\n",
          LOCK_FRAME_VERSION_STRING, mgpu::kSharedVersion);

    if (wcsstr(GetCommandLineW(), L"--no-fg")) { g_no_fg = true; }
    if (wcsstr(GetCommandLineW(), L"--no-pair-key")) { g_pair_frame_key = false; }
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
        logf_("[lock] --streams: mask=0x%X (mv=%d depth=%d color=%d ui=%d meta=%d)\n", mask,
              (mask & mgpu::kRequestMv) ? 1 : 0, (mask & mgpu::kRequestDepth) ? 1 : 0,
              (mask & mgpu::kRequestColor) ? 1 : 0, (mask & mgpu::kRequestUi) ? 1 : 0,
              (mask & mgpu::kRequestMeta) ? 1 : 0);
        logf_("[lock] --streams raw: mv=%d depth=%d\n",
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
            logf_("[lock] link budget declared as %.1f GB/s (OSD occupancy)\n", v);
        }
    }
    if (wcsstr(GetCommandLineW(), L"--no-depth")) { g_no_depth = true; }
    if (wcsstr(GetCommandLineW(), L"--no-mv")) { g_no_mv = true; }
    if (wcsstr(GetCommandLineW(), L"--mv-shadow")) { g_mv_shadow = true; }
    // --log-flush: fflush after every line (pre-change behaviour). Useful when watching
    // for a crash, where the last lines before the process dies are the interesting ones.
    if (wcsstr(GetCommandLineW(), L"--log-flush")) { g_log_flush_each = 1; }
    if (wcsstr(GetCommandLineW(), L"--capture-monitor")) { g_capture_monitor = true; }
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
        logf_("[lock] --gpu %ls: consumer adapter forced by spec\n", g_gpu_spec.c_str());
    }
    if (g_have_producer) {
        logf_("[lock] --producer-luid %08lX:%08lX: the pick will avoid the producer's"
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
    // --jbuf N: our own jitter buffer (see the comment next to g_jbuf).
    if (const wchar_t* jb = wcsstr(GetCommandLineW(), L"--jbuf ")) {
        const int n = _wtoi(jb + 7);
        if (n >= 0 && n <= 3) { g_jbuf = n; }
    }
    // --slack N: see the comment next to g_slack.
    if (const wchar_t* sl = wcsstr(GetCommandLineW(), L"--slack ")) {
        const int n = _wtoi(sl + 8);
        if (n >= 0 && n <= 3) { g_slack = static_cast<UINT>(n); }
    }
    if (wcsstr(GetCommandLineW(), L"--slack-auto")) {
        g_slack_auto = true;
        logf_("[lock] --slack-auto: a queued capture is held (instead of dropped) whenever"
              " the producer published new content since we consumed the last one - targeted at"
              " the occasional 'drop 1' in --interp-cadence runs, without --slack 1's constant"
              " +1 interval. Per-window counts: 'slack-auto: N held back / M still dropped'\n");
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
        logf_("[lock] --cpu-low: bridge process below normal, main thread below normal"
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
        logf_("[lock] --fg-jitter-zero: passing jitter = 0. Our colour is the post-upscaler"
              " image (the jitter is already resolved), so this is the A/B control for"
              " 'is XeFG shifting its history by a jitter that is no longer there?'\n");
    }
    if (wcsstr(GetCommandLineW(), L"--fg-debug-tag-interp")) { g_fg_tag_interp = true; }
    if (wcsstr(GetCommandLineW(), L"--fg-debug-only-interp")) { g_fg_only_interp = true; }
    // --version: print the product and wire versions, then exit. Useful for support
    // ("which build is this?") and for the release check script.
    if (wcsstr(GetCommandLineW(), L"--version")) {
        printf("lock_frame %s (shared protocol v%u)\n",
               LOCK_FRAME_VERSION_STRING, mgpu::kSharedVersion);
        return 0;
    }
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
    // --cadence-smooth N: EMA window (frames) for the interval that drives the cadence math.
    if (const wchar_t* cs = wcsstr(GetCommandLineW(), L"--cadence-smooth ")) {
        const int n = _wtoi(cs + 17);
        if (n >= 1 && n <= 240) { g_cad_smooth = n; }
    }
    // --cadence-hold N: frames to hold the interpolated count (0/1 = change every frame).
    if (const wchar_t* ch = wcsstr(GetCommandLineW(), L"--cadence-hold ")) {
        const int n = _wtoi(ch + 15);
        if (n >= 0 && n <= 240) { g_cad_hold_frames = n; }
    }
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
    // v29: slot gate. On by default in --mode: the forwarded input rate is held at
    // refresh/(interp+1) by dropping the surplus *before* any copy, decided on the game's
    // own clock (frames[].frame_qpc_ms). --no-slot-gate restores the old forward-everything
    // behaviour (the panel then has to decimate, which it does not do evenly).
    if (wcsstr(GetCommandLineW(), L"--no-slot-gate")) { g_slot_gate = false; }
    if (wcsstr(GetCommandLineW(), L"--slot-gate")) { g_slot_gate = true; }
    // --mode 2x | 3x | 4x: 固定倍率（输出 = 输入 × N）。旧的 30x4 / 40x3 作为
    // 4x / 3x 的别名保留，老快捷方式/脚本不用改。
    // 注意：不在这里启用 --lock 的丢帧逻辑。WGC 的到达时间有 1-2 ms 抖动，
    // 用 GetTickCount64 做"早到就丢"会周期性丢掉真实帧（输入 30 接受成 20）。
    // 输入锁由游戏侧（RTSS/游戏限制器）完成；这里只固定插帧数。
    if (const wchar_t* md = wcsstr(GetCommandLineW(), L"--mode ")) {
        const wchar_t* p = md + 7;
        while (*p == L' ') { ++p; }
        int mul = 0;
        if (wcsncmp(p, L"30x4", 4) == 0 || wcsncmp(p, L"4x", 2) == 0) {
            mul = 4;
        } else if (wcsncmp(p, L"40x3", 4) == 0 || wcsncmp(p, L"3x", 2) == 0) {
            mul = 3;
        } else if (wcsncmp(p, L"2x", 2) == 0) {
            mul = 2;
        }
        if (mul >= 2 && mul <= 4) {
            g_fg_interp = mul - 1;      // 1 张插值 = 2x，2 张 = 3x，3 张 = 4x
            // MUST be set: --unlock-mfg picks its patch target from this flag, and when it
            // is false it defaults to 3 (4x) and overwrites g_fg_interp. That is exactly
            // why 40x3 used to run 4x: mode set 2, then the patch reset it to 3.
            g_fg_interp_set = true;
            logf_("[lock] mode: fixed %dx (output = input x%d; lock input = panel Hz / %d)\n",
                  mul, mul, mul);
        } else {
            logf_("[lock] mode: unrecognized '%.8ls' - keeping interp=%d\n", p, g_fg_interp);
        }
        // Do NOT force g_interp_cadence off here: --interp-cadence is parsed earlier and
        // the UI's optional low-fps mode must survive --mode. With no such flag it is
        // already false, so the fixed path stays fixed.
        g_dyn_mfg = false;
        g_lock_auto = false;
    }
    // Lock-frame fixed mode: keep the original direct path. The colour jitter buffer
    // trades latency for smoothing and is intentionally not enabled in the lock scenario.
    // --jbuf remains CLI-only for A/B.
    // Echo the cadence knobs before anything can fail, so the log always says which
    // cadence model ran (both default to the old behaviour and stay silent).
    if (g_fg_rt_mode != kFgRtEma) {
        const char* what = (g_fg_rt_mode == kFgRtQuant) ? "median, snapped to whole vblanks"
                          : (g_fg_rt_mode == kFgRtRaw)  ? "the raw last input interval"
                          : (g_fg_rt_mode == kFgRtAuto) ? "max(m vblanks, median), ramped 0.5 ms"
                                                          " per batch (never below m vblanks)"
                                                        : "median of the last 16 input intervals";
        logf_("[lock] --fg-rt-mode %s%s: frameRenderTime = %s (source: the game's own"
              " present clock, frames[].frame_qpc_ms)\n",
              (g_fg_rt_mode == kFgRtAuto) ? "auto" : fg_rt_mode_name(),
              (g_fg_rt_mode == kFgRtAuto) ? " (default)" : "", what);
    }
    if (g_interp_auto) {
        logf_("[lock] --interp-auto (%s): the interpolation count follows the measured"
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
            logf_("[lock] NOTE: --interp-cadence with --fg-of. Optical flow is one more full"
                  " GPU pass on the same card that runs the generation, and cadence already"
                  " keeps it at 84-90%% load - measured 2026-11-09: drops become clearly more"
                  " frequent with it and almost disappear without it. Prefer --no-mv (smoothest)"
                  " or the game's vectors (cheaper, but flickers at 3x/4x - FINAL_CONFIG 5.17)\n");
        } else {
            logf_("[lock] NOTE: --interp-cadence with the game's motion vectors. This DLL's"
                  " 3x/4x path reuses one MV set for several time fractions and cadence changes"
                  " the count between batches, so expect flicker/ghosting that --no-mv does not"
                  " show. --no-mv is the smoothest of the three (FINAL_CONFIG 5.17.1);"
                  " --cadence-hold is measured worse, do not use it\n");
        }
    }
    if (g_lock_auto) {
        logf_("[lock] --lock-auto: solving (input lock, multiplier) so that"
              " input*(interp+1) divides the refresh - no RTSS, no VRR needed\n");
    }
    if (g_dyn_mfg) {
        static const char* roundings[] = { "at most", "nearest", "at least" };
        logf_("[lock] --dyn-mfg: OptiScaler's Dynamic MFG - target %s, rounding %s,"
              " 1X %s; the count follows the measured frame time and the learned cost of"
              " a generated frame\n",
              (g_dyn_target > 0.0) ? "from --fg-target" : "just under the refresh",
              roundings[(g_dyn_rounding < 0 || g_dyn_rounding > 2) ? 1 : g_dyn_rounding],
              g_dyn_allow1x ? "allowed" : "not allowed");
    }
    if (g_lock_fps > 1.0) {
        logf_("[lock] --lock %.2f fps: captured frames that arrive early are dropped so"
              " the forwarded input rate is exactly this\n", g_lock_fps);
    }
    if (g_interp_cadence) {
        logf_("[lock] --interp-cadence: variable-rate generation - the multiplier follows"
              " the vblanks between real frames (output runs at the panel refresh, no lock"
              " needed; the refresh is detected during FG init)\n");
        // The 2026-11-07 "cadence is worse" result (FINAL_CONFIG 9.18) was retracted: it
        // was the arrival-interval bug (GetTickCount64 truncated the 0 ms samples, so the
        // median read 31 ms instead of 25 ms and the count snapped to 4). With the WGC
        // SystemRelativeTime timestamp cadence measured 1.0-1.5% empty vblanks.
        logf_("[lock]   cadence: arrival smoothing %d frame(s), count hold %d,"
              " frameRenderTime %s\n",
              g_cad_smooth, g_cad_hold_frames, g_fg_cad_rt ? "batch*vblank" : "median");
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
    if (const wchar_t* mi = wcsstr(GetCommandLineW(), L"--wgc-minint ")) {
        g_wgc_minint_ms = _wtoi(mi + 13);
        if (g_wgc_minint_ms < 0) { g_wgc_minint_ms = 0; }   // 0 = no throttle
        if (g_wgc_minint_ms > 100) { g_wgc_minint_ms = 100; }
        logf_("[lock] --wgc-minint %d ms\n", g_wgc_minint_ms);
    }
    if (wcsstr(GetCommandLineW(), L"--no-of-fallback")) { g_of_fallback = false; }
    if (wcsstr(GetCommandLineW(), L"--of-miss-fallback")) { g_of_miss_fallback = true; }
    if (const wchar_t* fc = wcsstr(GetCommandLineW(), L"--fg-colour ")) {
        const wchar_t* p = fc + 12;
        while (*p == L' ') { ++p; }
        if (wcsncmp(p, L"shared", 6) == 0) {
            g_fg_colour_shared = true;
            // The colour arrives already PQ-encoded, so the convert pass must not encode
            // it again (mode 3 in k_conv_hlsl).
            g_colour_mode = 3;
            logf_("[lock] --fg-colour shared: colour comes from the add-on's stream[0]"
                  " (cross-adapter copy), not from WGC; frames are driven by the producer's"
                  " publications\n");
        }
    }
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
        logf_("[lock] --fg-of ignored: not compiled in (MV is a dead input for"
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
        logf_("[lock] --dump %d frame(s) to %s (colour/depth/MV raw + meta)\n",
              g_dump_frames, g_dump_dir);
        logf_("[lock]   run with real inputs (no --no-mv/--no-depth) or the dump"
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
        logf_("[lock] --input-copy-queue: MV/depth copies will use a COPY queue\n");
    }
    // --prefetch-inputs: copy the *next* frame's MV/depth during this frame's Present.
    // Implies --input-copy-queue (it needs the second queue to get the copies off the frame
    // in the first place). Off by default: it changes which frame each slot is read for.
    if (wcsstr(GetCommandLineW(), L"--prefetch-inputs")) {
        g_prefetch_inputs = true;
        g_input_copy_queue = true;
        logf_("[lock] --prefetch-inputs: MV/depth are copied one frame ahead (implies"
              " --input-copy-queue; this is the default)\n");
    }
    if (wcsstr(GetCommandLineW(), L"--no-prefetch-inputs")) {
        g_prefetch_inputs = false;
        g_pf_pub = false;
        g_input_copy_queue = false;
        logf_("[lock] --no-prefetch-inputs: copies run after WGC again (old order)\n");
    }
    if (wcsstr(GetCommandLineW(), L"--no-prefetch-publications")) {
        g_pf_pub = false;
        logf_("[lock] --no-prefetch-publications: one-slot-ahead prefetch instead of the"
              " publication cache\n");
    } else if (wcsstr(GetCommandLineW(), L"--prefetch-publications")) {
        g_pf_pub = true;
        logf_("[lock] --prefetch-publications: every new publication is copied into the"
              " local cache\n");
    }
    if (wcsstr(GetCommandLineW(), L"--pub-fifo")) {
        g_pub_fifo = true;
        g_pf_pub = true;
        logf_("[lock] --pub-fifo: copied publications are consumed in FIFO order (no slot"
              " key, no miss); queue holds at most %u, a reused slot drops its stale entry\n",
              mgpu::kMaxSlots);
    }
#if MGPU_FG_OF
    // Prefetch misses use the flow instead of a fallback copy. The FFX context keeps its
    // own history, so it must be fed every frame; the output is only tagged on a miss.
    // Opt-in: at 4K the flow costs ~2-3 ms on the render card, which alone exceeds the
    // 25 ms budget on this chain. The default miss path is the in-frame copy.
    if (g_of_miss_fallback && g_prefetch_inputs && g_of_fallback && !g_no_mv && !g_no_inputs) {
        g_of_track = true;
        logf_("[lock] of: tracking every frame for prefetch misses (no copy on miss,"
              " opt-in; expect ~2-3 ms at 4K)\n");
    }
#endif
    // --stream-auto: the consumer picks MV/depth StreamPercent per report window from the
    // measured WGC hop (p10) + the copy model, and writes it through protocol v15.
    // --stream-asym: MV stays 1:1, depth is scaled (ini value, or the auto choice). It
    // implies --upsample because XeFG needs the tagged MV/depth pair to be the same size.
    if (wcsstr(GetCommandLineW(), L"--stream-auto")) {
        g_stream_auto = true;
        logf_("[lock] --stream-auto: per-stream StreamPercent is chosen from the measured"
              " WGC age (p10) and the copy model, one step per window\n");
    }
    if (wcsstr(GetCommandLineW(), L"--stream-asym")) {
        g_stream_asym = true;
        logf_("[lock] --stream-asym: MV stays 1:1, depth is scaled; --upsample is implied"
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
        logf_("[lock] --upsample: ON - MV/depth are rebuilt from the stream size to the"
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
            logf_("[lock] --input-ms %.2f: the pacing model treats the input interval as"
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
    if (const wchar_t* pl = wcsstr(GetCommandLineW(), L"--pool ")) {
        const int n = _wtoi(pl + 7);
        if (n >= 2 && n <= 8) { g_wgc_pool = static_cast<UINT>(n); }
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

    // ---- 这一版的固定选择 -------------------------------------------------
    // 颜色永远走 WGC（producer 用 ColourStream=0 不发布颜色）；MV/depth 只用
    // 精确 marker 配对，所以关闭所有"按时间/轮转猜下一个槽"的路径：
    //   --prefetch-inputs / --prefetch-publications / --pub-fifo 都会在颜色到达
    //   之前预先拷贝一个"猜的"槽，锁帧版不需要也不允许这种猜测。
    // StreamPercent 由 producer 的 fakerFG.ini 固定（默认 150），消费者不再自动调。
    // 以上都是强制项：这一版只为固定倍率 2x/3x/4x 服务，A/B 入口在命令行保留但不再生效。
    g_fg_colour_shared = false;
    g_pair_frame_key   = false;
    g_prefetch_inputs  = false;
    g_pf_pub           = false;
    g_pub_fifo         = false;
    g_stream_auto      = false;
    g_stream_asym      = false;
    // 锁帧版固定开启：MV/depth 跨卡按 StreamPercent 传输（默认 150），到消费卡后
    // 用 080 模块的联合重建 pass 还原到 frames[].render_w/h（输入画面尺寸）再 tag。
    g_upsample         = true;
    if (!g_stream_mask_explicit) {
        g_stream_mask = mgpu::kRequestMv | mgpu::kRequestDepth;
    }
    logf_("[lock] --upsample: ON by default - MV/depth 150%% transfer, reconstructed to the"
          " input picture size before tagging\n");
    logf_("[lock] locked cadence: interp=%d (%dx per input), report every %d frames;"
          " slot gate: %s%s\n",
          g_fg_interp, g_fg_interp + 1, 40,
          g_slot_gate ? "on (drops the surplus on the game clock)" : "off",
          (g_lock_fps > 1.0) ? "; --lock also drops early captures" : "");

    logf_("[lock] mode: fg=%s den=%d scale=%.2f buffers=%u pool=%u latency=%u tearing=%d"
          " xell=%s(mode %d) inputs=%s sharp=%.2f colour=%d interp=%d unlock=%d down=%d\n",
          g_fg_warp ? "warp" : (g_no_fg ? "off" : "xess-fg"), g_out_den, g_scale,
          g_swap_buffers, g_wgc_pool, g_max_latency, g_tearing ? 1 : 0,
          g_no_xell ? "no-sleep" : "on", g_xell_mode,
          g_no_inputs ? "off" : "on", g_sharpen, g_colour_mode, g_fg_interp,
          g_unlock_mfg ? 1 : 0, g_down);
    if (g_overlay_monitor >= 0 || g_overlay_hidden) {
        logf_("[lock] overlay: monitor=%s hidden=%d (capture stays %s)\n",
              (g_overlay_monitor >= 0) ? "explicit" : "game window",
              g_overlay_hidden ? 1 : 0,
              g_capture_monitor ? "monitor" : "window");
    }

    if (g_game_hwnd != nullptr || g_game_pid != 0) {
        if (g_game_hwnd != nullptr) {
            logf_("[lock] target window %p (--hwnd)\n", (void*)g_game_hwnd);
            if (!IsWindow(g_game_hwnd)) { g_game_hwnd = nullptr; }
        } else {
            logf_("[lock] target process %lu (--pid)\n", (unsigned long)g_game_pid);
        }
    } else if (g_game_exe_set) {
        logf_("[lock] game process: %ls (--game)\n", g_game_exe.c_str());
    } else {
        logf_("[lock] no --game/--hwnd/--pid: using the foreground window\n");
    }

    {
        static const char* k_down_names[] = { "bilinear (1 tap)",
                                              "catmull-rom (support 2)",
                                              "mitchell B=C=1/3 (support 2)",
                                              "lanczos-3 (support 3)" };
        const int i = (g_down >= 0 && g_down <= 3) ? g_down : 0;
        if (g_down == 0) {
            logf_("[lock] resize filter: %s - add --down 1 (catmull-rom) for a real"
                  " low-pass; --down 2 soft/no ringing, --down 3 sharpest\n", k_down_names[i]);
        } else {
            logf_("[lock] resize filter: %s, taps follow the scale at draw time\n",
                  k_down_names[i]);
        }
    }

    if (g_fg_rt_zero || g_fg_ui_none || g_fg_scene > 0.0f || g_fg_depth_inv ||
        g_fg_jittered_mv_forced) {
        logf_("[lock] XeFG tuning: rt=%s ui=%s scene=%.2f depthInv=%d jitteredMV=%d%s\n",
              g_fg_rt_zero ? "zero" : "input",
              g_fg_ui_none ? "none" : "auto",
              g_fg_scene, g_fg_depth_inv ? 1 : 0, g_fg_jittered_mv ? 1 : 0,
              g_fg_jittered_mv_forced ? (g_fg_jittered_mv ? " (forced ON)" : " (forced OFF)")
                                      : " (automatic: follows the producer's DLSS flag)");
    }
    if (g_slack > 0) {
        logf_("[lock] --slack %u: holding up to %u captured frames, processing the oldest"
              " (adds up to %u input intervals of latency)\n", g_slack, g_slack + 1, g_slack);
        if (g_wgc_pool < g_slack + 2) {
            logf_("[lock]   note: --pool %u is smaller than slack+2; the WGC pool caps how"
                  " much can be held\n", g_wgc_pool);
        }
    }

    // libxess_fg.dll is loaded here rather than through an import library, so the
    // file can be chosen (--fg-lib) and its real version and MFG capability can be
    // reported before anything is initialised.
    //
    // This happens even for --fg warp: that path replaces the interpolator but still
    // presents through XeSS-FG's proxy swap chain, so the DLL is required either way.
    {
        std::string fg_err;
        if (xefg_dyn::Load(g_api, g_fg_lib, &fg_err)) {
            logf_("[lock] libxess_fg: %s\n", narrow_utf8(g_api.path).c_str());
            xefg_swapchain_version_t v{};
            if (g_api.GetVersion && g_api.GetVersion(&v) == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                logf_("[lock] libxess_fg version %u.%u.%u, SetNumInterpolatedFrames=%s\n",
                      v.major, v.minor, v.patch, g_api.has_mfg() ? "yes" : "NO");
            }
        } else {
            logf_("[lock] libxess_fg.dll NOT loaded: %s\n", fg_err.c_str());
            logf_("[lock]   the proxy swap chain needs it even with --fg warp;"
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
        logf_("[lock] RoInitialize FAILED\n");
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
            // a bare `lock_frame.exe` (or a double-clicked recipe) work; the old code fell
            // back to a hard-coded Cyberpunk2077.exe, which only worked on one machine.
            game_wnd = GetForegroundWindow();
            logf_("[lock] target: foreground window %p\n", (void*)game_wnd);
        }
        if (!game_wnd) {
            if (g_game_pid != 0) {
                logf_("[lock] no visible window for pid %lu\n", (unsigned long)g_game_pid);
            } else if (g_game_exe_set) {
                logf_("[lock] game window not found - is %ls running?\n", g_game_exe.c_str());
            } else {
                logf_("[lock] no foreground window - click into the game first,"
                      " or pass --game <exe> / --hwnd <hex>\n");
            }
            return 2;
        }
        logf_("[lock] game window %p\n", (void*)game_wnd);
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
                        logf_("[lock] %ls is an Unreal shipping build - using our optical flow"
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
            logf_("[lock] game window style=0x%08lX ex=0x%08lX topmost=%d rect=(%ld,%ld)-(%ld,%ld)%s\n",
                  static_cast<unsigned long>(GetWindowLongPtrW(g_game_wnd, GWL_STYLE)),
                  static_cast<unsigned long>(ex), (ex & WS_EX_TOPMOST) ? 1 : 0,
                  gr.left, gr.top, gr.right, gr.bottom,
                  (ex & WS_EX_TOPMOST)
                      ? "   (already topmost: possibly left over from an earlier run,"
                        " cleared on exit)"
                      : "");
        }
    } else {
        logf_("[lock] --fg-probe: no game window needed\n");
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
        logf_("[lock] no usable consumer adapter (only software adapters, or --gpu"
              " matched nothing)\n");
        logf_("[lock]   choose one explicitly: --gpu <index|nv|amd|intel|"
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
        logf_("[lock] --fg-of-selftest: %s (%ux%u field)\n", ok ? "OK" : "FAILED", g_of_w, g_of_h);
        return ok ? 0 : 13;
    }
#else
    if (wcsstr(GetCommandLineW(), L"--fg-of-selftest")) {
        logf_("[lock] --fg-of-selftest: not compiled in (MGPU_FG_OF=0)\n");
        return 13;
    }
#endif
    if (!open_shared()) { return 5; }
    // The capture item comes first: its size is the only trustworthy one, and the colour
    // sink / frame pool / FG swap chain all have to be built at that size.
    if (!create_capture_item(game_wnd)) { return 7; }
    adopt_capture_size();
    if (!create_colour_sink(g_width, g_height)) { return 6; }
    if (!init_convert()) { return 9; }
    if (!g_fg_colour_shared && !start_wgc()) { return 7; }
    if (g_fg_colour_shared) {
        logf_("[lock] --fg-colour shared: WGC is not started; the producer's stream[0]"
              " carries the colour\n");
    }
    if (g_pace_ms > 1.0 && !g_fg_warp) {
        logf_("[lock] note: --pace pins our Present times, but XeSS-FG already paces"
              " itself from the XeLL markers and blocks ~15 ms inside Present, so the two"
              " serialise (load rises, ticks get missed). Leave --pace off for XeSS-FG.\n");
    }
    if (!init_fg()) { return 8; }
    if (g_fg_warp && (!init_warp() || !init_warp_pso())) { return 10; }

    logf_("\n[lock] running - close the window or press Ctrl+C to stop\n\n");

    // --jbuf helpers: take/return a colour copy from our own pool.
    auto jbuf_take = [](ID3D11Texture2D* src) -> ComPtr<ID3D11Texture2D> {
        if (!g_jbuf_free.empty()) {
            ComPtr<ID3D11Texture2D> t = g_jbuf_free.back();
            g_jbuf_free.pop_back();
            return t;
        }
        D3D11_TEXTURE2D_DESC d{};
        src->GetDesc(&d);
        d.Usage          = D3D11_USAGE_DEFAULT;
        d.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = 0;
        d.MiscFlags      = 0;
        ComPtr<ID3D11Texture2D> t;
        if (FAILED(g_d3d11->CreateTexture2D(&d, nullptr, &t))) { return nullptr; }
        return t;
    };
    auto jbuf_give_back = [](ComPtr<ID3D11Texture2D>& t) {
        if (!t) { return; }
        if (g_jbuf > 0) { g_jbuf_pending.push_back(t); }   // ours: recycle through the pool
        t.Reset();                                          // WGC frame: let go of it now
    };

    // Frame timing bookkeeping shared by both paths: arrival EMA, jitter extremes and
    // the count of gaps (frames that never reached us at all).
    //
    // The interval comes from the capture frame's SystemRelativeTime, NOT from the wall
    // clock at the moment we dequeued it. That difference is load-bearing:
    //   * the compositor's stamp is the game's own cadence, quantised to vblanks;
    //   * GetTickCount64 has 1 ms resolution, so two frames drained in one loop iteration
    //     land in the same millisecond, the second delta is 0, and arr_push() drops it.
    //     That biased the median upward (31.0 ms for a locked 40 fps input) and made
    //     --fg-rt-mode quant snap frameRenderTime to 4 vblanks instead of 3, so the output
    //     ran at 96.8 fps on a 120 Hz panel and the cadence line said NOT-EXACT.
    // The wall clock is kept only as the fallback for a frame with no usable timestamp.
    auto note_arrival = [](IDirect3D11CaptureFrame* f) {
        const ULONGLONG now = GetTickCount64();
        double dt_arr = 0.0;                  // measured interval, compositor clock first

        ABI::Windows::Foundation::TimeSpan wts{};
        if (SUCCEEDED(f->get_SystemRelativeTime(&wts))) {
            const double ts_ms = static_cast<double>(wts.Duration) / 10000.0;
            g_wgc_ts_ms = ts_ms;
            if (g_arr_prev > 0.0) {
                const double d = ts_ms - g_arr_prev;
                if (d > 0.0 && d < 1000.0) {
                    dt_arr = d;
                    if (d < g_arr_min) { g_arr_min = d; }
                    if (d > g_arr_max) { g_arr_max = d; }
                    if (d > g_arrival_ms_ema * 1.6) { ++g_arr_drops; }
                    ++g_arr_count;
                    // Bucket it in vblanks: 3 = a 40 fps game on a 120 Hz panel.
                    const double vb = refresh_ms();
                    if (vb > 0.5) {
                        int k = static_cast<int>(d / vb + 0.5);
                        if (k < 1) { k = 1; }
                        if (k > 8) { k = 8; }
                        ++g_arr_hist[k];
                    }
                }
            }
            // How old is this frame when it reaches us? That is the WGC/DWM delay the
            // pipeline pays before any of our work starts.
            const double age = qpc_ms() - ts_ms;
            if (age > 0.0 && age < 500.0) {
                g_ms_age = g_ms_age * 0.9 + age * 0.1;
                if (age > g_max_age) { g_max_age = age; }
                age_push(age);          // --stream-auto budget (low percentile)
            }

            g_arr_prev = ts_ms;
        }

        if (dt_arr <= 0.0 && g_last_arrival) {
            const double dw = static_cast<double>(now - g_last_arrival);
            if (dw > 0.0 && dw < 1000.0) { dt_arr = dw; }
        }
        g_last_arrival = now;

        if (dt_arr > 0.0) {
            g_arrival_ms_ema = g_arrival_ms_ema * 0.9 + dt_arr * 0.1;
            arr_push(dt_arr);            // --fg-rt-mode median/quant + --interp-auto
            g_arr_wc_ring[g_arr_wc_i] = dt_arr;
            g_arr_wc_i = (g_arr_wc_i + 1) % kArrRing;
            if (g_arr_wc_n < kArrRing) { ++g_arr_wc_n; }

            // --fg-rt-mode auto: only collect here. The decision itself is taken once per
            // report window (see the report block): a 16-sample median jumps between the
            // quantised values 25.0 / 33.3 while an unlocked rate wobbles, so deciding per
            // frame made the pacer's input flap (measured: 4 flips in 18 s).
            g_arr_slow_ring[g_arr_slow_i] = dt_arr;
            g_arr_slow_i = (g_arr_slow_i + 1) % kArrSlowRing;
            if (g_arr_slow_n < kArrSlowRing) { ++g_arr_slow_n; }
            g_rt_ema_fast = arr_median_ms();          // short median (16 samples)
            g_rt_ema_slow = (g_arr_slow_n >= 32) ? arr_slow_median_ms() : g_rt_ema_fast;
        }
    };

    auto frame_to_texture = [](IDirect3D11CaptureFrame* f,
                               ComPtr<ID3D11Texture2D>& tex) -> bool {
        ComPtr<IDirect3DSurface> surface;
        f->get_Surface(&surface);
        ComPtr<IDirect3DDxgiInterfaceAccess> access;
        if (FAILED(surface.As(&access))) { return false; }
        return SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&tex)));
    };

    ComPtr<IDirect3D11CaptureFrame> pending;   // newest frame, waiting for its tick
    ComPtr<ID3D11Texture2D> pending_tex;

    // Frames we captured but have not processed yet. Only --slack > 0 lets this
    // hold more than one; every extra frame is an input interval of latency we
    // trade for absorbing a late one (see g_slack).
    std::deque<std::pair<ComPtr<IDirect3D11CaptureFrame>, ComPtr<ID3D11Texture2D>>> slack_q;

    // Escape hatch that does not depend on mgpu_ui.exe. If the UI hangs, lost the
    // hotkey or was closed, Ctrl+Alt+End still tears the overlay down.
    constexpr int kEscapeHotkeyId = 0x4D47;   // 'MG'
    // Ctrl+Alt+1..5 / 0: live stream switches. They write the same request_mask the CLI
    // does, so the producer reacts within one frame - no restart, no re-layout.
    constexpr int kStreamHkBase = 0x4D50;
    const uint32_t hk_bits[5] = { mgpu::kRequestMv, mgpu::kRequestDepth, mgpu::kRequestUi,
                                  mgpu::kRequestColor, mgpu::kRequestRawMv };
    bool stream_hk[6] = {};
    stream_hk[0] = RegisterHotKey(nullptr, kStreamHkBase + 0, MOD_CONTROL | MOD_ALT, '1') != FALSE;
    stream_hk[1] = RegisterHotKey(nullptr, kStreamHkBase + 1, MOD_CONTROL | MOD_ALT, '2') != FALSE;
    stream_hk[2] = RegisterHotKey(nullptr, kStreamHkBase + 2, MOD_CONTROL | MOD_ALT, '3') != FALSE;
    stream_hk[3] = RegisterHotKey(nullptr, kStreamHkBase + 3, MOD_CONTROL | MOD_ALT, '4') != FALSE;
    stream_hk[4] = RegisterHotKey(nullptr, kStreamHkBase + 4, MOD_CONTROL | MOD_ALT, '5') != FALSE;
    stream_hk[5] = RegisterHotKey(nullptr, kStreamHkBase + 5, MOD_CONTROL | MOD_ALT, '0') != FALSE;
    logf_("[lock] hotkeys: Ctrl+Alt+1 mv, +2 depth, +3 ui, +4 color, +5 rawmv, +0 all"
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
    logf_("[lock] escape hotkey Ctrl+Alt+End registered=%d\n", static_cast<int>(escape_hotkey));



    if (g_fg_colour_shared) {
        // Slot-driven loop: one bridge frame per published producer frame.
        //
        // Why not WGC: the producer publishes exactly one slot per *real* frame (its
        // upscaler/FG declaration cadence), which is the cadence XeSS-FG must interpolate.
        // With the game's own FG cut (FgSuppressNgx=1) the game still presents at 2x and
        // half of those presents carry nothing, so a window capture both mistimes us and
        // samples empty frames. Here the colour arrives through stream[0]
        // (copy_inputs_local), and the producer's publication is the clock.
        const UINT slots = (g_ctl->slots != 0) ? g_ctl->slots : 1;
        UINT64 last_pub = 0;
        logf_("[lock] --fg-colour shared: driving frames from the producer's publications"
              " (%u slots)\n", slots);
        for (;;) {
            MSG msg{};
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
            UINT pick = mgpu::kMaxSlots;
            UINT64 best = last_pub;
            for (UINT s = 0; s < slots; ++s) {
                const uint64_t pub = g_ctl->frames[s].published;
                if (pub > best) { best = pub; pick = s; }
            }
            if (pick >= mgpu::kMaxSlots) {
                // Nothing to do: say WHY once every ~2 s instead of spinning silently, which
                // is what made "no OSD" look like a crash (2026-11-11).
                static int idle = 0;
                if (++idle == 2000) {
                    idle = 0;
                    logf_("[lock] waiting for a publication: published=[%llu %llu %llu %llu]"
                          " last=%llu magic=0x%X last_pub=%llu\n",
                          (unsigned long long)g_ctl->frames[0].published,
                          (unsigned long long)g_ctl->frames[1].published,
                          (unsigned long long)g_ctl->frames[2].published,
                          (unsigned long long)g_ctl->frames[3].published,
                          (unsigned long long)g_ctl->frame_published, g_ctl->magic,
                          (unsigned long long)last_pub);
                }
                Sleep(1);
                keep_game_focus();
                continue;
            }
            g_mv_slot = pick;
            last_pub = best;
            run_frame(nullptr);
            assert_topmost();
        }
    }

    for (;;) {
        // Copy any new producer publication into the local cache while we poll for WGC.
        // This is the copy-on-publication step: it runs during the publisher's own window,
        // so the WGC lookup never has to copy. No-op when --no-prefetch-publications.
        prefetch_publications();

        // The window must be pumped or it goes blank and Windows marks the process
        // as not responding.
        MSG msg{};
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

        if (g_pace_ms > 0.0) {
            // Paced path: keep the newest frame that has arrived, present on a fixed
            // cadence. Frames arriving early simply wait; if none arrived by the tick
            // the previous frame is presented again, so the output rate never wobbles.
            const ULONGLONG t_poll = GetTickCount64();
            for (;;) {
                ComPtr<IDirect3D11CaptureFrame> f;
                if (FAILED(g_pool->TryGetNextFrame(&f)) || !f) { break; }
                ComPtr<ID3D11Texture2D> tex;
                if (!frame_to_texture(f.Get(), tex)) { continue; }
                note_arrival(f.Get());
                if (pending) { ++g_pace_skips; }
                pending = f;
                pending_tex = tex;
            }
            g_ms_capture = g_ms_capture * 0.9 +
                           static_cast<double>(GetTickCount64() - t_poll) * 0.1;

            if (g_pace_auto) {
                const double target = (g_arrival_ms_ema > 2.0) ? g_arrival_ms_ema : 25.0;
                g_pace_ms = g_pace_ms * 0.9 + target * 0.1;
            }
            const double now = qpc_ms();
            if (g_pace_next == 0.0) { g_pace_next = now + g_pace_ms; }
            if (now < g_pace_next) {
                // Sleep while there is real time left, spin for the last couple of ms:
                // a Sleep(0) this close to the deadline is what overshot the tick.
                if (g_pace_next - now > 2.0) { Sleep(1); }
                keep_game_focus();
                continue;
            }
            if (now - g_pace_next > g_pace_ms) { g_pace_next = now; }  // fell behind: resync
            g_pace_next += g_pace_ms;

            if (pending) {
                if (g_fg_warp) { run_frame_warp(pending_tex.Get()); }
                else           { run_frame(pending_tex.Get()); }
                pending.Reset();
                pending_tex.Reset();
            } else if (g_have_last) {
                g_reuse_frame = true;
                if (g_fg_warp) { run_frame_warp(nullptr); }
                else           { run_frame(nullptr); }
                g_reuse_frame = false;
                g_represents += 1.0;
            }
            assert_topmost();
            continue;
        }

        const ULONGLONG t_poll = GetTickCount64();
        // Keep a bounded queue of captured frames.
        //
        // --slack 0 (default) drains the pool and keeps only the NEWEST frame: an
        // over-budget frame costs an input frame, which is what keeps latency at one
        // input interval. Taking the oldest one unconditionally (what TryGetNextFrame
        // returns first) turns pool depth into latency and was what made the 5K 2x
        // pipeline slower and slower.
        //
        // --slack N keeps up to N+1 frames and processes the OLDEST, so a late frame
        // is absorbed by the next one instead of being thrown away. The cap is what
        // bounds the added latency: the (N+2)th frame in the queue drops the oldest.
        //
        // --slack-auto: the same absorption, but only while the queued frame is *new
        // content*. The OSD's "drop 1" is a capture we discard; whether that costs a real
        // input frame depends on whether the producer published something new in between.
        // With the game at ~44 fps on a 120 Hz panel WGC/DWM hands over more captures than the
        // game produces (arr(vb) alternates 2x/4x while game(vb) alternates 2x/3x - measured
        // 2026-11-09), and those duplicates are free to drop. Only when the producer's
        // publication counter moved do we hold the extra frame instead (one interval of
        // latency, once) so the real frame is not lost. `--slack 1` holds unconditionally.
        {
            // --jbuf N owns the queue depth in its own mode (N+1 in flight, oldest
            // processed first); --slack N is the legacy variant that holds WGC frames.
            // Promote last frame's returns now that their copies have retired.
            if (!g_jbuf_pending.empty()) {
                for (auto& t : g_jbuf_pending) { g_jbuf_free.push_back(t); }
                g_jbuf_pending.clear();
            }
            size_t cap = static_cast<size_t>((g_jbuf > 0) ? g_jbuf : g_slack) + 1;
            const bool new_content = g_ctl && (g_ctl->frame_published != g_last_content_pub);
            if (g_slack_auto && g_jbuf == 0 && g_slack == 0 && new_content) { cap = 2; }
            for (;;) {
                ComPtr<IDirect3D11CaptureFrame> f;
                if (FAILED(g_pool->TryGetNextFrame(&f)) || !f) { break; }
                ComPtr<ID3D11Texture2D> tex;
                if (!frame_to_texture(f.Get(), tex)) { continue; }
                note_arrival(f.Get());
                if (g_jbuf > 0) {
                    // Copy the colour out now and let the capture frame go, so the WGC
                    // pool is never held (that is the difference to --slack).
                    ComPtr<ID3D11Texture2D> own = jbuf_take(tex.Get());
                    if (!own) { continue; }
                    g_ctx11->CopyResource(own.Get(), tex.Get());
                    slack_q.emplace_back(nullptr, own);
                    tex.Reset();
                    f.Reset();
                } else {
                    slack_q.emplace_back(f, tex);
                }
                while (slack_q.size() > cap) {
                    // An input frame we will never process. With --jbuf the colour is ours
                    // (recycle it); without it the texture belongs to the capture frame and
                    // simply dropping the reference returns it to the WGC pool.
                    if (g_jbuf > 0) { jbuf_give_back(slack_q.front().second); }
                    slack_q.pop_front();
                    ++g_pace_skips;
                    if (g_slack_auto) {
                        ++g_slack_auto_drops;
                        // Re-check at the moment of the drop: if the producer has published a
                        // new frame since we consumed the last one, this capture carried real
                        // content and the drop costs a frame of motion. Otherwise it is a
                        // duplicate the compositor produced (WGC hands over more captures than
                        // the game presents) and dropping it is free.
                        const bool nc = g_ctl && (g_ctl->frame_published != g_last_content_pub);
                        if (nc) { ++g_slack_auto_real; }
                    }
                }
            }
            // --slack-auto bookkeeping: a frame was held back (= would have been dropped
            // with the default cap) when the queue still has one left after the drain.
            if (g_slack_auto && cap > 1 && slack_q.size() > 1) { ++g_slack_auto_holds; }
        }

        if (slack_q.empty()) {
            // Idle poll: keep the game focused so input never goes to the overlay.
            keep_game_focus();
            assert_topmost();
            Sleep(1);
            continue;
        }

        g_ms_capture = g_ms_capture * 0.9 +
                       static_cast<double>(GetTickCount64() - t_poll) * 0.1;
        // Take the oldest frame in the queue. With --slack 0 the queue only ever
        // holds one frame, so this is the same "process the newest" behaviour.
        ComPtr<IDirect3D11CaptureFrame> frame = slack_q.front().first;
        ComPtr<ID3D11Texture2D> texture = slack_q.front().second;
        slack_q.pop_front();
        // --jbuf textures belong to our own pool and are returned after the frame has
        // been submitted (see the explicit call at the end of this iteration). Do NOT do
        // that with a scope guard: a guard with a reference member released the texture
        // before run_frame() saw it, which made every frame black while all counters
        // stayed healthy.
        // --lock N / --lock-auto: drop captured frames so the *accepted* input rate is
        // exactly N fps.
        //
        // Why this is the root fix (rather than re-implementing XeSS-FG's
        // presentation): the proxy swap chain can only place frames into vblanks, so
        // motion is even exactly when input * (interp+1) spans an integer number of
        // vblanks. XeFG cannot invent a vblank, and without VRR the panel cannot
        // follow the output either - so the *input* has to be quantised. Doing it
        // here instead of with RTSS means no extra tool, no phase drift (every accept
        // advances the schedule by exactly one period) and it works for any game.
        // Dropping is cheap: the frame is already captured, we simply never tag it.
        if (g_lock_fps > 1.0) {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG period = static_cast<ULONGLONG>(1000.0 / g_lock_fps + 0.5);
            if (g_lock_next == 0) { g_lock_next = now; }
            if (now + 1 < g_lock_next) {
                ++g_lock_drops;          // too early: this frame is not used
                continue;
            }
            g_lock_next += (period > 0) ? period : 1;
            if (g_lock_next <= now) { g_lock_next = now + 1; }   // fell behind: resync
        }
        if (g_fg_warp) { run_frame_warp(texture.Get()); }
        else           { run_frame(texture.Get()); }
        // Return the colour texture: with --jbuf it goes back to our pool, otherwise this
        // just drops our reference and hands the buffer back to the WGC frame pool.
        jbuf_give_back(texture);
    }
}

