// ============================================================================
// in_fg.cpp - the game's OWN frame generation, read-only, diagnosis only
//
// Why this exists: the DLSS-SR hook (in_ngx.cpp) cannot see a projection matrix at
// all, and on UE5 its motion vectors are sometimes unusable
// (docs/findings/UE5_MV_OPTISCALER.md 7.4). Frame generation is the opposite: every
// FG API *requires* the camera, so everything we have been guessing at (--fg-fov,
// --fg-zn, the MV unit, the MV sign, jittered-vs-clean) is declared there by the
// game itself, for the frame it is about to present.
//
// This file is NOT an input source. poll() always returns false: nothing is
// published to the bridge, no source gets pinned, no protocol field changes. It
// exists only to receive module-load notifications (mgpu::input::Source is the
// only "a module was loaded" channel the host has) and to log what it sees.
//
// Three paths, all observers - every call is forwarded unchanged:
//
//   1. Streamline      sl.interposer.dll
//        slSetConstants / slSetTagForFrame / slSetTag / slEvaluateFeature /
//        slDLSSGGetState / slDLSSGSetOptions
//      sl::Constants carries cameraViewToClip, clipToCameraView, clipToPrevClip,
//      prevClipToClip, jitterOffset (pixels), mvecScale (normalised to [-1,1]),
//      near/far/fov/aspect and the Boolean flags (depthInverted,
//      cameraMotionIncluded, motionVectorsJittered, motionVectorsDilated, reset,
//      orthographicProjection, motionVectors3D). The resource tags carry the real
//      ID3D12Resource of depth / motion vectors / HUD-less colour, with extent and
//      lifecycle. This is the richest of the three and the only one that also tells
//      us how the engine *thinks* about the frame.
//
//   2. NGX DLSS-G      nvngx_dlssg.dll  (exports are prefixed: DLSSG_NVSDK_NGX_*)
//        Create/EvaluateFeature -> "DLSSG.CameraViewToClip", "DLSSG.ClipToPrevClip",
//        "DLSSG.MVecs", "DLSSG.Depth", "DLSSG.HUDLess", "DLSSG.MVecScaleX/Y",
//        "DLSSG.DepthInverted", "DLSSG.MultiFrameCount/Index", ...
//      (names from third_party/dlss/include/nvsdk_ngx_defs_dlssg.h). This is the path
//      a patched (SM86 / multiframe-unlocked) runtime still uses: the unlock changes
//      model selection inside the runtime, never the entry points, so hooking is
//      unaffected - and "DLSSG.MultiFrameCountMax" is the quickest proof of *which*
//      copy we attached to.
//
//   3. XeSS-FG         libxess_fg.dll  (xefgSwapChain* family)
//        xefgSwapChainTagFrameConstants  -> viewMatrix + projectionMatrix + jitter +
//        motionVectorScale + resetHistory + frameRenderTime
//        (third_party/xess/inc/xess_fg/xefg_swapchain.h:150)
//        xefgSwapChainD3D12TagFrameResource -> depth / MV / HUD-less / UI / backbuffer
//        xefgSwapChainD3D12InitFromSwapChain(Desc) -> the game's own initFlags
//      Cheapest matrix source of all: the integrated XeSS-FG plugin hands the library
//      a full view+projection pair per presentId.
//
// Enable with [capture] FgSniff=1 (and Log=1 - logf() is a no-op otherwise).
//
// Two safety rules this file follows, because a wrong offset produces
// plausible-looking garbage:
//   * the mirror structs below are asserted at compile time (offsetof/sizeof), and
//   * every structure is validated at run time by its Streamline GUID; on a mismatch
//     the raw bytes are logged and nothing is interpreted.
// ============================================================================
#include "inputs.h"

#include <tlhelp32.h>

#include <cmath>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <atomic>
#include <mutex>

#include "MinHook.h"
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_defs_dlssg.h"
#include "nvsdk_ngx_params.h"
// xefg_swapchain_d3d12.h pulls xefg_swapchain.h (frame constants, resource types)
// and <dxgi1_5.h>. xess/xess_d3d12.h in in_xess.cpp already drags the same headers
// in, so this is a duplicate include, not a new dependency.
#include "xess_fg/xefg_swapchain.h"
#include "xess_fg/xefg_swapchain_d3d12.h"

// One TU (unity.cpp) includes every source: keep all state in this namespace.
namespace fg_impl {

using namespace reshade::api;

// ---------------------------------------------------------------------------
// Log shaping
//
// The host's logf() writes one timestamped line per call and flushes, and it is
// called from render/present threads. A dump of four 4x4 matrices is five lines, so
// every block is built into a buffer first and emitted with a single logf("%s", ...)
// - one timestamp per block, and no interleaving from another thread in the middle
// of it.
// ---------------------------------------------------------------------------
// Same reason as g_hook_mutex below: no destructor, so a hot unload cannot leave a null mutex
// behind for a late callback to trip over.
static SRWLOCK g_log_mutex = SRWLOCK_INIT;


struct Buf
{
    char*  d;
    size_t cap;
    size_t len;

    Buf(char* data, size_t c) : d(data), cap(c), len(0) { if (cap) { d[0] = '\0'; } }

    void addf(const char* fmt, ...)
    {
        if (len + 1 >= cap) { return; }
        va_list a;
        va_start(a, fmt);
        const int n = vsnprintf(d + len, cap - len, fmt, a);
        va_end(a);
        if (n > 0) {
            len += static_cast<size_t>(n);
            if (len >= cap) { len = cap - 1; }
        }
    }
};

static void emit(const char* text)
{
    AcquireSRWLockExclusive(&g_log_mutex);
    mgpu::input::logf("%s", text);
    ReleaseSRWLockExclusive(&g_log_mutex);
}

// "%9.4f" of infinity prints "      inf" - readable enough, and %g would hide the
// difference between 1e5 and 75000 in a far plane.
static void matrix_rows(Buf& b, const char* label, const float* m)
{
    b.addf("  %-16s[", label);
    for (int r = 0; r < 4; ++r) {
        if (r != 0) { b.addf(" |"); }
        for (int c = 0; c < 4; ++c) { b.addf(" %9.4f", m[r * 4 + c]); }
    }
    b.addf(" ]\n");
}

// Streamline's Boolean is `enum Boolean : char`, eFalse=0 / eTrue=1 / eInvalid=2.
static int sb(uint8_t v) { return (v <= 1) ? static_cast<int>(v) : -1; }

static bool same_f(float a, float b, float eps)
{
    if (a == b) { return true; }
    if (a != a && b != b) { return true; }      // both NaN
    return fabsf(a - b) <= eps;
}

static bool same_mat(const float* a, const float* b, float eps = 1e-4f)
{
    for (int i = 0; i < 16; ++i) {
        if (!same_f(a[i], b[i], eps)) { return false; }
    }
    return true;
}

// Rate policy, shared by every path:
//   * the first kFirst calls in full (so a 3-frame run is still usable),
//   * immediately again when the declaration changed (preset / FOV / mvScale /
//     flags - i.e. the things worth noticing),
//   * otherwise one compact line per kEvery calls, and a full block every
//     kFullEvery calls.
// At 120 fps that is ~1 line/s and one full block per 10 s: small enough to leave
// on for a whole session. Jitter is deliberately *not* part of "changed": it moves
// every frame by design, and it is already printed on the compact line.
static constexpr uint32_t kFirst     = 3;
static constexpr uint32_t kEvery     = 120;
static constexpr uint32_t kFullEvery = 1200;

static bool want_line(uint32_t n, bool changed) { return n <= kFirst || changed || (n % kEvery) == 0; }
static bool want_full(uint32_t n, bool changed) { return n <= kFirst || changed || (n % kFullEvery) == 0; }

// ---------------------------------------------------------------------------
// Layout mirrors
//
// We deliberately do not include Streamline's headers: third_party/ has no
// Streamline, and copying sl.h in would drag in the whole SDK. The layouts below are
// transcribed from Streamline 2.x (sl_struct.h BaseStructure, sl_consts.h Constants,
// sl_core_types.h Resource/ResourceTag/ViewportHandle - the same ones OptiScaler
// vendors) and are asserted here, then re-validated at run time by GUID.
// ---------------------------------------------------------------------------
static constexpr uint32_t kGuidConstants   = 0xdcd35ad7u;   // sl::Constants
static constexpr uint32_t kGuidResourceTag = 0x4c6a5aadu;   // sl::ResourceTag
static constexpr uint32_t kGuidDlssgOptions = 0xfac5f1cbu;  // sl::DLSSGOptions
static constexpr uint32_t kGuidDlssgState   = 0xcc8ac8e1u;  // sl::DLSSGState

// sl::BaseStructure { BaseStructure* next; StructType structType; size_t version; }
// next(0..8), structType(8..24), structVersion(24..32), payload(32..).
static constexpr size_t kBaseSize     = 32;
static constexpr size_t kGuidOffset   = 8;    // StructType.data1
static constexpr size_t kVersionOffset = 24;

struct SlConstants
{
    void*    next;
    uint8_t  struct_type[16];      // StructType: data1, data2, data3, data4[8]
    size_t   struct_version;

