// ============================================================================
//  lock_frame module: 020_state_overlay.inc
//
//  Shared state, command-line switches, producer hints, overlay
//  window/placement helpers.
//
//  Included by lock_frame/lock_frame.cpp as part of the single-translation-unit build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

// ------------------------------------------------------------ shared state

static ComPtr<ID3D11Device>         g_d3d11;
static ComPtr<ID3D11DeviceContext>  g_ctx11;
static ComPtr<ID3D11Device5>        g_dev11_5;
static ComPtr<ID3D11DeviceContext4> g_ctx11_4;
static ComPtr<ID3D11Fence>          g_fence11;
static ComPtr<ID3D12Fence>          g_fence12;

static ComPtr<ID3D12Device>              g_device;
static ComPtr<ID3D12CommandQueue>        g_queue;
static ComPtr<ID3D12CommandAllocator>    g_alloc;
static ComPtr<ID3D12GraphicsCommandList> g_list;
static ComPtr<ID3D12Fence>               g_frame_fence;
static HANDLE                            g_frame_event = nullptr;
static UINT64                            g_frame_fence_value = 0;

static ComPtr<ID3D11Texture2D> g_shared11;    // WGC frame sink (D3D11)
static ComPtr<ID3D12Resource>  g_shared12;    // the same memory, seen by D3D12

static xefg_swapchain_handle_t g_fg = nullptr;
static xell_context_handle_t   g_xell = nullptr;
static ComPtr<IDXGISwapChain4> g_swapchain;

// The application swap chain handed to XeSS-FG must outlive the call that passes
// it: it used to be a block-local ComPtr, so our only reference died right after
// InitFromSwapChain returned and the process died with it.
static ComPtr<IDXGISwapChain1> g_app_swap;

// Composition fallback: kept alive for the whole run, otherwise the swap chain
// stops being presented once these go out of scope.
static ComPtr<IDCompositionDevice> g_dcomp;
static ComPtr<IDCompositionTarget> g_dcomp_target;
static ComPtr<IDCompositionVisual> g_dcomp_visual;

static HWND g_bridge_wnd = nullptr;
// The game window we are mirroring. Focus has to stay with it: the overlay is
// click-through, but keyboard input still goes to whatever window owns the
// foreground.
static HWND g_game_wnd = nullptr;
// assert_topmost() puts the game window directly underneath our WM_EX_TOPMOST
// overlay, and Windows promotes any window inserted above a topmost one - so the
// game itself becomes topmost and *stays* topmost after we die. That is the
// "退出 FG 后游戏一直贴在桌面最上层" report. Every exit path calls
// restore_game_zorder() to undo it.
static void restore_game_zorder(const char* why)
{
    if (!g_game_wnd || !IsWindow(g_game_wnd)) { return; }
    const LONG_PTR ex = GetWindowLongPtrW(g_game_wnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOPMOST) == 0) { return; }
    SetWindowLongPtrW(g_game_wnd, GWL_EXSTYLE,
                      ex & ~static_cast<LONG_PTR>(WS_EX_TOPMOST));
    SetWindowPos(g_game_wnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    logf_("[lock] %s: cleared WS_EX_TOPMOST on the game window %p\n",
          why, reinterpret_cast<void*>(g_game_wnd));
}
// --no-fg keeps the proxy swap chain but leaves frame generation off, so the
// picture can be compared against the interpolated one.
static bool g_no_fg = false;
// --overlay-layered: A/B switch. Some titles ignore the click-through bit until the
// window is also layered with a fully opaque alpha.
static bool g_overlay_layered = false;
// --relay-input: our overlay stops being click-through and forwards the mouse to the
// game window itself. Fallback for titles whose UI never sees the clicks even though
// the OS delivers them to their window (raw input still bypasses this, so aim/attack
// keeps working).
static bool g_relay_input = false;
static ULONGLONG g_relay_last_move = 0;
static volatile LONG g_relay_clicks = 0;
// --no-inputs skips importing the producer's cross-adapter heap entirely. That
// peer access (AMD reading NVIDIA VRAM) is the prime suspect for the NVIDIA TDRs
// (nvlddmkm id 153) that happen while the bridge runs, so this flag isolates it.
static bool g_no_inputs = false;
// --no-depth: hand XeFG a dummy depth of the right size instead of importing the real
// one. Depth is half of the cross-adapter traffic (33 MB/frame at 2160p) and only feeds
// disocclusion clamping, so this is both a measurable knob and a usable fallback when the
// import is what pushes `gpu` over the budget.
static bool g_no_depth = false;
// v14 switch board: which non-essential streams the producer should put on the wire. The
// defaults mirror the pre-v14 behaviour (MV + depth), so nothing changes unless asked.
static uint32_t g_stream_mask = mgpu::kRequestMv | mgpu::kRequestDepth;
// Link estimate for the OSD's occupancy figure. We cannot query the PCIe width from D3D12,
// so this is a declared number: 13 GB/s is PCIe 4.0 x8 (~80% of its 16 GB/s theoretical),
// which is what a bifurcated x8/x8 dual-GPU box actually delivers. --link=NN overrides.
static double   g_link_gbps = 13.0;

