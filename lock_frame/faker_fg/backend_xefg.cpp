// ============================================================================
//  backend_xefg.cpp - the execution side: XeSS-FG owns the present path.
//
//  Because XeFG *created* the swap chain (InitFromSwapChainDesc + GetSwapChainPtr
//  in proxy_dxgi.cpp), the title presents into XeFG's chain and XeFG is the one
//  that shows frames. Generation on/off and the interpolated count are therefore
//  entirely ours, and "the game must not generate, but the picture must still be
//  right" stops being a problem - it is XeFG presenting the real frame.
//
//  Per frame (from provider.cpp, on the title's own open command list):
//      tag MV / depth / hudless / UI  with the state WE declare
//      tag the frame constants (projection, jitter, MV scale, reset, frame time)
//      SetPresentId                  - XeFG pairs this frame with the next Present
//
//  States: the resources arrive through the game's FG contract, so the honest
//  value we can promise is COMMON (the same declaration our bridge has run in
//  production). Backbuffer is NOT tagged: it is XeFG's own swap chain backbuffer
//  by construction.
//
//  Provenance: flow follows OptiScaler's framegen/xefg/XeFG_Dx12.cpp and
//  inputs/FG/XeFG_Inputs_Dx12.cpp (GPL-3.0, see docs/design/FAKER_FG.md 7).
// ============================================================================
#include "fg_internal.h"

#include <DirectXMath.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include "xefg_dyn.h"

#include "fg_swapchain.h"

#include "xell/xell.h"
#include "xell/xell_d3d12.h"

