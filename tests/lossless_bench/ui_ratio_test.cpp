// ui_ratio_test - what does the fr-codec (protocol v30) do to a UI plane?
//
// The UI stream is not MV/depth: it is mostly transparent zeros with sparse text and
// a 2-bit alpha. This test feeds synthetic 4K/1080p R10G10B10A2 planes (packed as one
// u32 per pixel, NW=1) through the SAME kernel the producer uses
// (tests/lossless_bench/fr_codec.h, bit-exact with lock_frame/shaders/fr_codec.hlsl)
// and prints the coded size for a few HUD densities.
//
//   g++ -O2 -std=c++17 -I. ui_ratio_test.cpp -o ui_ratio_test
//   ./ui_ratio_test
//
// What the numbers mean for the transport: see docs/design/UI_PLANE_TRANSPORT_COST.md.
// They are the *coded* bytes; the shared-memory slot itself is reserved at the worst
// case (tiles x 264 words), i.e. the codec does not shrink the ring.
#include "fr_codec.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static inline uint32_t pack_r10(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (r & 1023u) | ((g & 1023u) << 10) | ((b & 1023u) << 20) | ((a & 3u) << 30);
}

static uint32_t rnd(uint32_t& s) { s = s * 1664525u + 1013904223u; return s >> 8; }

struct Case {
    const char* name;
    double coverage;   // fraction of the screen covered by HUD blocks
    int kind;          // 0 = opaque text-like, 1 = semi-transparent gradient panel
};

static void build(std::vector<uint32_t>& px, uint32_t w, uint32_t h, const Case& c) {
    px.assign(static_cast<size_t>(w) * h, 0u);
    uint32_t seed = 12345u;
    // ~20x20 px average blocks, so coverage maps to a plausible number of HUD elements.
    const uint32_t blocks = static_cast<uint32_t>(static_cast<double>(w) * h * c.coverage / 400.0);
    for (uint32_t i = 0; i < blocks; ++i) {
        const int bx = static_cast<int>(rnd(seed) % w);
        const int by = static_cast<int>(rnd(seed) % h);
        const int bw = 6 + static_cast<int>(rnd(seed) % 60);
        const int bh = 2 + static_cast<int>(rnd(seed) % 14);
        for (int y = by; y < by + bh && y < static_cast<int>(h); ++y) {
            for (int x = bx; x < bx + bw && x < static_cast<int>(w); ++x) {
                uint32_t v;
                if (c.kind == 0) {
                    // 1 px strokes on/off inside the block, opaque white (alpha = 3)
                    v = ((x % 2) == 0) ? pack_r10(1023, 1023, 1023, 3) : 0u;
                } else {
                    // smooth semi-transparent panel: easier to predict than text
                    const uint32_t g = static_cast<uint32_t>((x * 255) / static_cast<int>(w));
                    v = pack_r10(g * 4, g * 4, g * 4, 2);
                }
                px[static_cast<size_t>(y) * w + x] = v;
            }
        }
    }
}

int main() {
    const Case cases[] = {
        {"empty (no HUD)", 0.000, 0},
        {"sparse markers 1%", 0.010, 0},
        {"text HUD 3%", 0.030, 0},
        {"dense HUD 10%", 0.100, 0},
        {"menu 60% semi-transparent", 0.600, 1},
        {"full-screen panel 100%", 1.000, 1},
    };
    struct Res { uint32_t w, h; const char* tag; } ress[] = {
        {1920, 1080, "1080p"},
        {3840, 2160, "4K"},
    };

    for (const auto& r : ress) {
        printf("== %s (%ux%u), R10A2 as NW=1 u32 ==\n", r.tag, r.w, r.h);
        const double raw = static_cast<double>(r.w) * r.h * 4.0;
        const uint64_t worst =
            static_cast<uint64_t>(((r.w + fr::kTileW - 1) / fr::kTileW) *
                                  ((r.h + fr::kTileH - 1) / fr::kTileH)) * 264ull * 4ull;
        printf("   worst-case slot (ring capacity): %.2f MB\n", worst / 1e6);
        for (const auto& c : cases) {
            std::vector<uint32_t> px;
            build(px, r.w, r.h, c);
            std::vector<uint8_t> out;
            const auto t0 = std::chrono::steady_clock::now();
            fr::encode(px.data(), r.w, r.h, 1, out);
            const auto t1 = std::chrono::steady_clock::now();
            std::vector<uint32_t> back(px.size(), 0xDEADBEEFu);
            const bool exact = fr::decode(out, back.data(), r.w, r.h, 1) &&
                               std::memcmp(px.data(), back.data(), px.size() * 4) == 0;
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            printf("   %-30s raw %7.2f MB -> %6.3f MB  %6.1fx  %s (cpu %.0f ms)\n",
                   c.name, raw / 1e6, out.size() / 1e6,
                   raw / static_cast<double>(out.size()),
                   exact ? "bit-exact" : "MISMATCH", ms);
        }
    }
    return 0;
}
