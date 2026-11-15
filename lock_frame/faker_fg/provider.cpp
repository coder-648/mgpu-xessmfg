// ============================================================================
//  provider.cpp - we answer the DLSS-G contract, and harvest what it carries.
//
//  Two jobs, in this order, every Evaluate:
//    1. harvest: the parameter block -> one MgpuFgFrame (MV, depth, hudless, UI,
//       matrices, jitter, MV scale, reset, declared frame count). This is the
//       data the capture transports; it is a *record*, not a hook side effect.
//    2. publish + tag: hand the record to the capture API, and tag the same
//       resources with XeFG so it can present (and, in local mode, generate).
//
//  We do NOT write OutputInterpolated / OutputReal. That was the old, illegal
//  move: an observer cannot transition a title's swap-chain textures. Here the
//  presenter is XeFG and its backbuffer is the picture - nothing nobody wrote
//  can be presented, so the black-screen failure mode does not exist.
//
//  Provenance: handle space and parameter surface follow OptiScaler's
//  framegen/nvngx/Nvngx_FG.cpp (GPL-3.0, docs/design/FAKER_FG.md 7).
// ============================================================================
#include "fg_internal.h"

namespace fakerfg {
namespace {

constexpr uint64_t kHandleMagic = 0x4D47505546474831ull;   // 'MGPUFGH1'
constexpr int      kSlots = 4;

struct Handle
{
    uint64_t magic;
    uint32_t slot;
};

Handle        g_handles[kSlots]{};
bool          g_used[kSlots]{};
ID3D12Device* g_device = nullptr;
unsigned      g_evals = 0;
bool          g_fg_blocked = false;      // XeFG cannot run in this title (Activate() gate)

bool handle_ok(const void* h)
{
    if (h == nullptr) { return false; }
    const auto* p = static_cast<const Handle*>(h);
    return p->magic == kHandleMagic && p->slot < kSlots && g_used[p->slot];
}

// ---------------------------------------------------------------- read helpers
// Every Get's result is ignored on purpose: an absent parameter is normal (CP2077
// never sets OutputReal), and the record's zero means "not declared", which the
// capture already understands.
void get_res(NVSDK_NGX_Parameter* p, const char* name, MgpuFgResource& out)
{
    if (p == nullptr) { return; }
    ID3D12Resource* r = nullptr;
    if (p->Get(name, &r) != NVSDK_NGX_Result_Success || r == nullptr) { return; }
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    out.resource = r;
    out.state    = 0xFFFFFFFFu;           // not declared here - sl_stage_inputs() has it
    out.validity = MGPU_FG_RV_ONLY_NOW;   // OptiScaler: everything but eValidUntilPresent
    out.width    = static_cast<uint32_t>(d.Width);
    out.height   = d.Height;
    out.format   = static_cast<uint32_t>(d.Format);
}

void get_u32(NVSDK_NGX_Parameter* p, const char* name, uint32_t& out)
{
    if (p == nullptr) { return; }
    unsigned v = 0;
    if (p->Get(name, &v) == NVSDK_NGX_Result_Success) { out = v; }
}

void get_flag(NVSDK_NGX_Parameter* p, const char* name, uint32_t bit, uint32_t& flags)
{
    if (p == nullptr) { return; }
    int v = 0;
    if (p->Get(name, &v) == NVSDK_NGX_Result_Success && v != 0) { flags |= bit; }
}

void get_f32(NVSDK_NGX_Parameter* p, const char* name, float& out)
{
    if (p == nullptr) { return; }
    float v = 0.0f;
    if (p->Get(name, &v) == NVSDK_NGX_Result_Success) { out = v; }
}

// A zero matrix is what a title that never set the parameter leaves behind, so it
// counts as "not declared" and the library's own derivation wins.
void get_matrix(NVSDK_NGX_Parameter* p, const char* name, float (&out)[16], uint32_t bit,
                uint32_t& flags)
{
    if (p == nullptr) { return; }
    void* m = nullptr;
    if (p->Get(name, &m) != NVSDK_NGX_Result_Success || m == nullptr) { return; }
    memcpy(out, m, sizeof(float) * 16);
    bool any = false;
    for (int i = 0; i < 16; ++i) { if (out[i] != 0.0f) { any = true; break; } }
    if (any) { flags |= bit; }
    else { memset(out, 0, sizeof(float) * 16); }
}

} // namespace

// ---------------------------------------------------------------- harvest
void harvest(void* params, ID3D12GraphicsCommandList* list, MgpuFgFrame& f)
{
    auto* p = static_cast<NVSDK_NGX_Parameter*>(params);
    f = make_empty_record();
    f.cmd_list     = list;
    f.frame_qpc_ms = now_qpc_ms();
    f.mode         = api_mode();

    if (p == nullptr) { return; }

    // Resources: the contract's own list (FG_BACKEND.md 2).
    get_res(p, NVSDK_NGX_DLSSG_Parameter_Backbuffer, f.stream[MGPU_FG_STREAM_BACKBUFFER]);
    get_res(p, NVSDK_NGX_DLSSG_Parameter_MVecs, f.stream[MGPU_FG_STREAM_MV]);
    get_res(p, NVSDK_NGX_DLSSG_Parameter_Depth, f.stream[MGPU_FG_STREAM_DEPTH]);
    get_res(p, NVSDK_NGX_DLSSG_Parameter_HUDLess, f.stream[MGPU_FG_STREAM_HUDLESS]);
    get_res(p, NVSDK_NGX_DLSSG_Parameter_UI, f.stream[MGPU_FG_STREAM_UI_COLOR]);
    get_res(p, NVSDK_NGX_DLSSG_Parameter_UIAlpha, f.stream[MGPU_FG_STREAM_UI_ALPHA]);
    get_res(p, NVSDK_NGX_DLSSG_Parameter_OutputReal, f.stream[MGPU_FG_STREAM_OUTPUT_REAL]);
    get_res(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolated,
            f.stream[MGPU_FG_STREAM_OUTPUT_INTERP]);

    // Extents. The render size comes from the declared Width/Height when present,
    // else from the MV resource - that is the size the vectors are expressed in.
    uint32_t w = 0, h = 0;
    get_u32(p, NVSDK_NGX_Parameter_Width, w);
    get_u32(p, NVSDK_NGX_Parameter_Height, h);
    f.render_w = w;
    f.render_h = h;
    if (f.render_w == 0) { f.render_w = f.stream[MGPU_FG_STREAM_MV].width; }
    if (f.render_h == 0) { f.render_h = f.stream[MGPU_FG_STREAM_MV].height; }
    f.output_w = f.stream[MGPU_FG_STREAM_BACKBUFFER].width;
    f.output_h = f.stream[MGPU_FG_STREAM_BACKBUFFER].height;
    if (f.output_w == 0 && f.stream[MGPU_FG_STREAM_OUTPUT_REAL].resource != nullptr) {
        f.output_w = f.stream[MGPU_FG_STREAM_OUTPUT_REAL].width;
        f.output_h = f.stream[MGPU_FG_STREAM_OUTPUT_REAL].height;
    }

    // Flags and scalars.
    uint32_t flags = MGPU_FG_FRAME_FROM_FG;
    get_flag(p, NVSDK_NGX_DLSSG_Parameter_DepthInverted, MGPU_FG_FRAME_DEPTH_INVERTED, flags);
    get_flag(p, NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR, MGPU_FG_FRAME_HDR, flags);
    get_flag(p, NVSDK_NGX_DLSSG_Parameter_Reset, MGPU_FG_FRAME_RESET_HISTORY, flags);
    get_flag(p, NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, MGPU_FG_FRAME_CAMERA_MOTION,
             flags);
    if (f.stream[MGPU_FG_STREAM_MV].resource != nullptr) { flags |= MGPU_FG_FRAME_MV_ACTIVE; }
    if (f.stream[MGPU_FG_STREAM_MV].width > 0 && f.output_w > 0 &&
        f.stream[MGPU_FG_STREAM_MV].width > f.output_w) {
        // MV wider than the output: the consumer picks the matching scale.
        flags |= MGPU_FG_FRAME_HI_RES_MV;
    }
    if (cfg().mv_jittered) { flags |= MGPU_FG_FRAME_MV_JITTERED; }

    get_f32(p, NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, f.jitter_x);
    get_f32(p, NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, f.jitter_y);
    get_f32(p, NVSDK_NGX_DLSSG_Parameter_MvecScaleX, f.mv_scale_x);
    get_f32(p, NVSDK_NGX_DLSSG_Parameter_MvecScaleY, f.mv_scale_y);
    get_f32(p, NVSDK_NGX_DLSSG_Parameter_CameraNear, f.camera_near);
    get_f32(p, NVSDK_NGX_DLSSG_Parameter_CameraFar, f.camera_far);
    get_f32(p, NVSDK_NGX_DLSSG_Parameter_CameraFOV, f.camera_fov_y);
    get_f32(p, NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, f.aspect);
    get_matrix(p, NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, f.view_to_clip,
               MGPU_FG_FRAME_HAS_MATRIX, flags);
    get_matrix(p, NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip, f.clip_to_prev_clip,
               MGPU_FG_FRAME_HAS_PREV_CLIP, flags);
    get_matrix(p, NVSDK_NGX_DLSSG_Parameter_PrevClipToClip, f.prev_clip_to_clip, 0u, flags);

    uint32_t mf_count = 0;
    get_u32(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, mf_count);
    f.declared_frames = mf_count;

    f.flags = flags;

    // Camera declared? `CameraFOV` alone is enough to call it a camera.
    if (f.camera_near > 0.0f && f.camera_far > f.camera_near) {
        f.flags |= MGPU_FG_FRAME_HAS_CAMERA;
    }
}

// ---------------------------------------------------------------- provider
bool provider_fg_blocked() { return g_fg_blocked; }

// One-way. The library's own gate decides it (XeFG_Dx12::Activate), so once the title has
// shown the inputs it uses, there is no reason to keep promising frame generation: every
// later DLSS-G CreateFeature must fail so the title falls back to presenting its own frame.
void provider_block_fg(const char* why)
{
    if (g_fg_blocked) { return; }
    g_fg_blocked = true;
    nvngx_withdraw_frame_generation();
    logf("provider: frame generation withdrawn (%s) - the title keeps its own picture at 1X\n",
         (why != nullptr) ? why : "XeFG cannot run here");
}

bool provider_capture_dlssg()
{
    const Config& c = cfg();
    return c.fg_output == static_cast<int>(FgOutputSink::Capture) &&
           c.fg_input == static_cast<int>(FgInputSource::DLSSG);
}

bool provider_answers()
{
    const Config& c = cfg();
    if (c.passthrough != 0 ||
        c.fg_output == static_cast<int>(FgOutputSink::NoFG)) { return false; }
    return !g_fg_blocked;
}

bool provider_owns_output()
{
    const Config& c = cfg();
    if (c.passthrough != 0 ||
        c.fg_output == static_cast<int>(FgOutputSink::NoFG)) { return false; }
    // Withdrawn: the title asked for inputs XeFG cannot consume there, so the contract is
    // not ours any more - its DLSS-G CreateFeature goes to the real runtime (which fails on
    // a non-RTX GPU) and the title turns its own frame generation off.
    if (g_fg_blocked) { return false; }
    // Two conditions, both required: a live XeFG context AND the swap chain actually
    // in its hands. Answering the contract while the title still presents its own
    // chain is exactly the "black screen" shape, so it must be impossible here.
    // Ownership is decided by the configuration only (OptiScaler: `activeFgOutput ==
    // XeFG`, fixed at init). It must NOT follow the inputs: the missing-input case is
    // already handled per frame - sr_apply() reports "no inputs", xefg_frame_feedback()
    // parks XeFG at 1X - and flipping ownership mid-session leaves the title's feature
    // half ours, half the real runtime's, which is a black screen.
    if (!xefg_ready() || !dxgi_took_over_swapchain()) { return false; }
    return true;
}

bool provider_create(ID3D12Device* device, ID3D12GraphicsCommandList*, void*,
                     void** out_handle)
{
    if ((!provider_owns_output() && !provider_capture_dlssg()) || out_handle == nullptr) {
        return false;
    }
    // OptiScaler's semantics (Nvngx_FG.cpp:312-344): a provider answers only if it can really
    // create the feature. Ours cannot generate anything, so with FakeDlssgFeature=0 it
    // declines and the title turns its own frame generation off - which is what keeps the
    // picture on screen instead of a black "FG is on" swap chain.
    if (cfg().fake_dlssg_feature == 0) {
        static bool told = false;
        if (!told) {
            told = true;
            logf("provider: Declining DLSS-G CreateFeature (FakeDlssgFeature=0) - the title"
                 " keeps its own frame generation off\n");
        }
        return false;
    }

    for (int i = 0; i < kSlots; ++i) {
        if (g_used[i]) { continue; }
        g_used[i]     = true;
        g_handles[i]  = { kHandleMagic, static_cast<uint32_t>(i) };
        g_device      = device;
        *out_handle   = &g_handles[i];
        logf("provider: handle slot %d, FgOutput=%d interpolated=%d - CreateFeature is ours\n",
             i, cfg().fg_output, cfg().interpolated);
        return true;
    }
    logf("provider: no free handle slot (4 in use)\n");
    return false;
}

bool provider_ours(const void* handle)
{
    return handle_ok(handle);
}

unsigned provider_evaluate(ID3D12GraphicsCommandList* list, const void* handle, void* params)
{
    if (!handle_ok(handle)) { return 0; }
    // Withdrawn: report the failure so the title turns its own frame generation off (it is
    // the only way it learns that nothing is going to be generated).
    if (g_fg_blocked) { return 0; }
    ++g_evals;

    LARGE_INTEGER t0{}, fq{};
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&fq);
    struct Cost { ~Cost() {} };
    (void)fq;

