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
// v13: FrameShared gains the rest of the frame-generation declaration: the previous-frame
//      clip matrices, a frame key for pairing, resetHistory and the MV/depth/camera flags
//      Streamline's sl::Constants or DLSS-G's parameter block carries. This is metadata
//      only - no new texture streams. See docs/design/FG_INPUT_PLAN.md.
// v15: per-stream StreamPercent requests (consumer -> producer) + the producer's echo.
//      Lets the bridge run a slow controller (or the MV-100/depth-200 A/B mode) without
//      editing fakerFG.ini; a change only moves content inside fixed-capacity slots.
// v20: **8 slots**. The 43 fps dynamic run measured colour lag max = 6 publications,
// 332 `no_slot` and 211 `overflow` with 4 slots: the publication a colour needs was
// already overwritten. 8 slots cover lag 6 with margin. Capacity = render size x 4 B x
// 2 streams x slots (2227x1253 -> ~190 MB, 4K -> ~531 MB). Startup cost is accepted by
// the user (their slow launch was local CPU contention, not this). Both sides must be
// rebuilt together.
// v22: 16 slots. The colour path is inherently later than the MV path, so publications
// pile up; with 8 slots the 43 fps run showed colour lag max 14 and 332 `no_slot`. 16
// covers the measured p100 with margin at the current render size (~377 MB mapping).
// v23: 32 slots, and slot capacity follows the CONTENT size (see producer). The colour
// path lags the publisher; the ring is where that backlog lives. 32 x 1480x832 x 2 x 4 B
// is ~314 MB, which fits the consumer's free VRAM.
// v24: producer slots are TRANSIT ONLY (8). The colour backlog lives in the consumer's
// own ring (--input-ring, up to 256 publications), so the render card no longer pays for
// the depth of the queue.
// v20 (restored): the stable 8-slot asynchronous WGC+MV point. The consumer keeps a
// small local ring (8) so the colour/MV backlog stays bounded; dynamic rates are outside
// this design point (see docs).
// v22: the colour stream becomes a first-class member of the publication.
//       stream 0 was laid out as "reserved / WGC" until v21; from v22 the producer
//       places it at the frame's OUTPUT size and copies the game's own back buffer
//       into it (source format, no conversion). One publication therefore carries
//       colour + MV + depth + metadata with a single fence, and the consumer no
//       longer captures the window (docs/design/COLOUR_TRANSPORT_V22.md).
//       kMaxSlots 8 -> 4: the colour no longer lags the motion data, so the deep
//       transit ring (sized for WGC's one-to-two-frame colour delay) is not needed.
// lock_frame v26: kMaxSlots 8 -> 16 (default, user request). 30x4 measured lag
// up to 6 at 4K (colour + 4x present latency); 16 leaves a wide margin for WGC
// jitter and long frames. MV/depth only: 4K capacity is ~1.06 GB, 1440p ~472 MB.
inline constexpr uint32_t kSharedVersion = 28;

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
inline constexpr uint32_t kInputApiSl    = 5;   // Streamline sl::Constants / sl::ResourceTag
inline constexpr uint32_t kInputApiNvngxFg = 6; // DLSS-G NVSDK_NGX_Parameter (DLSSG.*)
inline constexpr uint32_t kInputApiXefg  = 7;   // the game's own XeSS-FG (xefgSwapChain* tags)
inline constexpr uint32_t kInputApiFfxFg = 8;   // the game's own FSR-FG (ffxDispatchDescFrameGenerationPrepare)
// fakerFG's own record (src/faker_fg/, MgpuFgGetApi). The FG pipeline owner hands the
// frame over as an explicit record, so the capture no longer has to guess which API
// produced it; this id is the label for that lane (docs/design/FAKER_FG.md 5).
inline constexpr uint32_t kInputApiFaker = 9;

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
// v13: additional declarations from the FG path. They are metadata, not new streams.
inline constexpr uint32_t kInputFlagHasPrevClip   = 1u << 8;  // clip_to_prev/prev_clip valid
inline constexpr uint32_t kInputFlagResetHistory  = 1u << 9;  // this frame invalidates FG history
inline constexpr uint32_t kInputFlagMvDilated     = 1u << 10; // MV already dilated by the game
inline constexpr uint32_t kInputFlagMv3D          = 1u << 11; // MV are 3D (not screen-space 2D)
inline constexpr uint32_t kInputFlagCameraMotion  = 1u << 12; // camera motion included in MV
inline constexpr uint32_t kInputFlagFgInput       = 1u << 13; // data came from the FG path
// The producer's own probe thinks the MV field is not usable (all-zero while the scene is
// demonstrably changing). The consumer may fall back to optical flow instead of feeding a
// dead field to XeFG. Game-agnostic on purpose: it is a statement about the DATA, not
// about which title produced it (docs/design/FG_CLAIM.md §3).
inline constexpr uint32_t kInputFlagMvSuspect     = 1u << 14;
// v22: this publication actually carries stream 0 colour. The consumer reuses the previous
// sink (and does not advance cadence) when it is missing; it must never read a stale slot.
inline constexpr uint32_t kInputFlagColourActive  = 1u << 15;

