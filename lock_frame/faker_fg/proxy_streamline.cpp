// ============================================================================
//  proxy_streamline.cpp - the part that makes frame generation *exist* on titles
//  that go through Streamline (CP2077 is the reference one).
//
//  Why this file is necessary (learned from OptiScaler, then confirmed in our own
//  live log): CP2077 does not ask NGX whether DLSS-G exists - it asks Streamline,
//  and `sl.interposer` answers by asking its `sl.dlss_g` plugin. Being
//  `nvngx_dlssg.dll` is therefore invisible to the menu. OptiScaler hooks six
//  Streamline exports and answers for `kFeatureDLSS_G` itself
//  (hooks/Streamline_Hooks.cpp: hkslIsFeatureSupported / IsFeatureLoaded /
//  GetFeatureRequirements / GetFeatureVersion / GetFeatureFunction /
//  SetFeatureLoaded), which is what makes the option appear at all.
//
//  On top of that this file is our input side: `slSetTag` / `slSetTagForFrame` /
//  `slSetConstants` stage the frame's HUDless / MV / depth / UI and the camera,
//  and `slEvaluateFeature(kFeatureDLSS_G)` turns the staging into one
//  MgpuFgFrame, publishes it for the capture, and tags XeFG for the present.
//
//  slInit drops kFeatureDLSS_G from `featuresToLoad` (OptiScaler does the same),
//  so the real plugin is never loaded and there is exactly one FG pipeline: ours.
// ============================================================================
#include "fg_internal.h"

#include "sl.h"
#include "sl_dlss_g.h"
#include "sl_pcl.h"

#include <MinHook.h>

#include <cmath>
#include <cstring>

namespace fakerfg {
namespace {

// ---------------------------------------------------------------- staging
// One frame's worth of declarations, filled by the tag lanes and consumed by
// Evaluate. Single-buffered on purpose: tags and Evaluate belong to the same
// frame and the same thread.
//
// The state and the lifecycle are NOT ours to guess: the title declares both in
// `sl::Resource::state` / `sl::ResourceTag::lifecycle`, and OptiScaler forwards them
// verbatim (Sl_Inputs::reportResource):
//
//     auto validity = (tag.lifecycle == sl::eValidUntilPresent) ? UntilPresent : ValidNow;
//     res.cmdList   = (ID3D12GraphicsCommandList*) cmdBuffer;   // Critical for eOnlyValidNow
//     res.state     = (D3D12_RESOURCE_STATES) tag.resource->state;
struct StageEntry
{
    ID3D12Resource* resource      = nullptr;
    uint32_t        state         = UINT_MAX;   // sl::Resource::state (UINT_MAX = not declared)
    bool            until_present = false;      // lifecycle == eValidUntilPresent
    // sl::ResourceTag::extent (zero when the title did not declare one). OptiScaler uses it
    // before the resource description (Streamline_Inputs.cpp:389-390), which matters when a
    // bigger texture carries a smaller valid region.
    uint32_t        ext_w         = 0;
    uint32_t        ext_h         = 0;
};

struct Stage
{
    StageEntry hudless{};
    StageEntry scaling{};
    StageEntry mv{};
    StageEntry depth{};
    StageEntry ui{};
    // P0: UI alpha flavours. Streamline can tag alpha either combined with the colour
    // (23, above) or as a separate plane: kBufferTypeUIAlpha (68, alpha only) and
    // kBufferTypeScalingOutputAlpha (67, the alpha that overrides ScalingOutputColor).
    // They are recorded but not transported yet (P4 consumes them).
    StageEntry ui_alpha{};
    uint32_t   ui_alpha_type = 0;      // sl::BufferType that filled ui_alpha (0 = none)
    StageEntry backbuffer{};
    void*           cmd        = nullptr;       // the frame's command buffer (from the tag call)

    unsigned char constants_raw[512]{};   // sl::Constants copy (opaque)
    bool          have_constants = false;

    bool          game_enabled = false;
    uint32_t      game_frames  = 1;
    uint32_t      frame_token  = 0;    // sl::FrameToken of the frame being published
};

Stage g_stage{};

// One publication per Streamline frame. DLSS-G Evaluate and the PCL ePresentStart
// fallback describe the SAME frame and carry the SAME FrameToken, but they used to call
// publish_record() twice: two seqs were allocated while the producer copied/published
// only the first record. The marker was drawn with the newer seq, so ~1/3 of the Wuchang
// colour frames had no matching publication (`no_slot`) even though every present was
// published. Collapse the two into one record by token.
static uint32_t g_sl_published_token = 0;
static uint32_t g_sl_eval_token = 0;
static uint32_t g_sl_staged_token = 0;
static unsigned g_sl_pub_calls = 0;
static unsigned g_sl_pub_collapsed = 0;
static unsigned g_sl_staged_skipped = 0;
static void sl_publish_record(const MgpuFgFrame& f, uint32_t token, const char* src)
{
    ++g_sl_pub_calls;
    if (token != 0 && token == g_sl_published_token) {
        ++g_sl_pub_collapsed;
        static unsigned logged = 0;
        if (logged++ < 3) {
            logf("sl: frame token %u already published - %s duplicate collapsed"
                 " (eval=%u staged=%u)\n", token, src, g_sl_eval_token, g_sl_staged_token);
        }
        return;
    }
    g_sl_published_token = token;
    publish_record(f);
    if ((g_sl_pub_calls % 600u) == 0u) {
        logf("sl: publish calls=%u collapsed=%u staged_skipped=%u (eval token=%u staged token=%u)\n",
             g_sl_pub_calls, g_sl_pub_collapsed, g_sl_staged_skipped,
             g_sl_eval_token, g_sl_staged_token);
    }
}

// ---------------------------------------------------------------- real entries
typedef sl::Result (*sl_pfn_supported)(sl::Feature, const sl::AdapterInfo&);
typedef sl::Result (*sl_pfn_loaded)(sl::Feature, bool&);
typedef sl::Result (*sl_pfn_requirements)(sl::Feature, sl::FeatureRequirements&);
typedef sl::Result (*sl_pfn_version)(sl::Feature, sl::FeatureVersion&);
typedef sl::Result (*sl_pfn_get_func)(sl::Feature, const char*, void*&);
typedef sl::Result (*sl_pfn_set_loaded)(sl::Feature, bool);
typedef sl::Result (*sl_pfn_init)(const sl::Preferences&, uint64_t);
typedef sl::Result (*sl_pfn_set_tag)(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t,
                                  sl::CommandBuffer*);
typedef sl::Result (*sl_pfn_set_tag_frame)(const sl::FrameToken&, const sl::ViewportHandle&,
                                        const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
typedef sl::Result (*sl_pfn_set_constants)(const sl::Constants&, const sl::FrameToken&,
                                        const sl::ViewportHandle&);
typedef sl::Result (*sl_pfn_evaluate)(sl::Feature, const sl::FrameToken&,
                                   const sl::BaseStructure**, uint32_t, sl::CommandBuffer*);
typedef sl::Result (*sl_pfn_set_options)(const sl::ViewportHandle&, const sl::DLSSGOptions&);
typedef sl::Result (*sl_pfn_get_state)(const sl::ViewportHandle&, sl::DLSSGState&,
                                       const sl::DLSSGOptions*);
typedef sl::Result (*sl_pfn_pcl_marker)(sl::PCLMarker, const sl::FrameToken&);

sl_pfn_supported     o_supported      = nullptr;
sl_pfn_loaded        o_loaded         = nullptr;
sl_pfn_requirements  o_requirements   = nullptr;
sl_pfn_version       o_version        = nullptr;
sl_pfn_get_func      o_get_func       = nullptr;
sl_pfn_set_loaded    o_set_loaded     = nullptr;
sl_pfn_init          o_init           = nullptr;
sl_pfn_set_tag       o_set_tag        = nullptr;
sl_pfn_set_tag_frame o_set_tag_frame  = nullptr;
sl_pfn_set_constants o_set_constants  = nullptr;
sl_pfn_evaluate      o_evaluate       = nullptr;
sl_pfn_set_options   o_set_options    = nullptr;
sl_pfn_get_state     o_get_state      = nullptr;
sl_pfn_pcl_marker    o_pcl_marker     = nullptr;

bool    g_sl_installed  = false;
bool    g_sl_tried      = false;
HMODULE g_interposer = nullptr;

// ---------------------------------------------------------------- helpers
// CP2077 tags HUDLESS_COLOR as onlyValidNow. At present-staged that resource can
// already be cleared/reused, which is exactly what produced a black HUDless stream.
// Snapshot it on the tag's own command list into a stable texture, so the producer
// reads real pixels later in the same frame.
struct HudlessSnapshot
{
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_DESC desc{};
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};
static HudlessSnapshot g_hud_snap[4];
static UINT g_hud_snap_i = 0;

static bool snapshot_hudless(const sl::ResourceTag& tag, void* cmd, StageEntry& slot)
{
    if (tag.resource == nullptr || tag.resource->native == nullptr || cmd == nullptr) {
        return false;
    }
    auto* src = reinterpret_cast<ID3D12Resource*>(tag.resource->native);
    auto* cl  = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd);
    ID3D12Device* dev = nullptr;
    if (FAILED(src->GetDevice(IID_PPV_ARGS(&dev)))) { return false; }
    const D3D12_RESOURCE_DESC d = src->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) { dev->Release(); return false; }

