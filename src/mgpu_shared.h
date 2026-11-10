// ============================================================================
// mgpu_shared.h  -  layout of the shared memory block used between the game
//                    process (producer) and the bridge process (consumer)
//
// Why two processes at all: ReShade's D3D12 hooks are process-global. Creating a
// second-vendor device inside the game process deadlocks (measured, see
// docs/findings/milestones/M3.2_RESULTS.md). Splitting them also means a crash on the AMD side can
// never take the game down.
//
// The block lives at offset 0 of a NAMED file mapping. Both processes wrap it as
// a D3D12 heap with ID3D12Device3::OpenExistingHeapFromFileMapping and place
// their own views of the same textures on top of it - no copies between the two
// processes, only the GPU writes and reads.
//
// Fence values stored here are NT handle values valid inside the PRODUCER
// process. The consumer duplicates them with DuplicateHandle(OpenProcess(...)).
// ============================================================================
#pragma once

#include <cstdint>

namespace mgpu {

inline constexpr uint32_t kSharedMagic   = 0x55475047u;   // 'GPUM'
// Bump whenever SharedControl changes shape. Both sides refuse to talk on a
// mismatch instead of silently reading garbage (a stale add-on paired with a new
// bridge used to do exactly that).
//
// v8: SharedControl gains `generation`. The producer now *rebuilds* the stream
//     layout when the game changes its render resolution (upscaler preset or mode
//     switch) instead of keeping the first size for the whole session: it bumps
//     `generation`, publishes `magic = 0`, re-places the slots and republishes.
//     The consumer notices the change, drops its placed views and re-places them on
//     the *same* mapping/heap/fences - no reopen, no restart.
// v7: SharedControl gains `consumer_request`, the one channel that goes the other
//     way. The bridge asks for the motion-vector stream only when it will actually
//     tag it (the UI exposes that choice), so the add-on stops converting and
//     copying 14 MB/frame just in case. ProducerInfo.input_flags gains
//     kInputFlagMvActive so the consumer can tell whether it really has vectors.
// v6: SharedControl gains `producer` (which adapter renders, which upscaler fed us,
//     what the camera was) and FrameShared gains the per-frame camera + the game's
//     own present interval. Both let the consumer stop guessing: it picks the *other*
//     GPU instead of "the AMD one" and can build a real projection. See
//     docs/design/VENDOR_COMPAT_PLAN.md M6.2.
// v5: FrameShared gains `ready`, published by the producer once its GPU work for that
//     slot has actually completed (cross-adapter fences are unreadable on the consumer,
//     so the consumer had no way to know - docs/findings/HANDOFF.md 8.6).
// v4: kMaxSlots 2 -> 4, so the bridge can hold one or two captured frames
//     (--slack) without the producer overwriting the MV/depth slot that those
//     frames still have to be paired with.
// v9: **decoupling.** The stream slots are placed once at kStreamMaxW/H and never move;
//     a resolution change only rewrites StreamShared.width/height (the *content* size
//     inside those slots) + bumps `generation`. There is no handshake left: the producer
//     never invalidates the mapping, never waits for the consumer, and the consumer read
//     path is `magic ? copy(streams[i].width x height from streams[i].offset) : pause`.
//     `consumer_request` (v7) stays a one-way hint the producer may ignore.
// v10: **消息队列语义补齐**（P1 + P2，见 docs/design/VENDOR_COMPAT_PLAN.md M6.4.4）。
//      (a) `consumed[]`：消费端 ACK "我已经读完槽 k 的第 N 次发布"，生产端复用槽位前必须
//          看到它 —— 槽位覆盖从"4 槽碰运气"变成有保证；顺带能算出真实队列深度。
//      (b) `FrameShared.published`：该槽最近一次发布的 fence 值，消费端据此回 ACK（它不看
//          跨卡 fence，那在上面读不出来，HANDOFF 8.6）。
//      (c) `StreamShared.seq`：元数据 seqlock（奇=正在写），替掉消费端"读两次碰运气 + clamp"。
// v11: FrameShared gains the producer's *measured* semantics (FrameShared.semantics +
//      valid_w/h): the online probe's verdict on MV unit / MV sign / depth direction, so
//      the consumer stops guessing when an API declaration is missing or wrong
//      (docs/findings/INPUT_SEMANTICS.md). Appended at the end - offsets up to v10 are
//      unchanged.
// v12: FrameShared gains `view_to_clip` + `matrix_tick_ms`: the game's own (jitter-free)
//      view-to-clip matrix from Streamline's sl::Constants, so the consumer stops
//      synthesising a projection from an FOV guess (docs/findings/INPUT_SEMANTICS.md).
//      Appended at the end - offsets up to v11 are unchanged.
inline constexpr uint32_t kSharedVersion = 12;

// The mapping is created once, at a size that fits the largest stream we support
// (see kStreamMaxW/H), so a layout rebuild never has to recreate it - that would
// break the consumer's handles.
inline constexpr uint32_t kStreamMaxW = 3840;
inline constexpr uint32_t kStreamMaxH = 2160;

// ---------------------------------------------------------------- input source
// Which upscaler handed us the frame. The add-on used to only ever hook NGX (DLSS),
// which pinned the whole pipeline to an NVIDIA card running DLSS; these ids are what
// the per-vendor input sources report (docs/design/VENDOR_COMPAT_PLAN.md M6.3..M6.5).
// Nothing here is vendor specific: the same API can be used on any card.
inline constexpr uint32_t kInputApiNone  = 0;
inline constexpr uint32_t kInputApiNgx   = 1;   // DLSS-SR (NVSDK_NGX_EvaluateFeature)
inline constexpr uint32_t kInputApiFfx   = 2;   // FSR 2.2 / 3.x (ffxDispatch)
inline constexpr uint32_t kInputApiXess  = 3;   // XeSS (xessD3D12Execute)
inline constexpr uint32_t kInputApiDepth = 4;   // ReShade depth only (no upscaler)

// What the frame's motion vectors / depth / camera actually are.
inline constexpr uint32_t kInputFlagInvertedDepth = 1u << 0;  // reversed/infinite Z
inline constexpr uint32_t kInputFlagMvNdc         = 1u << 1;  // MV in NDC, not UV
inline constexpr uint32_t kInputFlagMvJittered    = 1u << 2;  // game jitter still in MV
inline constexpr uint32_t kInputFlagHiResMv       = 1u << 3;  // MV at output resolution
inline constexpr uint32_t kInputFlagHasCamera     = 1u << 4;  // near/far/vfov are valid
inline constexpr uint32_t kInputFlagHdr           = 1u << 5;
inline constexpr uint32_t kInputFlagMvActive      = 1u << 6;  // the MV stream is being fed
// v12: FrameShared carries a real view-to-clip matrix (Streamline sl::Constants). With it the
// consumer uses the game's own projection instead of synthesising one from --fg-fov.
inline constexpr uint32_t kInputFlagHasMatrix     = 1u << 7;

// consumer -> producer requests (SharedControl.consumer_request)
inline constexpr uint32_t kRequestMv = 1u << 0;

// ---- v11: measured semantics (FrameShared.semantics) --------------------------------
// Packed so the whole verdict fits in one word. `Measured` tells the consumer whether the
// field came from the online probe or is still the producer's declaration/unknown.
inline constexpr uint32_t kSemMvUnitShift   = 0;   // 0 unknown, 1 pixels, 2 uv, 3 ndc
inline constexpr uint32_t kSemMvSignShift   = 2;   // 0 unknown, 1 prev-cur, 2 cur-prev
inline constexpr uint32_t kSemDepthShift    = 4;   // 0 unknown, 1 reversed, 2 forward
inline constexpr uint32_t kSemMvQualityShift = 8;  // 8 bits, quality * 255
inline constexpr uint32_t kSemMvMeasured    = 1u << 16;
inline constexpr uint32_t kSemSignMeasured  = 1u << 17;
inline constexpr uint32_t kSemDepthMeasured = 1u << 18;
// Was the MV field usable at all? Low = the residual test found the vectors do not align,
// which is the signal to fall back to our own optical flow (--fg-of).
inline constexpr uint32_t kSemMvUsable      = 1u << 19;
// The quality byte above is a real measurement (the sign test ran). Without this bit the
// consumer must print "unknown", not "quality 0" - a missing measurement is not a bad one.
inline constexpr uint32_t kSemQualityKnown  = 1u << 20;

// LUIDs travel as one uint64 so this header stays windows.h free.
inline uint64_t pack_luid(int32_t high, uint32_t low)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(high)) << 32) | low;
}
inline void unpack_luid(uint64_t v, int32_t* high, uint32_t* low)
{
    if (high) { *high = static_cast<int32_t>(static_cast<uint32_t>(v >> 32)); }
    if (low)  { *low  = static_cast<uint32_t>(v & 0xFFFFFFFFull); }
}

