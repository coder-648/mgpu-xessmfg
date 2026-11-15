#!/usr/bin/env python3
"""CP2077 regression check for fakerFG.

Reads the last session of Cyberpunk2077_fakerFG.log (everything after the last
"dxgi: real module" line) and prints PASS/FAIL for the things DLSS + XeFG must do,
plus the parts that must NOT happen after the Wuchang work (the DLSS-G backbuffer
copy, which is off in this game's ini).

Usage: check_fakerfg_regression.py [path-to-log]
"""
import re
import sys

DEFAULT = ("/mnt/e/SteamLibrary/steamapps/common/Cyberpunk 2077/bin/x64/"
           "Cyberpunk2077_fakerFG.log")

PATH = sys.argv[1] if len(sys.argv) > 1 else DEFAULT

with open(PATH, "r", encoding="utf-8", errors="replace") as fh:
    lines = fh.read().splitlines()

starts = [i for i, ln in enumerate(lines) if "dxgi: real module" in ln]
if starts:
    lines = lines[starts[-1]:]

text = "\n".join(lines)
denied = text.count("xefg[error]")

def has(pattern):
    return re.search(pattern, text) is not None

def count(pattern):
    return len(re.findall(pattern, text))

checks = [
    ("swap chain taken over", has(r"xefg: the swap chain is ours now")),
    ("no second real chain handed out", count(r"dxgi: could not take the swap chain") <= 1),
    ("XeFG activated", has(r"xefg: SetEnabled: true")),
    ("generating frames (2X + fgEnabled=1)", has(r"2X present status[^\n]*fgEnabled=1")),
    ("a present was OK with 2 frames", has(r"present status framesPresented=2[^\n]*result=OK")),
    ("no DLSS-G backbuffer copy (BlitDlssgBackbuffer off)", count(r"\nblit: #") == 0),
    ("no 'motion vector and depth ... must match'", count(r"resolutions must match") == 0),
    ("no ResizeBuffers fatal", count(r"ResizeBuffers.*failed") == 0),
    # v2 seam: the capture has something to copy. Link mode (FakeDlssgFeature=0) publishes
    # through publish_upscaler_record(); provider mode through provider_evaluate().
    ("a frame record was published (record: / provider: eval)",
     has(r"record: upscaler lane|provider: eval #1")),
]

fps = re.findall(r"present: \d+ frames avg=([0-9.]+) min=([0-9.]+) max=([0-9.]+)", text)
avg = f"{fps[-1][0]} (min {fps[-1][1]}, max {fps[-1][2]})" if fps else "?"

print(f"log: {PATH}")
print(f"lines in last session: {len(lines)}, xefg[error] lines: {denied}")
print(f"last present average: {avg} ms")
# Informational (only when the capture add-on is deployed): the v2 sink handshake.
print(f"sink: {'subscribed' if has(r'api: capture sink subscribed') else 'not subscribed'}")
print()
ok = True
for name, good in checks:
    print(f"  [{'PASS' if good else 'FAIL'}] {name}")
    ok = ok and good

print()
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