static void apply_stream_mask(uint32_t mask, const char* why)
{
    const uint32_t before = g_stream_mask;
    g_stream_mask = mask & mgpu::kRequestAllV15;
    if (before == g_stream_mask) { return; }
    logf_("[lock] streams -> mv=%d depth=%d color=%d ui=%d meta=%d rawmv=%d rawdepth=%d (%s)\n",
          (mask & mgpu::kRequestMv) ? 1 : 0, (mask & mgpu::kRequestDepth) ? 1 : 0,
          (mask & mgpu::kRequestColor) ? 1 : 0, (mask & mgpu::kRequestUi) ? 1 : 0,
          (mask & mgpu::kRequestMeta) ? 1 : 0, (mask & mgpu::kRequestRawMv) ? 1 : 0,
          (mask & mgpu::kRequestRawDepth) ? 1 : 0, why);
}
static bool     g_stream_mask_explicit = false;
static uint32_t g_stream_mask_last = 0xFFFFFFFFu;
// --no-mv: zeroed motion vectors but the real depth. Isolates "the vectors are wrong" from
// "the vectors are merely coarse" - with zero MVs a moving object must double unless depth
// plus the projection matrix can carry the motion.
static bool g_no_mv = false;
// --mv-shadow (diagnostic): with --no-mv, STILL copy the producer's motion vectors into a
// sink texture that is never tagged. That pays the full cross-adapter copy cost while XeFG
// keeps seeing zeroed vectors, which is the only way to separate "copy is expensive" from
// "XeFG actually consumes the vectors" (they were mixed together in every earlier A/B).
static bool g_mv_shadow = false;
static ComPtr<ID3D12Resource> g_mv_shadow_tex[mgpu::kMaxSlots];
// ================== EXPERIMENTAL: --fg-of (FidelityFX optical flow) =========
// Motion vectors are a DEAD INPUT for libxess_fg 1.3.1: zeroed MVs, a constant
// 100000 px field and the game's own (verified real) vectors all produce the
// identical picture, so the model estimates motion itself. Every --fg-of* knob
// and the whole flow pass are compiled OUT by default; the code and
// third_party/ffx are kept only so the conclusion can be re-tested after a
// libxess_fg upgrade (`--fg-of-const 100000 0` must break the picture again).
// Build with /DMGPU_FG_OF=1 to bring the switch back.
//   docs/findings/FINAL_CONFIG.md 2.1.2 (结论 1), docs/findings/HANDOFF.md 8.6 (第五/六轮)
#ifndef MGPU_FG_OF
#define MGPU_FG_OF 0
#endif
#if MGPU_FG_OF
// --fg-of: compute the motion vectors with FidelityFX optical flow on the AMD card from
// the captured colour, instead of copying the producer's stream.
static bool g_fg_of = false;
// A-v3: automatic fallback. The producer publishes whether its own vectors are usable
// (kSemMvUsable, from the online probe); when it says they are not, this frame uses our optical
// flow instead of feeding XeFG garbage. Only ever *adds* frames of good input - when the
// producer's vectors come back usable we switch straight back, because we never stop asking for
// them (unlike --fg-of, which drops the request entirely).
static bool g_of_fallback = true;    // --no-of-fallback turns it off
static bool g_game_mv_forced = false; // --game-mv: never auto-switch to optical flow
// --vrr: the panel follows us (G-Sync / FreeSync), so do NOT try to fill every vblank.
//
// Filling every vblank forces the count to alternate on a fractional input rate (3.5x average
// = 3,4,3,4...), which makes each output frame cover 8.3 or 11.1 ms: the refresh periods are
// full but the *motion* changes speed - precisely the residual judder measured on 2026-11-10
// (presents=140 vblanks=141, "avg 3.50x", 0.98 vblank/frame).
//
// With a VRR panel the better goal is uniform spacing: keep k fixed for as long as the input
// rate holds, and let each frame be k/t_in apart (e.g. 3 frames over 28.6 ms = 9.5 ms each =
// 105 fps). The panel locks to whatever we send; every interval is identical.
static bool         g_vrr = false;
// --wgc-minint <ms>: **default 0 = no throttle.** WGC's MinUpdateInterval defaults to one
// compositor interval (~16.6 ms), i.e. WGC hands over at most ~60 frames/s no matter how fast the
// game presents. Measured 2026-11-10: turning that throttle off made input latency and smoothness
// "instantly better" (PBZ), which also explains why every capture-rate measurement had capped out
// around 59.6 fps. 16 restores the old ~60 fps ceiling; N sets N ms.
static int          g_wgc_minint_ms = 0;

static bool g_of_active   = false;   // per frame: OF is this frame's vector source
static bool g_of_fresh    = false;   // per frame: of_record() actually ran this frame
// Prefetch-miss policy (2026-11-14): when the local lookup misses, do NOT copy; use the
// flow for this frame. `g_of_track` runs the flow every frame so the FFX context's
// internal history stays valid (running it only on misses would feed it a stale history).
static bool g_of_track    = false;   // run the flow every frame (history maintenance)
static bool g_of_miss     = false;   // this frame is a prefetch miss -> flow, no copy
static uint32_t g_of_miss_frames = 0;
// --fg-of-sign / --fg-of-scale: hit the MV convention A/B without a rebuild.
// --fg-of-guide: colour-similarity weight for the edge-aware upsample (0 = bilinear).
static float g_of_sign = 1.0f;
static float g_of_scale = 1.0f;
static float g_of_guide = 48.0f;
// --fg-of-depth-k: relative-depth weight for the depth-guided upsample (0 = off).
static float g_of_depth_k = 40.0f;
static UINT  g_of_depth_desc = 0;                          // ring entry the depth SRV was last bound for
static bool  g_of_depth_bound[mgpu::kMaxSlots] = {};       // ... and whether it has been bound at all
// MV unit convention. XeFG wants pixels of the MV texture, sign (previous - current):
// see third_party/optiscaler/shaders/camera_motion/CM_Common.h.
// 0 = pixels, 1 = UV, 2 = NDC, 3 = "mvpx" (XeFG native, default).
static int   g_of_units = 3;
static float g_of_const_x = 0.0f;      // --fg-of-const X Y (px): diagnostic override
static float g_of_const_y = 0.0f;
static bool  g_of_const_on = false;
static float g_of_deadzone = 0.60f;    // px; below this a vector means "static"
static float g_of_maxlen = 240.0f;     // px; clamp for bad matches
#endif

// ---------------------------------------------------------------- MV source: auto
// The UI no longer offers a "motion vectors" choice. Unless the command line says
// otherwise, the vectors come from the producer whenever its record says it is feeding
// them (kInputFlagMvActive + a placed stream), and from our own optical flow (or zeroed
// vectors) when it does not. `g_auto_use_mv` is refreshed once per captured frame from
// the published record - that is the "handle what we actually received" part.
static bool g_mv_auto = true;         // false as soon as any explicit MV flag is used
static bool g_auto_use_mv = true;     // per frame, from ProducerInfo/StreamShared