    // FP16 scRGB HUDless is converted once to R10 PQ. The UI synth only needs H and its
    // transport convention is PQ, so this halves both the snapshot write and the later H
    // read (66 MB + 66 MB -> 33 MB + 33 MB). Other formats keep the exact copy path.
    const bool to_pq = cfg().ui_snapshot_pq != 0 &&
                       (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                        d.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS);
    const DXGI_FORMAT out_fmt = to_pq ? DXGI_FORMAT_R10G10B10A2_UNORM : d.Format;

    HudlessSnapshot& s = g_hud_snap[g_hud_snap_i];
    if (s.resource == nullptr || s.desc.Width != d.Width || s.desc.Height != d.Height ||
        s.desc.Format != out_fmt) {
        if (s.resource != nullptr) { s.resource->Release(); s.resource = nullptr; }
        D3D12_RESOURCE_DESC rd = d;
        rd.MipLevels = 1;
        rd.DepthOrArraySize = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = to_pq ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                         : D3D12_RESOURCE_FLAG_NONE;
        rd.Format = out_fmt;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1;
        hp.VisibleNodeMask = 1;
        if (FAILED(dev->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                to_pq ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&s.resource)))) {
            dev->Release();
            s.resource = nullptr;
            return false;
        }
        s.desc = rd;
        s.state = to_pq ? D3D12_RESOURCE_STATE_COMMON
                        : D3D12_RESOURCE_STATE_COPY_DEST;
    }
    pass_set_device(dev);
    dev->Release();

    const D3D12_RESOURCE_STATES declared =
        static_cast<D3D12_RESOURCE_STATES>(tag.resource->state);
    if (declared == static_cast<D3D12_RESOURCE_STATES>(0xFFFFFFFFu)) {
        return false;   // no declared state: do not guess with a barrier
    }
    // OptiScaler's CyberpunkHudlessState quirk: CP2077 declares HUDLESS as
    // PIXEL|NON_PIXEL but the resource is actually a UAV. Using the declared state
    // for the barrier/copy reads undefined data (black). Treat it as UAV.
    D3D12_RESOURCE_STATES before = declared;
    if (before == (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) {
        before = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (to_pq) {
        if (s.state != D3D12_RESOURCE_STATE_COMMON) {
            D3D12_RESOURCE_BARRIER sb{};
            sb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            sb.Transition.pResource = s.resource;
            sb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            sb.Transition.StateBefore = s.state;
            sb.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            cl->ResourceBarrier(1, &sb);
            s.state = D3D12_RESOURCE_STATE_COMMON;
        }
        if (!pass_hudless_to_pq(cl, src, before, s.resource,
                                static_cast<UINT>(d.Width), d.Height)) {
            return false;
        }
        s.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    } else {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            cl->ResourceBarrier(1, &b);
        }
        if (s.state != D3D12_RESOURCE_STATE_COPY_DEST) {
            b.Transition.pResource = s.resource;
            b.Transition.StateBefore = s.state;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            cl->ResourceBarrier(1, &b);
            s.state = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        cl->CopyResource(s.resource, src);
        if (before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            b.Transition.pResource = src;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = before;
            cl->ResourceBarrier(1, &b);
        }
        b.Transition.pResource = s.resource;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
        s.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    slot.resource = s.resource;
    slot.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    slot.until_present = true;
    g_hud_snap_i = (g_hud_snap_i + 1u) % 4u;
    static bool logged = false;
    if (!logged) {
        logged = true;
        logf("sl: HUDLESS snapshot (onlyValidNow) %ux%u fmt %u -> %s\n",
             static_cast<unsigned>(d.Width), d.Height, static_cast<unsigned>(d.Format),
             to_pq ? "R10 PQ (compute, no full copy)" : "stable copy");
    }
    return true;
}

void remember(StageEntry& slot, const sl::ResourceTag& tag, void* cmd)
{
    if (tag.resource == nullptr || tag.resource->native == nullptr) { return; }
    if (tag.type == sl::kBufferTypeHUDLessColor &&
        tag.lifecycle != sl::eValidUntilPresent && snapshot_hudless(tag, cmd, slot)) {
        slot.ext_w = tag.extent.width;
        slot.ext_h = tag.extent.height;
        if (cmd != nullptr) { g_stage.cmd = cmd; }
        return;
    }
    slot.resource      = reinterpret_cast<ID3D12Resource*>(tag.resource->native);
    slot.state         = tag.resource->state;                       // the title's declaration
    // Sl_Inputs::reportResource (Streamline_Inputs.cpp:344-359):
    //   * eValidUntilPresent -> UntilPresent, anything else -> ValidNow;
    //   * a non-UntilPresent tag with no command buffer cannot be copied later, so it is
    //     "YOLOed" to UntilPresent;
    //   * FGDepthValidNow / FGVelocityValidNow / FGHudlessValidNow force ValidNow even when
    //     the title declared something else (kept on UntilPresent when there is no list,
    //     because our copy needs one).
    bool force_now = false;
    switch (tag.type) {
    case sl::kBufferTypeDepth:
    case sl::kBufferTypeHiResDepth:
    case sl::kBufferTypeLinearDepth:     force_now = cfg().fg_depth_valid_now != 0; break;
    case sl::kBufferTypeMotionVectors:   force_now = cfg().fg_velocity_valid_now != 0; break;
    case sl::kBufferTypeHUDLessColor:
    case sl::kBufferTypeScalingOutputColor:
                                         force_now = cfg().fg_hudless_valid_now != 0; break;
    default: break;
    }
    const bool until_by_lifecycle = (tag.lifecycle == sl::eValidUntilPresent);
    if (force_now && cmd != nullptr) {
        slot.until_present = false;                                  // ValidNow
    } else {
        slot.until_present = until_by_lifecycle || (cmd == nullptr); // YOLO without a list
    }
    slot.ext_w         = tag.extent.width;
    slot.ext_h         = tag.extent.height;
    if (cmd != nullptr) { g_stage.cmd = cmd; }
}

// P0 census (read-only): one line per newly-seen tag type per session. It never changes
// what is staged or published; a single run answers "which planes does this title
// actually hand over?" (23/67/68/hudless/backbuffer/...).
static uint32_t g_tag_census = 0;

static void census_tag(const char* name, uint32_t bit, const sl::ResourceTag& t)
{
    if ((g_tag_census & bit) != 0) { return; }
    g_tag_census |= bit;
    const bool until = (t.lifecycle == sl::eValidUntilPresent);
    if (t.resource == nullptr) {
        logf("sl: tags %s (null resource, lc=%s)\n", name, until ? "until_present" : "now");
        return;
    }
    // sl::Resource carries width/height/state but not the DXGI format; take it from the
    // D3D12 object the tag points at (this proxy is D3D12-only).
    unsigned fmt = 0;
    if (t.resource->native != nullptr) {
        const D3D12_RESOURCE_DESC d =
            reinterpret_cast<ID3D12Resource*>(t.resource->native)->GetDesc();
        fmt = static_cast<unsigned>(d.Format);
    }
    logf("sl: tags %s %ux%u fmt %u state 0x%X lc=%s extent %ux%u\n",
         name, t.resource->width, t.resource->height, fmt, t.resource->state,
         until ? "until_present" : "now", t.extent.width, t.extent.height);
}

// v32 cost isolation: the HUDless snapshot is a full-resolution CopyResource on the
// render card. It is only useful when this session actually publishes a UI stream
// (render-synth) or a clean HUDless stream. With both switches off, skip the tag copy so
// the "feature off" baseline really has no extra render-card work.
static bool hudless_staging_enabled()
{
    // Re-evaluate per call: the consumer can start after the game. With no consumer there
    // is nothing to transport, so the full-resolution snapshot must not run.
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) { *(slash + 1) = 0; }
    wcscat_s(path, MAX_PATH, L"fakerFG.ini");
    const int ui = GetPrivateProfileIntW(L"fakerFG", L"UiStream", 0, path);
    const int hl = GetPrivateProfileIntW(L"fakerFG", L"HudlessStream", 0, path);
    HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, L"Local\\mgpu_consumer_present_v1");
    const bool consumer = (ev != nullptr);
    if (ev != nullptr) { CloseHandle(ev); }
    const bool enabled = (ui != 0 || hl != 0) && consumer;
    static int last = -1;
    if (last != (enabled ? 1 : 0)) {
        last = enabled ? 1 : 0;
        logf("sl: HUDless staging %s (UiStream=%d HudlessStream=%d consumer=%d)\n",
             enabled ? "ON" : "OFF", ui, hl, consumer ? 1 : 0);
    }
    return enabled;
}

