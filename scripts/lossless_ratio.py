#!/usr/bin/env python3
"""Offline: how well would a lossless GPU codec compress this dump?

    python3 scripts/lossless_ratio.py <dump-dir> [--frames N] [--no-zlib]

Reads the raw frames written by ``--dump`` (``lock_frame/modules/110_pair_dump.inc``,
same format as ``scripts/analyse_dump.py``):

    fNN_{colour,depth,mv}.raw = 4 x uint32 {width, height, dxgiFormat, rowPitchBytes}
                                + height rows of rowPitch bytes

This is the fast **1-D approximation** (one pass, pure Python). The normative codec is
``tests/lossless_bench/fr_codec.h`` (2-D 16x16 tiles, left/top prediction, per-lane
absolutes) - it is bit-exact and lands consistently *above* these numbers (depth 1.94 vs
1.88, colour 1.22 vs 1.06 on real CP2077 frames), so screening with this script is safe.

Three numbers per file
----------------------
``zlib``  DEFLATE over the raw bytes - the *optimistic* proxy for "just link nvComp":
          LZ4/GDeflate/Zstd add GPU parallelism, not ratio.
``word``  the codec this repo would actually use (LOSSLESS_TRANSPORT.md §6):
          map each 32-bit word to a monotone integer, delta against the left
          neighbour, zigzag, store every 16x16 tile at its own max bit width.
          The printed number is the exact byte count of that format, headers included.
``half``  same, but the two fp16 lanes of each word are coded independently
          (what MV wants: x and y are unrelated). Only for RG16F / RGBA16F.

Decision rule (§8): use the **p5** (worst frame), not the mean - one 1.0x frame
in twenty turns the whole idea into an expansion.  Exit status is 0 either way:
this is a measurement, not a gate.

No third-party imports (numpy is not required), but it is a pure-Python loop:
roughly 5-15 s per 4K frame per codec, so ``--frames`` defaults to 10.
"""

import array
import glob
import os
import sys
import time
import zlib

HDR = 16  # 4 x uint32
TILE = 256  # values per tile (16x16 pixels for a one-word-per-pixel stream)
TILE_HDR = 4  # bytes per tile header: mode + bit width, padded to 4

# dxgiFormat -> (name, channels, bytes/channel, kind)
FMT = {
    10: ("R16G16B16A16_FLOAT", 4, 2, "colour"),
    34: ("R16G16_FLOAT", 2, 2, "mv"),
    41: ("R32_FLOAT", 1, 4, "depth"),
    39: ("R32_TYPELESS(as float)", 1, 4, "depth"),
    28: ("R8G8B8A8_UNORM", 4, 1, "colour"),
}


def read_header(path):
    with open(path, "rb") as f:
        raw = f.read(HDR)
    if len(raw) != HDR:
        raise RuntimeError("%s: shorter than the 16-byte header" % path)
    w, h, fmt, pitch = (int(v) for v in array.array("I", raw))
    if fmt not in FMT:
        raise RuntimeError("%s: unsupported dxgiFormat %d" % (path, fmt))
    name, ch, bpc, kind = FMT[fmt]
    bpp = ch * bpc
    need = HDR + (h - 1) * pitch + w * bpp
    if os.path.getsize(path) < need:
        raise RuntimeError("%s: size %d < expected %d" % (path, os.path.getsize(path), need))
    return w, h, fmt, pitch, kind, name, bpp


def load_words(path, w, h, pitch, bpp):
    """-> (array('I'), words_per_row): row pitch removed, 8-byte formats
    contribute two words per pixel (the codec is defined per 32-bit word)."""
    wpr = w * bpp // 4
    row = wpr * 4
    out = array.array("I")
    with open(path, "rb") as f:
        f.seek(HDR)
        for _ in range(h):
            out.frombytes(f.read(row))
            f.seek(pitch - row, 1)
    return out, wpr



def _mono32(v):
    return (v ^ 0xFFFFFFFF) if (v >> 31) else (v ^ 0x80000000)


def _mono16(v):
    return (v ^ 0xFFFF) if (v >> 15) else (v ^ 0x8000)


def scan_word(words, wpr, h):
    """-> (bytes, tiles) for the 32-bit-word codec (§6.1)."""
    bits = 0
    tiles = 0
    cur = 0
    n = 0
    for y in range(h):
        base = y * wpr
        prev = _mono32(words[base])
        for x in range(wpr):
            m = _mono32(words[base + x])
            d = (m - prev) & 0xFFFFFFFF
            z = ((d << 1) ^ (0xFFFFFFFF if (d >> 31) else 0)) & 0xFFFFFFFF
            b = z.bit_length()
            if b > cur:
                cur = b
            prev = m
            n += 1
            if n == TILE:
                bits += cur * TILE
                cur = 0
                n = 0
                tiles += 1
    if n:
        bits += cur * n  # last partial tile: same width, fewer values
        tiles += 1
    return (bits + 7) // 8 + tiles * TILE_HDR, tiles


