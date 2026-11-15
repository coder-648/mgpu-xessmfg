// marker.hlsl - ONE full-screen pass that paints the four corner blocks.
//
// Geometry (must match mgpu::marker_corner): 6x6 block per screen corner, margin =
// w/32 (h/32), 4-px aligned. Each block is a 3x3 grid of 2x2-px cells:
//   cell 0..7 = the block's 8 data bits
//   cell 8    = always white  (gives the min/max decoder contrast even if the data
//               bits happen to be all zeros, which would otherwise be a solid square)
//
// The 32-bit code is a SHORT SNOWFLAKE: payload24 (sequence12 | low12(ms)) plus a
// CRC-8, split into four bytes, one per corner. Each byte is XORed with 0x5A so a
// zero byte does not read as a solid block.
cbuffer MarkerCode : register(b1)
{
    uint code;       // payload24 | crc8<<24
    uint width;      // frame width
    uint height;     // frame height
    uint side;       // block side (6)
    uint cells;      // cells per block row (3)
    uint pad0;
    uint pad1;
    uint pad2;
};

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    const uint margin_x = (width  / 32u) & ~3u;
    const uint margin_y = (height / 32u) & ~3u;
    const uint left   = margin_x;
    const uint right  = width  - margin_x - side;
    const uint top    = margin_y;
    const uint bottom = height - margin_y - side;

    const uint2 p = uint2(pos.xy);
    uint blk;
    uint2 origin;
    if      (p.x >= left  && p.x < left  + side && p.y >= top    && p.y < top    + side) { blk = 0u; origin = uint2(left,  top);    }
    else if (p.x >= right && p.x < right + side && p.y >= top    && p.y < top    + side) { blk = 1u; origin = uint2(right, top);    }
    else if (p.x >= left  && p.x < left  + side && p.y >= bottom && p.y < bottom + side) { blk = 2u; origin = uint2(left,  bottom); }
    else if (p.x >= right && p.x < right + side && p.y >= bottom && p.y < bottom + side) { blk = 3u; origin = uint2(right, bottom); }
    else { discard; }

    const uint2 cell = (p - origin) >> 1;                 // 0..cells-1
    const uint idx = cell.y * cells + cell.x;             // 0..8
    uint byte = (code >> (blk * 8u)) & 0xFFu;
    byte ^= 0x5Au;                                       // same key as the decoder
    uint v;
    if (idx >= 8u) {
        // Reference cell: black only when the 8 data bits are all white, so the block
        // ALWAYS contains both levels (a flat block cannot be thresholded).
        v = (byte == 0xFFu) ? 0u : 1u;
    } else {
        v = (byte >> idx) & 1u;
    }
    const float f = v ? 1.0 : 0.0;
    return float4(f, f, f, 1.0);
}