// v4: CP2077 delivers the HUDless plane as onlyValidNow. Build the UI on the tag's own
// command list, using the current chain backbuffer as P, so the later record has a ready
// UI texture and no full-resolution HUDless snapshot is required.
static bool tag_time_synth(const sl::ResourceTag& tag, void* cmd, StageEntry& slot)
{
    if (cfg().ui_tag_synth == 0) { return false; }
    if (tag.resource == nullptr || tag.resource->native == nullptr || cmd == nullptr) {
        return false;
    }
    // CP2077 tags ScalingOutputColor before HUDless. It is the frame's final scaling
    // output (P), while the chain's "current" index at tag time can still be the previous
    // buffer. Prefer the explicitly tagged scaling output; fall back to the chain buffer.
    const StageEntry* p = g_stage.scaling.resource != nullptr ? &g_stage.scaling : nullptr;
    void* p_res = nullptr;
    uint32_t p_state = 0;
    if (p != nullptr) {
        p_res = p->resource;
        p_state = p->state;
    } else {
        MgpuFgFrame pb = make_empty_record();
        if (!xefg_record_backbuffer(&pb)) { return false; }
        const MgpuFgResource& bb = pb.stream[MGPU_FG_STREAM_BACKBUFFER];
        if (bb.resource == nullptr) { return false; }
        p_res = bb.resource;
        p_state = bb.state;
    }
    if (!producer_ui_tag(cmd, p_res, p_state,
                         reinterpret_cast<void*>(tag.resource->native),
                         tag.resource->state)) {
        return false;
    }
    slot.resource      = reinterpret_cast<ID3D12Resource*>(tag.resource->native);
    slot.state         = tag.resource->state;
    slot.until_present = false;      // only used if the pending tag synth is lost
    slot.ext_w         = tag.extent.width;
    slot.ext_h         = tag.extent.height;
    if (cmd != nullptr) { g_stage.cmd = cmd; }
    static bool told = false;
    if (!told) {
        told = true;
        logf("sl: tag-time UI synth accepted - HUDless snapshot skipped\n");
    }
    return true;
}

void record_tags(const sl::ResourceTag* tags, uint32_t count, void* cmd)
{
    if (tags == nullptr) { return; }
    for (uint32_t i = 0; i < count; ++i) {
        const sl::ResourceTag& t = tags[i];
        switch (t.type) {
        case sl::kBufferTypeHUDLessColor:
            census_tag("hudless", 1u << 0, t);
            if (hudless_staging_enabled()) {
                if (!tag_time_synth(t, cmd, g_stage.hudless)) {
                    remember(g_stage.hudless, t, cmd);
                }
            }
            break;
        case sl::kBufferTypeScalingOutputColor:
            census_tag("scaling_output", 1u << 1, t);
            // CP2077 tags HUDLESS_COLOR onlyValidNow (its content can already be
            // cleared at present). SCALING_OUTPUT is the upscaled scene image and is
            // tagged validUntilPresent; use it as the HUDless plane when available.
            if (hudless_staging_enabled()) { remember(g_stage.scaling, t, cmd); }
            {
                static bool told = false;
                if (!told && t.resource != nullptr) {
                    told = true;
                    logf("sl: SCALING_OUTPUT used as HUDless fallback (state=0x%X lc=%d)\n",
                         t.resource->state, static_cast<int>(t.lifecycle));
                }
            }
            break;
        case sl::kBufferTypeMotionVectors:
            census_tag("mv", 1u << 2, t);
            remember(g_stage.mv, t, cmd);
            break;
        // Sl_Inputs::GetResourceType maps all three depth flavours onto Depth
        // (Streamline_Inputs.cpp:287-300). A title that only tags HiResDepth or
        // LinearDepth used to reach us with no depth at all.
        case sl::kBufferTypeDepth:
            census_tag("depth", 1u << 3, t);
            remember(g_stage.depth, t, cmd);
            break;
        case sl::kBufferTypeHiResDepth:
            census_tag("hires_depth", 1u << 4, t);
            remember(g_stage.depth, t, cmd);
            break;
        case sl::kBufferTypeLinearDepth:
            census_tag("linear_depth", 1u << 5, t);
            remember(g_stage.depth, t, cmd);
            break;
        case sl::kBufferTypeUIColorAndAlpha:
            census_tag("ui_color_alpha", 1u << 6, t);
            remember(g_stage.ui, t, cmd);
            break;
        // P0: alpha-only flavours (Streamline core types 68 / 67). Recorded, not
        // transported yet; a title that only tags one of these currently looks like
        // "no UI" to the producer, which is exactly what the census is meant to expose.
        case sl::kBufferTypeUIAlpha:
            census_tag("ui_alpha", 1u << 7, t);
            if (t.resource != nullptr && t.resource->native != nullptr) {
                remember(g_stage.ui_alpha, t, cmd);
                g_stage.ui_alpha_type = static_cast<uint32_t>(t.type);
            }
            break;
        case sl::kBufferTypeScalingOutputAlpha:
            census_tag("scaling_output_alpha", 1u << 8, t);
            if (g_stage.ui_alpha.resource == nullptr &&
                t.resource != nullptr && t.resource->native != nullptr) {
                remember(g_stage.ui_alpha, t, cmd);
                g_stage.ui_alpha_type = static_cast<uint32_t>(t.type);
            }
            break;
        case sl::kBufferTypeBackbuffer:
            census_tag("backbuffer", 1u << 9, t);
            remember(g_stage.backbuffer, t, cmd);
            break;
        default: break;
        }
    }
}

// ---------------------------------------------------------------- camera from matrix
// Sl_Inputs::CaptureConstants' loadCameraMatrix (Streamline_Inputs.cpp:153-201): when the
// title's own camera fields are unusable, near/far/fov are recovered from
// cameraViewToClip. OptiScaler recomputes for every non-UE title; here the derived values
// only fill fields that are missing/implausible, so a title that reports a correct camera
// is never overridden (UE passes a matrix that is not a usable projection, which is why
// OptiScaler skips the recalculation for UE at all).
bool projection_camera(const sl::Constants& c, float& out_near, float& out_far, float& out_fov)
{
    if (c.orthographicProjection == sl::Boolean::eTrue) { return false; }

    // Row major, exactly the layout Streamline documents (sl_consts.h:177-181).
    float m[16]{};
    memcpy(m, &c.cameraViewToClip, sizeof(m));
    bool any = false;
    for (int i = 0; i < 16; ++i) { if (m[i] != 0.0f) { any = true; break; } }
    if (!any) { return false; }
    static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    if (memcmp(m, kIdentity, sizeof(m)) == 0) { return false; }

    const double b  = m[5];       // projMatrix[1][1]
    const double cc = m[10];      // projMatrix[2][2]
    const double d  = m[14];      // projMatrix[3][2]
    const double e  = m[11];      // projMatrix[2][3]
    if (!(std::fabs(b) > 1e-6)) { return false; }

    double zn = 0.0;
    double zf = 0.0;
    if (e < 0.0) {
        zn = (cc == 0.0) ? 0.0 : (d / cc);
        zf = (cc + 1.0 == 0.0) ? 0.0 : (d / (cc + 1.0));
    } else {
        zn = (cc == 0.0) ? 0.0 : (-d / cc);
        zf = (cc - 1.0 == 0.0) ? 0.0 : (-d / (cc - 1.0));
    }
    if (c.depthInverted == sl::Boolean::eTrue) {
        const double t = zn;
        zn = zf;
        zf = t;
    }
    const double fov = 2.0 * std::atan(1.0 / std::fabs(b));
    if (!std::isfinite(zn) || !std::isfinite(zf) || !std::isfinite(fov)) { return false; }
    if (!(zn > 0.0) || !(zf > zn) || !(fov > 0.05) || !(fov < 2.5)) { return false; }

    out_near = static_cast<float>(zn);
    out_far  = static_cast<float>(zf);
    out_fov  = static_cast<float>(fov);
    return true;
}

// ---------------------------------------------------------------- the six answers
// The menu gate. OptiScaler's behaviour (hooks/Streamline_Hooks.cpp) in intent.
// `note_feature` exists because the gate is invisible otherwise: CP2077 dropping DLSS
// from its menu looks identical whether it never asked, or asked and got an error - and
// the difference decides whether the fix is here or in the NGX core. Bounded: one line
// per (call, feature) pair, at most 24 lines.
void note_feature(const char* what, sl::Feature feature, sl::Result r)
{
    struct Seen { const char* what; int feature; int result; };
    static Seen seen[24]{};
    static int  n = 0;
    for (int i = 0; i < n; ++i) {
        if (seen[i].feature == static_cast<int>(feature) && strcmp(seen[i].what, what) == 0) {
            if (seen[i].result != static_cast<int>(r)) {
                seen[i].result = static_cast<int>(r);
                logf("sl: %s(feature=%d) -> %d (changed)\n", what,
                     static_cast<int>(feature), static_cast<int>(r));
            }
            return;
        }
    }
    if (n >= 24) { return; }
    seen[n++] = { what, static_cast<int>(feature), static_cast<int>(r) };
    logf("sl: %s(feature=%d) -> %d\n", what, static_cast<int>(feature), static_cast<int>(r));
}

sl::Result hk_supported(sl::Feature feature, const sl::AdapterInfo& adapter)
{
    if (feature == sl::kFeatureDLSS_G) {
        note_feature("slIsFeatureSupported", feature, sl::Result::eOk);
        return sl::Result::eOk;
    }
    const sl::Result r = (o_supported != nullptr)
                             ? o_supported(feature, adapter)
                             : sl::Result::eErrorNoSupportedAdapterFound;
    note_feature("slIsFeatureSupported", feature, r);
    return r;
}

