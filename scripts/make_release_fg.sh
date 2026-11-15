#!/usr/bin/env bash
# ===========================================================================
#  Release bundle for the CURRENT two-GPU setup: fakerFG + producer + consumer.
#  (The old make_local_release.sh packs the retired bridge/ReShade layout.)
#
#      ./scripts/make_release_fg.sh                 # build if missing, then pack
#      REBUILD=1 ./scripts/make_release_fg.sh       # fresh build of all three
#      MGPU_VERSION=0.11-marker ./scripts/make_release_fg.sh
#      SKIP_XESS_DLLS=1 ./scripts/make_release_fg.sh     # leave Intel's DLLs out
#
#  Produces dist/mgpu-<ver>/ and dist/mgpu-<ver>.zip. Everything inside
#  dist/mgpu-<ver>/game/ goes into the game's exe directory.
# ===========================================================================
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
say() { printf '[release] %s\n' "$*"; }
die() { printf '[release] ERROR: %s\n' "$*" >&2; exit 1; }

SHA="$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo nogit)"
VER="${MGPU_VERSION:-0.11-$SHA}"
OUT="$REPO/dist/mgpu-$VER"
ZIP="$REPO/dist/mgpu-$VER.zip"

if [ "${REBUILD:-0}" = "1" ]; then
    say "building fakerFG";  "$REPO/scripts/build_faker_fg.sh" || die "fakerFG build failed"
    say "building producer"; "$REPO/scripts/build_producer.sh" || die "producer build failed"
    say "building consumer"; "$REPO/scripts/build_consumer.sh" || die "consumer build failed"
fi
for f in build/fakerFG.dll build/xess_mfg_producer.dll build/xess_mfg_consumer.exe; do
    [ -f "$REPO/$f" ] || die "missing $f - run with REBUILD=1"
done

say "version $VER"
rm -rf "$OUT"; mkdir -p "$OUT/game" "$OUT/docs"

# --- game payload: one fakerFG binary under the three names it needs ---------
FG="$REPO/build/fakerFG.dll"
cp -f "$FG" "$OUT/game/dxgi.dll"
cp -f "$FG" "$OUT/game/nvngx_dlssg.dll"
cp -f "$FG" "$OUT/game/fakerFG.dll"
cp -f "$REPO/build/xess_mfg_producer.dll" "$OUT/game/"
cp -f "$REPO/build/xess_mfg_consumer.exe" "$OUT/game/"
# The UI launches the consumer from its own directory, so it ships next to it.
if [ -f "$REPO/build/mgpu_ui.exe" ]; then
    cp -f "$REPO/build/mgpu_ui.exe" "$OUT/game/"
else
    say "note: build/mgpu_ui.exe missing (run scripts/build_ui.sh) - UI not packed"
fi
if [ "${SKIP_XESS_DLLS:-0}" != "1" ]; then
    for dll in libxess_fg.dll libxell.dll; do
        if [ -f "$REPO/third_party/xess/bin/$dll" ]; then
            cp -f "$REPO/third_party/xess/bin/$dll" "$OUT/game/"
        else
            say "note: $dll not found in third_party/xess/bin - add it yourself"
        fi
    done
fi
[ -f "$REPO/packaging/reshade_pack/fakerFG.ini" ] \
    && cp -f "$REPO/packaging/reshade_pack/fakerFG.ini" "$OUT/game/"
cp -f "$REPO/scripts/release_run_consumer.bat" "$OUT/game/run_consumer.bat" \
    || die "missing scripts/release_run_consumer.bat"

# --- docs -------------------------------------------------------------------
cp -f "$REPO/CHANGELOG.md" "$OUT/"
cp -f "$REPO/docs/findings/PERF_ROUND_HANDOVER.md" "$OUT/docs/" \
    || say "note: PERF_ROUND_HANDOVER.md missing"
cp -f "$REPO/scripts/release_README.md" "$OUT/README.md" \
    || die "missing scripts/release_README.md"

# --- checksums + zip --------------------------------------------------------
( cd "$OUT" && find . -type f ! -name SHA256SUMS.txt -print0 | sort -z \
    | xargs -0 sha256sum > SHA256SUMS.txt )
( cd "$REPO/dist" && rm -f "$(basename "$ZIP")" && zip -qr "$(basename "$ZIP")" "$(basename "$OUT")" )
say "folder: $OUT"
say "zip   : $ZIP  ($(du -h "$ZIP" | cut -f1))"
say "next  : copy game/* into the game's exe directory, edit GAME_EXE, run run_consumer.bat"
