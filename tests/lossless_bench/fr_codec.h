// fr_codec.h - reference implementation of the fixed-rate block codec from
// docs/design/LOSSLESS_TRANSPORT.md section 6.1.
//
// Portable C++ (no CUDA / D3D): this is the *bitstream contract* the HLSL
// encoder/decoder must mirror. It exists so that
//   1. reversibility is proven, not assumed, and
//   2. scripts/lossless_ratio.py can be cross-checked byte for byte.
//
// Geometry
//   image = w x h pixels, each pixel = nw words (nw = bytes/pixel / 4: 1 for R32 and
//           RG16F, 2 for RGBA16F), row-major, unpadded.
//   tile  = 16 x 16 pixels anchored at (0,0); the right/bottom edge is clipped, so
//           partial tiles are legal.
//
// Per tile (little-endian)
//   u32 header : bits 0..5 = width w (0..32), bit 6 = mode (0 = packed, 1 = raw)
//   absolutes  : nw x u32 - the top-left pixel of the tile (nothing to predict from)
//   payload    : (cnt-1)*nw zigzag deltas, w bits each, LSB-first, pixel-major;
//                mode 1 instead stores cnt*nw words verbatim
//
// Prediction, per lane: left neighbour when x > tile_x, else top when y > tile_y,
// else the stored absolute. Depth / fp16 bit patterns are mapped to a monotone
// integer first so that "close in value" == "close as integer".
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <algorithm>

