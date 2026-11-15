// ============================================================================
//  consumer module: probe_mfg.cpp
//
//  libxess_fg probe and the --unlock-mfg patch.
//
//  Included by src/xess_mfg_consumer/unity.cpp as part of the unity build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

#include "../consumer_internal.h"
// Answers "which libxess_fg.dll is loaded, what version is it, and can it do MFG
// here?" without touching the game, the shared memory or a swap chain. It is the
// only way to tell a real MFG build from a file that merely has a different name.
static bool fg_probe()
{
    logf_("\n[consumer] --- libxess_fg probe ---\n");
    logf_("[consumer] probe: adapter    '%ls' %ls vendor=%04X device=%04X luid=%08lX:%08lX\n",
          g_consumer_desc.Description, vendor_name(g_consumer_vendor),
          g_consumer_desc.VendorId, g_consumer_desc.DeviceId,
          static_cast<unsigned long>(g_consumer_desc.AdapterLuid.HighPart),
          static_cast<unsigned long>(g_consumer_desc.AdapterLuid.LowPart));
    if (!g_api.ok()) {
        logf_("[consumer] probe: no DLL loaded\n");
        return false;
    }
    logf_("[consumer] probe: dll        %s\n", narrow_utf8(g_api.path).c_str());
    logf_("[consumer] probe: MFG export %s\n", g_api.has_mfg() ? "yes" : "NO");

    xefg_swapchain_version_t v{};
    if (g_api.GetVersion && g_api.GetVersion(&v) == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        logf_("[consumer] probe: version    %u.%u.%u\n", v.major, v.minor, v.patch);
    } else {
        logf_("[consumer] probe: version    (xefgSwapChainGetVersion failed)\n");
    }

    xefg_swapchain_handle_t ctx = nullptr;
    const xefg_swapchain_result_t cr = g_api.D3D12CreateContext(g_device.Get(), &ctx);
    logf_("[consumer] probe: CreateContext %s (%d)\n", xefg_str(cr), static_cast<int>(cr));
    if (cr != XEFG_SWAPCHAIN_RESULT_SUCCESS || !ctx) { return false; }

    xefg_swapchain_properties_t props{};
    const xefg_swapchain_result_t pr = g_api.GetProperties(ctx, &props);
    logf_("[consumer] probe: GetProperties %s (%d)\n", xefg_str(pr), static_cast<int>(pr));
    if (g_api.Destroy) { g_api.Destroy(ctx); }
    if (pr != XEFG_SWAPCHAIN_RESULT_SUCCESS) { return false; }

    const UINT mx = props.maxSupportedInterpolations ? props.maxSupportedInterpolations : 1;
    logf_("[consumer] probe: maxSupportedInterpolations %u  ->  %ux output\n", mx, mx + 1);

    // xefgSwapChainGetProperties only reports the count before initialization; the
    // heap/descriptor cost comes from the D3D12 variant, which takes the target
    // description. Ask it for the real number so the memory cost of MFG is known
    // up front.
    if (g_api.D3D12GetProperties) {
        xefg_swapchain_handle_t ctx2 = nullptr;
        if (g_api.D3D12CreateContext(g_device.Get(), &ctx2) == XEFG_SWAPCHAIN_RESULT_SUCCESS && ctx2) {
            xefg_swapchain_d3d12_init_params_t ip{};
            ip.maxInterpolatedFrames = mx;
            ip.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_NONE;
            ip.uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO;
            ip.creationNodeMask = 1;
            ip.visibleNodeMask  = 1;
            xefg_swapchain_properties_t p2{};
            const xefg_swapchain_result_t r2 =
                g_api.D3D12GetProperties(ctx2, &ip, 5120, 2880, DXGI_FORMAT_R10G10B10A2_UNORM, &p2);
            logf_("[consumer] probe: D3D12GetProperties(5K, 10bit) %s (%d)\n",
                  xefg_str(r2), static_cast<int>(r2));
            if (r2 == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                logf_("[consumer] probe: descriptors=%u tempBuffer=%.1f MB tempTexture=%.1f MB"
                      " constBuffer=%.1f MB\n",
                      p2.requiredDescriptorCount,
                      p2.tempBufferHeapSize / (1024.0 * 1024.0),
                      p2.tempTextureHeapSize / (1024.0 * 1024.0),
                      p2.constantBufferSize / (1024.0 * 1024.0));
            }
            if (g_api.Destroy) { g_api.Destroy(ctx2); }
        }
    }

    if (mx <= 1) {
        logf_("[consumer] probe: VERDICT 2x only - this DLL clamps MFG on this GPU"
              " (vendor check / model / XeLL).\n");
        logf_("[consumer] probe:   point --fg-lib at an MFG-capable libxess_fg.dll"
              " (+ matching libxell.dll)\n");
    } else {
        logf_("[consumer] probe: VERDICT MFG available, up to %ux (--interp 2..%u = 3x..%ux).\n",
              mx + 1, mx, mx + 1);
        // Exact-cadence reminder: the multiplier is what makes an unlocked refresh
        // reachable without VRR (e.g. 120 Hz = 30x4, 24x5, 20x6).
        logf_("[consumer] probe: at %ux the input rate must be refresh/%u for a whole-vblank"
              " cadence (use --list-modes to see the refresh rates)\n", mx + 1, mx + 1);
    }
    return true;
}


