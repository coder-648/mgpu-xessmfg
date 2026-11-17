#!/usr/bin/env python3
"""Write dist/lock_frame-<ver>/manifest.json.

Used by lock_frame/release.sh. No third-party imports.
"""
import argparse
import datetime
import hashlib
import json
import os


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--protocol", required=True)
    ap.add_argument("--git", default="nogit")
    ap.add_argument("--toolchain", default="")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    files = []
    for base, dirs, names in os.walk(root):
        dirs.sort()
        for name in sorted(names):
            if name in ("manifest.json", "SHA256SUMS.txt"):
                continue
            path = os.path.join(base, name)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            files.append({"path": rel, "size": os.path.getsize(path),
                          "sha256": sha256(path)})

    manifest = {
        "product": "lock_frame",
        "version": args.version,
        "shared_protocol": int(args.protocol),
        "git": args.git,
        "built_at_utc": datetime.datetime.now(datetime.timezone.utc)
                        .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "toolchain": args.toolchain,
        "files": files,
    }
    out = os.path.join(root, "manifest.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print("[manifest] %s (%d files)" % (out, len(files)))


if __name__ == "__main__":
    main()
