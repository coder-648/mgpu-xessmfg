#!/usr/bin/env python3
"""cadence_report.py - 一次解析 consumer 日志里的动态逐帧关键指标。

用途：把一轮 A/B（固定基线 + carry hold 8/12/15）用同一口径汇总，避免人工
在各处 grep。只读日志，不改任何文件；只用标准库。

    python3 scripts/cadence_report.py <log> [<log> ...]

解析的字段（都来自 consumer 自己的输出）：
  show=        OSD/报告行里的显示帧率（交换链实际交付）
  game=        producer 报告的游戏真实帧间隔 -> 输入 fps
  xN=          XeFG 的渲染倍率（真实帧 -> 输出张数）
  cadence core k/m/out/reason/dwell/swloss/regime
  cadence hist k 分布 + switches/deferred/refused
  out: presents/expected/vblanks（输出有没有掉）
  switch cost（每次计数切换实测丢几张）
  reset-history 三类归因
  HITCH 次数与最大 gap

注意：`out: presents` 与 `expected` 的口径是"交换链交付张数"，与
`switch cost` 共用，所以 colour-queue 掉帧也会计入 short。脚本把
`colour-drop` 的累计数单独列出，便于区分"切换成本"与"输入帧被丢"。
"""
import re
import statistics
import sys


def nums(pattern, txt, cast=float, group=1):
    return [cast(m.group(group)) for m in re.finditer(pattern, txt)]


def kv(pattern, txt):
    """把每条匹配里的 key=value 抓成 dict（匹配本身作为文本，故 pattern 需带上下文）。"""
    return [dict(re.findall(r"(\w+)=(\S+)", m.group(0)))
            for m in re.finditer(pattern, txt)]



def pct(vals, share):
    if not vals:
        return 0
    return sum(1 for v in vals if v >= share) * 100.0 / len(vals)


def fmt(vals, fmt_s="{:.1f}"):
    if not vals:
        return "  -  "
    return fmt_s.format(statistics.mean(vals))


