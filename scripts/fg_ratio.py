#!/usr/bin/env python3
"""Is anything generating frames right now?  (add-on log only, no bridge needed)

    python3 fg_ratio.py <game_capture.log> [seconds]

Two independent counters live in the add-on log:

  render  = sl.const #N            one increment per *rendered* frame (the game sets
                                   Streamline constants once per render)
  present = perf: frames=N         one increment per *presented* frame (ReShade's
                                   present callback)

so the ratio is the answer to "is a frame generator running":

  present/render ~ 1  -> nobody is generating frames (what you see is the render rate)
  present/render ~ 2  -> 2x FG somewhere (game, mod, or us)
  present/render ~ 3  -> 3x FG

It also prints the last DLSS-G options the game asked for, and whether our hooks
rewrote them.

Caveat: this measures the GAME SIDE only. Our bridge interpolates onto its own
overlay window, so its extra frames do not change this ratio - that is exactly
what makes the ratio a clean test for "is the game (or a mod inside it) already
generating frames that WGC would then hand to us".

SECOND caveat (measured 2026-11-10 on PRAGMATA): when the title drives DLSS-G
through Streamline, `slSetConstants` may be called once per *presented* frame
(generated frames included), so `sl.const #N` and `perf: frames=N` scale together
and the ratio stays ~1.0 even with frame generation on. Use it as a *lower bound*
detector (a ratio of 2/3 is conclusive; a ratio of 1 is not). The reliable
cross-check is an external present-rate counter (RTSS) with the game's FG toggled:
PRAGMATA measured 50 fps with FG off and 77 fps with FG on (x1.54 - the FG pass
costs render time, so it is never a clean 2x).
"""
import re
import sys

TS = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]\s*(.*)$")


def secs(h, m, s, ms):
    return int(h) * 3600 + int(m) * 60 + int(s) + int(ms) / 1000.0


def parse_const(line):
    m = re.search(r"sl\.const #(\d+)", line)
    return int(m.group(1)) if m else None


def parse_frames(line):
    m = re.search(r"perf: frames=(\d+)", line)
    return int(m.group(1)) if m else None


def main():
    path = sys.argv[1]
    window = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0

    consts = []      # (t, n)
    presents = []    # (t, n)
    options = []     # (t, text)
    rewards = []     # (t, text)

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = TS.match(line.rstrip("\n"))
            if not m:
                continue
            t = secs(*m.group(1, 2, 3, 4))
            body = m.group(5)
            n = parse_const(body)
            if n is not None:
                consts.append((t, n))
                continue
            n = parse_frames(body)
            if n is not None:
                presents.append((t, n))
                continue
            if "dlssg.options" in body:
                options.append((t, body.split(")", 0)[0][:110]))
            if "SlFgOff=1 - forcing" in body or "Claim=1 - " in body:
                rewards.append((t, body[:110]))

    if not consts or not presents:
        print("need both sl.const and perf: frames= lines "
              "(is Log=1 and FgSniff=1?)")
        return

    end = max(consts[-1][0], presents[-1][0])
    start = end - window

    def rate(seq):
        win = [x for x in seq if x[0] >= start]
        if len(win) < 2:
            return None
        dt = win[-1][0] - win[0][0]
        dn = win[-1][1] - win[0][1]
        return dn / dt if dt > 0 else None

    r = rate(consts)
    p = rate(presents)
    print("window            %.0f s (last)" % window)
    print("render  (sl.const) %s /s" % ("%.1f" % r if r else "n/a"))
    print("present (perf)     %s /s" % ("%.1f" % p if p else "n/a"))
    if r and p:
        print("present/render     %.2f   %s"
              % (p / r, "-> no frame generator" if 0.8 < p / r < 1.25 else
                 "-> something IS generating frames"))
    if options:
        print("last dlssg.options %s" % options[-1][1])
    if rewards:
        print("last rewrite       %s" % rewards[-1][1])


if __name__ == "__main__":
    main()
