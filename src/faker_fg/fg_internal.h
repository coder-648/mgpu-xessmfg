// ============================================================================
//  fg_internal.h - everything fakerFG's own translation units share.
//
//  One module, three jobs, decided by the file name the game loaded us as:
//
//      dxgi.dll           proxy 1: forwarding DXGI exports + the one vtable entry
//                         that gives us the game's swap chain creation
//      nvngx_dlssg.dll    proxy 2: the DLSS-G provider the title talks to
//      nvngx_dlssg_real.dll  the title's own runtime, forwarded to in observe mode
//
//  The FG pipeline is ours end to end:
//
//      CreateFeature  -> our handle (magic), remember device
//      swap chain     -> XeFG created it, so it presents (generation on/off is ours)
//      Evaluate       -> harvest (MgpuFgFrame) + tag resources + present id
//      capture        -> MgpuFgGetApi()/acquire() reads the record and transports it
//
//  Design: docs/design/FAKER_FG.md. Provenance: the flow mirrors OptiScaler's
//  framegen/nvngx/Nvngx_FG.cpp + framegen/xefg/XeFG_Dx12.cpp (GPL-3.0).
// ============================================================================
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdint.h>
#include <stdio.h>
#include <share.h>
#include <string>
#include <unordered_map>

#include "mgpu_fg_api.h"
#include "mgpu_shared.h"

// v22: src/mgpu_present_ring.h deleted (WGC pairing ring).

#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_defs_dlssg.h"
#include "nvsdk_ngx_params.h"

