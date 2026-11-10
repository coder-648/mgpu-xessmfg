// ============================================================================
// inputs.h - "where did this frame's upscaler inputs come from?"
//
// The add-on used to be NGX-only by construction: the hook, the stream sizing and
// the per-frame loop all read NVSDK_NGX_Parameter directly, which pinned the whole
// pipeline to an NVIDIA card running DLSS. This header is the seam that fixes that
// (docs/design/VENDOR_COMPAT_PLAN.md M6.3): a Source answers "which API fed us" and "what
// are this frame's MV/depth/jitter/camera", and the host (game_capture.cpp) only
// ever looks at FrameInputs.
//
// Nothing here is vendor specific. A source is selected by which upscaler DLL the
// game actually loaded (ngx / ffx / xess), not by which GPU it runs on - the same
// A card can run any of them.
// ============================================================================
#pragma once

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <string>

#include "reshade.hpp"
#include "mgpu_shared.h"

namespace mgpu::input {

// ---------------------------------------------------------- normalized semantics
// Every source declares its inputs in its own vocabulary (DLSS feature flags, FSR upscale
// flags, XeSS init flags). The consumer must never branch on the vendor, so the sources
// translate their declarations into the enums below and leave `unknown` when the API does
// not say. `unknown` is not a failure: the online probe (game_capture.cpp `probe:`)
// measures what is missing and logs the answer, and the two are cross-checked.
enum class MvUnit : uint8_t
{
    unknown = 0,   // the API said nothing (or something ambiguous) - the probe decides
    pixels,        // value * 1 = screen-space pixels
    uv,            // value * 1 = screen UV        (multiply by the render size for px)
    ndc            // value * 1 = NDC               (multiply by half the size for px)
};

enum class MvSign : uint8_t
{
    unknown = 0,
    prev_minus_cur,   // what XeFG / FSR style vectors use
    cur_minus_prev
};

enum class CameraSource : uint8_t
{
    none = 0,      // nothing anywhere: the consumer synthesises one
    declared,      // the API exported near/far/vfov (FSR 2.2 / 3.x does)
    matrix         // a real view-to-clip matrix was captured (Streamline / DLSS-G / XeFG)
};

inline const char* mv_unit_name(MvUnit u)
{
    switch (u) {
    case MvUnit::pixels: return "pixels";
    case MvUnit::uv:     return "uv";
    case MvUnit::ndc:    return "ndc";
    default:             return "unknown";
    }
}

inline const char* mv_sign_name(MvSign s)
{
    switch (s) {
    case MvSign::prev_minus_cur: return "prev-cur";
    case MvSign::cur_minus_prev: return "cur-prev";
    default:                     return "unknown";
    }
}

inline const char* camera_source_name(CameraSource c)
{
    switch (c) {
    case CameraSource::declared: return "declared";
    case CameraSource::matrix:   return "matrix";
    default:                     return "none";
    }
}

// One frame of upscaler inputs. Field meaning is fixed; only the producer changes.
struct FrameInputs
{
    ID3D12Resource* mv = nullptr;      // may be null (SkipMv=1, or an API without MV)
    ID3D12Resource* depth = nullptr;   // required
    uint32_t render_w = 0, render_h = 0;   // upscaler input size (= MV/depth size)
    uint32_t output_w = 0, output_h = 0;   // upscaler output size (host fills it)
    float jitter_x = 0.0f, jitter_y = 0.0f;
    float mv_scale_x = 1.0f, mv_scale_y = 1.0f;
    float near_plane = 0.0f, far_plane = 0.0f, vfov_rad = 0.0f;
    uint32_t flags = 0;                // mgpu::kInputFlag*

    // ---- normalized semantics (step 1 of docs/findings/INPUT_SEMANTICS.md) ----
    // Filled by the source wherever its API actually declares the fact, `unknown`/`none`
    // otherwise. Not part of the shared-memory protocol yet: the host logs it and the
    // online probe cross-checks the unit, which is how we stop guessing per title.
    MvUnit       mv_unit = MvUnit::unknown;
    MvSign       mv_sign = MvSign::unknown;
    CameraSource camera_source = CameraSource::none;
    float        invalid_marker = 0.0f;     // 0 = not declared (PBZ's SL path: FLT_MAX)
    uint32_t     valid_w = 0, valid_h = 0;  // valid sub-rect of the mv/depth textures
    // A real, jitter-free view-to-clip matrix (row major, sl::Constants layout). Set with
    // kInputFlagHasMatrix by the host when the diagnostic Streamline sniffer captured one and
    // the input source itself has no camera (DLSS-SR exports none). The consumer then uses
    // the game's own projection instead of synthesising one from --fg-fov.
    float        view_to_clip[16] = {};

