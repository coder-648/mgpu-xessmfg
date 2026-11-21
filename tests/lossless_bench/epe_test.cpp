// epe_test - gate 2 without the game: how much MV error does the downsample ->
// upsample transport add, measured against the full-resolution MV on disk?
//
//   g++ -O2 -std=c++17 -I. epe_test.cpp -o epe_test
//   ./epe_test <percent> f00_mv.raw [more frames...]
//
// Per frame:  full-res MV --(3x3 median, the shader's family)--> 150%/200%
//                --(bilinear, the cheapest reconstruction)--> full res
//             EPE = |reconstructed - original| in pixels.
//
// Bilinear is NOT the real `--upsample` (depth-guided JBU is better), so this
// OVERSTATES the error: it is the optimistic case for "pay for 100%".
// The edge band (local MV gradient > 0.5 px) is the metric that matters - that is
// the band that shows up as ghosting, and a lossless stream removes it by
// construction (EPE = 0).
#include "fr_codec.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

static float half_to_float(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu, out;
    if (exp == 0) {
        if (man == 0) { out = sign; }
        else {                                   // subnormal
            exp = 127 - 15 + 1;
            while ((man & 0x400u) == 0u) { man <<= 1; --exp; }
            man &= 0x3FFu;
            out = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 0x1Fu) {
        out = sign | 0x7F800000u | (man << 13);  // inf / nan
    } else {
        out = sign | ((exp + 127u - 15u) << 23) | (man << 13);
    }
    float f;
    memcpy(&f, &out, 4);
    return f;
}

static double pct(const std::vector<float>& v, double p) {
    if (v.empty()) return 0.0;
    const double i = (p / 100.0) * (double)(v.size() - 1) + 0.5;
    return (double)v[(size_t)std::min<double>((double)(v.size() - 1), i)];
}

static inline float lane(const std::vector<uint32_t>& b, size_t i, int l) {
    return half_to_float((uint16_t)((b[i] >> (l * 16)) & 0xFFFFu));
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: %s <percent> f00_mv.raw ...\n", argv[0]); return 2; }
    const int percent = atoi(argv[1]);
    if (percent < 100) { printf("percent must be >= 100\n"); return 2; }
    const double r = percent / 100.0;

    std::vector<float> all_epe, all_edge, per_frame_p95;
    for (int a = 2; a < argc; ++a) {
        FILE* f = fopen(argv[a], "rb");
        if (!f) continue;
        uint32_t hdr[4];
        if (fread(hdr, 4, 4, f) != 4) { fclose(f); continue; }
        const uint32_t w = hdr[0], h = hdr[1], fmt = hdr[2], pitch = hdr[3];
        if (fmt != 34 || !w || !h) { fclose(f); continue; }
        std::vector<uint32_t> src((size_t)w * h);
        std::vector<uint8_t> tmp(pitch);
        bool ok = true;
        for (uint32_t y = 0; y < h; ++y) {
            if (fread(tmp.data(), 1, pitch, f) != pitch) { ok = false; break; }
            memcpy((uint8_t*)src.data() + (size_t)y * w * 4, tmp.data(), (size_t)w * 4);
        }
        fclose(f);
        if (!ok) continue;

        // ---- downsample: per-lane median over the 3x3 footprint
        const uint32_t dw = (uint32_t)((double)w / r), dh = (uint32_t)((double)h / r);
        std::vector<uint32_t> ds((size_t)dw * dh);
        const double sx = (double)w / dw, sy = (double)h / dh;
        for (uint32_t y = 0; y < dh; ++y) {
            for (uint32_t x = 0; x < dw; ++x) {
                const double cx = (x + 0.5) * sx - 0.5, cy = (y + 0.5) * sy - 0.5;
                const double ox = sx / 3.0, oy = sy / 3.0;
                uint32_t word = 0;
                for (int l = 0; l < 2; ++l) {
                    uint32_t c[9];
                    int n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            int px = (int)(cx + dx * ox + 0.5), py = (int)(cy + dy * oy + 0.5);
                            px = std::max(0, std::min((int)w - 1, px));
                            py = std::max(0, std::min((int)h - 1, py));
                            c[n++] = (src[(size_t)py * w + px] >> (l * 16)) & 0xFFFFu;
                        }
                    for (int i = 1; i < n; ++i) {
                        const uint32_t k = c[i], mk = fr::mono(k);
                        int j = i - 1;
                        while (j >= 0 && fr::mono(c[j]) > mk) { c[j + 1] = c[j]; --j; }
                        c[j + 1] = k;
                    }
                    word |= (c[4] << (l * 16));
                }
                ds[(size_t)y * dw + x] = word;
            }
        }
        // --- bilinear upsample back to full res, then EPE
        std::vector<float> epe, edge;
        epe.reserve((size_t)w * h);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const double u = (x + 0.5) * ((double)dw / w) - 0.5;
                const double v = (y + 0.5) * ((double)dh / h) - 0.5;
                const int x0 = (int)floor(u), y0 = (int)floor(v);
                const double fx = u - x0, fy = v - y0;
                const int xa = std::max(0, std::min((int)dw - 1, x0));
                const int ya = std::max(0, std::min((int)dh - 1, y0));
                const int x1 = std::max(0, std::min((int)dw - 1, x0 + 1));
                const int y1 = std::max(0, std::min((int)dh - 1, y0 + 1));
                const size_t i00 = (size_t)ya * dw + xa, i10 = (size_t)ya * dw + x1;
                const size_t i01 = (size_t)y1 * dw + xa, i11 = (size_t)y1 * dw + x1;
                double e2 = 0.0;
                for (int l = 0; l < 2; ++l) {
                    const double up =
                        (lane(ds, i00, l) * (1 - fx) + lane(ds, i10, l) * fx) * (1 - fy) +
                        (lane(ds, i01, l) * (1 - fx) + lane(ds, i11, l) * fx) * fy;
                    // MV is in UV; convert to full-res pixels per axis
                    const double sc = (l == 0) ? (double)w : (double)h;
                    const double d = (up - lane(src, (size_t)y * w + x, l)) * sc;
                    e2 += d * d;
                }
                const float e = (float)sqrt(e2);
                epe.push_back(e);
                const size_t c0 = (size_t)y * w + x;
                double g = 0.0;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int py = std::max(0, std::min((int)h - 1, (int)y + dy));
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int px = std::max(0, std::min((int)w - 1, (int)x + dx));
                        const size_t q = (size_t)py * w + px;
                        const double ddx = (lane(src, q, 0) - lane(src, c0, 0)) * (double)w;
                        const double ddy = (lane(src, q, 1) - lane(src, c0, 1)) * (double)h;
                        g = std::max(g, sqrt(ddx * ddx + ddy * ddy));
                    }
                }
                if (g > 0.5) edge.push_back(e);
            }
        }
        std::sort(epe.begin(), epe.end());
        std::sort(edge.begin(), edge.end());
        per_frame_p95.push_back((float)pct(epe, 95));
        const char* base = strrchr(argv[a], '/');
        printf("%-14s %ux%u -> %ux%u  EPE p50=%.3f p95=%.3f p99=%.3f px | edge(%4.1f%%) p95=%.3f p99=%.3f\n",
               base ? base + 1 : argv[a], w, h, dw, dh, pct(epe, 50), pct(epe, 95), pct(epe, 99),
               100.0 * (double)edge.size() / (double)epe.size(), pct(edge, 95), pct(edge, 99));
        all_epe.insert(all_epe.end(), epe.begin(), epe.end());
        all_edge.insert(all_edge.end(), edge.begin(), edge.end());
        if (all_epe.size() > 40000000u) break;   // keep memory sane
    }
    if (!all_epe.empty()) {
        std::sort(all_epe.begin(), all_epe.end());
        std::sort(all_edge.begin(), all_edge.end());
        std::sort(per_frame_p95.begin(), per_frame_p95.end());
        printf("\nALL (%zu px) EPE p50=%.3f p95=%.3f p99=%.3f | edge(%4.1f%%) p95=%.3f p99=%.3f"
               "  | per-frame p95 median=%.3f worst=%.3f\n",
               all_epe.size(), pct(all_epe, 50), pct(all_epe, 95), pct(all_epe, 99),
               100.0 * (double)all_edge.size() / (double)all_epe.size(),
               pct(all_edge, 95), pct(all_edge, 99),
               (double)per_frame_p95[per_frame_p95.size() / 2], (double)per_frame_p95.back());
    }
    return 0;
}
