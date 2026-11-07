import re, statistics, sys

def analyze(path, label):
    txt = open(path, encoding="utf-8", errors="replace").read()
    rows = []
    for m in re.finditer(r"breakdown: (.*)", txt):
        line = m.group(1)
        d = dict(re.findall(r"([a-zA-Z_]+)=([-\d.]+)", line))
        if "gpu" in d and "present" in d and "total" in d:
            rows.append(d)
    fr = [dict(re.findall(r"([a-zA-Z_]+)=([-\d.]+)", m.group(1)))
          for m in re.finditer(r"frame \d+ .*\((.*?)\)", txt)]
    showed = [int(x) for x in re.findall(r"show=(\d+)", txt)]
    print(f"=== {label}  ({len(rows)} windows, {len(showed)} show samples)")
    def stat(key, mult=1.0, fmt="{:.1f}"):
        vals = [float(r[key]) * mult for r in rows if key in r]
        if not vals: return
        print(f"  {key:10s} med={fmt.format(statistics.median(vals)):>7s} "
              f"mean={fmt.format(statistics.mean(vals)):>7s} "
              f"min={fmt.format(min(vals)):>7s} max={fmt.format(max(vals)):>7s}")
    stat("gpu"); stat("present"); stat("total"); stat("load"); stat("age"); stat("skip")
    if showed:
        body = [s for s in showed if s > 50]
        print(f"  show       med={statistics.median(body):>7.0f} mean={statistics.mean(body):>7.0f} "
              f"min={min(body):>7d} max={max(body):>7d}")
    rec = [float(r["recent"]) for r in fr if "recent" in r]
    if rec:
        body = [x for x in rec if x > 1]
        print(f"  recent     med={statistics.median(body):>7.1f} mean={statistics.mean(body):>7.1f} "
              f"min={min(body):>7.1f} max={max(body):>7.1f}")

if __name__ == "__main__":
    for path, label in [(sys.argv[1], "baseline 4x"), (sys.argv[2], "tuned 4x")] if len(sys.argv) > 2 else [(sys.argv[1], "run")]:
        analyze(path, label)