// consumer -> producer requests (SharedControl.consumer_request / v14 request_mask)
//
// v14 turns the "produce only what the consumer will use" rule into a per-stream switch
// board. The *capacity* of every slot stays reserved (it is placed once and never moves),
// so switching a stream off costs nothing but also breaks nothing: the next switch-on
// writes the same rectangle again. That is what makes cost/quality experiments cheap -
// they change bytes on the wire, not the layout.
inline constexpr uint32_t kRequestMv    = 1u << 0;  // motion vectors
inline constexpr uint32_t kRequestDepth = 1u << 1;  // depth
inline constexpr uint32_t kRequestColor = 1u << 2;  // HUDless colour
inline constexpr uint32_t kRequestUi    = 1u << 3;  // UI colour+alpha (or its mask)
inline constexpr uint32_t kRequestMeta  = 1u << 4;  // camera/matrix/jitter metadata (else)
// v15: "dumb producer". These do not add data, they change WHO processes it: the producer
// then does a 1:1 copy at source resolution/format and the consumer resamples/converts on
// its own GPU (see docs/design/TRANSFER_V2.md section 11). Off by default, because each one
// trades main-GPU time for cross-adapter bytes: MV +51 MB/frame, depth +14.8 MB/frame.
inline constexpr uint32_t kRequestRawMv    = 1u << 5;
inline constexpr uint32_t kRequestRawDepth = 1u << 6;
inline constexpr uint32_t kRequestAll   = 0x1Fu;
inline constexpr uint32_t kRequestAllV15 = 0x7Fu;

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
    // v14 (append-only): what the FG declaration says about pacing. `declared_frames` is
    // DLSSGOptions.numFramesToGenerate (0 = the game never called SetOptions), so a
    // consumer can tell "the game is already generating 2/3 frames" from "we are the only
    // generator"; `harvest_flags` is the kHarvest* bitmap the source could fill.
    uint32_t declared_frames;
    uint32_t harvest_flags;
};

inline constexpr uint32_t kMaxStreams = 5;
// v22: 4 transit slots. Colour + MV + depth share the slot, so a publication is one
// frame in every stream; 4 slots = two in flight plus two buffered. (The old 8 was
// sized for WGC's late colour, which no longer exists.)
inline constexpr uint32_t kMaxSlots   = 32;

// v22 stream roles. 0/1/2 are placed and published; 3 (UI) and 4 (scar) are reserved
// and not placed by the current producer.
inline constexpr uint32_t kStreamColour = 0;
inline constexpr uint32_t kStreamMv     = 1;
inline constexpr uint32_t kStreamDepth  = 2;
inline constexpr uint32_t kStreamUi     = 3;