    // kStructVersion1
    float camera_view_to_clip[16]; // row major, MUST NOT contain TAA jitter
    float clip_to_camera_view[16];
    float clip_to_lens_clip[16];
    float clip_to_prev_clip[16];
    float prev_clip_to_clip[16];
    float jitter_offset[2];        // pixels
    float mvec_scale[2];           // normalises the vectors to [-1,1]
    float camera_pinhole_offset[2];
    float camera_pos[3];
    float camera_up[3];
    float camera_right[3];
    float camera_fwd[3];
    float camera_near;
    float camera_far;
    float camera_fov;              // radians
    float camera_aspect_ratio;
    float motion_vectors_invalid_value;
    uint8_t depth_inverted;            // Boolean
    uint8_t camera_motion_included;    // Boolean
    uint8_t motion_vectors_3d;         // Boolean
    uint8_t reset;                     // Boolean
    uint8_t orthographic_projection;   // Boolean
    uint8_t motion_vectors_dilated;    // Boolean
    uint8_t motion_vectors_jittered;   // Boolean
    uint8_t _pad0;
    // kStructVersion2
    float min_relative_linear_depth_object_separation;
};

struct SlViewportHandle
{
    void*    next;
    uint8_t  struct_type[16];
    size_t   struct_version;
    uint32_t value;
};

struct SlResource            // sl::Resource: only the head we read
{
    void*    next;
    uint8_t  struct_type[16];
    size_t   struct_version;
    uint32_t type;
    uint32_t _pad;
    void*    native;          // ID3D12Resource*
    void*    memory;
    void*    view;
    uint32_t state;           // D3D12_RESOURCE_STATES
    uint32_t width;
    uint32_t height;
    uint32_t native_format;
    uint32_t mip_levels;
};

struct SlResourceTag
{
    void*    next;
    uint8_t  struct_type[16];
    size_t   struct_version;
    void*    resource;        // sl::Resource*
    uint32_t type;            // BufferType
    uint32_t lifecycle;       // ResourceLifecycle
    uint32_t extent[4];       // top, left, width, height
};

// sl::DLSSGState head. Boolean is 1 byte; we stop after numFramesToGenerateMax
// (kStructVersion2) and never look at the rest.
struct SlDlssgState
{
    unsigned long long estimated_vram;
    uint32_t status;
    uint32_t min_width_or_height;
    uint32_t frames_actually_presented;
    uint32_t frames_to_generate_max;
};

static_assert(kBaseSize == 32, "sl::BaseStructure is next(8) + StructType(16) + size_t(8)");
// Compile-time size checks. A mismatch means the mirror no longer matches Streamline
// 2.x (a changed Boolean width, a new member, different padding) - fix the struct here
// rather than shipping wrong offsets. `offsetof` is deliberately not used: windows.h
// redefines it as a non-constant expression.
static_assert(sizeof(SlConstants) == 456, "sl::Constants v2 is 456 bytes (452 + 4 for v2)");
static_assert(sizeof(SlViewportHandle) == 40, "ViewportHandle = BaseStructure + uint32 + padding");
static_assert(sizeof(SlResourceTag) == 64, "ResourceTag = header+resource+type+lifecycle+extent");
static_assert(sizeof(SlDlssgState) == 24, "DLSSGState head = vram(8) + 4 x uint32");
static_assert(sizeof(SlConstants) > 32 + 16 * 5, "the five matrices must fit before jitter");


// ---------------------------------------------------------------------------
// 1. Streamline (sl.interposer.dll)
//
// Signatures are the C exports from sl_core_api.h; sl::Result is an int-sized enum
// and sl::Feature a uint32, so plain ints keep this file free of the SL headers.
// ---------------------------------------------------------------------------
typedef int (*pfn_sl_set_constants)(const void* values, const void* frame, const void* viewport);
typedef int (*pfn_sl_set_tag_for_frame)(const void* frame, const void* viewport,
                                        const void* tags, uint32_t num, void* cmd);
typedef int (*pfn_sl_set_tag)(const void* viewport, const void* tags, uint32_t num, void* cmd);
typedef int (*pfn_sl_evaluate)(uint32_t feature, const void* frame,
                               const void* const* inputs, uint32_t num, void* cmd);
typedef int (*pfn_sl_dlssg_get_state)(const void* viewport, void* state, const void* options);

static pfn_sl_set_constants     g_real_set_constants      = nullptr;
static pfn_sl_set_tag_for_frame g_real_set_tag_for_frame  = nullptr;
static pfn_sl_set_tag           g_real_set_tag            = nullptr;
static pfn_sl_evaluate          g_real_evaluate           = nullptr;
static pfn_sl_dlssg_get_state   g_real_dlssg_get_state    = nullptr;

// sl::Feature::kFeatureDLSS_G (sl_core_types.h:235). kFeatureDLSS (0) is the upscaler,
// which in_ngx.cpp already covers.
static constexpr uint32_t kFeatureDlssG = 1000;

static uint32_t g_const_calls = 0;
static uint32_t g_tag_calls   = 0;
static uint32_t g_eval_calls  = 0;
static uint32_t g_last_vp     = 0xFFFFFFFFu;
static uint32_t g_last_flags  = 0xFFFFFFFFu;
static unsigned long long g_tag_sig = 0;
static unsigned long long g_tag_last_ms = 0;
static SlConstants g_last_constants{};
static bool g_have_last_constants = false;
static bool g_const_guid_bad = false;
static bool g_tag_guid_bad   = false;
static long long g_last_mf_max = -1;

// sl::ResourceLifecycle (external/streamline/sl_core_types.h:372). The distinction matters:
// eValidUntilEvaluate means SL may still rewrite the resource until slEvaluateFeature returns,
// so a tag with that lifecycle is NOT yet "this frame's input" - OptiScaler carries a whole
// quirk for games that rely on it (IgnoreValidUntilEvaluateForFG,
// docs/findings/OPTISCALER_QUIRKS.md 3.3).
static const char* lifecycle_name(uint32_t lc)
{
    switch (lc) {
    case 0:  return "onlyValidNow";
    case 1:  return "validUntilPresent";
    case 2:  return "validUntilEvaluate";
    default: return "?";
    }
}

// BufferType names for the tags worth seeing in a log. The rest stay numeric.
static const char* buffer_type_name(uint32_t t)
{
    switch (t) {
    case 0:  return "DEPTH";
    case 1:  return "MOTION_VECTORS";
    case 2:  return "HUDLESS_COLOR";
    case 3:  return "SCALING_INPUT";
    case 4:  return "SCALING_OUTPUT";
    case 13: return "EXPOSURE";
    case 23: return "UI_COLOR_AND_ALPHA";
    case 30: return "RAYTRACING_DISTANCE";
    default: return nullptr;
    }
}

static bool is_identity_mat(const float* m)
{
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const float want = (r == c) ? 1.0f : 0.0f;
            if (!same_f(m[r * 4 + c], want, 1e-6f)) { return false; }
        }
    }
    return true;
}

static const void* tag_native(const SlResourceTag& tag, uint32_t* w, uint32_t* h, uint32_t* st)
{
    if (w)  { *w = 0; }
    if (h)  { *h = 0; }
    if (st) { *st = 0; }
    if (!tag.resource) { return nullptr; }
    const SlResource* r = static_cast<const SlResource*>(tag.resource);
    if (w)  { *w = r->width; }
    if (h)  { *h = r->height; }
    if (st) { *st = r->state; }
    return r->native;
}

// A `const sl::ViewportHandle&` is a real object (value at +0x20). A small integer here
// means the call has a different signature than assumed - slGetFeatureFunction passes a
// sl::Feature, for instance - and must never be dereferenced. This is not hypothetical:
// reading viewport->value in the slGetFeatureFunction hook crashed PBZ with an
// access violation at 0x408 (= feature 1000 + offset 0x20).
static uint32_t viewport_value(const void* vp)
{
    const uintptr_t p = reinterpret_cast<uintptr_t>(vp);
    if (p < 0x10000ull) { return 0xFFFFFFFFu; }
    return reinterpret_cast<const SlViewportHandle*>(vp)->value;
}

static unsigned long long fnv1a(unsigned long long h, unsigned long long v)
{
    h ^= v;
    return h * 1099511628211ull;
}

// Turns the game's own declaration into a log line. Everything printed here is a
// value the *engine* handed to its FG provider for the frame it is about to present.
// Hand the captured camera to the host (protocol v12). Streamline's sl::Constants is documented
// as jitter-free ("MUST NOT contain TAA jitter"), which is exactly what a consumer needs for its
// own projection: the real horizontal FOV, the infinite reversed-Z form and the true aspect all
// come with it, so the consumer can stop guessing with --fg-fov.
// [capture] StreamlineCamera=0 disables the hand-off (the sniffer keeps running for the log).
static bool camera_forwarding()
{
    static int cached = -1;
    if (cached < 0) {
        cached = mgpu::input::config_int("StreamlineCamera", 1);
        // One line so "the matrix never arrives" can be told apart from "we never asked".
        if (cached == 0) {
            mgpu::input::logf("fg: camera hand-off OFF ([capture] StreamlineCamera=0)\n");
        }
    }
    return cached != 0;
}

