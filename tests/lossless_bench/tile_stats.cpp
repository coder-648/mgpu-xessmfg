// tile_stats - per-tile mode / bit-width distribution of the production codec (fr_codec.h)
// on a real --dump file. Same loader and tile planner as fr_codec_test, but instead of
// proving the round trip it answers the encoder-cost question:
//
//   * how often the raw-escape mode (2) is chosen  -> those tiles skip the prefix scan
//     entirely in the GPU kernel (fr_codec.hlsl returns before the Hillis-Steele block),
//     so the scan's ablation number is only paid on the remaining tiles;
//   * which bit width wins (the packing traffic scales with it);
//   * how much of the stream is 32-bit patch escapes (mode 1).
//
//   g++ -O2 -std=c++17 -I. tile_stats.cpp -o tile_stats
//   ./tile_stats <dir>/*_mv.raw <dir>/*_depth.raw
//
// A .raw file is the --dump format: 4 x uint32 {w, h, dxgiFormat, rowPitch} + rows.
#include "fr_codec.h"

#include <cstdio>
#include <cstring>
#include <vector>

static uint32_t bytes_per_px(uint32_t fmt) {
    switch (fmt) {
        case 10: return 8;                        // RGBA16F (colour)
        case 34: return 4;                        // R16G16_FLOAT (MV)
        case 41: case 39: return 4;               // R32 / D32 (depth)
        case 28: return 4;                        // RGBA8
        default: return 0;
    }
}

static std::vector<uint32_t> load_raw(const char* path, size_t* w, size_t* h, size_t* nw,
                                      uint32_t* fmt_out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { printf("  skip %s (not a file)\n", path); return {}; }
    uint32_t hdr[4] = {0, 0, 0, 0};
    if (std::fread(hdr, 4, 4, f) != 4) { std::fclose(f); return {}; }
    const uint32_t ww = hdr[0], hh = hdr[1], fmt = hdr[2], pitch = hdr[3];
    const uint32_t bpp = bytes_per_px(fmt);
    if (!bpp || !ww || !hh || pitch < (size_t)ww * bpp) {
        printf("  skip %s (bad header w=%u h=%u fmt=%u pitch=%u)\n", path, ww, hh, fmt, pitch);
        std::fclose(f);
        return {};
    }
    const size_t row = (size_t)ww * bpp;
    std::vector<uint32_t> out((size_t)ww * hh * (bpp / 4));
    std::vector<uint8_t> tmp(pitch);
    for (uint32_t y = 0; y < hh; ++y) {
        if (std::fread(tmp.data(), 1, pitch, f) != pitch) { std::fclose(f); return {}; }
        std::memcpy((uint8_t*)out.data() + (size_t)y * row, tmp.data(), row);
    }
    std::fclose(f);
    *w = ww; *h = hh; *nw = bpp / 4; *fmt_out = fmt;
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: %s file.raw [...]\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        size_t w = 0, h = 0, nw = 0;
        uint32_t fmt = 0;
        auto v = load_raw(argv[i], &w, &h, &nw, &fmt);
        if (v.empty()) continue;
        const char* base = std::strrchr(argv[i], '/');
        base = base ? base + 1 : argv[i];

        size_t tiles = 0, mode_cnt[3] = {0, 0, 0};
        size_t width_hist[33] = {0};
        size_t bits_total = 0, bits_by_mode[3] = {0, 0, 0};
        size_t patch_tiles = 0, patch_vals = 0, patch_total = 0;

        fr::TilePlan p;
        for (size_t ty = 0; ty < h; ty += fr::kTileH) {
            for (size_t tx = 0; tx < w; tx += fr::kTileW) {
                fr::plan_tile(v.data(), w, h, nw, tx, ty, p);
                ++tiles;
                const int m = (p.mode >= 0 && p.mode <= 2) ? p.mode : 0;
                ++mode_cnt[m];
                bits_total += p.bits;
                bits_by_mode[m] += p.bits;
                if (p.w >= 0 && p.w <= 32) ++width_hist[p.w];
                if (m == fr::kModeExc) {
                    ++patch_tiles;
                    for (uint8_t b : p.patch) { ++patch_total; if (b) ++patch_vals; }
                }
            }
        }
        const double raw_mb = (double)(w * h * nw * 4) / 1e6;
        const double coded_mb = (double)bits_total / 8.0 / 1e6;
        printf("%-22s %zux%zu nw=%zu  %7.2f -> %6.2f MB  %.2fx  tiles=%zu\n",
               base, w, h, nw, raw_mb, coded_mb,
               coded_mb > 0.0 ? raw_mb / coded_mb : 0.0, tiles);
        printf("    mode: plain %5.1f%%  exc %5.1f%%  raw %5.1f%%   (raw tiles skip the scan)\n",
               100.0 * (double)mode_cnt[fr::kModePlain] / (double)tiles,
               100.0 * (double)mode_cnt[fr::kModeExc] / (double)tiles,
               100.0 * (double)mode_cnt[fr::kModeRaw] / (double)tiles);
        printf("    bits: plain %5.1f%%  exc %5.1f%%  raw %5.1f%%",
               100.0 * (double)bits_by_mode[fr::kModePlain] / (double)bits_total,
               100.0 * (double)bits_by_mode[fr::kModeExc] / (double)bits_total,
               100.0 * (double)bits_by_mode[fr::kModeRaw] / (double)bits_total);
        if (patch_total) {
            printf("   patches %.1f%% of exc values", 100.0 * (double)patch_vals / (double)patch_total);
        }
        printf("\n    width:");
        for (int b = 0; b <= 32; ++b) {
            if (width_hist[b] == 0) continue;
            printf(" %d:%.1f%%", b, 100.0 * (double)width_hist[b] / (double)tiles);
        }
        printf("\n");
    }
    return 0;
}
