#!/usr/bin/env bash
# ===========================================================================
#  Build the "double-click" deployment pack for a game:
#
#      dist/mgpu_reshade_pack/            <- folder, copy anywhere
#      dist/mgpu_reshade_pack.zip
#
#  Layout produced:
#      install.bat / install.ps1        drag the game .exe onto install.bat
#      uninstall.bat / uninstall.ps1    full rollback
#      README.txt
#      files/ReShade64.dll              ReShade 6.8 (matches the add-on ABI)
#      files/game_capture.addon64       built by scripts/build_addon.sh
#      files/game_capture.ini
#      files/ReShade.ini / ReShadePreset.ini
#      files/reshade-shaders/Shaders/mgpu_depth_tap.fx
#      tools/mgpu_ui.exe + bridge.exe   optional, for launching afterwards
#      md5.txt
#
#    ./scripts/make_addon_package.sh                 # build add-on if needed
#    RESHADE_SRC=/path/ReShade64.dll ./scripts/make_addon_package.sh
#    SKIP_TOOLS=1 ./scripts/make_addon_package.sh    # add-on only (small)
# ===========================================================================
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACK_SRC="$REPO/packaging/reshade_pack"
OUT="$REPO/dist/mgpu_reshade_pack"
ZIP="$REPO/dist/mgpu_reshade_pack.zip"
ADDON="$REPO/build/game_capture.addon64"
FX="$REPO/effects/mgpu_depth_tap.fx"
# Windows-visible staging dir (see docs/ops/CONFIG.md). Only used as a fallback
# location for the ReShade host DLL and the XeSS runtime DLLs.
STAGE_ROOT="${MGPU_STAGE:-/mnt/c/temp/mgpu}"

say() { printf '[pack] %s\n' "$*"; }
die() { printf '[pack] ERROR: %s\n' "$*" >&2; exit 1; }

[ -d "$PACK_SRC" ] || die "missing $PACK_SRC"

# ---------------------------------------------------------------- 1. add-on
if [ ! -f "$ADDON" ] || [ "${REBUILD:-0}" = "1" ]; then
    say "building game_capture.addon64 ..."
    "$REPO/scripts/build_addon.sh" src/game_capture game_capture || die "add-on build failed"
fi
[ -f "$ADDON" ] || die "no $ADDON after build"
say "add-on: $(md5sum "$ADDON" | cut -d' ' -f1)  $(stat -c%s "$ADDON") B"

# ---------------------------------------------------------------- 2. ReShade host
# The add-on's API/ABI is pinned to ReShade 6.8 (docs/ops/DEPENDENCIES.md). Point
# RESHADE_SRC at any 6.8 x64 ReShade64.dll (or dxgi.dll renamed to it); the two
# fallbacks only cover this project's own working layout. Never ship a mismatched
# version silently - the PE check below refuses anything else.
CANDIDATES=(
    "${RESHADE_SRC:-}"
    "$STAGE_ROOT/ReShade64.dll"
)
RESHADE=""
for c in "${CANDIDATES[@]}"; do
    [ -n "$c" ] && [ -f "$c" ] && { RESHADE="$c"; break; }
done
[ -n "$RESHADE" ] || die "no ReShade 6.8 x64 dll found - set RESHADE_SRC=/path/to/ReShade64.dll"
say "ReShade: $RESHADE ($(stat -c%s "$RESHADE") B)"

# x64 PE check: e_lfanew -> PE\0\0 -> machine 0x8664
python3 - "$RESHADE" <<'PY' || die "ReShade source is not an x64 PE"
import struct, sys
d = open(sys.argv[1], 'rb').read()
if d[:2] != b'MZ': sys.exit(1)
e = struct.unpack_from('<I', d, 0x3c)[0]
if d[e:e+4] != b'PE\0\0': sys.exit(1)
if struct.unpack_from('<H', d, e+4)[0] != 0x8664: sys.exit(1)
if d.lower().count(b'reshade') == 0: sys.exit(1)
PY

# ---------------------------------------------------------------- 3. assemble
rm -rf "$OUT"
mkdir -p "$OUT/files/reshade-shaders/Shaders"

# PowerShell 5.1 parses .ps1 as ANSI unless it sees a UTF-8 BOM - without it every
# Chinese string in the script becomes a parse error. Enforce it here so a careless
# edit cannot silently break the installers.
python3 - "$PACK_SRC" <<'PY' || die "ps1 re-encode failed"
import sys, os
base = sys.argv[1]
for name in ("install.ps1", "uninstall.ps1"):
    p = os.path.join(base, name)
    raw = open(p, "rb").read()
    body = raw[3:] if raw.startswith(b"\xef\xbb\xbf") else raw
    txt = body.decode("utf-8").replace("\r\n", "\n").replace("\n", "\r\n")
    open(p, "wb").write(b"\xef\xbb\xbf" + txt.encode("utf-8"))
    print("[pack] %s: UTF-8 BOM + CRLF (%d B)" % (name, os.path.getsize(p)))
PY

cp "$PACK_SRC"/install.bat "$PACK_SRC"/install.ps1 "$OUT/"
cp "$PACK_SRC"/uninstall.bat "$PACK_SRC"/uninstall.ps1 "$OUT/"
cp "$PACK_SRC"/README.txt "$OUT/"
cp "$PACK_SRC"/ReShade.ini "$PACK_SRC"/ReShadePreset.ini "$PACK_SRC"/game_capture.ini \
   "$OUT/files/"
cp "$RESHADE" "$OUT/files/ReShade64.dll"
cp "$ADDON"   "$OUT/files/game_capture.addon64"
cp "$FX"      "$OUT/files/reshade-shaders/Shaders/mgpu_depth_tap.fx"

if [ "${SKIP_TOOLS:-0}" != "1" ]; then
    mkdir -p "$OUT/tools"
    for f in mgpu_ui.exe bridge.exe; do
        if [ -f "$REPO/build/$f" ]; then cp "$REPO/build/$f" "$OUT/tools/"; fi
    done
    # bridge needs its two side DLLs at runtime; they come from the XeSS SDK
    # checkout (gitignored) or from the staging dir.
    for f in libxess_fg.dll libxell.dll; do
        for src in "$REPO/third_party/xess/bin/$f" "$STAGE_ROOT/bin/$f"; do
            if [ -f "$src" ]; then cp "$src" "$OUT/tools/"; break; fi
        done
    done
    say "tools: $(ls "$OUT/tools" 2>/dev/null | tr '\n' ' ')"
fi

# ---------------------------------------------------------------- 4. manifest
(cd "$OUT" && find . -type f ! -name md5.txt | sort | while read -r f; do
    printf '%s  %s\n' "$(md5sum "$f" | cut -d' ' -f1)" "${f#./}"
done > md5.txt)
say "manifest: $(grep -c '' "$OUT/md5.txt") files"

# ---------------------------------------------------------------- 5. zip
( cd "$REPO/dist" && rm -f "$(basename "$ZIP")" && zip -qr "$(basename "$ZIP")" "$(basename "$OUT")" ) \
    || die "zip failed (is zip installed?)"

say "done:"
say "  folder  $OUT"
say "  zip     $ZIP  ($(stat -c%s "$ZIP") B)"
say ""
say "install on Windows:  drag the game's .exe onto install.bat"
say "              or:    install.bat \"D:\\path\\to\\game\\bin\\x64\" [dxgi|d3d11|d3d12]"
