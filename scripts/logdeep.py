import re, statistics, sys

path = sys.argv[1]
txt = open(path, encoding="utf-8", errors="replace").read()

rows = []
for m in re.finditer(r"breakdown: (.*)", txt):
    line = m.group(1)
    d = {}
    for k, v in re.findall(r"([a-zA-Z_]+)=(-?[\d.]+)(?![\d.])", line):
        try:
            d.setdefault(k, float(v))
        except ValueError:
            pass
    if all(k in d for k in ("gpu", "present", "total", "load")):
        rows.append(d)

budget = 1000.0 / 30.0
over = [r for r in rows if r["total"] > budget]
skipw = [r for r in rows if r.get("skip", 0) > 0]

print(f"windows={len(rows)}  budget={budget:.1f}ms")
print(f"total > budget : {len(over)} windows ({100*len(over)/max(len(rows),1):.0f}%)")
print(f"skip > 0       : {len(skipw)} windows ({100*len(skipw)/max(len(rows),1):.0f}%)")
print(f"头寸余量(中位) : {budget - statistics.median([r['total'] for r in rows]):.1f} ms")

print("\nskip>0 的窗口（total / gpu / present / load / skip）:")
for r in skipw:
    print(f"  total={r['total']:5.1f} gpu={r['gpu']:5.1f} pres={r['present']:5.1f} "
          f"load={r['load']:5.0f}% skip={r.get('skip',0):3.0f} age={r.get('age',0):5.1f}")

print("\ngpu 分位:", " ".join(f"p{q}={statistics.quantiles([r['gpu'] for r in rows], n=100)[q-1]:.1f}"
                              for q in (50, 90, 99)))
print("total 分位:", " ".join(f"p{q}={statistics.quantiles([r['total'] for r in rows], n=100)[q-1]:.1f}"
                              for q in (50, 90, 99)))
