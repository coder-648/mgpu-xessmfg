// ============================================================================
//  lock_frame module: 070_osd.inc
//
//  OSD window, topmost/z-order repair, frame throttling.
//
//  Included by lock_frame/lock_frame.cpp as part of the single-translation-unit build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

// ---------------------------------------------------------------------------
// Single-line OSD.
//
// The corner block people see today belongs to RTSS: three lines, a big font and
// numbers that describe the tool, not this pipeline. We already know everything
// worth showing, so draw ONE thin line inside the game window ourselves: GDI text
// into a 32bpp DIB, handed to the compositor with UpdateLayeredWindow (per-pixel
// alpha, premultiplied). Updated from assert_topmost() - 2.5 times a second - and it
// never touches the frame-generation path.
//   --no-osd   turn it off (the UI checkbox "单行 OSD")
// ---------------------------------------------------------------------------
static HWND      g_osd_wnd = nullptr;
static bool      g_no_osd = false;

static LRESULT CALLBACK osd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(h, msg, wp, lp);
}

static bool osd_ensure_window()
{
    if (g_osd_wnd) { return true; }
    static bool class_ready = false;
    if (!class_ready) {
        class_ready = true;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = osd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
        wc.lpszClassName = L"mgpu_osd";
        RegisterClassExW(&wc);
    }
    // Layered + transparent + no-activate: it must never steal a click or the focus
    // (that is the bug we spent the whole day chasing; do not remove these bits).
    g_osd_wnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST,
        L"mgpu_osd", L"", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_osd_wnd) { return false; }
    // No SetLayeredWindowAttributes here: it switches the window to "one alpha for
    // the whole surface" and fights UpdateLayeredWindow()'s per-pixel alpha.
    return true;
}

