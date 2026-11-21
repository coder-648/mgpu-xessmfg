// ============================================================================
//  mgpu_fg_api.h - the in-process seam between fakerFG.dll and game_capture.
//
//  New producer architecture (docs/design/FAKER_FG.md):
//
//      fakerFG.dll            the FG pipeline owner  (loaded first, in the game)
//          provider           answers DLSS-G Create/Evaluate/Release
//          harvest            turns the parameter block / tags into a FrameRecord
//          backend            XeFG owns the present path (idle / local / bridge)
//          >>> this header <<<  one record per frame, for the transport side
//      xess_mfg_producer.dll  transport only: copy the record into the shared
//                             slots, publish, done. It never touches FG.
//
//  Why a plain C struct and not a shared-memory block:
//    both modules live in the same process, and the record holds resource
//    pointers that only mean something in this process. Copying the record is
//    free (a few hundred bytes); the *transport* (cross-adapter slots) is still
//    the capture side's job, on its own budget (StreamPercent).
//
//  Lifetime contract (binding):
//    * a record is valid from the Evaluate call that published it until the
//      next Evaluate - the capture must consume it inside that window
//      (MgpuFgAcquire may be called again, it does not destroy it);
//    * resources marked ONLY_NOW may already be reused by the title afterwards,
//      so a consumer that needs them later must copy them now;
//    * `cmd_list` is the open list the title handed to Evaluate. Recording
//      copies on it is how the old tag lane worked (CAPTURE_RULES 4/5), but the
//      record also carries the declared states, so a consumer-owned list is
//      possible later without changing this ABI.
//    * the open-list promise only holds for the Evaluate lane, so it is signalled
//      explicitly with MGPU_FG_FRAME_CMD_OPEN. A record published from the present
//      side (titles that stage tags but never Evaluate, e.g. PBZ) carries the list
//      the title has already closed; recording a Dispatch there is undefined
//      behaviour (commands dropped, NV UMD faults) and the transport must use its
//      own queue list instead.
//
//  Nothing here is a hook. fakerFG publishes; the capture reads. If fakerFG is
//  absent (old deployment), the capture falls back to its own lanes.
// ============================================================================
#pragma once

#include <stdint.h>

// v2: `subscribe`/`unsubscribe`. A poll (`frame_available`/`acquire`) is not enough for a
//     consumer that has to record GPU copies: the record's `cmd_list` is only open during
//     the Evaluate call it was published from. The sink runs synchronously inside that
//     window, on the thread the title evaluated on.
#define MGPU_FG_API_VERSION   2u
#define MGPU_FG_API_EXPORT    "MgpuFgGetApi"
#define MGPU_FG_MODULE_NAME   L"fakerFG.dll"

// The calling convention is only part of the ABI on Windows; keeping it behind
// a macro lets this header be syntax-checked on the build host as well.
#if defined(_WIN32)
#define MGPU_FG_CALL __cdecl
#else
#define MGPU_FG_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- modes
// Who generates the interpolated frames. fakerFG's decision only; the capture
// has no say and no way to change it - that is the point of the refactor.
typedef enum MgpuFgMode
{
    MGPU_FG_MODE_OFF = 0,   // we are not the provider; the title's runtime runs
    MGPU_FG_MODE_OBSERVE,   // we answer, then forward to the real runtime
    MGPU_FG_MODE_IDLE,      // we answer, generate nothing, present the real frame
    MGPU_FG_MODE_LOCAL,     // we answer, XeFG generates on this GPU
    MGPU_FG_MODE_BRIDGE     // we answer, the second GPU generates, we present
} MgpuFgMode;

