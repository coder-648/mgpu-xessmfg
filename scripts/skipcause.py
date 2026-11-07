import re, sys

txt = open(sys.argv[1], encoding="utf-8", errors="replace").read()
rows = []
for m in re.finditer(r"breakdown: (.*)", txt):
    d = {}
    for k, v in re.findall(r"([a-zA-Z_]+)=(-?[\d.]+)(?![\d.])", m.group(1)):
        try:
            d.setdefault(k, float(v))
        except ValueError:
            pass
    if "skip" in d and "total" in d:
        rows.append(d)

sk = [r for r in rows if r["skip"] > 0]
print(f"skip>0 窗口 {len(sk)} / {len(rows)}")
print(f"  其中 age>=10ms（WGC/DWM 到达成串）: {sum(1 for r in sk if r.get('age', 0) >= 10)}")
print(f"  其中 total>31ms（真的贴近/超过预算）: {sum(1 for r in sk if r['total'] > 31)}")
print(f"  其中 gpu<12ms（GPU 明明有余量）: {sum(1 for r in sk if r.get('gpu', 0) < 12)}")