namespace fakerfg {
namespace {

xefg_dyn::Api            g_xapi{};
xefg_swapchain_handle_t  g_ctx = nullptr;
IDXGISwapChain*          g_chain = nullptr;
unsigned                 g_max_interp = 1;
unsigned                 g_width = 0;
unsigned                 g_height = 0;
uint32_t                 g_chain_fmt = 0;       // DXGI_FORMAT of the XeFG chain
bool                     g_generate = false;   // XeFG generation actually on
bool                     g_want_generate = false; // FgOutput=XeFG and not passthrough
std::atomic<unsigned>    g_native_pending{ 0 };   // one Present per native publication
std::atomic<bool>        g_native_seen{ false };  // gate only after the first real record
std::atomic<uint64_t>    g_native_last_tick{ 0 }; // when the last native record arrived
uint32_t                 g_frames_presented = 0;   // framesPresented of the last present (logging)
unsigned                 g_gen_fail_run = 0;
uint32_t                 g_frame_id = 0;             // StartNewFrame counter (OptiScaler's _frameCount)
// Written by one thread, read by another (the second CreateSwapChainForHwnd while the first
// one initialises), so these are atomic - the wait loop must re-read them.
std::atomic<bool>        g_starting{ false };
std::atomic<bool>        g_chain_ready{ false };  // the proxy chain exists (g_ctx may predate it)
ID3D12Device*            g_xdev = nullptr;
// The chain the title presents on: the library object (RealSwapchainObject=1) or our facade.
IDXGISwapChain*          g_chain_api = nullptr;
ID3D12CommandQueue*      g_queue = nullptr;
FgSwapChain*             g_facade = nullptr;
HWND                     g_owner_hwnd = nullptr;
MgpuFgFrame              g_last_frame{};             // the frame the tags describe (for the present path)
bool                     g_tags_had_depth = false;  // what the last xefg_tag() delivered
bool                     g_tags_had_mv = false;
bool                     g_tags_size_ok = true;    // MV/depth resourceSize match (SDK rule)
bool                     g_inputs_logged = false;
bool                     g_lane_logged = false;
uint32_t                 g_last_tagged_id = 0;       // the frame the tags describe
uint32_t                 g_last_presented_id = 0;    // the id SetPresentId last used
// Evaluate-tagged lanes (DLSSG / NVNGX / FSR) hand their accepted tag id to the present
// path through this; one dispatch per accepted tag.
uint32_t                 g_submit_frame_id = 0;
uint32_t                 g_submit_dispatched_id = 0;
int64_t                  g_last_present_qpc = 0;
float                    g_frame_ms = 0.0f;
// State::lastFGFrameTime: the frame time measured while generation is OFF, frozen once it
// runs (LocalPresent only updates it when `currentFG == nullptr`).
float                    g_frame_ms_no_fg = 0.0f;
// Camera frame from sl::Constants (Sl_Inputs::CaptureConstants -> SetCameraValues).
float                    g_cam_right[3]{};
float                    g_cam_up[3]{};
float                    g_cam_forward[3]{};
float                    g_cam_pos[3]{};
bool                     g_cam_known = false;
// P2-E: contexts retired by FGPreserveSwapChain=false. The game may still hold the old
// facade/chain; destroying the context immediately is a use-after-free hazard. Keep a
// bounded retire list alive until backend_shutdown (a leak only for the rare title that
// recreates its chain; safer than returning from Present through freed memory).
xefg_swapchain_handle_t  g_retired_ctx[8]{};
unsigned                 g_retired_n = 0;
bool                     g_waiting_new_frame_data = false;   // _waitingNewFrameData
uint32_t                 g_fg_frame_present_id = 0;         // _fgFramePresentId
uint32_t                 g_last_fg_frame_present_id = 0;    // _lastFGFramePresentId

// TagAtEvaluate (see xefg_tag_at_evaluate): the frame the upscaler's own Evaluate tagged,
// the id those tags carry, and whether the next present still has to consume it. The flag
// is the frame-based "freshness" XeFG_Dx12::Dispatch gets from its IsReady(Depth/Velocity)
// gate: one tagged frame per presented frame, no wall clock.
MgpuFgFrame              g_eval_frame{};
uint32_t                 g_eval_frame_id = 0;
uint32_t                 g_eval_tag_counter = 0;             // one id per tagged frame
bool                     g_eval_tag_fresh = false;

const char* result_name(xefg_swapchain_result_t r)
{
    switch (r) {
    case XEFG_SWAPCHAIN_RESULT_SUCCESS:               return "OK";
    case XEFG_SWAPCHAIN_RESULT_WARNING_OLD_DRIVER:    return "WARNING_OLD_DRIVER";
    case XEFG_SWAPCHAIN_RESULT_WARNING_TOO_FEW_FRAMES: return "WARNING_TOO_FEW_FRAMES";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE: return "UNSUPPORTED_DEVICE";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DRIVER: return "UNSUPPORTED_DRIVER";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED:    return "UNINITIALIZED";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "DEVICE_OUT_OF_MEMORY";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED:      return "UNSUPPORTED";
    case XEFG_SWAPCHAIN_RESULT_ERROR_CANT_LOAD_LIBRARY: return "CANT_LOAD_LIBRARY";
    case XEFG_SWAPCHAIN_RESULT_ERROR_MISMATCH_INPUT_RESOURCES: return "MISMATCH_INPUT_RESOURCES";
    case XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER: return "WRONG_CALL_ORDER";
    case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED:
        return "LATENCY_REDUCTION_UNSUPPORTED";
    default:                                           return "ERROR";
    }
}

void log_cb(const char* message, xefg_swapchain_logging_level_t level, void*)
{
    const char* lv = "info";
    switch (level) {
    case XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG:   lv = "debug"; break;
    case XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING: lv = "warn";  break;
    case XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR:   lv = "error"; break;
    default: break;
    }
    logf("xefg[%s] %s\n", lv, (message != nullptr) ? message : "");
}

// One resource, with the state and validity we declare.
//   * a resource the TITLE owns is tagged ONLY_NOW: XeFG then records its read into the
//     command list we hand over, i.e. during the frame, which is the only moment that
//     resource is in the state we declared (OptiScaler: `validity = ValidNow` for both
//     motion vectors and depth in its input modules).
//   * a resource WE own (the R32 depth copy) is tagged UNTIL_NEXT_PRESENT: it stays
//     untouched until the present that consumes it (OptiScaler's ValidButMakeCopy).
// XeFG_Dx12::Dispatch(), the block guarded by `activeFgInput != FGInput::Upscaler`:
//
//      backbuffer.type      = XEFG_SWAPCHAIN_RES_BACKBUFFER;
//      backbuffer.validity  = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
//      backbuffer.resourceBase = { left, top };
//      backbuffer.resourceSize = { _interpolationWidth[fIndex], _interpolationHeight[fIndex] };
//      D3D12TagFrameResource(_swapChainContext, (ID3D12CommandList*) 1, frameId, &backbuffer);
//
// Note what is *not* set: no pResource (it is XeFG's own backbuffer) and no state - this
// tag only declares which rectangle of that backbuffer gets interpolated. The Upscaler
// input lane skips it, because there the interpolation rect is the full display size.
// Interpolation rectangle. XeFG requires HUDless/UI/back buffer to have the same
// resourceSize (xess_fg_developer_guide_english.md:796-802). When the title's HUDless is
// smaller than the chain, OptiScaler uses the HUDless extent and centres it inside the
// back buffer (XeFG_Dx12.cpp:943-991); the same rule is applied here.
bool tag_backbuffer(uint32_t frame_id, ID3D12CommandList* list, UINT w, UINT h,
                    UINT left, UINT top)
{
    if (g_ctx == nullptr || g_xapi.D3D12TagFrameResource == nullptr) { return false; }

    xefg_swapchain_d3d12_resource_data_t d{};
    d.type         = XEFG_SWAPCHAIN_RES_BACKBUFFER;
    d.validity     = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
    d.resourceBase = { left, top };
    d.resourceSize = { w, h };
    d.pResource    = nullptr;

    // intel/xess#45: with an UNTIL_NEXT_PRESENT tag the library wants a non-null pointer.
    ID3D12CommandList* tag_list = (list != nullptr) ? list
                                                    : reinterpret_cast<ID3D12CommandList*>(1);
    const xefg_swapchain_result_t r = g_xapi.D3D12TagFrameResource(g_ctx, tag_list, frame_id, &d);
    static bool warned = false;
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS && !warned) {
        warned = true;
        logf("xefg: tag backbuffer %ux%u+%u+%u failed: %s (%d) - logged once\n",
             d.resourceSize.x, d.resourceSize.y, d.resourceBase.x, d.resourceBase.y,
             result_name(r), static_cast<int>(r));
    }
    return r == XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

// One resource, with the state and validity we declare.
//   * a resource the TITLE owns is tagged ONLY_NOW: XeFG then records its read into the
//     command list we hand over, i.e. during the frame (OptiScaler: `validity = ValidNow`
//     for both motion vectors and depth in its input modules).
//   * a resource WE own is tagged UNTIL_NEXT_PRESENT (OptiScaler's ValidButMakeCopy).
bool tag_one(xefg_swapchain_resource_type_t type, ID3D12Resource* res,
             unsigned w, unsigned h, uint32_t declared_state, ID3D12CommandList* tag_list,
             bool until_present)
{
    if (g_ctx == nullptr || res == nullptr || w < 8 || h < 8) { return false; }
    if (g_xapi.D3D12TagFrameResource == nullptr) { return false; }

    xefg_swapchain_d3d12_resource_data_t d{};
    d.type          = type;
    d.validity      = until_present ? XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT
                                    : XEFG_SWAPCHAIN_RV_ONLY_NOW;
    d.resourceBase  = { 0, 0 };
    d.resourceSize  = { w, h };
    d.pResource     = res;
    d.incomingState = static_cast<D3D12_RESOURCE_STATES>(declared_state);

    const xefg_swapchain_result_t r = g_xapi.D3D12TagFrameResource(g_ctx, tag_list, g_frame_id, &d);
    static bool warned[4]{};
    const int idx = static_cast<int>(type);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS && idx >= 0 && idx < 4 && !warned[idx]) {
        warned[idx] = true;
        logf("xefg: tag type=%d (%ux%u state=0x%X) failed: %s (%d) - logged once\n",
             idx, w, h, declared_state, result_name(r), static_cast<int>(r));
    }
    return r == XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

// OptiScaler's Sl_Inputs stores cmdList per resource and XeFG copies a ValidNow resource
// on that list. Use the tag's own list when it has one; fall back to the list the caller
// passed (Evaluate / present) for UntilPresent resources and old records.
ID3D12CommandList* resource_cmd(const MgpuFgResource& r, void* per_resource_cmd,
                                ID3D12CommandList* fallback)
{
    if (r.validity == MGPU_FG_RV_ONLY_NOW && per_resource_cmd != nullptr) {
        return static_cast<ID3D12CommandList*>(per_resource_cmd);
    }
    return fallback;
}

} // namespace

UINT chain_current_index()
{
    IDXGISwapChain3* sc3 = nullptr;
    UINT idx = 0;
    if (g_chain_api != nullptr &&
        SUCCEEDED(g_chain_api->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3 != nullptr) {
        idx = sc3->GetCurrentBackBufferIndex();
        sc3->Release();
    }
    return idx;
}

bool xefg_ready() { return g_chain_ready; }

unsigned xefg_max_interp() { return g_max_interp; }

// StartNewFrame (IFGFeature::StartNewFrame): one id per frame whose inputs arrive, the
// tags of that frame carry it, and the present that follows presents it. OptiScaler's
// ids come from a monotonic frame counter, never from the app's present count.
uint32_t xefg_next_frame_id() { return ++g_frame_id; }

void xefg_note_submit(uint32_t present_id)
{
    if (present_id != 0) { g_submit_frame_id = present_id; }
}

// ---------------------------------------------------------------- XeLL
// XeFG refuses InitFromSwapChainDesc with LATENCY_REDUCTION_UNSUPPORTED (-15) unless
// a XeLL (low-latency) context is attached *before* initialization - the same trap our
// own bridge documents (src/bridge/bridge.cpp:2597). Creating the context is not
// enough: XeLL has to be enabled for Init to accept it.
namespace {
HMODULE               g_xell_dll = nullptr;
xell_context_handle_t g_xell     = nullptr;

bool attach_latency_reduction(ID3D12Device* device)
{
    if (g_xell != nullptr) { return true; }

    if (g_xell_dll == nullptr) {
        wchar_t dir[MAX_PATH]{};
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        if (wchar_t* slash = wcsrchr(dir, L'\\')) { *(slash + 1) = L'\0'; }

        // Our own folder first (OptiScaler keeps its vendor DLLs beside its own module),
        // then the title's, then the search path.
        wchar_t path[MAX_PATH]{};
        wcscpy_s(path, dir);
        wcscat_s(path, L"fakerFG\\libxell.dll");
        g_xell_dll = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (g_xell_dll == nullptr) {
            wcscpy_s(path, dir);
            wcscat_s(path, L"libxell.dll");
            g_xell_dll = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (g_xell_dll == nullptr) { g_xell_dll = LoadLibraryW(L"libxell.dll"); }
        if (g_xell_dll == nullptr) {
            logf("xell: libxell.dll not loadable (%lu) - XeFG will refuse init (-15)\n",
                 GetLastError());
            return false;
        }
        logf("xell: libxell.dll -> %p\n", (void*)g_xell_dll);
    }

    auto create = reinterpret_cast<decltype(&xellD3D12CreateContext)>(
        reinterpret_cast<void*>(GetProcAddress(g_xell_dll, "xellD3D12CreateContext")));
    auto sleep_mode = reinterpret_cast<decltype(&xellSetSleepMode)>(
        reinterpret_cast<void*>(GetProcAddress(g_xell_dll, "xellSetSleepMode")));
    if (create == nullptr) {
        logf("xell: xellD3D12CreateContext missing\n");
        return false;
    }

    const xell_result_t cr = create(device, &g_xell);
    logf("xell: D3D12CreateContext -> %d\n", static_cast<int>(cr));
    if (cr != XELL_RESULT_SUCCESS || g_xell == nullptr) { g_xell = nullptr; return false; }

    if (sleep_mode != nullptr) {
        xell_sleep_params_t sp{};
        sp.minimumIntervalUs = 0;
        sp.bLowLatencyMode   = 1;
        sp.bLowLatencyBoost  = 0;
        logf("xell: setSleepMode(lowLatency) -> %d\n",
             static_cast<int>(sleep_mode(g_xell, &sp)));
    }

    if (g_xapi.SetLatencyReduction != nullptr) {
        const xefg_swapchain_result_t lr =
            g_xapi.SetLatencyReduction(g_ctx, reinterpret_cast<void*>(g_xell));
        logf("xefg: SetLatencyReduction -> %s (%d)\n", result_name(lr), static_cast<int>(lr));
    } else {
        logf("xefg: libxess_fg has no SetLatencyReduction export\n");
    }
    return true;
}
} // namespace

// ---------------------------------------------------------------- no present-path work
// OptiScaler records every XeFG input where the title hands it over
// (XeFG_Dx12::SetResource, called from the input module during the frame) - on the
// frame's own command list. It records nothing on a list of its own for XeFG.

void before_present(void*);
void tag_frame_constants(const MgpuFgFrame& f, uint32_t frame_id);
void xefg_tag(ID3D12GraphicsCommandList* list, uint32_t frame_id, const MgpuFgFrame& f,
              bool upscaler_lane);
void xefg_frame_feedback(bool tags_complete);
bool xefg_tags_complete();

// IFGFeature::IsPaused() / UpdateTarget() (framegen/IFGFeature.cpp:154, 300): FG is paused
// for 10 frames after an event that invalidates its history. XeFG_Dx12::Dispatch() returns
// false while paused, and NewFrame() re-activates afterwards.
bool xefg_paused();
void xefg_pause(uint32_t frames);

// Called by the swap-chain facade after ResizeBuffers/ResizeBuffers1 went through:
// wrapped_swapchain.cpp:1099-1104 sets fgChanged and the feature then pauses, because the
// captured resources and the proxy chain's buffers are invalid from that point on.
void xefg_resize_notify();

// PresentHooks::after_resize -> the same pause, without exposing xefg_pause to the header.
void after_resize_hook(void*) { xefg_resize_notify(); }

// wrapped_swapchain.cpp:1212 (`DXGI_ERROR_DEVICE_REMOVED -> Util::GetDeviceRemovedReason`):
// the title only ever sees the HRESULT, so this is the only place the reason can be logged.
void present_failed(long hr, void*);

// ---------------------------------------------------------------- pause / re-activate
// IFGFeature::UpdateTarget() sets `_targetFrame = _frameCount + 10` and IsPaused() is
// `_targetFrame != 0 && _targetFrame >= _frameCount`; XeFG_Dx12::Dispatch() refuses to run
// while that holds, and NewFrame() calls Activate() once the counter passes it. Every place
// OptiScaler calls it (resize, interpolation-count change, hudless change) does
// `UpdateTarget(); Deactivate();` in one go.
uint32_t                 g_pause_until_frame = 0;   // _targetFrame
uint32_t                 g_present_counter = 0;     // _frameCount: advances on every present

bool xefg_paused()
{
    // Measured trap: comparing against g_frame_id (which only xefg_tag() advances) locks the
    // present path for good as soon as one pause is requested before the first tag - the
    // initial ResizeBuffers does exactly that. _frameCount in IFGFeature advances with the
    // frame flow, so the present counter is the right clock here.
    return g_pause_until_frame != 0 && g_present_counter < g_pause_until_frame;
}

void xefg_pause(uint32_t frames)
{
    if (frames == 0) { frames = 10; }
    g_pause_until_frame = g_present_counter + frames;
    // A tag recorded before the pause belongs to a frame the pause just invalidated; the
    // present after it must not reuse it (Dispatch() would pair live output with dead
    // resources - the loading-screen black screen).
    g_eval_tag_fresh = false;
    g_submit_frame_id = 0;                 // same for the evaluate-tagged lanes
    if (g_generate) { xefg_enable(false); }        // Deactivate()
    logf("xefg: paused for %u frames (UpdateTarget + Deactivate)\n", frames);
}

void xefg_resize_notify()
{
    // A resize invalidates the proxy chain's buffers and everything captured from them.
    // The DLSS-G backbuffer the title declared belongs to that same generation.
    xefg_pause(10);
}

// Upscaler_Inputs_Dx12 tags MV/depth inside the upscaler's own Evaluate, and
// XeFG_Dx12::Dispatch() - frame constants + SetPresentId - runs from the PRESENT path
// (FG_Hooks.cpp:1455 `fg->Present()`). Driving both from the DLSS-G Evaluate (what we did)
// only works while the title's frame generation goes through the real sl.dlss_g plugin;
// as soon as slDLSSGSetOptions is answered by us (FakeSlDlssg=1, Streamline_Hooks.cpp:437)
// that plugin never starts, no DLSS-G Evaluate arrives and nothing drives XeFG at all -
// "FG is on but nothing happens".
void present_dispatch()
{
    if (g_ctx == nullptr) { return; }

    MgpuFgFrame f{};
    uint32_t id = 0;

    // The evaluate-side tag only exists for FgInput=Upscaler (that is the lane whose
    // Evaluate carries the MV/depth); DLSSG keeps the present path below.
    if (cfg().tag_at_evaluate != 0 &&
        cfg().fg_input == static_cast<int>(FgInputSource::Upscaler)) {
        // TagAtEvaluate: MV/depth (and the colour, after the upscaler wrote it) were already
        // recorded inside the upscaler's own Evaluate - on the title's command list while it
        // was still OPEN (xefg_tag_at_evaluate). Present sends only what only present can
        // know: the frame constants and the present id (XeFG_Dx12::Dispatch, driven from
        // FG_Hooks.cpp:1455 `fg->Present()`).
        if (!xefg_wants_generation()) {
            g_eval_tag_fresh = false;          // FG switched off: drop the pending frame
            xefg_frame_feedback(false);
            return;
        }
        if (!g_eval_tag_fresh) {
            // No upscaler Evaluate since the last present: Dispatch()'s IsReady gate is not
            // met, so this frame stays 1X rather than pairing live output with the previous
            // frame's inputs.
            xefg_frame_feedback(false);
            return;
        }
        g_eval_tag_fresh = false;              // consumed by exactly this present
        f  = g_eval_frame;
        id = g_eval_frame_id;
        // Dispatch(): `if (!IsActive() || IsPaused()) return false;` - it never activates;
        // Activate() only happens in EvaluateState (XeFG_Dx12.cpp:583-610).
        if (!g_generate || !xefg_tags_complete()) { return; }

        tag_frame_constants(f, id);
        const xefg_swapchain_result_t r = g_xapi.SetPresentId(g_ctx, id);
        static int told_eval = 0;
        if (told_eval < 5) {
            ++told_eval;
            logf("xefg: present dispatch id=%u SetPresentId -> %s (%d) [tagged at evaluate]\n",
                 id, result_name(r), static_cast<int>(r));
        }
        // XeFG_Dx12.cpp:931-940: `SetPresentId error` -> UpdateTarget(); Deactivate();
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { xefg_pause(10); }
        return;
    }

    // FgInput=DLSSG / NVNGX / FSR: their inputs are tagged inside the title's Evaluate
    // (proxy_streamline.cpp / provider.cpp / proxy_ffx.cpp) on the command list that was
    // open there, so present only adds what only present can know - frame constants and
    // the present id - exactly like XeFG_Dx12::Dispatch(). One dispatch per accepted tag:
    // a second present without a new tag parks generation instead of interpolating stale
    // resources.
    if (cfg().fg_input != static_cast<int>(FgInputSource::Upscaler)) {
        const bool fresh = (g_submit_frame_id != 0) &&
                           (g_submit_frame_id == g_last_tagged_id) &&
                           (g_submit_dispatched_id != g_submit_frame_id);
        if (!fresh || !xefg_wants_generation() || !g_generate || !xefg_tags_complete()) {
            xefg_frame_feedback(false);
            return;
        }
        g_submit_dispatched_id = g_submit_frame_id;
        id = g_submit_frame_id;
        tag_frame_constants(g_last_frame, id);
        const xefg_swapchain_result_t r = g_xapi.SetPresentId(g_ctx, id);
        static int told_lane = 0;
        if (told_lane < 5) {
            ++told_lane;
            logf("xefg: present dispatch id=%u SetPresentId -> %s (%d) [tagged at evaluate,"
                 " lane=%d]\n", id, result_name(r), static_cast<int>(r), cfg().fg_input);
        }
        // XeFG_Dx12.cpp:931-940: `SetPresentId error` -> UpdateTarget(); Deactivate();
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { xefg_pause(10); }
        return;
    }

    if (!xefg_wants_generation() || !sr_apply(f)) {
        // No fresh upscaler inputs for this frame: stays 1X (XeFG_Dx12::Dispatch's
        // IsReady gate). xefg_frame_feedback(false) is what parks generation.
        xefg_frame_feedback(false);
        return;
    }

    // Upscaler_Inputs_Dx12.cpp:167-199 tags MV/depth with the upscaler's own command list and
    // ValidNow validity ("res.cmdList = InCmdList; res.validity = ValidNow;").
    // That placement is what TagAtEvaluate=1 reproduces inside the Evaluate itself; here the
    // upscaler's command list is CLOSED (UE5 executes it right after the evaluate), so only
    // UntilPresent tags are legal.
    ID3D12GraphicsCommandList* tag_list = nullptr;
    f.stream[MGPU_FG_STREAM_MV].validity    = MGPU_FG_RV_UNTIL_PRESENT;
    f.stream[MGPU_FG_STREAM_DEPTH].validity = MGPU_FG_RV_UNTIL_PRESENT;

    // XeFG's presentId must advance by exactly one per dispatched present: its internal
    // frame accounting is the same shape as OptiScaler's `_frameCount`/`willDispatchFrame`,
    // and a skipping id made the library answer SetPresentId OK while never generating
    // (measured: framesPresented=1, fgEnabled=0, result=OK with 180 upscaler Evaluates per
    // 120 presents). xefg_next_frame_id() is shared with the capture/record paths, which
    // consume ids without presenting - so this dispatch gets its own counter.
    static uint32_t dispatch_id = 0;
    id = ++dispatch_id;
    xefg_tag(tag_list, id, f, true);                // tags MV + depth (+ backbuffer/rect)
    xefg_frame_feedback(xefg_tags_complete());      // Activate() once the inputs are there
    if (!g_generate || !xefg_tags_complete()) { return; }

    tag_frame_constants(f, id);
    const xefg_swapchain_result_t r = g_xapi.SetPresentId(g_ctx, id);
    static int told = 0;
    if (told < 5) {
        ++told;
        logf("xefg: present dispatch id=%u SetPresentId -> %s (%d)\n", id, result_name(r),
             static_cast<int>(r));
    }
    // XeFG_Dx12.cpp:931-940: `SetPresentId error` -> UpdateTarget(); Deactivate();
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { xefg_pause(10); }
}

// Set by the DLSS-G provider: the buffer the title declared as its own backbuffer for this
// frame (borrowed, only used for the frame it came from) plus the evaluation that carried it,
// so the present path can tell a fresh frame from a stale one (the title cycles through
// several DLSS-G backbuffers; copying a stale one is one frame of lag = ghosting).
ID3D12Resource*    g_dlssg_ui = nullptr;
unsigned           g_dlssg_ui_state = 0xFFFFFFFFu;
void xefg_set_dlssg_ui(void* res, uint32_t state)
{
    g_dlssg_ui = static_cast<ID3D12Resource*>(res);
    g_dlssg_ui_state = state;
}

// The command-list ring the present-side shader passes (FGDrawUIOverFG) run on. Its own
// allocator/list/fence per slot, never waited on: the title's queue orders everything.
struct CopySlot
{
    ID3D12CommandAllocator*    alloc = nullptr;
    ID3D12GraphicsCommandList* list  = nullptr;
    ID3D12Fence*               fence = nullptr;
    UINT64                     value = 0;
};
CopySlot g_copy[4];
unsigned g_copy_next = 0;

CopySlot* copy_slot()
{
    CopySlot& s = g_copy[g_copy_next];
    if (s.alloc == nullptr && g_xdev != nullptr) {
        g_xdev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc));
        g_xdev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc, nullptr,
                                  IID_PPV_ARGS(&s.list));
        s.list->Close();
        g_xdev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence));
    }
    if (s.alloc == nullptr || s.list == nullptr || s.fence == nullptr) { return nullptr; }
    if (s.fence->GetCompletedValue() < s.value) { return nullptr; }   // still in flight
    g_copy_next = (g_copy_next + 1) % 4;
    return &s;
}