static bool mv_from_producer()
{
    if (g_no_inputs) { return false; }
#if MGPU_FG_OF
    // Auto-detected Unreal / --fg-of always means "our own vectors", even while the
    // producer still happens to have an MV stream. Checked first so g_mv_auto cannot
    // claim the producer's vectors after the title was classified as no-MV.
    if (g_fg_of) { return false; }
#endif
    if (g_mv_auto)   { return g_auto_use_mv; }
    return !g_no_mv;
}

// The authoritative "the producer is actually delivering motion vectors" is defined
// after g_ctl below (it needs the shared control block).

static bool mv_from_flow()
{
#if MGPU_FG_OF
    if (g_fg_of)     { return true; }
    if (g_mv_auto)   { return !g_auto_use_mv; }   // producer has no vectors -> our flow
#endif
    return false;
}

// Pairing health / optical-flow source counters. The OSD reports the share of frames
// that actually used the game's native MV, which is the quickest way to see whether a
// reset/degraded period is feeding generated vectors.
enum PairHealth
{
    kPairNormal = 0,
    kPairWatch,
    kPairSoftRecover,
    kPairDegraded,
    kPairHardReset,
    kPairRejoin
};
static PairHealth g_pair_health = kPairNormal;
static ULONGLONG  g_pair_health_since = 0;
static bool       g_pair_degraded = false;
static uint32_t   g_mv_native_frames = 0;
static uint32_t   g_mv_flow_frames = 0;
static uint32_t   g_mv_none_frames = 0;

static void lock_note_mv_source(bool native, bool flow)
{
    if (native) { ++g_mv_native_frames; }
    else if (flow) { ++g_mv_flow_frames; }
    else { ++g_mv_none_frames; }
}

// Debug features (OptiScaler exposes the same two): corner tags on generated frames, and
// presenting *only* interpolated frames - the real frames otherwise hide the artifacts.
static bool  g_fg_tag_interp = false;    // --fg-debug-tag-interp
static bool  g_fg_only_interp = false;   // --fg-debug-only-interp
// --capture-monitor: capture the display instead of the game window. Removes any
// window/swap-chain size mismatch between what the game renders and what WGC hands over.
// Kept for experiments
// only: it also captures our own overlay (feedback loop), so it is not usable in practice.
static bool g_capture_monitor = false;
// 0 = HDR10 + BT.2020 + PQ, 1 = HDR10 without the primaries matrix, 2 = SDR.
static int  g_colour_mode = 0;
// --half presents at half resolution: XeSS-FG interpolates at the swap chain size, so
// this quarters its GPU cost and isolates model cost from our own overhead.
static bool g_half = false;
static float g_sharpen = 0.0f;  // --sharp: unsharp-mask amount on the resize
// --down N picks the resampling kernel for the resize that happens before frame
// generation: 0 = single bilinear tap (the original behaviour), 1 = Catmull-Rom,
// 2 = Mitchell-Netravali, 3 = Lanczos-3. All three kernels are proper low-pass
// filters stretched by the scale factor, so 0.75x stops folding detail into moire.
static int   g_down = 0;
static float g_down_support = 0.0f;   // kernel support in output-pixel units
static float g_scale = 0.0f;   // --scale f: present at f x the captured size (0 = use g_out_den)
static int  g_out_den = 1;      // --den N: output at 1/N of the captured size
static UINT g_swap_buffers = 2; // --buffers N: XeSS-FG presents twice per input frame
static UINT g_wgc_pool = 4;     // --pool N: WGC frame pool depth (2..8)
static UINT g_max_latency = 1;  // --latency N: DXGI maximum frame latency
static UINT g_refresh_hz = 0;   // panel refresh, filled in once the output is known
static bool g_tearing = false;  // --tearing: never block on vblank (may tear)
static UINT g_present_flags() { return g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u; }
static int  g_xell_mode = 1;    // --xell-mode N: bLowLatencyMode handed to xellSetSleepMode
static bool g_no_xell = true;   // skip XeLL per-frame pacing by default (--xell re-enables)

// ---------------------------------------------------------------- MFG state
// --interp N: number of interpolated frames per real frame, so N + 1 frames are
// presented (1 = 2x, 2 = 3x, 3 = 4x). 0 = "whatever this DLL supports best".
// The DLL decides how much is really possible: maxSupportedInterpolations is 1
// on a non-Intel adapter with the official build, and only a community MFG build
// raises it (src/xefg_dyn.h).
static int          g_fg_interp = 0;
static bool         g_fg_interp_set = false;  // --interp was given (1 = 2x is a valid ask)
static UINT         g_fg_interp_actual = 1;   // what XeSS-FG really applies
static UINT         g_fg_interp_max = 1;      // maxSupportedInterpolations of the loaded DLL
static std::wstring g_fg_lib;              // --fg-lib <path>: alternative libxess_fg.dll
static bool         g_fg_probe = false;    // --fg-probe: report the DLL's MFG capability, exit
static bool         g_unlock_mfg = false;  // --unlock-mfg: lift the non-Intel clamp (byte patch)