// ------------------------------------------------------------------ MFG patch

// maxSupportedInterpolations as reported *before* any patch (0 = unknown). Only
// used by --fg-probe so the log can show the stock value next to the patched one.
static UINT read_max_interp()
{
    if (!g_api.ok() || !g_device || !g_api.D3D12CreateContext || !g_api.GetProperties) {
        return 0;
    }
    xefg_swapchain_handle_t ctx = nullptr;
    if (g_api.D3D12CreateContext(g_device.Get(), &ctx) != XEFG_SWAPCHAIN_RESULT_SUCCESS || !ctx) {
        return 0;
    }
    xefg_swapchain_properties_t props{};
    const UINT mx = (g_api.GetProperties(ctx, &props) == XEFG_SWAPCHAIN_RESULT_SUCCESS &&
                     props.maxSupportedInterpolations)
                        ? props.maxSupportedInterpolations : 0;
    if (g_api.Destroy) { g_api.Destroy(ctx); }
    return mx;
}

// Lifts the "non-Intel adapter => 1 interpolated frame" clamp (src/xefg_unlock.h,
// table from Universal MFG Unlocker, MIT). The count comes from --interp
// (default 3 = 4x).
//
// Why this is not unconditional any more: the clamp only exists for non-Intel
// adapters, so on an Intel GPU the stock value is already >= 1 *and* the MFG model
// is really there. Patching there would swap real multi-frame generation for
// repeated 2x frames (docs/findings/MFG.md 1). The vendor is known only after the consumer
// adapter is picked, which is why this runs from main() rather than at DLL load.
//
// Returns false only when a requested patch failed (caller decides whether to abort).
static bool apply_unlock_mfg()
{
    // Say it out loud, not only in docs/ops/LEGAL.md: this flag rewrites code inside a
    // commercial DLL while it is loaded. Research use only.
    logf_("[consumer] --unlock-mfg: NOTE - this modifies libxess_fg/libxell machine code in"
          " memory. Personal research only; it may violate the vendor EULA. See"
          " docs/ops/LEGAL.md. The DLLs are not distributed with this project.\n");

    if (!g_api.ok()) {
        logf_("[consumer] --unlock-mfg: no libxess_fg loaded - nothing to patch\n");
        return false;
    }

    if (g_consumer_vendor == 0x8086) {
        logf_("[consumer] --unlock-mfg: consumer adapter is Intel ('%ls') - NOT patching."
              " Its MFG model exists, so this DLL should already allow more than one"
              " interpolated frame\n", g_consumer_desc.Description);
        return true;    // not an error: the native path is the better one
    }

    // --interp 1 asks for 2x, which the stock clamp already allows: nothing to lift.
    // (Apply() only accepts 2..5, so asking it for 1 would fail with "interp out of
    // range" and - worse - leave the caller thinking 4x was applied.)
    const unsigned want = g_fg_interp_set ? static_cast<unsigned>(g_fg_interp) : 3u;
    if (want < 2) {
        logf_("[consumer] --unlock-mfg: %ux requested - the DLL already allows 1"
              " interpolated frame, nothing to patch\n", want + 1);
        return true;
    }

    if (g_fg_probe) {
        const UINT stock = read_max_interp();
        if (stock > 0) {
            logf_("[consumer] probe: maxSupportedInterpolations (stock) %u -> %ux\n",
                  stock, stock + 1);
        }
    }

    // libxell.dll is imported by this exe, so it is already loaded here; libxess_fg
    // loads it by name later and gets the same module.
    HMODULE ll = GetModuleHandleW(L"libxell.dll");
    wchar_t ll_path[MAX_PATH]{};
    if (ll) { GetModuleFileNameW(ll, ll_path, MAX_PATH); }
    logf_("[consumer] --unlock-mfg: target %ux (%u interpolated), libxell=%s\n",
          want + 1, want, ll ? narrow_utf8(ll_path).c_str() : "(not loaded)");

    std::string uerr;
    if (xefg_unlock::Apply(g_api.dll, ll, want, &uerr)) {
        g_fg_interp = static_cast<int>(want);
        g_fg_interp_max = want;
        logf_("[consumer] --unlock-mfg: all 6 sites patched and read back OK\n");
        logf_("[consumer]   every generated frame uses the 2x model (the MFG model is"
              " not in this build), so quality is not 2x quality\n");
        return true;
    }

    logf_("[consumer] --unlock-mfg FAILED: %s\n", uerr.c_str());
    logf_("[consumer]   the patch table is version specific (expects the"
          " libxess_fg 1.3.1.78/1.3.4 family + libxell 1.3.2.x)\n");
    return false;
}

// --------------------------------------------------------------------- main