// Bridges the vtable detours use (fg_swapchain_hooks.cpp).
// The device the chain/XeFG run on. The upscaler lane's record has no device of its
// own (it is built from the parameter block), and the transport needs one to place
// its slots - this is the same device the title renders with.
ID3D12Device* xefg_device()
{
    return g_xdev;
}

void xefg_before_present(bool real_present)
{
    if (!real_present) { return; }
    // PBZ stages Streamline tags but may not call slEvaluateFeature(DLSS_G). The
    // PCL marker hook (hg_pcl_marker) publishes those staged resources at PresentStart;
    // do not publish again here.
    // Snapshot the record about to be presented. (v22: the present-time ring is gone.)
    // The present-time list records the scar copy (and, when enabled, draws the
    // marker) on the chain buffer. It must run BEFORE producer_present(), because
    // that is what signals the publication fence the consumer waits on - a fence
    // signalled first would let the consumer read a scar slot the GPU has not
    // filled yet.
    before_present(nullptr);
    // The title has submitted this frame's lists: that is the point where the
    // transport may signal its forward fence (the copies were recorded inside the
    // upscaler's Evaluate, on a list the title has since executed).
    producer_present(g_queue);
}

void xefg_present_result(long hr)
{
    if (hr == static_cast<long>(DXGI_ERROR_DEVICE_REMOVED) ||
        hr == static_cast<long>(DXGI_ERROR_DEVICE_RESET)) {
        present_failed(hr, nullptr);
    }
}

void xefg_after_resize() { after_resize_hook(nullptr); }

// ---------------------------------------------------------------- marker sidecar
// v17: the pre-marker 16x16 tile never leaves the GPU. The present-time list
// normalises the chain buffer into a 16x16 R10 texture and the producer copies
// that straight into the scar slot of THIS frame's publication
// (streams[kStreamScar], same slot as MV/depth). No readback, no Map, no worker,
// no Present stall - and "which frame does this patch belong to" is the slot.
// The 16x16 scar tile transport (scar_dbg_* / marker_norm_init / g_scar_norm) is gone:
// the marker is now a self-contained 32-bit code, nothing is copied off the chain
// buffer and nothing is transported to the consumer.
//
// xefg_set_marker_color() stays as a no-op: the upscaler lane still reports the colour
// resource it used (OptiScaler's harvest path calls it), but nothing consumes it any
// more now that the tile is not normalised.
void xefg_set_marker_color(void*, uint32_t, uint32_t, uint32_t, uint32_t)
{
}

// v22: colour for the producer. In the consume/upscaler lane the record has no
// Backbuffer/OutputReal (sr_apply() deliberately leaves them empty), and the frame the
// consumer must interpolate is the chain buffer the title renders into. GetBuffer gives
// us that resource; the producer records a copy now and executes it after the title's
// present fence, so the content is complete by then. The caller releases the reference
// after dispatch_sink(); the swap chain itself keeps the resource alive.
bool xefg_record_backbuffer(MgpuFgFrame* f)
{
    if (f == nullptr || g_chain_api == nullptr || !g_chain_ready) { return false; }
    if (f->stream[MGPU_FG_STREAM_BACKBUFFER].resource != nullptr) { return true; }
    const UINT idx = chain_current_index();
    ID3D12Resource* res = nullptr;
    if (FAILED(g_chain_api->GetBuffer(idx, IID_PPV_ARGS(&res))) || res == nullptr) {
        return false;
    }
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    MgpuFgResource& bb = f->stream[MGPU_FG_STREAM_BACKBUFFER];
    bb.resource = res;
    bb.state    = D3D12_RESOURCE_STATE_COMMON;   // PRESENT and COMMON are both 0
    bb.validity = MGPU_FG_RV_UNTIL_PRESENT;
    bb.width    = static_cast<uint32_t>(d.Width);
    bb.height   = d.Height;
    bb.format   = static_cast<uint32_t>(d.Format);
    if (f->output_w == 0 || f->output_h == 0) {
        f->output_w = bb.width;
        f->output_h = bb.height;
    }
    return true;
}

static bool marker_probe_enabled()
{
    static int on = -1;
    if (on < 0) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        // The consumer loads the game-directory dxgi proxy too. If fakerFG runs
        // inside xess_mfg_consumer.exe it would write a marker the consumer never
        // sees - and the outer proxy would draw a second one.
        if (wcsstr(exe, L"xess_mfg_consumer") != nullptr) {
            on = 0;
            logf("marker probe: disabled in xess_mfg_consumer.exe (outer proxy)\n");
            return false;
        }
        // Default ON: the marker is the exact colour<->MV pairing key. The scar
        // tile rides the publication as a 16x16 GPU copy, so there is no per-frame
        // CPU cost keeping it off. marker_disable.on opts out (A/B).
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (wchar_t* slash = wcsrchr(path, L'\\')) { *(slash + 1) = 0; }
        wcscat_s(path, MAX_PATH, L"marker_disable.on");
        on = (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) ? 0 : 1;
        logf("marker probe: %s (opt-out file %ls)\n", on ? "ON (default)" : "off", path);
    }
    return on != 0;
}

// code_lo = seq32, code_hi = crc32(seq). The shader paints one bit per 2x2 cell,
// so the full 16x16 block carries the 64 bits exactly once (no tiling), which is
// far more tolerant of DWM scaling/filtering than 1-pixel cells.
// The marker carries a SHORT SNOWFLAKE id: sequence12 | low12(ms), plus a CRC-8
// (mgpu::marker_code32). A full 64-bit snowflake would not fit the 144-px budget and
// its 41-bit timestamp is redundant here - the consumer resolves the exact frame id
// from the present ring, so 24 bits only has to be different on every frame.
static uint32_t marker_code(uint64_t seq)
{
    const uint64_t id = mgpu::snowflake_id(now_qpc_ms(), static_cast<uint32_t>(seq));
    if ((seq & 0xFFFu) != mgpu::marker_seq_from_payload(mgpu::marker_payload_from_id(id))) {
        logf("marker: snowflake sequence mismatch (seq=%llu)\n",
             static_cast<unsigned long long>(seq));
    }
    return mgpu::marker_code32(mgpu::marker_payload_from_id(id));
}

static bool marker_file_exists(const wchar_t* name)
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) { *(slash + 1) = 0; }
    wcscat_s(path, MAX_PATH, name);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}
// marker_norm_debug_mode() removed with the norm pass.


static bool consumer_present_event()
{
    // Runs once per Present (40-120x/s) on the GAME's thread: OpenEventW + CloseHandle are
    // two syscalls. The answer only changes at session boundaries, so a positive result is
    // cached for 1 s. A negative result is still re-probed every frame, so the marker
    // starts as soon as the consumer window appears (a 1 s hole there would be ~40
    // unpaired frames).
    static ULONGLONG cached_ms = 0;
    static bool cached = false;
    if (cached) {
        const ULONGLONG now = GetTickCount64();
        if (cached_ms != 0 && now - cached_ms < 1000) { return true; }
    }
    HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, L"Local\\mgpu_consumer_present_v1");
    cached = (ev != nullptr);
    if (ev != nullptr) { CloseHandle(ev); }
    cached_ms = GetTickCount64();
    return cached;
}

// marker_disable.on is an A/B switch the user drops next to the exe before launching.
// GetFileAttributesW goes through the filesystem (and any AV filter) - tens of
// microseconds - so it is read once per second, not once per Present.
static bool marker_opt_out()
{
    static ULONGLONG checked_ms = 0;
    static bool off = false;
    const ULONGLONG now = GetTickCount64();
    if (checked_ms != 0 && now - checked_ms < 1000) { return off; }
    checked_ms = now;
    off = marker_file_exists(L"marker_disable.on");
    return off;
}

static void marker_probe_present()
{
    if (!marker_probe_enabled() || g_chain_api == nullptr || g_queue == nullptr) { return; }
    if (g_pending_seq == 0) { return; }
    if (!consumer_present_event()) { return; }   // no consumer: no pairing marker cost
    if (marker_opt_out()) { return; }
    const UINT idx = chain_current_index();
    ID3D12Resource* dst = nullptr;
    if (FAILED(g_chain_api->GetBuffer(idx, IID_PPV_ARGS(&dst))) || dst == nullptr) { return; }
    const D3D12_RESOURCE_DESC dd = dst->GetDesc();
    if (dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.Width < 64) {
        dst->Release(); return;
    }
    CopySlot* cs = copy_slot();
    if (cs == nullptr) { dst->Release(); return; }

    // Flip-model Present requires PRESENT; pass_render_ui (FGDrawUIOverFG) ends
    // there too. The marker is drawn at FOUR quadrant positions, all carrying the
    // same code, so a HUD element over one of them cannot break pairing.
    const D3D12_RESOURCE_STATES chain_state = D3D12_RESOURCE_STATE_PRESENT;
    const uint32_t code = marker_code(g_pending_seq);

    cs->alloc->Reset();
    cs->list->Reset(cs->alloc, nullptr);
    D3D12_RESOURCE_BARRIER mb{};
    mb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    mb.Transition.pResource = dst;
    mb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mb.Transition.StateBefore = chain_state;
    mb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cs->list->ResourceBarrier(1, &mb);

    // ONE full-screen draw: the pixel shader places the four corner blocks itself and
    // discards everything else. Four separate scissored draws only ever landed on the
    // first corner (corner 0 measured contrast 4.6, the other three exactly 0.0).
    const bool ok = pass_marker(cs->list, dst, static_cast<UINT>(dd.Width),
                               static_cast<UINT>(dd.Height), code);
    {
        static UINT draws = 0;
        if ((draws++ % 240u) == 0u) {
            uint32_t mxs[mgpu::kMarkerCorners]{}, mys[mgpu::kMarkerCorners]{};
            for (uint32_t i = 0; i < mgpu::kMarkerCorners; ++i) {
                mgpu::marker_corner(i, static_cast<uint32_t>(dd.Width),
                                    static_cast<uint32_t>(dd.Height), &mxs[i], &mys[i]);
            }
            logf("marker draw: #0(%u,%u) #1(%u,%u) #2(%u,%u) #3(%u,%u) code=0x%08X"
                 " one-pass ok=%d size=%ux%u\n",
                 mxs[0], mys[0], mxs[1], mys[1], mxs[2], mys[2], mxs[3], mys[3],
                 code, ok ? 1 : 0,
                 static_cast<unsigned>(dd.Width), static_cast<unsigned>(dd.Height));
        }
    }

    mb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    mb.Transition.StateAfter = chain_state;
    cs->list->ResourceBarrier(1, &mb);
    cs->list->Close();
    if (!ok) { dst->Release(); return; }
    g_queue->ExecuteCommandLists(1,
        reinterpret_cast<ID3D12CommandList* const*>(&cs->list));
    ++cs->value;
    g_queue->Signal(cs->fence, cs->value);
    dst->Release();
}