sl::Result hk_loaded(sl::Feature feature, bool& loaded)
{
    if (feature == sl::kFeatureDLSS_G) {
        loaded = true;
        note_feature("slIsFeatureLoaded", feature, sl::Result::eOk);
        return sl::Result::eOk;
    }
    const sl::Result r = (o_loaded != nullptr) ? o_loaded(feature, loaded)
                                               : sl::Result::eErrorFeatureMissing;
    note_feature("slIsFeatureLoaded", feature, r);
    return r;
}

sl::Result hk_requirements(sl::Feature feature, sl::FeatureRequirements& req)
{
    if (feature == sl::kFeatureDLSS_G) {
        note_feature("slGetFeatureRequirements", feature, sl::Result::eOk);
        return sl::Result::eOk;
    }
    const sl::Result r = (o_requirements != nullptr) ? o_requirements(feature, req)
                                                     : sl::Result::eErrorFeatureMissing;
    note_feature("slGetFeatureRequirements", feature, r);
    return r;
}

sl::Result hk_version(sl::Feature feature, sl::FeatureVersion& version)
{
    if (feature == sl::kFeatureDLSS_G) {
        version.versionSL  = { 2, 11, 1 };
        version.versionNGX = { 4, 2, 0 };
        note_feature("slGetFeatureVersion", feature, sl::Result::eOk);
        return sl::Result::eOk;
    }
    const sl::Result r = (o_version != nullptr) ? o_version(feature, version)
                                                : sl::Result::eErrorFeatureMissing;
    note_feature("slGetFeatureVersion", feature, r);
    return r;
}

sl::Result hk_set_loaded(sl::Feature feature, bool loaded)
{
    if (feature == sl::kFeatureDLSS_G) {
        note_feature("slSetFeatureLoaded", feature, sl::Result::eOk);
        return sl::Result::eOk;
    }
    const sl::Result r = (o_set_loaded != nullptr) ? o_set_loaded(feature, loaded)
                                                   : sl::Result::eOk;
    note_feature("slSetFeatureLoaded", feature, r);
    return r;
}

// slDLSSGSetOptions: the game turning frame generation on/off. We record it (it
// decides nothing by itself - FgOutput does) and answer eOk so the menu keeps state.
sl::Result dummy_set_options(const sl::ViewportHandle&, const sl::DLSSGOptions& options)
{
    const bool enabled = options.mode != sl::DLSSGMode::eOff;
    uint32_t   frames  = options.numFramesToGenerate;
    if (frames < 1) { frames = 1; }

    // P0 (read-only): does this engine even know about UI recomposition? We never modify
    // the options here - the runtime that would consume the flag is stripped on this lane
    // (strip_dlssg=1), so writing it has no effect. This line only answers "is it set?".
    // structVersion guards the v4 field: an older game passes a shorter struct.
    {
        static int told_uir = -2;
        const int uir = (options.structVersion >= sl::kStructVersion4)
                            ? static_cast<int>(options.enableUserInterfaceRecomposition)
                            : -1;
        if (uir != told_uir) {
            told_uir = uir;
            logf("sl: SetOptions mode=%u frames=%u structVersion=%zu uir=%d uiFmt=%u hudFmt=%u\n",
                 static_cast<unsigned>(options.mode), frames,
                 static_cast<size_t>(options.structVersion), uir,
                 options.uiBufferFormat, options.hudLessBufferFormat);
        }
    }

    if (enabled != g_stage.game_enabled || frames != g_stage.game_frames) {
        logf("sl: game requested DLSS-G mode=%u framesToGenerate=%u (was %d/%u)\n",
             static_cast<unsigned>(options.mode), frames,
             g_stage.game_enabled ? 1 : 0, g_stage.game_frames);
        g_stage.game_enabled = enabled;
        g_stage.game_frames  = frames;
        if (!enabled) { xefg_capture_disable(); }
    }
    return sl::Result::eOk;
}

sl::Result dummy_get_state(const sl::ViewportHandle&, sl::DLSSGState& state,
                           const sl::DLSSGOptions*)
{
    const unsigned gen =
        (cfg().fg_output == static_cast<int>(FgOutputSink::XeFG))
            ? static_cast<unsigned>(cfg().interpolated) : 0u;
    state.numFramesActuallyPresented = gen + 1;
    state.status = sl::DLSSGStatus::eOk;
    state.estimatedVRAMUsageInBytes = 300ull * 1024ull * 1024ull;
    state.minWidthOrHeight = 128;
    if (state.structVersion >= sl::kStructVersion2) {
        state.numFramesToGenerateMax   = 1;                  // 2x
        state.bIsVsyncSupportAvailable = sl::Boolean::eTrue;
        state.bReserved4               = sl::Boolean::eFalse;
    }
    return sl::Result::eOk;
}

sl::Result hk_set_options(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    // OptiScaler's Streamline_Hooks.cpp: only forward what the provider can generate.
    // On the dual-GPU path the game must stay at 1X/2X so its real-frame rate remains
    // the consumer's input rate; a 4X game-side multiplier would reduce Evaluate calls
    // to 15/s and make a 30x4 consumer output 60 fps.
    // PBZ also calls the exported slDLSSGSetOptions directly, not only the pointer
    // returned by slGetFeatureFunction. With FakeSlDlssg=1 that entry must be a no-op as
    // well, otherwise the real sl.dlss_g plugin starts its own frame generation even
    // though the game-facing function pointer is a dummy.
    if (cfg().fake_sl_player != 0 ||
        cfg().fg_input != static_cast<int>(FgInputSource::NVNGX)) {
        return dummy_set_options(viewport, options);
    }
    sl::DLSSGOptions local = options;
    if (local.mode == sl::DLSSGMode::eOff) { xefg_capture_disable(); }
    if (local.numFramesToGenerate > 1) {
        static int told = 0;
        if (told < 3) {
            ++told;
            logf("sl: DLSSG numFramesToGenerate %u -> 1 for the dual-GPU input lane\n",
                 local.numFramesToGenerate);
        }
        local.numFramesToGenerate = 1;
    }
    return (o_set_options != nullptr) ? o_set_options(viewport, local) : sl::Result::eOk;
}

sl::Result hk_get_state(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                        const sl::DLSSGOptions* options)
{
    if (cfg().fake_sl_player != 0 ||
        cfg().fg_input != static_cast<int>(FgInputSource::NVNGX)) {
        return dummy_get_state(viewport, state, options);
    }
    if (o_get_state == nullptr) { return sl::Result::eErrorFeatureMissing; }
    const sl::Result r = o_get_state(viewport, state, options);
    if (r == sl::Result::eOk && cfg().fake_sl_player == 0 &&
        cfg().fg_input == static_cast<int>(FgInputSource::NVNGX) &&
        state.structVersion >= sl::kStructVersion2) {
        state.numFramesToGenerateMax = 1;      // 2x, the input rate stays native
    }
    return r;
}

sl::Result hk_pcl_marker(sl::PCLMarker marker, const sl::FrameToken& frame)
{
    if (cfg().fg_input == static_cast<int>(FgInputSource::DLSSG) &&
        cfg().fg_output == static_cast<int>(FgOutputSink::Capture)) {
        if (!g_stage.game_enabled) { xefg_capture_disable(); }
        // OptiScaler's hkslPCLSetMarker: ePresentStart closes the staged frame. PBZ
        // resolves this through slGetFeatureFunction(feature=PCL).
        if (marker == sl::PCLMarker::ePresentStart) {
            g_stage.frame_token = static_cast<uint32_t>(frame);
            // A record is already queued for this presentation (Evaluate, another lane,
            // or an earlier PCL call). Publishing the staged copy as well allocates a
            // second seq; the producer only keeps the first record, while the marker
            // uses the second -> guaranteed no_slot. Only titles that stage tags WITHOUT
            // any earlier record (PBZ) need this fallback.
            if (fg_record_pending()) {
                ++g_sl_staged_skipped;
                static unsigned told = 0;
                if (told++ < 3) {
                    logf("sl: present-staged skipped - a record is already pending for this"
                         " present (eval token=%u pcl token=%u)\n",
                         g_sl_eval_token, g_stage.frame_token);
                }
            } else {
                sl_publish_staged();
            }
        }
        static unsigned logged = 0;
        if (++logged <= 8) {
            logf("sl: PCL marker %u frame=%u\n", static_cast<unsigned>(marker),
                 static_cast<unsigned>(frame));
        }
    }
    return (o_pcl_marker != nullptr) ? o_pcl_marker(marker, frame) : sl::Result::eOk;
}

