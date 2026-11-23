// ============================================================================
//  patch_local.hlsl - per-tile local tone residual for the v34 support patch.
//
//  The global 32-bin LUT aligns the average scene-referred H -> display-referred P
//  relationship. It cannot express a *spatially varying* exposure/tonemap difference.
//  This pass samples only clean (alpha=0) pixels inside each occupied 32x8 codec tile,
//  applies the global LUT, and stores
//
//      sum log2( P_nits / (patch_nits * globalLUT) )  + count
//
//  per tile in TileSum. patch_local_fill.hlsl turns that into a smooth per-tile gain
//  map; the compose pass applies it only where alpha > 0.
//
//  Bindings: t0 = present, t1 = patch, t2 = Occ, t3 = global LUT (Gain),
//            u0 = TileSum (float4: sumR,sumG,sumB,count)
// ============================================================================

#define HBINS 32u
#define NCH   3u

cbuffer Params : register(b0)
{
    uint W;
    uint H;
    uint TilesX;
    uint TilesY;
};

Texture2D<float4>        Present : register(t0);
Texture2D<float4>        Patch   : register(t1);
StructuredBuffer<uint>   Occ     : register(t2);
StructuredBuffer<float>  Gain    : register(t3);
RWStructuredBuffer<float4> TileSum : register(u0);

float3 pq_decode(float3 e)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float3 p = pow(max(e, 1e-8f), 1.0f / m2);
    const float3 num = max(p - c1, 0.0f);
    const float3 den = max(c2 - c3 * p, 1e-8f);
    return 10000.0f * pow(num / den, 1.0f / m1);
}

float3 apply_lut(float3 nits)
{
    const float conf = Gain[NCH * HBINS];
    if (conf <= 0.1f) { return nits; }
    [unroll] for (uint ch = 0u; ch < NCH; ++ch)
    {
        const float logl = log2(max(nits[ch], 1e-4f));
        const float pos = clamp((logl + 4.0f) / 16.0f * 32.0f - 0.5f, 0.0f, 31.0f);
        const int b0 = (int)floor(pos);
        const int b1 = min(b0 + 1, 31);
        const float f = pos - (float)b0;
        nits[ch] *= clamp(lerp(Gain[ch * HBINS + b0],
                               Gain[ch * HBINS + b1], f), 0.02f, 8.0f);
    }
    return nits;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint tile = id.x;
    const uint tiles = TilesX * TilesY;
    if (tile >= tiles) { return; }
    if (Occ[tile] == 0u) {
        TileSum[tile] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const uint tx = tile % TilesX;
    const uint ty = tile / TilesX;
    const int x0 = int(tx * 32u);
    const int y0 = int(ty * 8u);

    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float count = 0.0f;

    // 8 probes per 32x8 tile (alpha=0 only). The tile is already transmitted because
    // at least one group hit; these probes supply the local tone reference.
    [unroll] for (uint sy = 0u; sy < 2u; ++sy)
    {
        [unroll] for (uint sx = 0u; sx < 4u; ++sx)
        {
            const int x = x0 + 4 + int(sx) * 8;
            const int y = y0 + 2 + int(sy) * 4;
            if (x >= (int)W || y >= (int)H) { continue; }

            const float4 u = Patch.Load(int3(x, y, 0));
            if (u.a >= 0.5f) { continue; }          // UI pixel: no ground truth
            // Reject the AA ring around support too: those pixels can still contain UI
            // colour in present even when the binary patch alpha is 0.
            bool near_support = false;
            [unroll] for (int oy = -2; oy <= 2; ++oy) {
                [unroll] for (int ox = -2; ox <= 2; ++ox) {
                    const int qx = clamp(x + ox, 0, (int)W - 1);
                    const int qy = clamp(y + oy, 0, (int)H - 1);
                    if (Patch.Load(int3(qx, qy, 0)).a > 0.1f) { near_support = true; }
                }
            }
            if (near_support) { continue; }

            const float4 p = Present.Load(int3(x, y, 0));
            const float3 hn = apply_lut(pq_decode(u.rgb));
            const float3 pn = pq_decode(p.rgb);
            const float hl = dot(hn, float3(0.2126f, 0.7152f, 0.0722f));
            const float pl = dot(pn, float3(0.2126f, 0.7152f, 0.0722f));
            if (hl <= 0.5f || pl <= 0.5f) { continue; }

            const float3 r = pn / max(hn, float3(1e-4f, 1e-4f, 1e-4f));
            const float3 lr = clamp(log2(max(r, 1e-4f)), -4.0f, 4.0f);
            sum += lr;
            count += 1.0f;
        }
    }
    TileSum[tile] = float4(sum, count);
}