// v17 reserved a 16x16 R10 tile per publication for the pre-marker patch. From v22 the
// marker mechanism is gone with WGC (slot identity replaces it), so this stream is no
// longer laid out or published. The constant stays so the StreamShared array keeps its
// historical indices.
inline constexpr uint32_t kStreamScar = 4;
inline constexpr uint32_t kScarSide   = 16u;

// Marker geometry: a 32-bit SHORT SNOWFLAKE code is SPLIT over FOUR 6x6 blocks, one
// per screen corner. Each block is 3x3 cells of 2x2 px: 8 data bits + 1 reference cell
// that is white unless the data bits are all white (then black), so a block always has
// both levels for the min/max decoder. The four corners together carry payload24+CRC-8.
//
// Why 6x6: 4x6x6 = 144 px total instead of 256, i.e. 44% less area to patch back, and
// 2x2-px cells keep the bits readable if the capture ever goes through a filter. The
// blocks hug the four screen corners (margin = 1/32 of the frame).
inline constexpr uint32_t kMarkerSide     = 6u;
inline constexpr uint32_t kMarkerCorners  = 4u;
inline constexpr uint32_t kMarkerCellPx   = 2u;               // cells are 2x2 px
inline constexpr uint32_t kMarkerCells    = kMarkerSide / kMarkerCellPx;  // 3
inline constexpr uint32_t kMarkerCellsPerBlock = kMarkerCells * kMarkerCells;  // 9
inline constexpr uint32_t kMarkerBits     = 8u;               // data bits per block
inline constexpr uint32_t kMarkerPayloadBits = 24u;           // seq12 | ms12
inline constexpr uint32_t kMarkerSeqBits  = 12u;              // snowflake sequence bits
inline constexpr uint32_t kMarkerCodeBits = 32u;              // payload24 + CRC-8
inline constexpr uint32_t kMarkerMargin   = 32u;              // margin = 1/32 of the frame
// Every block's 8 data bits are XORed with this before drawing and after decoding, so
// a chunk of zeros does not turn the block into a solid square (the decoder needs two
// distinct levels; the always-white cell covers the all-zero case, this covers the
// rest and makes the pattern look like noise rather than a block).
inline constexpr uint32_t kMarkerXor      = 0x5Au;
// Legacy (unused) 16x16 tile byte count kept for the shared record layout.
inline constexpr uint32_t kFrameMarkerBytes = 16u * 16u * 8u;
// Diagnostics that still refer to the top-left block.
inline uint32_t marker_origin_x(uint32_t width)  { return (width  / kMarkerMargin) & ~3u; }
inline uint32_t marker_origin_y(uint32_t height) { return (height / kMarkerMargin) & ~3u; }

// ---------------------------------------------------------------- short snowflake
// A full 64-bit snowflake is (41-bit ms | 10-bit worker | 12-bit sequence). Two thirds
// of it are useless inside a 144-px marker: the ms timestamp is already known on both
// sides, and the worker id is fixed here. What the marker needs is a value that is
// different on every frame and cheap to check, so the payload is
//     payload24 = sequence12 | low12(ms)
// and the marker carries payload24 plus a CRC-8. The consumer resolves the full frame
// id from the present ring, so 24 bits never has to be globally unique.
inline constexpr uint64_t kSnowflakeEpochMs = 1600000000000ull;   // 2020-09-13
inline constexpr uint32_t kSnowflakeWorkerMask = 0x3FFu;

inline uint64_t snowflake_id(uint64_t ms_since_epoch, uint32_t sequence,
                            uint32_t worker = 0u)
{
    const uint64_t ts = (ms_since_epoch > kSnowflakeEpochMs)
                            ? (ms_since_epoch - kSnowflakeEpochMs) : ms_since_epoch;
    return ((ts & 0x1FFFFFFFFFFull) << 22) |
           ((static_cast<uint64_t>(worker) & kSnowflakeWorkerMask) << 12) |
           (static_cast<uint64_t>(sequence) & 0xFFFu);
}