sl::Result hk_get_func(sl::Feature feature, const char* name, void*& function)
{
    if (feature == sl::kFeaturePCL && name != nullptr &&
        strcmp(name, "slPCLSetMarker") == 0) {
        void* real = nullptr;
        if (o_get_func != nullptr) { o_get_func(feature, name, real); }
        if (real != nullptr) {
            o_pcl_marker = reinterpret_cast<sl_pfn_pcl_marker>(real);
            function = reinterpret_cast<void*>(&hk_pcl_marker);
            logf("sl: slPCLSetMarker -> our PCL frame-boundary hook\n");
            return sl::Result::eOk;
        }
    }
    // Streamline_Hooks.cpp:437-457 answers these two UNCONDITIONALLY for kFeatureDLSS_G:
    //     slDLSSGSetOptions -> dummy_slDLSSGSetOptions
    //     slDLSSGGetState   -> dummy_slDLSSGGetState
    // so the real sl.dlss_g plugin is never told to start frame generation. Forwarding them
    // (the old default) lets the real plugin run its own FG pipeline - its own swap-chain
    // and present hooks - on top of ours, which is measured as a doubled frame rate with a
    // black window. `FakeSlDlssg=0` restores the forward, for debugging only.
    if (feature == sl::kFeatureDLSS_G && name != nullptr) {
        if (cfg().fake_sl_player != 0) {
            if (strcmp(name, "slDLSSGSetOptions") == 0) {
                function = reinterpret_cast<void*>(&dummy_set_options);
                return sl::Result::eOk;
            }
            if (strcmp(name, "slDLSSGGetState") == 0) {
                function = reinterpret_cast<void*>(&dummy_get_state);
                return sl::Result::eOk;
            }
            return sl::Result::eOk;
        }
        // OptiScaler: replace the function pointers returned to the title, not only an
        // exported symbol. PBZ resolves these through slGetFeatureFunction.
        if (strcmp(name, "slDLSSGSetOptions") == 0 ||
            strcmp(name, "slDLSSGGetState") == 0) {
            void* real = nullptr;
            if (o_get_func != nullptr) { o_get_func(feature, name, real); }
            if (real != nullptr) {
                if (strcmp(name, "slDLSSGSetOptions") == 0) {
                    o_set_options = reinterpret_cast<sl_pfn_set_options>(real);
                    function = reinterpret_cast<void*>(&hk_set_options);
                } else {
                    o_get_state = reinterpret_cast<sl_pfn_get_state>(real);
                    function = reinterpret_cast<void*>(&hk_get_state);
                }
                return sl::Result::eOk;
            }
        }
        static int asked = 0;
        if (asked < 8) {
            ++asked;
            logf("sl: slGetFeatureFunction(DLSS_G, \"%s\") -> forwarded to the real plugin\n", name);
        }
    } else if (feature != sl::kFeatureDLSS_G && name != nullptr) {
        // Which functions the title resolves for the OTHER features: with DLSS missing
        // from the menu this list is short (or empty), which is the proof that the game
        // never got past the gate rather than failing inside DLSS.
        static int asked_other = 0;
        if (asked_other < 10) {
            ++asked_other;
            logf("sl: slGetFeatureFunction(feature=%d, \"%s\")\n",
                 static_cast<int>(feature), name);
        }
    }

    return (o_get_func != nullptr) ? o_get_func(feature, name, function)
                                   : sl::Result::eErrorFeatureMissing;
}

// slInit: pass the game's preferences through unchanged by default.
//
// Whether the real `sl.dlss_g` plugin may load is the single most important decision
// here, and it is the opposite of what it first looks like:
//
//   * CP2077 never calls slEvaluateFeature(kFeatureDLSS_G). Its frame generation is
//     driven by the sl.dlss_g plugin, which calls NVSDK_NGX_D3D12_EvaluateFeature -
//     measured: our survey logs have `fg: dlssg.ngx#N`, and the first run with the
//     plugin stripped had 0 evaluate calls. Stripping it therefore leaves the game
//     "on" with nothing generating, which is exactly the native-picture symptom.
//   * OptiScaler strips it only when the FG *input or output* is itself DLSSG
//     (Streamline_Hooks.cpp:287). Their CP2077 profile is Upscaler input + XeFG
//     output, so the plugin loads and their nvngx provider catches the NGX calls.
//
// So: keep it, and be the NGX provider instead (StripDlssGPlugin=1 is the other
// arrangement, for titles that call slEvaluateFeature themselves).
sl::Result hk_init(const sl::Preferences& pref, uint64_t sdk_version)
{
    if (o_init == nullptr) { return sl::Result::eErrorNotInitialized; }

    // What the title actually asked Streamline to load. CP2077 hiding DLSS is a question
    // about this list, so it has to be in the log.
    // Streamline_Hooks.cpp:168-171 treats the engine type it reports as ground truth.
    if (pref.engine == sl::EngineType::eUnreal) {
        set_game_quirk(Quirk_ForceUnrealEngine);
        set_game_quirk(Quirk_NoFSRFGFirstSwapchain);
        logf("sl: engine is Unreal - ForceUnrealEngine + NoFSRFGFirstSwapchain quirks on\n");
    }

    static bool told = false;
    if (!told) {
        told = true;
        char list[160]{};
        for (uint32_t i = 0; i < pref.numFeaturesToLoad && i < 16; ++i) {
            char one[16]{};
            sprintf_s(one, "%d,", static_cast<int>(pref.featuresToLoad[i]));
            strcat_s(list, one);
        }
        logf("sl: slInit featuresToLoad=[%s] strip_dlssg=%d\n", list, cfg().strip_dlssg);
    }

    if (cfg().strip_dlssg == 0) {
        logf("sl: slInit (%u features, DLSS-G plugin allowed - it is the caller of our"
             " NGX provider)\n", pref.numFeaturesToLoad);
        return o_init(pref, sdk_version);
    }

    sl::Preferences local = pref;
    sl::Feature features[16]{};
    uint32_t count = 0;
    for (uint32_t i = 0; i < pref.numFeaturesToLoad && i < 16; ++i) {
        if (pref.featuresToLoad[i] == sl::kFeatureDLSS_G) { continue; }
        features[count++] = pref.featuresToLoad[i];
    }
    local.featuresToLoad    = features;
    local.numFeaturesToLoad = count;
    logf("sl: slInit - StripDlssGPlugin=1, kFeatureDLSS_G removed (%u -> %u)\n",
         pref.numFeaturesToLoad, count);
    return o_init(local, sdk_version);
}

// ---------------------------------------------------------------- input side
sl::Result hk_set_tag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
                      uint32_t count, sl::CommandBuffer* cmd)
{
    record_tags(tags, count, cmd);
    return (o_set_tag != nullptr) ? o_set_tag(viewport, tags, count, cmd) : sl::Result::eOk;
}

sl::Result hk_set_tag_frame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport,
                            const sl::ResourceTag* tags, uint32_t count, sl::CommandBuffer* cmd)
{
    record_tags(tags, count, cmd);
    return (o_set_tag_frame != nullptr) ? o_set_tag_frame(frame, viewport, tags, count, cmd)
                                        : sl::Result::eOk;
}

sl::Result hk_set_constants(const sl::Constants& values, const sl::FrameToken& frame,
                            const sl::ViewportHandle& viewport)
{
    static_assert(sizeof(sl::Constants) <= sizeof(g_stage.constants_raw),
                  "staging buffer too small for sl::Constants");

    // Sl_Inputs::CaptureConstants (Streamline_Inputs.cpp:63-82): a v1 (or version-0)
    // struct is promoted into a v2 record. Spider-Man Remastered sends an invalid version;
    // copying the v1 prefix keeps the v2 tail (minRelativeLinearDepth...) from being read
    // out of a smaller struct.
    sl::Constants c{};
    const size_t v1_size = sizeof(sl::Constants) - sizeof(float);   // - minRelative...
    if (values.structVersion == c.structVersion) {
        c = values;
    } else if ((c.structVersion == sl::kStructVersion2 && values.structVersion == sl::kStructVersion1) ||
               values.structVersion == 0) {
        sl::BaseStructure* next = c.next;
        memcpy(&c, &values, v1_size);
        c.structVersion = sl::kStructVersion2;
        c.next = next;
    } else {
        // A newer version: our members are a prefix of theirs, so our size is safe to read.
        memcpy(&c, &values, sizeof(sl::Constants));
    }

    memcpy(g_stage.constants_raw, &c, sizeof(c));
    g_stage.have_constants = true;
    return (o_set_constants != nullptr) ? o_set_constants(values, frame, viewport)
                                        : sl::Result::eOk;
}

