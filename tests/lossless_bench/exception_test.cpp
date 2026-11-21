// exception_test - the per-tile *max* width is dominated by outliers. What if a tile
// stores a common width plus an exception list (mask + patches)?
//
//   g++ -O2 -std=c++17 -I. exception_test.cpp -o exception_test
//   ./exception_test f00_mv.raw [more...]
//
// Two candidates per 16x16 tile, pick the cheaper (1 bit mode in the header):
//   max-mode : (cnt-1)*nw*w_max bits                         <- what fr_codec.h does
//   exc-mode : cnt*nw (mask) + (cnt-1)*nw*w + n_out*32 bits  <- w chosen to minimise
//
// Decoding the exception form has NO cross-pixel dependency (unpack + add per value),
// which also fixes the AMD decode problem measured in tests/gpu_codec (section 13.2).
#include "fr_codec.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

static uint32_t bpp_of(uint32_t f) {
    switch (f) {
        case 10: return 8;
        case 34: case 41: case 39: case 28: return 4;
        default: return 0;
    }
}

struct Img { uint32_t w = 0, h = 0, nw = 0; std::vector<uint32_t> px; };

static bool load(const char* p, Img& im) {
    FILE* f = fopen(p, "rb");
    if (!f) return false;
    uint32_t hdr[4];
    if (fread(hdr, 4, 4, f) != 4) { fclose(f); return false; }
    im.w = hdr[0]; im.h = hdr[1];
    const uint32_t pitch = hdr[3], bpp = bpp_of(hdr[2]);
    if (!bpp) { fclose(f); return false; }
    im.nw = bpp / 4;
    im.px.assign((size_t)im.w * im.h * im.nw, 0);
    std::vector<uint8_t> tmp(pitch);
    for (uint32_t y = 0; y < im.h; ++y) {
        if (fread(tmp.data(), 1, pitch, f) != pitch) break;
        memcpy((uint8_t*)im.px.data() + (size_t)y * im.w * bpp, tmp.data(), (size_t)im.w * bpp);
    }
    fclose(f);
    return true;
}

// -> (bytes_max, bytes_exc)
static void tile_cost(const Img& im, size_t& outMax, size_t& outExc) {
    outMax = outExc = 0;
    const uint32_t w = im.w, h = im.h, nw = im.nw;
    std::vector<int> bl;
    for (uint32_t ty = 0; ty < h; ty += (uint32_t)fr::kTileH) {
        for (uint32_t tx = 0; tx < w; tx += (uint32_t)fr::kTileW) {
            const uint32_t pw = std::min<uint32_t>((uint32_t)fr::kTileW, w - tx);
            const uint32_t ph = std::min<uint32_t>((uint32_t)fr::kTileH, h - ty);
            bl.clear();
            int wmax = 0;
            for (uint32_t y = ty; y < ty + ph; ++y) {
                for (uint32_t x = tx; x < tx + pw; ++x) {
                    for (uint32_t l = 0; l < nw; ++l) {
                        const size_t i = ((size_t)y * w + x) * nw + l;
                        const bool hasL = (x > tx), hasU = (y > ty);
                        if (!hasL && !hasU) continue;
                        const size_t ref = hasL ? i - nw : i - (size_t)w * nw;
                        const int b = fr::bitlen(fr::zigzag(fr::mono(im.px[i]) - fr::mono(im.px[ref])));
                        bl.push_back(b);
                        if (b > wmax) wmax = b;
                    }
                }
            }
            const size_t cnt = bl.size();
            if (!cnt) { outMax += 4 + (size_t)nw * 4; outExc += 4 + (size_t)nw * 4; continue; }

            // max-mode
            size_t bestMax = (wmax >= 32) ? cnt * 32 : cnt * (size_t)wmax;
            // exc-mode: try every width
            size_t bestExc = bestMax + cnt;              // never worse than max + mask
            for (int wc = 0; wc <= 31; ++wc) {
                size_t outs = 0;
                for (int b : bl) if (b > wc) ++outs;
                const size_t bits = cnt /*mask*/ + (cnt - outs) * (size_t)wc + outs * 32;
                if (bits < bestExc) bestExc = bits;
            }
            outMax += 4 + (size_t)nw * 4 + (bestMax + 7) / 8;
            outExc += 4 + (size_t)nw * 4 + (bestExc + 7) / 8;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: %s f00_mv.raw ...\n", argv[0]); return 2; }
    printf("%-16s %7s %7s %7s\n", "frame", "max", "exc", "gain");
    double sm = 0, se = 0;
    int n = 0;
    std::vector<double> excRatios;
    for (int a = 1; a < argc; ++a) {
        Img im;
        if (!load(argv[a], im)) continue;
        const double rawMB = (double)im.w * im.h * im.nw * 4 / 1e6;
        size_t bm = 0, be = 0;
        tile_cost(im, bm, be);
        const double rm = rawMB / (bm / 1e6), re = rawMB / (be / 1e6);
        const char* base = strrchr(argv[a], '/');
        printf("%-16s %7.2f %7.2f %+6.1f%%\n", base ? base + 1 : argv[a], rm, re,
               100.0 * (re / rm - 1.0));
        sm += rm; se += re; ++n;
        excRatios.push_back(re);
    }
    if (n) {
        std::sort(excRatios.begin(), excRatios.end());
        printf("\nmean max=%.2f  exc=%.2f  (%+.1f%%)   exc p5=%.2f p50=%.2f\n",
               sm / n, se / n, 100.0 * (se / sm - 1.0),
               excRatios[excRatios.size() / 20], excRatios[excRatios.size() / 2]);
    }
    return 0;
}