static void osd_update()
{
    if (g_no_osd || !g_game_wnd || !IsWindow(g_game_wnd)) { return; }
    if (!osd_ensure_window()) { return; }

    // One line, numbers only: "120 fps  x5  in 24.0  gpu 7.9 ms  game 41.2 ms  lock 24"
    wchar_t line[256]{};
    int n = 0;
    const double in_fps = (g_arrival_ms_ema > 0.5) ? 1000.0 / g_arrival_ms_ema : 0.0;
    n = _snwprintf_s(line, 256, _TRUNCATE,
                     L"%d fps   x%u   in %.1f fps   gpu %.1f ms   game %.1f ms",
                     g_show_fps, static_cast<unsigned>(g_fg_interp_actual + 1),
                     in_fps, g_ms_throttle, g_arrival_ms_ema);
    if (n < 0) { n = 0; }
    if (n > 0 && n < 250 && g_lock_fps > 1.0) {
        n += _snwprintf_s(line + n, 256 - n, _TRUNCATE, L"   lock %.0f", g_lock_fps);
    }
    // Cross-adapter budget: stream_bytes is what the producer reported writing, so this is
    // a measurement. `in_fps` is the REAL-frame cadence, which is the rate the copies happen
    // at - 3x/4x generate more frames without moving more bytes (TRANSFER_V2 section 13).
    if (g_ctl && in_fps > 1.0 && n > 0 && n < 200) {
        const double mb = (g_ctl->stream_bytes[0] + g_ctl->stream_bytes[1] +
                           g_ctl->stream_bytes[2]) / (1024.0 * 1024.0);
        const double gbps = mb * in_fps * 1048576.0 / 1e9;
        const int pct = (g_link_gbps > 0.1)
                            ? static_cast<int>(100.0 * gbps / g_link_gbps + 0.5) : -1;
        n += _snwprintf_s(line + n, 256 - n, _TRUNCATE, L"   in %.0fMB %.1fG %d%%", mb, gbps, pct);
        if (n < 0) { n = 0; }
    }
    // Native-MV share: how often the frame that reached FG used the game's own
    // motion vectors rather than optical flow / a fallback. This is the quickest
    // health signal during a reset/degraded period.
    {
        const uint32_t native_n = g_mv_native_frames;
        const uint32_t flow_n = g_mv_flow_frames;
        const uint32_t none_n = g_mv_none_frames;
        const uint32_t total_n = native_n + flow_n + none_n;
        const int pct = (total_n > 0) ? static_cast<int>(100.0 * native_n / total_n + 0.5) : -1;
        const wchar_t* health = (g_pair_health == kPairNormal) ? L"OK"
                              : (g_pair_health == kPairWatch) ? L"WATCH"
                              : (g_pair_health == kPairSoftRecover) ? L"RECOVER"
                              : (g_pair_health == kPairDegraded) ? L"DEGRADED"
                              : (g_pair_health == kPairHardReset) ? L"RESET"
                              : L"REJOIN";
        if (n > 0 && n < 220) {
            n += _snwprintf_s(line + n, 256 - n, _TRUNCATE,
                              L"   mvN %d%%  %s", pct, health);
            if (n < 0) { n = 0; }
        }
    }
    const unsigned drops = g_lock_drops + g_pace_skips;
    if (n > 0 && n < 240 && drops > 0) {
        n += _snwprintf_s(line + n, 256 - n, _TRUNCATE, L"   drop %u", drops);
    }
    if (n <= 0) { return; }

    const UINT dpi = GetDpiForWindow(g_game_wnd);
    const int sc = static_cast<int>(dpi ? dpi : 96);

    // Place it in the game window's top-left corner, 8 px in.
    RECT cr{};
    GetClientRect(g_game_wnd, &cr);
    POINT org{ cr.left, cr.top };
    ClientToScreen(g_game_wnd, &org);
    const int margin = MulDiv(8, sc, 96);
    const int px = org.x + margin;
    const int py = org.y + margin;

    // The numbers change about once a second while this runs every 400 ms. Blitting an
    // identical bitmap forces DWM to recompose the layered window, which kicks the
    // overlay out of its independent flip and shows up as dropped input frames, so
    // skip the redraw when neither the text nor its position changed.
    static wchar_t last_line[256]{};
    static int last_n = -1, last_x = 0, last_y = 0, last_dpi = 0;
    if (n == last_n && px == last_x && py == last_y && sc == last_dpi &&
        wcscmp(last_line, line) == 0) {
        return;
    }
    last_n = n;
    last_x = px;
    last_y = py;
    last_dpi = sc;
    wmemcpy(last_line, line, static_cast<size_t>(n) + 1);

    osd_panel_render(g_osd_wnd, line, n, px, py, sc);
}