// Written once per process, right before `magic`. Changes as the game changes mode
// (render/output size follows the DLSS/FSR preset), so the consumer re-reads it.
struct ProducerInfo
{
    uint32_t input_api;      // kInputApi*
    uint32_t input_flags;    // kInputFlag*
    uint32_t render_w;       // upscaler input size (= MV/depth size)
    uint32_t render_h;
    uint32_t output_w;       // upscaler output size (the presented frame)
    uint32_t output_h;
    uint64_t device_luid;    // ID3D12Device::GetAdapterLuid() of the game's device
};

inline constexpr uint32_t kMaxStreams = 4;
inline constexpr uint32_t kMaxSlots   = 4;

// Fixed layout, written once by the producer before magic is set.
struct StreamShared
{
    // Content size: how much of each slot's first slot_w x slot_h pixels is live. This
    // is the ONLY thing that changes when the game switches DLSS/FSR/XeSS presets.
    uint32_t width;
    uint32_t height;
    uint32_t format;        // DXGI_FORMAT
    uint32_t _pad;
    uint64_t offset;        // byte offset of slot 0 inside the mapping
    uint64_t slot_size;     // bytes per slot (64 KB aligned)
    // v9: the placed resource is always this big; the live content sits in its top-left
    // corner. Both sides must place with these dims - the row pitch comes from here, so
    // a consumer that placed width x height instead would read a different layout.
    uint32_t slot_w;
    uint32_t slot_h;
    // v10: seqlock over (width, height). The producer bumps it to odd while it writes and
    // back to even when the pair is consistent; the consumer retries while it is odd or
    // changed underneath it, so it can never mix an old width with a new height.
    uint32_t seq;
    uint32_t _pad_v10;
};

