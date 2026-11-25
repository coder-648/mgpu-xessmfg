// ============================================================================
//  synth_ui_full.hlsl - render-card full-resolution UI synthesis (P1-B)
//
//  The DLSSG lane already has both images at Evaluate:
//      P = DLSSG.Backbuffer (composed, UI included)
//      H = HUDless         (scene without UI, same frame)
//  so the approximate UI plane can be built here, at full resolution, instead of
//  shipping a 22-27 MB HUDless frame to the second GPU:
//
//      d = P - H
//      m = smoothstep(t, 2t, max|d|)        // difference mask, not the true alpha
//      U = P - (1 - m) * H,  a = m          // premultiplied, satisfies U + (1-a)H = P
//
//  m is a heuristic; the low-frequency part is exact by construction (see
//  docs/design/UI_COMPUTE_QUALITY.md). The final on-screen UI still comes from the
//  full-resolution backbuffer via XeFG mode 4, so this only has to be a clean
//  HUDless base.
//
//  Both inputs must be the same format class. ConvertPQ = 1 for FP16 scRGB sources
//  (the output is R10 PQ, matching the consumer's chain buffer).
//
//  Bindings: t0 = P, t1 = H, u0 = UI (R10/RGBA8), b0 = {W,H,Threshold,ConvertPQ}
// ============================================================================

cbuffer Params : register(b0)
{
    uint  W;
    uint  H;
    float Threshold;
    uint  UseAlpha;       // 1 = Alpha (Streamline kBufferTypeUIAlpha/67) is valid
    uint  Linear;         // 1 = subtract in linear light (P4b), 0 = legacy code-space
    uint  PSrcScRGB;      // 1 = P is FP16 scRGB, 0 = P is R10 PQ (or SDR code-space)
    uint  HSrcScRGB;      // 1 = H is FP16 scRGB, 0 = H is R10 PQ (or SDR code-space)
    uint  Dilate;         // 1 = 3x3 max-dilate the difference mask (edge coverage)
    uint  Sparse;         // 1 = empty groups are skipped (codec writes mode=3 zero tiles)
    uint  Patch;          // v33: 1 = stream 3 carries the HUDless patch (scene under UI),
                          //      not a UI plane. The consumer tags H' only (XeFG mode 4).
    uint  PatchDirect;    // v34: 1 = PatchH (t3) is already display-referred; write raw
    float PatchWhite;     // v33: scRGB 1.0 = N nits for the patch. CP2077's HUDless is
                          //      scene-referred at ~20 nits (measured), not 80.
    uint  StatsOn;        // 1 = write the coverage/diagnostic counters this frame. The
                          // counters are per-pixel/8x8 InterlockedAdds into 9 addresses,
                          // which is measurable at 4K; UiStats (fakerFG.ini) sets how
                          // often they run (default every 60th frame).
};

Texture2D<float4>   P : register(t0);
Texture2D<float4>   Hudless : register(t1);
// Only sampled when UseAlpha != 0; the producer binds a valid descriptor either way.
Texture2D<float>    Alpha : register(t2);
// v34: optional display-referred patch source (ScalingOutputColor). When PatchDirect=1
// this is the colour written to the patch; Hudless remains the mask reference above.
Texture2D<float4>   PatchH : register(t3);
SamplerState        Smp : register(s0);
RWTexture2D<float4> Ui : register(u0);
// [0] = pixels with m > 0.5, [1] = pixels with m > 0.01. The producer reads these at most
// once per second and publishes the coverage in StreamShared.ui_quality.
// [2]/[3]/[4] = sampled H/P code sums + sample count (diagnostic: is the source H black
// or is the P/H colour interpretation wrong?).
// [5] normal raw |P-H|, [6] flip-X, [7] flip-Y, [8] flip-XY. The last four tell us whether
// H is a different frame/space rather than a different transfer function.
RWStructuredBuffer<uint> Stats : register(u2);
// Per-8x8-group occupancy (sparse UI encode). Descriptor table u0 = UI, u1 = GroupMask.
RWStructuredBuffer<uint> GroupMask : register(u1);

float3 pq_encode(float3 nits)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

static const float3x3 k709to2020 = float3x3(
    0.6274, 0.3293, 0.0433,
    0.0691, 0.9195, 0.0114,
    0.0164, 0.0880, 0.8956);

float3 pq_decode(float3 e)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float3 p = pow(max(e, 1e-8f), 1.0f / m2);
    const float3 num = max(p - c1, 0.0f);
    const float3 den = max(c2 - c3 * p, 1e-8f);
    return 10000.0f * pow(num / den, 1.0f / m1);
}

float3 linear709_to_nits2020(float3 c)
{
    return mul(k709to2020, c * 80.0f);
}

float3 to_pq(float3 linear709)
{
    return pq_encode(linear709_to_nits2020(linear709));
}

