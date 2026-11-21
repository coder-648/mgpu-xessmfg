// ============================================================================
//  lock_frame module: 150_probe_mfg.inc
//
//  libxess_fg probe and the --unlock-mfg patch.
//
//  Included by lock_frame/lock_frame.cpp as part of the single-translation-unit build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

// MFG-Unlocker's ported-model route (ShaderPort + ModelPort + unlock without
// fg_keep_2x_model). Header-only; uses logf_ from 010_platform.inc.
#include "xefg_mfg/xefg_mfg_port.h"

// --mfg-model=port|keep2x. Default is keep2x: the ported model 15/16 is an
// explicit experiment, because on CP2077 4x its edge artifacts were worse than
// the 2x-model route (2x stayed clean, 4x_keep2x was visibly better). `port`
// still selects the full ShaderPort + ModelPort + keep2x=false path.
static int g_mfg_model = 2;   // 0 auto (legacy), 1 port, 2 keep2x (default)

static std::wstring mfg_program_dir()
{
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { return {}; }
    std::wstring p(path, n);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) { p.clear(); }
    else { p.resize(slash + 1); }
    p += L"xefg_programs";
    return p;
}

// Answers "which libxess_fg.dll is loaded, what version is it, and can it do MFG
// here?" without touching the game, the shared memory or a swap chain. It is the
// only way to tell a real MFG build from a file that merely has a different name.
static bool fg_probe()
{
    logf_("\n[lock] --- libxess_fg probe ---\n");
    logf_("[lock] probe: adapter    '%ls' %ls vendor=%04X device=%04X luid=%08lX:%08lX\n",
          g_consumer_desc.Description, vendor_name(g_consumer_vendor),
          g_consumer_desc.VendorId, g_consumer_desc.DeviceId,
          static_cast<unsigned long>(g_consumer_desc.AdapterLuid.HighPart),
          static_cast<unsigned long>(g_consumer_desc.AdapterLuid.LowPart));
    if (!g_api.ok()) {
        logf_("[lock] probe: no DLL loaded\n");
        return false;
    }
    logf_("[lock] probe: dll        %s\n", narrow_utf8(g_api.path).c_str());
    logf_("[lock] probe: MFG export %s\n", g_api.has_mfg() ? "yes" : "NO");

    xefg_swapchain_version_t v{};
    if (g_api.GetVersion && g_api.GetVersion(&v) == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        logf_("[lock] probe: version    %u.%u.%u\n", v.major, v.minor, v.patch);
    } else {
        logf_("[lock] probe: version    (xefgSwapChainGetVersion failed)\n");
    }

    xefg_swapchain_handle_t ctx = nullptr;
    const xefg_swapchain_result_t cr = g_api.D3D12CreateContext(g_device.Get(), &ctx);
    logf_("[lock] probe: CreateContext %s (%d)\n", xefg_str(cr), static_cast<int>(cr));
    if (cr != XEFG_SWAPCHAIN_RESULT_SUCCESS || !ctx) { return false; }

    xefg_swapchain_properties_t props{};
    const xefg_swapchain_result_t pr = g_api.GetProperties(ctx, &props);
    logf_("[lock] probe: GetProperties %s (%d)\n", xefg_str(pr), static_cast<int>(pr));
    if (g_api.Destroy) { g_api.Destroy(ctx); }
    if (pr != XEFG_SWAPCHAIN_RESULT_SUCCESS) { return false; }

    const UINT mx = props.maxSupportedInterpolations ? props.maxSupportedInterpolations : 1;
    logf_("[lock] probe: maxSupportedInterpolations %u  ->  %ux output\n", mx, mx + 1);

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
            logf_("[lock] probe: D3D12GetProperties(5K, 10bit) %s (%d)\n",
                  xefg_str(r2), static_cast<int>(r2));
            if (r2 == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                logf_("[lock] probe: descriptors=%u tempBuffer=%.1f MB tempTexture=%.1f MB"
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
        logf_("[lock] probe: VERDICT 2x only - this DLL clamps MFG on this GPU"
              " (vendor check / model / XeLL).\n");
        logf_("[lock] probe:   point --fg-lib at an MFG-capable libxess_fg.dll"
              " (+ matching libxell.dll)\n");
    } else {
        logf_("[lock] probe: VERDICT MFG available, up to %ux (--interp 2..%u = 3x..%ux).\n",
              mx + 1, mx, mx + 1);
        // Exact-cadence reminder: the multiplier is what makes an unlocked refresh
        // reachable without VRR (e.g. 120 Hz = 30x4, 24x5, 20x6).
        logf_("[lock] probe: at %ux the input rate must be refresh/%u for a whole-vblank"
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
    logf_("[lock] --unlock-mfg: NOTE - this modifies libxess_fg/libxell machine code in"
          " memory. Personal research only; it may violate the vendor EULA. See"
          " docs/ops/LEGAL.md. The DLLs are not distributed with this project.\n");

    if (!g_api.ok()) {
        logf_("[lock] --unlock-mfg: no libxess_fg loaded - nothing to patch\n");
        return false;
    }

    if (g_consumer_vendor == 0x8086) {
        logf_("[lock] --unlock-mfg: consumer adapter is Intel ('%ls') - NOT patching."
              " Its MFG model exists, so this DLL should already allow more than one"
              " interpolated frame\n", g_consumer_desc.Description);
        return true;    // not an error: the native path is the better one
    }

    // --interp 1 asks for 2x, which the stock clamp already allows: nothing to lift.
    // (Apply() only accepts 2..5, so asking it for 1 would fail with "interp out of
    // range" and - worse - leave the caller thinking 4x was applied.)
    const unsigned want = g_fg_interp_set ? static_cast<unsigned>(g_fg_interp) : 3u;
    if (want < 2) {
        logf_("[lock] --unlock-mfg: %ux requested - the DLL already allows 1"
              " interpolated frame, nothing to patch\n", want + 1);
        return true;
    }

    if (g_fg_probe && g_mfg_model == 2) {
        // Only on the explicit keep2x route: creating a context here would make
        // ModelPort's "install before the first context" precondition false.
        const UINT stock = read_max_interp();
        if (stock > 0) {
            logf_("[lock] probe: maxSupportedInterpolations (stock) %u -> %ux\n",
                  stock, stock + 1);
        }
    } else if (g_fg_probe) {
        logf_("[lock] probe: stock maxSupportedInterpolations skipped (ported-model"
              " route needs no prior context)\n");
    }

    // libxell.dll is imported by this exe, so it is already loaded here; libxess_fg
    // loads it by name later and gets the same module.
    HMODULE ll = GetModuleHandleW(L"libxell.dll");
    wchar_t ll_path[MAX_PATH]{};
    if (ll) { GetModuleFileNameW(ll, ll_path, MAX_PATH); }
    logf_("[lock] --unlock-mfg: target %ux (%u interpolated), libxell=%s\n",
          want + 1, want, ll ? narrow_utf8(ll_path).c_str() : "(not loaded)");

    std::string uerr;
    // Recommended route: MFG-Unlocker's ported model (ShaderPort + ModelPort +
    // frame-count unlock WITHOUT fg_keep_2x_model). The model 15 that already
    // lives in libxess_fg is relabelled as model 16, so XeFG's MFG path runs the
    // real multi-frame model instead of reusing the 2x model.
    if (g_mfg_model != 2) {
        const std::wstring dir = mfg_program_dir();
        xefg_mfg::SetDataRoot(std::filesystem::path(dir));
        std::string perr;
        if (xefg_mfg::Install(g_api.dll, g_consumer_vendor, &perr, g_mfg_replace_all)) {
            if (xefg_unlock::Apply(g_api.dll, ll, want, &uerr, false)) {
                g_fg_interp = static_cast<int>(want);
                g_fg_interp_max = want;
                logf_("[lock] --unlock-mfg: ported MFG model route active"
                      " (model 15 -> 16, 5 sites, no fg_keep_2x_model)\n");
                logf_("[lock]   programs: %ls\n", dir.c_str());
                return true;
            }
            logf_("[lock] --unlock-mfg: port installed but the unlock failed: %s\n",
                  uerr.c_str());
            xefg_mfg::Restore();
        } else {
            logf_("[lock] --unlock-mfg: ported MFG model not used: %s"
                  " -> falling back to the 2x-model route\n", perr.c_str());
        }
    }

    // Legacy route: keep fg_keep_2x_model. Every generated frame reuses the 2x
    // model; this is the behaviour shipped before the port existed.
    if (xefg_unlock::Apply(g_api.dll, ll, want, &uerr, true)) {
        g_fg_interp = static_cast<int>(want);
        g_fg_interp_max = want;
        logf_("[lock] --unlock-mfg: 2x-model route active (fg_keep_2x_model kept)\n");
        logf_("[lock]   every generated frame uses the 2x model, so quality is not"
              " true-MFG quality\n");
        return true;
    }

    logf_("[lock] --unlock-mfg FAILED: %s\n", uerr.c_str());
    logf_("[lock]   the patch table is version specific (expects the"
          " libxess_fg 1.3.1.78/1.3.4 family + libxell 1.3.2.x)\n");
    return false;
}

// --------------------------------------------------------------------- main

