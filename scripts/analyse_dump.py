#!/usr/bin/env python3
"""Offline analysis of a bridge --dump export: are colour, depth and motion
vectors from the same game frame?

    python3 scripts/analyse_dump.py <dump-dir>

The bridge writes, per frame, <dir>/fNN_colour.raw / _depth.raw / _mv.raw plus
_meta.txt (see dump_frame() in src/bridge/bridge.cpp).  Each .raw starts with
4 x uint32 { width, height, dxgiFormat, rowPitchBytes } and then the rows.

What it answers
---------------
1. How much did the picture move between consecutive dumped frames?  (global
translation from a coarse SSD search on luma, reported in full-res pixels)
2. Does frame i's MV match the motion (i-1 -> i) or (i -> i+1)?
   i-1 -> i : the MV belongs to the same frame as the colour (correct)
   i -> i+1 : we are feeding motion from one frame ahead (what --mv-newest
              can do, see docs/findings/HANDOFF.md section 8.6)
3. The same question for depth, using the depth map's own apparent motion.
4. Sanity: MV statistics, depth range and Z convention.

The scene must contain real motion: a static picture gives 0 px and every
offset looks equally good.  The script says when that happens.
"""

import os
import sys
import glob

try:
    import numpy as np
except ImportError:
    sys.exit("numpy is required: sudo apt install python3-numpy")

# DXGI formats the bridge may tag (only these are handled)
FMT = {
    10: ("<f2", 4, "R16G16B16A16_FLOAT"),
    34: ("<f2", 2, "R16G16_FLOAT"),
    41: ("<f4", 1, "R32_FLOAT"),
    39: ("<f4", 1, "R32_TYPELESS(as float)"),
    28: ("<u1", 4, "R8G8B8A8_UNORM"),
}


def load_raw(path):
    """-> (memmap[h,w,ch], info); memmap keeps 4K frames cheap."""
    with open(path, "rb") as f:
        hdr = np.fromfile(f, dtype=np.uint32, count=4)
    w, h, fmt, pitch = (int(v) for v in hdr)
    if fmt not in FMT:
        raise RuntimeError("%s: unsupported DXGI format %d" % (path, fmt))
    npfmt, ch, name = FMT[fmt]
    dt = np.dtype(npfmt)
    row_elems = w * ch
    expect = 16 + row_elems * dt.itemsize * h
    size = os.path.getsize(path)
    if size != expect:
        raise RuntimeError("%s: size %d != expected %d (%dx%d %s)"
                           % (path, size, expect, w, h, name))
    arr = np.memmap(path, dtype=dt, mode="r", offset=16, shape=(h, row_elems))
    return arr.reshape(h, w, ch), {"w": w, "h": h, "fmt": name}


def read_meta(path):
    meta = {}
    with open(path, "r") as f:
        for line in f:
            parts = line.split()
            if parts:
                meta[parts[0]] = parts[1:]
    return meta