// FgSwapChain's Present() -> this -> the real Present, which is where OptiScaler's
// WrappedIDXGISwapChain4::LocalPresent runs fg->Present(): frame constants + present id
// for the frame that is being presented, then the title's present.
void before_present(void*)
{
    ++g_present_counter;                 // _frameCount: the clock IsPaused() compares against
    // FGDrawUIOverFG -> RUI_Dx12 (XeFG_Dx12.cpp:1265-1300): draw the UI the title declared
    // over the chain buffer it is about to present, on our own list, before Present.
    if (cfg().fg_draw_ui_over_fg != 0 && g_dlssg_ui != nullptr && g_chain_api != nullptr) {
        const UINT idx = chain_current_index();
        ID3D12Resource* dst = nullptr;
        if (SUCCEEDED(g_chain_api->GetBuffer(idx, IID_PPV_ARGS(&dst))) && dst != nullptr) {
            const D3D12_RESOURCE_DESC d = dst->GetDesc();
            CopySlot* slot = copy_slot();
            if (slot != nullptr) {
                slot->alloc->Reset();
                slot->list->Reset(slot->alloc, nullptr);
                if (pass_render_ui(slot->list, dst, static_cast<UINT>(d.Width), d.Height,
                                   g_dlssg_ui, g_dlssg_ui_state)) {
                    slot->list->Close();
                    g_queue->ExecuteCommandLists(
                        1, reinterpret_cast<ID3D12CommandList* const*>(&slot->list));
                    ++slot->value;
                    g_queue->Signal(slot->fence, slot->value);
                } else {
                    slot->list->Close();
                }
            }
            dst->Release();
        }
    }

    // lock_frame: 每帧 present 前画四角 marker。producer 已把这一帧的 seq 写进
    // FrameShared.frame_id；消费端 WGC 抓到画面后解出 seq12+ms12，与发布精确匹配。
    // marker_disable.on 仍可关闭（A/B）。
    marker_probe_present();

    // XeFG_Dx12::Present(): `if ((_fgFramePresentId - _lastFGFramePresentId) > 3 && IsActive()
    // && !_waitingNewFrameData) { Deactivate(); ... }` and Dispatch() itself refuses while
    // paused. Nothing is submitted to the library in that state - the title's frame goes
    // straight through, which is what keeps a loading screen or a resize visible.
    if (g_ctx == nullptr || xefg_paused()) { return; }

    // The interval between two presents is the frame time XeFG paces against.
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    if (g_last_present_qpc != 0 && freq.QuadPart != 0) {
        g_frame_ms = static_cast<float>(now.QuadPart - g_last_present_qpc) * 1000.0f /
                     static_cast<float>(freq.QuadPart);
        // State::lastFGFrameTime is only updated while there is no FG running; no clamping
        // or filtering of its own.
        if (!g_generate) { g_frame_ms_no_fg = g_frame_ms; }
    }

    // First few presents verbatim: the interval says whether the title presents at all while
    // the startup hitch is happening (a missing present line used to be indistinguishable
    // from a very slow one).
    {
        static int told = 0;
        if (told < 8) {
            ++told;
            logf("present: #%d after %.1f ms (generate=%d paused=%d)\n", told,
                 g_frame_ms, g_generate ? 1 : 0, xefg_paused() ? 1 : 0);
        }
    }
    g_last_present_qpc = now.QuadPart;

    // Present cadence: a smooth 2x shows a tight distribution around half the game's
    // frame time. A bimodal/long tail means pacing, not our GPU work.
    {
        static double sum = 0.0, mn = 1e9, mx = 0.0;
        static unsigned n = 0, long_ones = 0;
        if (g_frame_ms > 0.01f) {
            const double ms = g_frame_ms;
            sum += ms; ++n;
            if (ms < mn) { mn = ms; }
            if (ms > mx) { mx = ms; }
            if (n > 8 && ms > 2.5 * (sum / n)) { ++long_ones; }
            if (n >= 120) {
                logf("present: %u frames avg=%.2f min=%.2f max=%.2f ms, %u long\n",
                     n, sum / n, mn, mx, long_ones);
                sum = 0.0; mn = 1e9; mx = 0.0; n = 0; long_ones = 0;
            }
        }
    }

    // LocalPresent(): "Update swapchain info every frame" -
    //     if (isCurrent && pSwapChain->GetDesc(&State::Instance().currentSwapchainDesc) != S_OK)
    //         LOG_WARN("Can't get swapchain desc!");
    // which is what keeps the interpolation rect/tag sizes right after a resolution change.
    if (g_chain != nullptr) {
        DXGI_SWAP_CHAIN_DESC real{};
        if (g_chain->GetDesc(&real) == S_OK) {
            if (real.BufferDesc.Width != g_width || real.BufferDesc.Height != g_height) {
                logf("xefg: swapchain is now %ux%u (was %ux%u) - tags follow it\n",
                     real.BufferDesc.Width, real.BufferDesc.Height, g_width, g_height);
                g_width  = real.BufferDesc.Width;
                g_height = real.BufferDesc.Height;
            }
        } else {
            static bool warned_desc = false;
            if (!warned_desc) {
                warned_desc = true;
                logf("xefg: can't get swapchain desc!\n");
            }
        }
    }

    // SetPresentId describes the frame that is being presented. OptiScaler calls it from
    // Dispatch() on EVERY present - including its 1X state, which is `SetEnabled(0)` with
    // the FG object still active. A run of presents without a new tagged frame means the
    // title stopped evaluating; XeFG would then keep using stale data, so we drop to 1X.
    if (g_ctx == nullptr || g_xapi.SetPresentId == nullptr) { return; }

    // 1X diagnostic: with generation off, XeFG's chain still has to composite the title's
    // back buffer onto the window. A frozen framesPresented while our own present counter
    // keeps moving is exactly the black-screen shape, so it must be visible in the log.
    // It runs in both states on purpose: while generating, frameGenResult carries the
    // library's own rejection code (-12 MISMATCH_INPUT_RESOURCES and friends). Skip it in
    // the Capture lane: local XeFG is only watching the title's chain there and its 1X
    // status is expected, not diagnostic.
    if (cfg().fg_output == static_cast<int>(FgOutputSink::XeFG) &&
        g_xapi.GetLastPresentStatus != nullptr) {
        static unsigned polls = 0, seen = 0, no_input = 0, last_harvests = 0;
        ++polls;
        if (cfg().fg_input == static_cast<int>(FgInputSource::Upscaler) &&
            polls % 120 == 0) {
            const unsigned h = sr_harvest_count();
            logf("xefg: upscaler Evaluates %u in the last 120 presents (total %u)\n",
                 h - last_harvests, h);
            last_harvests = h;
        }
        if (polls % 240 == 0) {
            xefg_swapchain_present_status_t st{};
            if (g_xapi.GetLastPresentStatus(g_ctx, &st) == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                logf("xefg: %s present status framesPresented=%u (was %u) fgEnabled=%u"
                     " result=%s (%d) srFresh=%d\n",
                     g_generate ? "2X" : "1X", st.framesPresented, seen,
                     st.isFrameGenEnabled, result_name(st.frameGenResult),
                     static_cast<int>(st.frameGenResult), sr_lane_ready() ? 1 : 0);
                seen = st.framesPresented;
            }
            // The lane is configured but nothing ever arrived: with FgInput=Upscaler that
            // means the title never evaluated DLSS-SR (DLSS off in the menu), not a broken
            // link - say so once instead of leaving the reader to infer it.
            if (cfg().fg_input == static_cast<int>(FgInputSource::Upscaler) &&
                !sr_lane_ready() && no_input < 2) {
                ++no_input;
                logf("xefg: FgInput=Upscaler but no upscaler Evaluate has been seen - turn"
                     " DLSS (not TAA/TSR) on in the game; FG stays at 1X until then\n");
            }
        }
    }

    // Present():
    //     if ((_fgFramePresentId - _lastFGFramePresentId) > 3 && IsActive() && !_waitingNewFrameData)
    //     { Deactivate(); _waitingNewFrameData = true; return false; }
    //     _fgFramePresentId++;
    ++g_fg_frame_present_id;
    if ((g_fg_frame_present_id - g_last_fg_frame_present_id) > 3 && g_generate &&
        !g_waiting_new_frame_data) {
        g_waiting_new_frame_data = true;
        xefg_enable(false);
        logf("present: %u presents without a dispatch - Deactivate()\n",
             g_fg_frame_present_id - g_last_fg_frame_present_id);
        return;
    }

    // Everything else is Dispatch(): tags from the upscaler lane, frame constants, present
    // id. `xefg_tag` refreshes _lastFGFramePresentId, which is what the >3 rule above tests.
    present_dispatch();
}


bool xefg_start(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGIFactory2* factory,
                HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, bool title_fullscreen,
                IDXGISwapChain1** out_chain)
{
    // Already ours: hand the SAME facade back instead of returning true with a null chain,
    // which the caller can only treat as "keep the title's own chain".
    if (g_chain_ready) {
        if (g_chain_api != nullptr && out_chain != nullptr &&
            (g_owner_hwnd == nullptr || g_owner_hwnd == hwnd)) {
            g_facade->AddRef();
            *out_chain = static_cast<IDXGISwapChain1*>(g_facade);
            logf("xefg: already ours - handing the same facade %p again\n",
                 static_cast<void*>(g_facade));
            return true;
        }
        return true;
    }
    if (device == nullptr || queue == nullptr || factory == nullptr ||
        hwnd == nullptr || desc == nullptr || out_chain == nullptr) {
        logf("xefg: start needs device/queue/factory/hwnd/desc - got %p/%p/%p/%p/%p\n",
             (void*)device, (void*)queue, (void*)factory, (void*)hwnd, (void*)desc);
        return false;
    }
    // Re-entrant create: this is libxess's OWN CreateSwapChainForHwnd, called from inside
    // D3D12InitFromSwapChainDesc on the thread below. It MUST pass through to the real DXGI -
    // that call is what creates the chain we later wrap. Waiting for the outer call here
    // blocks the very call it waits for (measured: 20 s added to a 2 s init).
    if (g_starting) {
        logf("xefg: nested create from the library - passing through to the real DXGI\n");
        return false;
    }
    g_starting = true;

    struct Guard { std::atomic<bool>& f; ~Guard() { f = false; } } guard{ g_starting };

    // The XeFG context is created once and kept (OptiScaler: `_swapChainContext` is
    // memoised in CreateSwapchainContext and reused by every later attempt).
    std::string err;
    if (!xefg_dyn::Load(g_xapi, L"", &err)) {
        logf("xefg: libxess_fg not loadable (%s) - FG output stays off\n", err.c_str());
        return false;
    }
    logf("xefg: libxess_fg %ls\n", g_xapi.path.c_str());

    xefg_swapchain_version_t ver{};
    if (g_xapi.GetVersion != nullptr && g_xapi.GetVersion(&ver) == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        logf("xefg: version %u.%u.%u\n", ver.major, ver.minor, ver.patch);
    }

    g_xdev = device;
    g_queue = queue;
    mv_resample_set_device(device);
    pass_set_device(device);

    xefg_swapchain_result_t r = XEFG_SWAPCHAIN_RESULT_SUCCESS;
    if (g_ctx == nullptr) {
        r = g_xapi.D3D12CreateContext(device, &g_ctx);
        logf("xefg: D3D12CreateContext -> %s (%d)\n", result_name(r), static_cast<int>(r));
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS || g_ctx == nullptr) { g_ctx = nullptr; return false; }

        if (g_xapi.SetLoggingCallback != nullptr) {
            g_xapi.SetLoggingCallback(g_ctx, XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG, log_cb, nullptr);
        }

        xefg_swapchain_properties_t props{};
        if (g_xapi.GetProperties != nullptr &&
            g_xapi.GetProperties(g_ctx, &props) == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            g_max_interp = props.maxSupportedInterpolations ? props.maxSupportedInterpolations : 1;
            logf("xefg: maxSupportedInterpolations=%u descriptors=%u\n",
                 g_max_interp, props.requiredDescriptorCount);
        } else {
            g_max_interp = 1;
            logf("xefg: GetProperties failed - assuming 1 interpolated frame\n");
        }

        // Before Init, or XeFG answers -15 (see attach_latency_reduction).
        attach_latency_reduction(device);
    } else {
        logf("xefg: reusing the existing XeFG context for this swap chain\n");
    }

    const Config& c = cfg();
    g_want_generate = (c.fg_output == static_cast<int>(FgOutputSink::XeFG)) &&
                      c.passthrough == 0;
    g_generate = false;                          // the context starts in 1X (XeFGInputs::PassInit)

    // OptiScaler's parameter set (XeFG_Dx12::CreateSwapchain*): node masks left 0 (their
    // `params{}` default), the UI-alpha flag driven by config, and a full-screen desc
    // that is always built even when the title does not pass one.
    xefg_swapchain_d3d12_init_params_t ip{};
    ip.pApplicationSwapChain = nullptr;      // Desc route: XeFG creates the chain
    // CreateSwapchain*Internal(): intTarget = _maxInterpolationCount (SetNumInterpolatedFrames
    // exists), `params.maxInterpolatedFrames = intTarget`, and the real count is applied
    // afterwards with SetNumInterpolatedFrames.
    ip.maxInterpolatedFrames = (g_max_interp > 0) ? g_max_interp : 1u;
    ip.initFlags             = XEFG_SWAPCHAIN_INIT_FLAG_NONE;
    if (c.depth_inverted)  { ip.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH; }
    if (c.mv_jittered)     { ip.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV; }
    if (c.high_res_mv && c.mv_resample == 0) {
        ip.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV;   // not when we resample them
    }
    if (c.ui_premultiplied == 0) {
        ip.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_UITEXTURE_NOT_PREMUL_ALPHA;
    }
    ip.creationNodeMask = 0;
    ip.visibleNodeMask  = 0;
    // uiMode stays 0 = XEFG_SWAPCHAIN_UI_MODE_AUTO, exactly like OptiScaler's `params{}`:
    // AUTO resolves from the inputs that were actually tagged (hudless/UI), so with no UI
    // texture tagged it behaves as NONE. No config knob - theirs has none either.
    ip.uiMode           = XEFG_SWAPCHAIN_UI_MODE_AUTO;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_local{};

    // The title's description is handed to the library verbatim. Rebuilding it the way
    // XeFG_Dx12.cpp:296-331 does (AlphaMode = IGNORE, Stereo = false, mapped Scaling) was
    // measured as a regression on CP2077: every present then answers
    // -12 MISMATCH_INPUT_RESOURCES, while the title's own desc gives result=OK /
    // framesPresented=2. The title passes AlphaMode=UNSPECIFIED, Scaling=STRETCH,
    // Stereo=0, 2 buffers, FLIP_DISCARD, flags 0x840.
    (void)fs_local;

    logf("xefg: init %ux%u fmt=%u flags=0x%X maxInterpolated=%u uiMode=%d\n",
         desc->Width, desc->Height, static_cast<unsigned>(desc->Format),
         static_cast<unsigned>(ip.initFlags), ip.maxInterpolatedFrames,
         static_cast<int>(ip.uiMode));

    const uint64_t t_init = GetTickCount64();

    // OptiScaler's proxy creates the chain itself and passes it as pApplicationSwapChain
    // (inputs/FG/XeFG_Inputs_Dx12.cpp:370-392). Measured with this libxess build (1.3.1):
    // the library IGNORES that chain, creates its own (the nested `count=3`
    // CreateSwapChainForHwnd inside InitFromSwapChain), destroys the app chain and returns
    // its own object from GetSwapChainPtr. Handing the app chain to the title afterwards is
    // an access violation with `rip = <app chain address>`. The Desc route stays - it is the
    // only one this build actually supports.
    r = g_xapi.D3D12InitFromSwapChainDesc(g_ctx, hwnd, desc, fs, queue, factory, &ip);
    logf("xefg: D3D12InitFromSwapChainDesc -> %s (%d) in %llu ms\n", result_name(r),
         static_cast<int>(r), static_cast<unsigned long long>(GetTickCount64() - t_init));
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        // Keep the context: OptiScaler memoises it (`XeFG_Dx12.cpp:406-410`
        // `if (_swapChainContext == nullptr) CreateSwapchainContext(device);`) and only
        // returns false. Destroying it here made the next CreateSwapChain - UE5 always makes
        // two at startup - pay `D3D12CreateContext` + descriptor heaps again, which is the
        // measured ~1 s startup hitch (10.426 -> 11.422 in the Wuchang log).
        return false;
    }

    IDXGISwapChain1* chain = nullptr;
    r = g_xapi.D3D12GetSwapChainPtr(g_ctx, __uuidof(IDXGISwapChain1),
                                   reinterpret_cast<void**>(&chain));
    logf("xefg: D3D12GetSwapChainPtr -> %s (%d), chain=%p\n",
         result_name(r), static_cast<int>(r), static_cast<void*>(chain));
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS || chain == nullptr) {
        return false;      // same: the context stays for the next attempt
    }

    g_chain  = chain;
    g_chain_ready = true;
    g_width  = desc->Width;
    g_height = desc->Height;
    g_chain_fmt = static_cast<uint32_t>(desc->Format);
    g_owner_hwnd = hwnd;

    // No SetEnabled() here: a new XeFG context starts with generation off until the app
    // enables it (XeFGInputs::PassInit), which is what Activate() does later. The count is
    // applied at that point too, like Dispatch().
    logf("xefg: context ready - generation stays off until the first complete frame\n");

    // OptiScaler's default (0.7): how big a change has to be before the library drops
    // its history. Cheap, and part of what keeps generated frames stable.
    if (g_xapi.SetSceneChangeThreshold != nullptr) {
        const xefg_swapchain_result_t sc = g_xapi.SetSceneChangeThreshold(g_ctx, 0.7f);
        logf("xefg: SetSceneChangeThreshold(0.7) -> %s (%d)\n",
             result_name(sc), static_cast<int>(sc));
    }

    // Dispatch() calls these every frame; the values only change with the config, so once
    // after init is the same thing without a per-frame call.
    if (g_xapi.EnableDebugFeature != nullptr) {
        g_xapi.EnableDebugFeature(g_ctx, XEFG_SWAPCHAIN_DEBUG_FEATURE_TAG_INTERPOLATED_FRAMES,
                                  cfg().debug_view != 0, nullptr);
        // Their XEFG_SWAPCHAIN_DEBUG_FEATURE_SHOW_ONLY_INTERPOLATION (state.fgOnlyGenerated):
        // only the generated frames are shown, which is the strongest check that generation
        // runs at all.
        g_xapi.EnableDebugFeature(g_ctx, XEFG_SWAPCHAIN_DEBUG_FEATURE_SHOW_ONLY_INTERPOLATION,
                                  cfg().debug_only_generated != 0, nullptr);
        if (cfg().debug_view != 0 || cfg().debug_only_generated != 0) {
            logf("xefg: debug view on (tag=%d only-generated=%d)\n", cfg().debug_view,
                 cfg().debug_only_generated);
        }
    }

    // OptiScaler's shape (RealSwapchainObject=1): hand the title the library's own object and
    // detour its vtable. Everything the title does then has the native object identity.
    if (cfg().real_swapchain_object != 0) {
        g_chain_api = chain;
        fg_hook_swapchain(chain);
        chain->AddRef();
        g_facade = nullptr;
        g_owner_hwnd = hwnd;
        *out_chain = static_cast<IDXGISwapChain1*>(chain);
        logf("xefg: the title keeps the library's swap chain object %p (hooked)\n",
             static_cast<void*>(chain));
        return true;
    }

    // The game gets our facade, never the backend's object (OptiScaler's shape).
    g_facade = new FgSwapChain(chain, PresentHooks{ &before_present, &present_failed,
                                                    &after_resize_hook, nullptr },
                               title_fullscreen);
    g_chain_api = static_cast<IDXGISwapChain*>(g_facade);
    *out_chain = static_cast<IDXGISwapChain1*>(g_facade);
    logf("xefg: the swap chain is ours now (%ux%u) - generation %s\n",
         g_width, g_height, g_generate ? "ON (local)" : "OFF (idle/bridge)");
    return true;
}

