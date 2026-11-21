// temporal_test - is inter-frame (temporal) prediction a better general path than
// intra-frame prediction? Measured on consecutive frames of a real dump.
//
//   g++ -O2 -std=c++17 -I. temporal_test.cpp -o temporal_test
//   ./temporal_test f00_mv.raw f01_mv.raw ... f31_mv.raw
//
// For each consecutive pair it builds the per-lane residual
//     r = zigzag(mono(cur) - mono(prev))
// and reports the exact byte count of storing r in the same 16x16 tile format
// (header + per-tile max bit width, raw escape at 32 bits).
//
// This is the CONSERVATIVE variant: temporal delta only, no spatial prediction on
// top of the residual. A real implementation would also predict r from its left
// neighbour, which is worth another 10-20% on smooth content.
#include "fr_codec.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

static uint32_t bpp_of(uint32_t fmt) {
    switch (fmt) {
        case 10: return 8;
        case 34: case 41: case 39: case 28: return 4;
        default: return 0;
    }
}

struct Frame {
    uint32_t w = 0, h = 0, fmt = 0, nw = 0;
    std::vector<uint32_t> px;
};

static bool load(const char* path, Frame& f) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    uint32_t hdr[4];
    if (fread(hdr, 4, 4, fp) != 4) { fclose(fp); return false; }
    f.w = hdr[0]; f.h = hdr[1]; f.fmt = hdr[2];
    const uint32_t pitch = hdr[3], bpp = bpp_of(f.fmt);
    if (!bpp || !f.w || !f.h) { fclose(fp); return false; }
    f.nw = bpp / 4;
    f.px.assign((size_t)f.w * f.h * f.nw, 0);
    std::vector<uint8_t> tmp(pitch);
    for (uint32_t y = 0; y < f.h; ++y) {
        if (fread(tmp.data(), 1, pitch, fp) != pitch) break;
        memcpy((uint8_t*)f.px.data() + (size_t)y * f.w * bpp, tmp.data(), (size_t)f.w * bpp);
    }
    fclose(fp);
    return true;
}

// exact byte count of the tile format over a residual buffer
static size_t residual_bytes(const std::vector<uint32_t>& r, uint32_t w, uint32_t h, uint32_t nw) {
    size_t bytes = 0;
    for (uint32_t ty = 0; ty < h; ty += (uint32_t)fr::kTileH) {
        for (uint32_t tx = 0; tx < w; tx += (uint32_t)fr::kTileW) {
            const uint32_t pw = std::min<uint32_t>((uint32_t)fr::kTileW, w - tx);
            const uint32_t ph = std::min<uint32_t>((uint32_t)fr::kTileH, h - ty);
            int width = 0;
            for (uint32_t y = ty; y < ty + ph; ++y)
                for (uint32_t x = tx; x < tx + pw; ++x)
                    for (uint32_t l = 0; l < nw; ++l) {
                        const int b = fr::bitlen(r[((size_t)y * w + x) * nw + l]);
                        if (b > width) width = b;
                    }
            bytes += 4;                                  // header word
            if (width >= 32) { bytes += (size_t)pw * ph * nw * 4; }
            else { bytes += (size_t)nw * 4 + ((size_t)(pw * ph * nw - nw) * width + 7) / 8; }
        }
    }
    return bytes;
}


