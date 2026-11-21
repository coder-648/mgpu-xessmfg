// downsample_test - answer one question: if we keep today's 150% stream and only
// compress *that*, what ratio do we get?
//
//   g++ -O2 -std=c++17 -I. downsample_test.cpp -o dst_test
//   ./dst_test <f00_mv.raw> [more ...]
//
// For each input it prints:
//   source            w x h (bytes)
//   downsample 150%   dw x dh (bytes)      <- what the producer copies today
//   codec(150%)       compressed bytes, ratio vs the downsampled size
//   codec(100%)       compressed bytes, ratio vs the source size
//
// The 150% filter here is a per-lane median over the same 3x3 footprint the HLSL
// uses (downsample_mv.hlsl picks a real sample / medoid; a median is the same
// family and keeps the statistics honest). It is NOT bit-identical to the shader,
// so treat the 150% ratio as an estimate - it is a measurement of the *data*,
// not of our shader.
#include "fr_codec.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static uint32_t bytes_per_px(uint32_t fmt) {
    switch (fmt) {
        case 10: return 8;
        case 34: case 41: case 39: case 28: return 4;
        default: return 0;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: %s f00_mv.raw ...\n", argv[0]); return 2; }
    for (int a = 1; a < argc; ++a) {
        FILE* f = fopen(argv[a], "rb");
        if (!f) { printf("skip %s\n", argv[a]); continue; }
        uint32_t hdr[4] = {0, 0, 0, 0}, w = 0, h = 0, fmt = 0, pitch = 0;
        if (fread(hdr, 4, 4, f) != 4) { fclose(f); continue; }
        w = hdr[0]; h = hdr[1]; fmt = hdr[2]; pitch = hdr[3];
        const uint32_t bpp = bytes_per_px(fmt), nw = bpp / 4;
        if (!bpp || !w || !h) { fclose(f); continue; }
        const size_t row = (size_t)w * bpp;
        std::vector<uint32_t> src((size_t)w * h * nw);
        std::vector<uint8_t> tmp(pitch);
        for (uint32_t y = 0; y < h; ++y) {
            if (fread(tmp.data(), 1, pitch, f) != pitch) break;
            memcpy((uint8_t*)src.data() + (size_t)y * row, tmp.data(), row);
        }
        fclose(f);

        // --- 150%: every dimension 2/3
        const uint32_t dw = (uint32_t)((uint64_t)w * 2 / 3), dh = (uint32_t)((uint64_t)h * 2 / 3);
        std::vector<uint32_t> dst((size_t)dw * dh * nw);
        const double sx = (double)w / dw, sy = (double)h / dh;
        for (uint32_t y = 0; y < dh; ++y) {
            for (uint32_t x = 0; x < dw; ++x) {
                const double cx = (x + 0.5) * sx - 0.5, cy = (y + 0.5) * sy - 0.5;
                const double ox = sx / 3.0, oy = sy / 3.0;
                for (uint32_t l = 0; l < nw; ++l) {
                    uint32_t cand[9];
                    int n = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            int px = (int)(cx + dx * ox + 0.5), py = (int)(cy + dy * oy + 0.5);
                            if (px < 0) px = 0; if (px > (int)w - 1) px = (int)w - 1;
                            if (py < 0) py = 0; if (py > (int)h - 1) py = (int)h - 1;
                            cand[n++] = src[((size_t)py * w + px) * nw + l];
                        }
                    }
                    for (int i = 1; i < n; ++i) {          // insertion sort on the ordered int
                        const uint32_t k = cand[i];
                        const uint32_t mk = fr::mono(k);
                        int j = i - 1;
                        while (j >= 0 && fr::mono(cand[j]) > mk) { cand[j + 1] = cand[j]; --j; }
                        cand[j + 1] = k;
                    }
                    dst[((size_t)y * dw + x) * nw + l] = cand[4];
                }
            }
        }

        std::vector<uint8_t> s100, s150;
        fr::encode(src.data(), w, h, nw, s100);
        fr::encode(dst.data(), dw, dh, nw, s150);
        const double raw100 = (double)w * h * bpp, raw150 = (double)dw * dh * bpp;
        const char* base = strrchr(argv[a], '/');
        printf("%-14s %ux%u %5.1f MB | 150%% %ux%u %5.1f MB | codec(150%%) %5.2f MB = %.2fx"
               " | codec(100%%) %5.2f MB = %.2fx\n",
               base ? base + 1 : argv[a], w, h, raw100 / 1e6, dw, dh, raw150 / 1e6,
               s150.size() / 1e6, raw150 / (double)s150.size(),
               s100.size() / 1e6, raw100 / (double)s100.size());
    }
    return 0;
}
