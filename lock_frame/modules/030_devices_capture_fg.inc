// ============================================================================
//  lock_frame module: 030_devices_capture_fg.inc
//
//  Adapter/device creation, WGC capture, XeSS-FG init and --list-modes.
//
//  Included by lock_frame/lock_frame.cpp as part of the single-translation-unit build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

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
    logf_("[lock] enumerating adapters (overlay monitor %p)\n",
          reinterpret_cast<void*>(mon));

    AdapterPick by_gpu, by_monitor, by_other, by_vram;
    for (unsigned i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) { break; }
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) { continue; }
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            logf_("[lock]   %u: '%ls' is a software adapter - skipped\n", i, d.Description);
            continue;
        }

        unsigned outputs = 0;
        for (;; ++outputs) {
            ComPtr<IDXGIOutput> o;
            if (a->EnumOutputs(outputs, &o) == DXGI_ERROR_NOT_FOUND) { break; }
        }
        const bool producer = g_have_producer && pack_luid(d.AdapterLuid) == g_producer_luid;
        const bool on_mon   = drives_monitor(a.Get(), mon);
        logf_("[lock]   %u: '%ls' %ls vendor=%04X device=%04X vram=%llu MB outputs=%u"
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
        logf_("[lock] WARNING: --gpu %ls matched no adapter - falling back to the"
              " automatic pick\n", g_gpu_spec.c_str());
    }
    if (by_gpu.adapter)          { pick = by_gpu; }
    else if (by_monitor.adapter) { pick = by_monitor; }
    else if (by_other.adapter)   { pick = by_other; }
    else                         { pick = by_vram; }

    if (pick.adapter) {
        logf_("[lock] consumer adapter: '%ls' (%ls vendor=%04X) - %ls\n",
              pick.desc.Description, vendor_name(pick.desc.VendorId), pick.desc.VendorId,
              pick.why);
        if (mon && !drives_monitor(pick.adapter.Get(), mon)) {
            logf_("[lock] WARNING: this adapter does not drive the monitor the overlay is"
                  " on; CreateSwapChainForHwnd may fail with E_ACCESSDENIED - move the"
                  " overlay there with --overlay-monitor, or pick another --gpu\n");
        }
        if (g_have_producer && pack_luid(pick.desc.AdapterLuid) == g_producer_luid) {
            logf_("[lock]   note: same GPU as the producer - frame generation competes"
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
        logf_("[lock] D3D11CreateDevice FAILED\n");
        return false;
    }
    if (FAILED(g_d3d11.As(&g_dev11_5)) || FAILED(g_ctx11.As(&g_ctx11_4))) {
        logf_("[lock] ID3D11Device5/Context4 unavailable\n");
        return false;
    }
    // --d3d-debug: turn on the D3D12 debug layer for OUR device and keep an InfoQueue, so an
    // invalid call on the shared (cross-adapter) resources prints its real message instead
    // of only "device removed". Needs the Graphics Tools feature; a failure is not fatal.
    if (g_d3d_debug) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            logf_("[lock] --d3d-debug: D3D12 debug layer enabled\n");
        } else {
            logf_("[lock] --d3d-debug: D3D12GetDebugInterface FAILED (Graphics Tools not"
                  " installed?) - continuing without validation\n");
        }
    }
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device)))) {
        logf_("[lock] D3D12CreateDevice FAILED\n");
        return false;
    }
    if (g_d3d_debug) {
        if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&g_info_q)))) {
            logf_("[lock] --d3d-debug: InfoQueue attached (messages go to this log)\n");
        }
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue))) ||
        FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&g_alloc))) ||
        FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           g_alloc.Get(), nullptr, IID_PPV_ARGS(&g_list))) ||
        FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_frame_fence)))) {
        logf_("[lock] D3D12 queue/list/fence FAILED\n");
        return false;
    }
    g_list->Close();
    g_frame_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (FAILED(g_dev11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_fence11)))) {
        logf_("[lock] ID3D11Fence FAILED\n");
        return false;
    }
    HANDLE fh = nullptr;
    if (FAILED(g_fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh))) {
        logf_("[lock] fence CreateSharedHandle FAILED\n");
        return false;
    }
    const HRESULT hr = g_device->OpenSharedHandle(fh, IID_PPV_ARGS(&g_fence12));
    CloseHandle(fh);
    if (FAILED(hr)) { logf_("[lock] fence OpenSharedHandle FAILED\n"); return false; }

    logf_("[lock] devices ready (D3D11 + D3D12 on '%ls', %ls)\n",
          g_consumer_desc.Description, vendor_name(g_consumer_vendor));
    return true;
}

// ------------------------------------------------------------------- WGC

static ComPtr<IDirect3D11CaptureFramePool> g_pool;
static ComPtr<IGraphicsCaptureSession>     g_session;
static ComPtr<IGraphicsCaptureItem>        g_capture_item;
static SizeInt32 g_item_size{};
static SizeInt32 g_pool_size{};

