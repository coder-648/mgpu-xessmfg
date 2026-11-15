#!/usr/bin/env python3
"""pair_report.py - 配对/命中率日志分析（mgpu consumer）

用法:
    python3 scripts/pair_report.py <consumer.log> [more.log ...]
    python3 scripts/pair_report.py --strict <log>     # 0 次帧内拷贝才算通过

解析本次审计新增的 `pair:` 行和原有的 `pf:` 行，给出：
  * exact / ms_fail / no_slot / ambiguous / unreadable / stale / lost / mismatch
  * 命中率 = served / (served + missed + copy-in-flight)
  * 颜色滞后 lag(avg/max) 与槽位容量（max 必须 < slots-2）
  * 100% 判定（strict: in-frame copy == 0 且 mismatch == 0 且 lost == 0 且 overflow == 0）

退出码：0 = 通过，1 = 未通过/无法判定。
"""
import argparse
import re
import sys

PAIR_RE = re.compile(
    r"pair:\s*exact=(\d+)\s+ms_fail=(\d+)\s+no_slot=(\d+)\s+ambiguous=(\d+)"
    r"\s+unreadable=(\d+)\s+stale=(\d+)\s+lost\(1X\)=(\d+)\s+mismatch=(\d+)"
    r"\s+lag\(avg=([\d.]+)\s+max=(\d+)\s+n=(\d+),\s*slots=(\d+)\)")
PF_RE = re.compile(
    r"pf:\s*(\d+)\s+served,\s*(\d+)\s+missed,\s*(\d+)\s+copy-still-in-flight"
    r"\s*\(copied in-frame\),\s*(\d+)\s+dropped by overflow")
RESET_RE = re.compile(r"reset-history: forced")
SLOW_RE = re.compile(r"SLOW frame:")
HITCH_RE = re.compile(r"HITCH gap=")


def add(acc, key, val):
    acc[key] = acc.get(key, 0) + val


def parse(paths):
    acc = {}
    per_file = []
    for path in paths:
        cur = dict(pair=0, exact=0, ms_fail=0, no_slot=0, ambiguous=0, unreadable=0,
                   stale=0, lost=0, mismatch=0, lag_max=0, lag_n=0, lag_sum=0.0,
                   served=0, missed=0, inflight=0, overflow=0, resets=0,
                   slow=0, hitch=0, slots=0, lines=0)
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                cur["lines"] += 1
                m = PAIR_RE.search(line)
                if m:
                    cur["pair"] += 1
                    cur["exact"] += int(m.group(1))
                    cur["ms_fail"] += int(m.group(2))
                    cur["no_slot"] += int(m.group(3))
                    cur["ambiguous"] += int(m.group(4))
                    cur["unreadable"] += int(m.group(5))
                    cur["stale"] += int(m.group(6))
                    cur["lost"] += int(m.group(7))
                    cur["mismatch"] += int(m.group(8))
                    cur["lag_sum"] += float(m.group(9)) * int(m.group(11))
                    cur["lag_max"] = max(cur["lag_max"], int(m.group(10)))
                    cur["lag_n"] += int(m.group(11))
                    cur["slots"] = int(m.group(12))
                    continue
                m = PF_RE.search(line)
                if m:
                    cur["served"] += int(m.group(1))
                    cur["missed"] += int(m.group(2))
                    cur["inflight"] += int(m.group(3))
                    cur["overflow"] += int(m.group(4))
                    continue
                if RESET_RE.search(line):
                    cur["resets"] += 1
                if SLOW_RE.search(line):
                    cur["slow"] += 1
                if HITCH_RE.search(line):
                    cur["hitch"] += 1
        per_file.append((path, cur))
        for k, v in cur.items():
            add(acc, k, v)
    return acc, per_file


def pct(a, b):
    return (100.0 * a / b) if b else 0.0

