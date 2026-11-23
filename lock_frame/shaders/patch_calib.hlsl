// ============================================================================
//  patch_calib.hlsl - per-frame tone/exposure calibration for the v33 patch.
//
//  Clean (alpha=0) pixels inside the occupied tiles describe the same scene twice:
//  `patch` is the producer's PQ(H) and `present` is the consumer's display-referred PQ.
//  Their nits ratio is the missing reference-white / exposure / tone-curve factor.
//
//  A single global ratio is not enough: the game's tonemap compresses highlights, so the
//  ratio depends on scene luminance. This pass builds a 2D histogram
//      [32 luminance bins] x [32 log2 ratio bins]
//  and patch_gain.hlsl turns each luminance row into a gain (median) -> a 32-entry
//  luminance LUT. Applying one LUT gain to R/G/B preserves white balance.
//
//  Bindings: t0 = present (R10 PQ), t1 = patch (R10 PQ + UI alpha),
//            t2 = Occ (uint per 32x8 tile), u0 = hist, u1 = total
// ============================================================================

#define HBINS    32u
#define RBINS    32u
#define NCH      3u
#define HLOG_MIN -4.0
#define HLOG_MAX 12.0
#define RLOG_MIN -4.0
#define RLOG_MAX 4.0

cbuffer Params : register(b0)
{
    uint  W;
    uint  H;
    uint  UiW;
    uint  UiH;
    uint  Stride;      // sample one pixel every Stride in x and y
    uint  Bins;        // HBINS (informational)
    float LogMin;      // RLOG_MIN
    float LogMax;      // RLOG_MAX
};

Texture2D<float4>        Present : register(t0);
Texture2D<float4>        Patch   : register(t1);
StructuredBuffer<uint>   Occ     : register(t2);
RWStructuredBuffer<uint> Hist    : register(u0);   // [HBINS][RBINS]
RWStructuredBuffer<uint> Total   : register(u1);   // [HBINS] valid, [HBINS..] candidates

float3 pq_decode(float3 e)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float3 p = pow(max(e, 1e-8f), 1.0f / m2);
    const float3 num = max(p - c1, 0.0f);
    const float3 den = max(c2 - c3 * p, 1e-8f);
    return 10000.0f * pow(num / den, 1.0f / m1);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint x = id.x * Stride + (Stride >> 1);
    const uint y = id.y * Stride + (Stride >> 1);
    if (x >= W || y >= H) { return; }

    const uint tiles_x = (W + 31u) >> 5;
    const uint tile    = (y >> 3) * tiles_x + (x >> 5);
    if (Occ[tile] == 0u) { return; }

    const float4 ui = Patch.Load(int3(int(x), int(y), 0));
    if (ui.a >= 0.5f) { return; }      // UI pixel: not a scene sample
    // Reject the AA ring around support; present there can still contain UI colour.
    [unroll] for (int oy = -2; oy <= 2; ++oy) {
        [unroll] for (int ox = -2; ox <= 2; ++ox) {
            const int qx = clamp(int(x) + ox, 0, int(W) - 1);
            const int qy = clamp(int(y) + oy, 0, int(H) - 1);
            if (Patch.Load(int3(qx, qy, 0)).a > 0.1f) { return; }
        }
    }

    const float4 pv = Present.Load(int3(int(x), int(y), 0));
    const float3 un = pq_decode(ui.rgb);
    const float3 pn = pq_decode(pv.rgb);
    const float ul = 0.2126f * un.r + 0.7152f * un.g + 0.0722f * un.b;
    const float pl = 0.2126f * pn.r + 0.7152f * pn.g + 0.0722f * pn.b;

    if (ul > 0.5f && pl > 0.5f)
    {
        // Per-channel tone curves: the scene-referred patch and the display-referred
        // present differ per channel (gamut + tonemap), so a single luminance gain
        // desaturates the result. Histogram each channel against its own log2 nits.
        [unroll] for (uint ch = 0u; ch < NCH; ++ch)
        {
            const float uc = un[ch];
            const float pc = pn[ch];
            if (uc <= 0.5f || pc <= 0.5f) { continue; }
            const float hl = log2(uc);
            int hb = (int)((hl - HLOG_MIN) / (HLOG_MAX - HLOG_MIN) * (float)HBINS);
            hb = clamp(hb, 0, (int)HBINS - 1);
            const float lr = log2(pc / uc);
            if (lr >= RLOG_MIN && lr <= RLOG_MAX)
            {
                int rb = (int)((lr - RLOG_MIN) / (RLOG_MAX - RLOG_MIN) * (float)RBINS);
                rb = clamp(rb, 0, (int)RBINS - 1);
                InterlockedAdd(Hist[((uint)ch * HBINS + (uint)hb) * RBINS + (uint)rb], 1u);
                InterlockedAdd(Total[(uint)ch * 2u * HBINS + (uint)hb], 1u);
            }
            InterlockedAdd(Total[((uint)ch * 2u + 1u) * HBINS + (uint)hb], 1u);
        }
    }
}