namespace fakerfg {

// ---------------------------------------------------------------- config
// fakerFG.ini next to the game exe. Read once (rule 7): a mode change is a restart.
enum class Mode
{
    Off = 0,     // never answer, never take over
    Observe,     // answer, forward to the real runtime (A/B baseline)
    Idle,        // answer, generate nothing, present the real frame
    Local,       // answer, XeFG generates on this GPU   <- self-test platform
    Bridge,      // answer, generation off in-game; bridge.exe produces the frames
    Consume      // same as Bridge, but named for the dual-GPU contract: harvest the full
                 // input set, actively refuse FG execution, native frame to the display
};

struct Config
{
    Mode    mode            = Mode::Local;  // what proof looks like by default
    int     interpolated    = 1;            // generated frames per real frame (local)
    int     inject_reshade  = 1;
    int     own_swapchain   = 1;
    int     passthrough     = 0;            // 1 = never answer, never take over
    int     depth_inverted  = 1;            // reversed-Z: true for CP2077 / UE5 / RE
    int     mv_jittered     = 0;
    int     high_res_mv     = 0;
    // Full-resolution motion vectors onto the interpolation size (shaders/mv_resample.hlsl).
    // After this pass the input really is render-sized, so LowResMV is true by construction -
    // OptiScaler's gate passes for a legitimate reason and no size is faked.
    int     mv_resample      = 0;
    // 1 = hand the title the swap chain libxess created and detour its vtable, the way
    // OptiScaler does (FGHooks::HookFGSwapchain). 0 = our own facade object.
    int     real_swapchain_object = 0;
    // 1 = answer the DLSS-G CreateFeature ourselves (our fake handle). OptiScaler never does
    // this: Nvngx_FG::D3D12_CreateFeature forwards to a real provider and answers Fail when
    // there is none (Nvngx_FG.cpp:312-344), so the title turns its own FG off and renders its
    // own frame. 0 = the same behaviour here.
    int     fake_dlssg_feature = 1;
    // 1 = on every DLSS-G Evaluate, copy the frame the title declared (DLSSG.Backbuffer) into
    // the contract's OutputReal, on the title's own command list. That is the provider's job
    // for a title that consumes the NGX contract, and it is what lets the title present its
    // own frame while we keep every input for our own generator.
    int     write_output_real = 0;
    // OptiScaler's present/evaluate shader passes (FGDrawUIOverFG / FGUIPremultipliedAlpha /
    // FGHudCutoff / fgHudlessCompare). Same names, same defaults: all off.
    int     fg_draw_ui_over_fg = 0;
    int     fg_ui_premultiplied = 1;
    int     fg_hud_cutoff      = 0;      // /10 -> DiffThreshold, like OptiScaler
    int     fg_hudless_compare = 0;
    int     ui_premultiplied = 0;           // 1 = the UI texture is already premultiplied
    int     tag_hudless     = 0;            // tag DLSSG.HUDLess as the colour source
    // Send the backbuffer tag (which is also the only carrier of the interpolation rect in
    // this API version) in the Upscaler lane too. 1 = send, 0 = OptiScaler's
    // XeFG_Dx12.cpp:951 behaviour for that lane.
    int     tag_backbuffer  = 1;
    // 1 = tag MV/depth inside the upscaler's own Evaluate instead of at present time.
    // This is OptiScaler's actual code path (Upscaler_Inputs_Dx12: `res.cmdList = InCmdList;
    // res.validity = ValidNow;`), so it is the default. 0 keeps the old present-path tag
    // (`UntilPresent`) purely as an A/B escape hatch.
    int     tag_at_evaluate = 1;
    // Interpolation rectangle inside XeFG's own backbuffer; OptiScaler's FGRectLeft /
    // FGRectTop / FGRectWidth / FGRectHeight. 0 = whole backbuffer (the normal case).
    int     rect_left       = 0;
    int     rect_top        = 0;
    int     rect_w          = 0;
    int     rect_h          = 0;
    int     tag_ui          = 0;            // tag DLSSG.UI (+alpha) for recomposition
    int     strip_dlssg     = 0;            // 1 = keep sl.dlss_g out (we answer it directly)
    int     fake_sl_player  = 1;            // 1 = answer slDLSSGSetOptions/GetState ourselves (OptiScaler: always)
    int     ngx_core_proxy  = 0;            // 1 = interpose nvngx.dll/_nvngx.dll (WIP: breaks DLSS-SR)
    int     arch_spoof      = 1;            // answer the Streamline arch gate (see nvapi_arch.cpp)
    // 1 = XEFG_SWAPCHAIN_DEBUG_FEATURE_TAG_INTERPOLATED_FRAMES (interpolated frames carry a
    // marker). OptiScaler's FGXeFGDebugView - the quickest way to see whether generated
    // frames are really reaching the screen.
    int     debug_view      = 0;
    int     debug_only_generated = 0;       // XEFG_SWAPCHAIN_DEBUG_FEATURE_SHOW_ONLY_INTERPOLATION
    // OptiScaler FGXeFGIgnoreInitChecks: activate XeFG even when neither low-res MV nor
    // native AA is detected.
    int     ignore_init_checks = 0;
    // UpscalerInputs::GetCameraValues, verbatim: the Upscaler lane does NOT use the
    // title's camera; it uses these (FsrCameraNear/Far = 0.1/100000, FsrVerticalFov = 60
    // degrees) unless the upscaler's block carries the FSR keys.
    float   camera_near     = 0.1f;
    float   camera_far      = 100000.0f;
    float   vertical_fov_deg = 60.0f;
    // OptiScaler [Hotfix] MotionVectorResourceBarrier / DepthResourceBarrier. 0 = their
    // default, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE (0x40 = 64).
    int     mv_state        = 0;
    int     depth_state     = 0;
    // The colour the upscaler writes (NVSDK_NGX_Parameter_Output) has no declared state in
    // its parameter block; 0 = hand the library UINT_MAX, i.e. "no state declared", the
    // same thing OptiScaler does for a tag that carries none.
    int     color_state     = 0;
    // XeFG cannot interpolate in exclusive fullscreen: the facade swallows the title's
    // SetFullscreenState(TRUE) (OptiScaler FGXeFGForceBorderless). Off by default like
    // theirs (Config.h:731); a title that really runs exclusive fullscreen needs it - XeFG
    // otherwise answers "Interpolation is not supported in fullscreen mode" and disables
    // generation (measured on Wuchang, docs/design/FAKER_FG_PORT.md 12).
    int     force_borderless = 0;
    // OptiScaler Config.h:514 `SkipFirstFrames` (NVNGX_DLSS_Dx12.cpp:933): the first N
    // upscaler evaluations are not forwarded at all. Wuchang's GameConfigs entry is 10
    // ("Skip 1 frame use of upscaler which cause crash").
    int     skip_first_frames = 0;
    // OptiScaler SimulateWaitableObject: hand out our own frame-latency semaphore instead of
    // the proxy chain's (that chain has none). Off by default, exactly like theirs.
    int     simulate_waitable = 0;
    // OptiScaler's [FrameGen] FGInput / FGOutput: ONE input lane, ONE output, chosen by
    // config - there is no automatic lane. Codes are theirs:
    //   FgInput:  NoFG (default) | Upscaler | DLSSG
    //   FgOutput: NoFG (default) | XeFG
    int     fg_input        = 0;            // 0 NoFG, 1 Upscaler, 2 DLSSG
    int     fg_output       = 0;            // 0 NoFG, 1 XeFG
    int     log             = 1;
    // 1 = publish nothing at all: no record snapshot, no sink call, no transport work.
    // Measurement switch for the publish/subscribe cost (docs/design/XESS_MFG_PRODUCER.md).
    int     no_publish      = 0;
};

Config& cfg();

// ---------------------------------------------------------------- game quirks
// OptiScaler misc/Quirks.h + misc/GameConfigs.cpp: per-title adjustments keyed by the
// executable name and by the engine Streamline reports. Only the ones this port uses.
enum GameQuirk : unsigned
{
    Quirk_None                  = 0u,
    Quirk_ForceUnrealEngine     = 1u << 0,   // ForceUnrealEngine
    Quirk_NoFSRFGFirstSwapchain = 1u << 1,   // NoFSRFGFirstSwapchain: first chain passes through
    Quirk_ForceFGRenderSizeMVs  = 1u << 2,   // ForceFGRenderSizeMVs: activate without low-res MV
    Quirk_CyberpunkHudlessState = 1u << 3,   // CyberpunkHudlessState (hudless arrives SDR)
};

unsigned game_quirks();
void     set_game_quirk(GameQuirk q);
bool     has_game_quirk(GameQuirk q);

const char* mode_name(Mode m);
Mode parse_mode(const char* s);

// ---------------------------------------------------------------- logging
// One file: <exe>_fakerFG.log. Every subsystem writes here, so a session has a
// single timeline (the capture's log is a second one, on purpose).
void log_open();
void logf(const char* fmt, ...);
void log_flush();

// Persist the three XeFG init flags that come from the upscaler's create flags
// (OptiScaler's Config::SaveXeFG): XeFG's context is created before the upscaler
// initialises, so the values only take effect on the next init.
void cfg_save_upscaler_flags(int depth_inverted, int mv_jittered, int high_res_mv);

// crash reporting: writes exception code, faulting address and the stack frames that
// resolve to a loaded module, so a dead process says *who* died instead of only that it did.
void install_crash_log();

// ---------------------------------------------------------------- producer link
// xess_mfg_producer.dll (optional): loads on the loader thread, subscribes to the
// record, and is ticked from the Present path. Absent = no transport, no change to
// the FG chain.
void producer_link_start();
void producer_present(void* queue);

// ---------------------------------------------------------------- lifecycle
// Loader work starts on our own thread: DllMain must not LoadLibrary anything
// that can call back into us, and rules forbid hooks under the loader lock.
void start_loader_thread();

// ---------------------------------------------------------------- real runtime
// `nvngx_dlssg_real.dll` (the file we displaced). Absent is a normal state:
// then there is nothing to forward to and the provider must stand alone.
bool load_real_ngx();
void* real_ngx(const char* export_name);

// ---------------------------------------------------------------- record / API
void publish_record(const MgpuFgFrame& f);
void clear_record();
MgpuFgFrame make_empty_record();
// ---------------------------------------------------------------- instance bridge
// The module is deployed twice (dxgi.dll and nvngx_dlssg.dll), and the loader
// creates a separate instance for each name - so the instance that took the swap
// chain is NOT the one Streamline calls CreateFeature on. The dxgi instance is
// always the primary (a D3D12 title imports dxgi at load); the DLSS-G instance
// forwards the contract to it through this table instead of keeping a second,
// useless copy of the pipeline.
struct NgxBridge
{
    uint32_t size;
    NVSDK_NGX_Result (NVSDK_CONV* create)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                          NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    NVSDK_NGX_Result (NVSDK_CONV* evaluate)(ID3D12GraphicsCommandList*,
                                            const NVSDK_NGX_Handle*,
                                            const NVSDK_NGX_Parameter*, void*);
    NVSDK_NGX_Result (NVSDK_CONV* release)(NVSDK_NGX_Handle*);
    NVSDK_NGX_Result (NVSDK_CONV* requirements)(IDXGIAdapter*,
                                                const NVSDK_NGX_FeatureDiscoveryInfo*,
                                                NVSDK_NGX_FeatureRequirement*);
    NVSDK_NGX_Result (NVSDK_CONV* capability)(NVSDK_NGX_Parameter**);
    NVSDK_NGX_Result (NVSDK_CONV* populate)(NVSDK_NGX_Parameter*);
};

const MgpuFgApi* api_table();          // the table MgpuFgGetApi hands out


// ---------------------------------------------------------------- XeFG backend
bool     xefg_ready();
bool     xefg_start(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGIFactory2* factory,
                    HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, bool title_fullscreen,
                    IDXGISwapChain1** out_chain);
// The chain we already own, for the same window (OptiScaler's FGPreserveSwapChain), with
// one reference for the caller. nullptr when the window differs or nothing is owned.
IDXGISwapChain1* xefg_owned_chain(HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc);
void     xefg_enable(bool on);
void     xefg_set_count(unsigned n);
unsigned xefg_max_interp();
uint32_t xefg_next_frame_id();
// QueryPerformanceCounter in milliseconds. WGC's SystemRelativeTime is QPC-based,
// so every producer-side frame timestamp must use this clock, not GetTickCount64.
uint64_t now_qpc_ms();
// Present-time ring: begin() snapshots the record about to be presented, commit()
// writes the entry after the real Present. The consumer uses it to pair WGC colour
// with the producer's frame_id.

// v22: present-ring prototypes removed (publication slot is the identity).
// v3 producer sidecar: record the 16x16 pre-marker tile (R10) into the scar slot
// of THIS frame's publication, on `list`. `origin_x/y` are the marker's chain-space
// coordinates (published as metadata). Returns 0 on success; non-zero is the
// producer's reason (2 = not ready / no slot, -1 = API absent).
// (producer_record_scar is gone: the marker carries only a 32-bit code)
// Tag the frame's inputs. `list` must be the command list the title handed the frame
// over on: OptiScaler's SetResource records the tags there. `upscaler_lane` says the MV
// and depth came from the upscaler (FGInput=Upscaler), which also decides whether the
// backbuffer tag is sent (Dispatch() skips it for that lane).
void     xefg_tag(ID3D12GraphicsCommandList* list, uint32_t present_id, const MgpuFgFrame& f,
                  bool upscaler_lane = false);
void     xefg_log_status();
void     xefg_frame_feedback(bool tags_complete);
// The device the chain runs on (the transport places its slots on it).
ID3D12Device* xefg_device();
// Did the last xefg_tag() actually hand XeFG a depth and a velocity? Generation must
// stay off when it did not (XeFG_Dx12::Dispatch's own gate).
bool     xefg_tags_complete();
// TagAtEvaluate: record the tags where OptiScaler records them, inside the upscaler's
// Evaluate and on the title's still-open command list.
//   EvalTag_Inputs - before the real Evaluate is forwarded (Upscaler_Inputs_Dx12
//                    UpscaleStart): MV/depth (+ backbuffer/rect), ValidNow.
//   EvalTag_Color  - after it (UpscaleEnd): the upscaler's output has been written by
//                    then, so a ValidNow colour tag copies the frame that was produced.
// Returns true when `stage` actually tagged something; the caller only sends the colour
// stage when the inputs stage returned true.
enum EvalTagStage { EvalTag_Inputs = 0, EvalTag_Color = 1 };
bool     xefg_tag_at_evaluate(void* cmd_list, int stage);
// The frame the evaluate-side tag produced (post-MvResample), for the capture record.
// False when no upscaler tag happened since the last present.
bool     xefg_last_eval_frame(MgpuFgFrame* out);
bool     xefg_wants_generation();
// The sl::Constants camera frame (right/up/forward/position), used to build the view matrix.
void     xefg_camera(const float right[3], const float up[3], const float forward[3],
                     const float position[3]);
void     backend_shutdown();

// ---------------------------------------------------------------- provider
// Answer the game? Only when we actually own the output path (see §9 risk 4:
// answering without a presenter is the one failure mode that shows a black
// screen, so it is not allowed).
bool provider_answers();          // capability / requirements
bool provider_owns_output();      // take over CreateFeature/Evaluate
// Once the title's own inputs show XeFG cannot run (Activate()'s gate), the DLSS-G contract
// stops answering: the title then composes and presents its own frame at 1X instead of
// leaving the picture to a provider that is not generating (the black-screen shape).
void provider_block_fg(const char* why);
bool provider_fg_blocked();
void nvngx_withdraw_frame_generation();   // un-patches FrameGeneration.Available on the real block
// Full-resolution MV -> interpolation size (MvResample). Records one compute dispatch on the
// title's command list and rewrites f->stream[MV] to the resampled texture.
bool mv_resample_frame(ID3D12CommandList* list, MgpuFgFrame* f);
void mv_resample_set_device(ID3D12Device* device);
// OptiScaler's shader passes (passes.cpp).
void pass_set_device(ID3D12Device* device);
// Swap chain vtable hooks (fg_swapchain_hooks.cpp), OptiScaler's shape.
bool fg_hook_swapchain(IDXGISwapChain* chain);
// Bridges the detours use, implemented in backend_xefg.cpp.
void xefg_before_present(bool real_present);
void xefg_present_result(long hr);
void xefg_after_resize();
bool pass_render_ui(ID3D12CommandList* list, ID3D12Resource* sc_buffer, UINT w, UINT h,
                    ID3D12Resource* ui, uint32_t ui_state);
// code = 32-bit short-snowflake marker code (payload24 | crc8<<24).
bool pass_marker(ID3D12CommandList* list, ID3D12Resource* sc_buffer,
                 UINT w, UINT h, uint32_t code);
// (pass_norm_tile is gone with the scar tile transport)
bool pass_hud_copy(ID3D12CommandList* list, ID3D12Resource* hudless, ID3D12Resource* present,
                   uint32_t hudless_state, uint32_t present_state, float threshold);
// The UI resource the title declared (FGDrawUIOverFG).
void xefg_set_dlssg_ui(void* resource, uint32_t state);
// The upscaler lane's real colour resource (g_sr.color), used for the marker
// sidecar; the chain buffer is not guaranteed to hold it at present time.
void xefg_set_marker_color(void* resource, uint32_t format, uint32_t state,
                           uint32_t w, uint32_t h);
// v22: hand the current chain back buffer to the record as its colour stream. The title
// renders into this buffer (OwnSwapchain=1), so it is the frame the consumer must
// interpolate - the upscaler lane deliberately carries no colour.
bool xefg_record_backbuffer(MgpuFgFrame* f);
// Record the DLSS-G backbuffer -> current chain buffer copy on the title's own command list:
// OptiScaler's HudCopy placement (Nvngx_FG::D3D12_EvaluateFeature). False when it did not
// happen, in which case the present path does it itself.
bool xefg_copy_dlssg_backbuffer(ID3D12GraphicsCommandList* list, void* source,
                                unsigned long long eval_seq);
bool provider_create(ID3D12Device* device, ID3D12GraphicsCommandList* list, void* params,
                     void** out_handle);
bool provider_ours(const void* handle);
unsigned provider_evaluate(ID3D12GraphicsCommandList* list, const void* handle, void* params);
unsigned provider_release(const void* handle);
void provider_shutdown();

// Harvest: parameter block -> MgpuFgFrame (one frame).
void harvest(void* params, ID3D12GraphicsCommandList* list, MgpuFgFrame& out);

// Our own NVSDK_NGX_Parameter, for the case where no real runtime is present.
NVSDK_NGX_Parameter* new_parameter_block();
void destroy_parameter_block(NVSDK_NGX_Parameter* p);

// Streamline asks NvAPI for the GPU architecture before slInit; we answer it.
void arch_install_if_possible();

// The Streamline tag lane: the resources the title declared for this frame, with the state
// and lifecycle it chose (OptiScaler's Sl_Inputs). Returns how many it provided.
int sl_stage_inputs(MgpuFgFrame& f);

// The Streamline layer: the DLSS-G menu gate and our tag/Evaluate input path.
void sl_install_if_possible();
// Signalled by the module-load hook when sl.interposer.dll appears; the loader thread waits
// on it instead of polling GetModuleHandleW (which takes the loader lock while the game is
// loading its own DLLs).
void sl_wait_for_interposer(unsigned ms);
// Loads our own sl.interposer.dll + nvngx_dlssg.dll from <exe>\fakerFG\streamline\ by full
// path before the game asks for them (OptiScaler: proxies/Streamline_Proxy.h:88-105). The
// loader keys modules by base name, so the title's copies are never used and the layout of
// each game stops mattering. Returns true when our copy is in place.
bool sl_preload_own_copy();
bool sl_installed();

// The upscaler lane: OptiScaler's CP2077 profile takes MV/depth from DLSS-SR
// (`FGInput=Upscaler`), not from the DLSS-G parameter block - the SR textures are the
// two-channel / R32 pair XeFG expects. Recorded while forwarding, used by Evaluate.
void sr_harvest(void* ngx_params);
// True while the upscaler lane is live: a DLSS-SR Evaluate was seen in the last couple of
// seconds. `FgInput=Upscaler` must not claim the FG output without it (that is the
// black-screen shape: FG on, our chain in the title's hands, nothing to interpolate).
bool sr_lane_ready();
// Monotonic count of upscaler Evaluates (diagnostic: growth vs presents tells whether the
// lane is sparse or dead).
unsigned sr_harvest_count();
void   sr_note_command_list(void* cmd_list);
void*  sr_command_list();
// The present counter both files share: the input lane is judged per FRAME (OptiScaler:
// `_fgFramePresentId - _lastFGFramePresentId > 3`), never by wall clock.
extern uint32_t g_present_counter;
// Apply the upscaler's last frame (its MV/depth/render size/jitter/MV scale) to `f`.
// Returns false when the upscaler did not evaluate this frame.
bool sr_apply(MgpuFgFrame& f);

// No resource-state tracking and no barrier of a title resource: OptiScaler never
// touches one for XeFG (Sl_Inputs forwards the state the title declared), so there is
// nothing to track. res_track.cpp is not part of the build any more.

// OptiScaler's LibraryLoad_Hooks: the NGX core request is answered with our own module, so
// every NGX entry point of every plugin (DLSS-SR included) is routed through us.
void kernel_install_if_possible();
HMODULE ngx_real_core();   // the driver's own nvngx.dll/_nvngx.dll, used as forward target

// GetProcAddress interposition for the driver's NGX core (nvngx.dll / _nvngx.dll).
void nvngx_install_if_possible();
// The real GetProcAddress, so a forward target is never one of our own thunks.
void* ngx_real_get_proc(HMODULE m, const char* name);
void nvngx_scan_upscalers();   // hooks the upscaler modules' Evaluate export
bool nvngx_installed();

// ---------------------------------------------------------------- DXGI proxy
// The exports live in proxy_dxgi.cpp; these are the two things the rest of the
// module needs from them.
bool    dxgi_install_factory_hook(IUnknown* factory);
bool    dxgi_took_over_swapchain();
HMODULE dxgi_ensure_real();
void    dxgi_set_real_module(HMODULE m);
HMODULE dxgi_real_module();

} // namespace fakerfg
