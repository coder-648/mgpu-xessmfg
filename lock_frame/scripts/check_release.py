#!/usr/bin/env python3
"""Validate a lock_frame release directory before it is published.

Checks required files, forbidden binaries/debug files, fakerFG.ini defaults,
manifest version/protocol and every SHA256SUMS entry.
"""
import argparse
import hashlib
import json
import os
import sys


REQUIRED = [
    "lock_frame.exe",
    "lock_frame_ui.exe",
    "fakerFG.dll",
    "xess_mfg_producer.dll",
    "fakerFG.ini",
    "README.md",
    "README.txt",
    "RELEASE_NOTES.md",
    "deploy_game.bat",
    "LICENSE",
    "NOTICE",
    "manifest.json",
    "SHA256SUMS.txt",
]

INI_REQUIRED = [
    "FgInput=DLSSG",
    "FgOutput=Capture",
    "ColourStream=0",
    "StreamPercent=150",
]

FORBIDDEN_SUFFIX = (".pdb", ".map", ".log", ".ilk", ".obj")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def fail(msg):
    print("[check] FAIL: " + msg, file=sys.stderr)
    raise SystemExit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--protocol", required=True)
    ap.add_argument("--version", required=True)
    args = ap.parse_args()
    root = os.path.abspath(args.root)

    for name in REQUIRED:
        if not os.path.isfile(os.path.join(root, name)):
            fail("missing " + name)

    has_intel = os.path.isfile(os.path.join(root, "INTEL-XESS-LICENSE.txt"))
    files = []
    for base, dirs, names in os.walk(root):
        dirs.sort()
        for name in sorted(names):
            rel = os.path.relpath(os.path.join(base, name), root).replace(os.sep, "/")
            files.append(rel)
            if name.lower().endswith(FORBIDDEN_SUFFIX):
                fail("debug/runtime file in package: " + rel)
            if name.lower() in ("libxess_fg.dll", "libxell.dll") and not has_intel:
                fail("Intel runtime without INTEL-XESS-LICENSE.txt: " + rel)

    with open(os.path.join(root, "fakerFG.ini"), "r", encoding="utf-8-sig",
              errors="replace") as f:
        ini = f.read()
    for key in INI_REQUIRED:
        if key not in ini:
            fail("fakerFG.ini missing " + key)

    with open(os.path.join(root, "manifest.json"), "r", encoding="utf-8") as f:
        manifest = json.load(f)
    if str(manifest.get("version")) != args.version:
        fail("manifest version %r != %r" % (manifest.get("version"), args.version))
    if str(manifest.get("shared_protocol")) != args.protocol:
        fail("manifest protocol %r != %r" %
             (manifest.get("shared_protocol"), args.protocol))

    sums_path = os.path.join(root, "SHA256SUMS.txt")
    with open(sums_path, "r", encoding="utf-8") as f:
        lines = [ln.strip() for ln in f if ln.strip()]
    if not lines:
        fail("SHA256SUMS.txt is empty")
    for line in lines:
        parts = line.split(None, 1)
        if len(parts) != 2:
            fail("bad SHA256SUMS line: " + line)
        want, rel = parts[0], parts[1].lstrip("*").replace("\\", "/")
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            fail("checksum entry has no file: " + rel)
        got = sha256(path)
        if got != want:
            fail("checksum mismatch: " + rel)

    print("[check] OK: %d files, version %s, protocol v%s" %
          (len(files), args.version, args.protocol))


if __name__ == "__main__":
    main()
