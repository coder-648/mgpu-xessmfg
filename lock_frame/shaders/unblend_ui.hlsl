// Consumer-side UI unblend (feeds XeFG mode 5; mode 4 with --no-ui-mode5).
//
// Rebuilds a HUDless frame from the presented image (which already contains UI) and
// the UI-only texture the producer transported:
//
//   premultiplied: hudless = (present - ui.rgb) / max(1 - ui.a, eps)
//   straight:      hudless = (present - ui.rgb * ui.a) / max(1 - ui.a, eps)
//
// The UI texture may be smaller than the present image; it is sampled with bilinear
// filtering at the present pixel's UV. Both images must be in the same colour space
// (the producer converts scRGB -> PQ in downsample_ui.hlsl when needed).
//
// Opaque UI has no recoverable scene behind it, so the shader writes black there (see the
// branch below): keeping the presented colour would leave the HUD in the "HUDless" and the
// generated frames would interpolate it. XeFG's mode 4 takes the opaque UI from the
// backbuffer, so the scene under it is never shown.

cbuffer Params : register(b0)
{
    uint  DstW;
    uint  DstH;
    uint  UiW;
    uint  UiH;
    float Premultiplied;   // 1 = UI.rgb already premultiplied
    float OpaqueAlpha;     // e.g. 0.99
    float Linear;          // v31/P4b: 1 = do the subtraction in PQ-decoded linear nits
    uint  Sparse;          // 1 = Occ (t2) is bound and valid for this dispatch
    uint  Patch;           // v33: 1 = stream 3 is the HUDless patch (scene under UI),
                           //      not a UI plane. H' = present outside the support, patch
                           //      inside it; no subtraction, no alpha, no division.
    uint  Local;           // v34: 1 = LocalGain (t4) has a valid per-tile tone field
    uint  DisplayRef;      // v34: 1 = patch is already display-referred (WGC-matched)
    uint  Use3D;           // v34: 1 = Lut (t5) is a valid H->P 3D LUT
    uint  LutN;            // 3D LUT edge length (17): nodes live at i/(LutN-1)
};

Texture2D<float4>    Present : register(t0);   // R10G10B10A2_UNORM, with UI
Texture2D<float4>    Ui      : register(t1);   // UI-only, lower resolution
StructuredBuffer<uint> Occ   : register(t2);   // per 32x8 codec tile: 0 = no UI
// v33: 32-bin luminance LUT [0..31] + confidence [32], from patch_calib.hlsl/
// patch_gain.hlsl. Only patch mode reads it; the other modes bind a dummy.
StructuredBuffer<float> Gain : register(t3);
// v34: per-tile local tone residual (patch_local.hlsl/fill). rgb = gain, a = 1.
StructuredBuffer<float4> LocalGain : register(t4);
// v34: H(scene code) -> P(WGC code) 3D LUT.
Texture3D<float4>   Lut : register(t5);
SamplerState         Smp     : register(s0);
RWTexture2D<float4>  Hudless : register(u0);   // R10G10B10A2_UNORM

float3 pq_encode(float3 nits)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float3 pq_decode(float3 e)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float3 p = pow(max(e, 1e-8f), 1.0f / m2);
    const float3 num = max(p - c1, 0.0f);
    const float3 den = max(c2 - c3 * p, 1e-8f);
    return 10000.0f * pow(num / den, 1.0f / m1);
}

// Bilinear sample of the per-32x8-tile local tone gain. The producer tile grid is
// (DstW+31)/32 x (DstH+7)/8; the gain is 1 outside the corrected region.
float3 local_gain_at(uint2 p)
{
    const uint tx = (DstW + 31u) >> 5;
    const uint ty = (DstH + 7u) >> 3;
    const float2 gc = float2((float)p.x / 32.0f, (float)p.y / 8.0f) - 0.5f;
    const int2 c0 = int2(floor(gc));
    const float2 f = frac(gc);
    const int x0 = clamp(c0.x, 0, (int)tx - 1);
    const int y0 = clamp(c0.y, 0, (int)ty - 1);
    const int x1 = clamp(c0.x + 1, 0, (int)tx - 1);
    const int y1 = clamp(c0.y + 1, 0, (int)ty - 1);
    const float3 g00 = LocalGain[uint(y0) * tx + uint(x0)].rgb;
    const float3 g10 = LocalGain[uint(y0) * tx + uint(x1)].rgb;
    const float3 g01 = LocalGain[uint(y1) * tx + uint(x0)].rgb;
    const float3 g11 = LocalGain[uint(y1) * tx + uint(x1)].rgb;
    return lerp(lerp(g00, g10, f.x), lerp(g01, g11, f.x), f.y);
}