// ------------------------------------------------- consumer adapter (M6.1)
// Which GPU runs XeSS-FG. This used to be hard-coded to the AMD adapter
// (VendorId 0x1002), which was the only vendor assumption left in this process:
// XeFG itself is a cross-vendor library (it ran on an RTX 5050 already, see
// docs/findings/XESS_FG_NOTES.md 1.5), and the swap chain only cares that the adapter owns
// the monitor our overlay sits on. See docs/design/VENDOR_COMPAT_PLAN.md M6.1.
//
//   --gpu <spec>          force the adapter: index / vendor id (10de, 1002, 8086)
//                         / nv|amd|intel / luid:HI:LO / description substring
//   --producer-luid HI:LO the GPU the game renders on; the pick avoids it.
//                         (protocol v6 publishes this from the add-on; until then
//                         it can be passed by hand)
static std::wstring g_gpu_spec;            // --gpu
static bool         g_have_producer = false;
static uint64_t     g_producer_luid = 0;   // packed Hi<<32 | Lo
static UINT         g_consumer_vendor = 0; // VendorId of the picked adapter
static DXGI_ADAPTER_DESC1 g_consumer_desc{};
// Protocol v6: what the producer said about its own inputs (see mgpu_shared.h).
// Set from the shared block before the FG context is created, because
// kInputFlagInvertedDepth changes the init flags.
static UINT         g_producer_api = 0;      // mgpu::kInputApi*
static UINT         g_producer_flags = 0;    // mgpu::kInputFlag*
static UINT         g_producer_render_w = 0; // upscaler input size
static UINT         g_producer_render_h = 0;
static UINT         g_producer_output_w = 0;
static UINT         g_producer_output_h = 0;
// --slack N: hold up to N captured frames and process the OLDEST first.
//
// Default 0 = drain the pool and keep only the newest frame, which is what makes
// the pipeline "zero latency": an over-budget frame costs an input frame. With
// N > 0 a late frame is absorbed by the next one instead, at the price of N input
// intervals of latency. It only helps when gpu_p50 is well under the interval and
// the tail is what misses - a sustained deficit fills the buffer and then drops
// frames anyway (measured, docs/findings/FINAL_CONFIG.md 9.8).
static UINT         g_slack = 0;
// --slack-auto (see the drain block in main): absorb one queued frame instead of dropping it
// when the previous frame did not fit its input interval. Off by default; it is the targeted
// fix for the occasional "drop 1" the OSD shows in --interp-cadence runs.
static bool         g_slack_auto = false;
static UINT         g_slack_auto_drops = 0;   // frames still dropped while --slack-auto is on
static UINT         g_slack_auto_real  = 0;   // ... of which had new producer content (real loss)
static UINT         g_slack_auto_holds = 0;   // frames held back (would have been dropped)
static bool         g_last_overran = false;   // last run_frame() exceeded the input interval
// Producer publication counter we last consumed (v10's `frame_published`). --slack-auto uses
// it to tell "this queued capture carries new content" from "DWM re-composited the same
// frame": only the former is worth holding an extra interval for.
static uint64_t     g_last_content_pub = 0;
// --jbuf N (0..3): the *correct* version of the same idea (P3). Every captured frame is
// copied into one of OUR textures immediately and the WGC frame is released at once, so
// the capture pool is never held hostage - holding it is exactly what made --slack cost
// +5.7 ms of gpu time for nothing (docs/findings/FINAL_CONFIG.md 9.9). Deferring the *processing*
// by N frames absorbs jitter at the price of N input intervals of latency; unlike --slack
// it does not touch WGC or the producer.
static int          g_jbuf = 0;
static std::vector<ComPtr<ID3D11Texture2D>> g_jbuf_free;
// Returned textures wait one frame before they can be handed out again: the immediate
// context copy that filled them has to retire first (D3D11 would serialise it anyway,
// this just keeps that off the critical path).
static std::vector<ComPtr<ID3D11Texture2D>> g_jbuf_pending;
// One-shot warning: XeFG paces its batch against vblank, so a whole-frame cost
// far below the input interval while several frames are being presented means
// something else took over the presentation (an OSD or a latency tool hooking
// Present, or an occluded overlay). FPS counters still read N+1 x but the
// picture judders, so it is worth saying out loud exactly once.
static bool         g_nopace_warned = false;
static UINT         g_nopace_windows = 0;   // consecutive suspicious windows