// The add-on's shared block still advertises a colour stream at a fixed 5120x2880 (and
// with `Color=0` does not even allocate it), so that size is a placeholder, not the game's
// resolution. What WGC hands us is the real window content size, and everything built from
// it - colour sink, frame pool, FG swap chain, the resize ratio - has to agree with it,
// otherwise a 4K window gets a 5K picture stretched over it.
static void adopt_capture_size()
{
    if (g_item_size.Width <= 0 || g_item_size.Height <= 0) { return; }
    const UINT w = static_cast<UINT>(g_item_size.Width);
    const UINT h = static_cast<UINT>(g_item_size.Height);
    if (w == g_width && h == g_height) { return; }
    logf_("[lock] capture size taken from the window: %ux%u (shared block declared %ux%u)\n",
          w, h, g_width, g_height);
    g_width  = w;
    g_height = h;
    g_full_w = w;
    g_full_h = h;
}

static bool create_capture_item(HWND game_wnd)
{
    ComPtr<IGraphicsCaptureItemInterop> interop;
    {
        const wchar_t* kClass = L"Windows.Graphics.Capture.GraphicsCaptureItem";
        HSTRING hs = nullptr;
        WindowsCreateString(kClass, static_cast<UINT32>(wcslen(kClass)), &hs);
        const HRESULT h = RoGetActivationFactory(hs, IID_PPV_ARGS(&interop));
        WindowsDeleteString(hs);
        if (FAILED(h)) { logf_("[lock] capture item activation FAILED\n"); return false; }
    }

    // --capture-monitor: capture the display instead of the window. What the panel shows is
    // what we get, so any window/swap-chain height mismatch (which showed up as a one-line
    // shift at the bottom edge) cannot happen.
    if (g_capture_monitor) {
        const HMONITOR mon = MonitorFromWindow(game_wnd, MONITOR_DEFAULTTOPRIMARY);
        if (mon && SUCCEEDED(interop->CreateForMonitor(mon, __uuidof(IGraphicsCaptureItem),
                                                      reinterpret_cast<void**>(
                                                          g_capture_item.GetAddressOf())))) {
            g_capture_item->get_Size(&g_item_size);
            logf_("[lock] capture item (monitor) %dx%d\n", g_item_size.Width, g_item_size.Height);
            return true;
        }
        logf_("[lock] CreateForMonitor FAILED - falling back to the window\n");
    }

    if (FAILED(interop->CreateForWindow(game_wnd, __uuidof(IGraphicsCaptureItem),
                                        reinterpret_cast<void**>(g_capture_item.GetAddressOf())))) {
        logf_("[lock] CreateForWindow FAILED (exclusive fullscreen?)\n");
        return false;
    }
    g_capture_item->get_Size(&g_item_size);
    logf_("[lock] capture item %dx%d\n", g_item_size.Width, g_item_size.Height);
    return true;
}

