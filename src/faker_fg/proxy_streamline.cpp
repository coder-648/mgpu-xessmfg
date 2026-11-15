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

#include <MinHook.h>

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
};

struct Stage
{
    StageEntry hudless{};
    StageEntry mv{};
    StageEntry depth{};
    StageEntry ui{};
    ID3D12Resource* backbuffer = nullptr;
    void*           cmd        = nullptr;       // the frame's command buffer (from the tag call)

    unsigned char constants_raw[512]{};   // sl::Constants copy (opaque)
    bool          have_constants = false;

    bool          game_enabled = false;
    uint32_t      game_frames  = 1;
};

Stage g_stage{};

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

bool    g_sl_installed  = false;
bool    g_sl_tried      = false;
HMODULE g_interposer = nullptr;

// ---------------------------------------------------------------- helpers
void remember(StageEntry& slot, const sl::ResourceTag& tag, void* cmd)
{
    if (tag.resource == nullptr || tag.resource->native == nullptr) { return; }
    slot.resource      = reinterpret_cast<ID3D12Resource*>(tag.resource->native);
    slot.state         = tag.resource->state;                       // the title's declaration
    slot.until_present = (tag.lifecycle == sl::eValidUntilPresent);
    if (cmd != nullptr) { g_stage.cmd = cmd; }
}

void record_tags(const sl::ResourceTag* tags, uint32_t count, void* cmd)
{
    if (tags == nullptr) { return; }
    for (uint32_t i = 0; i < count; ++i) {
        const sl::ResourceTag& t = tags[i];
        switch (t.type) {
        case sl::kBufferTypeHUDLessColor:    remember(g_stage.hudless, t, cmd); break;
        case sl::kBufferTypeMotionVectors:   remember(g_stage.mv, t, cmd); break;
        case sl::kBufferTypeDepth:           remember(g_stage.depth, t, cmd); break;
        case sl::kBufferTypeUIColorAndAlpha: remember(g_stage.ui, t, cmd); break;
        case sl::kBufferTypeBackbuffer:
            if (t.resource != nullptr && t.resource->native != nullptr) {
                g_stage.backbuffer = reinterpret_cast<ID3D12Resource*>(t.resource->native);
            }
            break;
        default: break;
        }
    }
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
// decides nothing by itself - `Mode` does) and answer eOk so the menu keeps state.
sl::Result dummy_set_options(const sl::ViewportHandle&, const sl::DLSSGOptions& options)
{
    const bool enabled = options.mode != sl::DLSSGMode::eOff;
    uint32_t   frames  = options.numFramesToGenerate;
    if (frames < 1) { frames = 1; }

    if (enabled != g_stage.game_enabled || frames != g_stage.game_frames) {
        logf("sl: game requested DLSS-G mode=%u framesToGenerate=%u (was %d/%u)\n",
             static_cast<unsigned>(options.mode), frames,
             g_stage.game_enabled ? 1 : 0, g_stage.game_frames);
        g_stage.game_enabled = enabled;
        g_stage.game_frames  = frames;
    }
    return sl::Result::eOk;
}

sl::Result dummy_get_state(const sl::ViewportHandle&, sl::DLSSGState& state,
                           const sl::DLSSGOptions*)
{
    const unsigned gen = (cfg().mode == Mode::Local) ? static_cast<unsigned>(cfg().interpolated)
                                                     : 0u;
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

sl::Result hk_get_func(sl::Feature feature, const char* name, void*& function)
{
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
    memcpy(g_stage.constants_raw, &values, sizeof(sl::Constants));
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
    f.mode             = static_cast<uint32_t>(cfg().mode);
    f.frame_qpc_ms     = now_qpc_ms();
    f.declared_frames  = g_stage.game_frames;

    sl_stage_inputs(f);

    f.render_w = f.stream[MGPU_FG_STREAM_MV].width;
    f.render_h = f.stream[MGPU_FG_STREAM_MV].height;
    f.output_w = f.stream[MGPU_FG_STREAM_HUDLESS].width;
    f.output_h = f.stream[MGPU_FG_STREAM_HUDLESS].height;
    if (f.output_w < 8 && g_stage.backbuffer != nullptr) {
        f.output_w = f.stream[MGPU_FG_STREAM_BACKBUFFER].width;
        f.output_h = f.stream[MGPU_FG_STREAM_BACKBUFFER].height;
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
        f.camera_near  = c.cameraNear;
        f.camera_far   = c.cameraFar;
        f.camera_fov_y = c.cameraFOV;
        f.aspect       = c.cameraAspectRatio;
        flags |= MGPU_FG_FRAME_HAS_MATRIX | MGPU_FG_FRAME_HAS_PREV_CLIP;
        if (c.cameraNear > 0.0f && c.cameraFar > c.cameraNear) {
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

    const uint32_t present_id = xefg_next_frame_id();
    f.present_id = present_id;

    publish_record(f);
    // FGInput=DLSSG only: this lane is Sl_Inputs, so it must not tag anything when the
    // configured input lane is the upscaler (OptiScaler has exactly one active lane).
    if (cfg().fg_input == 2 && xefg_wants_generation()) {
        xefg_tag(reinterpret_cast<ID3D12GraphicsCommandList*>(cmd), present_id, f);
    }
    // Same gate as provider.cpp: what the library accepted for THIS frame.
    xefg_frame_feedback((cfg().fg_input == 2 && xefg_wants_generation())
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
        f.stream[stream].width    = static_cast<uint32_t>(d.Width);
        f.stream[stream].height   = d.Height;
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
    put(MGPU_FG_STREAM_HUDLESS, g_stage.hudless);
    put(MGPU_FG_STREAM_UI_COLOR, g_stage.ui);

    if (g_stage.backbuffer != nullptr) {
        const D3D12_RESOURCE_DESC d = g_stage.backbuffer->GetDesc();
        f.stream[MGPU_FG_STREAM_BACKBUFFER].resource = g_stage.backbuffer;
        f.stream[MGPU_FG_STREAM_BACKBUFFER].width    = static_cast<uint32_t>(d.Width);
        f.stream[MGPU_FG_STREAM_BACKBUFFER].height   = d.Height;
        f.stream[MGPU_FG_STREAM_BACKBUFFER].format   = static_cast<uint32_t>(d.Format);
        ++provided;
    }
    return provided;
}

} // namespace fakerfg