// ---------------------------------------------------------------- Evaluate
// The frame's inputs are staged (tags + constants). Build the record, hand it to
// the capture, tag XeFG. Success means "generated by us": the real plugin is not
// loaded, so there is nothing to forward to.
sl::Result hk_evaluate(sl::Feature feature, const sl::FrameToken& frame,
                       const sl::BaseStructure** inputs, uint32_t numInputs,
                       sl::CommandBuffer* cmd)
{
    if (feature != sl::kFeatureDLSS_G || !provider_owns_output()) {
        if (feature == sl::kFeatureDLSS_G) {
            static unsigned forwarded = 0;
            if (++forwarded <= 3) {
                logf("sl: evaluate(DLSS_G) #%u forwarded (owns_output=%d)\n",
                     forwarded, provider_owns_output() ? 1 : 0);
            }
        }
        return (o_evaluate != nullptr) ? o_evaluate(feature, frame, inputs, numInputs, cmd)
                                       : sl::Result::eErrorFeatureMissing;
    }

    MgpuFgFrame f      = make_empty_record();
    f.cmd_list         = cmd;
    f.mode             = api_mode();
    f.frame_qpc_ms     = now_qpc_ms();
    f.declared_frames  = g_stage.game_frames;

    sl_stage_inputs(f);

    // A title that does not tag motion vectors still has depth (and a back buffer).
    // Render size must therefore fall back to depth: with render_w/h == 0 the producer's
    // update_content() is a no-op, the stream rectangles stay 0 and no publication ever
    // happens - the consumer then sees 100% no_slot even though the marker is fine.
    f.render_w = f.stream[MGPU_FG_STREAM_MV].width;
    f.render_h = f.stream[MGPU_FG_STREAM_MV].height;
    if (f.render_w < 8 || f.render_h < 8) {
        f.render_w = f.stream[MGPU_FG_STREAM_DEPTH].width;
        f.render_h = f.stream[MGPU_FG_STREAM_DEPTH].height;
    }
    f.output_w = f.stream[MGPU_FG_STREAM_HUDLESS].width;
    f.output_h = f.stream[MGPU_FG_STREAM_HUDLESS].height;
    if (f.output_w < 8 && g_stage.backbuffer.resource != nullptr) {
        f.output_w = f.stream[MGPU_FG_STREAM_BACKBUFFER].width;
        f.output_h = f.stream[MGPU_FG_STREAM_BACKBUFFER].height;
    }
    if (f.render_w < 8 || f.render_h < 8) {
        // Loading/menu: nothing to publish. Still answer eOk - the title asked us for a
        // frame and there is no error, there just are no inputs yet.
        return sl::Result::eOk;
    }
    static bool told_depth_only_eval = false;
    if (!told_depth_only_eval && f.stream[MGPU_FG_STREAM_MV].resource == nullptr) {
        told_depth_only_eval = true;
        logf("sl: evaluate record is depth-only (no MV tagged) render=%ux%u depth=%ux%u\n",
             f.render_w, f.render_h,
             f.stream[MGPU_FG_STREAM_DEPTH].width,
             f.stream[MGPU_FG_STREAM_DEPTH].height);
    }

    if (f.stream[MGPU_FG_STREAM_MV].resource != nullptr ||
        f.stream[MGPU_FG_STREAM_DEPTH].resource != nullptr) {
        // no external pace: let the title's timeline drive the capture
    }

    uint32_t flags = MGPU_FG_FRAME_FROM_FG;
    if (f.stream[MGPU_FG_STREAM_MV].resource != nullptr) { flags |= MGPU_FG_FRAME_MV_ACTIVE; }

    if (g_stage.have_constants) {
        sl::Constants c{};
        memcpy(&c, g_stage.constants_raw, sizeof(c));
        memcpy(f.view_to_clip, &c.cameraViewToClip, sizeof(f.view_to_clip));
        memcpy(f.clip_to_prev_clip, &c.clipToPrevClip, sizeof(f.clip_to_prev_clip));
        memcpy(f.prev_clip_to_clip, &c.prevClipToClip, sizeof(f.prev_clip_to_clip));
        f.jitter_x     = c.jitterOffset.x;
        f.jitter_y     = c.jitterOffset.y;
        f.mv_scale_x   = c.mvecScale.x;
        f.mv_scale_y   = c.mvecScale.y;
        // sl::Constants::mvecScale normalizes the MV texels (sl_consts.h:197-198). The
        // texel -> pixel factor XeFG multiplies by is mvecScale * (MV width, MV height) -
        // exactly what Sl_Inputs::CaptureConstants does (Streamline_Inputs.cpp:231-238:
        // "UE games and Dead Rising expect that multiplication to be done, even if the
        // scale is 1.0"). MvScaleMultiplyResolution=0 is the escape for a title whose
        // mvecScale already is in pixels.
        if (cfg().mv_scale_resolution != 0) {
            const float mw = static_cast<float>(f.stream[MGPU_FG_STREAM_MV].width);
            const float mh = static_cast<float>(f.stream[MGPU_FG_STREAM_MV].height);
            if (mw > 1.0f && mh > 1.0f) {
                f.mv_scale_x *= mw;
                f.mv_scale_y *= mh;
            }
        }
        // Camera: keep the title's values when they are plausible; otherwise recover
        // near/far/fov from cameraViewToClip (OptiScaler's loadCameraMatrix). A far plane
        // of zero means "infinite"; CameraFar must stay > CameraNear for the consumers.
        float cam_near = c.cameraNear;
        float cam_far  = c.cameraFar;
        float cam_fov  = c.cameraFOV;
        const bool camera_ok = (cam_near > 0.0f) && (cam_far > cam_near) &&
                               (cam_fov > 0.05f) && (cam_fov < 2.5f);
        if (!camera_ok) {
            float dn = 0.0f, df = 0.0f, dv = 0.0f;
            if (projection_camera(c, dn, df, dv)) {
                cam_near = dn;
                cam_far  = df;
                cam_fov  = dv;
            }
        }
        if (cam_near > 0.0f && cam_far == 0.0f) { cam_far = cam_near + 1.0f; }
        f.camera_near  = cam_near;
        f.camera_far   = cam_far;
        f.camera_fov_y = cam_fov;
        f.aspect       = c.cameraAspectRatio;
        flags |= MGPU_FG_FRAME_HAS_MATRIX | MGPU_FG_FRAME_HAS_PREV_CLIP;
        if (cam_near > 0.0f && cam_far > cam_near) {
            flags |= MGPU_FG_FRAME_HAS_CAMERA;
        }
        if (c.depthInverted == sl::Boolean::eTrue)        { flags |= MGPU_FG_FRAME_DEPTH_INVERTED; }
        if (c.motionVectorsJittered == sl::Boolean::eTrue) { flags |= MGPU_FG_FRAME_MV_JITTERED; }
        if (c.reset == sl::Boolean::eTrue)                { flags |= MGPU_FG_FRAME_RESET_HISTORY; }
        if (c.motionVectors3D == sl::Boolean::eTrue)      { flags |= MGPU_FG_FRAME_MV_3D; }
        if (c.motionVectorsDilated == sl::Boolean::eTrue) { flags |= MGPU_FG_FRAME_MV_DILATED; }
        if (c.cameraMotionIncluded == sl::Boolean::eTrue) { flags |= MGPU_FG_FRAME_CAMERA_MOTION; }
    }
    f.flags = flags;

    // One id per Streamline frame: a title that evaluates DLSS-G twice for the same token
    // (multi-view / settings) must not make the id sequence skip, because a skipped id
    // makes XeFG answer SetPresentId OK and never generate. The second evaluate re-tags
    // the same frame id (resources only get replaced).
    static uint32_t s_eval_id_token = 0;
    static uint32_t s_eval_id = 0;
    const uint32_t eval_token = static_cast<uint32_t>(frame);
    uint32_t present_id = 0;
    if (eval_token != 0 && eval_token == s_eval_id_token && s_eval_id != 0) {
        present_id = s_eval_id;
    } else {
        present_id = xefg_next_frame_id();
        s_eval_id_token = eval_token;
        s_eval_id = present_id;
    }
    f.present_id = present_id;

    // `cmd` came in with slEvaluateFeature, so it is the title's still-open list: the
    // transport may append its own passes to it (see MGPU_FG_FRAME_CMD_OPEN).
    f.flags |= MGPU_FG_FRAME_CMD_OPEN;

    g_sl_eval_token = static_cast<uint32_t>(frame);
    sl_publish_record(f, g_sl_eval_token, "evaluate");
    // FGInput=DLSSG only: this lane is Sl_Inputs, so it must not tag anything when the
    // configured input lane is the upscaler (OptiScaler has exactly one active lane).
    if (cfg().fg_output == static_cast<int>(FgOutputSink::XeFG) &&
        cfg().fg_input == static_cast<int>(FgInputSource::DLSSG)) {
        fg_output_submit(f);
    }
    // Same gate as provider.cpp: what the library accepted for THIS frame.
    xefg_frame_feedback((cfg().fg_input == static_cast<int>(FgInputSource::DLSSG) &&
                         xefg_wants_generation())
                            ? xefg_tags_complete() : false);

    static unsigned logged = 0;
    ++logged;
    if (logged <= 3 || (logged % 600) == 0) {
        logf("sl: evaluate #%u id=%u render=%ux%u out=%ux%u mv=%p depth=%p hudless=%p ui=%p"
             " game=%d/%u\n", logged, present_id, f.render_w, f.render_h, f.output_w,
             f.output_h, f.stream[MGPU_FG_STREAM_MV].resource,
             f.stream[MGPU_FG_STREAM_DEPTH].resource,
             f.stream[MGPU_FG_STREAM_HUDLESS].resource,
             f.stream[MGPU_FG_STREAM_UI_COLOR].resource,
             g_stage.game_enabled ? 1 : 0, g_stage.game_frames);
        if (logged == 3) { xefg_log_status(); }
    }

    return sl::Result::eOk;
}

} // namespace
} // namespace fakerfg
namespace fakerfg {

// Installs the Streamline layer. Called from the loader thread and retried from the
// early entry points, because sl.interposer arrives with the game's own streaming
// stack - often after us.
void sl_install_if_possible()
{
    if (g_sl_installed || g_sl_tried) { return; }

    HMODULE sl = GetModuleHandleW(L"sl.interposer.dll");
    if (sl == nullptr) {
        static bool told = false;
        if (!told) { told = true; logf("sl: sl.interposer not loaded yet - will retry\n"); }
        return;
    }
    g_sl_tried = true;

    struct Entry { const char* name; void* hook; void** real; };
    const Entry entries[] = {
        { "slIsFeatureSupported",    (void*)&hk_supported,      (void**)&o_supported },
        { "slIsFeatureLoaded",       (void*)&hk_loaded,         (void**)&o_loaded },
        { "slGetFeatureRequirements",(void*)&hk_requirements,   (void**)&o_requirements },
        { "slGetFeatureVersion",     (void*)&hk_version,        (void**)&o_version },
        { "slGetFeatureFunction",    (void*)&hk_get_func,       (void**)&o_get_func },
        { "slSetFeatureLoaded",      (void*)&hk_set_loaded,     (void**)&o_set_loaded },
        { "slInit",                  (void*)&hk_init,           (void**)&o_init },
        { "slSetTag",                (void*)&hk_set_tag,        (void**)&o_set_tag },
        { "slSetTagForFrame",        (void*)&hk_set_tag_frame,  (void**)&o_set_tag_frame },
        { "slSetConstants",          (void*)&hk_set_constants,  (void**)&o_set_constants },
        { "slEvaluateFeature",       (void*)&hk_evaluate,       (void**)&o_evaluate },
        { "slDLSSGSetOptions",        (void*)&hk_set_options,    (void**)&o_set_options },
        { "slDLSSGGetState",          (void*)&hk_get_state,      (void**)&o_get_state },
    };

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        logf("sl: MinHook init failed\n");
        return;
    }

