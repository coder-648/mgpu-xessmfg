// ============================================================================
//  patch_lut3d_decay.hlsl - exponential decay of the H->P histogram accumulator.
//
//  patch_lut3d.hlsl adds this frame's samples to LutSum (sum of P codes + count per
//  H-code cell); clearing it every frame makes the LUT a *single-frame* estimate. The
//  sample set changes every frame (stride-4 pixels), so cells with few samples (dark
//  cells, thin UI surroundings) get a different mean each frame -> LUT(patch) flickers,
//  and XeFG's UI detection (backbuffer - H') sees that as a changing UI region.
//
//  Keeping the last ~1/(1-retain) frames as an exponential average makes the LUT stable
//  while it still follows scene changes within a few frames. Cost: 33^3 * 4 words.
//
//  Bindings: u0 = LutSum [cell*4 + {r,g,b,count}], b0 = {Cells, RetainPercent, 0, 0}
// ============================================================================

cbuffer Params : register(b0)
{
    uint Cells;
    uint Retain;      // percent kept per frame (0..99). 0 is handled on the CPU (clear).
    uint Pad0;
    uint Pad1;
};

RWStructuredBuffer<uint> LutSum : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint cell = id.x;
    if (cell >= Cells) { return; }
    // Round to nearest so the sum/count ratio survives; the count keeps a floor of 1 so a
    // cell that has data never turns into "empty" (which would switch the fill pass to its
    // neighbour fallback).
    [unroll] for (uint k = 0u; k < 4u; ++k) {
        const uint idx = cell * 4u + k;
        const uint v   = LutSum[idx];
        uint nv = (v * Retain + 50u) / 100u;
        if (k == 3u && v != 0u && nv == 0u) { nv = 1u; }
        LutSum[idx] = nv;
    }
}