def report(paths, strict):
    acc, per_file = parse(paths)
    print("=== pair_report ===")
    for path, c in per_file:
        pairs = c["exact"] + c["lost"] + c["ms_fail"] + c["no_slot"] + c["ambiguous"] + c["unreadable"]
        hit = c["served"] + c["missed"] + c["inflight"]
        print(f"\n-- {path}")
        print(f"   pair windows={c['pair']}  slots={c['slots'] or '?'}")
        print(f"   exact={c['exact']}  lost(1X)={c['lost']}  mismatch={c['mismatch']}")
        print(f"   ms_fail={c['ms_fail']} no_slot={c['no_slot']} ambiguous={c['ambiguous']}"
              f" unreadable={c['unreadable']} stale={c['stale']}")
        print(f"   exact rate={pct(c['exact'], pairs):.2f}%  lost rate={pct(c['lost'], pairs):.2f}%")
        if hit:
            print(f"   cache hit={pct(c['served'], hit):.2f}%  in-frame={pct(c['missed'] + c['inflight'], hit):.2f}%"
                  f"  overflow={c['overflow']}")
        if c["lag_n"]:
            print(f"   colour lag: avg={c['lag_sum'] / c['lag_n']:.2f} max={c['lag_max']}"
                  f" publications (n={c['lag_n']})")
        print(f"   resets={c['resets']} SLOW={c['slow']} HITCH={c['hitch']}")

    pairs = acc.get("exact", 0) + acc.get("lost", 0) + acc.get("ms_fail", 0) + \
        acc.get("no_slot", 0) + acc.get("ambiguous", 0) + acc.get("unreadable", 0)
    hit = acc.get("served", 0) + acc.get("missed", 0) + acc.get("inflight", 0)

    print("\n=== totals ===")
    if pairs == 0:
        print("no `pair:` lines found - this log predates the exact-pair build")
        return 1
    print(f"exact={acc['exact']} lost={acc['lost']} mismatch={acc['mismatch']}"
          f" overflow={acc['overflow']} in-frame={acc['missed'] + acc['inflight']}")
    print(f"exact rate={pct(acc['exact'], pairs):.2f}%  lost rate={pct(acc['lost'], pairs):.2f}%")
    if hit:
        print(f"cache hit={pct(acc['served'], hit):.2f}%")

    checks = []
    checks.append(("mismatch == 0 (never tagged the wrong publication)",
                   acc["mismatch"] == 0))
    checks.append(("ms_fail == 0 (no same-seq/different-time publication)",
                   acc["ms_fail"] == 0))
    checks.append(("ambiguous == 0 (seq12 wrap never collided)",
                   acc["ambiguous"] == 0))
    if acc.get("lag_n", 0) and acc.get("slots", 0):
        checks.append((f"lag max {acc['lag_max']} < slots-2 ({acc['slots'] - 2})",
                       acc["lag_max"] < max(1, acc["slots"] - 2)))
    if strict:
        checks.append(("in-frame copy == 0 (strict 100%)",
                       acc["missed"] + acc["inflight"] == 0))
        checks.append(("lost == 0 (strict 100%)", acc["lost"] == 0))
        checks.append(("cache overflow == 0", acc["overflow"] == 0))
    else:
        checks.append(("lost <= 0.5% of colour frames", pct(acc["lost"], pairs) <= 0.5))
        checks.append(("cache overflow == 0", acc["overflow"] == 0))

    print("\n=== verdict ===")
    failed = 0
    for name, ok in checks:
        print(("  PASS  " if ok else "  FAIL  ") + name)
        if not ok:
            failed += 1
    print("\nRESULT:", "PASS" if failed == 0 else f"FAIL ({failed} check(s))")
    return 0 if failed == 0 else 1


def main():
    ap = argparse.ArgumentParser(description="pair/hit-rate analysis for mgpu consumer logs")
    ap.add_argument("--strict", action="store_true",
                    help="require 0 in-frame copies / 0 lost / 0 overflow")
    ap.add_argument("logs", nargs="+")
    a = ap.parse_args()
    return report(a.logs, a.strict)


if __name__ == "__main__":
    sys.exit(main())