// Per-slot frame record, written by the producer just before it signals the
// slot's fence. The bridge needs the jitter (and the frame id, for pairing
// against WGC's one-frame-late colour).
struct FrameShared
{
    uint64_t frame_id;
    float    jitter_x;
    float    jitter_y;
    uint32_t render_w;
    uint32_t render_h;
    // Motion-vector scale the game itself hands to NGX (MV.Scale.X/Y). Multiplying a
    // motion vector by these gives the screen-space UV delta, which is what our own
    // interpolator needs. Games differ here, so it cannot be assumed.
    float    mv_scale_x;
    float    mv_scale_y;
    // Set by the producer (CPU) once it has *seen* the GPU finish the copies for this
    // slot, i.e. after polling its own forward fence. 0 = not published yet. The
    // consumer cannot read the fence itself: cross-adapter fences report UINT64_MAX
    // on the other node, which made every "wait" a no-op.
    uint64_t ready;
    // ---- v6 ----
    // Camera of the frame (0 when the source has none, e.g. DLSS-SR). FSR 3.1 exports
    // near/far/vfov, which lets the consumer build a real projection instead of the
    // --fg-fov guess. Only valid when ProducerInfo.input_flags has kInputFlagHasCamera.
    float    near_plane;
    float    far_plane;
    float    vfov_rad;
    // The game's own present interval in ms. The bridge feeds frameRenderTime from its
    // own capture cadence (docs/findings/FINAL_CONFIG.md 5.4); this is the second opinion.
    float    frame_time_ms;
    // ---- v10 ----
    // Forward-fence value of this publication, written before the signal. The consumer
    // acknowledges with it (see SharedControl.consumed): cross-adapter fences cannot be
    // queried from the consuming node, so this header field is the only way it can name
    // "which publication I am done with".
    uint64_t published;
    // ---- v11 ----
    // What the producer *measured* about its own inputs, as opposed to what the API
    // declared (that stays in ProducerInfo.input_flags). The add-on's online probe fills
    // these every ~2 s; `kSem*Measured` says which of them are real measurements.
    // See docs/findings/INPUT_SEMANTICS.md.
    uint32_t semantics;      // kSem* bits
    uint32_t valid_w;        // valid sub-rect of the MV/depth textures (0 = whole texture)
    uint32_t valid_h;
    uint32_t _pad_v11;
    // ---- v12 ----
    // The game's own view-to-clip matrix, captured from Streamline's sl::Constants (which
    // is documented as jitter-free). With it the consumer stops *synthesising* a projection
    // from an FOV guess: the real aspect, the infinite-far form and reversed-Z are all in
    // here. Row major, same layout as sl::Constants::cameraViewToClip. Only meaningful when
    // ProducerInfo.input_flags has kInputFlagHasMatrix; `matrix_tick_ms` is when the
    // producer captured it, so a stale matrix (the game switched to another API) is
    // detectable the same way a stale source is.
    float    view_to_clip[16];
    uint64_t matrix_tick_ms;
};

