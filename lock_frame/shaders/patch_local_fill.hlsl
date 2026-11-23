// ============================================================================
//  patch_local_fill.hlsl - fill the per-tile local tone gain map.
//
//  A tile with clean probes uses exp2(mean log2 ratio). Tiles fully covered by UI have
//  no clean probes; fill them from valid neighbours (normalized convolution) so the
//  local tone field is continuous instead of jumping to 1 at the HUD.
//
//  Bindings: t0 = TileSum (float4), u0 = TileGain (float4 per 32x8 tile)
// ============================================================================

#define RADIUS 2

cbuffer Params : register(b0)
{
    uint TilesX;
    uint TilesY;
    uint pad0;
    uint pad1;
};

StructuredBuffer<float4>   TileSum  : register(t0);
RWStructuredBuffer<float4> TileGain : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint tile = id.x;
    const uint tiles = TilesX * TilesY;
    if (tile >= tiles) { return; }

    const float4 own = TileSum[tile];
    float3 gain = float3(1.0f, 1.0f, 1.0f);
    if (own.w > 0.5f) {
        gain = exp2(own.xyz / own.w);
    } else {
        const int tx = int(tile % TilesX);
        const int ty = int(tile / TilesX);
        float3 acc = float3(0.0f, 0.0f, 0.0f);
        float wsum = 0.0f;
        [loop] for (int oy = -RADIUS; oy <= RADIUS; ++oy)
        {
            [loop] for (int ox = -RADIUS; ox <= RADIUS; ++ox)
            {
                const int nx = tx + ox;
                const int ny = ty + oy;
                if (nx < 0 || ny < 0 || nx >= (int)TilesX || ny >= (int)TilesY) { continue; }
                const float4 n = TileSum[uint(ny) * TilesX + uint(nx)];
                if (n.w <= 0.5f) { continue; }
                const float w = 1.0f / (1.0f + float(ox * ox + oy * oy));
                acc += exp2(n.xyz / n.w) * w;
                wsum += w;
            }
        }
        if (wsum > 0.0f) { gain = acc / wsum; }
    }
    gain = clamp(gain, 0.25f, 4.0f);
    TileGain[tile] = float4(gain, 1.0f);
}
