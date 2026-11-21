#!/usr/bin/env python3
"""Aggregate the per-frame ratios printed by fr_codec_test into p5/p50/p95."""
import re
import sys

pat = re.compile(r'^(\S+)_(colour|depth|mv)\.raw\s+\S+\s+\d+x\d+\s+nw=\d+\s+'
                 r'([\d.]+) MB ->\s+([\d.]+) MB\s+([\d.]+)x')
raw = {}
miss = 0
for line in open(sys.argv[1] if len(sys.argv) > 1 else '/tmp/gate1_run.txt'):
    m = pat.match(line.strip())
    if not m:
        if 'MISMATCH' in line:
            miss += 1
        continue
    kind = m.group(2)
    raw.setdefault(kind, []).append((float(m.group(3)), float(m.group(4)), float(m.group(5))))

def pct(v, p):
    s = sorted(v)
    i = min(len(s) - 1, max(0, int(round((p / 100.0) * (len(s) - 1)))))
    return s[i]

print("%-7s %5s %10s %10s %10s" % ("stream", "n", "raw MB", "out MB", "ratio"))
for kind in ("mv", "depth", "colour"):
    rows = raw.get(kind, [])
    if not rows:
        continue
    r = [x[2] for x in rows]
    print("%-7s %5d %10.1f %10.1f  p5=%.3f p50=%.3f p95=%.3f min=%.3f max=%.3f"
          % (kind, len(rows), rows[0][0], rows[0][1], pct(r, 5), pct(r, 50), pct(r, 95),
             min(r), max(r)))
if miss:
    print("MISMATCHED frames:", miss)
