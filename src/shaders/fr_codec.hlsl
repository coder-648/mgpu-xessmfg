// fr_codec.hlsl - the lossless MV/depth codec (docs/design/LOSSLESS_TRANSPORT.md
// sections 6.1 + 14.3, bitstream v2.2) on the GPU.
//
// PRODUCTION shader: this file is the exact kernel that tests/gpu_codec verified on
// both vendors (byte-identical to the CPU reference fr_codec.h, roundtrip bit-exact).
// Build: scripts/build_producer.sh / build_consumer.sh compile BOTH entry points with
// fxc and embed them (kFrEncodeCSO / kFrDecodeCSO).
//
// Host contract (must match):
//   cbuffer b0 : W, H, NW, TilesX, TilesY, SlotWords   (6 x 32-bit)
//   tile       : kTileW x kTileH pixels = 256 pixels; NW = 1 (MV/depth)
//   output     : one "slot" of SlotWords words per tile + TileLen[tile] = used bytes
//   worst case : 4 + 256*NW*4 bytes per tile (raw escape) -> size slots from that
//
// Two entry points, compiled separately (cs_5_0):
//     fxc /T cs_5_0 /E Encode /Fo encode.cso codec.hlsl
//     fxc /T cs_5_0 /E Decode /Fo decode.cso codec.hlsl
//
// Layout: slot per 16x16 tile. Tile t lives at Dst[t * SlotWords] (words) and the
// used byte count is written to TileLen[t]. Inside a slot the bytes are exactly
// what fr::encode_tile() produces (verified by tests/gpu_codec + fr_codec_test).
//
// Shape: one block (256 threads) per tile. Encode: every thread owns one value, the
// tile is built in LDS (per-warp histogram, mask, block prefix sum, per-value atomic
// OR) and flushed coalesced. Decode: parallel unpack, then a per-row ping-pong scan.
// Measured 4K / 33 MB per stream: encode 1.02 ms, decode 0.58-0.72 ms (RTX 3080 Ti).
//
// Integer-only: no float is ever interpreted, so NaN/Inf bit patterns and vendor
// float differences cannot change the bytes.

cbuffer Params : register(b0)
{
    uint W;          // pixels
    uint H;
    uint NW;         // words per pixel (1 = R32/RG16F, 2 = RGBA16F)
    uint TilesX;
    uint TilesY;
    uint SlotWords;  // words per output slot
};

// I/O, production shape:
//   Encode : the game's MV/depth texture (R16G16_FLOAT) -> compressed bytes in the slot
//   Decode : compressed bytes in the slot -> the consumer's g_mv_local[] texture
// The pixel side is a texture in both directions; only the compressed side is a buffer.
// (fxc accepts two resources on one register when each entry point uses only one.)
Texture2D<float2>   SrcTex   : register(t0);   // Encode source
ByteAddressBuffer   SrcBytes : register(t0);   // Decode input
RWByteAddressBuffer DstBytes : register(u0);   // Encode output (slot bytes)
RWTexture2D<float2> DstTex   : register(u0);   // Decode output (local MV texture)
RWStructuredBuffer<uint> TileLen : register(u1);   // used by the offline harness only

static const uint kTileW = 32;     // tile width: 32 px = 128 B per row store (= 1 cache line)
static const uint kTileH = 8;      // tile height (vertical chain = 8)

uint mono(uint v)  { return (v >> 31) ? (v ^ 0xFFFFFFFFu) : (v ^ 0x80000000u); }
uint unmono(uint u) { return (u >> 31) ? (u ^ 0x80000000u) : (u ^ 0xFFFFFFFFu); }
uint zigzag(uint d) { return (d << 1) ^ ((d >> 31) ? 0xFFFFFFFFu : 0u); }
uint unzigzag(uint z) { return (z >> 1) ^ (0u - (z & 1u)); }

int bitlen(uint v)
{
    int n = 0;
    [unroll] for (int i = 0; i < 32; ++i) { if (v) { ++n; v >>= 1; } }
    return n;
}

// value i = (pixel - 1) * NW + lane, pixel-major inside the tile.
// W / NW / Src are globals; tx / ty are the tile origin.
// --- pixel accessors -----------------------------------------------------------
// f16 -> f32 -> f16 is bit-exact for every finite half, so going through the texture
// does not change the byte stream (the harness checks this against the buffer path).
uint ldPix(uint y, uint x) {
    const float2 v = SrcTex.Load(int3((int)x, (int)y, 0));
    return (f32tof16(v.x) & 0xFFFFu) | (f32tof16(v.y) << 16);
}
void stPix(uint y, uint x, uint v) {
    DstTex[int2((int)x, (int)y)] = float2(f16tof32(v & 0xFFFFu), f16tof32(v >> 16));
}