    MgpuFgFrame f{};
    harvest(params, list, f);

    // Pace only after the parameter block yielded a usable frame. Loading/menu
    // Evaluates with no MV/depth must not be throttled.
    if (f.stream[MGPU_FG_STREAM_MV].resource != nullptr ||
        f.stream[MGPU_FG_STREAM_DEPTH].resource != nullptr) {
        // no external pace
    }

    // One input lane, exactly as configured (OptiScaler's FGInput):
    //   Upscaler -> Upscaler_Inputs_Dx12 (the upscaler's parameter block)
    //   DLSSG    -> Sl_Inputs::reportResource (the title's Streamline tags)
    //   NoFG     -> no FG inputs at all.
    // No fallback to another lane, and nothing the DLSS-G block declared leaks into the FG
    // inputs: Sl_Inputs only sets what the title tagged, so anything else stays empty and
    // Dispatch() treats Depth/Velocity as not ready for that frame.
    MgpuFgFrame fg_view = f;
    for (int i = 0; i < MGPU_FG_STREAM_COUNT; ++i) {
        if (i == MGPU_FG_STREAM_BACKBUFFER) { continue; }
        fg_view.stream[i] = MgpuFgResource{};
    }

    bool upscaler_lane = false;
    bool have_inputs   = false;
    if (cfg().fg_input == static_cast<int>(FgInputSource::Upscaler)) {
        upscaler_lane = sr_apply(fg_view);
        have_inputs   = upscaler_lane;
        if (!have_inputs) {
            // FgInput=Upscaler needs the DLSS-SR evaluate; without an upscaler running
            // (DLSS off / native resolution) there is no source in this lane at all.
            static bool told = false;
            if (!told) {
                told = true;
                logf("provider: FgInput=Upscaler but no upscaler evaluated - is DLSS Super"
                     " Resolution off in the game? (render=%ux%u out=%ux%u)\n",
                     f.render_w, f.render_h, f.output_w, f.output_h);
            }
        }
    } else if (cfg().fg_input == static_cast<int>(FgInputSource::DLSSG)) {
        have_inputs = sl_stage_inputs(fg_view) > 0;
    }

