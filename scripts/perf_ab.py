#!/usr/bin/env python3
"""perf_ab.py - compare two or more bridge.exe logs from an A/B run.

The point of this script is that the numbers that matter for a performance
change are NOT fps (a paced loop is locked to the limiter) but the stage
breakdown, the cross-adapter traffic and the cadence:

    gpu       waiting for the previous frame's GPU work  (the real budget)
    present   how long XeFG blocks us
    colour    D3D11 keyed-mutex + full-frame CopyResource
    copy      D3D12 queue waiting for that copy
    work      our CPU up to submitting the command list
    cpu       total - every stage above (no home = it is ours)
    bus       MB per frame leaving the render card + GB/s

Usage:
    python3 scripts/perf_ab.py perf_A.txt perf_B.txt ...
    python3 scripts/perf_ab.py --skip 3 perf_A.txt perf_B.txt   # drop warm-up windows

Each file is summarised over all `breakdown:` windows (median + mean + worst),
so a single run per configuration is enough as long as the scene was the same.
"""
import re
import statistics
import sys

# Fields we report, in the order they appear in the breakdown line.
FIELDS = ["gpu", "present", "colour", "copy", "work", "wait", "mvs", "of",
          "xell", "fg", "age", "total", "cpu", "load", "slow", "skip"]

KV = re.compile(r"([a-zA-Z_]+)=(-?[\d.]+)")


def parse(path, skip):
    txt = open(path, encoding="utf-8", errors="replace").read()

    windows = []
    for m in re.finditer(r"breakdown: (.*)", txt):
        d = dict(KV.findall(m.group(1)))
        if "gpu" in d and "total" in d:
            windows.append(d)
    if skip:
        windows = windows[skip:]

    cadence = []
    for m in re.finditer(r"out = ([\d.]+) vblank/frame", txt):
        cadence.append(float(m.group(1)))
    drops = [float(m.group(1)) for m in re.finditer(r"lock=[\d.]+ drops=(\d+)", txt)]

    bus_mb = [float(m.group(1)) for m in re.finditer(r"= ([\d.]+)MB/frame", txt)]
    bus_gbs = [float(m.group(1)) for m in re.finditer(r"\(([\d.]+) GB/s", txt)]

    # Producer/consumer frame rate, if the file has the cadence line.
    in_ms = [float(m.group(1)) for m in re.finditer(r"input=([\d.]+)ms", txt)]

    return {
        "path": path,
        "n": len(windows),
        "win": windows,
        "cadence": cadence,
        "drops": drops,
        "bus_mb": bus_mb,
        "bus_gbs": bus_gbs,
        "in_ms": in_ms,
    }


def fmt(v, unit=""):
    if v is None:
        return "     -"
    if abs(v) >= 100:
        return f"{v:6.0f}{unit}"
    return f"{v:6.2f}{unit}"


def summarize(r):
    out = {}
    for f in FIELDS:
        vals = [float(w[f]) for w in r["win"] if f in w]
        out[f] = statistics.median(vals) if vals else None
        out[f + "_max"] = max(vals) if vals else None
    out["cadence"] = statistics.median(r["cadence"]) if r["cadence"] else None
    out["bus_mb"] = statistics.median(r["bus_mb"]) if r["bus_mb"] else None
    out["bus_gbs"] = statistics.median(r["bus_gbs"]) if r["bus_gbs"] else None
    out["in_ms"] = statistics.median(r["in_ms"]) if r["in_ms"] else None
    return out


def main(argv):
    skip = 0
    args = []
    i = 0
    while i < len(argv):
        if argv[i] == "--skip":
            skip = int(argv[i + 1])
            i += 2
            continue
        args.append(argv[i])
        i += 1
    if len(args) < 1:
        print(__doc__)
        return 2

    runs = [parse(p, skip) for p in args]
    sums = [summarize(r) for r in runs]

    label = [p.split("/")[-1] for p in args]
    print(f"windows/skipped: " + "  ".join(f"{l}={r['n']}" for l, r in zip(label, runs)))
    print()

    head = f"{'field':10s}" + "".join(f"{l[:16]:>18s}" for l in label)
    if len(args) > 1:
        head += f"{'delta':>10s}"
    print(head)
    print("-" * len(head))

    for f in FIELDS:
        row = f"{f:10s}"
        for s in sums:
            unit = "%" if f == "load" else ""
            row += f"{fmt(s[f], unit):>18s}"
        if len(sums) > 1 and sums[0][f] is not None and sums[1][f] is not None:
            d = sums[1][f] - sums[0][f]
            row += f"{d:+10.2f}"
        print(row)

    for key, unit in (("cadence", " vb"), ("bus_mb", " MB"), ("bus_gbs", " GB/s"),
                      ("in_ms", " ms")):
        row = f"{key:10s}"
        for s in sums:
            row += f"{fmt(s[key], unit):>18s}"
        if len(sums) > 1 and sums[0][key] is not None and sums[1][key] is not None:
            row += f"{sums[1][key] - sums[0][key]:+10.2f}"
        print(row)

    print()
    for l, s in zip(label, sums):
        worst = " ".join(f"{f}={s[f + '_max']:.0f}" for f in
                         ("gpu", "present", "total", "copy") if s[f + "_max"] is not None)
        print(f"  worst in {l}: {worst}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