bool xefg_wants_generation()
{
    // FgOutput must be XeFG and an input lane must be selected (OptiScaler's dllmain:
    // "an FG Input without an FG Output, using none"). Capture never generates here.
    return g_want_generate && cfg().fg_output == static_cast<int>(FgOutputSink::XeFG) &&
           cfg().fg_input != 0;
}

// Capture owns the presentation chain: one published native record is the only frame
// allowed through the real DXGI Present. The title's DLSSG plugin may still call
// Present for generated frames; those calls are acknowledged but not forwarded.
void xefg_native_frame()
{
    // Present gating is only needed when the real NVNGX/Streamline caller can emit
    // extra generated presents. DLSSG input has that plugin stripped, so gating it
    // would deadlock the game's own present path.
    if (cfg().fg_output == static_cast<int>(FgOutputSink::Capture) &&
        cfg().fg_input == static_cast<int>(FgInputSource::NVNGX)) {
        g_native_seen.store(true, std::memory_order_release);
        g_native_pending.store(1, std::memory_order_release);
        g_native_last_tick.store(GetTickCount64(), std::memory_order_release);
    }
}

bool xefg_should_present()
{
    if (cfg().fg_output != static_cast<int>(FgOutputSink::Capture) ||
        cfg().fg_input != static_cast<int>(FgInputSource::NVNGX)) {
        return true;
    }
    // Startup/loading has no published record yet: never gate those presents.
    if (!g_native_seen.load(std::memory_order_acquire)) { return true; }
    // The gate exists to suppress the title's *generated* presents while the real DLSS-G
    // plugin runs. If the record stream stops (FG switched off, provider never evaluates,
    // NVNGX lane used without a DLSS-G Evaluate), gating every present would freeze the
    // window - measured as "游戏启动卡死". Release it after a short silence and keep
    // presenting; a later record re-arms the gate.
    {
        const uint64_t last = g_native_last_tick.load(std::memory_order_acquire);
        if (last == 0 || (GetTickCount64() - last) > 500) {
            if (g_native_seen.exchange(false, std::memory_order_acq_rel)) {
                logf("xefg: no native record for >500 ms - capture present gate released"
                     " (presents continue instead of freezing)\n");
            }
            return true;
        }
    }
    if (g_native_pending.exchange(0, std::memory_order_acq_rel) != 0) { return true; }
    static unsigned dropped = 0;
    if (++dropped <= 5 || (dropped % 600) == 0) {
        logf("xefg: capture present dropped (no new native record, total %u)\n", dropped);
    }
    return false;
}

void xefg_capture_disable()
{
    g_native_seen.store(false, std::memory_order_release);
    g_native_pending.store(0, std::memory_order_release);
    g_native_last_tick.store(0, std::memory_order_release);
}

// Capture output: a native publication is the only frame that may reach the display.
// The title's own DLSSG still calls Present for its generated frames; those calls are
// acknowledged below but the real Present is skipped, so the game window keeps the
// native rate (and the scar is drawn on that frame).

// sl::Constants' camera frame (Sl_Inputs::CaptureConstants in OptiScaler). tag_frame_constants
// builds the view matrix from it, exactly like Dispatch() does.
void xefg_camera(const float right[3], const float up[3], const float forward[3],
                 const float position[3])
{
    if (right == nullptr || up == nullptr || forward == nullptr || position == nullptr) { return; }
    memcpy(g_cam_right, right, sizeof(g_cam_right));
    memcpy(g_cam_up, up, sizeof(g_cam_up));
    memcpy(g_cam_forward, forward, sizeof(g_cam_forward));
    memcpy(g_cam_pos, position, sizeof(g_cam_pos));
    g_cam_known = true;
}

// The device died inside our present path. Log the reason once and stop feeding the
// library: every further tag/present on a removed device is wasted work (and the shape
// of the 4-5 s per present we measured earlier).
void present_failed(long hr, void*)
{
    static bool told = false;
    if (told) { return; }
    told = true;

    const char* reason = "?";
    if (g_xdev != nullptr) {
        switch (g_xdev->GetDeviceRemovedReason()) {
        case S_OK:                          reason = "S_OK (not removed?)"; break;
        case DXGI_ERROR_DEVICE_HUNG:        reason = "DEVICE_HUNG"; break;
        case DXGI_ERROR_DEVICE_REMOVED:     reason = "DEVICE_REMOVED"; break;
        case DXGI_ERROR_DEVICE_RESET:       reason = "DEVICE_RESET"; break;
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: reason = "DRIVER_INTERNAL_ERROR"; break;
        case DXGI_ERROR_INVALID_CALL:       reason = "INVALID_CALL"; break;
        default:                            reason = "other"; break;
        }
    }
    logf("present: xefg failed 0x%08X, device reason: %s - generation off\n",
         static_cast<unsigned>(hr), reason);
    g_want_generate = false;
    xefg_enable(false);
}

bool xefg_prepare_recreate()
{
    if (g_chain == nullptr && g_ctx == nullptr) { return true; }
    // Do NOT Destroy(g_ctx): the game may still Present through the old facade/chain.
    // Retire it until shutdown and build the new context; the library allows a second
    // context for the new chain. (See g_retired_ctx.)
    if (g_ctx != nullptr) {
        if (g_retired_n < 8) {
            g_retired_ctx[g_retired_n++] = g_ctx;
            logf("xefg: FGPreserveSwapChain=false - retired old context (kept alive until"
                 " shutdown, %u total); creating a new chain\n", g_retired_n);
        } else {
            logf("xefg: recreate: retire list full - refusing to drop an old context\n");
            return false;
        }
    }
    g_ctx = nullptr;
    g_chain = nullptr;
    g_chain_api = nullptr;
    g_facade = nullptr;       // the game still owns the old facade object
    fg_reset_swapchain_hook();
    g_chain_ready = false;
    g_owner_hwnd = nullptr;
    g_chain_fmt = 0;
    g_generate = false;
    return true;
}


// FGPreserveSwapChain: hand the same window the chain we already own. XeFG has no second
// chain, and a title that re-creates its chain (settings change, mode switch) would
// otherwise render into a chain nobody presents.
IDXGISwapChain1* xefg_owned_chain(HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc)
{
    if (g_chain_api == nullptr || hwnd == nullptr || hwnd != g_owner_hwnd) { return nullptr; }

    if (desc != nullptr && (desc->Width != g_width || desc->Height != g_height) &&
        desc->Width >= 8 && desc->Height >= 8) {
        // Same as their branch: resize the chain we own instead of building a new one.
        const HRESULT hr = g_chain_api->ResizeBuffers(0, desc->Width, desc->Height, desc->Format,
                                                     desc->Flags);
        logf("dxgi: reusing our chain - ResizeBuffers(%ux%u) -> 0x%08X\n", desc->Width,
             desc->Height, static_cast<unsigned>(hr));
        if (SUCCEEDED(hr)) {
            g_width  = desc->Width;
            g_height = desc->Height;
            g_chain_fmt = static_cast<uint32_t>(desc->Format);
        }
    }
    g_chain_api->AddRef();
    IDXGISwapChain1* out = nullptr;
    g_chain_api->QueryInterface(IID_PPV_ARGS(&out));
    g_chain_api->Release();
    return out;
}

// OptiScaler's Dispatch() gate: both Depth and Velocity must be ready for the frame
// that is about to be presented (XeFG_Dx12::Dispatch lines 696-699).
bool xefg_tags_complete() { return g_tags_had_depth && g_tags_had_mv && g_tags_size_ok; }