// Same vocabulary as the protocol (src/mgpu_shared.h kInputFlag*) so the
// capture copies bits through without a translation table.
typedef enum MgpuFgFrameFlag
{
    MGPU_FG_FRAME_DEPTH_INVERTED = 1u << 0,
    MGPU_FG_FRAME_MV_NDC         = 1u << 1,
    MGPU_FG_FRAME_MV_JITTERED    = 1u << 2,
    MGPU_FG_FRAME_HI_RES_MV      = 1u << 3,
    MGPU_FG_FRAME_HAS_CAMERA     = 1u << 4,
    MGPU_FG_FRAME_HDR            = 1u << 5,
    MGPU_FG_FRAME_MV_ACTIVE      = 1u << 6,
    MGPU_FG_FRAME_HAS_MATRIX     = 1u << 7,
    MGPU_FG_FRAME_HAS_PREV_CLIP  = 1u << 8,
    MGPU_FG_FRAME_RESET_HISTORY  = 1u << 9,
    MGPU_FG_FRAME_MV_DILATED     = 1u << 10,
    MGPU_FG_FRAME_MV_3D          = 1u << 11,
    MGPU_FG_FRAME_CAMERA_MOTION  = 1u << 12,
    MGPU_FG_FRAME_FROM_FG        = 1u << 13,
    // `cmd_list` is still the title's *open* recording list, so the capture may append its
    // own passes to it (Evaluate lane). NOT set by a record published from the present
    // side: there `cmd_list` is the list the title has already closed and executed.
    // Bits 14/15 are the shared protocol's kInputFlagMvSuspect / kInputFlagColourActive,
    // and the transport forwards `flags` into ProducerInfo.input_flags, so this one lives
    // above them.
    MGPU_FG_FRAME_CMD_OPEN       = 1u << 16
} MgpuFgFrameFlag;

// How long a resource stays usable, in the XeFG sense; the capture only needs
// to know whether it must copy inside this call.
typedef enum MgpuFgValidity
{
    MGPU_FG_RV_ONLY_NOW = 0,
    MGPU_FG_RV_UNTIL_PRESENT,
    MGPU_FG_RV_VALID_BUT_MAKE_COPY,
    MGPU_FG_RV_VALID_UNTIL_PRESENT_FROM_DISPATCH
} MgpuFgValidity;

// Declared state of one resource. fakerFG *is* the FG pass, so these states are
// declarations we own (CAPTURE_RULES rule 4); the capture uses them verbatim.
typedef enum MgpuFgStream
{
    MGPU_FG_STREAM_MV = 0,
    MGPU_FG_STREAM_DEPTH,
    MGPU_FG_STREAM_HUDLESS,
    MGPU_FG_STREAM_UI_COLOR,
    MGPU_FG_STREAM_UI_ALPHA,
    MGPU_FG_STREAM_BACKBUFFER,
    MGPU_FG_STREAM_OUTPUT_REAL,
    MGPU_FG_STREAM_OUTPUT_INTERP,
    MGPU_FG_STREAM_COUNT
} MgpuFgStream;

typedef struct MgpuFgResource
{
    void*        resource;      // ID3D12Resource*, borrowed - do not Release
    uint32_t     state;         // D3D12_RESOURCE_STATES, declared by fakerFG
    uint32_t     validity;      // MgpuFgValidity
    uint32_t     width;
    uint32_t     height;
    uint32_t     format;        // DXGI_FORMAT, 0 when the stream is absent
    uint32_t     _pad;
} MgpuFgResource;

// ---------------------------------------------------------------- one frame
// `size` + `version` are first so a newer fakerFG can extend the struct and an
// older capture still reads the fields it knows.
typedef struct MgpuFgFrame
{
    uint32_t       size;
    uint32_t       version;

    uint64_t       seq;              // monotonic, one per Evaluate
    uint64_t       present_id;       // the id XeFG pairs this frame with
    uint64_t       frame_qpc_ms;     // when the title called Evaluate
    void*          cmd_list;         // ID3D12GraphicsCommandList*, open, borrowed
    void*          device;           // ID3D12Device*, borrowed

    MgpuFgResource stream[MGPU_FG_STREAM_COUNT];

    // Views. `render_*` is the extent MV/depth are declared at; `output_*` is
    // what the presenter shows. Both come from the title's own declaration.
    uint32_t       render_w, render_h;
    uint32_t       output_w, output_h;
    uint32_t       declared_frames;  // numFramesToGenerate the title asked for
    uint32_t       mode;             // MgpuFgMode at the time of this frame
    uint32_t       flags;            // MgpuFgFrameFlag bits
    uint32_t       generation;       // ++ on resize / backend restart

    float          jitter_x, jitter_y;
    float          mv_scale_x, mv_scale_y;
    float          camera_near, camera_far, camera_fov_y, aspect;

    // Row-major, the same convention as protocol v13 `view_to_clip`.
    float          view_to_clip[16];
    float          clip_to_prev_clip[16];
    float          prev_clip_to_clip[16];
} MgpuFgFrame;

