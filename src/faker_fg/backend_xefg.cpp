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
bool                     g_generate = false;   // XeFG generation actually on
bool                     g_want_generate = false; // what Mode asked for
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
bool                     g_inputs_logged = false;
bool                     g_lane_logged = false;
uint32_t                 g_last_tagged_id = 0;       // the frame the tags describe
uint32_t                 g_last_presented_id = 0;    // the id SetPresentId last used
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
bool tag_backbuffer(uint32_t frame_id, ID3D12CommandList* list)
{
    if (g_ctx == nullptr || g_xapi.D3D12TagFrameResource == nullptr) { return false; }

    xefg_swapchain_d3d12_resource_data_t d{};
    d.type         = XEFG_SWAPCHAIN_RES_BACKBUFFER;
    d.validity     = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
    d.resourceBase = { static_cast<uint32_t>(cfg().rect_left), static_cast<uint32_t>(cfg().rect_top) };
    d.resourceSize = { (cfg().rect_w > 0) ? static_cast<uint32_t>(cfg().rect_w) : g_width,
                       (cfg().rect_h > 0) ? static_cast<uint32_t>(cfg().rect_h) : g_height };
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
    if (cfg().tag_at_evaluate != 0 && cfg().fg_input == 1) {
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


static void marker_probe_present()
{
    if (!marker_probe_enabled() || g_chain_api == nullptr || g_queue == nullptr) { return; }
    if (g_pending_seq == 0) { return; }
    if (marker_file_exists(L"marker_disable.on")) { return; }
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

    // v22: marker_probe_present() removed - the consumer no longer decodes a marker.

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
    // library's own rejection code (-12 MISMATCH_INPUT_RESOURCES and friends).
    if (g_xapi.GetLastPresentStatus != nullptr) {
        static unsigned polls = 0, seen = 0, no_input = 0, last_harvests = 0;
        ++polls;
        if (polls % 120 == 0) {
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
            if (!sr_lane_ready() && no_input < 2) {
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
    g_want_generate = (c.mode == Mode::Local);   // our equivalent of IsFGEnabled()
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
    // "an FG Input without an FG Output, using none").
    return g_want_generate && cfg().fg_output != 0 && cfg().fg_input != 0;
}

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
bool xefg_tags_complete() { return g_tags_had_depth && g_tags_had_mv; }

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
    if (g_cam_known && (g_cam_pos[0] != 0.0f || g_cam_pos[1] != 0.0f || g_cam_pos[2] != 0.0f)) {
        using namespace DirectX;
        const XMVECTOR right   = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(g_cam_right));
        const XMVECTOR up      = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(g_cam_up));
        const XMVECTOR forward = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(g_cam_forward));
        const XMVECTOR pos     = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(g_cam_pos));

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
    // Sl_Inputs: "A CameraFar value of zero indicates an infinite far plane."
    bool infinite = false;
    if (near_plane != 0.0f && far_plane == 0.0f) {
        infinite   = true;
        far_plane  = near_plane + 1.0f;
    }
    if (cfg().depth_inverted) { std::swap(near_plane, far_plane); }
    // Dispatch(): `_infiniteDepth` swaps the infinity onto whichever side the swap left it.
    if (infinite && far_plane > near_plane) { far_plane = std::numeric_limits<float>::infinity(); }
    else if (infinite && near_plane > far_plane) { near_plane = std::numeric_limits<float>::infinity(); }

    if (near_plane > 0.0f && far_plane > 0.0f && f.camera_fov_y != 0.0f && f.aspect != 0.0f) {
        using namespace DirectX;
        if (std::abs(near_plane - far_plane) < 1e-5f) { far_plane += 1.0f; }
        const XMMATRIX projection = XMMatrixPerspectiveFovLH(f.camera_fov_y, f.aspect,
                                                             near_plane, far_plane);
        memcpy(cd.projectionMatrix, projection.r, sizeof(cd.projectionMatrix));
    } else {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logf("xefg: can't calculate projectionMatrix (near=%.3f far=%.3f fov=%.3f aspect=%.3f)"
                 " - camera values missing\n", near_plane, far_plane, f.camera_fov_y, f.aspect);
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
    // Verbatim: `res.state = (D3D12_RESOURCE_STATES) tag.resource->state` - UINT_MAX when
    // the title declared none, and the library sees exactly that.
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
// MV + depth on `tag_list`, with the sizes OptiScaler's Upscaler_Inputs_Dx12 uses (see
// below). Returns false when a tag the library actually received failed; XeFG_Dx12 then
// does `UpdateTarget(); Deactivate();` (XeFG_Dx12.cpp:1639-1647), which the caller mirrors.
bool tag_inputs(ID3D12CommandList* tag_list, uint32_t frame_id, const MgpuFgFrame& f,
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

    // Sizes, verbatim from OptiScaler's Upscaler_Inputs_Dx12.cpp:
    //     velocity: LowResMV() ? RenderWidth/Height : TargetWidth/Height   (lines 167-177)
    //     depth:    RenderWidth/Height                                     (lines 219-221)
    // `LowResMV` is the negation of our HighResMv (IFeature.cpp:113 saves it that way:
    // FGXeFGHighResMV = !_initFlags.LowResMV), and both numbers come from the upscaler's
    // parameter block (IFeature.cpp:120-165), not from the resource descriptions.
    const bool mv_at_render = (cfg().high_res_mv == 0) || cfg().mv_resample != 0;
    const unsigned mv_w = mv_at_render ? rw : ow;
    const unsigned mv_h = mv_at_render ? rh : oh;
    {
        static bool told_sizes = false;
        if (!told_sizes) {
            told_sizes = true;
            logf("xefg: tag sizes %s: mv %ux%u (resource %ux%u) depth %ux%u target %ux%u"
                 " high_res_mv=%d\n",
                 upscaler_lane ? "Upscaler" : "DLSSG",
                 mv_w, mv_h, f.stream[MGPU_FG_STREAM_MV].width,
                 f.stream[MGPU_FG_STREAM_MV].height, rw, rh, ow, oh, cfg().high_res_mv);
        }
    }

    bool ok = true;

    g_tags_had_depth = false;
    g_tags_had_mv = tag_one(XEFG_SWAPCHAIN_RES_MOTION_VECTOR,
                            static_cast<ID3D12Resource*>(f.stream[MGPU_FG_STREAM_MV].resource),
                            mv_w, mv_h, state_of(f.stream[MGPU_FG_STREAM_MV]), tag_list,
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
        g_tags_had_depth = tag_one(XEFG_SWAPCHAIN_RES_DEPTH, depth, rw, rh,
                                   state_of(f.stream[MGPU_FG_STREAM_DEPTH]), tag_list,
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
        tag_backbuffer(frame_id, nullptr);
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

    const unsigned ow = (f.output_w >= 8) ? f.output_w : g_width;
    const unsigned oh = (f.output_h >= 8) ? f.output_h : g_height;
    if (cfg().tag_hudless != 0) {
        tag_one(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR,
                static_cast<ID3D12Resource*>(f.stream[MGPU_FG_STREAM_HUDLESS].resource),
                ow, oh, state_of(f.stream[MGPU_FG_STREAM_HUDLESS]), tag_list,
                until_present(f.stream[MGPU_FG_STREAM_HUDLESS]));
    }
    if (cfg().tag_ui != 0) {
        tag_one(XEFG_SWAPCHAIN_RES_UI,
                static_cast<ID3D12Resource*>(f.stream[MGPU_FG_STREAM_UI_COLOR].resource),
                ow, oh, state_of(f.stream[MGPU_FG_STREAM_UI_COLOR]), tag_list,
                until_present(f.stream[MGPU_FG_STREAM_UI_COLOR]));
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
    tag_begin(frame_id, f);
    if (!tag_inputs(static_cast<ID3D12CommandList*>(list), frame_id, f, upscaler_lane)) {
        // XeFG_Dx12.cpp:1639-1647: a rejected tag -> UpdateTarget(); Deactivate();
        xefg_pause(10);
        return;
    }
    tag_color(static_cast<ID3D12CommandList*>(list), frame_id, f, upscaler_lane);
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
    if (cfg().tag_at_evaluate == 0 || cfg().fg_input != 1) { return false; }
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
        // IsLowResMV() is `!_constants.flags[DisplayResolutionMVs]` (IFGFeature.cpp:158) and
        // that flag is `!feature->LowResMV()` (Upscaler_Inputs.cpp:66-67) - i.e. the
        // upscaler's own create flag. The velocity resource's size does NOT enter, and our
        // old `mv.width == output.width` heuristic turned a high-res-MV title that is
        // upscaling into "native AA" and enabled XeFG on inputs it cannot use.
        const bool low_res_mv = (cfg().high_res_mv == 0) || cfg().mv_resample != 0;
        const unsigned render_w = g_last_frame.render_w;
        const unsigned render_h = g_last_frame.render_h;
        const unsigned target_w = (g_last_frame.output_w >= 8) ? g_last_frame.output_w : g_width;
        const unsigned target_h = (g_last_frame.output_h >= 8) ? g_last_frame.output_h : g_height;
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
    g_last_tagged_id = 0;
    g_last_presented_id = 0;
    g_waiting_new_frame_data = false;
    g_eval_frame = MgpuFgFrame{};
    g_eval_frame_id = 0;
    g_eval_tag_fresh = false;
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