    const uint32_t present_id = xefg_next_frame_id();
    f.present_id = present_id;

    // The transport needs the device to place its slots on. The Upscaler lane fills
    // this in publish_upscaler_record(); the DLSSG lane publishes from here, so it
    // must do the same or the producer refuses with "record has no device".
    if (f.device == nullptr) {
        f.device = (g_device != nullptr) ? g_device : xefg_device();
    }

    // 1) the capture's view of this frame (publish before tagging: a consumer that
    //    records copies on the list must see the record it copies).
    publish_record(f);

    // 2) output sink: only XeFG consumes the record here. Capture leaves the title's
    //    own presentation untouched and the producer is already fed above.
    if (cfg().fg_output == static_cast<int>(FgOutputSink::XeFG) &&
        cfg().fake_sl_player == 0 && cfg().tag_at_evaluate == 0) {
        fg_output_submit(f);
    }
    // Remember the buffer the title calls its own backbuffer: with a presenting FG output it
    // is the only place its frame exists (measured: the swap chain gets the UI only).
    // Nvngx_FG.cpp:405-459: with FGHudCutoff set, the hudless is blended into the frame the
    // title declared, on this same list, before anything else looks at it.
    if (cfg().fg_hud_cutoff > 0 && list != nullptr) {
        const MgpuFgResource& hud = f.stream[MGPU_FG_STREAM_HUDLESS];
        const MgpuFgResource& bb = f.stream[MGPU_FG_STREAM_BACKBUFFER];
        if (hud.resource != nullptr && bb.resource != nullptr) {
            pass_hud_copy(list, static_cast<ID3D12Resource*>(hud.resource),
                          static_cast<ID3D12Resource*>(bb.resource),
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COMMON,
                          static_cast<float>(cfg().fg_hud_cutoff) / 10.0f);
        }
    }