// xefg_swapchain_frame_constant_data_t for the frame being presented. OptiScaler sends
// this from Dispatch() (TagFrameConstants) right before SetPresentId; `frameRenderTime`
// is the interval between the last two presents, which is only known here.
// XeFG_Dx12::Dispatch(), the frame constants block, verbatim - and this is where we had
// two real divergences:
//
//   * OptiScaler does NOT use the game's view-to-clip matrix. It builds the projection
//     from the camera parameters ("Cyberpunk seems to be sending LH so do the same"):
//         XMMatrixPerspectiveFovLH(_cameraVFov[fIndex], _cameraAspectRatio[fIndex],
//                                  _cameraNear[fIndex], _cameraFar[fIndex])
//     after `if (FGXeFGDepthInverted) std::swap(_cameraNear[fIndex], _cameraFar[fIndex]);`
//   * it builds the view matrix from cameraRight/Up/Forward/Position when the position is
//     not zero; the combined matrix the title tags is only used for the 2D/prev reconstructions
//     (`Sl_Inputs`'s reprojection), never as the projection.
void tag_frame_constants(const MgpuFgFrame& f, uint32_t frame_id)
{
    if (g_ctx == nullptr || g_xapi.TagFrameConstants == nullptr) { return; }

    xefg_swapchain_frame_constant_data_t cd{};

    // viewMatrix, exactly as above (zero when the title declared no camera position).
    // Prefer the frame's own record (Streamline fills it; the NGX DLSSG lane sets it from
    // CameraPos/Right/Up/Fwd). Fall back to the last sl::Constants camera for lanes whose
    // record does not carry a basis (e.g. the upscaler lane).
    const bool rec_cam =
        (f.camera_right[0] != 0.0f || f.camera_right[1] != 0.0f || f.camera_right[2] != 0.0f) &&
        (f.camera_up[0] != 0.0f || f.camera_up[1] != 0.0f || f.camera_up[2] != 0.0f) &&
        (f.camera_forward[0] != 0.0f || f.camera_forward[1] != 0.0f ||
         f.camera_forward[2] != 0.0f);
    const float* cam_r = rec_cam ? f.camera_right : g_cam_right;
    const float* cam_u = rec_cam ? f.camera_up : g_cam_up;
    const float* cam_f = rec_cam ? f.camera_forward : g_cam_forward;
    const float* cam_p = rec_cam ? f.camera_pos : g_cam_pos;
    const bool cam_known = rec_cam || g_cam_known;
    if (cam_known && (cam_p[0] != 0.0f || cam_p[1] != 0.0f || cam_p[2] != 0.0f)) {
        using namespace DirectX;
        const XMVECTOR right   = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(cam_r));
        const XMVECTOR up      = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(cam_u));
        const XMVECTOR forward = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(cam_f));
        const XMVECTOR pos     = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(cam_p));

        const float x = -XMVectorGetX(XMVector3Dot(pos, right));
        const float y = -XMVectorGetX(XMVector3Dot(pos, up));
        const float z = -XMVectorGetX(XMVector3Dot(pos, forward));

        const XMMATRIX view = {
            XMVectorSet(XMVectorGetX(right),   XMVectorGetX(up),   XMVectorGetX(forward),   0.0f),
            XMVectorSet(XMVectorGetY(right),   XMVectorGetY(up),   XMVectorGetY(forward),   0.0f),
            XMVectorSet(XMVectorGetZ(right),   XMVectorGetZ(up),   XMVectorGetZ(forward),   0.0f),
            XMVectorSet(x, y, z, 1.0f)
        };
        memcpy(cd.viewMatrix, view.r, sizeof(cd.viewMatrix));
    }

    float near_plane = f.camera_near;
    float far_plane  = f.camera_far;
    // Sl_Inputs: "A CameraFar value of zero indicates an infinite far plane." The producer
    // now also publishes the *derived* infinity as a real +inf, so both spellings mean the
    // same thing here.
    bool infinite = !std::isfinite(far_plane) || (near_plane != 0.0f && far_plane == 0.0f);
    if (near_plane != 0.0f && far_plane == 0.0f) {
        far_plane  = near_plane + 1.0f;
    }
    if (!std::isfinite(far_plane)) {
        far_plane = (near_plane > 0.0f) ? near_plane + 1.0f : 1.0f;
    }
    if (cfg().depth_inverted) { std::swap(near_plane, far_plane); }
    // Dispatch(): `_infiniteDepth` swaps the infinity onto whichever side the swap left it.
    if (infinite && far_plane > near_plane) { far_plane = std::numeric_limits<float>::infinity(); }
    else if (infinite && near_plane > far_plane) { near_plane = std::numeric_limits<float>::infinity(); }

    // OptiScaler's Unreal rule (XeFG_Inputs_Dx12.cpp:464-478): Unreal's XeFG integration
    // sends an infinite far plane, but its own DLSSG integration sends 75000 instead.
    // Apply the finite form with the same near plane; the limit form is mathematically
    // equivalent for XeFG's purposes.
    const bool ue_far = sl_engine_unreal() || has_game_quirk(Quirk_ForceUnrealEngine);
    if (infinite && ue_far) {
        const float zn = std::min(near_plane, far_plane);
        near_plane = zn;
        far_plane  = 75000.0f;
        infinite   = false;
        static bool told_ue = false;
        if (!told_ue) {
            told_ue = true;
            logf("xefg: Unreal far plane -> 75000 (near %.3f)\n", zn);
        }
    }

    if (near_plane > 0.0f && far_plane > 0.0f && f.camera_fov_y != 0.0f && f.aspect != 0.0f) {
        using namespace DirectX;
        if (std::abs(near_plane - far_plane) < 1e-5f) { far_plane += 1.0f; }
        if (infinite) {
            // DirectXMath's XMMatrixPerspectiveFovLH computes far/(far-near) and would turn
            // an infinite far plane into inf/inf = NaN. Use the limit form: the same matrix
            // XeFG_Dx12 builds for a finite far, with m[10]=1 and m[14]=-near.
            const float fl = 1.0f / tanf(f.camera_fov_y * 0.5f);
            memset(cd.projectionMatrix, 0, sizeof(cd.projectionMatrix));
            cd.projectionMatrix[0]  = fl / f.aspect;
            cd.projectionMatrix[5]  = fl;
            cd.projectionMatrix[10] = 1.0f;
            cd.projectionMatrix[11] = 1.0f;
            const float zn = std::isfinite(near_plane) ? near_plane
                             : (std::isfinite(far_plane) ? far_plane : 1.0f);
            cd.projectionMatrix[14] = -zn;
        } else {
            const XMMATRIX projection = XMMatrixPerspectiveFovLH(f.camera_fov_y, f.aspect,
                                                                 near_plane, far_plane);
            memcpy(cd.projectionMatrix, projection.r, sizeof(cd.projectionMatrix));
        }
    } else {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logf("xefg: can't calculate projectionMatrix (near=%.3f far=%.3f fov=%.3f aspect=%.3f)"
                 " - using a default LH matrix (XeFG requires one)\n",
                 near_plane, far_plane, f.camera_fov_y, f.aspect);
        }
        // XeFG requires view/projection matrices. OptiScaler leaves the struct zeroed when
        // the camera values are unusable; a default LH projection is the safe fallback.
        using namespace DirectX;
        const float fov = (f.camera_fov_y > 0.05f && f.camera_fov_y < 2.5f)
                              ? f.camera_fov_y
                              : (60.0f * 3.14159265358979f / 180.0f);
        float aspect = f.aspect;
        if (!(aspect > 0.1f && aspect < 10.0f)) {
            aspect = (g_height > 0) ? static_cast<float>(g_width) / static_cast<float>(g_height)
                                    : (16.0f / 9.0f);
        }
        const XMMATRIX projection = XMMatrixPerspectiveFovLH(fov, aspect, 0.1f, 10000.0f);
        memcpy(cd.projectionMatrix, projection.r, sizeof(cd.projectionMatrix));
        if (cd.viewMatrix[0] == 0.0f && cd.viewMatrix[5] == 0.0f &&
            cd.viewMatrix[10] == 0.0f) {
            cd.viewMatrix[0] = cd.viewMatrix[5] = cd.viewMatrix[10] = cd.viewMatrix[15] = 1.0f;
        }
    }

    cd.jitterOffsetX      = f.jitter_x;
    cd.jitterOffsetY      = f.jitter_y;
    // XeFG_Dx12.cpp:890 - `constData.motionVectorScaleX = _mvScaleX[fIndex];`, i.e. the
    // scale goes in VERBATIM. Our version multiplied it by the MV resolution because
    // Sl_Inputs does that for the DLSS-G lane (`multiplyByResolution = true`), but that is
    // a different lane: CP2077's DLSS-SR block reports MV_Scale = (3840, 2160) here, so the
    // product was 3840*3840 = 14.7 million and every reprojection was garbage - a black
    // picture at a doubled frame rate (measured: "mvScale=(3840.000,2160.000)").
    cd.motionVectorScaleX = f.mv_scale_x;
    cd.motionVectorScaleY = f.mv_scale_y;
    cd.resetHistory       = (f.flags & MGPU_FG_FRAME_RESET_HISTORY) ? 1u : 0u;
    // FTInput default is FrameTimeSource::Input -> the game's frame time as measured while
    // generation was OFF (State::lastFGFrameTime is only updated when there is no FG);
    // feeding the live present interval here halves it as soon as FG runs.
    cd.frameRenderTime    = g_frame_ms_no_fg;

    const xefg_swapchain_result_t r = g_xapi.TagFrameConstants(g_ctx, frame_id, &cd);
    static bool warned = false;
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS && !warned) {
        warned = true;
        logf("xefg: TagFrameConstants failed: %s (%d) - logged once\n", result_name(r),
             static_cast<int>(r));
    }
}

void xefg_enable(bool on)
{
    if (g_ctx == nullptr || g_xapi.SetEnabled == nullptr) { return; }
    if (g_generate == on) { return; }
    const xefg_swapchain_result_t r = g_xapi.SetEnabled(g_ctx, on ? 1u : 0u);
    logf("xefg: generation -> %s (%s)\n", on ? "ON" : "OFF", result_name(r));
    if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS) { g_generate = on; }
}

void xefg_set_count(unsigned n)
{
    if (g_ctx == nullptr || g_xapi.SetNumInterpolatedFrames == nullptr) { return; }
    if (n < 1) { n = 1; }
    if (n > g_max_interp) { n = g_max_interp; }
    g_xapi.SetNumInterpolatedFrames(g_ctx, n);
}

// ---------------------------------------------------------------- no conversion, ever
// OptiScaler never converts an XeFG input: the DLSS-G input lane forwards the title's
// resources verbatim (`Sl_Inputs::reportResource`: state from `sl::Resource::state`,
// validity from the tag lifecycle) and the only depth conversion it owns is a compute
// shader used for non-inverted titles. Every "copy the depth to R32" path we wrote here
// was invention: it needed us to guess the game's resource state, and a wrong StateBefore
// removes the device (measured: 13.8 ms -> 4-5 s per present, twice).

namespace {

// The part every lane shares: the id the tags carry and IFGFeature_Dx12::NewFrame()'s
// bookkeeping.
void tag_begin(uint32_t frame_id, const MgpuFgFrame& f)
{
    g_frame_id = frame_id;
    g_last_tagged_id = frame_id;
    g_last_frame = f;          // the present path sends its constants (Dispatch order)
    g_tags_had_mv = false;
    g_tags_size_ok = true;

    // IFGFeature_Dx12::NewFrame() (framegen/IFGFeature_Dx12.cpp:340): every *dispatched*
    // frame refreshes _lastFGFramePresentId and clears _waitingNewFrameData (re-activate).
    // Present() then only trips `_fgFramePresentId - _lastFGFramePresentId > 3` when the
    // title really stops dispatching. Syncing it on present instead was measured as a
    // first-present Deactivate: "5879 presents without a dispatch - Deactivate()" 10 ms
    // after SetEnabled, i.e. generation switched itself off the instant it came on.
    g_last_fg_frame_present_id = g_fg_frame_present_id;
    g_waiting_new_frame_data = false;
}

uint32_t state_of(const MgpuFgResource& r)
{
    // The title's declaration, except for the "not declared" sentinel: XeFG requires a
    // valid D3D12_RESOURCE_STATES, and 0xFFFFFFFF is not one. COMMON is the neutral value
    // that lets D3D12 promote/demote for the copy.
    if (r.state == 0xFFFFFFFFu) { return static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON); }
    return r.state;
}

bool until_present(const MgpuFgResource& r)
{
    return r.validity == MGPU_FG_RV_UNTIL_PRESENT;
}

