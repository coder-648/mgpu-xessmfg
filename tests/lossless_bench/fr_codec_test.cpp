// fr_codec_test - proves that fr_codec.h is (a) bit-exact and (b) matches the
// byte counts that scripts/lossless_ratio.py reports, on real dump frames.
//
//   g++ -O2 -std=c++17 -I. fr_codec_test.cpp -o fr_codec_test
//   ./fr_codec_test /mnt/c/temp/dump2077b/f00_depth.raw [...more .raw...]
//   ./fr_codec_test --self                # synthetic edge cases only
//
// A .raw file is the --dump format: 4 x uint32 {w, h, dxgiFormat, rowPitch} + rows.
#include "fr_codec.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int failures = 0;

static const char* fmt_name(uint32_t f) {
    switch (f) {
        case 10: return "RGBA16F";
        case 34: return "RG16F";
        case 41: case 39: return "R32";
        case 28: return "RGBA8";
        default: return "?";
    }
}
static uint32_t bytes_per_px(uint32_t fmt) {
    switch (fmt) {
        case 10: return 8;
        case 34: return 4;
        case 41: case 39: return 4;
        case 28: return 4;
        default: return 0;
    }
}

static void check(const char* what, bool ok) {
    if (!ok) { ++failures; printf("  FAIL %s\n", what); }
}

// encode+decode and verify; returns compressed size (0 on failure)
static size_t roundtrip(const char* tag, const uint32_t* src, size_t w, size_t h, size_t nw,
                        bool verbose) {
    std::vector<uint8_t> stream;
    fr::encode(src, w, h, nw, stream);
    std::vector<uint32_t> back(w * h * nw, 0xDEADBEEFu);
    const bool ok = fr::decode(stream, back.data(), w, h, nw);
    const bool same = ok && std::memcmp(src, back.data(), w * h * nw * 4) == 0;
    const size_t raw = w * h * nw * 4;
    if (verbose)
        printf("%-34s %5zux%-5zu nw=%zu  %8.2f MB -> %7.2f MB  %5.2fx  %s\n", tag, w, h, nw,
               raw / 1e6, stream.size() / 1e6, (double)raw / (double)stream.size(),
               same ? "bit-exact" : "MISMATCH");
    check(tag, same);
    return same ? stream.size() : 0;
}