static void sniff_constants(const void* values, const void* viewport_raw)
{
    if (!values) { return; }
    const unsigned char* raw = static_cast<const unsigned char*>(values);

    uint32_t guid = 0;
    memcpy(&guid, raw + kGuidOffset, sizeof(guid));
    if (guid != kGuidConstants) {
        if (!g_const_guid_bad) {
            g_const_guid_bad = true;
            char buf[384];
            Buf b(buf, sizeof(buf));
            b.addf("fg: slSetConstants: unexpected struct GUID 0x%08X (mirror expects 0x%08X)."
                   " Raw head:", guid, kGuidConstants);
            for (int i = 0; i < 32; ++i) { b.addf(" %02X", raw[i]); }
            b.addf("\n  -> layout differs from the Streamline 2.x mirror in this file;"
                   " nothing is interpreted (no values, no conclusions).\n");
            emit(buf);
        }
        return;
    }

    const SlConstants* c = reinterpret_cast<const SlConstants*>(values);
    const uint32_t n = ++g_const_calls;
    const uint32_t vp = viewport_value(viewport_raw);

    const uint32_t flags =
        static_cast<uint32_t>(c->depth_inverted) |
        (static_cast<uint32_t>(c->camera_motion_included) << 8) |
        (static_cast<uint32_t>(c->motion_vectors_3d) << 16) |
        (static_cast<uint32_t>(c->reset) << 24);

    // v12: forward the camera *before* any logging gate - want_line() can return early (and
    // logging is off by default), but the consumer still needs the projection.
    if (camera_forwarding()) {
        mgpu::input::publish_fg_camera(c->camera_view_to_clip, c->camera_near, c->camera_far,
                                       c->camera_fov, GetTickCount64());
        static bool once = false;
        if (!once) {
            once = true;
            mgpu::input::logf("fg: camera source = Streamline sl::Constants"
                              " (cameraViewToClip)\n");
        }
    }

    // "changed" = the *declaration* moved (preset, FOV, aspect, MV scale, depth
    // direction, reset). Measured on PBZ: the projection carries the frame's TAA jitter
    // in its off-diagonal terms (~5e-4 per frame), so a 1e-4 epsilon made every frame
    // "changed" and produced 4.5 MB of log in 4 minutes. 1e-2 ignores the jitter but
    // still catches a preset/FOV change, which moves m[0][0] by far more than that.
    const bool changed =
        !g_have_last_constants ||
        vp != g_last_vp ||
        flags != g_last_flags ||
        !same_mat(c->camera_view_to_clip, g_last_constants.camera_view_to_clip, 1e-2f) ||
        !same_f(c->mvec_scale[0], g_last_constants.mvec_scale[0], 1e-4f) ||
        !same_f(c->mvec_scale[1], g_last_constants.mvec_scale[1], 1e-4f) ||
        !same_f(c->camera_near, g_last_constants.camera_near, 1e-5f) ||
        !same_f(c->camera_far, g_last_constants.camera_far, 1e-2f) ||
        !same_f(c->camera_fov, g_last_constants.camera_fov, 1e-4f) ||
        !same_f(c->camera_aspect_ratio, g_last_constants.camera_aspect_ratio, 1e-4f);

    if (!want_line(n, changed)) { return; }

    char buf[3072];
    Buf b(buf, sizeof(buf));
    const bool full = want_full(n, changed);

    // mvPx: if mvecScale is 1/size (the "texels are pixels" convention), this prints the
    // size it implies - the fastest way to see which unit a title uses.
    const float mvx = c->mvec_scale[0] > 0.0f ? 1.0f / c->mvec_scale[0] : 0.0f;
    const float mvy = c->mvec_scale[1] > 0.0f ? 1.0f / c->mvec_scale[1] : 0.0f;
    b.addf("fg: sl.const #%u%svp=%u ver=%llu jit=(%+.3f,%+.3f)px mvS=(%.6f,%.6f) mvPx=%dx%d"
           " zn=%.4f zf=%.1f fovY=%.4frad(%.2fdeg) aspect=%.4f"
           " dpi=%d cmi=%d mv3d=%d reset=%d ortho=%d mvj=%d mvd=%d mvBad=%.3e"
           " projShear=(%+.5f,%+.5f)\n",
           n, changed ? " *CHANGED* " : " ", vp,
           static_cast<unsigned long long>(c->struct_version),
           c->jitter_offset[0], c->jitter_offset[1],
           c->mvec_scale[0], c->mvec_scale[1],
           static_cast<int>(mvx + 0.5f), static_cast<int>(mvy + 0.5f),
           c->camera_near, c->camera_far, c->camera_fov,
           c->camera_fov * 57.2957795f, c->camera_aspect_ratio,
           sb(c->depth_inverted), sb(c->camera_motion_included), sb(c->motion_vectors_3d),
           sb(c->reset), sb(c->orthographic_projection),
           sb(c->motion_vectors_jittered), sb(c->motion_vectors_dilated),
           c->motion_vectors_invalid_value,
           c->camera_view_to_clip[8], c->camera_view_to_clip[9]);

    if (full) {
        matrix_rows(b, "cameraViewToClip", c->camera_view_to_clip);
        matrix_rows(b, "clipToCameraView", c->clip_to_camera_view);
        matrix_rows(b, "clipToPrevClip", c->clip_to_prev_clip);
        matrix_rows(b, "prevClipToClip", c->prev_clip_to_clip);
        b.addf("  cameraPos=(%.3f,%.3f,%.3f) up=(%.3f,%.3f,%.3f) right=(%.3f,%.3f,%.3f)"
               " fwd=(%.3f,%.3f,%.3f) lensIdentity=%d pinhole=(%.4f,%.4f) minDepthSep=%.2f\n",
               c->camera_pos[0], c->camera_pos[1], c->camera_pos[2],
               c->camera_up[0], c->camera_up[1], c->camera_up[2],
               c->camera_right[0], c->camera_right[1], c->camera_right[2],
               c->camera_fwd[0], c->camera_fwd[1], c->camera_fwd[2],
               is_identity_mat(c->clip_to_lens_clip) ? 1 : 0,
               c->camera_pinhole_offset[0], c->camera_pinhole_offset[1],
               c->min_relative_linear_depth_object_separation);
    }
    emit(buf);

    g_last_constants = *c;
    g_have_last_constants = true;
    g_last_vp = vp;
    g_last_flags = flags;
}

// The tags are the resources the FG library will actually read. Their native
// pointers and sizes are what tell us "is the FG's MV the same texture the
// upscaler saw, and at which resolution".
static void sniff_tags(const void* viewport_raw, const void* tags_raw, uint32_t num)
{
    if (!tags_raw || num == 0 || num > 64) { return; }

    const SlResourceTag* tags = static_cast<const SlResourceTag*>(tags_raw);
    uint32_t guid = 0;
    memcpy(&guid, reinterpret_cast<const unsigned char*>(tags) + kGuidOffset, sizeof(guid));
    if (guid != kGuidResourceTag) {
        if (!g_tag_guid_bad) {
            g_tag_guid_bad = true;
            const unsigned char* raw = reinterpret_cast<const unsigned char*>(tags);
            char buf[384];
            Buf b(buf, sizeof(buf));
            b.addf("fg: slSetTag: unexpected tag GUID 0x%08X (mirror expects 0x%08X)."
                   " Raw head:", guid, kGuidResourceTag);
            for (int i = 0; i < 32; ++i) { b.addf(" %02X", raw[i]); }
            b.addf("\n  -> tag layout differs; resources are not interpreted.\n");
            emit(buf);
        }
        return;
    }

    const uint32_t n = ++g_tag_calls;
    const uint32_t vp = viewport_value(viewport_raw);

    unsigned long long sig = 1469598103934665603ull;
    for (uint32_t i = 0; i < num; ++i) {
        uint32_t w = 0, h = 0;
        const void* native = tag_native(tags[i], &w, &h, nullptr);
        sig = fnv1a(sig, tags[i].type);
        sig = fnv1a(sig, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(native)));
        sig = fnv1a(sig, (static_cast<unsigned long long>(w) << 32) | h);
    }
    const bool changed = (sig != g_tag_sig);
    g_tag_sig = sig;
    // A tag *set* can legitimately vary call to call (the engine tags subsets), so a
    // pure "changed" rule can print every frame. Cap it at ~4 lines/s; the periodic
    // line still shows the current state.
    const unsigned long long now = GetTickCount64();
    if (n > kFirst && (now - g_tag_last_ms) < 250ull) { return; }
    if (!want_line(n, changed)) { return; }
    g_tag_last_ms = now;

    char buf[2048];
    Buf b(buf, sizeof(buf));
    b.addf("fg: sl.tags #%u%svp=%u n=%u\n", n, changed ? " *CHANGED* " : " ", vp, num);
    for (uint32_t i = 0; i < num; ++i) {
        const SlResourceTag& t = tags[i];
        uint32_t w = 0, h = 0, st = 0;
        const void* native = tag_native(t, &w, &h, &st);
        const char* nm = buffer_type_name(t.type);
        b.addf("  %-18s(%2u) %p %ux%u state=0x%X lc=%u(%s)%s",
               nm ? nm : "OTHER", t.type, native, w, h, st, t.lifecycle,
               lifecycle_name(t.lifecycle),
               // Staged for Evaluate: SL may still rewrite it, so it must not be treated as
               // the frame's finished input (the depth/MV we copy comes from the evaluate
               // params, not from these tags - this line is what proves it).
               (t.lifecycle == 2 && t.type <= 1u)   // 0 = depth, 1 = motion vectors
                   ? " [staged for Evaluate - not read as input]" : "");
        if (t.extent[2] || t.extent[3]) {
            b.addf(" extent=(%u,%u %ux%u)", t.extent[0], t.extent[1], t.extent[2], t.extent[3]);
        }
        b.addf("%s\n", native ? "" : " (no resource)");
    }
    emit(buf);
}