// XeFG tuning knobs. Each mirrors something OptiScaler exposes for its own XeFG
// output; all default to our previous behaviour so an A/B run only changes one
// thing at a time.
//   --fg-rt 0      frameRenderTime = 0        (OptiScaler [FrameGen] FTInput=2)
//   --fg-ui none   UI_MODE_NONE, not AUTO     (OptiScaler [XeFG] UIComposition=false)
//   --fg-scene F   xefgSwapChainSetSceneChangeThreshold(F) (default in the DLL: 0.7)
//   --fg-depth-inv INVERTED_DEPTH init flag    (reversed/infinite-Z depth)
//   --fg-jittered-mv / --fg-no-jittered-mv  JITTERED_MV init flag ("remove jitter from
//                  input velocity"); normally driven by the producer's DLSS MVJittered hint
static bool         g_fg_rt_zero = false;
// The frame constants are the one input XeFG gets that we can only approximate. UE5's
// jitter sign and the synthetic projection matrix are the two remaining suspects for the
// "never goes away" edge / model-edge artifacts, so both are switchable at runtime.
static bool         g_fg_jitter_flip = false;   // --fg-jitter-flip
// --fg-jitter-zero: DEPRECATED-looking name, but it is the A/B control once more
// (2026-11-09). The 2026-11-08 measurement ("real jitter is better") was made while the
// motion-vector stream was junk, so it is being re-tested: our colour is the *upscaled*
// output (DLSS/TSR already resolved the jitter), so telling XeFG that this frame carries
// j0 may make it shift its history by j0 for nothing - which reads as edge ghosting that no
// MV source fixes.
static bool         g_fg_jitter_zero = false;   // --fg-jitter-zero: pass jitter = 0
static bool         g_fg_no_proj = false;       // --fg-no-proj
// --fg-use-matrix: hand the game's own view-to-clip to XeFG verbatim. Off by default, because
// OptiScaler does NOT do that even though it has the same matrix: it derives near/far/FOV from
// it and passes camera values instead, with a comment that UE titles "seem to not be passing
// the correct cameraViewToClip" (inputs/FG/Streamline_Inputs.cpp:207). Deriving keeps us on the
// projection form this code already ships (reversed-Z infinite far) and only makes the numbers
// real - which is the whole point of v12 anyway.
static bool         g_fg_use_matrix = false;
// --fg-proj-rz: build the synthetic projection in reversed-Z / infinite-far form. Only the
// SYNTHETIC matrix is ever affected (the game's own matrix needs --fg-use-matrix), and
// libxess_fg 1.3.1 does not read projectionMatrix at all - it reprojects with the motion
// vectors (docs/findings/FINAL_CONFIG.md 5.16). Left as a switch for our own --fg-warp /
// --fg-of paths; not needed by the shipping data plane.
static bool         g_fg_proj_rz = false;
// The projection is the one frame constant we can only approximate, so its two numbers are
// tunable: --fg-fov (vertical field of view, degrees) and --fg-zn (near plane). UE5's
// defaults are ~58.7 deg vertical (90 horizontal at 16:9) and a near plane of 10.
float               g_fg_fov_y = 60.0f;
float               g_fg_near_z = 10.0f;
static bool         g_fg_consts_logged = false;
static bool         g_fg_ui_none = false;
static bool         g_mv_gate_off = false;   // this frame has no MV -> 1X, no tagging
static float        g_fg_scene = 0.0f;
static bool         g_fg_depth_inv = false;
// --fg-jittered-mv / --fg-no-jittered-mv: XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV, which the
// SDK documents as "Remove jitter from input velocity"
// (third_party/xess/inc/xess_fg/xefg_swapchain.h:95). UE5 computes its velocity on the
// jittered render, so the +-0.5 px Halton offset is still inside the vectors; without this
// flag XeFG treats that offset as real motion, which is exactly the model-edge smear we
// kept attributing to "UE5 MVs are weird".
// OptiScaler does the same: it maps the game's DLSS MVJittered flag onto this init flag
// (OptiScaler/upscalers/IFeature.cpp:112 -> OptiScaler/framegen/xefg/XeFG_Dx12.cpp:362),
// see docs/findings/UE5_MV_OPTISCALER.md 5.1.
// The add-on now publishes the game's declaration as mgpu::kInputFlagMvJittered, so this
// turns itself on per game; --fg-jittered-mv forces it on and --fg-no-jittered-mv vetoes
// the automatic path (that pairing is what makes a clean A/B possible).
static bool         g_fg_jittered_mv = false;
static bool         g_fg_jittered_mv_forced = false;   // -jittered-mv / --no-jittered-mv given
// --fg-mv-scale raw|px|<float>: what we hand XeFG as motionVectorScaleX/Y.
//   raw  (default) the producer's own NGX MV.Scale, unchanged
//   px             game scale * MV texture size (turns "UV-space vectors", which report a
//                  scale of 1.0, into the pixel-space vectors XeFG documents:
//                  "screen-space motion in pixels", third_party/xess/doc/...guide:809)
//   <float>        force both components to that number
// PBZ reports MV.Scale = (1, 1) while CP2077 reports (2560, 1440), so the two games store
// their vectors in different units - this switch is how the difference gets measured
// instead of guessed (docs/findings/UE5_MV_OPTISCALER.md 5.2).
static int          g_fg_mv_scale_mode = 0;            // 0 raw, 1 px, 2 forced
static float        g_fg_mv_scale_force = 1.0f;

// True when the vectors we tag are NOT the game's (optical flow, or none at all).
static bool own_mv_path()
{
#if MGPU_FG_OF
    return g_fg_of;
#else
    return false;
#endif
}

// Turn the producer's own declarations into our init flags. Called at every point where the
// flags could have become known (peek, open, and again just before the XeFG swap chain is
// created), because the add-on only learns them when the game creates its DLSS feature and
// that can easily be later than the bridge start.
static void adopt_producer_hints(uint32_t producer_flags)
{
    // v13.1: the producer's probe says the MV field is dead (all-zero while depth content
    // moves). Say so loudly; do not silently feed a zero field to XeFG - the visible symptom
    // is judder, which is very hard to attribute. Falling back automatically is left to an
    // explicit switch until the optical-flow cost is measured (docs/design/FG_CLAIM.md §3).
    if ((producer_flags & mgpu::kInputFlagMvSuspect) != 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logf_("[lock]   producer flags its MV field as unusable (kInputFlagMvSuspect):"
                  " the vectors are all-zero while the depth is moving.\n"
                  "[lock]   that field is what XeFG would interpolate with - expect judder."
                  " Consider --fg-of (AMD optical flow) or --no-mv for this title.\n");
        }
    }
    if (!g_fg_depth_inv && (producer_flags & mgpu::kInputFlagInvertedDepth) != 0) {
        g_fg_depth_inv = true;
        logf_("[lock]   producer says the depth is inverted - enabling"
              " XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH (as --fg-depth-inv would)\n");
    }
    // Scope of the automatic path: only the "game's own motion vectors" recipe. With --fg-of
    // the vectors are measured by us between two captured (already jittered) frames, and with
    // --no-mv there are none at all; both are recipes that were never measured with this
    // flag, so they keep byte-for-byte the old behaviour. --fg-jittered-mv still forces it.
    if (!g_fg_jittered_mv && !g_fg_jittered_mv_forced && !g_no_mv && !own_mv_path() &&
        (producer_flags & mgpu::kInputFlagMvJittered) != 0) {
        g_fg_jittered_mv = true;
        logf_("[lock]   producer says its motion vectors still carry the jitter - enabling"
              " XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV (as --fg-jittered-mv would)\n");
    }
}
static xefg_dyn::Api g_api;                // resolved libxess_fg.dll entry points

// The window that actually owns the swap chain. It is not always g_bridge_wnd:
// this machine rejects CreateSwapChainForHwnd on our own class, so a plain window
// is used instead and has to become the visible overlay.
static HWND g_sc_wnd = nullptr;

// The window that is actually presented on. Since the HDR10 change XeSS-FG creates
// the swap chain itself, so g_sc_wnd stays null and g_bridge_wnd is the overlay -
// every z-order and focus decision must use this.
static HWND overlay_wnd()
{
    return g_sc_wnd ? g_sc_wnd : g_bridge_wnd;
}
static UINT g_width = 5120;
static UINT g_full_w = 5120;   // window size: always the full screen
static UINT g_full_h = 2880;
static UINT g_height = 2880;

