#!/usr/bin/env python3
"""perf_windows.py - per-window report-line analysis for bridge.exe logs.

Unlike perf_ab.py (which averages the EMAs of every window, loading screens included),
this one reconstructs each window from the four diagnostic lines and separates
"in-world" windows from loading/hitch windows, because mixing them is how a perfectly
steady pipeline ends up looking like it drops 5% of its frames:

    arr(vb): 2x=40  n=40 ...              capture-side arrival intervals, in vblanks
    game(vb): 2x=40  n=40                 the producer's own present intervals, in vblanks
    out: presents=80 expected=80 vblanks=81   swap-chain presents vs elapsed refresh periods
    breakdown: ... load=45% slow=0 skip=0

A window is counted as a hitch/loading window when `load > 100` (the work did not fit the
input interval) or `skip > 2` (we threw away queued frames) - that is what a loading
screen looks like. Everything else is "steady".

Usage:
    python3 scripts/perf_windows.py perf_A.txt perf_B.txt ...
    python3 scripts/perf_windows.py --all perf_A.txt      # include hitch windows
"""
import re
import sys


def parse(path):
    wins = []
    cur = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"arr\(vb\):((?: \d+x=\d+)*)\s+n=(\d+)", line)
        if m:
            cur["arr"] = {int(k): int(v) for k, v in re.findall(r"(\d+)x=(\d+)", m.group(1))}
            cur["arr_n"] = int(m.group(2))
            continue
        m = re.search(r"game\(vb\):((?: \d+x=\d+)*)\s+n=(\d+)", line)
        if m:
            cur["game"] = {int(k): int(v) for k, v in re.findall(r"(\d+)x=(\d+)", m.group(1))}
            continue
        m = re.search(r"out: presents=(\d+)\s+expected=(\d+)\s+vblanks=(\d+)", line)
        if m:
            cur["pres"], cur["exp"], cur["vb"] = (int(m.group(i)) for i in (1, 2, 3))
            continue
        m = re.search(r"breakdown:.*?gpu=([\d.]+)ms.*?total=([\d.]+)ms.*?load=(\d+)%.*?"
                      r"slow=(\d+).*?skip=(\d+)", line)
        if m and "pres" in cur:
            cur["gpu"], cur["total"] = float(m.group(1)), float(m.group(2))
            cur["load"], cur["slow"], cur["skip"] = (int(m.group(i)) for i in (3, 4, 5))
            m2 = re.search(r"present=([\d.]+)ms", line)
            cur["present"] = float(m2.group(1)) if m2 else 0.0
            wins.append(cur)
            cur = {}
    return wins


def late(h):
    """Frames that arrived later than the nominal spacing of 2 vblanks."""
    return sum(v for k, v in h.items() if k > 2)


def stats(wins):
    if not wins:
        return None
    n = len(wins)
    arr_late = sum(late(w.get("arr", {})) for w in wins)
    arr_n = sum(w.get("arr_n", 0) for w in wins)
    game_late = sum(late(w.get("game", {})) for w in wins)
    game_n = sum(sum(w.get("game", {}).values()) for w in wins)
    empty = sum(max(0, w["vb"] - w["pres"]) for w in wins)
    vbs = sum(w["vb"] for w in wins)
    return {
        "n": n,
        "arr_late": arr_late / max(arr_n, 1) * 100,
        "game_late": game_late / max(game_n, 1) * 100,
        "empty": empty / max(vbs, 1) * 100,
        "empty_n": empty,
        "vb": vbs,
        "load": sum(w["load"] for w in wins) / n,
        "gpu": sum(w["gpu"] for w in wins) / n,
        "present": sum(w["present"] for w in wins) / n,
        "total": sum(w["total"] for w in wins) / n,
        "skip": sum(w["skip"] for w in wins),
    }


def main(argv):
    show_all = "--all" in argv
    files = [a for a in argv if not a.startswith("--")]
    if not files:
        print(__doc__)
        return 2

    print(f"{'file':22s}{'win':>5s}{'game>2vb':>10s}{'arr>2vb':>9s}"
          f"{'empty vb':>10s}{'load':>7s}{'gpu':>7s}{'present':>9s}{'skipped':>9s}")
    print("-" * 88)
    for path in files:
        wins = parse(path)
        steady = [w for w in wins if w["load"] <= 100 and w["skip"] <= 2]
        hitch = [w for w in wins if not (w["load"] <= 100 and w["skip"] <= 2)]
        for label, sel in (("steady", steady), ("hitch", hitch)) if show_all else (("steady", steady),):
            s = stats(sel)
            if not s:
                continue
            name = f"{path.split('/')[-1][:16]}" + ("" if label == "steady" else " [hitch]")
            print(f"{name:22s}{s['n']:5d}{s['game_late']:9.1f}%{s['arr_late']:8.1f}%"
                  f"{s['empty']:9.1f}%{s['load']:6.0f}%{s['gpu']:6.1f}{s['present']:8.1f}"
                  f"{s['skip']:9d}")
    print("\nsteady = load<=100% and skip<=2. `empty vb` = refresh periods with no new frame,")
    print("excluding window-boundary rounding it should be ~0 when the input is steady.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