namespace fr {

constexpr size_t kTileW = 32;   // tile width  in pixels (row segment length)
constexpr size_t kTileH = 8;    // tile height in pixels (vertical chain length)
constexpr size_t kTilePx = kTileW;  // legacy alias

inline uint32_t mono(uint32_t v) {
    return (v >> 31) ? (v ^ 0xFFFFFFFFu) : (v ^ 0x80000000u);
}
inline uint32_t unmono(uint32_t u) {
    return (u >> 31) ? (u ^ 0x80000000u) : (u ^ 0xFFFFFFFFu);
}
inline uint32_t zigzag(uint32_t d) { return (d << 1) ^ (d >> 31 ? 0xFFFFFFFFu : 0u); }
inline uint32_t unzigzag(uint32_t z) { return (z >> 1) ^ (0u - (z & 1u)); }

inline int bitlen(uint32_t v) {
    int n = 0;
    while (v) { ++n; v >>= 1; }
    return n;
}

struct Writer {
    std::vector<uint8_t> buf;
    uint64_t acc = 0;
    int bits = 0;
    void put(uint32_t v, int w) {                       // LSB-first
        if (w <= 0) return;
        acc |= (uint64_t)v << bits;
        bits += w;
        while (bits >= 8) {
            buf.push_back((uint8_t)(acc & 0xFFu));
            acc >>= 8;
            bits -= 8;
        }
    }
    void align() {
        if (bits) { buf.push_back((uint8_t)(acc & 0xFFu)); acc = 0; bits = 0; }
    }
    void word(uint32_t v) { align(); put(v, 32); align(); }
};

struct Reader {
    const uint8_t* p = nullptr;
    size_t n = 0, i = 0;
    uint64_t acc = 0;
    int bits = 0;
    void align() { acc = 0; bits = 0; }
    uint32_t get(int w) {
        if (w <= 0) return 0;
        while (bits < w) {
            const uint64_t b = (i < n) ? p[i] : 0u;
            ++i;
            acc |= b << bits;
            bits += 8;
        }
        const uint32_t mask = (w >= 32) ? 0xFFFFFFFFu : (uint32_t)((1ull << w) - 1u);
        const uint32_t v = (uint32_t)(acc & mask);
        acc >>= w;
        bits -= w;
        return v;
    }
    uint32_t word() { align(); const uint32_t v = get(32); align(); return v; }
};

// ============================================================================
// Bitstream v2 - one tile
//
//   u32 header : bits 0..5 = w (0..31), bits 6..7 = mode
//   mode 0 plain      : nw absolutes, then cnt values at w bits        (== v1 layout)
//   mode 1 exception  : nw absolutes, cnt mask bits, (cnt-n_out) values at w bits,
//                       then n_out patches at 32 bits
//   mode 2 raw escape : pw*ph*nw words verbatim (no absolutes)
//
// The encoder picks the cheapest of the three per tile, so v1 is always available as
// a fallback and the worst case is unchanged. Integer-only, no float anywhere.
// ============================================================================
constexpr int kModePlain = 0;
constexpr int kModeExc = 1;
constexpr int kModeRaw = 2;

struct TilePlan {
    int mode = kModePlain;
    int w = 0;
    size_t cnt = 0;                  // values in the tile, first pixel excluded
    size_t bits = 0;                 // whole tile in bits (header + absolutes included)
    std::vector<uint32_t> z;         // zigzag deltas, coding order
    std::vector<uint8_t> patch;      // mode 1 only: 1 = value rides in the patch list
};

inline void plan_tile(const uint32_t* src, size_t w, size_t h, size_t nw,
                      size_t tx0, size_t ty0, TilePlan& p) {
    const size_t pw = std::min(kTileW, w - tx0);
    const size_t ph = std::min(kTileH, h - ty0);
    p.z.clear();
    int hist[33] = {0};
    int wmax = 0;
    for (size_t y = ty0; y < ty0 + ph; ++y) {
        for (size_t x = tx0; x < tx0 + pw; ++x) {
            const uint32_t* px = src + (y * w + x) * nw;
            const uint32_t* lf = (x > tx0) ? px - nw : nullptr;
            const uint32_t* up = (y > ty0) ? px - w * nw : nullptr;
            if (!lf && !up) continue;                     // the absolute pixel
            // predictor: left neighbour, else the pixel above (JPEG-LS-like family).
            // Measured: this beats "top only" by 11% on depth, and 0% on MV.
            const uint32_t* ref = lf ? lf : up;
            for (size_t l = 0; l < nw; ++l) {
                const uint32_t z = zigzag(mono(px[l]) - mono(ref[l]));
                p.z.push_back(z);
                const int b = bitlen(z);
                ++hist[b];
                if (b > wmax) wmax = b;
            }
        }
    }
    const size_t cnt = p.z.size();
    p.cnt = cnt;
    const size_t header = 32 + nw * 32;

    size_t best;
    if (wmax >= 32) {                                     // raw stores every word
        p.mode = kModeRaw; p.w = 0; best = 32 + (cnt + nw) * 32;
    } else {
        p.mode = kModePlain; p.w = wmax; best = header + cnt * (size_t)wmax;
    }
    size_t over[33];
    over[32] = 0;
    for (int b = 31; b >= 0; --b) over[b] = over[b + 1] + (size_t)hist[b + 1];
    for (int c = 0; c <= 31; ++c) {
        const size_t bits = header + cnt + (cnt - over[c]) * (size_t)c + over[c] * 32;
        if (bits < best) { best = bits; p.mode = kModeExc; p.w = c; }
    }
    p.bits = best;
    if (p.mode == kModeExc) {
        p.patch.assign(cnt, 0);
        for (size_t i = 0; i < cnt; ++i) p.patch[i] = (bitlen(p.z[i]) > p.w) ? 1 : 0;
    }
}

inline void write_tile(const TilePlan& p, const uint32_t* src, size_t w, size_t nw,
                       size_t tx0, size_t ty0, size_t pw, size_t ph, Writer& wr) {
    wr.word((uint32_t)p.w | ((uint32_t)p.mode << 6));
    if (p.mode == kModeRaw) {
        for (size_t y = ty0; y < ty0 + ph; ++y)
            for (size_t x = tx0; x < tx0 + pw; ++x)
                for (size_t l = 0; l < nw; ++l) wr.word(src[(y * w + x) * nw + l]);
        return;
    }
    for (size_t l = 0; l < nw; ++l) wr.word(src[(ty0 * w + tx0) * nw + l]);
    if (p.mode == kModePlain) {
        for (uint32_t z : p.z) wr.put(z, p.w);
    } else {
        // v2.2: the mask is a word-aligned bit array BEFORE the values, so a GPU can
        // compute every field offset in parallel (a per-value inline flag cannot).
        const size_t maskWords = (p.cnt + 31) / 32;
        for (size_t wi = 0; wi < maskWords; ++wi) {
            uint32_t wordBits = 0;
            for (size_t b = 0; b < 32; ++b) {
                const size_t i = wi * 32 + b;
                if (i < p.cnt && p.patch[i]) wordBits |= (1u << b);
            }
            wr.word(wordBits);
        }
        for (size_t i = 0; i < p.cnt; ++i) wr.put(p.z[i], p.patch[i] ? 32 : p.w);
    }
    wr.align();
}

// src: w*h*nw words (unpadded). out: the compressed stream.
inline void encode(const uint32_t* src, size_t w, size_t h, size_t nw,
                   std::vector<uint8_t>& out) {
    Writer wr;
    TilePlan p;
    for (size_t ty = 0; ty < h; ty += kTileH) {
        for (size_t tx = 0; tx < w; tx += kTileW) {
            plan_tile(src, w, h, nw, tx, ty, p);
            write_tile(p, src, w, nw, tx, ty,
                       std::min(kTileW, w - tx),
                       std::min(kTileH, h - ty), wr);
        }
    }
    wr.align();
    out.swap(wr.buf);
}

// One tile only, byte-identical to the matching slice of encode().
inline void encode_tile(const uint32_t* src, size_t w, size_t h, size_t nw,
                        size_t tx0, size_t ty0, std::vector<uint8_t>& out) {
    TilePlan p;
    plan_tile(src, w, h, nw, tx0, ty0, p);
    Writer wr;
    write_tile(p, src, w, nw, tx0, ty0,
               std::min(kTileW, w - tx0),
               std::min(kTileH, h - ty0), wr);
    wr.align();
    out.swap(wr.buf);
}

// dst: w*h*nw words. Returns false only for a truncated/corrupt stream.
inline bool decode(const std::vector<uint8_t>& in, uint32_t* dst, size_t w, size_t h, size_t nw) {
    Reader rd{in.data(), in.size()};
    for (size_t ty = 0; ty < h; ty += kTileH) {
        for (size_t tx = 0; tx < w; tx += kTileW) {
            const size_t pw = std::min(kTileW, w - tx);
            const size_t ph = std::min(kTileH, h - ty);
            if (rd.i + 4 > in.size()) return false;
            const uint32_t hdr = rd.word();
            const int width = (int)(hdr & 0x3Fu);
            const int mode = (int)((hdr >> 6) & 0x3u);
            if (mode == kModeRaw) {
                for (size_t y = ty; y < ty + ph; ++y)
                    for (size_t x = tx; x < tx + pw; ++x)
                        for (size_t l = 0; l < nw; ++l) dst[(y * w + x) * nw + l] = rd.word();
                continue;
            }
            for (size_t l = 0; l < nw; ++l) dst[(ty * w + tx) * nw + l] = rd.word();
            const size_t cnt = pw * ph * nw - nw;
            std::vector<uint32_t> z(cnt, 0);
            if (mode == kModeExc) {
                const size_t maskWords = (cnt + 31) / 32;
                std::vector<uint32_t> mask(maskWords, 0);
                for (size_t wi = 0; wi < maskWords; ++wi) mask[wi] = rd.word();
                for (size_t i = 0; i < cnt; ++i) {
                    const bool patch = ((mask[i >> 5] >> (i & 31u)) & 1u) != 0;
                    z[i] = rd.get(patch ? 32 : width);
                }
            } else {
                for (size_t i = 0; i < cnt; ++i) z[i] = rd.get(width);
            }
            size_t k = 0;
            const uint32_t* abs0 = dst + (ty * w + tx) * nw;
            for (size_t y = ty; y < ty + ph; ++y) {
                for (size_t x = tx; x < tx + pw; ++x) {
                    if (x == tx && y == ty) continue;          // absolute
                    uint32_t* px = dst + (y * w + x) * nw;
                    const uint32_t* up = (x > tx) ? px - nw : ((y > ty) ? px - w * nw : abs0);
                    for (size_t l = 0; l < nw; ++l) {
                        const uint32_t ref = mono(up[l]);
                        px[l] = unmono(ref + unzigzag(z[k++]));
                    }
                }
            }
            rd.align();
        }
    }
    return true;
}



}  // namespace fr
