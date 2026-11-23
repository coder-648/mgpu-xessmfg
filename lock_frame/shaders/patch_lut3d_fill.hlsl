// ============================================================================
//  patch_lut3d_fill.hlsl - fill the 17^3 H->P color LUT.
//
//  Occupied cells use the average paired P code. Empty cells are filled from valid
//  3D neighbours; if none exist, use the identity (the H code itself), so the LUT
//  never invents a colour.
//
//  Bindings: t0 = LutSum, u0 = Texture3D<float4> LUT
// ============================================================================

cbuffer Params : register(b0)
{
    uint N;
    uint pad0;
    uint pad1;
    uint pad2;
};

StructuredBuffer<uint> LutSum : register(t0);
RWTexture3D<float4>    LUT    : register(u0);

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= N || id.y >= N || id.z >= N) { return; }
    const uint cell = (id.z * N + id.y) * N + id.x;
    const uint count = LutSum[cell * 4u + 3u];
    float3 v = float3(id) / float(N - 1u);
    if (count > 0u) {
        v = float3(LutSum[cell * 4u + 0u], LutSum[cell * 4u + 1u],
                   LutSum[cell * 4u + 2u]) / (float(count) * 1023.0f);
    } else {
        float3 acc = float3(0.0f, 0.0f, 0.0f);
        float wsum = 0.0f;
        [loop] for (int oz = -2; oz <= 2; ++oz) {
            [loop] for (int oy = -2; oy <= 2; ++oy) {
                [loop] for (int ox = -2; ox <= 2; ++ox) {
                    const int nx = int(id.x) + ox;
                    const int ny = int(id.y) + oy;
                    const int nz = int(id.z) + oz;
                    if (nx < 0 || ny < 0 || nz < 0 ||
                        nx >= int(N) || ny >= int(N) || nz >= int(N)) { continue; }
                    const uint nc = (uint(nz) * N + uint(ny)) * N + uint(nx);
                    const uint n = LutSum[nc * 4u + 3u];
                    if (n == 0u) { continue; }
                    const float w = 1.0f / (1.0f + float(ox * ox + oy * oy + oz * oz));
                    acc += float3(LutSum[nc * 4u + 0u], LutSum[nc * 4u + 1u],
                                  LutSum[nc * 4u + 2u]) / (float(n) * 1023.0f) * w;
                    wsum += w;
                }
            }
        }
        if (wsum > 0.0f) { v = acc / wsum; }
    }
    LUT[id] = float4(saturate(v), 1.0f);
}
