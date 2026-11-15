#!/usr/bin/env python3
"""Convert a R10G10B10A2 raw dump (frame_dump.raw / marker_out.raw) to a PNG.

    raw_to_png.py <raw> <out.png> <width> <height> [max_width]

PQ is decoded to display-linear, then to sRGB so the picture looks like the screen.
Downsampling takes the MAX of each block, so a thin bright/dark artefact survives the
reduction instead of being averaged away. No third-party modules (PIL is not installed
in this environment): the PNG is written by hand.
"""
import struct
import sys
import zlib


def pq_to_linear(v):
    v = min(max(v, 0.0), 1.0)
    m1, m2 = 0.1593017578125, 78.84375
    c1, c2, c3 = 0.8359375, 18.8515625, 18.6875
    p = v ** (1.0 / m2)
    den = c2 - c3 * p
    if den <= 1e-6:
        return 1.0
    num = max(p - c1, 0.0)
    return (num / den) ** (1.0 / m1)


def to_srgb(v):
    v = min(max(v, 0.0), 1.0)
    return v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1.0 / 2.4) - 0.055


_LUT = [int(255.0 * to_srgb(pq_to_linear(i / 1023.0)) + 0.5) for i in range(1024)]


def write_png(path, w, h, rgb):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * w * 3:(y + 1) * w * 3]

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    raw_path, out_path, sw, sh = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    max_w = int(sys.argv[5]) if len(sys.argv) > 5 else 1200
    data = open(raw_path, "rb").read()
    need = sw * sh * 4
    if len(data) < need:
        sys.exit("raw is %d bytes, need %d for %dx%d" % (len(data), need, sw, sh))
    step = max(1, (sw + max_w - 1) // max_w)
    ow, oh = sw // step, sh // step
    rgb = bytearray(ow * oh * 3)
    for oy in range(oh):
        for ox in range(ow):
            mr = mg = mb = 0
            for dy in range(step):
                row = (oy * step + dy) * sw
                for dx in range(step):
                    o = (row + ox * step + dx) * 4
                    v = data[o] | (data[o + 1] << 8) | (data[o + 2] << 16) | (data[o + 3] << 24)
                    r = _LUT[v & 0x3FF]
                    g = _LUT[(v >> 10) & 0x3FF]
                    b = _LUT[(v >> 20) & 0x3FF]
                    if r > mr: mr = r
                    if g > mg: mg = g
                    if b > mb: mb = b
            i = (oy * ow + ox) * 3
            rgb[i] = mr
            rgb[i + 1] = mg
            rgb[i + 2] = mb
    write_png(out_path, ow, oh, rgb)
    print("%s -> %s (%dx%d, step %d)" % (raw_path, out_path, ow, oh, step))


if __name__ == "__main__":
    main()