uint zOf(uint i, uint tx, uint ty) {
    const uint p1 = i / NW + 1u;                 // pixel index inside the tile
    const uint l  = i - (p1 - 1u) * NW;
    const uint xx = tx + (p1 % kTileW), yy = ty + (p1 / kTileW);
    const uint idx = (yy * W + xx) * NW + l;
    const uint cur0 = ldPix(yy, xx);
    const uint ref0 = (xx > tx) ? ldPix(yy, xx - 1u) : ldPix(yy - 1u, xx);
    return zigzag(mono(cur0) - mono(ref0));
}

groupshared uint e_px[512];           // encoder: staged pixels
groupshared uint e_hist[264];         // encoder: per-warp bit-length histogram (8 warps x 33)
groupshared uint e_mask[16];
groupshared uint e_meta[2];           // mode, width
groupshared uint e_scan[256];
groupshared uint e_scan2[256];
groupshared uint e_out[520];          // encoder: packed tile (worst case 513 words)
groupshared uint g_mask[16];          // 512 patch bits
groupshared uint g_abs[2];
groupshared uint g_z[512];            // residuals, value order
groupshared uint g_scan[256];
groupshared uint g_scan2[256];
groupshared uint g_col0[32];          // value of column 0 for every row (kTileH x NW)
groupshared uint g_rowsum[512];       // within-row cumulative sums (per pixel)
groupshared uint g_rowsum2[512];      // ping-pong partner for the row scan