// ------------------------------------------------------------------ SL hooks
// Every one of these forwards the call unchanged. The hook is a reader.

static int hook_sl_set_constants(const void* values, const void* frame, const void* viewport)
{
    sniff_constants(values, viewport);
    return g_real_set_constants(values, frame, viewport);
}

static int hook_sl_set_tag_for_frame(const void* frame, const void* viewport,
                                     const void* tags, uint32_t num, void* cmd)
{
    sniff_tags(viewport, tags, num);
    return g_real_set_tag_for_frame(frame, viewport, tags, num, cmd);
}

static int hook_sl_set_tag(const void* viewport, const void* tags, uint32_t num, void* cmd)
{
    sniff_tags(viewport, tags, num);
    return g_real_set_tag(viewport, tags, num, cmd);
}

// "Which structures did the engine hand to DLSS-G for this frame". The GUIDs answer
// questions the values cannot: e.g. whether the game sends one Constants struct per
// generated frame or one per real frame.
static const char* sl_struct_name(uint32_t guid)
{
    switch (guid) {
    case kGuidConstants:   return "Constants";
    case kGuidResourceTag: return "ResourceTag";
    default:               return nullptr;
    }
}

static int hook_sl_evaluate(uint32_t feature, const void* frame,
                            const void* const* inputs, uint32_t num, void* cmd)
{
    const uint32_t n = ++g_eval_calls;
    // Only DLSS-G is interesting here (kFeatureDLSS=0 is the upscaler, whose
    // parameters in_ngx.cpp already covers), but the count is logged for all so a
    // silent path is visible.
    if (feature == kFeatureDlssG && want_line(n, false)) {
        char buf[1024];
        Buf b(buf, sizeof(buf));
        b.addf("fg: sl.eval #%u feature=DLSS_G inputs=%u", n, num);
        for (uint32_t i = 0; i < num && inputs; ++i) {
            if (!inputs[i]) { b.addf(" [null]"); continue; }
            const unsigned char* raw = static_cast<const unsigned char*>(inputs[i]);
            uint32_t guid = 0;
            size_t   ver = 0;
            memcpy(&guid, raw + kGuidOffset, sizeof(guid));
            memcpy(&ver, raw + kVersionOffset, sizeof(ver));
            const char* nm = sl_struct_name(guid);
            b.addf(" [%s v%llu]", nm ? nm : "?", static_cast<unsigned long long>(ver));
        }
        b.addf("\n");
        emit(buf);
    }
    return g_real_evaluate(feature, frame, inputs, num, cmd);
}

// slDLSSGGetState is the only place that answers "how many frames can this runtime
// generate". On an SM86-patched runtime frames_to_generate_max is > 1 where the stock
// one reports 1 - i.e. this line proves the hook landed on the patched copy.
static int hook_sl_dlssg_get_state(const void* viewport, void* state, const void* options)
{
    const int r = g_real_dlssg_get_state(viewport, state, options);
    if (r == 0 && state) {
        // Validate before interpreting: DLSSGState is versioned and this mirror is only
        // the head of it. A wrong GUID means "do not read", not "read anyway".
        uint32_t guid = 0;
        memcpy(&guid, static_cast<const unsigned char*>(state) + kGuidOffset, sizeof(guid));
        const SlDlssgState* s = static_cast<const SlDlssgState*>(state);
        if (guid != kGuidDlssgState || s->frames_to_generate_max > 16u) { return r; }

        const long long mf = static_cast<long long>(s->frames_to_generate_max);
        if (mf != g_last_mf_max) {
            g_last_mf_max = mf;
            char buf[512];
            Buf b(buf, sizeof(buf));
            b.addf("fg: slDLSSGGetState: status=%u minDim=%u presented=%u"
                   " framesToGenerateMax=%lld%s\n",
                   s->status, s->min_width_or_height, s->frames_actually_presented, mf,
                   (mf > 1) ? "  (multiframe capable: patched / newer runtime)"
                            : "  (2x only: stock runtime or a driver cap)");
            emit(buf);
        }
    }
    return r;
}

// ------------------------------------------- DLSS-G options via slGetFeatureFunction
//
// Streamline does not export slDLSSGSetOptions / slDLSSGGetState: they live in the plugin
// and are fetched with slGetFeatureFunction(viewport, "slDLSSGSetOptions", &pfn). PBZ's
// interposer indeed reports "does not export slDLSSGGetState", so hooking that call is
// the only way to see *whether frame generation is on, at what multiplier, and with which
// buffer sizes/formats*. The list of names it asks for is also a fingerprint of the
// integration (DLSSG* => frame generation, slDLSSGet* => the upscaler).
struct SlDlssgOptions
{
    void*    next;
    uint8_t  struct_type[16];
    size_t   struct_version;
    int32_t  mode;                    // 0 off, 1 on, 2 dynamic
    uint32_t num_frames_to_generate;  // 1 = 2x, 2 = 3x, 3 = 4x
    uint32_t flags;
    uint32_t dynamic_res_w;
    uint32_t dynamic_res_h;
    uint32_t num_back_buffers;
    uint32_t mvec_depth_w;
    uint32_t mvec_depth_h;
    uint32_t color_w;
    uint32_t color_h;
    uint32_t color_format;
    uint32_t mvec_format;
    uint32_t depth_format;
    uint32_t hudless_format;
    uint32_t ui_format;
};
static_assert(sizeof(SlDlssgOptions) == 96, "DLSSGOptions head: 32 + 15 x u32 + padding");

typedef int (*pfn_sl_get_feature_function)(uint32_t feature, const char* name, void** pfn);
typedef int (*pfn_sl_dlssg_set_options)(const void* viewport, const void* options);

static pfn_sl_get_feature_function g_real_get_feature_function = nullptr;
static pfn_sl_dlssg_set_options    g_real_dlssg_set_options    = nullptr;
static int g_dlssg_options_key = -999;

static int hook_sl_dlssg_set_options(const void* viewport, const void* options)
{
    (void)viewport;
    if (options) {
        uint32_t guid = 0;
        memcpy(&guid, static_cast<const unsigned char*>(options) + kGuidOffset, sizeof(guid));
        if (guid == kGuidDlssgOptions) {
            const SlDlssgOptions* o = static_cast<const SlDlssgOptions*>(options);
            const int key = static_cast<int>(o->mode) * 100 + static_cast<int>(o->num_frames_to_generate);
            if (key != g_dlssg_options_key) {
                g_dlssg_options_key = key;
                char buf[768];
                Buf b(buf, sizeof(buf));
                b.addf("fg: dlssg.options mode=%d framesToGenerate=%u (=%ux) mvecDepth=%ux%u"
                       " color=%ux%u backBuffers=%u fmt(color=%u mv=%u depth=%u hudless=%u ui=%u)"
                       "%s\n",
                       o->mode, o->num_frames_to_generate, o->num_frames_to_generate + 1,
                       o->mvec_depth_w, o->mvec_depth_h, o->color_w, o->color_h,
                       o->num_back_buffers, o->color_format, o->mvec_format, o->depth_format,
                       o->hudless_format, o->ui_format,
                       (o->mode == 0) ? "  (frame generation OFF)" : "");
                emit(buf);
            }
        }
    }
    return g_real_dlssg_set_options(viewport, options);
}

// NOTE ON THE FIRST ARGUMENT: this is `sl::Feature`, NOT a viewport. The vendored header
// says `PFun_slGetFeatureFunction = Result(sl::Feature feature, const char* functionName,
// void*& function)`, and the first version of this hook assumed a ViewportHandle and
// dereferenced it - which crashed PBZ at startup (access violation at 0x408 = feature
// 1000 + 0x20). Never touch the pointer here; log it as an integer, that *is* the feature.
static int hook_sl_get_feature_function(uint32_t feature, const char* name, void** pfn)
{
    const int r = g_real_get_feature_function(feature, name, pfn);

    if (name != nullptr) {
        // Each distinct name is logged once (cap 24): this is the cheapest way to see
        // which integration is in use, and it costs nothing when a title never asks.
        static char seen[24][40];
        static int  seen_count = 0;
        bool known = false;
        for (int i = 0; i < seen_count; ++i) {
            if (strncmp(seen[i], name, sizeof(seen[0]) - 1) == 0) { known = true; break; }
        }
        if (!known && seen_count < 24) {
            strncpy_s(seen[seen_count], sizeof(seen[0]), name, _TRUNCATE);
            ++seen_count;
            // feature: 0 = DLSS, 1000 = DLSS-G, 1001 = DLSS-RR (sl_core_types.h)
            mgpu::input::logf("fg: sl.getFeatureFunction feature=%u name=%s -> %p\n",
                              feature, name, (r == 0 && pfn) ? *pfn : nullptr);
        }

        // The option/state entry points are only reachable here, so hook them on first ask.
        // slDLSSGGetState is the one that answers "how many frames can this runtime
        // generate" - PBZ fetches it ~14 s before it ever touches SetOptions, so hooking
        // both means the multiplier is known even if the menu never enables FG.
        if (r == 0 && pfn != nullptr && *pfn != nullptr &&
            strcmp(name, "slDLSSGSetOptions") == 0 && g_real_dlssg_set_options == nullptr) {
            LPVOID addr = reinterpret_cast<LPVOID>(*pfn);
            if (MH_CreateHook(addr, reinterpret_cast<LPVOID>(&hook_sl_dlssg_set_options),
                              reinterpret_cast<LPVOID*>(&g_real_dlssg_set_options)) == MH_OK &&
                MH_EnableHook(addr) == MH_OK) {
                mgpu::input::logf("fg: hooked slDLSSGSetOptions @ %p (via slGetFeatureFunction)\n",
                                  addr);
            }
        }
        if (r == 0 && pfn != nullptr && *pfn != nullptr &&
            strcmp(name, "slDLSSGGetState") == 0 && g_real_dlssg_get_state == nullptr) {
            LPVOID addr = reinterpret_cast<LPVOID>(*pfn);
            if (MH_CreateHook(addr, reinterpret_cast<LPVOID>(&hook_sl_dlssg_get_state),
                              reinterpret_cast<LPVOID*>(&g_real_dlssg_get_state)) == MH_OK &&
                MH_EnableHook(addr) == MH_OK) {
                mgpu::input::logf("fg: hooked slDLSSGGetState @ %p (via slGetFeatureFunction)\n",
                                  addr);
            }
        }
    }
    return r;
}

