#!/usr/bin/env python3
"""Cross-check what the add-on published against what the bridge actually consumed.

    python3 verify_pipeline.py <game_capture.log> <bridge_log>

The two processes are the two halves of the pipeline: the add-on turns the game's FG
declaration into FrameShared v14, the bridge turns FrameShared into an XeFG call. A field
can be captured perfectly and still be dropped on the way, so this compares them field by
field and prints PASS/FAIL instead of leaving it to eyeballing.

Exit code 0 = every field that both sides report agrees.
"""
import re
import sys

FIELDS = [
    ("api", "producer api id", "bridge api name"),
    ("flags", "input_flags", "producer flags"),
    ("render", "render size", "render size"),
    ("output", "output size", "output size"),
    ("mv_unit", "mv unit", "mv unit (declared)"),
    ("depth", "depth direction", "initFlags inverted_depth"),
    ("mv_scale", "mv scale", "mv scale"),
    ("jitter", "jitter (last published frame)", "jitter (frame constants)"),
    ("stream", "stream size", "MV stream size"),
    ("pairing", "frame token", "frame-key pairing"),
]

API_MAP = {"sl": "sl/streamline", "ngx": "ngx/dlss", "ffx": "ffx", "xess": "xess",
           "xefg": "xefg/xess-fg", "ffxfg": "ffx/fsr-fg", "depth": "reshade-depth"}


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def last(pat, lines, group=1):
    out = None
    for line in lines:
        m = re.search(pat, line)
        if m:
            if group == 0:
                out = m.group(0)
            else:
                g = m.groups()
                out = g[0] if len(g) == 1 else g
    return out