// ---------------------------------------------------------------- encode -----
// Fully parallel: every thread owns one value. The tile is built in LDS (bit fields
// OR-ed in with atomics) and flushed to global memory coalesced, so there is no
// serial 255-value loop and no per-value re-computation.
[numthreads(256, 1, 1)]
void Encode(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const uint tile = gid.x;
    const uint t = tid.x;
    if (tile >= TilesX * TilesY) { return; }
    const uint tx = (tile % TilesX) * kTileW;
    const uint ty = (tile / TilesX) * kTileH;
    const uint pw = min(kTileW, W - tx);
    const uint ph = min(kTileH, H - ty);
    const uint np = pw * ph;
    const uint cnt = np * NW - NW;
    // This encoder maps one value to one thread (256 threads = 256 values). That is
    // exactly the MV/depth case (NW=1 -> 255 values). NW=2 (colour) would need 510
    // and is not implemented: colour goes over WGC anyway (section 15.1).
    if (NW != 1u) { return; }

    for (uint k0 = t; k0 < 264u; k0 += 256u) { e_hist[k0] = 0u; }
    if (t < 2u)  { e_meta[t] = 0u; }
    if (t < 16u) { e_mask[t] = 0u; }
    if (t < np) {
        const uint y0 = ty + (t / kTileW), x0 = tx + (t % kTileW);
        for (uint l = 0; l < NW; ++l) { e_px[t * NW + l] = ldPix(y0, x0); }
    }
    for (uint k = t; k < 520u; k += 256u) { e_out[k] = 0u; }
    GroupMemoryBarrierWithGroupSync();

    // residual of this thread's own value (index t, pixel-major, absolute excluded)
    uint z = 0u, bl = 0u;
    if (t < cnt) {
        const uint p1 = t / NW + 1u;
        const uint l  = t - (p1 - 1u) * NW;
        const uint refP = (p1 % kTileW) ? (p1 - 1u) : (p1 - kTileW);
        z = zigzag(mono(e_px[p1 * NW + l]) - mono(e_px[refP * NW + l]));
        bl = (uint)bitlen(z);
        InterlockedAdd(e_hist[(t >> 5) * 33u + bl], 1u);   // per-warp: 8x less contention
    }
    GroupMemoryBarrierWithGroupSync();

    // mode / width decided by one thread from the histogram (33 steps)
    // merge the per-warp histograms into slots 0..32
    if (t < 33u) {
        uint sum = 0u;
        [unroll] for (uint wq = 0; wq < 8u; ++wq) { sum += e_hist[wq * 33u + t]; }
        e_hist[t] = sum;
    }
    GroupMemoryBarrierWithGroupSync();

    if (t == 0u) {
        uint over_[33];
        over_[32] = 0u;
        [unroll] for (int bb = 31; bb >= 0; --bb) { over_[bb] = over_[bb + 1] + e_hist[bb + 1]; }
        const uint headerBits = 32u + NW * 32u;
        uint wmax = 0u;
        [unroll] for (int b2 = 0; b2 <= 32; ++b2) { if (e_hist[b2]) { wmax = (uint)b2; } }
        uint mode, width, best;
        if (wmax >= 32u) { mode = 2u; width = 0u; best = 32u + (cnt + NW) * 32u; }
        else { mode = 0u; width = wmax; best = headerBits + cnt * wmax; }
        [unroll] for (int cc = 0; cc <= 31; ++cc) {
            const uint bits = headerBits + cnt + (cnt - over_[cc]) * (uint)cc + over_[cc] * 32u;
            if (bits < best) { best = bits; mode = 1u; width = (uint)cc; }
        }
        e_meta[0] = mode; e_meta[1] = width;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint mode = e_meta[0], width = e_meta[1];

    if (mode == 2u) {
        // raw escape: header + every word verbatim, straight to global
        if (t == 0u) { DstBytes.Store(tile * SlotWords * 4, 2u << 6); }
        if (t < np) {
            for (uint l = 0; l < NW; ++l) {
                DstBytes.Store(tile * SlotWords * 4 + 4u + (t * NW + l) * 4, e_px[t * NW + l]);
            }
        }
        if (t == 0u) { TileLen[tile] = 4u + np * NW * 4u; }
        return;
    }

    // mask bit for this value
    if (t < cnt && bl > width) { InterlockedOr(e_mask[t >> 5], 1u << (t & 31u)); }
    GroupMemoryBarrierWithGroupSync();

    // bit offset of this value: block prefix sum of the field sizes
    const uint myBits = (t < cnt) ? ((bl > width) ? 32u : width) : 0u;
    e_scan[t] = myBits;
    GroupMemoryBarrierWithGroupSync();
    // ping-pong Hillis-Steele: read A / write B, so one barrier per step (was two)
    [unroll] for (uint st = 0u; st < 8u; ++st) {
        const uint off = 1u << st;
        const bool toB = ((st & 1u) == 0u);
        const uint add = (t >= off) ? (toB ? e_scan[t - off] : e_scan2[t - off]) : 0u;
        const uint val = (toB ? e_scan[t] : e_scan2[t]);
        if (toB) { e_scan2[t] = val + add; } else { e_scan[t] = val + add; }
        GroupMemoryBarrierWithGroupSync();
    }
    const uint maskWords = (mode == 1u) ? ((cnt + 31u) / 32u) : 0u;   // mode 0 has no mask block
    const uint valBase = 32u + NW * 32u + maskWords * 32u;
    uint bitpos = valBase + ((t > 0u) ? e_scan[t - 1] : 0u);
    if (t == 0u) { TileLen[tile] = (valBase + e_scan[255] + 7u) / 8u; }

    // header + absolutes + mask words + this thread's value, all OR-ed into LDS
    if (t == 0u) {
        e_out[0] |= (width | (mode << 6));
    }
    if (t < NW) {
        const uint bp = 32u + t * 32u;
        InterlockedOr(e_out[bp >> 5], e_px[t] << (bp & 31u));
    }
    if (t < maskWords) {
        const uint bp = 32u + NW * 32u + t * 32u;
        InterlockedOr(e_out[bp >> 5], e_mask[t] << (bp & 31u));
    }
    if (t < cnt) {
        const uint w = (bl > width) ? 32u : width;
        if (w) {
            const uint w0 = bitpos >> 5, o = bitpos & 31u;
            InterlockedOr(e_out[w0], z << o);
            if (o + w > 32u) { InterlockedOr(e_out[w0 + 1u], z >> (32u - o)); }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // coalesced flush
    const uint words = (TileLen[tile] + 3u) / 4u;
    for (uint k2 = t; k2 < words; k2 += 256u) {
        DstBytes.Store(tile * SlotWords * 4 + k2 * 4, e_out[k2]);
    }
}

// ---------------------------------------------------------------- decode -----
// groupshared scratch for the block-shaped decoder (HLSL requires file scope)

[numthreads(256, 1, 1)]
void Decode(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    // one block (256 threads) per 16x16 tile: unpack in parallel, then rebuild
    // rows in order. No staging array, no per-byte loop, no within-row chain walk
    // longer than 16.
    const uint tile = gid.x;
    const uint t = tid.x;
    const uint tx = (tile % TilesX) * kTileW;
    const uint ty = (tile / TilesX) * kTileH;
    const uint pw = min(kTileW, W - tx);
    const uint ph = min(kTileH, H - ty);
    const uint base = tile * SlotWords * 4;
    const uint hdr = SrcBytes.Load(base);
    const uint width = hdr & 0x3Fu;
    const uint mode = (hdr >> 6) & 0x3u;
    const uint cnt = pw * ph * NW - NW;

    if (mode == 2) {                                  // raw escape
        if (t < pw * ph) {
            const uint y = ty + (t / kTileW), x = tx + (t % kTileW);
            for (uint l = 0; l < NW; ++l) {
                stPix(y, x, SrcBytes.Load(base + 4 + (t * NW + l) * 4));
            }
        }
        return;
    }

    const uint maskWords = (mode == 1) ? ((cnt + 31u) / 32u) : 0u;
    if (t < NW) { g_abs[t] = SrcBytes.Load(base + 4 + 4 * t); }
    if (mode == 1 && t < maskWords) { g_mask[t] = SrcBytes.Load(base + 4 + NW * 4 + 4 * t); }

    GroupMemoryBarrierWithGroupSync();

    // ---- phase 1: unpack all values in parallel via a block prefix sum of widths
    const uint K = (cnt + 255u) / 256u;
    uint myBits = 0u;
    for (uint j = 0; j < K; ++j) {
        const uint i = t * K + j;
        if (i < cnt) {
            const uint patch = (mode == 1) ? ((g_mask[i >> 5] >> (i & 31u)) & 1u) : 0u;
            myBits += patch ? 32u : width;
        }
    }
    g_scan[t] = myBits;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint st = 0u; st < 8u; ++st) {          // ping-pong: 1 barrier per step
        const uint off = 1u << st;
        const bool toB = ((st & 1u) == 0u);
        const uint add = (t >= off) ? (toB ? g_scan[t - off] : g_scan2[t - off]) : 0u;
        const uint val = (toB ? g_scan[t] : g_scan2[t]);
        if (toB) { g_scan2[t] = val + add; } else { g_scan[t] = val + add; }
        GroupMemoryBarrierWithGroupSync();
    }
    uint bitpos = 32u + NW * 32u + maskWords * 32u + ((t > 0) ? g_scan[t - 1] : 0u);
    for (uint j2 = 0; j2 < K; ++j2) {
        const uint i = t * K + j2;
        if (i < cnt) {
            const uint patch = (mode == 1) ? ((g_mask[i >> 5] >> (i & 31u)) & 1u) : 0u;
            const uint w = patch ? 32u : width;
            uint v = 0u;
            if (w) {
                const uint wi = bitpos >> 5, off2 = bitpos & 31u;
                v = SrcBytes.Load(base + wi * 4) >> off2;
                if (off2 + w > 32u) { v |= SrcBytes.Load(base + (wi + 1u) * 4) << (32u - off2); }
                if (w < 32u) { v &= ((1u << w) - 1u); }
            }
            g_z[i] = v;
            bitpos += w;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- phase 2: three independent pieces instead of a 2D scan
    //   A) within-row cumulative sums   (all 256 threads in parallel)
    //   B) the column-0 chain           (one thread, 16 steps)
    //   C) value = col0[row] + rowsum   (all threads, one add)
    const uint c = t % kTileW, r = t / kTileW;
    // within-row inclusive scan (ping-pong, one barrier per step): the old version
    // summed up to 31 deltas serially, which cost AMD 4x more than NV
    for (uint l = 0; l < NW; ++l) {
        // column 0 is part of the g_col0 chain, so the row sum must start at c = 1
        g_rowsum[t * NW + l] = (c == 0u) ? 0u
            : unzigzag(g_z[((r * kTileW + c) - 1u) * NW + l]);
    }
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint st = 0u; st < 5u; ++st) {          // kTileW = 32 -> 5 steps
        const uint off = 1u << st;
        const bool toB = ((st & 1u) == 0u);
        for (uint l = 0; l < NW; ++l) {
            const uint add = (c >= off) ? (toB ? g_rowsum[(t - off) * NW + l]
                                               : g_rowsum2[(t - off) * NW + l]) : 0u;
            const uint val = (toB ? g_rowsum[t * NW + l] : g_rowsum2[t * NW + l]);
            if (toB) { g_rowsum2[t * NW + l] = val + add; } else { g_rowsum[t * NW + l] = val + add; }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    // 5 steps end in g_rowsum2 (odd number of flips: 0->B,1->A,2->B,3->A,4->B)

    if (t == 0u) {                                 // B: 16-element chain down column 0
        for (uint l = 0; l < NW; ++l) {
            uint acc = mono(g_abs[l]);
            g_col0[l] = acc;
            for (uint rr = 1u; rr < ph; ++rr) {
                acc += unzigzag(g_z[(rr * kTileW - 1u) * NW + l]);
                g_col0[rr * NW + l] = acc;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (c < pw && r < ph) {                        // C: combine and store
        const uint y = ty + r, x = tx + c;
        for (uint l = 0; l < NW; ++l) {
            const uint v = g_col0[r * NW + l] + g_rowsum2[t * NW + l];
            stPix(y, x, unmono(v));
        }
    }
}