static bool install_sl_hooks(HMODULE sl)
{
    struct Target { const char* name; LPVOID detour; LPVOID* original; };
    const Target targets[] = {
        { "slSetConstants",
          reinterpret_cast<LPVOID>(&hook_sl_set_constants),
          reinterpret_cast<LPVOID*>(&g_real_set_constants) },
        { "slSetTagForFrame",
          reinterpret_cast<LPVOID>(&hook_sl_set_tag_for_frame),
          reinterpret_cast<LPVOID*>(&g_real_set_tag_for_frame) },
        { "slSetTag",
          reinterpret_cast<LPVOID>(&hook_sl_set_tag),
          reinterpret_cast<LPVOID*>(&g_real_set_tag) },
        { "slEvaluateFeature",
          reinterpret_cast<LPVOID>(&hook_sl_evaluate),
          reinterpret_cast<LPVOID*>(&g_real_evaluate) },
        { "slDLSSGGetState",
          reinterpret_cast<LPVOID>(&hook_sl_dlssg_get_state),
          reinterpret_cast<LPVOID*>(&g_real_dlssg_get_state) },
        { "slGetFeatureFunction",
          reinterpret_cast<LPVOID>(&hook_sl_get_feature_function),
          reinterpret_cast<LPVOID*>(&g_real_get_feature_function) },
    };

    int ok = 0, missing = 0;
    for (const auto& t : targets) {
        if (*t.original != nullptr) {
            // Already installed through another provider (the driver shim forwards to the
            // runtime we hooked). Re-patching the prologue would break the existing
            // trampoline - and ReShade hooks Streamline too, so this is not theoretical.
            ++ok;
            continue;
        }
        LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(sl, t.name));
        if (!addr) {
            ++missing;
            mgpu::input::logf("fg: sl.interposer does not export %s\n", t.name);
            continue;
        }
        if (MH_CreateHook(addr, t.detour, t.original) != MH_OK ||
            MH_EnableHook(addr) != MH_OK) {
            mgpu::input::logf("fg: hook %s FAILED\n", t.name);
            continue;
        }
        ++ok;
    }
    mgpu::input::logf("fg: Streamline hooks active (%d installed, %d not exported)."
                      " A missing \"#1\" line in the log means that path is not used"
                      " by this title.\n", ok, missing);
    return ok != 0;
}

// ---------------------------------------------------------------------------
// 2. NGX DLSS-G (nvngx_dlssg.dll)
//
// The runtime's exports are prefixed (`DLSSG_NVSDK_NGX_D3D12_*`), which is also how a
// patched (SM86 / multiframe-unlocked) copy is still reached: the unlock patches bytes
// inside the runtime, not the entry points. We therefore hook the prefixed exports and
// fall back to the unprefixed ones if a build only has those.
//
// The parameters are the same NVSDK_NGX_Parameter object the game's helper filled in
// (third_party/dlss/include/nvsdk_ngx_helpers_dlssg_d3d.h), so reading them here is
// reading the game's own numbers - nothing is synthesised.
// ---------------------------------------------------------------------------
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_ngx_create)(
    ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result (NVSDK_CONV* pfn_ngx_evaluate)(
    ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);

static pfn_ngx_create   g_real_dlssg_create   = nullptr;
static pfn_ngx_evaluate g_real_dlssg_evaluate = nullptr;

static uint32_t g_dlssg_evals = 0;
static unsigned long long g_dlssg_sig = 0;
static bool g_dlssg_logged_create = false;

static ID3D12Resource* ngx_res(const NVSDK_NGX_Parameter* p, const char* name)
{
    if (!p) { return nullptr; }
    ID3D12Resource* r = nullptr;
    if (p->Get(name, &r) == NVSDK_NGX_Result_Success) { return r; }
    // Integrations that use SetVoidPointer instead of SetD3d12Resource land in the same
    // map entry but come back through the void** overload; try it before giving up.
    void* v = nullptr;
    if (p->Get(name, &v) == NVSDK_NGX_Result_Success) {
        return static_cast<ID3D12Resource*>(v);
    }
    return nullptr;
}

static bool ngx_f(const NVSDK_NGX_Parameter* p, const char* name, float* out)
{
    if (p && p->Get(name, out) == NVSDK_NGX_Result_Success) { return true; }
    *out = 0.0f;
    return false;
}

static bool ngx_ui(const NVSDK_NGX_Parameter* p, const char* name, unsigned* out)
{
    if (p && p->Get(name, out) == NVSDK_NGX_Result_Success) { return true; }
    *out = 0u;
    return false;
}

// The DLSSG matrices travel as void* to 16 floats (NVSDK_NGX_Parameter_SetVoidPointer),
// not as 16 SetF calls - which is why the plain float Get overload finds nothing for
// them and the void** one is required.
static const float* ngx_mat(const NVSDK_NGX_Parameter* p, const char* name)
{
    void* v = nullptr;
    if (p && p->Get(name, &v) == NVSDK_NGX_Result_Success) {
        return static_cast<const float*>(v);
    }
    return nullptr;
}

static void res_desc(ID3D12Resource* r, uint32_t* w, uint32_t* h, uint32_t* fmt)
{
    *w = 0; *h = 0; *fmt = 0;
    if (!r) { return; }
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    *w = static_cast<uint32_t>(d.Width);
    *h = d.Height;
    *fmt = static_cast<uint32_t>(d.Format);
}

static void hook_one(HMODULE m, const char* name, LPVOID detour, LPVOID* original,
                     int* installed, int* missing)
{
    // Never patch the same entry point twice: a second MH_CreateHook on an address that is
    // already trampolined (by us or by ReShade) is how a prologue ends up broken.
    if (original != nullptr && *original != nullptr) { ++*installed; return; }
    LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(m, name));
    if (!addr) { ++*missing; return; }
    if (MH_CreateHook(addr, detour, original) != MH_OK || MH_EnableHook(addr) != MH_OK) {
        mgpu::input::logf("fg: hook %s FAILED\n", name);
        if (original != nullptr) { *original = nullptr; }
        return;
    }
    ++*installed;
}

