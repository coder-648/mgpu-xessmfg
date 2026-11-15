// ============================================================================
//  consumer module: devices_capture_fg.cpp
//
//  Adapter/device creation, WGC capture, XeSS-FG init and --list-modes.
//
//  Included by src/xess_mfg_consumer/unity.cpp as part of the unity build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

#include "../consumer_internal.h"
// --------------------------------------------------------------- devices

struct AdapterPick
{
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1    desc{};
    const wchar_t*        why = L"none";
};

static const wchar_t* vendor_name(UINT vendor)
{
    switch (vendor) {
        case 0x1002: return L"AMD";
        case 0x10DE: return L"NVIDIA";
        case 0x8086: return L"Intel";
        case 0x1414: return L"Microsoft";
        default:     return L"unknown";
    }
}

static uint64_t pack_luid(const LUID& luid)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) |
           static_cast<uint32_t>(luid.LowPart);
}

static std::wstring lower_w(std::wstring s)
{
    for (wchar_t& c : s) { c = static_cast<wchar_t>(towlower(c)); }
    return s;
}

static bool all_digits(const std::wstring& s)
{
    if (s.empty()) { return false; }
    for (const wchar_t c : s) { if (c < L'0' || c > L'9') { return false; } }
    return true;
}

// One --gpu token -> "is this adapter it?". Everything is case-insensitive.
//   luid:HI:LO                      exact LUID (hex)
//   nv|nvidia / amd / intel / ms    vendor alias
//   10de / 1002 / 8086 / 0x10de     vendor id (hex)
//   0 / 1                           adapter index (EnumAdapters1 order)
//   6750 / 3080                     description substring
static bool adapter_matches(const DXGI_ADAPTER_DESC1& d, unsigned index, const std::wstring& spec)
{
    const std::wstring s = lower_w(spec);
    if (s.empty()) { return false; }

    if (s.rfind(L"luid:", 0) == 0) {
        unsigned long long hi = 0, lo = 0;
        if (swscanf_s(s.c_str() + 5, L"%llx:%llx", &hi, &lo) == 2) {
            return pack_luid(d.AdapterLuid) == ((hi << 32) | (lo & 0xFFFFFFFFull));
        }
        return false;
    }

    unsigned long vendor = 0;
    if (s == L"nv" || s == L"nvidia")         { vendor = 0x10DE; }
    else if (s == L"amd")                     { vendor = 0x1002; }
    else if (s == L"intel")                   { vendor = 0x8086; }
    else if (s == L"ms" || s == L"microsoft") { vendor = 0x1414; }
    else if (s.rfind(L"0x", 0) == 0)          { vendor = wcstoul(s.c_str() + 2, nullptr, 16); }
    else if (s == L"10de" || s == L"1002" || s == L"8086" || s == L"1414") {
        vendor = wcstoul(s.c_str(), nullptr, 16);
    }
    if (vendor != 0) { return d.VendorId == vendor; }

    if (all_digits(s)) {
        // an index, when it is a plausible one; otherwise fall through so
        // `--gpu 3080` still works as a description substring
        if (wcstoul(s.c_str(), nullptr, 10) == index) { return true; }
        if (s.size() <= 2) { return false; }
    }

    return lower_w(d.Description).find(s) != std::wstring::npos;
}

// Does this adapter drive that monitor? CreateSwapChainForHwnd returns
// E_ACCESSDENIED when it does not, so this is the one hard constraint.
static bool drives_monitor(IDXGIAdapter1* adapter, HMONITOR mon)
{
    if (!adapter || !mon) { return false; }
    for (unsigned j = 0;; ++j) {
        ComPtr<IDXGIOutput> out;
        if (adapter->EnumOutputs(j, &out) == DXGI_ERROR_NOT_FOUND) { break; }
        DXGI_OUTPUT_DESC od{};
        if (FAILED(out->GetDesc(&od))) { continue; }
        if (od.Monitor == mon) { return true; }
    }
    return false;
}