// ---------------------------------------------------------------- overlay placement
// Which display the overlay (and therefore XeSS-FG's swap chain) is presented on.
//   -1    follow the game window (historical behaviour)
//   >=0   the Nth monitor in EnumDisplayMonitors() order   (--overlay-monitor N)
//
// This is the mirror of --capture-monitor ("which screen do we capture"): the present
// side is a separate decision, and it is also the only way to give the game its
// independent flip back - with a fullscreen topmost overlay on the SAME monitor, DWM has
// to compose both, which measured as 5-10% of captures arriving one vblank late
// (docs/findings/PERF_BASELINE.md 3.10/3.11).
static int  g_overlay_monitor = -1;
// --overlay-hidden: create the swap chain and keep presenting, but never show the window.
// Measurement only - it answers "is our overlay the reason the game loses its flip layer?"
// because with the overlay hidden the game window is the topmost visible thing again.
static bool g_overlay_hidden = false;
static bool g_overlay_hidden_warned = false;
// The adapter XeSS-FG runs on, kept so --overlay-monitor can be validated before the
// window/swap chain are created (CreateSwapChainForHwnd requires the device to drive the
// output, and failing there leaves the user with no picture at all).
static ComPtr<IDXGIAdapter1> g_consumer_adapter;

struct MonitorPick { int want; int index; HMONITOR mon; };

static BOOL CALLBACK pick_monitor_proc(HMONITOR mon, HDC, LPRECT, LPARAM lp)
{
    auto* p = reinterpret_cast<MonitorPick*>(lp);
    if (p->index == p->want) { p->mon = mon; return FALSE; }   // stop enumerating
    ++p->index;
    return TRUE;
}

static HMONITOR overlay_monitor(HWND game_wnd)
{
    if (g_overlay_monitor >= 0) {
        MonitorPick p{ g_overlay_monitor, 0, nullptr };
        EnumDisplayMonitors(nullptr, nullptr, pick_monitor_proc,
                            reinterpret_cast<LPARAM>(&p));
        if (p.mon) { return p.mon; }
        if (!g_overlay_hidden_warned) {
            g_overlay_hidden_warned = true;
            logf_("[lock] WARNING: --overlay-monitor %d matched no monitor (%d present)"
                  " - following the game window instead\n", g_overlay_monitor, p.index);
        }
    }
    if (game_wnd) { return MonitorFromWindow(game_wnd, MONITOR_DEFAULTTOPRIMARY); }
    const POINT origin{ 0, 0 };
    return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

// The display mode of the monitor we actually present on. It used to read the *primary*
// display (EnumDisplaySettingsW(nullptr, ...)), which is wrong the moment the overlay
// lives on another output: --fg-rt-mode quant and --list-modes would snap to the wrong
// vblank. --overlay-monitor makes that a supported configuration, so read the right one.
static bool overlay_display_mode(DEVMODEW* dm)
{
    if (!dm) { return false; }
    ZeroMemory(dm, sizeof(*dm));
    dm->dmSize = sizeof(*dm);
    const HMONITOR mon = overlay_monitor(nullptr);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, reinterpret_cast<MONITORINFO*>(&mi))) {
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, dm)) { return true; }
    }
    return EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, dm) != FALSE;
}

// Where the overlay window has to be: exactly the monitor we present on. --den/--scale
// change the swap chain's back-buffer size, never the window rect (DXGI stretches it).
static void overlay_rect(RECT* out)
{
    if (!out) { return; }
    out->left = 0;
    out->top = 0;
    out->right = static_cast<LONG>(g_full_w);
    out->bottom = static_cast<LONG>(g_full_h);

    const HMONITOR mon = overlay_monitor(nullptr);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) { return; }
    *out = mi.rcMonitor;

    static bool logged = false;
    if (!logged) {
        logged = true;
        logf_("[lock] overlay target: monitor %p rect=(%ld,%ld)-(%ld,%ld) %lux%lu"
              " (%s)\n", reinterpret_cast<void*>(mon),
              mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
              static_cast<unsigned long>(mi.rcMonitor.right - mi.rcMonitor.left),
              static_cast<unsigned long>(mi.rcMonitor.bottom - mi.rcMonitor.top),
              (g_overlay_monitor >= 0) ? "--overlay-monitor" : "follows the game window");
    }
}

static mgpu::SharedControl*   g_ctl = nullptr;

// The authoritative "the producer is actually delivering motion vectors" test.
// - active_mask is written by the producer only for streams it really copied;
// - the MV slot is ALWAYS placed (fixed layout), so slot_size/width alone is not
//   evidence that any vector data exists. That was the trap: a depth-only title has
//   streams[1].slot_size != 0, so the old `placed` check kept g_auto_use_mv = true.
static bool producer_has_mv_stream()
{
    if (g_ctl == nullptr) { return false; }
    return (g_ctl->active_mask & mgpu::kRequestMv) != 0 &&
           g_ctl->streams[mgpu::kStreamMv].slot_size != 0;
}

static ComPtr<ID3D12Resource> g_mv[mgpu::kMaxSlots];
static ComPtr<ID3D12Resource> g_depth[mgpu::kMaxSlots];
// --fg-colour shared: take the colour from the add-on's stream[0] (a cross-adapter copy of
// the game's back buffer) instead of WGC. That is the "own the output" half of the FG plan:
// with the game's own frame generation cut at the NGX core (FgSuppressNgx=1) the game still
// presents at its FG cadence and half of those presents are empty, so capturing its window
// yields black frames. The add-on has the *real* frames (the back buffer before present), and
// stream[0] is how they get here (docs/design/FG_CLAIM.md §16).
static ComPtr<ID3D12Resource> g_colour[mgpu::kMaxSlots];   // stream 0: HUDless colour
static ComPtr<ID3D12Resource> g_ui[mgpu::kMaxSlots];       // stream 3: UI colour+alpha
static bool                   g_fg_colour_shared = false;      // --fg-colour shared
// v8: the mapping-backed heap is kept so the views can be re-placed when the add-on
// rebuilds its slot layout (upscaler preset/mode switch).
static ComPtr<ID3D12Heap> g_shared_heap;
static uint32_t g_seen_generation = 0;
// Separate copy for the geometry cache: the "release local copies" path and the
// "re-read stream geometry" path both key off generation, but they must be able to
// disagree (e.g. geometry unknown while generation is already current).
static uint32_t g_seen_geo_generation = 0xFFFFFFFFu;

