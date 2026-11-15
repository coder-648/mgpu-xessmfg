// ============================================================================
//  mgpu_pair_core.h - colour <-> publication pairing, pure logic.
//
//  Why this exists (2026-11-15 audit):
//    The old path decoded the marker of the PREVIOUS colour frame (two staging
//    slots) and then paired "decoded seq + 1" with the current colour. That is
//    only correct while the processed colour sequence is consecutive:
//      * a colour dropped by WGC makes the guess one publication stale;
//      * a duplicate delivered by DWM makes it one publication ahead.
//    Both cases were silent: `pf served` still counted a hit.
//
//  This header is the replacement decision, with no Windows/D3D dependency so
//  tests/pair_core can prove it:
//    * the caller decodes the marker of the frame it is holding (no +1);
//    * a candidate must match frame_id's low 12 bits AND the marker's 12-bit ms
//      field (circular, because 4096 ms wraps constantly);
//    * "no match" is an explicit outcome; the caller presents 1X instead of
//      tagging a guessed publication.
//
//  SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#pragma once

#include <cstdint>

namespace mgpu_pair {

// 12-bit millisecond field: wraps every 4096 ms, so distance must be circular.
inline uint32_t ms12_of(uint64_t qpc_ms)
{
    return static_cast<uint32_t>(qpc_ms & 0xFFFu);
}

inline int ms12_distance(uint32_t a, uint32_t b)
{
    int d = static_cast<int>(a) - static_cast<int>(b);
    if (d < 0) { d = -d; }
    if (d > 2048) { d = 4096 - d; }
    return d;
}

// True when the frame's own timestamp agrees with the marker's low 12 ms bits.
// tol_ms covers Evaluate -> Present skew and the 1 ms marker granularity.
inline bool ms12_close(uint64_t qpc_ms, uint32_t marker_ms12, uint32_t tol_ms)
{
    return ms12_distance(ms12_of(qpc_ms), marker_ms12) <= static_cast<int>(tol_ms);
}

// The only fields of FrameShared the decision needs.
struct SlotView
{
    uint64_t frame_id;
    uint64_t frame_qpc_ms;
    uint64_t published;
};

struct Match
{
    int  slot       = -1;     // -1 = no verified publication
    bool ms_checked = false;  // the ms12 field agreed (require_ms == true path)
    bool ambiguous  = false;  // two slots share seq12 (wrap) -> caller must not tag
};

// Exact identity match. `require_ms` must stay true in the shipping path; false
// is only for diagnostics/tests.
inline Match find_exact(const SlotView* slots, int n,
                        uint32_t seq12, uint32_t marker_ms12,
                        uint32_t tol_ms, bool require_ms = true)
{
    Match m;
    int best = -1, second = -1;
    uint64_t best_pub = 0;
    for (int i = 0; i < n; ++i) {
        if (slots[i].frame_id == 0) { continue; }
        if (static_cast<uint32_t>(slots[i].frame_id & 0xFFFu) != seq12) { continue; }
        const bool ok_ms = ms12_close(slots[i].frame_qpc_ms, marker_ms12, tol_ms);
        if (require_ms && !ok_ms) { continue; }
        if (slots[i].published >= best_pub) {
            second   = best;
            best_pub = slots[i].published;
            best     = i;
            m.ms_checked = ok_ms;
        } else {
            second = i;
        }
    }
    m.slot      = best;
    m.ambiguous = (best >= 0 && second >= 0);
    return m;
}

// A local copy may be acknowledged only after its copy fence retired, and only
// with the publication value that was actually copied (never the slot's current
// `published`, which may have advanced while the copy was queued).
inline bool ack_value(uint64_t local_pub, uint64_t copy_fence_value,
                      uint64_t copy_fence_completed, uint64_t already_consumed,
                      uint64_t* out)
{
    if (local_pub == 0 || copy_fence_value == 0) { return false; }
    if (copy_fence_completed < copy_fence_value) { return false; }
    if (local_pub <= already_consumed) { return false; }
    if (out != nullptr) { *out = local_pub; }
    return true;
}

// Cache capacity from measured colour lag. `p99_lag_publications` is what the
// new `pair:` line reports; the +2 covers a burst after a stall.
inline int required_depth(double p99_lag_publications)
{
    int n = static_cast<int>(p99_lag_publications) + 3;
    if (n < 4) { n = 4; }
    if (n > 16) { n = 16; }
    return n;
}

} // namespace mgpu_pair
