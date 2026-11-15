#!/usr/bin/env python3
"""Validate a module-index program (bridge.exe or the independent consumer).

bridge.exe is one translation unit whose entry .cpp lists modules/*.inc; the
consumer is a fakerFG-style unity build whose unity.cpp lists modules/*.cpp.
This check catches the failure modes a normal compiler will not report clearly:
a module listed in the index but missing on disk, a stray module that nobody
includes, an empty module, or a duplicate include.

Usage: python3 scripts/check_bridge_modules.py [src/bridge|src/xess_mfg_consumer]
Exit: 0 ok, 1 problem.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path


def find_index(root: Path) -> Path | None:
    for name in ("unity.cpp", "consumer.cpp", "bridge.cpp"):
        if (root / name).is_file():
            return root / name
    return None


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 else Path("src/bridge")
    index = find_index(root)
    moddir = root / "modules"
    if index is None:
        print(f"[modules] missing unity.cpp/consumer.cpp/bridge.cpp in {root}")
        return 1
    if not moddir.is_dir():
        print(f"[modules] missing {moddir}")
        return 1

    text = index.read_text(encoding="utf-8")
    listed = re.findall(r'#include\s+"modules/([^"]+\.(?:inc|cpp))"', text)
    on_disk = sorted(p.name for p in moddir.iterdir() if p.suffix in (".inc", ".cpp"))
    problems: list[str] = []

    if len(set(listed)) != len(listed):
        problems.append("module index contains a duplicate include")
    for name in listed:
        if name not in on_disk:
            problems.append(f"index includes missing module: {name}")
        else:
            body = (moddir / name).read_text(encoding="utf-8", errors="replace")
            if not body.strip():
                problems.append(f"module is empty: {name}")
            if not body.lstrip().startswith("// ===="):
                problems.append(f"module lacks the generated banner: {name}")
    for name in on_disk:
        if name not in listed:
            problems.append(f"module on disk is not included: {name}")

    # bridge uses NN_name.inc; the consumer uses semantic .cpp names. Only check the
    # numbering when every unit follows the bridge convention.
    if all(re.match(r"\d+_", n) for n in listed):
        prefixes = [int(m.group(1)) for n in listed if (m := re.match(r"(\d+)_", n))]
        if len(set(prefixes)) != len(prefixes):
            problems.append("duplicate numeric module prefix")

    if problems:
        for p in problems:
            print(f"[modules] ERROR: {p}")
        return 1
    print(f"[modules] {len(listed)} modules OK ({', '.join(listed)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