    // WriteOutputReal: the DLSS-G contract's two outputs are the title's own textures and the
    // provider is what fills them (a real DLSS-G runtime writes both). The real frame is the
    // frame the title declared in DLSSG.Backbuffer; the interpolated one is our generator's
    // job and stays untouched until it exists. Everything runs on the title's open command
    // list, the same place a real provider does it.
    if (cfg().write_output_real != 0 && list != nullptr) {
        const MgpuFgResource& src = f.stream[MGPU_FG_STREAM_BACKBUFFER];
        const MgpuFgResource& dst = f.stream[MGPU_FG_STREAM_OUTPUT_REAL];
        if (src.resource != nullptr && dst.resource != nullptr) {
            auto* s2 = static_cast<ID3D12Resource*>(src.resource);
            auto* d2 = static_cast<ID3D12Resource*>(dst.resource);
            if (s2 != d2) {
                ID3D12GraphicsCommandList* cl = static_cast<ID3D12GraphicsCommandList*>(list);
                D3D12_RESOURCE_BARRIER b[2]{};
                b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b[0].Transition.pResource = d2;
                b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b[1].Transition.pResource = s2;
                b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                cl->ResourceBarrier(2, b);
                cl->CopyResource(d2, s2);
                b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                cl->ResourceBarrier(2, b);
                static bool told = false;
                if (!told) {
                    told = true;
                    logf("provider: OutputReal <- DLSSG.Backbuffer (%ux%u), contract satisfied\n",
                         src.width, src.height);
                }
            }
        }
    }