// Picks the GPU that runs XeSS-FG.
//
// Order: --gpu (explicit) > the adapter that drives the game window's monitor
// (the only one our overlay can present on) > "not the producer" > largest VRAM.
// Every adapter is logged, so "why did it pick that one" is answerable from the log
// alone - that was the expensive part of getting the old AMD-only check right.
static AdapterPick find_consumer_adapter(HWND game_wnd)
{
    AdapterPick pick;
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) { return pick; }

    const HMONITOR mon = overlay_monitor(game_wnd);
    logf_("[consumer] enumerating adapters (overlay monitor %p)\n",
          reinterpret_cast<void*>(mon));

    AdapterPick by_gpu, by_monitor, by_other, by_vram;
    for (unsigned i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) { break; }
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) { continue; }
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            logf_("[consumer]   %u: '%ls' is a software adapter - skipped\n", i, d.Description);
            continue;
        }

        unsigned outputs = 0;
        for (;; ++outputs) {
            ComPtr<IDXGIOutput> o;
            if (a->EnumOutputs(outputs, &o) == DXGI_ERROR_NOT_FOUND) { break; }
        }
        const bool producer = g_have_producer && pack_luid(d.AdapterLuid) == g_producer_luid;
        const bool on_mon   = drives_monitor(a.Get(), mon);
        logf_("[consumer]   %u: '%ls' %ls vendor=%04X device=%04X vram=%llu MB outputs=%u"
              " luid=%08lX:%08lX%s%s\n",
              i, d.Description, vendor_name(d.VendorId), d.VendorId, d.DeviceId,
              static_cast<unsigned long long>(d.DedicatedVideoMemory / (1024ull * 1024ull)),
              outputs,
              static_cast<unsigned long>(d.AdapterLuid.HighPart),
              static_cast<unsigned long>(d.AdapterLuid.LowPart),
              on_mon ? "  <== overlay monitor" : "", producer ? "  (producer)" : "");

        if (!g_gpu_spec.empty() && !by_gpu.adapter && adapter_matches(d, i, g_gpu_spec)) {
            by_gpu.adapter = a; by_gpu.desc = d; by_gpu.why = L"--gpu";
        }
        if (!by_monitor.adapter && on_mon) {
            by_monitor.adapter = a; by_monitor.desc = d;
            by_monitor.why = L"drives the overlay monitor";
        }
        if (!by_other.adapter && g_have_producer && !producer) {
            by_other.adapter = a; by_other.desc = d; by_other.why = L"not the producer";
        }
        if (!by_vram.adapter || d.DedicatedVideoMemory > by_vram.desc.DedicatedVideoMemory) {
            by_vram.adapter = a; by_vram.desc = d; by_vram.why = L"largest VRAM";
        }
    }

    if (!g_gpu_spec.empty() && !by_gpu.adapter) {
        logf_("[consumer] WARNING: --gpu %ls matched no adapter - falling back to the"
              " automatic pick\n", g_gpu_spec.c_str());
    }
    if (by_gpu.adapter)          { pick = by_gpu; }
    else if (by_monitor.adapter) { pick = by_monitor; }
    else if (by_other.adapter)   { pick = by_other; }
    else                         { pick = by_vram; }

    if (pick.adapter) {
        logf_("[consumer] consumer adapter: '%ls' (%ls vendor=%04X) - %ls\n",
              pick.desc.Description, vendor_name(pick.desc.VendorId), pick.desc.VendorId,
              pick.why);
        if (mon && !drives_monitor(pick.adapter.Get(), mon)) {
            logf_("[consumer] WARNING: this adapter does not drive the monitor the overlay is"
                  " on; CreateSwapChainForHwnd may fail with E_ACCESSDENIED - move the"
                  " overlay there with --overlay-monitor, or pick another --gpu\n");
        }
        if (g_have_producer && pack_luid(pick.desc.AdapterLuid) == g_producer_luid) {
            logf_("[consumer]   note: same GPU as the producer - frame generation competes"
                  " with the game for GPU time (use --gpu to choose the other card)\n");
        }
    }
    return pick;
}

static bool init_devices(IDXGIAdapter1* adapter)
{
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                 D3D11_SDK_VERSION, &g_d3d11, nullptr, &g_ctx11))) {
        logf_("[consumer] D3D11CreateDevice FAILED\n");
        return false;
    }
    if (FAILED(g_d3d11.As(&g_dev11_5)) || FAILED(g_ctx11.As(&g_ctx11_4))) {
        logf_("[consumer] ID3D11Device5/Context4 unavailable\n");
        return false;
    }
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device)))) {
        logf_("[consumer] D3D12CreateDevice FAILED\n");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue))) ||
        FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&g_alloc))) ||
        FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           g_alloc.Get(), nullptr, IID_PPV_ARGS(&g_list))) ||
        FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_frame_fence)))) {
        logf_("[consumer] D3D12 queue/list/fence FAILED\n");
        return false;
    }
    g_list->Close();
    g_frame_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_pub_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_frame_event || !g_pub_event) {
        logf_("[consumer] V2 event creation FAILED - falling back to timed polling\n");
        g_pub_event_ok = false;
        g_frame_event_ok = false;
    }

    if (FAILED(g_dev11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_fence11)))) {
        logf_("[consumer] ID3D11Fence FAILED\n");
        return false;
    }
    HANDLE fh = nullptr;
    if (FAILED(g_fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh))) {
        logf_("[consumer] fence CreateSharedHandle FAILED\n");
        return false;
    }
    const HRESULT hr = g_device->OpenSharedHandle(fh, IID_PPV_ARGS(&g_fence12));
    CloseHandle(fh);
    if (FAILED(hr)) { logf_("[consumer] fence OpenSharedHandle FAILED\n"); return false; }

    logf_("[consumer] devices ready (D3D11 + D3D12 on '%ls', %ls)\n",
          g_consumer_desc.Description, vendor_name(g_consumer_vendor));
    logf_("[consumer] startup: +%llu ms devices ready\n", startup_ms());
    return true;
}