    int hooked = 0;
    for (const Entry& e : entries) {
        LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(sl, e.name));
        if (addr == nullptr) { continue; }
        if (MH_CreateHook(addr, e.hook, e.real) != MH_OK) {
            logf("sl: could not hook %s\n", e.name);
            continue;
        }
        ++hooked;
    }
    // One suspension for all eleven (OptiScaler: a single Detour transaction). Eleven
    // separate MH_EnableHook calls freeze every thread eleven times, which is visible as a
    // stutter during startup.
    if (hooked > 0 && MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        logf("sl: MH_EnableHook(all) failed\n");
        hooked = 0;
    }

    g_interposer = sl;
    g_sl_installed  = hooked > 0;
    logf("sl: %d/%d Streamline exports hooked (sl.interposer=%p) - DLSS-G is answered by us\n",
         hooked, static_cast<int>(sizeof(entries) / sizeof(entries[0])), (void*)sl);
}

bool sl_installed() { return g_sl_installed; }

// OptiScaler installs its Streamline hooks inside LibraryLoad_Hooks, i.e. the moment
// sl.interposer.dll finishes loading. We cannot call MH_EnableHook from there (that would
// suspend threads under the loader lock), so the module-load hook only signals and the
// loader thread installs immediately. That removes the GetModuleHandleW polling that was
// contending with the game's own LoadLibrary calls during startup.
HANDLE sl_loaded_event()
{
    static HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return h;
}

void sl_note_interposer_loaded()
{
    if (HANDLE h = sl_loaded_event()) { SetEvent(h); }
}

void sl_wait_for_interposer(unsigned ms)
{
    if (HANDLE h = sl_loaded_event()) { WaitForSingleObject(h, ms); }
}

// OptiScaler's shape (proxies/Streamline_Proxy.h:88-105): its own copies of the Streamline
// DLLs live in a fixed `streamline\` folder next to OptiScaler.dll and are loaded by FULL
// PATH before the game asks for them. The Windows loader keys modules by base name, so the
// game's later LoadLibrary("sl.interposer.dll") - wherever the game keeps it - resolves to
// this copy. That is what makes OptiScaler independent of each title's layout.
//
// Returns true when our own interposer is now loaded and hooked.
bool sl_preload_own_copy()
{
    wchar_t base[MAX_PATH]{};
    GetModuleFileNameW(nullptr, base, MAX_PATH);
    if (wchar_t* slash = wcsrchr(base, L'\\')) { *slash = L'\0'; }

    wchar_t path[MAX_PATH]{};
    wcscpy_s(path, base);
    wcscat_s(path, L"\\fakerFG\\streamline\\sl.interposer.dll");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) { return false; }

    HMODULE m = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    logf("sl: own interposer %ls -> %p\n", path, (void*)m);
    if (m == nullptr) { return false; }

    // nvngx_dlssg.dll from the same folder: Streamline asks for it by name, the loader
    // matches the base name to this copy (which is us, deployed as fakerFG.dll).
    wchar_t dlssg[MAX_PATH]{};
    wcscpy_s(dlssg, base);
    wcscat_s(dlssg, L"\\fakerFG\\streamline\\nvngx_dlssg.dll");
    if (GetFileAttributesW(dlssg) != INVALID_FILE_ATTRIBUTES) {
        HMODULE d = LoadLibraryExW(dlssg, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        logf("sl: own nvngx_dlssg %ls -> %p\n", dlssg, (void*)d);
    }
    return true;
}

// OptiScaler's Sl_Inputs::reportResource, as a source function: the resources the title
// tagged through Streamline, each with the state IT declared (`sl::Resource::state`) and
// the lifecycle it chose (eValidUntilPresent -> UntilPresent, anything else -> ValidNow).
// This is the lane's authoritative input description - we never invent a state and never
// touch the resource.
int sl_stage_inputs(MgpuFgFrame& f)
{
    int provided = 0;

    auto put = [&f, &provided](MgpuFgStream stream, const StageEntry& e) {
        if (e.resource == nullptr) { return; }
        const D3D12_RESOURCE_DESC d = e.resource->GetDesc();
        f.stream[stream].resource = e.resource;
        f.stream[stream].state    = e.state;                 // verbatim, may be UINT_MAX
        f.stream[stream].validity = e.until_present ? MGPU_FG_RV_UNTIL_PRESENT
                                                    : MGPU_FG_RV_ONLY_NOW;
        // The tag's declared region wins over the resource description (OptiScaler:
        // `width = tag.extent ? tag.extent.width : desc.Width`, Streamline_Inputs.cpp:389).
        f.stream[stream].width    = e.ext_w ? e.ext_w : static_cast<uint32_t>(d.Width);
        f.stream[stream].height   = e.ext_h ? e.ext_h : d.Height;
        f.stream[stream].format   = static_cast<uint32_t>(d.Format);
        ++provided;
    };

    // The camera frame of this frame's sl::Constants (Sl_Inputs::CaptureConstants): the
    // view matrix is built from it in the present path.
    if (g_stage.have_constants) {
        sl::Constants c{};
        memcpy(&c, g_stage.constants_raw, sizeof(c));
        const float right[3]   = { c.cameraRight.x, c.cameraRight.y, c.cameraRight.z };
        const float up[3]      = { c.cameraUp.x, c.cameraUp.y, c.cameraUp.z };
        const float forward[3] = { c.cameraFwd.x, c.cameraFwd.y, c.cameraFwd.z };
        const float position[3] = { c.cameraPos.x, c.cameraPos.y, c.cameraPos.z };
        xefg_camera(right, up, forward, position);
    }

    put(MGPU_FG_STREAM_MV, g_stage.mv);
    put(MGPU_FG_STREAM_DEPTH, g_stage.depth);
    // HUDless source order. A HUDLESS tag that was a stable snapshot (onlyValidNow ->
    // copied at tag time, `until_present`) is the real scene-without-UI. SCALING_OUTPUT
    // is only a fallback: on CP2077 with the game's own FG off it can be an empty target
    // even though the tag exists (measured: the transported stream 3 was all black and
    // the unblend mask became all-ones). Prefer the snapshot when we have one.
    static int logged_hudless_src = 0;
    const bool scaling_ok = g_stage.scaling.resource != nullptr;
    const bool snapshot_ok = g_stage.hudless.resource != nullptr &&
                             g_stage.hudless.until_present;
    // v34: the support mask must be computed against the scene-referred HUDLess (the
    // same space as DLSSG.Backbuffer). The patch colour, however, can come from the
    // display-referred ScalingOutputColor, which matches the WGC present. Keep the two
    // roles separate: HUDLESS = mask/fallback, SCALING = optional display patch source.
    const bool want_scaling =
        (cfg().hudless_source == 1) ||
        (cfg().hudless_source == 0 && has_game_quirk(Quirk_CyberpunkScalingHudless));
    const bool use_scaling = scaling_ok && (want_scaling || !snapshot_ok);
    if (snapshot_ok) {
        put(MGPU_FG_STREAM_HUDLESS, g_stage.hudless);
    } else if (scaling_ok) {
        // No stable HUDLess: the scaling image is the only colour we have. It is also
        // display-referred, so the mask will be full-frame and the patch path will be
        // expensive, but it is better than publishing nothing.
        put(MGPU_FG_STREAM_HUDLESS, g_stage.scaling);
    } else {
        put(MGPU_FG_STREAM_HUDLESS, g_stage.hudless);
    }
    if (use_scaling) {
        put(MGPU_FG_STREAM_SCALING, g_stage.scaling);
        f.stream[MGPU_FG_STREAM_SCALING]._pad = 1;   // display-referred patch source
        if (logged_hudless_src != 2) {
            logged_hudless_src = 2;
            logf("sl: HUDless patch = SCALING_OUTPUT (display-referred);"
                 " mask = %s\n", snapshot_ok ? "HUDLess snapshot" : "SCALING_OUTPUT");
        }
    } else if (snapshot_ok) {
        if (logged_hudless_src != 1) {
            logged_hudless_src = 1;
            logf("sl: HUDless source = HUDLESS snapshot (stable copy)\n");
        }
    } else if (scaling_ok) {
        if (logged_hudless_src != 2) {
            logged_hudless_src = 2;
            logf("sl: HUDless source = SCALING_OUTPUT (no stable snapshot)\n");
        }
    }
    put(MGPU_FG_STREAM_UI_COLOR, g_stage.ui);
    // P0: carry the alpha-only plane in the record (the producer ignores it today).
    put(MGPU_FG_STREAM_UI_ALPHA, g_stage.ui_alpha);
    if (g_stage.ui.resource == nullptr && g_stage.ui_alpha.resource != nullptr) {
        static bool told_alpha_only = false;
        if (!told_alpha_only) {
            told_alpha_only = true;
            logf("sl: UI is alpha-only (tag %u) - no UIColorAndAlpha; recorded for P4\n",
                 g_stage.ui_alpha_type);
        }
    }

    // The backbuffer is implicit for XeFG (proxy swapchain), but its declared state is
    // part of the record now: the P1 synth pass has to read it on the title's list.
    put(MGPU_FG_STREAM_BACKBUFFER, g_stage.backbuffer);
    return provided;
}