def report(path):
    txt = open(path, encoding="utf-8", errors="replace").read().replace("\r", "")

    show = [s for s in nums(r"show=(\d+)", txt, int) if s > 50]
    game = nums(r"game=([\d.]+)ms", txt)
    xn = nums(r"xN=([\d.]+)", txt)
    load = nums(r"load=([\d.]+)%", txt)
    skip = nums(r"skip=(\d+)", txt, int)

    core = kv(r"cadence core: (k=[^\n]*)", txt)
    k_hist = kv(r"cadence hist: ([^\n]*)", txt)

    out_rows = kv(r"out: presents=\d+\s+expected=\d+\s+vblanks=\d+", txt)
    switch_cost = re.findall(
        r"switch cost: (\d+) change\(s\), (\d+) output\(s\) short \(([\d.]+) per change\)", txt)


    hitch = re.findall(r"HITCH gap=([\d.]+)ms.*?xN=(\d+)", txt)
    resets = re.findall(r"reset-history: forced for frame seq=\d+ \(pair-gap (\d+), colour-drop (\d+), producer (\d+)", txt)
    capmsgs = len(re.findall(r"cadence core: cap-limited", txt))
    missing = len(re.findall(r"OUTPUT FRAMES MISSING", txt))

    name = path.split("/")[-1]
    print(f"\n=== {name}")
    print(f"  windows={len(out_rows)}  report40 lines={len(show)}  "
          f"input fps(mean)={fmt([1000.0 / g for g in game if g > 0])}  "
          f"game ms(mean)={fmt(game)}")

    if show:
        show_sorted = sorted(show)
        def q(f):
            return show_sorted[min(len(show_sorted) - 1, int(f * len(show_sorted)))]
        print(f"  show      mean={fmt(show, '{:.1f}')} p10={q(.10)} p50={q(.50)} p90={q(.90)} "
              f"min={min(show)} max={max(show)}  (<100: {100 - pct(show, 100):.0f}%"
              f"  <110: {100 - pct(show, 110):.0f}%)")
    if xn:
        print(f"  xN        mean={fmt(xn, '{:.2f}')} min={min(xn):.2f} max={max(xn):.2f}")
    if load:
        print(f"  load      mean={fmt(load, '{:.0f}')}% max={max(load):.0f}%")
    if skip:
        neg = sum(1 for v in skip if v > 0)
        print(f"  skip      windows with skip>0: {neg}/{len(skip)}")

    if core:
        outs = [float(re.sub(r"[^\d.\-]", "", c["out"])) for c in core if "out" in c]

        reasons = {}
        regimes = {}
        dwell = set()
        swloss = []
        for c in core:
            reasons[c.get("reason", "?")] = reasons.get(c.get("reason", "?"), 0) + 1
            regimes[c.get("regime", "?")] = regimes.get(c.get("regime", "?"), 0) + 1
            dwell.add(c.get("dwell", "?"))
            try:
                v = float(c.get("swloss", "-1"))
                if v >= 0:
                    swloss.append(v)
            except ValueError:
                pass
        print(f"  out       mean={fmt(outs)} Hz  min={min(outs):.1f} max={max(outs):.1f}")
        print(f"  regime    {regimes}   dwell={sorted(dwell)}   cap-limited msgs={capmsgs}")
        slots = [float(re.sub(r"[^\d.\-]", "", c["slots"])) for c in core if "slots" in c]
        tms = [float(re.sub(r"[^\d.\-]", "", c["T"])) for c in core if "T" in c]
        if slots:
            print(f"  core in   slots(mean)={fmt(slots, '{:.2f}')}  T(mean)={fmt(tms, '{:.2f}')}ms"
                  f"  -> f_in={fmt([1000.0 / t for t in tms if t > 0])} fps")
        # 节奏指标（比 show 更重要）：每帧"这批 k 张覆盖了几个 vblank"的误差。
        # 43 fps @120Hz = 2.8 vblank/帧，任何整数 k 都对不齐 → 这项就是肉眼卡顿。
        kk = [int(re.sub(r"[^\d]", "", c["k"])) for c in core if "k" in c]
        if slots and len(slots) == len(kk):
            mis = [abs(a - b) for a, b in zip(kk, slots)]
            sp = [abs(s / a - 1.0) for s, a in zip(slots, kk) if a > 0]
            bad = sum(1 for x in mis if x > 0.15)
            print(f"  rhythm    |k-slots| mean={fmt(mis, '{:.2f}')}  >0.15vb: "
                  f"{100.0 * bad / len(mis):.0f}% of frames   spacing err mean="
                  f"{fmt([100 * x for x in sp], '{:.1f}')}% max={100 * max(sp):.0f}%")


        top = sorted(reasons.items(), key=lambda kv_: -kv_[1])[:5]
        print(f"  reason    {top}")
        if swloss:
            print(f"  swloss    mean={fmt(swloss, '{:.2f}')} max={max(swloss):.2f} (n={len(swloss)})")

    if k_hist:
        kdist = {}
        sw = de = rf = 0
        for h in k_hist:
            for k, v in h.items():
                if re.fullmatch(r"\dx", k):
                    kdist[k] = kdist.get(k, 0) + int(float(v))
            sw += int(float(h.get("switches", 0)))
            de += int(float(h.get("deferred", 0)))
            rf += int(float(h.get("refused", 0)))
        print(f"  k dist    {dict(sorted(kdist.items()))}")
        per = (sw / len(k_hist)) if k_hist else 0.0
        print(f"  switches  total={sw}  per window={per:.2f}  deferred={de} refused={rf}")

    if out_rows:
        pres = [int(r["presents"]) for r in out_rows]
        exp = [int(float(r["expected"])) for r in out_rows]
        vb = [int(r["vblanks"]) for r in out_rows]
        short = [e - p for p, e in zip(pres, exp) if e > p + 1]
        ratio = (sum(pres) / sum(exp) * 100.0) if sum(exp) else 0.0
        per_vblank = (sum(pres) / sum(vb) * 100.0) if sum(vb) else 0.0
        print(f"  out:      presents/expected={ratio:.1f}%  presents/vblanks={per_vblank:.1f}%"
              f"  short windows={len(short)}/{len(out_rows)} missing-flag={missing}")
        if short:
            print(f"            short sum={sum(short)}  per window={sum(short) / len(out_rows):.2f}"
              f"  worst={max(short)}")


    if switch_cost:
        per_change = [float(r[2]) for r in switch_cost]
        print(f"  switch cost lines={len(switch_cost)}  per-change mean={fmt(per_change, '{:.2f}')}"
              f"  max={max(per_change):.2f}")

    # colour stall guard（2026-11-15 新增）：每窗口跳过的拷贝数
    cskip = nums(r"cskip=(\d+)", txt, int)
    if cskip:
        print(f"  cskip     total={sum(cskip)}  windows with >0: {sum(1 for v in cskip if v)}"
              f"  max/window={max(cskip)}")
    stall_ev = len(re.findall(r"colour: .*reusing the last frame", txt))
    if stall_ev:
        print(f"  stall ev  {stall_ev} colour-stall skip(s) logged")

    # 显示端相位（2026-11-15 新增：DXGI output 校准的真实 vblank）
    vb = re.findall(r"vblank: period=([\d.]+)ms phase mean=([-\d.]+) std=([\d.]+)"
                    r" min=([-\d.]+) max=([-\d.]+) ms", txt)
    if vb:
        per = [float(r[0]) for r in vb]
        pm = [float(r[1]) for r in vb]
        ps = [float(r[2]) for r in vb]
        mx = [float(r[4]) for r in vb]
        print(f"  vphase    period={per[0]:.4f}ms  mean={fmt(pm, '{:.2f}')}ms"
              f"  std(win)={fmt(ps, '{:.2f}')}ms  worst std={max(ps):.2f}ms"
              f"  max phase={max(mx):.2f}ms")




    if resets:
        pg, cd, pr = (int(resets[-1][0]), int(resets[-1][1]), int(resets[-1][2]))
        print(f"  resets    events={len(resets)}  final: pair-gap={pg} colour-drop={cd} producer={pr}")

    if hitch:
        gaps = [float(g) for g, _ in hitch]
        small = sum(1 for _, x in hitch if x == "1")
        print(f"  HITCH     count={len(hitch)} (xN=1: {small})  max gap={max(gaps):.0f}ms  mean={fmt(gaps, '{:.0f}')}ms")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    for p in sys.argv[1:]:
        report(p)