struct SharedControl
{
    uint32_t magic;
    uint32_t version;
    uint32_t producer_pid;
    uint32_t slots;
    uint32_t stream_count;
    uint32_t _pad0;
    uint64_t mapping_size;
    uint64_t frame_published;      // diagnostics only, written by the producer
    StreamShared streams[kMaxStreams];
    uint64_t fence_forward[kMaxSlots];   // producer -> consumer, NT handle values
    uint64_t fence_back[kMaxSlots];      // consumer -> producer
    // Per-slot frame data the bridge needs to pair colour with MV/depth.
    FrameShared frames[kMaxSlots];
    // v6: appended at the end (existing field offsets unchanged). Who the producer is
    // and what its inputs mean - see ProducerInfo above.
    ProducerInfo producer;
    // v7: the consumer's ask. Written by the bridge (FILE_MAP_ALL_ACCESS), read by the
    // add-on once per captured frame. 0 = the bridge will not tag motion vectors, so
    // producing them is wasted bandwidth. One-way hint: the producer must never block on
    // it, and the bridge is free to ignore whatever the producer sends (v9).
    uint32_t consumer_request;
    // v8/v9: bumped by the producer whenever the *content* size changes. Informational
    // only - nothing is re-placed and nobody waits, so the consumer just notices it and
    // reports the new size. `magic` is only 0 while the mapping is being (re)created.
    uint32_t generation;
    // v9: was `consumer_ack` in v8 (tear-down handshake). Unused now: the producer never
    // recycles slot memory, so there is nothing to acknowledge. Kept so the struct stays
    // 8-byte friendly.
    uint32_t _reserved_ack;
    // ---- v10: credit window (P1) ----
    // Per-slot ACK: the forward-fence value the consumer has finished reading. The
    // producer may not overwrite slot k until consumed[k] >= the value it last published
    // there. 0 = nothing consumed yet (a fresh consumer, or none at all - the producer
    // then falls back to its old round-robin behaviour after a grace period).
    uint64_t consumed[kMaxSlots];
    // Producer diagnostics, so "were we ever starved?" stops being a guess.
    uint32_t producer_overrun;   // publications dropped: no free slot
    uint32_t producer_waits;     // times it had to spin for a slot (policy=wait)
    uint32_t slot_policy;        // 0 = drop, 1 = wait (mirrors [capture] SlotPolicy)
    uint32_t _pad_v10;
};

// The textures start after the control block, on a 64 KB boundary.
inline constexpr uint64_t kDataStart = 65536;
inline constexpr uint64_t align64k(uint64_t v) { return (v + 65535ull) & ~65535ull; }

// Name used by both sides. "Local\\" keeps it inside the user session.
inline constexpr const char* kMappingName = "Local\\mgpu_mailbox_v1";

} // namespace mgpu
