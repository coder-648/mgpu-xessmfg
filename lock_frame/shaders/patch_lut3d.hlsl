// ============================================================================
//  patch_lut3d.hlsl - build a per-frame 3D color transform from scene H to WGC P.
//
//  The clean (alpha=0) pixels give paired R10 codes:
//      Patch(H)  = scene-referred HUDless
//      Present(P)= WGC display (after XeFG's display conversion)
//  A 17^3 LUT in code space captures the game's tonemap/gamut/cross-channel
//  relationship without any spatial filtering. patch_lut3d_fill.hlsl fills the
//  texture; the compose pass samples it for support pixels.
//
//  Bindings: t0 = present, t1 = patch, t2 = Occ, u0 = LutSum [cell*4 + {r,g,b,count}]
// ============================================================================

cbuffer Params : register(b0)
{
    uint W;
    uint H;
    uint Stride;
    uint N;
};

Texture2D<float4>        Present : register(t0);
Texture2D<float4>        Patch   : register(t1);
StructuredBuffer<uint>   Occ     : register(t2);
RWStructuredBuffer<uint> LutSum  : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint x = id.x * Stride + (Stride >> 1);
    const uint y = id.y * Stride + (Stride >> 1);
    if (x >= W || y >= H) { return; }
    const uint tiles_x = (W + 31u) >> 5;
    const uint tile = (y >> 3) * tiles_x + (x >> 5);
    if (Occ[tile] == 0u) { return; }

    const float4 u = Patch.Load(int3(int(x), int(y), 0));
    if (u.a >= 0.5f) { return; }

    // Exclude the AA ring around support: present there can still contain UI.
    [unroll] for (int oy = -2; oy <= 2; ++oy) {
        [unroll] for (int ox = -2; ox <= 2; ++ox) {
            const int qx = clamp(int(x) + ox, 0, int(W) - 1);
            const int qy = clamp(int(y) + oy, 0, int(H) - 1);
            if (Patch.Load(int3(qx, qy, 0)).a > 0.1f) { return; }
        }
    }

    const float4 p = Present.Load(int3(int(x), int(y), 0));
    const uint3 c = uint3(
        min(uint(saturate(u.r) * float(N - 1u) + 0.5f), N - 1u),
        min(uint(saturate(u.g) * float(N - 1u) + 0.5f), N - 1u),
        min(uint(saturate(u.b) * float(N - 1u) + 0.5f), N - 1u));
    const uint cell = (c.z * N + c.y) * N + c.x;
    InterlockedAdd(LutSum[cell * 4u + 0u], uint(saturate(p.r) * 1023.0f + 0.5f));
    InterlockedAdd(LutSum[cell * 4u + 1u], uint(saturate(p.g) * 1023.0f + 0.5f));
    InterlockedAdd(LutSum[cell * 4u + 2u], uint(saturate(p.b) * 1023.0f + 0.5f));
    InterlockedAdd(LutSum[cell * 4u + 3u], 1u);
}