// ------------------------------------------------------------------- colour
// v22: the colour is the producer's stream 0. There is no capture item, frame pool or
// session here any more; the sink texture is built from the stream's own format/size and
// filled by one cross-adapter CopyTextureRegion per publication.
// --------------------------------------------------------------- XeSS-FG

// --dyn-mfg needs to be visible to fg_apply_interp() below (it decides the start count).
static bool g_dyn_mfg = false;

// Runtime count changes go through xefgSwapChainSetNumInterpolatedFrames. Only a
// DLL that exports it can change the count after initialization; the official
// build already fixed the count with the init parameter, and on a non-Intel
// adapter it clamps that to 1 anyway.
//
// The XeSS-FG guide warns that the call "may be expensive (XeSS-FG may switch to a
// different model and trigger shader compilation)" and recommends it only as a
// response to a settings change. --interp-auto therefore changes it at most once
// per two report windows (~2.7 s) and only when the target really moved.
static void fg_set_interp(UINT want, const char* when, bool quiet = false)
{
    if (want < 1) { want = 1; }
    if (want > g_fg_interp_max) { want = g_fg_interp_max; }
    if (want == g_fg_interp_actual) { return; }

    if (!g_api.has_mfg()) {
        logf_("[consumer] cannot apply interp=%u %s: this DLL has no"
              " xefgSwapChainSetNumInterpolatedFrames\n", want, when);
        return;
    }

    const xefg_swapchain_result_t r = g_api.SetNumInterpolatedFrames(g_fg, want);
    if (!quiet) {
        logf_("[consumer] SetNumInterpolatedFrames(%u) %s: %s (%d)\n",
              want, when, xefg_str(r), static_cast<int>(r));
    }
    if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS) { g_fg_interp_actual = want; }
}

static void fg_apply_interp(const char* when)
{
    UINT want = (g_fg_interp > 0) ? static_cast<UINT>(g_fg_interp) : g_fg_interp_actual;
    // --dyn-mfg: --interp is the *ceiling* (the unlock patches it in), the start is 4x.
    // Without this the "after SetEnabled" call below overwrote the start with the
    // ceiling and the first run began at 6x (bridge_pbz_dynmfg.txt: gpu 47 ms, skip 67).
    if (g_dyn_mfg) { want = (g_fg_interp_max >= 3) ? 3u : g_fg_interp_max; }
    // --interp-cadence: init_fg() set the start to 2x; the post-SetEnabled call must not
    // overwrite it with the --interp ceiling (that was the first-frame 6x surge).
    if (g_interp_cadence) { want = g_fg_interp_actual; }
    fg_set_interp(want, when);
}

// ------------------------------------------------------------ --list-modes
// Without VRR the picture can only be perfectly even when the *input* rate is
// locked to an exact divisor of the panel refresh:
//     output fps = input fps * (interp + 1)  must divide the refresh rate
// so 120 Hz accepts 120 / 60 / 40 / 30 / 24 / 20 ... fps output. This prints the
// refresh rates the current output really offers, plus the exact input rate for
// each multiplier, so the right lock can be picked without guessing.
static int list_display_modes()
{
    DEVMODEW cur{};
    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur)) {
        logf_("[consumer] EnumDisplaySettingsW(current) failed\n");
        return 1;
    }
    logf_("[consumer] current mode: %lux%lu @ %lu Hz\n",
          static_cast<unsigned long>(cur.dmPelsWidth),
          static_cast<unsigned long>(cur.dmPelsHeight),
          static_cast<unsigned long>(cur.dmDisplayFrequency));

    UINT hz[64] = {};
    int n = 0;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i) {
        if (dm.dmPelsWidth != cur.dmPelsWidth || dm.dmPelsHeight != cur.dmPelsHeight) { continue; }
        bool dup = false;
        for (int k = 0; k < n; ++k) { if (hz[k] == dm.dmDisplayFrequency) { dup = true; break; } }
        if (!dup && n < 64) { hz[n++] = dm.dmDisplayFrequency; }
    }
    if (n == 0) { logf_("[consumer] no modes enumerated for this resolution\n"); return 1; }

    logf_("[consumer] refresh rates at %lux%lu:",
          static_cast<unsigned long>(cur.dmPelsWidth),
          static_cast<unsigned long>(cur.dmPelsHeight));
    for (int k = 0; k < n; ++k) { logf_(" %u", hz[k]); }
    logf_("\n");

    // Exact input rates per output multiplier (output = input * mul must divide Hz).
    for (int k = 0; k < n; ++k) {
        const double r = static_cast<double>(hz[k]);
        logf_("[consumer]   %3u Hz -> exact input for", hz[k]);
        for (int mul = 1; mul <= 6; ++mul) {
            logf_("  %dx@%.2f", mul, r / mul);
        }
        logf_("  (fps)\n");
    }
    logf_("[consumer] pick the highest exact input rate the game can always hold, lock RTSS"
          " to it, and set --interp so that input*(interp+1) == refresh.\n");
    return 0;
}