// Some titles (PBZ) answer slDLSSGSetOptions but stage resources without ever calling
// slEvaluateFeature(DLSS_G). OptiScaler's Sl_Inputs is Evaluate-driven; this present-side
// fallback publishes exactly the resources the title tagged most recently, so the
// capture-only lane still has a complete record.
void sl_publish_staged()
{
    if (cfg().fg_input != static_cast<int>(FgInputSource::DLSSG) ||
        cfg().fg_output != static_cast<int>(FgOutputSink::Capture)) {
        return;
    }
    if (!g_stage.game_enabled) { xefg_capture_disable(); return; }   // DLSSFG off: release the gate

    MgpuFgFrame f = make_empty_record();
    f.cmd_list     = g_stage.cmd;
    f.mode         = api_mode();
    f.frame_qpc_ms = now_qpc_ms();
    f.declared_frames = g_stage.game_frames;

    if (sl_stage_inputs(f) == 0) { return; }

    f.render_w = f.stream[MGPU_FG_STREAM_MV].width;
    f.render_h = f.stream[MGPU_FG_STREAM_MV].height;
    if (f.render_w == 0) { f.render_w = f.stream[MGPU_FG_STREAM_DEPTH].width; }
    if (f.render_h == 0) { f.render_h = f.stream[MGPU_FG_STREAM_DEPTH].height; }
    f.output_w = f.stream[MGPU_FG_STREAM_HUDLESS].width;
    f.output_h = f.stream[MGPU_FG_STREAM_HUDLESS].height;
    if (f.output_w < 8) {
        f.output_w = f.stream[MGPU_FG_STREAM_BACKBUFFER].width;
        f.output_h = f.stream[MGPU_FG_STREAM_BACKBUFFER].height;
    }
    if (f.render_w < 8 || f.render_h < 8) { return; }
    // Motion vectors are OPTIONAL here: a depth-only title (Wuchang/Unreal with the
    // upscaler lane, or any title that never tags sl::kFeatureDLSS_G velocity) still
    // has to produce a publication - the marker carries the identity and the consumer
    // generates its own vectors with optical flow. Only "no depth AND no MV" is a
    // loading/menu frame.
    if (f.stream[MGPU_FG_STREAM_MV].resource == nullptr &&
        f.stream[MGPU_FG_STREAM_DEPTH].resource == nullptr) {
        return;
    }
    static bool told_depth_only = false;
    if (!told_depth_only && f.stream[MGPU_FG_STREAM_MV].resource == nullptr) {
        told_depth_only = true;
        logf("sl: depth-only publication (no MV tagged) render=%ux%u depth=%ux%u"
             " - the consumer will use optical flow\n",
             f.render_w, f.render_h,
             f.stream[MGPU_FG_STREAM_DEPTH].width,
             f.stream[MGPU_FG_STREAM_DEPTH].height);
    }


    uint32_t flags = MGPU_FG_FRAME_FROM_FG;
    if (f.stream[MGPU_FG_STREAM_MV].resource != nullptr) { flags |= MGPU_FG_FRAME_MV_ACTIVE; }
    if (g_stage.have_constants) {
        sl::Constants c{};
        memcpy(&c, g_stage.constants_raw, sizeof(c));
        memcpy(f.view_to_clip, &c.cameraViewToClip, sizeof(f.view_to_clip));
        memcpy(f.clip_to_prev_clip, &c.clipToPrevClip, sizeof(f.clip_to_prev_clip));
        memcpy(f.prev_clip_to_clip, &c.prevClipToClip, sizeof(f.prev_clip_to_clip));
        f.jitter_x = c.jitterOffset.x;
        f.jitter_y = c.jitterOffset.y;
        f.mv_scale_x = c.mvecScale.x;
        f.mv_scale_y = c.mvecScale.y;
        f.camera_near = c.cameraNear;
        f.camera_far = c.cameraFar;
        f.camera_fov_y = c.cameraFOV;
        f.aspect = c.cameraAspectRatio;
        flags |= MGPU_FG_FRAME_HAS_MATRIX | MGPU_FG_FRAME_HAS_PREV_CLIP;
        if (c.cameraNear > 0.0f && c.cameraFar > c.cameraNear) {
            flags |= MGPU_FG_FRAME_HAS_CAMERA;
        }
        if (c.depthInverted == sl::Boolean::eTrue)        { flags |= MGPU_FG_FRAME_DEPTH_INVERTED; }
        if (c.motionVectorsJittered == sl::Boolean::eTrue) { flags |= MGPU_FG_FRAME_MV_JITTERED; }
        if (c.reset == sl::Boolean::eTrue)                { flags |= MGPU_FG_FRAME_RESET_HISTORY; }
    }
    f.flags = flags;
    f.present_id = xefg_next_frame_id();

    // NO MGPU_FG_FRAME_CMD_OPEN here: this lane runs from slPCLSetMarker(ePresentStart),
    // i.e. after the title closed and submitted `g_stage.cmd`. Recording a Dispatch on a
    // closed list is undefined behaviour - the commands are dropped (the codec's TileLen
    // read back 0.00 MB/frame for whole PBZ sessions) and the NV UMD intermittently
    // faults (PBZ 2026-11-22, 0xC0000005 in nvwgf2umx.dll inside
    // fr_encode_dispatch's SetComputeRootDescriptorTable). The producer now records its
    // own passes on its DIRECT queue list, which is submitted after the frame fence.

    // The producer's transport needs the D3D12 device; the staged tags do not carry it.
    ID3D12Resource* res = static_cast<ID3D12Resource*>(
        f.stream[MGPU_FG_STREAM_MV].resource != nullptr
            ? f.stream[MGPU_FG_STREAM_MV].resource
            : f.stream[MGPU_FG_STREAM_DEPTH].resource);
    ID3D12Device* dev = nullptr;
    if (res != nullptr) { res->GetDevice(IID_PPV_ARGS(&dev)); }
    f.device = dev;
    g_sl_staged_token = g_stage.frame_token;
    sl_publish_record(f, g_sl_staged_token, "present-staged");
    if (dev != nullptr) { dev->Release(); }

    static unsigned logged = 0;
    if (++logged <= 3 || (logged % 600) == 0) {
        logf("sl: present fallback publish #%u render=%ux%u mv=%p depth=%p"
             " hudless=%p ui=%p hudless_size=%ux%u fmt=%u ui_size=%ux%u fmt=%u\n",
             logged, f.render_w, f.render_h, f.stream[MGPU_FG_STREAM_MV].resource,
             f.stream[MGPU_FG_STREAM_DEPTH].resource,
             f.stream[MGPU_FG_STREAM_HUDLESS].resource,
             f.stream[MGPU_FG_STREAM_UI_COLOR].resource,
             f.stream[MGPU_FG_STREAM_HUDLESS].width,
             f.stream[MGPU_FG_STREAM_HUDLESS].height,
             f.stream[MGPU_FG_STREAM_HUDLESS].format,
             f.stream[MGPU_FG_STREAM_UI_COLOR].width,
             f.stream[MGPU_FG_STREAM_UI_COLOR].height,
             f.stream[MGPU_FG_STREAM_UI_COLOR].format);
    }
}

}

