#!/usr/bin/env bash
# ===========================================================================
#  Build a ReShade add-on (DLL, *.addon64) from a module under tests/<name>/.
#
#    ./scripts/build_addon.sh src/m2_probe          -> m2_probe.addon64
#    ./scripts/build_addon.sh src/m2_probe probe     -> probe.addon64
#
#  Sources are staged into <STAGE_DIR>\<module> because cl.exe cannot use a
#  UNC path as its working directory; the result is copied back into build/.
# ===========================================================================
set -uo pipefail

REL="${1:-src/m2_probe}"
NAME="${2:-$(basename "$REL")}"
REPO_WSL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# accept either "src/foo", "tests/foo", or a bare module name
if [[ "$REL" == */* ]]; then
    SRC_DIR="$REPO_WSL/$REL"
else
    SRC_DIR="$REPO_WSL/tests/$REL"
fi
STAGE_ROOT="${MGPU_STAGE:-/mnt/c/temp/mgpu}"  # Windows-visible staging dir (cl.exe cannot use a UNC cwd)
BUILD_LNX="$STAGE_ROOT/$NAME"
REPO_WIN="$(wslpath -w "$REPO_WSL")"

if [ ! -d "$SRC_DIR" ]; then
    echo "[build] no such module dir: $SRC_DIR"; exit 1
fi

echo "[build] source dir : $SRC_DIR"
echo "[build] output     : $NAME.addon64"
echo "[build] repo (win) : $REPO_WIN"

rm -rf "$BUILD_LNX"
mkdir -p "$BUILD_LNX" "$REPO_WSL/build"

shopt -s nullglob
srcs=("$SRC_DIR"/*.cpp)
hdrs=("$SRC_DIR"/*.h)
shopt -u nullglob
if [ ${#srcs[@]} -eq 0 ]; then
    echo "[build] no .cpp in $SRC_DIR"; exit 1
fi
cp "${srcs[@]}" "$SRC_DIR/build_addon.bat" "$BUILD_LNX/"
# Headers too: multi-file modules (src/game_capture) keep their interfaces next to the
# sources and the bat puts the staging dir on the include path (/I"%~dp0").
if [ ${#hdrs[@]} -gt 0 ]; then cp "${hdrs[@]}" "$BUILD_LNX/"; fi

cd "$BUILD_LNX" || exit 1
first_src="$(basename "${srcs[0]}")"
# A module that ships a unity.cpp is compiled through it (multi-file modules such as
# src/game_capture: host + one file per input source). Single-file modules keep the
# old behaviour.
if [ -f "$SRC_DIR/unity.cpp" ]; then first_src="unity.cpp"; fi

cmd.exe /c build_addon.bat "$REPO_WIN" "$first_src" "$NAME.addon64" > build.log 2>&1
rc=$?

echo "[build] cl exit code: $rc"
echo "---------------- build.log ----------------"
iconv -f GBK -t UTF-8 -c build.log 2>/dev/null || cat build.log
echo "-------------------------------------------"

if [ -f "$NAME.addon64" ]; then
    cp "$NAME.addon64" "$REPO_WSL/build/"
    echo "[build] addon -> $REPO_WSL/build/$NAME.addon64"
else
    echo "[build] NO DLL PRODUCED"
fi
exit $rc