    uint64_t serial = 0;               // advances per captured frame
    // GetTickCount64() when the API call that produced this frame happened. The host uses
    // it to tell a *live* source from one that is only replaying its last buffered frame:
    // after an upscaler switch the old source stops being called, but poll() still returns
    // its last frame once, and pinning that source means no data at all from then on
    // (measured: switching DLSS->FSR->XeSS pinned the silent ngx source and the whole
    // session published nothing).
    uint64_t tick_ms = 0;
};

// ---------------------------------------------------------------- host services
// A source reports raw ID3D12Resource pointers from the game. The host owns what
// happens next: the state machine (we must know the resource's current state to emit
// a legal transition), the deferred release (D3D12 does not keep a resource alive
// just because a recorded command list references it) and the log.

void logf(const char* fmt, ...);

// MinHook has to be initialised once, by the host. Hook installs from a source go
// through this so the loader hooks and the upscaler hooks share one init.
bool ensure_minhook();

// Read a key from the add-on's own ini ([capture] <key>), default when missing.
// A source cannot open that file itself: the path is resolved by the host next to the
// add-on module (with the %LOCALAPPDATA%\mgpu fallback), see resolve_ini_path().
int config_int(const char* key, int def);

// Start watching these two resources; the states are the expected ones *before* the
// barrier tracker has seen anything (they are replaced by every observed barrier).
// Only a changed pointer resets the guess, exactly like the old NGX-only code.
void track(ID3D12Resource* mv, ID3D12Resource* depth,
           reshade::api::resource_usage mv_state,
           reshade::api::resource_usage depth_state);

// Keep the resource alive for a few frames (deferred release).
void hold(ID3D12Resource* res);

// The diagnostic Streamline source (in_fg.cpp) runs on the game's thread and is not an input;
// these two are how its camera reaches the host without making the sniffer an input source.
// publish_fg_camera() is called from the snConstants hook, fg_camera() from the frame path.
// A torn read is prevented by the sequence counter; tick_ms lets the host reject a camera
// older than the frame it is about to publish.
void publish_fg_camera(const float view_to_clip[16], float near_z, float far_z,
                       float vfov_rad, uint64_t tick_ms);
bool fg_camera(float out16[16], float* near_z, float* far_z, float* vfov_rad, uint64_t* tick_ms);

// --------------------------------------------------------------------- Source
class Source
{
public:
    virtual ~Source() = default;

    virtual const char* name() const = 0;      // "ngx" / "ffx" / "xess"
    virtual uint32_t api_id() const = 0;       // mgpu::kInputApi*

    // false = diagnostic only: the host never polls it, never pins it and never lists
    // it in the "waiting for ..." hints. in_fg.cpp uses this: it wants the module-load
    // notifications, not to feed the bridge.
    virtual bool is_input() const { return true; }

    // Every successful LoadLibrary* lands here; the source decides by module path.
    virtual void on_module(HMODULE module) = 0;

    // Modules that were already loaded when the add-on got injected.
    virtual void scan_existing() {}

    // Once per frame. false = this source has nothing new (not "no frame ever").
    virtual bool poll(FrameInputs& out) = 0;
};

// One factory per API. M6.4 adds make_ffx_source(), M6.5 make_xess_source().
Source* make_ngx_source();
Source* make_ffx_source();
Source* make_xess_source();
// Not an input: the game's own frame generation (Streamline / NGX DLSS-G / XeSS-FG),
// read-only. poll() always returns false, so it can never be pinned; it exists for the
// module-load notifications and logs what the FG path is handed. See in_fg.cpp and
// [capture] FgSniff.
Source* make_fg_source();

// Called by the NGX source for every EvaluateFeature, before the call is forwarded.
// Some titles have no Streamline and no integrated XeSS-FG: their frame generation is
// an NGX feature evaluated through the *same* module the upscaler source already hooks
// (PBZ: NGX comes from the driver shim `_nvngx.dll`). A second MinHook on that address
// is refused, so the FG sniffer borrows the hook that already exists - it looks for the
// DLSSG.* parameters in this block and does nothing when they are absent.
// `parameters` is a `const NVSDK_NGX_Parameter*`, kept as void* to keep this header
// vendor free. A no-op unless [capture] FgSniff=1.
void on_ngx_evaluate(const void* parameters);

} // namespace mgpu::input
