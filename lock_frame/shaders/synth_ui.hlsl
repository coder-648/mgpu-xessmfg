// ============================================================================
//  synth_ui.hlsl - consumer-side UI synthesis for the HUDless stream (v29).
//
//  Stream 3 normally carries the title's UI-only texture (RGB + alpha). When the
//  title has no UI tag, the producer sends a downsampled HUDless frame instead and
//  this pass reconstructs an approximate UI from the full-resolution WGC present:
//
//      P = present (with UI), H = HUDless (scene only)
//      U = P - (1 - m) * H,  a = m
//      => U + (1 - a) * H = P   (real frame is exact by construction)
//
//  m is a difference mask, not the true UI alpha: a single (P, H) pair cannot
//  uniquely recover (RGB, alpha). Opaque HUD (crosshair, bars, text) is accurate;
//  semi-transparent or additive UI is approximate. This is the same trade-off the
//  XeFG BACKBUFFER_HUDLESS mode documents, and the final UI is still extracted from
//  the full-resolution backbuffer by XeFG, so the goal here is a clean HUDless base,
//  not a display-quality UI texture.
//
//  Bindings match unblend_ui.hlsl so the existing root signature/descriptor heap can
//  be reused:
//      t0 = present (R10 PQ), t1 = HUDless (low res), u0 = synthesized UI (low res)
// ============================================================================

cbuffer Params : register(b0)
{
    uint  DstW;          // synthesized UI width  (= stream 3 width)
    uint  DstH;
    uint  SrcW;          // present width (for the sample footprint)
    uint  SrcH;
    float Threshold;     // normalized difference threshold
    float Dilate;        // reserved (0 = off)
    float pad0;
    float pad1;
};

Texture2D<float4>    Present : register(t0);   // full-resolution, contains UI
Texture2D<float4>    Hudless : register(t1);   // low-resolution, scene only
SamplerState         Smp     : register(s0);
RWTexture2D<float4>  Ui      : register(u0);   // low-resolution, premultiplied

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DstW || id.y >= DstH) { return; }

    // Average the four full-resolution samples inside this low-resolution pixel.
    // This is a cheap box filter, close to the producer's 2x2 downsample, so scene
    // detail does not turn into false UI.
    const float2 uv = (float2(id.xy) + 0.5) / float2((float)DstW, (float)DstH);
    const float2 o  = 0.5 / float2((float)DstW, (float)DstH);
    float3 p = 0.25 * (Present.SampleLevel(Smp, uv + float2(-o.x, -o.y), 0).rgb +
                       Present.SampleLevel(Smp, uv + float2( o.x, -o.y), 0).rgb +
                       Present.SampleLevel(Smp, uv + float2(-o.x,  o.y), 0).rgb +
                       Present.SampleLevel(Smp, uv + float2( o.x,  o.y), 0).rgb);

    const float3 h = Hudless.Load(int3(id.xy, 0)).rgb;
    const float3 d = p - h;
    const float delta = max(max(abs(d.r), abs(d.g)), abs(d.b));

    float m;
    if (Threshold <= 1e-5) {
        m = (delta > 0.002f) ? 1.0f : 0.0f;
    } else {
        m = smoothstep(Threshold, Threshold * 2.0f, delta);
    }

    // Premultiplied UI colour that reproduces the present image when composited over H.
    // The alpha is the heuristic mask; m = 0 keeps H, m = 1 freezes the pixel's UI.
    const float3 u = p - (1.0f - m) * h;
    Ui[id.xy] = float4(saturate(u), saturate(m));
}