static void refresh_input_geo();   // defined next to the stream-copy helpers below
static bool     g_layout_pending = false;
static ComPtr<ID3D12Fence>    g_fwd[mgpu::kMaxSlots];
static UINT64                 g_fwd_last[mgpu::kMaxSlots] = {};
static UINT                   g_shared_slot = 0;

// A plain perspective projection, since DLSS-SR does not publish the real one
// (docs/findings/milestones/M2_RESULTS.md section 4). Row-major.
// UE5 (and most modern titles) use a REVERSED-Z projection with an infinite far plane:
// near maps to 1, far to 0. The standard matrix below maps near to 0, so unprojecting the
// game's depth with it inverts every occlusion test - which is exactly what "artifacts on
// everything that moves, plus flicker" looks like.
//
// Since protocol v6 the FOV/near plane can come from the producer (FSR 3.1 exports
// near/far/vfov); --fg-fov/--fg-zn are the fallback for sources without a camera.
static void approx_projection_rz(float* m, float aspect, float fov_y_rad, float zn)
{
    const float f = 1.0f / tanf(fov_y_rad * 0.5f);
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect;
    m[5] = f;
    m[10] = 0.0f;                            // infinite far plane
    m[11] = -1.0f;
    m[14] = zn;                              // near plane -> 1
}

static void approx_projection(float* m, float aspect, float fov_y_rad, float zn, float zf)
{
    const float f = 1.0f / tanf(fov_y_rad * 0.5f);
    if (zf <= zn) { zf = zn + 1.0f; }
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect;
    m[5] = f;
    m[10] = zf / (zn - zf);
    m[11] = -1.0f;
    m[14] = (zn * zf) / (zn - zf);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    // The overlay must never take input away from the game.
    if (msg == WM_NCHITTEST)    { return HTTRANSPARENT; }
    if (msg == WM_MOUSEACTIVATE) { return MA_NOACTIVATE; }
    // The launcher closes us with WM_CLOSE. Answer it from here instead of letting
    // DefWindowProc destroy the window and hoping the frame loop is pumping - if it
    // happens to be blocked inside Present, that hope is worth seconds.
    if (msg == WM_CLOSE) { hard_exit("WM_CLOSE from the launcher"); }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

// The window that ends up owning the swap chain may be a system class window, so
// it has to be subclassed to make it click-through.
static WNDPROC g_overlay_prev_proc = nullptr;
static volatile LONG g_hittest_count = 0;

static void focus_game();

static LRESULT CALLBACK overlay_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST) {
        // Counted so the log proves the overlay really is in the hit-test path.
        ++g_hittest_count;
        return HTTRANSPARENT;
    }
    if (msg == WM_MOUSEACTIVATE) { return MA_NOACTIVATE; }
    if (msg == WM_CLOSE) { hard_exit("WM_CLOSE on the overlay"); }
    if (msg == WM_ACTIVATE) {
        if (LOWORD(wp) != WA_INACTIVE) { focus_game(); }
    }
    // --relay-input: forward the mouse to the game in ITS client coordinates. The
    // overlay covers the whole screen at 0,0, so screen space is the common ground.
    if (g_relay_input && g_game_wnd) {
        switch (msg) {
        case WM_MOUSEMOVE: {
            const ULONGLONG t = GetTickCount64();
            if (t - g_relay_last_move < 8) { return 0; }   // throttle: the game also gets raw input
            g_relay_last_move = t;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(h, &pt);
            ScreenToClient(g_game_wnd, &pt);
            PostMessageW(g_game_wnd, msg, wp, MAKELPARAM(pt.x, pt.y));
            return 0;
        }
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            LPARAM out = lp;
            if (msg != WM_MOUSEWHEEL) {          // wheel lParam is already screen space
                ClientToScreen(h, &pt);
                ScreenToClient(g_game_wnd, &pt);
                out = MAKELPARAM(pt.x, pt.y);
            }
            PostMessageW(g_game_wnd, msg, wp, out);
            const LONG k = InterlockedIncrement(&g_relay_clicks);
            if (k <= 4) {
                logf_("[lock] relay #%ld: msg=0x%04X -> game at %ld,%ld\n", k, msg, pt.x, pt.y);
            }
            return 0;
        }
        default: break;
        }
    }

    // Smoking gun for "the overlay eats the click": a click-through window must never
    // see these. Logged once each so a real report is unambiguous.
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) {
        static bool logged_l = false, logged_r = false;
        bool& done = (msg == WM_LBUTTONDOWN) ? logged_l : logged_r;
        if (!done) {
            done = true;
            logf_("[lock] WARNING overlay received %s - the click was NOT passed through\n",
                  (msg == WM_LBUTTONDOWN) ? "a left click" : "a right click");
        }
    }
    return CallWindowProcW(g_overlay_prev_proc, h, msg, wp, lp);
}

static HWND create_bridge_window(UINT w, UINT h)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"mgpu_bridge_window";
    RegisterClassExW(&wc);

    // Created without WS_EX_TOPMOST and without WS_VISIBLE, and with an exact
    // client size: a topmost or not-yet-mapped window made
    // CreateSwapChainForHwnd return E_ACCESSDENIED here (the identical swap chain
    // worked on a plain, hidden, exactly sized window). The overlay is turned into
    // a visible borderless topmost window by make_borderless() once the swap chain
    // exists.
    //
    // WS_EX_TOOLWINDOW is set *here*, at creation, so the shell never registers this as an
    // application window in the first place - that is what kept it out of Alt+Tab/Win+Tab
    // without needing an owner relationship (which broke the game's mouse input).
    RECT r{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"MGPU Bridge",
                                WS_OVERLAPPEDWINDOW, 0, 0,
                                r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd) {
        RECT client{};
        GetClientRect(hwnd, &client);
        // System metrics are not always enough for an exact client size, so measure
        // and correct once.
        const LONG dw = static_cast<LONG>(w) - (client.right - client.left);
        const LONG dh = static_cast<LONG>(h) - (client.bottom - client.top);
        if (dw || dh) {
            SetWindowPos(hwnd, nullptr, 0, 0, (r.right - r.left) + dw, (r.bottom - r.top) + dh,
                         SWP_NOZORDER | SWP_NOMOVE);
            GetClientRect(hwnd, &client);
        }
        logf_("[lock] window client %ldx%ld (requested %ux%u)\n",
              client.right - client.left, client.bottom - client.top, w, h);
    }
    return hwnd;
}