def scan_half(words, wpr, h):
    """-> (bytes, tiles) with the two fp16 lanes coded independently."""
    bits = 0
    tiles = 0
    cur_lo = cur_hi = 0
    n = 0
    for y in range(h):
        base = y * wpr
        v = words[base]
        prev_lo = _mono16(v & 0xFFFF)
        prev_hi = _mono16(v >> 16)
        for x in range(wpr):
            v = words[base + x]
            lo = _mono16(v & 0xFFFF)
            hi = _mono16(v >> 16)
            dlo = (lo - prev_lo) & 0xFFFF
            dhi = (hi - prev_hi) & 0xFFFF
            b = (((dlo << 1) ^ (0xFFFF if (dlo >> 15) else 0)) & 0xFFFF).bit_length()
            if b > cur_lo:
                cur_lo = b
            b = (((dhi << 1) ^ (0xFFFF if (dhi >> 15) else 0)) & 0xFFFF).bit_length()
            if b > cur_hi:
                cur_hi = b
            prev_lo, prev_hi = lo, hi
            n += 1
            if n == TILE:
                bits += (cur_lo + cur_hi) * TILE
                cur_lo = cur_hi = 0
                n = 0
                tiles += 1
    if n:
        bits += (cur_lo + cur_hi) * n
        tiles += 1
    return (bits + 7) // 8 + tiles * TILE_HDR, tiles


def pct(vals, p):
    if not vals:
        return 0.0
    s = sorted(vals)
    i = min(len(s) - 1, max(0, int(round((p / 100.0) * (len(s) - 1)))))
    return s[i]

def main(argv):
    if len(argv) < 2 or argv[1].startswith("-"):
        sys.exit(__doc__)
    dump = argv[1]
    nframes = 10
    use_zlib = True
    i = 2
    while i < len(argv):
        if argv[i] == "--frames":
            nframes = int(argv[i + 1])
            i += 2
        elif argv[i] == "--no-zlib":
            use_zlib = False
            i += 1
        else:
            sys.exit("unknown option %s\n\n%s" % (argv[i], __doc__))

    files = []
    for kind in ("colour", "mv", "depth"):
        files += sorted(glob.glob(os.path.join(dump, "*_%s.raw" % kind)))[:nframes]
    if not files:
        sys.exit("no *_colour.raw / *_mv.raw / *_depth.raw in %s" % dump)

    rows = {"colour": [], "mv": [], "depth": []}
    print("%-26s %-7s %8s %6s %6s %6s" % ("file", "kind", "raw MB", "zlib", "word", "half"))
    for path in files:
        try:
            w, h, fmt, pitch, kind, name, bpp = read_header(path)
        except RuntimeError as e:
            print("%-26s SKIP  %s" % (os.path.basename(path), e))
            continue
        raw = w * h * bpp
        t0 = time.time()
        words, wpr = load_words(path, w, h, pitch, bpp)
        zr = wr = hr = 0.0
        if use_zlib:
            with open(path, "rb") as f:
                f.seek(HDR)
                zr = raw / max(1, len(zlib.compress(f.read(), 6)))
        n, _ = scan_word(words, wpr, h)
        wr = raw / max(1, n)
        if fmt in (10, 34):
            n, _ = scan_half(words, wpr, h)
            hr = raw / max(1, n)
        print("%-26s %-7s %8.1f %6s %6.2f %6.2f   (%.1fs)"
              % (os.path.basename(path), kind, raw / 1e6,
                 ("%.2f" % zr) if zr else "-", wr, hr, time.time() - t0))
        if wr > 64.0:
            print("      ^ degenerate stream (ratio > 64x): almost certainly all-zero or a "
                  "constant\n        dummy. Check the capture: `--no-mv` / `SkipMv=1` dumps a zero "
                  "MV buffer.")
        rows[kind].append((zr, wr, hr))

    print()
    for kind, rs in rows.items():
        if not rs:
            continue
        print("%s (n=%d)" % (kind, len(rs)))
        for idx, label in ((0, "zlib"), (1, "word"), (2, "half")):
            vals = [r[idx] for r in rs if r[idx]]
            if vals:
                print("   %-5s p5=%.2f  p50=%.2f  p95=%.2f  min=%.2f"
                      % (label, pct(vals, 5), pct(vals, 50), pct(vals, 95), min(vals)))
    mv = [r[1] for r in rows["mv"] if r[1]]
    print()
    if not mv:
        print("VERDICT: no usable MV frames -> gate 1 cannot be decided.")
    elif max(mv) > 64.0:
        print("VERDICT: MV looks degenerate (%.0fx) - the dump used --no-mv / SkipMv=1."
              % max(mv))
        print("         re-capture with MV enabled (LOSSLESS_TRANSPORT.md 11.3).")
    else:
        p5 = pct(mv, 5)
        print("VERDICT: MV word p5=%.2f -> %s" % (p5, "GO (>= 2.0)" if p5 >= 2.0 else
              ("STOP (< 1.5)" if p5 < 1.5 else "MARGINAL (1.5-2.0): needs gate 2 to justify")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))