// v33 patch: same conversion, but the HUDless's scRGB scale is not always 80 nits.
// CP2077 hands a scene-referred target whose 1.0 is ~20 nits; encoding it at 80 made
// the transported patch ~1.6x brighter than the consumer's display-referred present.
float3 to_pq_white(float3 linear709, float white_nits)
{
    return pq_encode(mul(k709to2020, linear709 * white_nits));
}

float raw_diff(float3 a, float3 b)
{
    const float3 d = abs(a - b);
    return max(max(d.r, d.g), d.b);
}

// Normalized P-H difference at one pixel. Used by the 3x3 dilation below.
float delta_at(int2 q)
{
    q = clamp(q, int2(0, 0), int2((int)W - 1, (int)H - 1));
    const float3 pr = P.Load(int3(q, 0)).rgb;
    const float3 hr = Hudless.Load(int3(q, 0)).rgb;
    float3 pv, hv;
    if (Linear != 0) {
        pv = (PSrcScRGB != 0) ? linear709_to_nits2020(pr) : pq_decode(pr);
        hv = (HSrcScRGB != 0) ? linear709_to_nits2020(hr) : pq_decode(hr);
    } else {
        pv = (PSrcScRGB != 0) ? to_pq(pr) : pr;
        hv = (HSrcScRGB != 0) ? to_pq(hr) : hr;
    }
    float d = max(max(abs(pv.r - hv.r), abs(pv.g - hv.g)), abs(pv.b - hv.b));
    if (Linear != 0) {
        d /= max(max(pv.r, pv.g), max(pv.b, 1.0f));
    }
    return d;
}

bool marker_pixel(int2 p)
{
    // The producer synth can run after fakerFG's four-corner marker was drawn into the
    // chain buffer, while the HUDless snapshot is pre-marker. Treat those 6x6 blocks as
    // non-UI; otherwise U = P - H carries the marker into the transported UI.
    const int mx = int((W / 32u) & ~3u);
    const int my = int((H / 32u) & ~3u);
    const int x1 = int(W) - mx - 6;
    const int y1 = int(H) - my - 6;
    return (p.y >= my && p.y < my + 6 &&
            ((p.x >= mx && p.x < mx + 6) || (p.x >= x1 && p.x < x1 + 6))) ||
           (p.y >= y1 && p.y < y1 + 6 &&
            ((p.x >= mx && p.x < mx + 6) || (p.x >= x1 && p.x < x1 + 6)));
}