static void sniff_dlssg(const NVSDK_NGX_Parameter* p, const char* where)
{
    if (!p) { return; }
    const uint32_t n = ++g_dlssg_evals;

    ID3D12Resource* mv      = ngx_res(p, NVSDK_NGX_DLSSG_Parameter_MVecs);
    ID3D12Resource* depth   = ngx_res(p, NVSDK_NGX_DLSSG_Parameter_Depth);
    ID3D12Resource* hudless = ngx_res(p, NVSDK_NGX_DLSSG_Parameter_HUDLess);
    ID3D12Resource* ui      = ngx_res(p, NVSDK_NGX_DLSSG_Parameter_UI);
    ID3D12Resource* ui_a    = ngx_res(p, NVSDK_NGX_DLSSG_Parameter_UIAlpha);
    ID3D12Resource* out     = ngx_res(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolated);
    ID3D12Resource* back    = ngx_res(p, NVSDK_NGX_DLSSG_Parameter_Backbuffer);

    const float* v2c    = ngx_mat(p, NVSDK_NGX_DLSSG_Parameter_CameraViewToClip);
    const float* c2prev = ngx_mat(p, NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip);
    const float* p2c    = ngx_mat(p, NVSDK_NGX_DLSSG_Parameter_PrevClipToClip);

    float jx = 0, jy = 0, msx = 0, msy = 0, zn = 0, zf = 0, fov = 0, aspect = 0;
    unsigned dpinv = 0, cmi = 0, reset = 0, hdr = 0, mf = 0, mfi = 0, mfmax = 0;
    unsigned internal_w = 0, internal_h = 0;
    ngx_f(p, NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, &jx);
    ngx_f(p, NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, &jy);
    ngx_f(p, NVSDK_NGX_DLSSG_Parameter_MvecScaleX, &msx);
    ngx_f(p, NVSDK_NGX_DLSSG_Parameter_MvecScaleY, &msy);
    ngx_f(p, NVSDK_NGX_DLSSG_Parameter_CameraNear, &zn);
    ngx_f(p, NVSDK_NGX_DLSSG_Parameter_CameraFar, &zf);
    ngx_f(p, NVSDK_NGX_DLSSG_Parameter_CameraFOV, &fov);
    ngx_f(p, NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, &aspect);
    ngx_ui(p, NVSDK_NGX_DLSSG_Parameter_DepthInverted, &dpinv);
    ngx_ui(p, NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, &cmi);
    ngx_ui(p, NVSDK_NGX_DLSSG_Parameter_Reset, &reset);
    ngx_ui(p, NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR, &hdr);
    ngx_ui(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, &mf);
    ngx_ui(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, &mfi);
    ngx_ui(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &mfmax);
    ngx_ui(p, NVSDK_NGX_DLSSG_Parameter_InternalWidth, &internal_w);
    ngx_ui(p, NVSDK_NGX_DLSSG_Parameter_InternalHeight, &internal_h);

    // "changed" tracks the declaration and the resource identity, not the jitter and
    // not the per-frame clip matrices.
    unsigned long long sig = 1469598103934665603ull;
    sig = fnv1a(sig, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mv)));
    sig = fnv1a(sig, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(depth)));
    sig = fnv1a(sig, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hudless)));
    sig = fnv1a(sig, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(back)));
    sig = fnv1a(sig, static_cast<unsigned long long>(static_cast<unsigned>(fov * 1e5f)));
    sig = fnv1a(sig, static_cast<unsigned long long>(static_cast<unsigned>(msx * 1e3f)));
    sig = fnv1a(sig, static_cast<unsigned long long>(static_cast<unsigned>(msy * 1e3f)));
    sig = fnv1a(sig, static_cast<unsigned long long>(dpinv | (cmi << 8) | (mf << 16) | (mfi << 24)));
    const bool changed = (sig != g_dlssg_sig);
    g_dlssg_sig = sig;
    if (!want_line(n, changed)) { return; }

    char buf[3072];
    Buf b(buf, sizeof(buf));
    b.addf("fg: dlssg.%s#%u%s internal=%ux%u mf=%u/%u(max=%u)"
           " jit=(%+.3f,%+.3f) mvecScale=(%.4f,%.4f) zn=%.4f zf=%.1f fovY=%.4frad(%.2fdeg)"
           " aspect=%.4f dpi=%u cmi=%u reset=%u hdr=%u\n",
           where, n, changed ? " *CHANGED* " : " ", internal_w, internal_h, mf, mfi, mfmax,
           jx, jy, msx, msy, zn, zf, fov, fov * 57.2957795f, aspect,
           dpinv, cmi, reset, hdr);

    struct R { const char* name; ID3D12Resource* r; };
    const R res[] = { { "mv", mv }, { "depth", depth }, { "hudless", hudless },
                      { "ui", ui }, { "uiAlpha", ui_a }, { "backbuffer", back },
                      { "outInterp", out } };
    for (const auto& e : res) {
        uint32_t w = 0, h = 0, fmt = 0;
        res_desc(e.r, &w, &h, &fmt);
        b.addf("  %-11s %p %ux%u fmt=%u\n", e.name, e.r, w, h, fmt);
    }

    if (want_full(n, changed)) {
        if (v2c)    { matrix_rows(b, "cameraViewToClip", v2c); }
        else        { b.addf("  cameraViewToClip not set by the game\n"); }
        if (c2prev) { matrix_rows(b, "clipToPrevClip", c2prev); }
        else        { b.addf("  clipToPrevClip not set by the game\n"); }
        if (p2c)    { matrix_rows(b, "prevClipToClip", p2c); }
        else        { b.addf("  prevClipToClip not set by the game\n"); }
    }
    emit(buf);
}

// --------------------------------------------------------------------- NGX entry
// PBZ is the case this exists for: no Streamline in the process and no integrated
// XeSS-FG, so its frame generation is an NGX feature evaluated through the same
// `_nvngx.dll` that in_ngx.cpp already hooks. MinHook refuses a second hook on the same
// address, so the sniffer borrows that one: in_ngx calls on_ngx_evaluate() for every
// evaluation and we only act when the parameter block is a frame-generation one.
//
// The discriminator is the parameter itself: DLSS-G uses its own names (DLSSG.MVecs,
// DLSSG.Depth, ...) and never sets the SR ones, so the presence of DLSSG.MVecs is both
// the cheapest and the most reliable test - no handle bookkeeping, no guessing.
static bool is_dlssg_params(const NVSDK_NGX_Parameter* p)
{
    // Motion vectors are a required DLSS-G input, so this is a reliable marker that
    // does not depend on the game filling in the optional camera parameters.
    return ngx_res(p, NVSDK_NGX_DLSSG_Parameter_MVecs) != nullptr;
}

static NVSDK_NGX_Result NVSDK_CONV hook_dlssg_create(
    ID3D12GraphicsCommandList* cmd_list, NVSDK_NGX_Feature feature,
    NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out_handle)
{
    if (!g_dlssg_logged_create) {
        g_dlssg_logged_create = true;
        unsigned w = 0, h = 0, mfmax = 0;
        ngx_ui(params, NVSDK_NGX_Parameter_Width, &w);
        ngx_ui(params, NVSDK_NGX_Parameter_Height, &h);
        ngx_ui(params, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &mfmax);
        mgpu::input::logf("fg: DLSSG CreateFeature feature=%d output=%ux%u MultiFrameCountMax=%u"
                          " (0 here is normal - it is a device-capability parameter; the"
                          " Streamline slDLSSGGetState line is the authoritative one)\n",
                          static_cast<int>(feature), w, h, mfmax);
    }
    return g_real_dlssg_create(cmd_list, feature, params, out_handle);
}

static NVSDK_NGX_Result NVSDK_CONV hook_dlssg_evaluate(
    ID3D12GraphicsCommandList* cmd_list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* params, void* callback)
{
    sniff_dlssg(params, "eval");
    return g_real_dlssg_evaluate(cmd_list, handle, params, callback);
}

static bool install_dlssg_hooks(HMODULE ngx)
{
    int installed = 0, missing = 0;
    // Prefer the prefixed exports; a build that only has the unprefixed ones (or a
    // proxy that re-exports them) is still covered - decided per function, so a runtime
    // that exports only one of them is not left half-hooked.
    hook_one(ngx, "DLSSG_NVSDK_NGX_D3D12_CreateFeature",
             reinterpret_cast<LPVOID>(&hook_dlssg_create),
             reinterpret_cast<LPVOID*>(&g_real_dlssg_create), &installed, &missing);
    if (g_real_dlssg_create == nullptr) {
        hook_one(ngx, "NVSDK_NGX_D3D12_CreateFeature",
                 reinterpret_cast<LPVOID>(&hook_dlssg_create),
                 reinterpret_cast<LPVOID*>(&g_real_dlssg_create), &installed, &missing);
    }
    hook_one(ngx, "DLSSG_NVSDK_NGX_D3D12_EvaluateFeature",
             reinterpret_cast<LPVOID>(&hook_dlssg_evaluate),
             reinterpret_cast<LPVOID*>(&g_real_dlssg_evaluate), &installed, &missing);
    if (g_real_dlssg_evaluate == nullptr) {
        hook_one(ngx, "NVSDK_NGX_D3D12_EvaluateFeature",
                 reinterpret_cast<LPVOID>(&hook_dlssg_evaluate),
                 reinterpret_cast<LPVOID*>(&g_real_dlssg_evaluate), &installed, &missing);
    }
    mgpu::input::logf("fg: DLSSG hooks active (%d installed, %d not exported)."
                      " A missing \"dlssg.eval#1\" line means the game is not generating"
                      " frames through this module.\n", installed, missing);
    return installed != 0;
}

// ---------------------------------------------------------------------------
// 3. XeSS-FG (libxess_fg.dll, the xefgSwapChain* family)
//
// This is the only path where the *game* hands over a view matrix and a projection
// matrix for the frame it is presenting (xefg_swapchain_frame_constant_data_t), so it
// is a direct answer to "what does the integrated FG actually use" - and a ground
// truth for our own synthesized projection.
// ---------------------------------------------------------------------------
typedef xefg_swapchain_result_t (*pfn_xefg_tag_constants)(
    xefg_swapchain_handle_t, uint32_t present_id, const xefg_swapchain_frame_constant_data_t*);
typedef xefg_swapchain_result_t (*pfn_xefg_d3d12_tag_resource)(
    xefg_swapchain_handle_t, ID3D12CommandList*, uint32_t present_id,
    const xefg_swapchain_d3d12_resource_data_t*);
typedef xefg_swapchain_result_t (*pfn_xefg_init_from_swapchain)(
    xefg_swapchain_handle_t, ID3D12CommandQueue*,
    const xefg_swapchain_d3d12_init_params_t*);
typedef xefg_swapchain_result_t (*pfn_xefg_init_from_desc)(
    xefg_swapchain_handle_t, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, ID3D12CommandQueue*, IDXGIFactory2*,
    const xefg_swapchain_d3d12_init_params_t*);