// The cadence helpers (median of the input intervals, refresh period, double-abs)
// are defined below; the solver needs them before their definitions.
static double arr_median_ms();
static double refresh_ms();
static double dabs_(double v);

// --lock N: enforce an input rate by dropping captured frames (see the frame loop).
// --lock-auto: solve the pair (rate, multiplier) from the refresh and the measured
// input capability, then enforce both. This is the general form of "the input must
// be an exact divisor of the refresh": for 120 Hz the exact options are
//   120/2 = 60 fps (2x), 120/3 = 40 (3x), 120/4 = 30 (4x), 120/5 = 24 (5x), 120/6 = 20 (6x)
// and the solver takes the highest one the game can actually sustain, preferring the
// shortest output period (1 vblank) and then the fewest generated frames.
static double    g_lock_fps = 0.0;
static bool      g_lock_auto = false;
static ULONGLONG g_lock_next = 0;
static UINT      g_lock_drops = 0;
static int       g_lock_pending_m = 0;
static double    g_lock_pending_fps = 0.0;
static UINT      g_lock_hits = 0;
static bool      g_lock_none_logged = false;

static void fg_solve_cadence(const char* why)
{
    if (!g_lock_auto || g_refresh_hz < 1) { return; }
    const double cap = 1000.0 / arr_median_ms();          // input fps the game sustains
    if (cap < 1.0) { return; }
    const int mmax = static_cast<int>(g_fg_interp_max) + 1;

    int best_m = 0, best_vb = 0;
    double best_lock = 0.0;
    for (int vb = 1; vb <= 4 && best_m == 0; ++vb) {
        for (int m = 2; m <= mmax; ++m) {
            const double lock = static_cast<double>(g_refresh_hz) / (static_cast<double>(m) * vb);
            if (lock > cap * 0.97) { continue; }          // the game cannot hold this
            // We can only *drop* captured frames, never hold or re-time them, so a
            // dropped frame turns one interval of T into two intervals of 2T. That is
            // only invisible when T is an exact multiple of the lock period, i.e.
            // when cap / lock is a whole number (dropping 1 of every 2 / 3 / 4 ...).
            // Anything else (26 -> 24 fps = drop 1 in 13) would show up as a 2T gap
            // every 13th frame - worse than the judder we are trying to remove.
            const double ratio = cap / lock;
            const double rfrac = dabs_(ratio - static_cast<double>(static_cast<int>(ratio + 0.5)));
            if (rfrac > 0.12) { continue; }
            best_m = m;
            best_vb = vb;
            best_lock = lock;
            break;                                        // fewest frames for this vb
        }
    }
    if (best_m == 0) {
        if (!g_lock_none_logged) {
            g_lock_none_logged = true;
            logf_("[consumer] --lock-auto: input %.1f fps at %u Hz has no exact cadence with"
                  " up to %dx - falling back to the unlocked rate\n", cap, g_refresh_hz, mmax);
        }
        return;
    }

    const int want_m = best_m;
    const bool same = (want_m == g_lock_pending_m);
    g_lock_pending_m = want_m;
    g_lock_pending_fps = best_lock;
    if (same) { if (++g_lock_hits < 2) { return; } } else { g_lock_hits = 1; return; }

    // Stable for two windows: apply. The lock takes effect immediately; the count
    // change costs one model switch (documented as expensive in the XeSS-FG guide).
    g_lock_hits = 0;
    if (dabs_(g_lock_fps - best_lock) > 0.05) {
        g_lock_fps = best_lock;
        g_lock_next = 0;                                  // restart the schedule
        logf_("[consumer] --lock-auto: input %.1f fps -> lock %.2f fps + %dx = %.1f fps"
              " (%.2f vblank/frame, %s)\n",
              cap, best_lock, want_m,
              best_lock * want_m,
              (best_lock * want_m > 0.0)
                  ? (1000.0 / (best_lock * want_m)) / refresh_ms() : 0.0,
              why);
    }
    fg_set_interp(static_cast<UINT>(want_m - 1), "lock-auto (exact cadence)");
}

