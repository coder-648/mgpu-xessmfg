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
};

Texture2D<float4>    Present : register(t0);   // R10G10B10A2_UNORM, with UI
Texture2D<float4>    Ui      : register(t1);   // UI-only, lower resolution
StructuredBuffer<uint> Occ   : register(t2);   // per 32x8 codec tile: 0 = no UI
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

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DstW || id.y >= DstH) { return; }

    const int2 p = int2(id.xy);
    const float4 present = Present.Load(int3(p, 0));

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