static pfn_xefg_tag_constants       g_real_xefg_tag_constants = nullptr;
static pfn_xefg_d3d12_tag_resource  g_real_xefg_tag_resource  = nullptr;
static pfn_xefg_init_from_swapchain g_real_xefg_init          = nullptr;
static pfn_xefg_init_from_desc      g_real_xefg_init_desc     = nullptr;

static uint32_t g_xefg_const_calls = 0;
static uint32_t g_xefg_res_calls   = 0;
static uint32_t g_xefg_res_seen    = 0;   // bit per resource type already logged
static unsigned long long g_xefg_const_sig = 0;

static const char* xefg_res_name(xefg_swapchain_resource_type_t t)
{
    switch (t) {
    case XEFG_SWAPCHAIN_RES_HUDLESS_COLOR: return "HUDLESS_COLOR";
    case XEFG_SWAPCHAIN_RES_DEPTH:         return "DEPTH";
    case XEFG_SWAPCHAIN_RES_MOTION_VECTOR: return "MOTION_VECTOR";
    case XEFG_SWAPCHAIN_RES_UI:            return "UI";
    case XEFG_SWAPCHAIN_RES_BACKBUFFER:    return "BACKBUFFER";
    default:                               return "OTHER";
    }
}

static xefg_swapchain_result_t hook_xefg_tag_constants(
    xefg_swapchain_handle_t sc, uint32_t present_id,
    const xefg_swapchain_frame_constant_data_t* c)
{
    if (c) {
        const uint32_t n = ++g_xefg_const_calls;
        // The game (or the unlocker) hands XeFG a row-major projection matrix, with the jitter
        // kept separate in jitterOffsetX/Y - exactly the jitter-free projection a consumer
        // needs. This is the camera source for titles whose FG runs outside Streamline (PBZ
        // via the XeSS-FG unlocker: no sl::Constants at all, measured 2026-11-10 04:00).
        if (camera_forwarding()) {
            mgpu::input::publish_fg_camera(c->projectionMatrix, 0.0f, 0.0f, 0.0f,
                                           GetTickCount64());
            static bool once = false;
            if (!once) {
                once = true;
                mgpu::input::logf("fg: camera source = XeFG frame constants (projectionMatrix)\n");
            }
        }
        unsigned long long sig = 1469598103934665603ull;
        sig = fnv1a(sig, static_cast<unsigned long long>(
            static_cast<unsigned>(c->projectionMatrix[0] * 1e5f)));
        sig = fnv1a(sig, static_cast<unsigned long long>(
            static_cast<unsigned>(c->projectionMatrix[5] * 1e5f)));
        sig = fnv1a(sig, static_cast<unsigned long long>(
            static_cast<unsigned>(c->projectionMatrix[10] * 1e5f)));
        sig = fnv1a(sig, static_cast<unsigned long long>(
            static_cast<unsigned>(c->motionVectorScaleX * 1e3f)));
        sig = fnv1a(sig, static_cast<unsigned long long>(
            static_cast<unsigned>(c->motionVectorScaleY * 1e3f)));
        sig = fnv1a(sig, c->resetHistory ? 1u : 0u);
        const bool changed = (sig != g_xefg_const_sig);
        g_xefg_const_sig = sig;
        if (want_line(n, changed)) {
            char buf[2048];
            Buf b(buf, sizeof(buf));
            b.addf("fg: xefg.const #%u%s presentId=%u jit=(%+.3f,%+.3f)px"
                   " mvs=(%.3f,%.3f) reset=%u frameTime=%.3fms\n",
                   n, changed ? " *CHANGED* " : " ", present_id,
                   c->jitterOffsetX, c->jitterOffsetY,
                   c->motionVectorScaleX, c->motionVectorScaleY,
                   c->resetHistory, c->frameRenderTime);
            if (want_full(n, changed)) {
                matrix_rows(b, "viewMatrix", c->viewMatrix);
                matrix_rows(b, "projectionMatrix", c->projectionMatrix);
            }
            emit(buf);
        }
    }
    return g_real_xefg_tag_constants(sc, present_id, c);
}

static xefg_swapchain_result_t hook_xefg_d3d12_tag_resource(
    xefg_swapchain_handle_t sc, ID3D12CommandList* cmd, uint32_t present_id,
    const xefg_swapchain_d3d12_resource_data_t* rd)
{
    if (rd) {
        const uint32_t n = ++g_xefg_res_calls;
        // One call per resource per frame, and a different resource each time, so a
        // "changed" rule would print every call. Instead: the first occurrence of each
        // type always prints (that is the one that answers the resolution question),
        // then the periodic cadence covers later changes.
        const bool first_of_type =
            (static_cast<uint32_t>(rd->type) < 32) &&
            ((g_xefg_res_seen & (1u << static_cast<uint32_t>(rd->type))) == 0);
        if (first_of_type) { g_xefg_res_seen |= (1u << static_cast<uint32_t>(rd->type)); }

        if (first_of_type || want_line(n, false)) {
            char buf[1024];
            Buf b(buf, sizeof(buf));
            b.addf("fg: xefg.res #%u%s presentId=%u %-14s(%u) %p base=(%u,%u) size=(%u,%u)"
                   " incomingState=0x%X validity=%u\n",
                   n, first_of_type ? " *FIRST* " : " ", present_id,
                   xefg_res_name(rd->type), static_cast<unsigned>(rd->type), rd->pResource,
                   rd->resourceBase.x, rd->resourceBase.y,
                   rd->resourceSize.x, rd->resourceSize.y,
                   static_cast<unsigned>(rd->incomingState), static_cast<unsigned>(rd->validity));
            emit(buf);
        }
    }
    return g_real_xefg_tag_resource(sc, cmd, present_id, rd);
}

// The init flags are the game's own statement about its inputs - the same three things
// in_ngx.cpp forwards from DLSS's CreateFlags, so the two can be compared directly.
static void log_xefg_init(const xefg_swapchain_d3d12_init_params_t* p)
{
    if (!p) { return; }
    const uint32_t f = p->initFlags;
    mgpu::input::logf("fg: xefg init: initFlags=0x%X invertedDepth=%d externalHeap=%d"
                      " highResMV=%d ndcVelocity=%d jitteredMV=%d uiNotPremul=%d"
                      " maxInterpolatedFrames=%u uiMode=%d\n",
                      f,
                      (f & XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH) ? 1 : 0,
                      (f & XEFG_SWAPCHAIN_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP) ? 1 : 0,
                      (f & XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV) ? 1 : 0,
                      (f & XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY) ? 1 : 0,
                      (f & XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV) ? 1 : 0,
                      (f & XEFG_SWAPCHAIN_INIT_FLAG_UITEXTURE_NOT_PREMUL_ALPHA) ? 1 : 0,
                      p->maxInterpolatedFrames,
                      static_cast<int>(p->uiMode));
}

static xefg_swapchain_result_t hook_xefg_init_from_swapchain(
    xefg_swapchain_handle_t sc, ID3D12CommandQueue* q,
    const xefg_swapchain_d3d12_init_params_t* p)
{
    log_xefg_init(p);
    return g_real_xefg_init(sc, q, p);
}

static xefg_swapchain_result_t hook_xefg_init_from_desc(
    xefg_swapchain_handle_t sc, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, ID3D12CommandQueue* q,
    IDXGIFactory2* factory, const xefg_swapchain_d3d12_init_params_t* p)
{
    log_xefg_init(p);
    return g_real_xefg_init_desc(sc, hwnd, desc, fs, q, factory, p);
}

static bool install_xefg_hooks(HMODULE fg)
{
    int installed = 0, missing = 0;
    hook_one(fg, "xefgSwapChainTagFrameConstants",
             reinterpret_cast<LPVOID>(&hook_xefg_tag_constants),
             reinterpret_cast<LPVOID*>(&g_real_xefg_tag_constants), &installed, &missing);
    hook_one(fg, "xefgSwapChainD3D12TagFrameResource",
             reinterpret_cast<LPVOID>(&hook_xefg_d3d12_tag_resource),
             reinterpret_cast<LPVOID*>(&g_real_xefg_tag_resource), &installed, &missing);
    hook_one(fg, "xefgSwapChainD3D12InitFromSwapChain",
             reinterpret_cast<LPVOID>(&hook_xefg_init_from_swapchain),
             reinterpret_cast<LPVOID*>(&g_real_xefg_init), &installed, &missing);
    hook_one(fg, "xefgSwapChainD3D12InitFromSwapChainDesc",
             reinterpret_cast<LPVOID>(&hook_xefg_init_from_desc),
             reinterpret_cast<LPVOID*>(&g_real_xefg_init_desc), &installed, &missing);
    mgpu::input::logf("fg: XeSS-FG hooks active (%d installed, %d not exported)."
                      " A missing \"xefg.const#1\" line means the title is not using the"
                      " XeSS-FG swap-chain API.\n", installed, missing);
    return installed != 0;
}

// ---------------------------------------------------------------------------
// Module dispatch
//
// The host hands every successful LoadLibrary* to every source (game_capture.cpp
// notify_sources) and rescans the module list every 120 frames, which is also what
// covers LoaderHook=0. Name matching is case-insensitive and then confirmed by
// GetProcAddress inside each installer, so a title that merely *ships* one of these
// DLLs without calling it produces "0 installed" instead of a fake hook.
// ---------------------------------------------------------------------------
static bool wcs_contains_ci(const wchar_t* s, const wchar_t* needle)
{
    if (!s || !needle || !*needle) { return false; }
    const size_t n = wcslen(needle);
    for (const wchar_t* p = s; *p != L'\0'; ++p) {
        size_t i = 0;
        while (i < n && p[i] != L'\0' && towlower(p[i]) == towlower(needle[i])) { ++i; }
        if (i == n) { return true; }
    }
    return false;
}

