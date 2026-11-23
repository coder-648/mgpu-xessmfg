// ui_occ.hlsl - per-tile UI occupancy, derived from the sparse UI codec stream itself.
//
// The sparse encoder (UiSparseCodec=1) writes a 4-word header per 32x8 tile; mode 3 means
// "this tile is empty" and the decoder fills it with zeros. The consumer's unblend only
// has to do arithmetic where UI exists (measured coverage on CP2077: 6.4-6.8%), and for an
// empty tile its result is exactly `present`. So one flag per tile is enough for the
// unblend to skip the UI sample and the two PQ pow chains for ~93% of the pixels.
//
// Deriving it here instead of transporting a mask keeps the protocol at v30: the only new
// data is a consumer-local u32 per tile (~130 KB at 4K).
//
// Bindings: t0 = codec slot bytes (raw/R32_TYPELESS), u0 = occupancy, b0 = {Tiles, SlotWords}

cbuffer Params : register(b0)
{
    uint Tiles;       // tiles_x * tiles_y
    uint SlotWords;   // words per tile slot in the codec (264)
};

ByteAddressBuffer        Src : register(t0);   // encode output: one slot per tile
RWStructuredBuffer<uint> Occ : register(u0);   // 1 = tile has UI, 0 = empty (mode 3)

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint tile = id.x;
    if (tile >= Tiles) { return; }
    const uint hdr  = Src.Load(tile * SlotWords * 4u);
    const uint mode = (hdr >> 6) & 0x3u;
    // mode 3 = the sparse encoder's "empty tile" escape. Any other mode (0/1/2) carries
    // pixels, which may still be all zero - treating them as non-empty is the safe
    // direction (the unblend then produces exactly the same result as before).
    Occ[tile] = (mode == 3u) ? 0u : 1u;
}