// The 24-bit payload a marker block set carries for this id.
inline uint32_t marker_payload_from_id(uint64_t id)
{
    const uint32_t seq = static_cast<uint32_t>(id & 0xFFFu);              // sequence12
    const uint32_t ms = static_cast<uint32_t>((id >> 22) & 0xFFFu);       // low12(ms)
    return seq | (ms << kMarkerSeqBits);
}

inline uint32_t marker_seq_from_payload(uint32_t payload)
{
    return payload & 0xFFFu;
}

// CRC-8 (poly 0x07) over the 24-bit payload: a misread corner must never be turned
// into a wrong pairing, and 8 bits is plenty next to the full-id comparison.
inline uint8_t marker_crc8(uint32_t payload24)
{
    uint32_t v = (payload24 & 0xFFFFFFu) << 8;
    uint8_t crc = 0u;
    for (int i = 0; i < 32; ++i) {
        const uint8_t bit = static_cast<uint8_t>(((v >> 31) & 1u) ^ (crc >> 7));
        crc = static_cast<uint8_t>(crc << 1);
        if (bit != 0u) { crc ^= 0x07u; }
        v <<= 1;
    }
    return crc;
}

inline uint32_t marker_code32(uint32_t payload24)
{
    return (payload24 & 0xFFFFFFu) |
           (static_cast<uint32_t>(marker_crc8(payload24)) << 24);
}

