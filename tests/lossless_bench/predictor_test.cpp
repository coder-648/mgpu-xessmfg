// predictor_test - which intra predictor is worth its complexity, on real frames?
//
//   g++ -O2 -std=c++17 -I. predictor_test.cpp -o predictor_test
//   ./predictor_test f00_mv.raw [more...]
//
// Variants (all on the monotone-integer mapping, same 16x16 tile accounting):
//   left      left neighbour, else top, else absolute   (what fr_codec.h does today)
//   med       JPEG-LS MED: clamp(a+b-c) between min/max of (a,b)
//   paeth     PNG Paeth
//   avg       (a + b) / 2
// The tile cost is 4 B header + nw*4 B absolutes + (cnt-1)*nw*w bits, w == 32 -> raw.
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

static inline uint32_t predict(int mode, uint32_t a, uint32_t b, uint32_t c) {
    const int32_t aa = (int32_t)a, bb = (int32_t)b, cc = (int32_t)c;
    switch (mode) {
        case 1: {                                    // MED / JPEG-LS
            const int32_t mx = aa > bb ? aa : bb, mn = aa < bb ? aa : bb;
            if (cc >= mx) return (uint32_t)mn;
            if (cc <= mn) return (uint32_t)mx;
            return (uint32_t)(aa + bb - cc);
        }
        case 2: {                                    // Paeth
            const int32_t p = aa + bb - cc;
            const int32_t pa = p > aa ? p - aa : aa - p;
            const int32_t pb = p > bb ? p - bb : bb - p;
            const int32_t pc = p > cc ? p - cc : cc - p;
            if (pa <= pb && pa <= pc) return (uint32_t)aa;
            if (pb <= pc) return (uint32_t)bb;
            return (uint32_t)cc;
        }
        case 3: return (uint32_t)(((int64_t)aa + bb) / 2);
        default: return a;                           // left
    }
}

static size_t tile_bytes(const Img& im, int mode) {
    size_t bytes = 0;
    const uint32_t w = im.w, h = im.h, nw = im.nw;
    for (uint32_t ty = 0; ty < h; ty += (uint32_t)fr::kTileH) {
        for (uint32_t tx = 0; tx < w; tx += (uint32_t)fr::kTileW) {
            const uint32_t pw = std::min<uint32_t>((uint32_t)fr::kTileW, w - tx);
            const uint32_t ph = std::min<uint32_t>((uint32_t)fr::kTileH, h - ty);
            int width = 0;
            for (uint32_t y = ty; y < ty + ph; ++y) {
                for (uint32_t x = tx; x < tx + pw; ++x) {
                    for (uint32_t l = 0; l < nw; ++l) {
                        const size_t i = ((size_t)y * w + x) * nw + l;
                        const bool hasL = (x > tx), hasU = (y > ty);
                        if (!hasL && !hasU) continue;
                        uint32_t ref;
                        if (mode == 4) {                     // top-only: no row chain
                            ref = fr::mono(im.px[hasU ? i - (size_t)w * nw : i - nw]);
                        } else if (mode == 0 || !hasL || !hasU) {
                            ref = fr::mono(im.px[hasL ? i - nw : i - (size_t)w * nw]);
                        } else {
                            const uint32_t a = fr::mono(im.px[i - nw]);
                            const uint32_t b = fr::mono(im.px[i - (size_t)w * nw]);
                            const uint32_t c = fr::mono(im.px[i - (size_t)w * nw - nw]);
                            ref = predict(mode, a, b, c);
                        }
                        const int bits = fr::bitlen(fr::zigzag(fr::mono(im.px[i]) - ref));
                        if (bits > width) width = bits;
                    }
                }
            }
            bytes += 4;
            if (width >= 32) { bytes += (size_t)pw * ph * nw * 4; }
            else { bytes += (size_t)nw * 4 + ((size_t)(pw * ph * nw - nw) * width + 7) / 8; }
        }
    }
    return bytes;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: %s f00_mv.raw ...\n", argv[0]); return 2; }
    const char* names[5] = { "left", "med", "paeth", "avg", "top" };
    printf("%-16s %7s %7s %7s %7s %7s\n", "frame", names[0], names[1], names[2], names[3], names[4]);
    std::vector<double> sums(5, 0.0);
    int n = 0;
    for (int a = 1; a < argc; ++a) {
        Img im;
        if (!load(argv[a], im)) continue;
        const double rawMB = (double)im.w * im.h * im.nw * 4 / 1e6;
        double r[5];
        for (int m = 0; m < 5; ++m) r[m] = rawMB / (tile_bytes(im, m) / 1e6);
        const char* base = strrchr(argv[a], '/');
        printf("%-16s %7.3f %7.3f %7.3f %7.3f %7.3f\n", base ? base + 1 : argv[a], r[0], r[1], r[2], r[3], r[4]);
        for (int m = 0; m < 5; ++m) sums[m] += r[m];
        ++n;
    }
    if (n) {
        printf("\nmean:   ");
        for (int m = 0; m < 5; ++m) printf(" %s=%.3f", names[m], sums[m] / n);
        printf("\nvs left:");
        for (int m = 1; m < 5; ++m) printf("  %s %+.1f%%", names[m],
                                          100.0 * (sums[m] / sums[0] - 1.0));
        printf("\n");
    }
    return 0;
}