static bool start_wgc()
{
    // Pool at the captured size: asking WGC for a smaller pool crops instead of scaling,
    // so the resize happens in our own pass, where we control the filter.
    g_pool_size.Width  = static_cast<INT32>(g_width);
    g_pool_size.Height = static_cast<INT32>(g_height);
    logf_("[lock] capture pool %dx%d\n", g_pool_size.Width, g_pool_size.Height);

    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(g_d3d11.As(&dxgi_device))) { return false; }
    ComPtr<IInspectable> inspectable;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), &inspectable))) {
        logf_("[lock] CreateDirect3D11DeviceFromDXGIDevice FAILED\n");
        return false;
    }
    ComPtr<IDirect3DDevice> rt_device;
    if (FAILED(inspectable.As(&rt_device))) { return false; }

    ComPtr<IDirect3D11CaptureFramePoolStatics2> statics;
    {
        const wchar_t* kClass = L"Windows.Graphics.Capture.Direct3D11CaptureFramePool";
        HSTRING hs = nullptr;
        WindowsCreateString(kClass, static_cast<UINT32>(wcslen(kClass)), &hs);
        const HRESULT h = RoGetActivationFactory(hs, IID_PPV_ARGS(&statics));
        WindowsDeleteString(hs);
        if (FAILED(h)) { logf_("[lock] frame pool activation FAILED\n"); return false; }
    }

    if (FAILED(statics->CreateFreeThreaded(
            rt_device.Get(),
            ABI::Windows::Graphics::DirectX::DirectXPixelFormat_R16G16B16A16Float,
            g_wgc_pool, g_pool_size, &g_pool))) {
        logf_("[lock] CreateFreeThreaded FAILED\n");
        return false;
    }

    if (FAILED(g_pool->CreateCaptureSession(g_capture_item.Get(), &g_session))) {
        logf_("[lock] CreateCaptureSession FAILED\n");
        return false;
    }
    ComPtr<IGraphicsCaptureSession2> session2;
    if (SUCCEEDED(g_session.As(&session2))) { session2->put_IsCursorCaptureEnabled(FALSE); }

    // WGC can add a capture border and secondary windows (Win11 22621+). Both cost work
    // and neither belongs in the colour XeFG receives, so turn them off when available.
    ComPtr<IGraphicsCaptureSession3> session3;
    if (SUCCEEDED(g_session.As(&session3))) {
        const HRESULT hr = session3->put_IsBorderRequired(FALSE);
        logf_("[lock] WGC border capture = off (%s)\n",
              SUCCEEDED(hr) ? "accepted" : "ignored by this OS");
    }
    ComPtr<IGraphicsCaptureSession6> session6;
    if (SUCCEEDED(g_session.As(&session6))) {
        const HRESULT hr = session6->put_IncludeSecondaryWindows(FALSE);
        logf_("[lock] WGC secondary windows = off (%s)\n",
              SUCCEEDED(hr) ? "accepted" : "ignored by this OS");
    }


    // ---- WGC delivery throttle (Windows 11, SDK 22621+) --------------------------------
    // IGraphicsCaptureSession5::MinUpdateInterval defaults to one compositor interval
    // (~16.6 ms), i.e. WGC hands over at most ~60 frames/s no matter how fast the game
    // presents - the "WGC has a 16 ms floor" that users report, and the reason a title running
    // at 90+ fps can still look like 60 through this path. Setting it to 0 removes the
    // throttle; it costs more capture work, so it stays a switch (`--wgc-minint 0`) until
    // measured. Cases: -1 = leave Windows' default (log it), 0 = no throttle, N = N ms.
    {
        ComPtr<IGraphicsCaptureSession5> session5;
        if (SUCCEEDED(g_session.As(&session5))) {
            if (g_wgc_minint_ms >= 0) {
                ABI::Windows::Foundation::TimeSpan ts{};
                ts.Duration = static_cast<LONGLONG>(g_wgc_minint_ms) * 10000;   // 100 ns ticks
                const HRESULT hr = session5->put_MinUpdateInterval(ts);
                logf_("[lock] WGC MinUpdateInterval = %d ms (%s)\n", g_wgc_minint_ms,
                      SUCCEEDED(hr) ? "accepted" : "REJECTED - the driver/OS ignored it");
            } else {
                ABI::Windows::Foundation::TimeSpan cur{};
                if (SUCCEEDED(session5->get_MinUpdateInterval(&cur)) && cur.Duration > 0) {
                    logf_("[lock] WGC MinUpdateInterval = %.2f ms (Windows default; %.0f fps cap"
                          " - try --wgc-minint 0)\n", cur.Duration / 10000.0,
                          10000000.0 / static_cast<double>(cur.Duration));
                } else {
                    logf_("[lock] WGC MinUpdateInterval = 0 (no throttle)\n");
                }
            }
        } else {
            logf_("[lock] WGC session has no MinUpdateInterval (Windows 10) - delivery is"
                  " capped by the compositor\n");
        }
    }
    if (FAILED(g_session->StartCapture())) {
        logf_("[lock] StartCapture FAILED\n");
        return false;
    }
    logf_("[lock] WGC running (FP16 scRGB, free-threaded)\n");
    return true;
}

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
        logf_("[lock] cannot apply interp=%u %s: this DLL has no"
              " xefgSwapChainSetNumInterpolatedFrames\n", want, when);
        return;
    }

    const xefg_swapchain_result_t r = g_api.SetNumInterpolatedFrames(g_fg, want);
    if (!quiet) {
        logf_("[lock] SetNumInterpolatedFrames(%u) %s: %s (%d)\n",
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
        logf_("[lock] EnumDisplaySettingsW(current) failed\n");
        return 1;
    }
    logf_("[lock] current mode: %lux%lu @ %lu Hz\n",
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
    if (n == 0) { logf_("[lock] no modes enumerated for this resolution\n"); return 1; }

    logf_("[lock] refresh rates at %lux%lu:",
          static_cast<unsigned long>(cur.dmPelsWidth),
          static_cast<unsigned long>(cur.dmPelsHeight));
    for (int k = 0; k < n; ++k) { logf_(" %u", hz[k]); }
    logf_("\n");

    // Exact input rates per output multiplier (output = input * mul must divide Hz).
    for (int k = 0; k < n; ++k) {
        const double r = static_cast<double>(hz[k]);
        logf_("[lock]   %3u Hz -> exact input for", hz[k]);
        for (int mul = 1; mul <= 6; ++mul) {
            logf_("  %dx@%.2f", mul, r / mul);
        }
        logf_("  (fps)\n");
    }
    logf_("[lock] pick the highest exact input rate the game can always hold, lock RTSS"
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
            logf_("[lock] --lock-auto: input %.1f fps at %u Hz has no exact cadence with"
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
        logf_("[lock] --lock-auto: input %.1f fps -> lock %.2f fps + %dx = %.1f fps"
              " (%.2f vblank/frame, %s)\n",
              cap, best_lock, want_m,
              best_lock * want_m,
              (best_lock * want_m > 0.0)
                  ? (1000.0 / (best_lock * want_m)) / refresh_ms() : 0.0,
              why);
    }
    fg_set_interp(static_cast<UINT>(want_m - 1), "lock-auto (exact cadence)");
}