// i = 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right.
// Margins are a FRACTION of the frame (w/32, h/32, 4-px aligned) rather than absolute
// pixels, so the producer (chain buffer) and the consumer (WGC copy, possibly at a
// different scale) compute the same spot. At 3840x2160 that is (120, 64) from each
// edge - still literally "the corner of the picture", never a hard-coded pixel.
inline void marker_corner(uint32_t i, uint32_t w, uint32_t h, uint32_t* x, uint32_t* y)
{
    const bool right  = (i & 1u) != 0u;
    const bool bottom = (i & 2u) != 0u;
    const uint32_t mx = (w / kMarkerMargin) & ~3u;
    const uint32_t my = (h / kMarkerMargin) & ~3u;
    if (x != nullptr) {
        *x = right ? (w - mx - kMarkerSide) : mx;
    }
    if (y != nullptr) {
        *y = bottom ? (h - my - kMarkerSide) : my;
    }
}

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
    // ---- v23: global publication sequence ----
    // Every PUBLICATION increments this (one per publish_frame), so it is unique and
    // monotonic across slots. `published` above is a per-SLOT fence value and repeats
    // across slots (1,1,1,1,2,2,2,2...), which made a single watermark skip 3 of every 4
    // publications. The consumer orders by pub_seq; `published` stays the ACK token.
    uint64_t pub_seq;
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
    // ---- v13 ----
    // Previous-frame clip matrices from sl::Constants / DLSSG.ClipToPrevClip. They are
    // *not* handed to XeFG verbatim; the consumer may use them for its own reprojection
    // guide or for diagnostics. `frame_key` is the Streamline frame token / DLSS-G
    // BackbufferFrameID and is only a pairing key.
    float    clip_to_prev_clip[16];
    float    prev_clip_to_clip[16];
    uint64_t frame_key;
    // QPC milliseconds (same machine clock as WGC's SystemRelativeTime) when the
    // producer captured this frame. Used to pair the late WGC colour with the right
    // MV/depth slot instead of relying on slot rotation.
    uint64_t frame_qpc_ms;
    uint32_t present_id;
    // Packed FG declaration: kInputFlagResetHistory / kInputFlagMvDilated /
    // kInputFlagMv3D / kInputFlagCameraMotion / kInputFlagInvertedDepth / ...
    uint32_t fg_flags;
    float    mv_invalid_value;
    float    camera_pos[3];
    float    camera_up[3];
    float    camera_right[3];
    float    camera_fwd[3];
    float    camera_pinhole[2];
    // ---- v16/v17: pre-marker 16x16 sidecar, same frame_id and same slot as MV/depth ----
    // fakerFG records a 16x16 copy of the chain buffer (before it draws the marker)
    // into streams[kStreamScar] of THE SAME publication slot. The consumer restores
    // it after its own HDR conversion; `marker_valid` says the copy was recorded for
    // this publication. The pixels are not stored here (v17) - only the geometry.
    uint32_t marker_valid;
    uint32_t marker_format;   // DXGI_FORMAT of the tile in the scar slot
    uint32_t marker_x;
    uint32_t marker_y;
    uint8_t  marker_tile[kFrameMarkerBytes];   // legacy (v16 CPU path), unused
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
    uint32_t slot_policy;        // Reserved: the producer always writes 0 (drop). A wait
                                 // policy was never implemented and producer_waits stays 0.
    uint32_t _pad_v10;
    // ---- v14: per-stream switch board ----
    // Written by the consumer at any time (no handshake, no restart): which non-essential
    // streams it wants. `active_mask` is the producer's echo of what it actually wrote in
    // the last published frame, and `stream_bytes` is what that cost, so the consumer can
    // report "mv=66.4 MB depth=14.7 MB" instead of guessing.
    uint32_t request_mask;
    uint32_t active_mask;
    uint32_t stream_bytes[kMaxStreams];
    uint32_t _pad_v14;
    // ---- v15: per-stream StreamPercent requests (consumer -> producer) ----
    // 0 = no override: the producer keeps the value read from fakerFG.ini. Values are
    // 100..400 and are applied on the producer's next record. A change bumps `generation`
    // (the content rectangles move inside fixed-capacity slots), so the consumer rebuilds
    // its local copies. `active_*` echo what the producer is actually using, which is what
    // the controller and the A/B UI read back. This is a one-way hint like v7/v14: the
    // producer never waits on it.
    uint32_t requested_percent_mv;
    uint32_t requested_percent_depth;
    uint32_t active_percent_mv;
    uint32_t active_percent_depth;
    // ---- v21: colour-driven flow control ----
    // The consumer's most recently PRESENTED publication id. The producer uses it to
    // decide which slot may be overwritten: anything at or below this id has already been
    // consumed by the colour path, anything above it is still waiting for its colour.
    // When every slot is above it, the producer SKIPS this publication instead of
    // overwriting data the colour still needs (`producer_skipped` counts that).
    uint64_t colour_seq;
    uint32_t producer_skipped;
    uint32_t _pad_v21;
    // ---- v27: connection reset ----
    // Written by the consumer immediately after it opens the mapping. The producer
    // compares it with the last value it saw and, on a change, clears colour_seq,
    // consumed[], frames[] and frame_published before publishing the new session.
    // A stale ACK from a previous consumer can otherwise be thousands of frames
    // ahead of a new producer/consumer pair and skip every publication forever.
    uint64_t consumer_session;
};

// The textures start after the control block, on a 64 KB boundary.
// The control block grew past the old hard-coded 64 KB when the frame records were
// expanded. If kDataStart stays below sizeof(SharedControl), the first slots overwrite
// the tail of the control block (observed: colour_seq/consumer_session became random
// slot data and the producer reset forever). Always derive it from the real struct.
inline constexpr uint64_t kDataStart =
    (sizeof(SharedControl) + 65535ull) & ~65535ull;
inline constexpr uint64_t align64k(uint64_t v) { return (v + 65535ull) & ~65535ull; }
static_assert(kDataStart >= sizeof(SharedControl),
              "shared data must start after the whole control block");

// Name used by both sides. "Local\\" keeps it inside the user session.
inline constexpr const char* kMappingName = "Local\\mgpu_mailbox_v1";
// Created and set by the consumer at startup. The producer refuses to create the mapping
// / heap / placed textures until this event exists, so launching the game WITHOUT the
// consumer costs the transport layer nothing (the old behaviour created 90-540 MB of
// cross-adapter resources during the game's black loading screen).
inline constexpr const wchar_t* kConsumerPresentEvent = L"Local\\mgpu_consumer_present_v1";

} // namespace mgpu
