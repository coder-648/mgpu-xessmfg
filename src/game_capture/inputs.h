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

// Start watching these two resources; the states are the expected ones *before* the
// barrier tracker has seen anything (they are replaced by every observed barrier).
// Only a changed pointer resets the guess, exactly like the old NGX-only code.
void track(ID3D12Resource* mv, ID3D12Resource* depth,
           reshade::api::resource_usage mv_state,
           reshade::api::resource_usage depth_state);

// Keep the resource alive for a few frames (deferred release).
void hold(ID3D12Resource* res);

// --------------------------------------------------------------------- Source
class Source
{
public:
    virtual ~Source() = default;

    virtual const char* name() const = 0;      // "ngx" / "ffx" / "xess"
    virtual uint32_t api_id() const = 0;       // mgpu::kInputApi*

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

} // namespace mgpu::input
