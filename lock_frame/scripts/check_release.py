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
    "fakerFG.full.ini",
    "README.md",
    "RELEASE_NOTES.md",
    "collect_diagnostics.bat",
    "LICENSE",
    "NOTICE",
    "manifest.json",
    "SHA256SUMS.txt",
]

INI_REQUIRED = [
    "FgInput=DLSSG",
    "FgOutput=Capture",
    "StreamPercent=150",          # 非码流回退路径
    # v30 无损码流：默认两条流都走 codec
    "MvCodec=auto",
    "DepthCodec=auto",
    # 1.2.8 默认：MV 1:1 比特精确，depth 150%
    "CodecStreamPercent=100",
    "DepthCodecPercent=150",
    # 1.3.0/v34: UI composition is a supported default (patch + mode 4 only)
    "UiStream=0",
    "UiSource=Off",
    "UiSlots=2",
    "UiFullRes=1",
    "UiCodec=1",
    # 1.3.0/v34: stream 4 carries the support patch by default; stream 3 is sealed off.
    "HudlessStream=1",
    "UiPatch=1",
    "UiPatchWhiteNits=80",
    "UiAlpha8=0",
    "UiSparseCodec=1",
    # 诊断开关绝不能出现在发布包里
    "DepthFp16=0",
    "ProfGpu=0",
    "FrCodecAblate=0",
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

    # The test template ships in the same package; it must never turn a diagnostic on.
    with open(os.path.join(root, "fakerFG.full.ini"), "r", encoding="utf-8-sig",
              errors="replace") as f:
        full_ini = f.read()
    for key in ("FgInput=DLSSG", "UiStream=0", "UiSource=Off", "UiPatch=1",
                "HudlessStream=1", "DepthFp16=0", "ProfGpu=0", "FrCodecAblate=0"):
        if key not in full_ini:
            fail("fakerFG.full.ini missing " + key)

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
