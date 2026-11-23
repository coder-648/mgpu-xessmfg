// ============================================================================
//  hudless_to_pq.hlsl - HUDless snapshot conversion for the UI synth path.
//
//  CP2077 tags HUDless as FP16 scRGB. The old snapshot did a full-resolution
//  CopyResource (66 MB read + 66 MB write). The synth only needs H, and its
//  transport convention is R10 PQ, so convert once here:
//
//      H_fp16 (scRGB linear) -> H_r10 (PQ, 33 MB write)
//
//  The producer then reads the R10 H (33 MB) instead of FP16 (66 MB), and the
//  synth's existing HSrcScRGB=0 path already handles PQ.
// ============================================================================
Texture2D<float4>   Hudless : register(t0);
RWTexture2D<float4> OutPq   : register(u0);

static const float3x3 k709to2020 = float3x3(
    0.6274, 0.3293, 0.0433,
    0.0691, 0.9195, 0.0114,
    0.0164, 0.0880, 0.8956);

float3 pq_encode(float3 nits)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 p = id.xy;
    const float3 c = Hudless.Load(int3(p, 0)).rgb;
    const float3 nits = mul(k709to2020, c * 80.0f);
    OutPq[p] = float4(pq_encode(nits), 1.0f);
}