def all_of(pat, lines):
    """Every match of `pat` across the whole log -> list of tuples (or single values)."""
    out = []
    for line in lines:
        m = re.search(pat, line)
        if m:
            g = m.groups()
            out.append(g[0] if len(g) == 1 else g)
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    addon, bridge = read(sys.argv[1]), read(sys.argv[2])

    # The add-on log spans many sessions and live config changes, so each field is a SET of
    # values it ever published; the bridge's single value has to be in that set. Equality
    # against "the last line" produced false failures (PRAGMATA: 2228x1254 vs an older
    # 2560x1440 bridge run).
    a_apis = set(all_of(r"shared: producer .*api=(\w+)", addon))
    a_flags = {f.upper() for f in all_of(r"shared: producer .*flags=0x([0-9A-Fa-f]+)", addon)}
    a_render = set(all_of(r"shared: producer .*render=(\d+x\d+)", addon))
    a_output = set(all_of(r"shared: producer .*output=(\d+x\d+)", addon))
    a_units = all_of(r"semantics: mv=(\w+)", addon)
    a_depth = all_of(r"semantics: .*depth=(\w+)", addon)   # ordered: last = current
    a_scales = set(all_of(r"-- frame \d+ .*mvscale=\(([-\d.]+, [-\d.]+)\)", addon))
    a_mvat = set(all_of(r"shared: mv at (\d+x\d+)", addon))
    a_token = set(all_of(r"harvest: .*token=(\w)", addon))

    b_prod = last(r"shared: producer .*api=([\w/.-]+) flags=0x([0-9A-Fa-f]+)"
                  r" render=(\d+x\d+) output=(\d+x\d+)", bridge)
    b_sem = last(r"producer semantics: mv=(\w+)\((\w+)\).*?depth=(\w+)\((\w+)\)", bridge)
    b_fc = last(r"frame constants:.*?mvscale=\(([-\d.]+), ([-\d.]+)\)", bridge)
    b_init = last(r"initFlags=0x([0-9A-Fa-f]+) \(inverted_depth=(\d)", bridge)
    b_mv = last(r"MV (\d+x\d+), depth", bridge)
    b_pair = last(r"frame-key pair:", bridge, group=0)

    rows = []

    def add(name, a, b, ok, note=""):
        rows.append((name, a, b, "PASS" if ok else "FAIL", note))

    if b_prod:
        api, flags, render, output = b_prod
        add("api", "/".join(sorted(a_apis)) or "-", api,
            any(API_MAP.get(x, x) == api for x in a_apis), "bridge name maps to a published api")
        add("flags", " ".join("0x" + f for f in sorted(a_flags)) or "-", "0x" + flags.upper(),
            flags.upper() in a_flags, "the bridge's flags were published at some point")
        add("render", " ".join(sorted(a_render)) or "-", render, render in a_render)
        add("output", " ".join(sorted(a_output)) or "-", output, output in a_output)
    else:
        add("api", "-", "-", False, "bridge log has no producer line")

    if b_sem:
        add("mv_unit", " ".join(sorted(a_units)) or "-", b_sem[0], b_sem[0] in a_units)
        # The bridge logs the producer's own declaration: "depth=<forward|reversed>(declared)".
        # That is the value to compare against, and only the LAST published one counts.
        b_depth = b_sem[2] if len(b_sem) > 2 else "?"
        a_last = a_depth[-1] if a_depth else "?"
        if b_depth in ("forward", "reversed"):
            add("depth", a_last, b_depth, a_last == b_depth,
                "producer and consumer agree on the depth direction")
        else:
            want_inv = (a_last == "reversed")
            have_inv = bool(b_init) and b_init[1] == "1"
            add("depth", a_last, ("inverted_depth=" + b_init[1]) if b_init else "-",
                want_inv == have_inv, "init flag follows the published direction")
    else:
        add("mv_unit", "-", "-", False, "bridge log has no semantics line")

    if b_fc:
        # Numeric compare with a small tolerance: the add-on prints the *native* tag extent
        # (Wuchang 2558x1439) while the stream it publishes is cropped to an even height
        # (2558x1438), so a one-row difference is a rounding of the same declaration, not a
        # mismatch. Anything beyond 0.5% is a real disagreement.
        want = (float(b_fc[0]), float(b_fc[1]))
        best, best_d = None, None
        for s in a_scales:
            try:
                x, y = s.split(",")
                cand = (float(x), float(y))
            except ValueError:
                continue
            d = max(abs(cand[0] - want[0]) / max(abs(want[0]), 1e-6),
                    abs(cand[1] - want[1]) / max(abs(want[1]), 1e-6))
            if best_d is None or d < best_d:
                best, best_d = cand, d
        ok = best_d is not None and best_d < 0.005
        note = ""
        if best is not None and best_d:
            dx = abs(best[0] - want[0]) / max(abs(want[0]), 1e-6) * 100.0
            dy = abs(best[1] - want[1]) / max(abs(want[1]), 1e-6) * 100.0
            note = "closest %.4f,%.4f (%.3f%%, %.3f%%)" % (best[0], best[1], dx, dy)
        add("mv_scale", " ".join(sorted(a_scales)) or "-",
            "(%s, %s)" % (b_fc[0], b_fc[1]), ok, note)
    else:
        add("mv_scale", "-", "-", False, "bridge log has no frame constants line")

    add("stream", " ".join(sorted(a_mvat)) or "-", b_mv or "-", bool(b_mv) and b_mv in a_mvat)
    add("pairing", " ".join("token=" + t for t in sorted(a_token)) or "-",
        "frame-key pair" if b_pair else "none",
        ("y" in a_token) == bool(b_pair), "pairing happens iff a token was published")


    width = max(len(r[0]) for r in rows) + 1
    print("%-*s %-22s %-22s %s" % (width, "field", "add-on (published)", "bridge (consumed)",
                                   "verdict"))
    print("-" * (width + 22 + 22 + 30))
    bad = 0
    for name, a, b, verdict, note in rows:
        if verdict == "FAIL":
            bad += 1
        print("%-*s %-22s %-22s %-5s %s" % (width, name, str(a)[:22], str(b)[:22], verdict, note))
    print()
    print("%d field(s) checked, %d FAIL" % (len(rows), bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
