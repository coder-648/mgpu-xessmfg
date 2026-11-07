import sys
sys.path.insert(0, "scripts")
from pe import pe_sections, rva_to_off

FG = "third_party/xess/bin/libxess_fg.dll"
LL = "third_party/xess/bin/libxell.dll"

def dump(path, rva, n, label, expect):
    d, base, sections = pe_sections(path)
    off = rva_to_off(sections, rva)
    blob = d[off:off + n]
    hexs = " ".join(f"{b:02X}" for b in blob)
    ok = hexs.startswith(expect)
    print(f"{label:26s} RVA 0x{rva:06X}  {hexs}   {'MATCH' if ok else 'differs'}")
    if not ok:
        print(f"{'':26s} expected            {expect}")
    return ok

checks = [
    (FG, 0x1A45C2, 10, "fg_override_clamp",  "C7 87 6C 01 00 00 01 00 00 00"),
    (FG, 0x1A517D, 5,  "fg_default_ceiling", "BB 03 00 00 00"),
    (FG, 0x20973B, 5,  "fg_reported_maximum","B8 01 00 00 00"),
    (FG, 0x1A5DE4, 2,  "fg_keep_2x_model",   "74"),
    (FG, 0x21B86B, 10, "fg_live_count",      "83 F8 01 74 0F 83 FA 01 74 0A"),
    (LL, 0xD1C9,   5,  "ll_generated_gate",  "83 FB 03 76 07"),
]
good = 0
for path, rva, n, label, expect in checks:
    good += dump(path, rva, n, label, expect)
print(f"\n{good}/{len(checks)} sites match the reference layout")