// Tetrahedral interpolation of the H->P 3D LUT. Nodes are at integer texture
// coordinates i/(LutN-1), so this avoids both the half-texel coordinate issue and
// the extra smoothing of hardware trilinear filtering.
float3 lut3d_tetrahedral(float3 code)
{
    const int n = max((int)LutN, 2);
    const float3 pos = saturate(code) * float(n - 1);
    int3 i0 = int3(floor(pos));
    i0 = clamp(i0, int3(0, 0, 0), int3(n - 1, n - 1, n - 1));
    const int3 i1 = min(i0 + int3(1, 1, 1), int3(n - 1, n - 1, n - 1));
    const float3 f = pos - float3(i0);

    const float3 c000 = Lut.Load(int4(i0.x, i0.y, i0.z, 0)).rgb;
    const float3 c100 = Lut.Load(int4(i1.x, i0.y, i0.z, 0)).rgb;
    const float3 c010 = Lut.Load(int4(i0.x, i1.y, i0.z, 0)).rgb;
    const float3 c001 = Lut.Load(int4(i0.x, i0.y, i1.z, 0)).rgb;
    const float3 c110 = Lut.Load(int4(i1.x, i1.y, i0.z, 0)).rgb;
    const float3 c101 = Lut.Load(int4(i1.x, i0.y, i1.z, 0)).rgb;
    const float3 c011 = Lut.Load(int4(i0.x, i1.y, i1.z, 0)).rgb;
    const float3 c111 = Lut.Load(int4(i1.x, i1.y, i1.z, 0)).rgb;

    if (f.x > f.y) {
        if (f.y > f.z) {
            return c000 + f.x * (c100 - c000) + f.y * (c110 - c100) + f.z * (c111 - c110);
        } else if (f.x > f.z) {
            return c000 + f.x * (c100 - c000) + f.z * (c101 - c100) + f.y * (c111 - c101);
        }
        return c000 + f.z * (c001 - c000) + f.x * (c101 - c001) + f.y * (c111 - c101);
    }
    if (f.z > f.y) {
        return c000 + f.z * (c001 - c000) + f.y * (c011 - c001) + f.x * (c111 - c011);
    }
    if (f.z > f.x) {
        return c000 + f.y * (c010 - c000) + f.z * (c011 - c010) + f.x * (c111 - c011);
    }
    return c000 + f.y * (c010 - c000) + f.x * (c110 - c010) + f.z * (c111 - c110);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DstW || id.y >= DstH) { return; }

    const int2 p = int2(id.xy);
    const float4 present = Present.Load(int3(p, 0));

    // v33: HUDless support patch (XeFG mode 4). The patch carries the scene under the UI
    // exactly as the game's HUDless produced it; outside the support `present` already IS
    // the HUDless (no UI there), so the composition is a copy, not an inverse:
    //
    //   H' = present                     (tile has no UI)
    //   H' = patch                       (tile has UI; no (P-U)/(1-a) anywhere)
    //
    // Occupancy comes from the sparse codec headers, so a tile is either fully patched or
    // fully copied. The patch is full-resolution and 1:1, hence Load, not SampleLevel.
    if (Patch != 0u) {
        if (Sparse != 0u) {
            const uint tiles_x = (DstW + 31u) >> 5;
            const uint tile    = (id.y >> 3) * tiles_x + (id.x >> 5);
            if (Occ[tile] != 0u) {
                const float4 u = Ui.Load(int3(p, 0));
                // v34: support mask recall + outward feather. Inside the producer's
                // support (alpha=1) use patch; just outside it, blend to present over a
                // 7x7 support-coverage band. The band lies outside the UI, so present is
                // clean there; this does not mix UI into H'. It removes the hard 1-2 px
                // boundary left by a pure dilate.
                float blend = saturate(u.a);
                if (blend < 0.5f) {
                    float cov = 0.0f;
                    [unroll] for (int oy = -3; oy <= 3; ++oy) {
                        [unroll] for (int ox = -3; ox <= 3; ++ox) {
                            const int2 q = clamp(p + int2(ox, oy),
                                                 int2(0, 0), int2((int)DstW - 1, (int)DstH - 1));
                            cov += Ui.Load(int3(q, 0)).a;
                        }
                    }
                    cov /= 49.0f;
                    blend = smoothstep(0.02f, 0.35f, cov);
                }
                blend = saturate(blend);
                if (blend <= 0.001f) {
                    Hudless[id.xy] = float4(saturate(present.rgb), 1.0);
                    return;
                }
                // v34: global H->P 3D LUT. This is the deterministic color-management
                // step between scene H and the WGC display chain.
                if (Use3D != 0u) {
                    // Tetrahedral interpolation samples the LUT nodes directly; the
                    // node grid is i/(LutN-1), so no half-texel offset is involved.
                    const float3 pc = lut3d_tetrahedral(saturate(u.rgb));
                    Hudless[id.xy] = float4(saturate(lerp(present.rgb, pc, blend)), 1.0);
                    return;
                }
                // v34: display-referred source still needs one global mapping from the
                // producer's display chain to the WGC capture chain. The local tone field
                // is not used here; the remaining difference is global.
                const float3 lg = (Local != 0u) ? local_gain_at(id.xy)
                                                : float3(1.0f, 1.0f, 1.0f);
                // v33 dynamic tone/gamut calibration: per-channel 32-bin LUTs built from
                // clean (alpha=0) pixels, plus the v34 per-tile local residual.
                const float conf = Gain[96];
                if (conf <= 0.1f && all(abs(lg - 1.0f) < 0.001f)) {
                    Hudless[id.xy] = float4(saturate(lerp(present.rgb, u.rgb, blend)), 1.0);
                    return;
                }
                float3 nits = pq_decode(u.rgb);
                if (conf > 0.1f) {
                    [unroll] for (uint ch = 0u; ch < 3u; ++ch) {
                        const float logl = log2(max(nits[ch], 1e-4f));
                        const float pos = clamp((logl + 4.0f) / 16.0f * 32.0f - 0.5f,
                                                0.0f, 31.0f);
                        const int   b0 = (int)floor(pos);
                        const int   b1 = min(b0 + 1, 31);
                        const float f  = pos - (float)b0;
                        const float g  = lerp(Gain[ch * 32u + b0],
                                              Gain[ch * 32u + b1], f);
                        nits[ch] *= clamp(g, 0.02f, 8.0f);
                    }
                }
                nits *= lg;
                const float3 h = pq_encode(clamp(nits, 0.0f, 10000.0f));
                Hudless[id.xy] = float4(saturate(lerp(present.rgb, h, blend)), 1.0);
                return;
            }
        }
        // No occupancy (codec off) means the patch is not trustworthy: keep present.
        Hudless[id.xy] = float4(saturate(present.rgb), 1.0);
        return;
    }

    // Sparse path: the sparse UI codec marks an empty 32x8 tile with mode=3, and the
    // consumer derives Occ from those headers (shaders/ui_occ.hlsl). For such a tile the
    // decoder wrote zeros, so ui.a == 0 everywhere and the arithmetic below reduces to
    // "write present" - skip the bilinear UI sample and both PQ pow chains instead.
    // Only enabled for a full-resolution UI stream (a half-res sample could pull a
    // neighbouring texel into an "empty" tile), see record_ui_unblend().
    if (Sparse != 0u) {
        const uint tiles_x = (DstW + 31u) >> 5;
        const uint tile    = (id.y >> 3) * tiles_x + (id.x >> 5);
        if (Occ[tile] == 0u) {
            Hudless[id.xy] = float4(saturate(present.rgb), 1.0);
            return;
        }
    }

    const float2 uv = (float2(id.xy) + 0.5) / float2((float)DstW, (float)DstH);
    float4 ui = Ui.SampleLevel(Smp, uv, 0);
    if (Premultiplied < 0.5) { ui.rgb *= ui.a; }

    const float inv = 1.0 - ui.a;
    float3 h;
    if (ui.a < OpaqueAlpha) {
        if (Linear > 0.5f) {
            // P4b: both sides are R10 PQ (HDR10/BT.2020); decode to nits, subtract, encode.
            const float3 pn = pq_decode(present.rgb);
            const float3 un = pq_decode(ui.rgb);
            h = pq_encode(max((pn - un) / max(inv, 1e-5f), 0.0f));
        } else {
            h = saturate((present.rgb - ui.rgb) / max(inv, 1e-5));
        }
    } else {
        // Opaque UI: nothing of the scene is recoverable, and keeping the presented colour
        // here would bake the HUD into the "HUDless" - mode 4 then interpolates a frame that
        // still contains the HUD, which is exactly the ghosting this pass is supposed to
        // remove. OptiScaler's UiUnblend writes black for this branch ("black still differs
        // from the UI so FG treats it as UI"); mode 4 extracts the opaque UI from the
        // backbuffer, so the scene under it never reaches the screen.
        h = 0.0;
    }

    Hudless[id.xy] = float4(saturate(h), 1.0);
}