// [capture] FgSniff, read once. config_int lives in the host because the ini path
// belongs to it (resolve_ini_path).
static bool sniff_enabled()
{
    static std::atomic<int> state{ -1 };
    int v = state.load(std::memory_order_acquire);
    if (v >= 0) { return v != 0; }
    v = (mgpu::input::config_int("FgSniff", 0) != 0) ? 1 : 0;
    state.store(v, std::memory_order_release);
    return v != 0;
}

// SRWLOCK, not std::mutex: ReShade loads and unloads add-ons repeatedly (PBZ: five times in one
// start-up) and a std::mutex has a *destructor* - once it has run, the object's internal pointer
// is null and the next lock() dereferences it. That is exactly the crash on Wuchang: Fallen
// Feathers 2026-11-10 (minidump: msvcp140!mtx_do_lock -> game_capture+0x2964, on the RHI
// thread). SRWLOCK is a POD initialised by SRWLOCK_INIT: no destructor, nothing to invalidate.
static SRWLOCK g_hook_mutex = SRWLOCK_INIT;

struct HookLock
{
    HookLock()  { AcquireSRWLockExclusive(&g_hook_mutex); }
    ~HookLock() { ReleaseSRWLockExclusive(&g_hook_mutex); }
    HookLock(const HookLock&) = delete;
    HookLock& operator=(const HookLock&) = delete;
};

static std::atomic<bool> g_logged_state{ false };

// Two tables on purpose: modules we hooked, and modules we deliberately ignored (either
// because they are not ours or because the first table filled up). Without the second one a
// full table meant "never recorded", so every 120-frame module rescan re-installed the same
// MinHook trampolines - on functions ReShade 6.8 hooks too (its dxgi.dll contains
// HookStreamline). That double patching is the prime suspect for the 2026-11-10
// loading-screen exits.
static constexpr int kMaxFgModules = 8;
static HMODULE g_fg_modules[kMaxFgModules] = {};
static int     g_fg_module_count = 0;
static HMODULE g_fg_ignored[kMaxFgModules] = {};
static int     g_fg_ignored_count = 0;

static bool module_seen(HMODULE m)
{
    for (int i = 0; i < g_fg_module_count; ++i) {
        if (g_fg_modules[i] == m) { return true; }
    }
    for (int i = 0; i < g_fg_ignored_count; ++i) {
        if (g_fg_ignored[i] == m) { return true; }
    }
    return false;
}

static void module_note(HMODULE m)
{
    if (g_fg_module_count < kMaxFgModules) { g_fg_modules[g_fg_module_count++] = m; }
    else if (g_fg_ignored_count < kMaxFgModules) { g_fg_ignored[g_fg_ignored_count++] = m; }
}

static void log_state_once()
{
    if (g_logged_state.exchange(true)) { return; }
    if (sniff_enabled()) {
        mgpu::input::logf("fg sniffer: ON ([capture] FgSniff=1) - read-only, never publishes;"
                          " DLSS-G is also read through the NGX evaluate hook\n");
    } else if (camera_forwarding()) {
        mgpu::input::logf("fg sniffer: camera only ([capture] StreamlineCamera=1) - Streamline"
                          " constants feed the projection (v12), nothing else is read\n");
    } else {
        mgpu::input::logf("fg sniffer: OFF (set [capture] FgSniff=1 and Log=1 to enable)\n");
    }
}

// Caller holds g_hook_mutex.
static void consider_module(HMODULE m)
{
    // Streamline is hooked for two independent reasons: the log (FgSniff=1) and the camera
    // hand-off that feeds protocol v12 (StreamlineCamera=1, on by default - it is the only way
    // an NGX title gets a real projection). The DLSS-G / XeSS-FG hooks exist only for the log.
    const bool want_log = sniff_enabled();
    const bool want_cam = camera_forwarding();
    if (!m || (!want_log && !want_cam) || module_seen(m)) { return; }

    wchar_t full[MAX_PATH]{};
    if (GetModuleFileNameW(m, full, MAX_PATH) == 0) { return; }
    const wchar_t* base = wcsrchr(full, L'\\');
    base = (base != nullptr) ? base + 1 : full;

    // sl.interposer is the only Streamline DLL worth hooking: the plugins behind it
    // (sl.dlss_g.dll, sl.reflex.dll) receive values the interposer already resolved.
    //
    // Only for the *log* (FgSniff=1). Hooking Streamline is the most invasive thing this add-on
    // does - MinHook patches another vendor's exports while the game is still initialising - and
    // Wuchang: Fallen Feathers (Streamline is under Engine/Plugins/Runtime/Nvidia/Streamline)
    // died right after this on 2026-11-10. The camera therefore comes from XeFG's frame
    // constants only; a title that exposes its projection *solely* through Streamline needs
    // [capture] FgSniff=1.
    if (wcs_contains_ci(base, L"sl.interposer")) {
        if (!want_log) {
            mgpu::input::logf("fg: Streamline found but not hooked (FgSniff=0) - camera comes"
                              " from XeFG frame constants only\n");
            return;
        }
        module_note(m);
        mgpu::input::logf("\nfg sniffer: Streamline  %ls\n", full);
        install_sl_hooks(m);
        return;
    }
    if (!want_log && !want_cam) { return; }
    // XeSS-FG carries a projection matrix in its frame constants, so it is a camera source too
    // (protocol v12) - install for either reason.
    if (wcs_contains_ci(base, L"xess_fg")) {
        module_note(m);
        if (want_log) { mgpu::input::logf("\nfg sniffer: XeSS-FG      %ls\n", full); }
        install_xefg_hooks(m);
        return;
    }
    if (!want_log) { return; }
    // Matches nvngx_dlssg.dll and proxies like dlssg_to_fsr3*.dll (a proxy is fine:
    // it is the module the game actually calls, and we only read).
    if (wcs_contains_ci(base, L"dlssg")) {
        module_note(m);
        mgpu::input::logf("\nfg sniffer: DLSS-G       %ls\n", full);
        install_dlssg_hooks(m);
        return;
    }
}

// ---------------------------------------------------------------------------
// The source itself
//
// This is not an input. `is_input()` returns false, so the host never polls it, never
// pins it, and leaves it out of the "waiting for ngx, ffx, xess" hints; `poll()` also
// returns false as a second line of defence. Nothing it sees reaches the shared block.
// What it does use is on_module()/scan_existing(): the host's only "a module was
// loaded" channel.
// ---------------------------------------------------------------------------
class FgSnifferSource final : public mgpu::input::Source
{
public:
    const char* name() const override { return "fg"; }
    uint32_t api_id() const override { return mgpu::kInputApiNone; }
    bool is_input() const override { return false; }

    void on_module(HMODULE module) override
    {
        log_state_once();
        if (!sniff_enabled() && !camera_forwarding()) { return; }
        HookLock lock;
        consider_module(module);
    }

    void scan_existing() override
    {
        log_state_once();
        if (!sniff_enabled() && !camera_forwarding()) { return; }
        // With the loader hook active we are *told* about every module load, so walking the
        // module list as well is pure risk: CreateToolhelp32Snapshot suspends the other threads
        // and takes the loader lock, and on Wuchang: Fallen Feathers (2026-11-10) the process
        // sat down right here - twice, once during AddonInit and once on the first frame after
        // that was deferred. Only do it when nothing else will tell us (LoaderHook=0).
        if (mgpu::input::config_int("LoaderHook", 1) != 0) {
            mgpu::input::logf("fg: module scan skipped (LoaderHook is on - every load is queued"
                              " and handled outside the loader lock)\n");
            return;
        }
        mgpu::input::logf("fg: scanning already-loaded modules (LoaderHook is off)\n");
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE) { return; }
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        int seen = 0;
        if (Module32FirstW(snap, &me)) {
            do {
                ++seen;
                HookLock lock;
                consider_module(me.hModule);
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        mgpu::input::logf("fg: module scan done (%d modules)\n", seen);
    }

    // Never true: we are a reader, not an input.
    bool poll(mgpu::input::FrameInputs&) override { return false; }
};

static FgSnifferSource g_fg_source;

} // namespace fg_impl

mgpu::input::Source* mgpu::input::make_fg_source()
{
    return &fg_impl::g_fg_source;
}

// Entry point from the NGX source (in_ngx.cpp). Borrowing that hook is the only way to
// see frame generation in a title without Streamline, which is exactly PBZ: its NGX
// comes from the driver shim `_nvngx.dll` and both features are evaluated through it.
// `where` = "ngx" in the log, so the two routes (module hook vs borrowed hook) stay
// distinguishable.
void mgpu::input::on_ngx_evaluate(const void* parameters)
{
    if (!fg_impl::sniff_enabled()) { return; }
    const NVSDK_NGX_Parameter* p = static_cast<const NVSDK_NGX_Parameter*>(parameters);
    if (!fg_impl::is_dlssg_params(p)) { return; }
    fg_impl::sniff_dlssg(p, "ngx");
}