// per-tile best-of(intra width, temporal width) + 1 bit mode: the adaptive variant.
// Width is what the tile costs, so min(width) == min(bytes) per tile.
static size_t adaptive_bytes(const std::vector<uint32_t>& cur, const std::vector<uint32_t>& prev,
                             const std::vector<uint32_t>& res, uint32_t w, uint32_t h, uint32_t nw) {
    size_t bytes = 0;
    for (uint32_t ty = 0; ty < h; ty += (uint32_t)fr::kTileH) {
        for (uint32_t tx = 0; tx < w; tx += (uint32_t)fr::kTileW) {
            const uint32_t pw = std::min<uint32_t>((uint32_t)fr::kTileW, w - tx);
            const uint32_t ph = std::min<uint32_t>((uint32_t)fr::kTileH, h - ty);
            int wi = 0, wt = 0;
            for (uint32_t y = ty; y < ty + ph; ++y) {
                for (uint32_t x = tx; x < tx + pw; ++x) {
                    for (uint32_t l = 0; l < nw; ++l) {
                        const size_t i = ((size_t)y * w + x) * nw + l;
                        const bool hasL = (x > tx), hasU = (y > ty);
                        int bi = 0;
                        if (hasL || hasU) {
                            const size_t ref = hasL ? i - nw : i - (size_t)w * nw;
                            bi = fr::bitlen(fr::zigzag(fr::mono(cur[i]) - fr::mono(cur[ref])));
                        }
                        const int bt = fr::bitlen(res[i]);
                        if (bi > wi) wi = bi;
                        if (bt > wt) wt = bt;
                    }
                }
            }
            const int wbest = std::min(wi, wt);
            bytes += 4;
            if (wbest >= 32) { bytes += (size_t)pw * ph * nw * 4; }
            else { bytes += (size_t)nw * 4 + ((size_t)(pw * ph * nw - nw) * wbest + 7) / 8; }
        }
    }
    return bytes;
}

static double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double i = (p / 100.0) * (double)(v.size() - 1) + 0.5;
    return v[(size_t)std::min<double>((double)(v.size() - 1), i)];
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: %s f00.raw f01.raw ...\n", argv[0]); return 2; }
    Frame prev, cur;
    if (!load(argv[1], prev)) { printf("cannot read %s\n", argv[1]); return 1; }
    const double rawMB = (double)prev.w * prev.h * prev.nw * 4 / 1e6;
    std::vector<double> tRatio, iRatio, aRatio;
    printf("%-14s %8s %8s %8s %8s\n", "delta", "intra", "temporal", "adaptive", "best");
    for (int a = 2; a < argc; ++a) {
        if (!load(argv[a], cur)) break;
        if (cur.w != prev.w || cur.h != prev.h || cur.nw != prev.nw) {
            printf("  size changed at %s, stop\n", argv[a]);
            break;
        }
        const size_t n = prev.px.size();
        std::vector<uint32_t> r(n);
        for (size_t i = 0; i < n; ++i) {
            r[i] = fr::zigzag(fr::mono(cur.px[i]) - fr::mono(prev.px[i]));
        }
        const size_t tb = residual_bytes(r, cur.w, cur.h, cur.nw);
        const size_t ab = adaptive_bytes(cur.px, prev.px, r, cur.w, cur.h, cur.nw);
        std::vector<uint8_t> intra;
        fr::encode(cur.px.data(), cur.w, cur.h, cur.nw, intra);
        const double tr = rawMB / (tb / 1e6), ir = rawMB / (intra.size() / 1e6);
        tRatio.push_back(tr);
        iRatio.push_back(ir);
        aRatio.push_back(rawMB / (ab / 1e6));
        const char* base = strrchr(argv[a], '/');
        printf("%-14s %8.2f %8.2f %8.2f %8.2f\n", base ? base + 1 : argv[a], ir, tr,
               rawMB / (ab / 1e6), rawMB / (ab / 1e6) / ir);
        prev.px.swap(cur.px);
    }
    if (!tRatio.empty()) {
        printf("\nintra    p5=%.2f p50=%.2f p95=%.2f\n", pct(iRatio, 5), pct(iRatio, 50), pct(iRatio, 95));
        printf("adaptive p5=%.2f p50=%.2f p95=%.2f   (+%.1f%% vs intra at p50)\n",
               pct(aRatio, 5), pct(aRatio, 50), pct(aRatio, 95),
               100.0 * (pct(aRatio, 50) / pct(iRatio, 50) - 1.0));
        printf("temporal p5=%.2f p50=%.2f p95=%.2f   (%.1f%% fewer bytes at p50)\n",
               pct(tRatio, 5), pct(tRatio, 50), pct(tRatio, 95),
               100.0 * (1.0 - pct(iRatio, 50) / pct(tRatio, 50)));
    }
    return 0;
}