static void assert_topmost()
{
    HWND ovl = overlay_wnd();
    if (!ovl) { return; }
    const ULONGLONG now = GetTickCount64();
    if (now - g_last_topmost < 400) { return; }
    g_last_topmost = now;

    // --overlay-hidden: the window must stay invisible and must NOT be forced above the
    // game, otherwise the experiment measures nothing. Only the focus guard still runs.
    if (g_overlay_hidden) {
        focus_game();
        if (g_game_wnd && !IsWindow(g_game_wnd)) {
            logf_("[lock] the game window disappeared - exiting\n");
            PostQuitMessage(0);
        }
        return;
    }

    // Re-assert the overlay's place in the topmost band, but *below* the OSD window.
    // HWND_TOPMOST means "top of the whole topmost band", so doing it unconditionally
    // every 400 ms put the full-screen overlay above the OSD line until osd_update()
    // pushed the OSD back up - a visible 2.5 Hz flicker. Inserting the overlay after
    // the OSD window keeps a stable order, and skipping the call when the stack has
    // not drifted avoids needless DWM recomposes (those cost the game its flip).
    {
        const HWND want_after = g_osd_wnd ? g_osd_wnd : HWND_TOPMOST;
        const bool already = (want_after == HWND_TOPMOST)
            ? ((GetWindowLongPtrW(ovl, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
            : (GetWindow(ovl, GW_HWNDPREV) == want_after);
        if (!already) {
            SetWindowPos(ovl, want_after, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    // Click-through is not something the library guarantees: check it and put it back.
    {
        const LONG_PTR ex = GetWindowLongPtrW(ovl, GWL_EXSTYLE);
        const bool want_transparent = !g_relay_input;
        const bool has_transparent = (ex & WS_EX_TRANSPARENT) != 0;
        const bool ok = (has_transparent == want_transparent) &&
                        ((ex & WS_EX_NOACTIVATE) != 0);
        if (!ok) {
            LONG_PTR fixed = (ex | WS_EX_NOACTIVATE);
            if (want_transparent) { fixed |= WS_EX_TRANSPARENT; }
            else                  { fixed &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT); }
            SetWindowLongPtrW(ovl, GWL_EXSTYLE, fixed);
            if (!g_clickthrough_logged) {
                g_clickthrough_logged = true;
                logf_("[lock] overlay had lost WS_EX_TRANSPARENT/NOACTIVATE - re-applied"
                      " (before this, clicks went to the overlay, not the game)\n");
            }
        }
    }

    // Focus guard. Two separate problems live here:
    //  1) If OUR window ever owns the foreground, the game believes it is inactive.
    //     Raw input (attacks) still works, but its UI ignores mouse clicks - that is
    //     exactly the "menus are dead, attacks are fine" report. Hand it back.
    //  2) If a THIRD-PARTY window keeps the foreground, the overlay is not needed any
    //     more: exit after g_exit_after_s seconds instead of staying topmost forever.
    {
        const HWND fg = GetForegroundWindow();
        if (fg_is_ours(fg) && g_game_wnd) {
            focus_game();
            g_fg_lost_ms = 0;
        } else if (fg && fg != g_game_wnd && !fg_is_shell_ui(fg)) {
            // The launcher is exempt (cached: this runs every 400 ms).
            static DWORD launcher_pid = 0;
            static ULONGLONG launcher_check = 0;
            DWORD fpid = 0;
            GetWindowThreadProcessId(fg, &fpid);
            if (fpid != launcher_pid && now - launcher_check > 4000) {
                launcher_check = now;
                launcher_pid = fg_is_launcher(fg) ? fpid : 0;
            }
            if (fpid != 0 && fpid == launcher_pid) {
                g_fg_lost_ms = 0;
            } else if (g_fg_lost_ms == 0) { g_fg_lost_ms = now; }
            else if (g_exit_after_s > 0 && now - g_fg_lost_ms >= g_exit_after_s * 1000ull) {
                logf_("[lock] another window has been in front for %us - exiting\n",
                      g_exit_after_s);
                PostQuitMessage(0);
            }
        } else {
            g_fg_lost_ms = 0;
        }
        if (g_game_wnd && !IsWindow(g_game_wnd)) {
            logf_("[lock] the game window disappeared - exiting\n");
            PostQuitMessage(0);
        }
    }

    // Click probe. WindowFromPoint() skips WS_EX_TRANSPARENT windows, i.e. it answers
    // exactly the question "where would this click land?". If the answer is not the
    // game, our overlay (or a helper window of ours) is eating the input no matter
    // what the ex-styles claim, and the log tells us which one.
    //
    // It keeps sampling for the whole session (every 30 s after the first 12 probes): an
    // input regression that only shows up after minutes of play is exactly the kind of bug
    // that cannot be diagnosed from a 30 s test run.
    if (g_game_wnd && (g_click_probe < 12 || now - g_click_probe_ms > 30000)) {
        POINT pt{};
        GetCursorPos(&pt);
        RECT gr{};
        GetWindowRect(g_game_wnd, &gr);
        if (PtInRect(&gr, pt)) {
            HWND under = WindowFromPoint(pt);
            HWND root = under ? GetAncestor(under, GA_ROOT) : nullptr;
            DWORD upid = 0;
            if (under) { GetWindowThreadProcessId(under, &upid); }
            ++g_click_probe;
            g_click_probe_ms = now;
            const char* verdict = "  <-- NOT the game";
            if (root == g_game_wnd) { verdict = "  (ok)"; }
            else if (root == ovl) { verdict = "  <== OUR OVERLAY eats this click"; }
            else if (g_osd_wnd && root == g_osd_wnd) { verdict = "  <== the OSD window eats it"; }
            logf_("[lock] click probe: cursor %ld,%ld under=%p pid=%lu root=%p game=%p%s\n",
                  pt.x, pt.y, reinterpret_cast<void*>(under), static_cast<unsigned long>(upid),
                  reinterpret_cast<void*>(root), reinterpret_cast<void*>(g_game_wnd), verdict);
        } else {
            g_click_probe_ms = now;      // cursor is outside the game: say nothing
        }
    }

    // Put the game window directly underneath us. Windows only promotes a window to
    // a direct-flip layer when it is the topmost visible thing on the monitor; with
    // the game tucked below our overlay, normal DWM composition is used and the
    // frame-generated picture stays visible.
    if (g_game_wnd && GetWindow(ovl, GW_HWNDNEXT) != g_game_wnd) {
        SetWindowPos(g_game_wnd, ovl, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else if (!g_zorder_logged) {
        g_zorder_logged = true;
        logf_("[lock] z-order: game window is directly below the overlay\n");
    }

    // Same 400 ms cadence as everything else here.
    osd_update();
}

// Dumps the real window stacking order: tells us whether the game is genuinely
// above our overlay in the z-order (software problem) or whether the game is
// scanning out directly while our window sits on top (DWM/direct-flip problem).
static void dump_zorder()
{
    logf_("[lock] zorder: fg=%p game=%p overlay=%p overlay_visible=%d overlay_topmost=%d\n",
          reinterpret_cast<void*>(GetForegroundWindow()),
          reinterpret_cast<void*>(g_game_wnd),
          reinterpret_cast<void*>(overlay_wnd()),
          overlay_wnd() ? IsWindowVisible(overlay_wnd()) : 0,
          (overlay_wnd() && (GetWindowLongPtrW(overlay_wnd(), GWL_EXSTYLE) & WS_EX_TOPMOST)) ? 1 : 0);

    HWND w = GetTopWindow(nullptr);
    for (int i = 0; i < 8 && w; ++i) {
        wchar_t cls[64]{};
        GetClassNameW(w, cls, 64);
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        RECT r{};
        GetWindowRect(w, &r);
        logf_("[lock]   z#%d %p pid=%u vis=%d %ldx%ld cls=%ls%s\n", i,
              reinterpret_cast<void*>(w), pid, IsWindowVisible(w),
              r.right - r.left, r.bottom - r.top, cls,
              (w == overlay_wnd()) ? "  <== OVERLAY" : (w == g_game_wnd ? "  <== GAME" : ""));
        w = GetWindow(w, GW_HWNDNEXT);
    }
}

// Keeps at most one frame in flight on the GPU. Without this the queue builds up and
// every Present waits for work submitted several frames earlier, which is felt as
// input latency. It also measures the real GPU time per frame.
static void gpu_throttle()
{
    if (!g_frame_fence) { return; }
    // Depth 1: wait for the previous frame's signal to retire before starting new work.
    const UINT64 target = g_frame_fence_value;
    if (target == 0 || g_frame_fence->GetCompletedValue() >= target) { return; }

    // No per-frame event: see wait_fence_value(). QPC, not GetTickCount64, because this
    // number is reported as `gpu` and feeds `load` - a 15.6 ms clock quantised it to
    // 0/16 ms and made both look like noise.
    const double t0 = qpc_ms();
    wait_fence_value(g_frame_fence.Get(), target, 200);
    g_ms_throttle = g_ms_throttle * 0.9 + (qpc_ms() - t0) * 0.1;
}