// ---------------------------------------------------------------- the table
// Function pointers instead of an import library: the module is deployed under
// a proxy name (dxgi.dll / nvngx_dlssg.dll / ...), so there is nothing to link
// against and no stable file name to import from.
//
// v2 sink: invoked synchronously from publish_record(), on the thread that produced the
// record, while the record's command list is still open (see the field comment).
typedef void (MGPU_FG_CALL* MgpuFgFrameSink)(const MgpuFgFrame* frame, void* user);

typedef struct MgpuFgApi
{
    uint32_t       size;
    uint32_t       version;

    // What the backend is doing right now (updated live, cheap).
    uint32_t     (*mode)(void);
    // 1 when a frame newer than the last `acquire` exists. Never blocks.
    int          (*frame_available)(void);
    // Copy the current record. Returns 0 on success. Safe to call from the
    // Evaluate thread only; returns non-zero if called from another thread.
    int          (*acquire)(MgpuFgFrame* out);
    // Forget the record (the capture does this after publishing, so a stall in
    // the capture cannot make the next Evaluate re-publish stale resources).
    void         (*release)(void);

    // One log line into fakerFG's log, so a session has a single timeline.
    // Optional: a capture that logs on its own is still correct.
    void         (*log)(const char* line);

    // Reserved for a fakerFG-owned transport block. 0 today; the capture uses
    // its own block (mgpu_shared.h v14).
    void*        (*shared_block)(uint32_t* out_version);

    // v2: synchronous per-frame sink. Called from publish_record() - i.e. on the
    // thread and at the moment the record was produced, while `frame->cmd_list`
    // is still open. `frame` is a private copy owned by fakerFG and is only
    // valid for the duration of the call. Returns 0 on success.
    int          (*subscribe)(MgpuFgFrameSink sink, void* user);
    void         (*unsubscribe)(void);
} MgpuFgApi;

typedef const MgpuFgApi* (MGPU_FG_CALL* MgpuFgGetApiFn)(uint32_t min_version,
                                                       uint32_t max_version);

#ifdef __cplusplus
} // extern "C"
#endif

// ---------------------------------------------------------------- attach
// The capture side includes this header with MGPU_FG_API_IMPLEMENT before its
// first use, which defines one static helper. Header-only on purpose: the
// add-on is a single unity TU, and a test module that includes this file must
// not create a link dependency on fakerFG.
#ifdef MGPU_FG_API_IMPLEMENT

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// The module may be deployed under any proxy name (OptiScaler supports dxgi /
// d3d12 / winmm / dbghelp / dinput8 / version), so the *export* identifies it -
// never the file name.
static inline const MgpuFgApi* mgpu_fg_attach(void)
{
    static const wchar_t* kNames[] = {
        MGPU_FG_MODULE_NAME, L"dxgi.dll", L"nvngx_dlssg.dll", L"nvngx.dll",
        L"d3d12.dll", L"winmm.dll", L"dbghelp.dll", L"dinput8.dll", L"version.dll"
    };
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        HMODULE m = GetModuleHandleW(kNames[i]);
        if (m == nullptr) { continue; }
        MgpuFgGetApiFn get = (MgpuFgGetApiFn)(void*)GetProcAddress(m, MGPU_FG_API_EXPORT);
        if (get == nullptr) { continue; }
        const MgpuFgApi* api = get(MGPU_FG_API_VERSION, MGPU_FG_API_VERSION);
        if (api != nullptr && api->size >= sizeof(MgpuFgApi) &&
            api->acquire != nullptr && api->frame_available != nullptr) {
            return api;
        }
    }
    return nullptr;
}

#endif // MGPU_FG_API_IMPLEMENT
