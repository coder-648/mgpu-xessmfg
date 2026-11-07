import re, statistics, sys

txt = open(sys.argv[1], encoding="utf-8", errors="replace").read()

comp = [float(m.group(1)) for m in re.finditer(r"compose->present ([\d.]+)ms", txt)]
phot = [float(m.group(1)) for m in re.finditer(r"compose->photons ([\d.]+)\.\.([\d.]+)ms", txt)]
click = [float(m.group(1)) for m in re.finditer(r"est\. click->photons >= ([\d.]+)ms", txt)]
age = [float(m.group(1)) for m in re.finditer(r"\(game->wgc ([\d.]+) \+ bridge", txt)]
brg = [float(m.group(1)) for m in re.finditer(r"\+ bridge ([\d.]+)\)", txt)]

def line(name, v, unit="ms"):
    if not v:
        print(f"  {name:22s} (none)")
        return
    b = sorted(v)
    print(f"  {name:22s} n={len(b):3d}  p50={statistics.median(b):6.1f}  mean={statistics.mean(b):6.1f}"
          f"  min={b[0]:6.1f}  max={b[-1]:6.1f} {unit}")

print(f"=== {sys.argv[1]} ===")
line("game->wgc", age)
line("bridge", brg)
line("compose->present", comp)
pairs = [(float(m.group(1)), float(m.group(2)))
         for m in re.finditer(r"compose->photons\s+([\d.]+)\.\.([\d.]+)ms", txt)]
line("compose->photons lo", [a for a, _ in pairs])
line("compose->photons hi", [b for _, b in pairs])
line("click->photons lower", click)