// MV + depth (+ backbuffer/rect) on `tag_list`.
//   * from the upscaler's Evaluate (TagAtEvaluate=1) `tag_list` is the title's still-OPEN
//     command list, the resources are ValidNow and XeFG copies them at this point in the
//     frame - OptiScaler's Upscaler_Inputs_Dx12::UpscaleStart.
//   * from the present path it is nullptr and the caller has set UntilPresent: only that
//     validity is legal once the title closed/executed the list (measured: handing it over
//     there crashes).
// MV + depth on `tag_list`. Returns false when a tag the library actually received failed;
// XeFG_Dx12 then does `UpdateTarget(); Deactivate();` (XeFG_Dx12.cpp:1639-1647), which the
// caller mirrors. Sizes come from each record stream (the title's own tag extent), not from
// a config guess; mismatched MV/depth are resampled on the open list when possible.
bool tag_inputs(ID3D12CommandList* tag_list, uint32_t frame_id, MgpuFgFrame& f,
                bool upscaler_lane)
{
    // Inputs. The FG contract declares these resources as readable for this pass, and
    // Inputs, exactly as the title declared them (OptiScaler's Sl_Inputs): `state` is the
    // value from `sl::Resource::state`, `validity` from `sl::ResourceTag::lifecycle`.
    // Nothing is barriered and nothing is copied - XeFG performs its own transition from
    // the state we hand it, which is only correct if that state is the title's, not ours.
    const unsigned rw = (f.render_w >= 8) ? f.render_w : g_width;
    const unsigned rh = (f.render_h >= 8) ? f.render_h : g_height;
    const unsigned ow = (f.output_w >= 8) ? f.output_w : g_width;
    const unsigned oh = (f.output_h >= 8) ? f.output_h : g_height;

    // XeFG requires motion vectors and depth to have the same resourceSize
    // (xess_fg_developer_guide_english.md:677/798/853). Titles in the field differ here:
    // UE5's PBZ has MV 2562x1441 and depth 1704x960, Wuchang hands full-res MVs while depth
    // is render-sized. When the extents differ, rebuild the MV field onto the depth grid on
    // the title's open command list (the same pass OptiScaler's MvResample path uses).
    // The record's own stream extents are authoritative; cfg().high_res_mv only decides the
    // upscaler lane's create flag and must not override a resource size here.
    UINT mv_w = f.stream[MGPU_FG_STREAM_MV].width;
    UINT mv_h = f.stream[MGPU_FG_STREAM_MV].height;
    UINT dp_w = f.stream[MGPU_FG_STREAM_DEPTH].width;
    UINT dp_h = f.stream[MGPU_FG_STREAM_DEPTH].height;
    if (mv_w < 8 || mv_h < 8) { mv_w = rw; mv_h = rh; }
    if (dp_w < 8 || dp_h < 8) { dp_w = rw; dp_h = rh; }
    const UINT mv_src_w = mv_w, mv_src_h = mv_h;
    ID3D12CommandList* mv_tag_list =
        resource_cmd(f.stream[MGPU_FG_STREAM_MV], f.stream_cmd[MGPU_FG_STREAM_MV], tag_list);
    if (f.stream[MGPU_FG_STREAM_MV].resource != nullptr &&
        f.stream[MGPU_FG_STREAM_DEPTH].resource != nullptr &&
        (mv_w != dp_w || mv_h != dp_h) && mv_tag_list != nullptr &&
        (!upscaler_lane || cfg().mv_resample != 0)) {
        if (mv_resample_frame(mv_tag_list, &f, true)) {
            mv_w = f.stream[MGPU_FG_STREAM_MV].width;
            mv_h = f.stream[MGPU_FG_STREAM_MV].height;
        }
    }
    g_tags_size_ok = (f.stream[MGPU_FG_STREAM_MV].resource == nullptr ||
                      f.stream[MGPU_FG_STREAM_DEPTH].resource == nullptr ||
                      (mv_w == dp_w && mv_h == dp_h));
    if (!g_tags_size_ok) {
        static bool told_mismatch = false;
        if (!told_mismatch) {
            told_mismatch = true;
            logf("xefg: MV %ux%u != depth %ux%u and MvResample did not run - XeFG"
                 " rejects mismatched resourceSize; generation stays at 1X\n",
                 mv_w, mv_h, dp_w, dp_h);
        }
    }
    {
        static bool told_sizes = false;
        if (!told_sizes) {
            told_sizes = true;
            logf("xefg: tag sizes %s: mv %ux%u (resource %ux%u) depth %ux%u target %ux%u"
                 " high_res_mv=%d resampled=%d\n",
                 upscaler_lane ? "Upscaler" : "DLSSG",
                 mv_w, mv_h, f.stream[MGPU_FG_STREAM_MV].width,
                 f.stream[MGPU_FG_STREAM_MV].height, dp_w, dp_h, ow, oh,
                 cfg().high_res_mv,
                 (mv_w != mv_src_w || mv_h != mv_src_h) ? 1 : 0);
        }
    }

    bool ok = true;

    g_tags_had_depth = false;
    g_tags_had_mv = tag_one(XEFG_SWAPCHAIN_RES_MOTION_VECTOR,
                            static_cast<ID3D12Resource*>(f.stream[MGPU_FG_STREAM_MV].resource),
                            mv_w, mv_h, state_of(f.stream[MGPU_FG_STREAM_MV]),
                            resource_cmd(f.stream[MGPU_FG_STREAM_MV],
                                         f.stream_cmd[MGPU_FG_STREAM_MV], tag_list),
                            until_present(f.stream[MGPU_FG_STREAM_MV]));
    if (!g_tags_had_mv && f.stream[MGPU_FG_STREAM_MV].resource != nullptr) { ok = false; }
    // What we handed the library, once: the first thing to look at when it disagrees.
    if (!g_inputs_logged) {
        g_inputs_logged = true;
        logf("xefg: inputs mv 0x%X(%u) depth 0x%X(%u) declared=%d/%d valid-until-present=%d/%d\n",
             state_of(f.stream[MGPU_FG_STREAM_MV]), f.stream[MGPU_FG_STREAM_MV].validity,
             state_of(f.stream[MGPU_FG_STREAM_DEPTH]), f.stream[MGPU_FG_STREAM_DEPTH].validity,
             f.stream[MGPU_FG_STREAM_MV].state != 0xFFFFFFFFu ? 1 : 0,
             f.stream[MGPU_FG_STREAM_DEPTH].state != 0xFFFFFFFFu ? 1 : 0,
             until_present(f.stream[MGPU_FG_STREAM_MV]) ? 1 : 0,
             until_present(f.stream[MGPU_FG_STREAM_DEPTH]) ? 1 : 0);
    }
    ID3D12Resource* depth =
        static_cast<ID3D12Resource*>(f.stream[MGPU_FG_STREAM_DEPTH].resource);
    if (depth != nullptr) {
        g_tags_had_depth = tag_one(XEFG_SWAPCHAIN_RES_DEPTH, depth, dp_w, dp_h,
                                   state_of(f.stream[MGPU_FG_STREAM_DEPTH]),
                                   resource_cmd(f.stream[MGPU_FG_STREAM_DEPTH],
                                                f.stream_cmd[MGPU_FG_STREAM_DEPTH], tag_list),
                                   until_present(f.stream[MGPU_FG_STREAM_DEPTH]));
        if (!g_tags_had_depth) { ok = false; }
    }
    // What this lane declares, once - XeFG's rejections are format/UI-mode complaints and
    // neither is visible without it. FGInput=Upscaler on CP2077 is OptiScaler's
    // `WithUpscalerFGInput(FGDisableHUDFix = true)`: no colour, and (XeFG_Dx12.cpp:945)
    // no backbuffer tag either, so UI_MODE_AUTO resolves to NONE.
    if (!g_lane_logged) {
        g_lane_logged = true;
        // Format ids resolved from the SDK enums at runtime: the -4 INVALID_ARGUMENT the
        // library answers here is an argument problem, and the depth format is the only
        // difference between the three resources (MV 34, colour 10, depth 19 on Wuchang).
        logf("xefg: fmt ids: R16G16_FLOAT=%d R16G16B16A16_FLOAT=%d R8G8B8A8_UNORM=%d"
             " R8G8B8A8_UNORM_SRGB=%d R10G10B10A2_UNORM=%d R32_TYPELESS=%d D32_FLOAT=%d"
             " R32_FLOAT=%d\n",
             static_cast<int>(DXGI_FORMAT_R16G16_FLOAT),
             static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT),
             static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM),
             static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),
             static_cast<int>(DXGI_FORMAT_R10G10B10A2_UNORM),
             static_cast<int>(DXGI_FORMAT_R32_TYPELESS),
             static_cast<int>(DXGI_FORMAT_D32_FLOAT),
             static_cast<int>(DXGI_FORMAT_R32_FLOAT));
        logf("xefg: lane=%s tags: mv %ux%u fmt=%u state=0x%X, depth %ux%u fmt=%u state=0x%X,"
             " colour %s, backbuffer %s\n",
             upscaler_lane ? "Upscaler" : "DLSSG",
             f.stream[MGPU_FG_STREAM_MV].width, f.stream[MGPU_FG_STREAM_MV].height,
             f.stream[MGPU_FG_STREAM_MV].format, state_of(f.stream[MGPU_FG_STREAM_MV]),
             f.stream[MGPU_FG_STREAM_DEPTH].width, f.stream[MGPU_FG_STREAM_DEPTH].height,
             f.stream[MGPU_FG_STREAM_DEPTH].format, state_of(f.stream[MGPU_FG_STREAM_DEPTH]),
             upscaler_lane ? "none" : ((cfg().tag_hudless != 0) ? "hudless" : "none"),
             (!upscaler_lane && cfg().tag_backbuffer != 0) ? "yes" : "no");
    }
    // Dispatch(): `if (state.activeFgInput != FGInput::Upscaler) { ...backbuffer tag... }`
    // (XeFG_Dx12.cpp:945). The Upscaler lane does not send it, so the interpolation rect
    // comes from XeFG's default (the whole backbuffer). The list argument stays the
    // (ID3D12CommandList*)1 sentinel OptiScaler passes for this tag: it carries no
    // pResource, so nothing is recorded into a list.
    if (!upscaler_lane && cfg().tag_backbuffer != 0) {
        // The interpolation rect is the configured one, or the whole chain. Do NOT derive
        // it from a scaled HUDless extent: a smaller HUDless is a transport scale, not a
        // letterbox region (tag_color skips it instead).
        const UINT rect_x = static_cast<UINT>(cfg().rect_left);
        const UINT rect_y = static_cast<UINT>(cfg().rect_top);
        UINT rect_w = (cfg().rect_w > 0) ? static_cast<UINT>(cfg().rect_w) : g_width;
        UINT rect_h = (cfg().rect_h > 0) ? static_cast<UINT>(cfg().rect_h) : g_height;
        if (rect_w > g_width) { rect_w = g_width; }
        if (rect_h > g_height) { rect_h = g_height; }
        tag_backbuffer(frame_id, nullptr, rect_w, rect_h, rect_x, rect_y);
    }
    // Frame constants are NOT sent here: OptiScaler sends them from Dispatch(), i.e.
    // in the present path, right before SetPresentId - they describe the frame that is
    // about to be presented (and carry the measured frame time).
    return ok;
}

// Colour (HUDless/UI), DLSSG lane only. OptiScaler's Upscaler_Inputs_Dx12 only hands a
// colour over when FGHUDFix is on (UpscaleEnd, Config.h:671 default false), and its CP2077
// profiles disable the HUD fix - so the Upscaler lane has no colour at all and XeFG
// interpolates its own back buffer (UI_MODE_AUTO -> NONE). The DLSSG lane keeps tagging
// what the title published through Streamline (Sl_Inputs), exactly like before.
void tag_color(ID3D12CommandList* tag_list, uint32_t frame_id, const MgpuFgFrame& f,
               bool upscaler_lane)
{
    if (upscaler_lane) { return; }
    g_frame_id = frame_id;   // tag_one stamps the frame with this

    // XeFG: HUDless and UI must have the same format/colour space/size as the back buffer
    // (xess_fg_developer_guide_english.md:855-871). The resourceSize comes from each tag's
    // own extent, and tag_inputs() already moved the back buffer rect to the HUDless extent.
    const UINT hw = (f.stream[MGPU_FG_STREAM_HUDLESS].width >= 8)
                        ? f.stream[MGPU_FG_STREAM_HUDLESS].width : g_width;
    const UINT hh = (f.stream[MGPU_FG_STREAM_HUDLESS].height >= 8)
                        ? f.stream[MGPU_FG_STREAM_HUDLESS].height : g_height;
    const UINT uw = (f.stream[MGPU_FG_STREAM_UI_COLOR].width >= 8)
                        ? f.stream[MGPU_FG_STREAM_UI_COLOR].width : g_width;
    const UINT uh = (f.stream[MGPU_FG_STREAM_UI_COLOR].height >= 8)
                        ? f.stream[MGPU_FG_STREAM_UI_COLOR].height : g_height;
    UINT target_w = (cfg().rect_w > 0) ? static_cast<UINT>(cfg().rect_w) : g_width;
    UINT target_h = (cfg().rect_h > 0) ? static_cast<UINT>(cfg().rect_h) : g_height;
    if (target_w > g_width) { target_w = g_width; }
    if (target_h > g_height) { target_h = g_height; }
    if (cfg().tag_hudless != 0) {
        const uint32_t fmt = f.stream[MGPU_FG_STREAM_HUDLESS].format;
        if (hw != target_w || hh != target_h ||
            (g_chain_fmt != 0 && fmt != 0 && fmt != g_chain_fmt)) {
            static bool told_hf = false;
            if (!told_hf) {
                told_hf = true;
                logf("xefg: HUDless %ux%u fmt %u != chain/rect %ux%u fmt %u - not tagging it"
                     " (XeFG requires the same extent/format)\n",
                     hw, hh, fmt, target_w, target_h, g_chain_fmt);
            }
        } else {
            tag_one(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR,
                    static_cast<ID3D12Resource*>(f.stream[MGPU_FG_STREAM_HUDLESS].resource),
                    hw, hh, state_of(f.stream[MGPU_FG_STREAM_HUDLESS]),
                    resource_cmd(f.stream[MGPU_FG_STREAM_HUDLESS],
                                 f.stream_cmd[MGPU_FG_STREAM_HUDLESS], tag_list),
                    until_present(f.stream[MGPU_FG_STREAM_HUDLESS]));
        }
    }
    if (cfg().tag_ui != 0) {
        const uint32_t fmt = f.stream[MGPU_FG_STREAM_UI_COLOR].format;
        // HUDless/UI/backbuffer share resourceSize: tag UI only when it matches the same
        // chain/rect extent XeFG receives for the back buffer. (HUDless is skipped above if
        // it does not match, so a UI-only fallback is still governed by the chain size.)
        if ((uw != target_w || uh != target_h) ||
            (g_chain_fmt != 0 && fmt != 0 && fmt != g_chain_fmt)) {
            static bool told_ui = false;
            if (!told_ui) {
                told_ui = true;
                logf("xefg: UI %ux%u fmt %u does not match HUDless %ux%u fmt %u (chain fmt %u)"
                     " - not tagging UI\n", uw, uh, fmt, hw, hh,
                     f.stream[MGPU_FG_STREAM_HUDLESS].format, g_chain_fmt);
            }
        } else {
            tag_one(XEFG_SWAPCHAIN_RES_UI,
                    static_cast<ID3D12Resource*>(f.stream[MGPU_FG_STREAM_UI_COLOR].resource),
                    uw, uh, state_of(f.stream[MGPU_FG_STREAM_UI_COLOR]),
                    resource_cmd(f.stream[MGPU_FG_STREAM_UI_COLOR],
                                 f.stream_cmd[MGPU_FG_STREAM_UI_COLOR], tag_list),
                    until_present(f.stream[MGPU_FG_STREAM_UI_COLOR]));
        }
    }
}

} // namespace