// One flag per 8x8 thread group: 0 = no UI, skip the full-res P/H work.
groupshared uint g_ui_group_hit;

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID,
            uint3 tid : SV_GroupThreadID)
{
    // Do not return before the group barrier: out-of-range lanes still participate.
    const int2 p = int2(id.xy);

    // ---- v33: HUDless support patch ------------------------------------------------
    // Case 2 of the UI plan: the consumer needs a clean HUDless, and the only place it
    // cannot get one from `present` is where UI covers the scene. H already contains that
    // scene, so ship H on the UI support and let the consumer copy it. `a` is not an
    // opacity here - the support is a recall-first set (over-cover only costs bytes).
    //
    // We fill the whole frame with H (empty tiles are dropped by the sparse codec), which
    // guarantees every transmitted 32x8 tile carries valid scene pixels: the encoder works
    // per tile, not per 8x8 group, so a partially covered tile must not contain stale H.
    if (Patch != 0u) {
        if (tid.x == 0u && tid.y == 0u) { g_ui_group_hit = 0u; }
        GroupMemoryBarrierWithGroupSync();

        float3 h_enc = float3(0.0f, 0.0f, 0.0f);
        float  hit   = 0.0f;
        const bool in_range = (id.x < W && id.y < H);
        if (in_range) {
            const float3 hudless_raw = Hudless.Load(int3(p, 0)).rgb;
            const float3 present_raw = P.Load(int3(p, 0)).rgb;
            // Transport space: R10 PQ when the source is HDR (FP16 scRGB), otherwise the
            // source is already in the transport encoding (R10 PQ or SDR code values).
            // v34: PatchH may be a display-referred plane (ScalingOutputColor); in that
            // case write it raw, while the mask still comes from Hudless.
            const float3 patch_raw = (PatchDirect != 0u) ? PatchH.Load(int3(p, 0)).rgb
                                                         : hudless_raw;
            h_enc = (PatchDirect != 0u)
                        ? patch_raw
                        : ((HSrcScRGB != 0) ? to_pq_white(patch_raw, PatchWhite)
                                            : patch_raw);

            // Difference support (relative, in linear nits when UiLinear=1). This is also
            // the fallback for additive UI: such UI can have alpha 0 while still changing
            // P, and alpha alone would miss it.
            float3 pv, hv;
            if (Linear != 0) {
                pv = (PSrcScRGB != 0) ? linear709_to_nits2020(present_raw) : pq_decode(present_raw);
                hv = (HSrcScRGB != 0) ? linear709_to_nits2020(hudless_raw) : pq_decode(hudless_raw);
            } else {
                pv = (PSrcScRGB != 0) ? to_pq(present_raw) : present_raw;
                hv = (HSrcScRGB != 0) ? to_pq(hudless_raw) : hudless_raw;
            }
            float d = max(max(abs(pv.r - hv.r), abs(pv.g - hv.g)), abs(pv.b - hv.b));
            if (Linear != 0) {
                d /= max(max(pv.r, pv.g), max(pv.b, 1.0f));
            }
            bool hit_b = (d > max(Threshold, 0.002f));
            if (UseAlpha != 0) {
                // The title's own alpha plane is the exact support for normal alpha UI;
                // OR it with the difference so additive/glow UI is not missed.
                const float2 uv = (float2(p) + 0.5f) / float2((float)W, (float)H);
                hit_b = hit_b || (Alpha.SampleLevel(Smp, uv, 0).r > 0.002f);
            }
            hit = hit_b ? 1.0f : 0.0f;
            if (marker_pixel(p)) { hit = 0.0f; }
        }

        if (hit > 0.5f) { InterlockedOr(g_ui_group_hit, 1u); }
        GroupMemoryBarrierWithGroupSync();
        const uint gx = (W + 7u) / 8u;
        if (tid.x == 0u && tid.y == 0u) {
            GroupMask[gid.y * gx + gid.x] = g_ui_group_hit;
        }
        if (in_range) {
            // alpha = per-pixel UI mask (1 = UI, 0 = clean scene). The consumer uses
            // alpha=0 pixels to calibrate the scene-referred -> display-referred ratio;
            // the patch RGB itself is always the clean HUDless.
            Ui[p] = float4(saturate(h_enc), (hit > 0.5f) ? 1.0f : 0.0f);
            if (StatsOn != 0u && hit > 0.5f) {
                InterlockedAdd(Stats[0], 1u);
                InterlockedAdd(Stats[1], 1u);
            }
        }
    } else {

    // Coarse group gate: 4x4 point samples over this 8x8 group. A group with no P/H
    // difference writes zero and returns before the 4K P/H loads. UseAlpha keeps the old
    // path (the alpha plane may contain thin UI the point samples miss).
    if (tid.x == 0u && tid.y == 0u) {
        uint hit = 0u;
        if (UseAlpha != 0) {
            hit = 1u;
        } else {
            const int bx = int(gid.x) * 8;
            const int by = int(gid.y) * 8;
            [unroll] for (int sy = 0; sy < 4; ++sy) {
                [unroll] for (int sx = 0; sx < 4; ++sx) {
                    const int2 q = clamp(int2(bx + sx * 2 + 1, by + sy * 2 + 1),
                                         int2(0, 0), int2(int(W) - 1, int(H) - 1));
                    const float3 pp = P.Load(int3(q, 0)).rgb;
                    const float3 hh = Hudless.Load(int3(q, 0)).rgb;
                    const float d = max(max(abs(pp.r - hh.r), abs(pp.g - hh.g)),
                                        abs(pp.b - hh.b));
                    if (d > max(Threshold, 0.002f)) { hit = 1u; }
                }
            }
        }
        g_ui_group_hit = hit;
        const uint gx = (W + 7u) / 8u;
        GroupMask[gid.y * gx + gid.x] = hit;
    }
    GroupMemoryBarrierWithGroupSync();
    if (id.x >= W || id.y >= H) { return; }
    if (g_ui_group_hit == 0u) {
        // Sparse codec emits a mode=3 header for this 32x8 tile and the decoder zeroes it,
        // so there is no need to write 4K zeros here. Keep the explicit clear for the
        // normal codec (and for UI dump frames, where the BMP must show a clean zero).
        if (Sparse == 0u) { Ui[p] = float4(0.0f, 0.0f, 0.0f, 0.0f); }
        return;
    }

    const float3 present_raw = P.Load(int3(p, 0)).rgb;
    const float3 hudless_raw = Hudless.Load(int3(p, 0)).rgb;

    float3 pv, hv;
    // CP2077 (and other DLSS-G titles) can hand us mixed encodings: the chain buffer is
    // R10 PQ, while the HUDless is FP16 scRGB. Convert each side on its own, then the
    // subtraction happens in one common space.
    const bool p_sc = (PSrcScRGB != 0);
    const bool h_sc = (HSrcScRGB != 0);
    if (Linear != 0) {
        pv = p_sc ? linear709_to_nits2020(present_raw) : pq_decode(present_raw);
        hv = h_sc ? linear709_to_nits2020(hudless_raw) : pq_decode(hudless_raw);
    } else {
        pv = p_sc ? to_pq(present_raw) : present_raw;
        hv = h_sc ? to_pq(hudless_raw) : hudless_raw;
    }

    const float3 d = pv - hv;
    float delta = max(max(abs(d.r), abs(d.g)), abs(d.b));
    if (Linear != 0) {
        // Relative difference in linear light; the same Threshold then means "~x% of the
        // scene luminance", which is closer to the PQ code-space intent than nits/10000.
        delta /= max(max(pv.r, pv.g), max(pv.b, 1.0f));
    }
    // 3x3 max dilation: covers the AA/soft edge on the UI side instead of leaving a
    // 1-pixel ring of scene in the HUDless (the "smear" around HUD text). Max, not blur:
    // the edge stays sharp, it only expands by one pixel.
    if (Dilate != 0 && UseAlpha == 0) {
        [unroll] for (int oy = -1; oy <= 1; ++oy) {
            [unroll] for (int ox = -1; ox <= 1; ++ox) {
                if (ox != 0 || oy != 0) { delta = max(delta, delta_at(p + int2(ox, oy))); }
            }
        }
    }

    float m;
    if (UseAlpha != 0) {
        // True UI alpha from the title (8-bit or better): no mask guessing. Bilinear so a
        // lower-resolution alpha plane (1/2, 1/4) still aligns with the full-res P/H.
        const float2 uv = (float2(p) + 0.5f) / float2((float)W, (float)H);
        m = saturate(Alpha.SampleLevel(Smp, uv, 0).r);
    } else if (Threshold <= 1e-5) {
        m = (delta > 0.002f) ? 1.0f : 0.0f;
    } else {
        m = smoothstep(Threshold, Threshold * 2.0f, delta);
    }

    // Premultiplied UI colour that reproduces P when composited over H. In linear mode the
    // subtraction happens in nits and the result is PQ-encoded for the transport.
    // Outside the HUD mask the correct UI is exactly zero: P-H there is only the tiny
    // frame/rounding residual (measured rawdiff 0.012), and transporting it would add a
    // low-level veil over the whole screen (UI BMP was 77% non-zero at mean 10/255).
    if (m < 0.01f) { m = 0.0f; }
    float3 u;
    if (m <= 0.0f) {
        u = 0.0f;
    } else if (Linear != 0) {
        u = pq_encode(max(pv - (1.0f - m) * hv, 0.0f));
    } else {
        u = pv - (1.0f - m) * hv;
    }
    if (marker_pixel(p)) { m = 0.0f; u = 0.0f; }
    Ui[p] = float4(saturate(u), saturate(m));

    // P3b coverage counters. Only on the frames the CPU asked for (Stats != 0): these are
    // per-pixel InterlockedAdds into two addresses, plus a 4-load/4-atomic diagnostic block
    // every 8x8 pixels. At 4K that is ~500k atomics/frame for a 6% UI and ~130k more for
    // the diagnostics - measurable on the render card for a readout that is logged 1 Hz.
    if (StatsOn != 0u) {
        if (m > 0.5f)  { InterlockedAdd(Stats[0], 1u); }
        if (m > 0.01f) { InterlockedAdd(Stats[1], 1u); }
    }
    // Diagnostic sample every 8x8: raw code maxima. If H is black this is ~0 even when
    // the scene is bright; if H is a valid scene these two means are close.
    if (StatsOn != 0u && ((p.x | p.y) & 7) == 0) {
        const float ph = max(max(hudless_raw.r, hudless_raw.g), hudless_raw.b);
        const float pp = max(max(present_raw.r, present_raw.g), present_raw.b);
        InterlockedAdd(Stats[2], uint(saturate(ph) * 255.0f));
        InterlockedAdd(Stats[3], uint(saturate(pp) * 255.0f));
        InterlockedAdd(Stats[4], 1u);

        const int ix = int(id.x), iy = int(id.y);
        const int fx = int(W) - 1 - ix, fy = int(H) - 1 - iy;
        const float3 hx = Hudless.Load(int3(fx, iy, 0)).rgb;
        const float3 hy = Hudless.Load(int3(ix, fy, 0)).rgb;
        const float3 hxy = Hudless.Load(int3(fx, fy, 0)).rgb;
        InterlockedAdd(Stats[5], uint(saturate(raw_diff(present_raw, hudless_raw)) * 255.0f));
        InterlockedAdd(Stats[6], uint(saturate(raw_diff(present_raw, hx)) * 255.0f));
        InterlockedAdd(Stats[7], uint(saturate(raw_diff(present_raw, hy)) * 255.0f));
        InterlockedAdd(Stats[8], uint(saturate(raw_diff(present_raw, hxy)) * 255.0f));
    }
    }   // else (non-patch)
}
