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
    float3 h;
    if (ui.a < OpaqueAlpha) {
        h = saturate((present.rgb - ui.rgb) / max(inv, 1e-5));
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