// One lane's complete tag set, in the present-path form (`list == nullptr`, resources
// UntilPresent). TagAtEvaluate=1 uses tag_inputs/tag_color directly instead, split around
// the upscaler's Evaluate.
void xefg_tag(ID3D12GraphicsCommandList* list, uint32_t frame_id, const MgpuFgFrame& f,
              bool upscaler_lane)
{
    if (g_ctx == nullptr) { return; }
    // Mutable copy: the DLSSG lane may resample MV onto the depth grid on the open list
    // (XeFG requires the same resourceSize for the pair). tag_begin() below copies the
    // pre-resample frame, so store the updated one back for the present path.
    MgpuFgFrame frame = f;
    tag_begin(frame_id, frame);
    if (!tag_inputs(static_cast<ID3D12CommandList*>(list), frame_id, frame, upscaler_lane)) {
        // XeFG_Dx12.cpp:1639-1647: a rejected tag -> UpdateTarget(); Deactivate();
        xefg_pause(10);
        return;
    }
    tag_color(static_cast<ID3D12CommandList*>(list), frame_id, frame, upscaler_lane);
    g_last_frame = frame;
}

// TagAtEvaluate: the placement OptiScaler uses (inputs/FG/Upscaler_Inputs_Dx12.cpp).
// `cmd_list` is the upscaler's own list and it is OPEN here - the only moment a ValidNow
// tag is legal for a title resource.
//
//   stage 0, before the real Evaluate is forwarded (UpscaleStart):
//       res.cmdList = InCmdList; res.validity = ValidNow;   -> MV/depth (+ rect/backbuffer)
//   stage 1, after it (UpscaleEnd):
//       info.buffer = NVSDK_NGX_Parameter_Output; ...       -> HUDless/UI colour
//
// The id is generated HERE and the present path reuses it verbatim (g_eval_frame_id), so
// the frame XeFG pairs with the present is the frame whose inputs were tagged. At most one
// tagged frame per present interval: a title that evaluates the upscaler per view must not
// hand XeFG two ids for one present.
bool xefg_tag_at_evaluate(void* cmd_list, int stage)
{
    if (cfg().tag_at_evaluate == 0 ||
        cfg().fg_input != static_cast<int>(FgInputSource::Upscaler)) { return false; }
    if (g_ctx == nullptr || cmd_list == nullptr || !xefg_wants_generation()) { return false; }

    if (stage == EvalTag_Color) {
        if (!g_eval_tag_fresh || g_eval_frame_id == 0) { return false; }
        tag_color(static_cast<ID3D12CommandList*>(cmd_list), g_eval_frame_id, g_eval_frame, true);
        return true;
    }

    if (g_eval_tag_fresh) { return false; }   // already tagged for this present interval

    MgpuFgFrame f{};
    if (!sr_apply(f)) { return false; }

    // MvResample: the title hands full-resolution motion vectors while depth is at the
    // upscaler's render size (Wuchang: MV 3840x2160 R16G16_FLOAT, depth 2560x1440). XeFG
    // requires the two to match, so the field is resampled onto the depth's size here, on
    // this same command list, before anything is tagged - after it the input genuinely is
    // low-res MV and OptiScaler's gate applies unchanged.
    mv_resample_frame(static_cast<ID3D12CommandList*>(cmd_list), &f);

    // XeFG_Dx12::EvaluateState() -> Activate() runs before the inputs are handed over, and
    // Upscaler_Inputs_Dx12 only tags while `fg->IsActive()` (Upscaler_Inputs_Dx12.cpp:129).
    // An inactive feature gets no resources at all.
    g_last_frame = f;                      // Activate() reads render/target sizes from here
    xefg_frame_feedback(true);
    if (!g_generate) { return false; }

    const uint32_t id = ++g_eval_tag_counter;
    tag_begin(id, f);                      // the >3 bookkeeping belongs to the tag
    if (!tag_inputs(static_cast<ID3D12CommandList*>(cmd_list), id, f, true)) {
        // XeFG_Dx12.cpp:1639-1647: a tag the library rejected -> UpdateTarget(); Deactivate();
        xefg_pause(10);
        return false;
    }

    g_eval_frame     = f;
    g_eval_frame_id  = id;
    g_eval_tag_fresh = true;

    static int told = 0;
    if (told < 5) {
        ++told;
        logf("xefg: evaluate-side tag id=%u list=%p valid-now mv=%d depth=%d\n",
             id, cmd_list, g_tags_had_mv ? 1 : 0, g_tags_had_depth ? 1 : 0);
    }
    return true;
}

bool xefg_last_eval_frame(MgpuFgFrame* out)
{
    if (out == nullptr || !g_eval_tag_fresh) { return false; }
    *out = g_eval_frame;          // exactly what XeFG consumes: post-MvResample
    return true;
}

// OptiScaler's Dispatch() only runs once Depth and Velocity are ready, and drops back
// to 1X when they are not. Ours does the same from the outside: generation is armed
// by the first complete frame, and any rejection by the library parks us in 1X (real
// frames, correct picture) instead of presenting nothing (black screen).
// XeFG_Dx12::Activate() / Deactivate() / Dispatch(), 1:1.
//
//   Activate():   SetEnabled(true) when the context exists, it is not active yet, and
//                 (IsLowResMV() || nativeAA || ForceFGRenderSizeMVs || IgnoreInitChecks).
//                 With MV at render res that is low-res MV; at display res it is native AA,
//                 so in practice the condition holds whenever the inputs exist.
//   Dispatch():   returns early when Depth or Velocity is not ready - it does NOT toggle
//                 SetEnabled for a frame with missing inputs.
//   rejected:     `state.fgChanged = true; UpdateTarget(); Deactivate();` - immediate, no
//                 timer of its own.
void xefg_frame_feedback(bool tags_complete)
{
    if (g_ctx == nullptr) { return; }

    // FgOutput=Capture never drives the XeFG generator. Input routing and output
    // selection are independent.
    if (cfg().fg_output != static_cast<int>(FgOutputSink::XeFG)) {
        if (g_generate) { xefg_enable(false); }
        return;
    }

    // IsPaused(): Dispatch() is not called, so no activation either (XeFG_Dx12.cpp:691).
    if (xefg_paused()) { return; }

    if (!g_want_generate) {
        if (g_generate) { xefg_enable(false); }          // Deactivate()
        return;
    }
    if (!tags_complete) { return; }                       // Dispatch() early-out

    if (!g_generate) {
        // XeFG_Dx12::Activate() (XeFG_Dx12.cpp:583-610), verbatim:
        //     nativeAA = currentFeature->RenderWidth() == currentFeature->DisplayWidth();
        //     activate when IsLowResMV() || nativeAA || ForceFGRenderSizeMVs || IgnoreInitChecks
        // IsLowResMV() is `!_constants.flags[DisplayResolutionMVs]` (IFGFeature.cpp:158).
        // OptiScaler sets that flag from sl::Constants::motionVectorsDilated (or the NGX
        // wide-MV declaration). The declaration alone is not enough here: some titles set
        // dilated while the vectors are still render-sized, so require the actual MV
        // extent to reach the display grid too. A resampled pair is low-res by
        // construction and must pass.
        const unsigned render_w = g_last_frame.render_w;
        const unsigned render_h = g_last_frame.render_h;
        const unsigned target_w = (g_last_frame.output_w >= 8) ? g_last_frame.output_w : g_width;
        const unsigned target_h = (g_last_frame.output_h >= 8) ? g_last_frame.output_h : g_height;
        const bool mv_hi_extent =
            g_last_frame.stream[MGPU_FG_STREAM_MV].width >= target_w &&
            g_last_frame.stream[MGPU_FG_STREAM_MV].height >= target_h &&
            g_last_frame.stream[MGPU_FG_STREAM_MV].resource != nullptr;
        const bool declared_high =
            (g_last_frame.flags & MGPU_FG_FRAME_HI_RES_MV) != 0 ||
            ((g_last_frame.flags & MGPU_FG_FRAME_MV_DILATED) != 0 && mv_hi_extent);
        const bool low_res_mv = (!declared_high && cfg().high_res_mv == 0) ||
                                cfg().mv_resample != 0;
        const bool native_aa = (render_w >= 8 && target_w >= 8 && render_w == target_w);
        // XeFG_Dx12::Activate(): `IsLowResMV() || nativeAA || ForceFGRenderSizeMVs ||
        // FGXeFGIgnoreInitChecks`.
        if (!(low_res_mv || native_aa || has_game_quirk(Quirk_ForceFGRenderSizeMVs) ||
              cfg().ignore_init_checks != 0)) {
            static bool told = false;
            if (!told) {
                told = true;
                logf("xefg: FG stays off - high-res MV (HighResMv=%d) and render %ux%u !="
                     " display %ux%u; Activate() needs low-res MV or native AA\n",
                     cfg().high_res_mv, render_w, render_h, target_w, target_h);
            }
            // And tell the title: it is handing the picture over to a provider that can
            // never generate here, which is the black screen (only the UI is left). A short
            // run of refusals first, so one odd frame during loading cannot withdraw it.
            // NOT while BlitDlssgBackbuffer is doing the compositing: the title's DLSS-G block
            // is then the only source of the frame, and withdrawing makes it stop sending it
            // (black screen again) - XeFG just keeps presenting the real frames at 1X.
            static int refusals = 0;
            if (++refusals >= 30) {
                provider_block_fg("high-res MV with render != display");
            }
            return;
        }

        if (g_xapi.SetEnabled != nullptr &&
            g_xapi.SetEnabled(g_ctx, 1u) == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            g_generate = true;
            g_waiting_new_frame_data = false;
            // Activate() starts a new epoch: the present counter must not carry the 1X
            // history into the first generated frame (XeFG_Dx12::Disable() does the same
            // with its two ids).
            g_last_fg_frame_present_id = g_fg_frame_present_id;
            g_last_presented_id = 0;
            // Dispatch() applies the interpolation count: `SetNumInterpolatedFrames(targetCount)`
            // when it changed. Ours is fixed, so once per activation.
            if (g_xapi.SetNumInterpolatedFrames != nullptr) {
                unsigned count = static_cast<unsigned>(cfg().interpolated);
                if (count < 1) { count = 1; }
                if (count > g_max_interp) { count = g_max_interp; }
                g_xapi.SetNumInterpolatedFrames(g_ctx, count);
            }
            logf("xefg: SetEnabled: true (lowResMV=%d nativeAA=%d)\n",
                 low_res_mv ? 1 : 0, native_aa ? 1 : 0);
        }
        return;
    }
    // Nothing to do per frame: OptiScaler never inspects `frameGenResult` to toggle the
    // generation switch (GetLastPresentStatus only feeds its frame counter); a rejected
    // input set is a problem to fix, not to paper over by turning generation off.
}

void xefg_log_status()
{
    if (g_ctx == nullptr || g_xapi.GetLastPresentStatus == nullptr) { return; }
    xefg_swapchain_present_status_t st{};
    if (g_xapi.GetLastPresentStatus(g_ctx, &st) != XEFG_SWAPCHAIN_RESULT_SUCCESS) { return; }
    logf("xefg: last present: framesPresented=%u fgEnabled=%u result=%s (%d)\n",
         st.framesPresented, st.isFrameGenEnabled,
         result_name(st.frameGenResult), static_cast<int>(st.frameGenResult));
}

void backend_shutdown()
{
    g_tags_had_depth = false;
    g_tags_had_mv = false;
    g_tags_size_ok = true;
    g_chain_fmt = 0;
    g_last_tagged_id = 0;
    g_last_presented_id = 0;
    g_submit_frame_id = 0;
    g_submit_dispatched_id = 0;
    g_waiting_new_frame_data = false;
    g_eval_frame = MgpuFgFrame{};
    g_eval_frame_id = 0;
    g_eval_tag_fresh = false;
    // P2-E: release the contexts retired across FGPreserveSwapChain=false recreations.
    if (g_xapi.Destroy != nullptr) {
        for (unsigned i = 0; i < g_retired_n; ++i) {
            if (g_retired_ctx[i] != nullptr) { g_xapi.Destroy(g_retired_ctx[i]); }
            g_retired_ctx[i] = nullptr;
        }
    }
    g_retired_n = 0;
    if (g_ctx != nullptr && g_xapi.Destroy != nullptr) { g_xapi.Destroy(g_ctx); }
    if (g_xell != nullptr && g_xell_dll != nullptr) {
        auto destroy = reinterpret_cast<decltype(&xellDestroyContext)>(
            reinterpret_cast<void*>(GetProcAddress(g_xell_dll, "xellDestroyContext")));
        if (destroy != nullptr) { destroy(g_xell); }
        g_xell = nullptr;
    }
    g_ctx = nullptr;
    g_chain = nullptr;
    g_facade = nullptr;   // released by the game when it drops the chain
}

} // namespace fakerfg
