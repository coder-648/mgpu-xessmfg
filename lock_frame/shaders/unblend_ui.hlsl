// Consumer-side UI unblend (B mode 4).
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
// Opaque UI has no recoverable scene behind it. We keep the presented colour for those
// pixels instead of producing a black halo; XeFG's mode 4 only uses the reconstructed
// HUDless as the interpolation base, and the opaque UI is extracted from the backbuffer.

cbuffer Params : register(b0)
{
    uint  DstW;
    uint  DstH;
    uint  UiW;
    uint  UiH;
    float Premultiplied;   // 1 = UI.rgb already premultiplied
    float OpaqueAlpha;     // e.g. 0.99
    float pad0;
    float pad1;
};

Texture2D<float4>    Present : register(t0);   // R10G10B10A2_UNORM, with UI
Texture2D<float4>    Ui      : register(t1);   // UI-only, lower resolution
SamplerState         Smp     : register(s0);
RWTexture2D<float4>  Hudless : register(u0);   // R10G10B10A2_UNORM

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DstW || id.y >= DstH) { return; }

    const int2 p = int2(id.xy);
    const float4 present = Present.Load(int3(p, 0));

    const float2 uv = (float2(id.xy) + 0.5) / float2((float)DstW, (float)DstH);
    float4 ui = Ui.SampleLevel(Smp, uv, 0);
    if (Premultiplied < 0.5) { ui.rgb *= ui.a; }

    const float inv = 1.0 - ui.a;
    float3 h = present.rgb;
    if (inv > 0.001) {
        h = (present.rgb - ui.rgb) / inv;
    } else if (OpaqueAlpha > 0.5) {
        // No recoverable scene behind opaque UI. Keep the presented colour so the
        // interpolated edge does not darken; mode 4's UI extraction supplies the UI.
        h = present.rgb;
    }

    Hudless[id.xy] = float4(saturate(h), 1.0);
}