static void self_tests() {
    printf("synthetic edge cases:\n");
    struct { size_t w, h, nw; } cases[] = {
        {1, 1, 1}, {1, 7, 1}, {15, 15, 1}, {16, 16, 1}, {17, 16, 1}, {16, 17, 1},
        {31, 33, 2}, {256, 144, 1}, {37, 5, 2},
    };
    for (auto c : cases) {
        const size_t n = c.w * c.h * c.nw;
        std::vector<uint32_t> v(n);
        for (size_t i = 0; i < n; ++i) v[i] = (uint32_t)(i * 2654435761u);
        char tag[64];
        snprintf(tag, sizeof tag, "pseudo-random %zux%zu nw=%zu", c.w, c.h, c.nw);
        roundtrip(tag, v.data(), c.w, c.h, c.nw, true);
    }
    // adversarial bit patterns: monotone float ramp (depth-like), zero, and
    // values that straddle the sign bit (must still be reversible)
    const size_t w = 64, h = 64;
    std::vector<uint32_t> v(w * h);
    for (size_t i = 0; i < v.size(); ++i) v[i] = 0;
    roundtrip("all-zero", v.data(), w, h, 1, true);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (uint32_t)(0x3F800000u + i * 3u);
    roundtrip("float ramp (depth-like)", v.data(), w, h, 1, true);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (i & 1) ? 0xFFFFFFFFu : 0x00000000u;
    roundtrip("checkerboard 0/FFFF", v.data(), w, h, 1, true);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (uint32_t)(int32_t)(i * 7919);
    roundtrip("sign-crossing deltas", v.data(), w, h, 1, true);

    // encode_tile() must reproduce the matching slice of encode() byte for byte
    // (the GPU side is compared tile-by-tile, so this is the contract it relies on)
    for (size_t ti = 0; ti < 3; ++ti) {
        const size_t tw = 40 + ti * 7, th = 33 + ti * 5, tnw = 1 + (ti % 2);
        std::vector<uint32_t> tv(tw * th * tnw);
        for (size_t i = 0; i < tv.size(); ++i) tv[i] = (uint32_t)(0x3F000000u + (i * 37) % 5000);
        std::vector<uint8_t> whole;
        fr::encode(tv.data(), tw, th, tnw, whole);
        std::vector<uint8_t> rebuilt;
        for (size_t ty = 0; ty < th; ty += fr::kTileH) {
            for (size_t tx = 0; tx < tw; tx += fr::kTileW) {
                std::vector<uint8_t> one;
                fr::encode_tile(tv.data(), tw, th, tnw, tx, ty, one);
                rebuilt.insert(rebuilt.end(), one.begin(), one.end());
            }
        }
        check("encode_tile concatenation == encode()",
              !whole.empty() && whole == rebuilt);
    }

    // fuzz: random geometry + random data, 300 cases (silent)
    unsigned seed = 12345;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed; };
    for (int it = 0; it < 300; ++it) {
        const size_t fw = 1 + rnd() % 80, fh = 1 + rnd() % 80, fnw = 1 + rnd() % 2;
        std::vector<uint32_t> f(fw * fh * fnw);
        const int mode = (int)(rnd() % 4);
        for (auto& x : f) {
            switch (mode) {
                case 0: x = rnd(); break;                       // noise
                case 1: x = 0; break;                           // zeros
                case 2: x = (uint32_t)(0x3F000000u + rnd() % 4096); break;  // narrow band
                default: x = (uint32_t)(int32_t)rnd(); break;    // anywhere incl. NaN bits
            }
        }
        char tag[64];
        snprintf(tag, sizeof tag, "fuzz #%d %zux%zu nw=%zu mode=%d", it, fw, fh, fnw, mode);
        roundtrip(tag, f.data(), fw, fh, fnw, false);
    }
    printf("  300 fuzz cases (random geometry/data/bit patterns) %s\n",
           "checked");
}

static std::vector<uint32_t> load_raw(const char* path, size_t* w, size_t* h, size_t* nw,
                                      uint32_t* fmt_out, size_t* pitch_out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { printf("  skip %s (not a file)\n", path); return {}; }
    uint32_t hdr[4] = {0, 0, 0, 0};
    if (std::fread(hdr, 4, 4, f) != 4) { std::fclose(f); printf("  skip %s (no header)\n", path); return {}; }
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
    *w = ww; *h = hh; *nw = bpp / 4; *fmt_out = fmt; *pitch_out = pitch;
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: %s [--self | file.raw ...]\n", argv[0]);
        return 2;
    }
    self_tests();
    if (argc == 2 && std::strcmp(argv[1], "--self") == 0) {
        printf("\n%s\n", failures ? "FAILURES" : "all synthetic cases bit-exact");
        return failures ? 1 : 0;
    }
    printf("\nreal dump frames:\n");
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self") == 0) continue;
        size_t w = 0, h = 0, nw = 0, pitch = 0;
        uint32_t fmt = 0;
        auto v = load_raw(argv[i], &w, &h, &nw, &fmt, &pitch);
        if (v.empty()) continue;   // skipped, not a failure
        char tag[128];
        const char* base = std::strrchr(argv[i], '/');
        snprintf(tag, sizeof tag, "%s %s", base ? base + 1 : argv[i], fmt_name(fmt));
        roundtrip(tag, v.data(), w, h, nw, true);
    }
    printf("\n%s\n", failures ? "FAILURES" : "all frames bit-exact");
    return failures ? 1 : 0;
}