    // Keep-alive copy for a real DLSS-G plugin: it expects the provider to produce an
    // interpolated output. The dual-GPU path presents the separated stream itself, so this
    // is deliberately the real frame (a duplicate), not a generated one.
    if (cfg().write_output_interp != 0 && list != nullptr) {
        const MgpuFgResource& src = f.stream[MGPU_FG_STREAM_BACKBUFFER];
        const MgpuFgResource& dst = f.stream[MGPU_FG_STREAM_OUTPUT_INTERP];
        if (src.resource != nullptr && dst.resource != nullptr) {
            auto* s2 = static_cast<ID3D12Resource*>(src.resource);
            auto* d2 = static_cast<ID3D12Resource*>(dst.resource);
            if (s2 != d2) {
                ID3D12GraphicsCommandList* cl = static_cast<ID3D12GraphicsCommandList*>(list);
                D3D12_RESOURCE_BARRIER b[2]{};
                b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b[0].Transition.pResource = d2;
                b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b[1].Transition.pResource = s2;
                b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                cl->ResourceBarrier(2, b);
                cl->CopyResource(d2, s2);
                b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                cl->ResourceBarrier(2, b);
                static bool told_interp = false;
                if (!told_interp) {
                    told_interp = true;
                    logf("provider: OutputInterpolated <- DLSSG.Backbuffer (keep-alive)\n");
                }
            }
        }
    }