// Turns the overlay into a visible, borderless, topmost window. Called only after
// the swap chain exists, because the window style present during
// CreateSwapChainForHwnd matters on this machine.
// Giving the foreground back is not just SetForegroundWindow: Windows blocks a
// background process from activating a window owned by another thread unless both
// input queues are attached for the call.
static void focus_game()
{
    if (!g_game_wnd) { return; }
    const HWND fg = GetForegroundWindow();
    if (fg == g_game_wnd) { return; }

    const DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD my_thread = GetCurrentThreadId();
    bool attached = false;
    if (fg_thread && fg_thread != my_thread) {
        attached = AttachThreadInput(my_thread, fg_thread, TRUE) != FALSE;
    }
    SetForegroundWindow(g_game_wnd);
    if (attached) { AttachThreadInput(my_thread, fg_thread, FALSE); }
}

// Keeps the overlay out of Alt+Tab, Task View (Win+Tab), Aero Peek and the taskbar.
//
// Why this is not just WS_EX_TOOLWINDOW: the window is created hidden and *plain* on
// purpose (CreateSwapChainForHwnd rejects a topmost/visible window on this machine), so a
// style applied afterwards can arrive after the shell already registered the entry. Hence:
//   * WS_EX_TOOLWINDOW at creation (see create_bridge_window) - never registered as an app
//   * DWMWA_EXCLUDED_FROM_PEEK / DWMWA_DISALLOW_PEEK - out of peek/thumbnail previews
//   * ITaskbarList::DeleteTab - drops an entry that already exists
//
// Deliberately NOT used: SetWindowLongPtr(GWLP_HWNDPARENT, game_wnd). Making the overlay an
// owned window of the game does group it in Task View, but it also changed how input
// reached the game: the mouse stopped responding while the keyboard kept working (measured
// by the user right after it was added). A mirror window must stay a plain top-level window.
static void overlay_hide_from_switcher(HWND hwnd)
{
    if (!hwnd) { return; }

    BOOL peek = TRUE;
    const HRESULT h_ex = DwmSetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &peek,
                                               sizeof(peek));
    const HRESULT h_dp = DwmSetWindowAttribute(hwnd, DWMWA_DISALLOW_PEEK, &peek,
                                               sizeof(peek));

    HRESULT h_tab = E_NOINTERFACE;
    ComPtr<ITaskbarList> bar;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&bar))) && bar) {
        if (SUCCEEDED(bar->HrInit())) { h_tab = bar->DeleteTab(hwnd); }
    }

    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    logf_("[lock] switcher: peek excl hr=0x%08lX/0x%08lX, DeleteTab hr=0x%08lX,"
          " exstyle=0x%08lX (no owner - see the comment; owner broke mouse input)\n",
          static_cast<unsigned long>(h_ex), static_cast<unsigned long>(h_dp),
          static_cast<unsigned long>(h_tab),
          static_cast<unsigned long>(ex));
}

static void make_borderless(HWND hwnd, int x, int y, UINT w, UINT h)
{
    // WS_EX_TRANSPARENT  - the window is skipped when the mouse is hit tested
    // WS_EX_NOACTIVATE   - it never becomes active, so the game keeps focus
    // WS_EX_TOOLWINDOW   - keeps it out of Alt+Tab and Task View
    LONG_PTR ex = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    if (!g_relay_input) { ex |= WS_EX_TRANSPARENT; }   // relay mode must RECEIVE the mouse
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    // No WS_VISIBLE in the style: setting that bit with SetWindowLongPtr can
    // activate the window, which is what stole input from the game.
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP);

    if (!g_overlay_prev_proc) {
        g_overlay_prev_proc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(overlay_proc)));
    }

    // --overlay-hidden: same window, same swap chain, same presents - just never shown.
    // Everything the frame loop measures (arr/out/game histograms, age, colour) still
    // works; what changes is that the game window is the topmost visible thing on its
    // monitor again. That is the whole point of the switch: it isolates "our overlay
    // costs the game its flip layer" from "WGC/composition is lossy anyway".
    if (g_overlay_hidden) {
        SetWindowPos(hwnd, nullptr, x, y, static_cast<int>(w), static_cast<int>(h),
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW);
        ShowWindow(hwnd, SW_HIDE);
        focus_game();
        logf_("[lock] --overlay-hidden: window stays hidden (presents keep running);"
              " the game window should be the topmost visible one now\n");
        return;
    }

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, static_cast<int>(w), static_cast<int>(h),
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    focus_game();

    // The window exists, is in the topmost band and has its ex-styles: strip it from the
    // taskbar / Alt+Tab / Task View so the user only ever sees one picture (the game's).
    overlay_hide_from_switcher(hwnd);

    RECT client{};
    GetClientRect(hwnd, &client);
    logf_("[lock] borderless: client %ldx%ld (wanted %ux%u) scroll=%d fg=%d\n",
          client.right - client.left, client.bottom - client.top, w, h,
          (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) ? 1 : 0,
          (GetForegroundWindow() == g_game_wnd) ? 1 : 0);
}

// Hand the foreground back to the game if the overlay grabbed it. Other
// applications are left alone, so alt-tabbing away still works. Throttled: doing
// this on every idle poll caused foreground storms.
static ULONGLONG g_last_focus_try = 0;

static void keep_game_focus()
{
    if (!g_game_wnd) { return; }
    const HWND fg = GetForegroundWindow();
    if (fg != nullptr && fg != overlay_wnd()) { return; }

    const ULONGLONG now = GetTickCount64();
    if (now - g_last_focus_try < 1000) { return; }
    g_last_focus_try = now;
    focus_game();
}

