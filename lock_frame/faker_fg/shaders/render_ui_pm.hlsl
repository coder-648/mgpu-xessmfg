// ============================================================================
//  render_ui_pm.hlsl - OptiScaler shaders/render_ui/precompile/render_ui_pm.hlsl,
//  verbatim: the straight-alpha ("source over") variant, used when
//  FGUIPremultipliedAlpha is 0.
// ============================================================================
Texture2D<float4> UI : register(t0);
Texture2D<float4> BackBuffer : register(t1);
SamplerState Sampler : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 PSMain(VSOut i) : SV_Target
{
    float4 dst = BackBuffer.Sample(Sampler, i.uv); // dst = current swapchain color
    float4 src = UI.Sample(Sampler, i.uv);         // src = UI color

    // Straight-alpha "source over":
    float a = saturate(src.a);
    float3 outRgb = src.rgb * a + dst.rgb * (1.0 - a);
    return float4(outRgb, 1.0);
}
