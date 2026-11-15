// ============================================================================
//  tests/pair_core/sim.cpp - host-side proof for src/mgpu_pair_core.h
//
//  Scenarios are the ones that broke the old "+1" pairing:
//    consecutive / duplicate / dropped / evicted / seq wrap / ms mismatch.
//  Build+run: tests/pair_core/run.sh
// ============================================================================
#include "../../src/mgpu_pair_core.h"

#include <cstdio>
#include <cstring>

using namespace mgpu_pair;

static int g_fail = 0;
static int g_checks = 0;

static void check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("[FAIL] %s\n", what); }
    else     { std::printf("[ ok ] %s\n", what); }
}

// One publication per slot, newest first is irrelevant: find_exact picks max published.
static Match match_one(uint64_t id, uint64_t qpc, uint32_t marker_seq, uint32_t marker_ms,
                       bool require_ms = true)
{
    SlotView v[1] = {{ id, qpc, 1 }};
    return find_exact(v, 1, marker_seq, marker_ms, 40, require_ms);
}

int main()
{
    std::printf("=== pair_core: exact identity ===\n");

    // 1. Consecutive: colour N carries marker N (current-frame decode).
    {
        const uint64_t qpc = 1000;
        const uint32_t seq12 = static_cast<uint32_t>(42u & 0xFFFu);
        const Match m = match_one(42, qpc, seq12, ms12_of(qpc));
        check(m.slot == 0 && !m.ambiguous, "consecutive: current marker matches frame_id");
    }

    // 2. Duplicate colour: the same id resolves to the same publication twice.
    {
        const uint64_t qpc = 2000;
        const Match a = match_one(43, qpc, 43, ms12_of(qpc));
        const Match b = match_one(43, qpc, 43, ms12_of(qpc));
        check(a.slot == 0 && b.slot == 0, "duplicate: same colour -> same publication (no +1)");
    }

    // 3. Dropped colour: colour jumps N-1 -> N+1; the current marker is N+1 and
    //    must match N+1, never the "+1 of the decoded previous frame" (which
    //    would have been N).
    {
        const uint64_t qpc = 3000;
        const Match m = match_one(45, qpc, 45, ms12_of(qpc));
        check(m.slot == 0, "dropped: current id matches itself, not a guessed neighbour");
        const Match wrong = match_one(44, qpc, 45, ms12_of(qpc));
        check(wrong.slot < 0, "dropped: a stale publication is not accepted");
    }

    // 4. Evicted advertisement: publication no longer in any slot -> explicit miss.
    {
        const Match m = match_one(46, 4000, 47, ms12_of(4000));
        check(m.slot < 0, "evicted: no candidate -> caller must present 1X");
    }

    // 5. seq12 wrap: 4095 -> 0. The ms field disambiguates the current one; note
    //    frame_id itself is never 0 (0 means "empty slot"), only its low 12 bits.
    {
        SlotView v[2] = {{4095, 100, 10}, {4096, 5100, 20}};
        const Match m = find_exact(v, 2, 0, ms12_of(5100), 40, true);
        check(m.slot == 1 && !m.ambiguous, "wrap: ms12 picks the current publication");
        const Match old = find_exact(v, 2, 0, ms12_of(100), 40, true);
        check(old.slot < 0, "wrap: a 5 s old ms12 is not accepted");
    }

    // 6. ms mismatch (wrong publication with the same seq12) -> refused.
    {
        SlotView v[1] = {{77, 1000, 1}};
        const Match m = find_exact(v, 1, 77, ms12_of(3000), 40, true);
        check(m.slot < 0, "ms mismatch: refused instead of tagging a stale pair");
        const Match loose = find_exact(v, 1, 77, ms12_of(3000), 40, false);
        check(loose.slot == 0, "(diagnostic mode without ms check still finds it)");
    }

    // 7. Ambiguity: same seq12 and same ms in two slots -> must not tag.
    {
        SlotView v[2] = {{5, 1000, 10}, {5 + 4096, 1000, 20}};
        const Match m = find_exact(v, 2, 5, ms12_of(1000), 40, true);
        check(m.ambiguous, "ambiguous seq12/ms: flagged, caller refuses");
    }

    std::printf("\n=== pair_core: publication ACK ===\n");
    {
        uint64_t out = 0;
        check(!ack_value(0, 5, 5, 0, &out), "ack: no local copy -> no ack");
        check(!ack_value(7, 5, 4, 0, &out), "ack: copy fence not retired -> no ack");
        check(ack_value(7, 5, 5, 0, &out) && out == 7, "ack: exact copied value");
        check(!ack_value(7, 5, 5, 7, &out), "ack: never goes backwards");
        check(!ack_value(6, 5, 5, 7, &out), "ack: older publication is not re-acked");
    }

    std::printf("\n=== pair_core: capacity ===\n");
    check(required_depth(0)   == 4,  "depth: floor is 4");
    check(required_depth(2.4) == 5,  "depth: 2 + 3 = 5");
    check(required_depth(9.0) == 12, "depth: 9 + 3");
    check(required_depth(40)  == 16, "depth: capped at 16");

    std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