    // The UI resource, for the present-side RenderUI pass (FGDrawUIOverFG).
    xefg_set_dlssg_ui(f.stream[MGPU_FG_STREAM_UI_COLOR].resource,
                      f.stream[MGPU_FG_STREAM_UI_COLOR].state);

    // OptiScaler's placement (Nvngx_FG.cpp:419-459): the frame the title declares as its
    // backbuffer is copied onto the chain buffer it is about to present, on the title's own
    // list, inside this Evaluate. Nothing is queued or waited on from here. Off by default:
    // on Wuchang the app apparently keeps writing into the chain after this point and the
    // result flickers, so the present-path copy is the stable one.

    // Readiness is what the library accepted for THIS frame (Depth and Velocity), not
    // what the parameter block declared - XeFG_Dx12::Dispatch's own gate.
    xefg_frame_feedback((have_inputs && xefg_wants_generation()) ? xefg_tags_complete() : false);

    if (g_evals <= 3) {
        logf("provider: eval #%u id=%u FgOutput=%d render=%ux%u out=%ux%u flags=0x%X"
             " mv=%p(fmt %u) depth=%p(fmt %u) hudless=%p(fmt %u) ui=%p frames=%u\n",
             g_evals, present_id, cfg().fg_output,
             f.render_w, f.render_h, f.output_w, f.output_h, f.flags,
             f.stream[MGPU_FG_STREAM_MV].resource, f.stream[MGPU_FG_STREAM_MV].format,
             f.stream[MGPU_FG_STREAM_DEPTH].resource, f.stream[MGPU_FG_STREAM_DEPTH].format,
             f.stream[MGPU_FG_STREAM_HUDLESS].resource, f.stream[MGPU_FG_STREAM_HUDLESS].format,
             f.stream[MGPU_FG_STREAM_UI_COLOR].resource,
             f.declared_frames);
        // The title's own "backbuffer" from the DLSS-G block, next to the swap-chain buffer
        // it was handed: equal pointers mean it renders into our chain (and only the UI
        // reaches it), different ones mean it expects the provider to blit this into it.
        const MgpuFgResource& bb = f.stream[MGPU_FG_STREAM_BACKBUFFER];
        logf("provider: eval #%u DLSSG block: backbuffer=%p %ux%u fmt=%u state=0x%X"
             " hudless=%ux%u ui=%ux%u\n",
             g_evals, bb.resource, bb.width, bb.height, bb.format, bb.state,
             f.stream[MGPU_FG_STREAM_HUDLESS].width, f.stream[MGPU_FG_STREAM_HUDLESS].height,
             f.stream[MGPU_FG_STREAM_UI_COLOR].width, f.stream[MGPU_FG_STREAM_UI_COLOR].height);
    } else if ((g_evals % 600) == 0) {
        xefg_log_status();
        logf("provider: eval #%u (runs=%u) id=%u\n", g_evals, g_evals, present_id);
    }
    {
        LARGE_INTEGER t1{}, fq2{};
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&fq2);
        static double sum = 0.0, mx = 0.0;
        static unsigned n = 0;
        if (fq2.QuadPart != 0 && t0.QuadPart != 0) {
            const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                              static_cast<double>(fq2.QuadPart);
            sum += ms; ++n;
            if (ms > mx) { mx = ms; }
            if (n >= 300) {
                logf("provider: eval cost avg=%.3f max=%.3f ms over %u frames\n",
                     sum / n, mx, n);
                sum = 0.0; mx = 0.0; n = 0;
            }
        }
    }
    return 1;
}

unsigned provider_release(const void* handle)
{
    if (!handle_ok(handle)) { return 0; }
    const auto* h = static_cast<const Handle*>(handle);
    g_used[h->slot] = false;
    // The pointer itself is NOT cleared here: measured, the title releases the DLSS-G feature
    // and keeps rendering into that same buffer (picture stays correct), so the texture
    // outlives the handle. What does invalidate it is a ResizeBuffers - see
    // xefg_resize_notify(), which clears it.
    logf("provider: handle slot %u released\n", h->slot);
    return 1;
}

void provider_shutdown()
{
    for (int i = 0; i < kSlots; ++i) { g_used[i] = false; }
    g_device = nullptr;
    backend_shutdown();
    clear_record();
}

} // namespace fakerfg