def blocks(img, factor=8):
    """Average a 2D map, or the luma of an RGB image, over factor x factor cells."""
    if img.ndim == 3:
        if img.dtype == np.float16:
            img = img.astype(np.float32)
        if img.shape[2] >= 3:
            img = (img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152
                   + img[:, :, 2] * 0.0722)
        else:
            img = img[:, :, 0]
    else:
        img = np.asarray(img, dtype=np.float32)
    h = (img.shape[0] // factor) * factor
    w = (img.shape[1] // factor) * factor
    return img[:h, :w].reshape(h // factor, factor, w // factor, factor).mean(axis=(1, 3))


def mv_blocks(mv, factor=8):
    a = np.asarray(mv, dtype=np.float32)
    h = (a.shape[0] // factor) * factor
    w = (a.shape[1] // factor) * factor
    return (a[:h, :w]
            .reshape(h // factor, factor, w // factor, factor, a.shape[2])
            .mean(axis=(1, 3)))

def best_shift(a, b, radius=6):
    """Global integer translation mapping a onto b, in units of a's own pixels.

    Brute-force SSD over a +-radius window plus parabolic sub-cell refinement.
    Cheap because the inputs are already block-averaged.
    """
    a = a - a.mean()
    b = b - b.mean()
    h, w = a.shape
    m = radius + 2
    best = (1e30, 0, 0)
    scores = np.full((2 * radius + 1, 2 * radius + 1), np.nan)
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            ay0, ay1 = max(0, dy), min(h, h + dy)
            ax0, ax1 = max(0, dx), min(w, w + dx)
            by0, by1 = max(0, -dy), min(h, h - dy)
            bx0, bx1 = max(0, -dx), min(w, w - dx)
            if ay1 - ay0 < m or ax1 - ax0 < m:
                continue
            d = a[ay0:ay1, ax0:ax1] - b[by0:by1, bx0:bx1]
            s = float((d * d).mean())
            scores[dy + radius, dx + radius] = s
            if s < best[0]:
                best = (s, dx, dy)
    s0, dx0, dy0 = best

    def refine(cm, c0, cp):
        den = cm - 2.0 * c0 + cp
        if abs(den) < 1e-12:
            return 0.0
        return 0.5 * (cm - cp) / den

    iy, ix = dy0 + radius, dx0 + radius
    sy = 0.0
    sx = 0.0
    if 0 < iy < scores.shape[0] - 1:
        sy = refine(scores[iy - 1, ix], scores[iy, ix], scores[iy + 1, ix])
    if 0 < ix < scores.shape[1] - 1:
        sx = refine(scores[iy, ix - 1], scores[iy, ix], scores[iy, ix + 1])
    # best_shift samples a at (x+dx); content motion is the opposite sign.
    return -(dx0 + sx), -(dy0 + sy), float(np.sqrt(max(s0, 0.0)))

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    d = sys.argv[1]
    metas = sorted(glob.glob(os.path.join(d, "f*_meta.txt")))
    if not metas:
        sys.exit("no f*_meta.txt in %s" % d)

    frames = []
    for mp in metas:
        base = mp[:-len("_meta.txt")]
        meta = read_meta(mp)
        colour, cinfo = load_raw(base + "_colour.raw")
        depth, dinfo = load_raw(base + "_depth.raw")
        mv, minfo = load_raw(base + "_mv.raw")
        frames.append({"meta": meta, "colour": colour, "cinfo": cinfo,
                       "depth": depth, "mv": mv, "minfo": minfo})
        print("%s: colour %dx%d %s | depth %dx%d %s | mv %dx%d %s | slot %s frame_id %s"
              % (os.path.basename(base), cinfo["w"], cinfo["h"], cinfo["fmt"],
                 dinfo["w"], dinfo["h"], dinfo["fmt"],
                 minfo["w"], minfo["h"], minfo["fmt"],
                 " ".join(meta.get("slot", ["?"])),
                 " ".join(meta.get("producer_frame_id", ["?"]))))

    flags = frames[0]["meta"].get("flags", [])
    if len(flags) >= 4 and (flags[1] != "0" or flags[3] != "0"):
        print("WARNING: dump taken with --no-mv/--no-depth - the pairing question "
              "cannot be answered from it.")

    cw = frames[0]["cinfo"]["w"]
    ch = frames[0]["cinfo"]["h"]

    print("\n--- colour motion between consecutive dumped frames (full-res px) ---")
    lumas = [blocks(f["colour"]) for f in frames]
    bf = cw / float(lumas[0].shape[1])          # one luma cell -> full-res px
    colour_motion = []
    for i in range(len(frames) - 1):
        dx, dy, score = best_shift(lumas[i], lumas[i + 1])
        colour_motion.append((dx * bf, dy * bf, score))
        print("  %d -> %d : dx=%+8.2f px  dy=%+8.2f px  (residual %.4f)"
              % (i, i + 1, dx * bf, dy * bf, score))

    print("\n--- what the dumped MV claims for each frame ---")
    mv_means = []
    for i, f in enumerate(frames):
        msx = float(f["meta"].get("mv_scale", ["1", "1"])[0])
        msy = float(f["meta"].get("mv_scale", ["1", "1"])[1])
        # The bridge converts with  uv = mv * (1/mv_scale)  (run_frame: msx =
        # 1/mv_scale), so full-res pixels are uv * colour size.
        sx = (1.0 / msx) if msx > 0 else (1.0 / cw)
        sy = (1.0 / msy) if msy > 0 else (1.0 / ch)
        field = mv_blocks(f["mv"])
        px = field * np.array([sx * cw, sy * ch], dtype=np.float32)
        mv_means.append((float(px[:, :, 0].mean()), float(px[:, :, 1].mean()),
                         float(np.abs(px).mean())))
        print("  frame %d : mean=(%+8.2f, %+8.2f) px   mean|mv|=%7.2f px   zeros=%.0f%%"
              % (i, mv_means[-1][0], mv_means[-1][1], mv_means[-1][2],
                 100.0 * float((np.abs(field).sum(axis=2) < 1e-6).mean())))

    print("\n--- verdict: which colour pair does frame i's MV match? ---")
    for i in range(1, len(frames) - 1):
        mvx, mvy, _ = mv_means[i]
        prev = colour_motion[i - 1]             # (i-1) -> i
        nxt = colour_motion[i]                  # i -> (i+1)
        if abs(mvx) < 0.4 and abs(mvy) < 0.4:
            print("  frame %d : MV is ~0 - no usable motion, skipped" % i)
            continue
        if min(prev[0] ** 2 + prev[1] ** 2, nxt[0] ** 2 + nxt[1] ** 2) < 1.0:
            print("  frame %d : picture barely moved (%.2f/%.2f px) - inconclusive,"
                  " capture a panning scene" % (i, prev[0], prev[1]))
            continue
        # If the motion barely changes from frame to frame, both offsets predict
        # the same thing and the comparison proves nothing.
        sep = abs(prev[0] - nxt[0]) + abs(prev[1] - nxt[1])
        if sep < 1.0:
            print("  frame %d : motion is nearly constant (%.2f vs %.2f px) -"
                  " inconclusive, capture a scene where the speed changes"
                  % (i, prev[0], nxt[0]))
            continue
        d_prev = abs(mvx - prev[0]) + abs(mvy - prev[1])
        d_next = abs(mvx - nxt[0]) + abs(mvy - nxt[1])
        if d_prev < d_next:
            print("  frame %d : MV ~ (%d->%d)  |diff| %.2f vs %.2f"
                  "  => SAME FRAME as the colour (correct)"
                  % (i, i - 1, i, d_prev, d_next))
        else:
            print("  frame %d : MV ~ (%d->%d)  |diff| %.2f vs %.2f"
                  "  => MV IS ONE FRAME AHEAD (mismatch)"
                  % (i, i, i + 1, d_prev, d_next))

    print("\n--- depth ---")
    depths = [blocks(f["depth"]) for f in frames]
    for i, f in enumerate(frames):
        dv = np.asarray(f["depth"], dtype=np.float32)
        print("  frame %d : min=%.6f max=%.6f mean=%.6f  zeros=%.1f%%  ones=%.1f%%"
              % (i, float(dv.min()), float(dv.max()), float(dv.mean()),
                 100.0 * float((dv == 0).mean()), 100.0 * float((dv == 1).mean())))
    for i in range(len(frames) - 1):
        dx, dy, score = best_shift(depths[i], depths[i + 1])
        print("  depth %d -> %d : dx=%+8.2f px  dy=%+8.2f px  (residual %.4f)"
              % (i, i + 1, dx * bf, dy * bf, score))
    print("\nA reversed-Z depth (UE5, CP2077) has near=1 far=0; if the values sit")
    print("almost entirely near 0 the map is standard-Z and the synthesised")
    print("projection matrix (--fg-proj-rz / --fg-fov) is inverted for it.")


if __name__ == "__main__":
    main()

