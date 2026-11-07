#!/usr/bin/env bash
# ===========================================================================
#  Build mgpu_ui.exe (the Lossless-Scaling-style front-end) on the Windows side.
#  Same trick as build_bridge.sh: stage into <STAGE_DIR>\ui because cl cannot use
#  a UNC working directory, then copy the exe back into build/.
# ===========================================================================
set -uo pipefail

REPO_WSL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE_ROOT="${MGPU_STAGE:-/mnt/c/temp/mgpu}"  # Windows-visible staging dir (cl.exe cannot use a UNC cwd)
BUILD_LNX="$STAGE_ROOT/ui"
REPO_WIN="$(wslpath -w "$REPO_WSL")"

rm -rf "$BUILD_LNX"
mkdir -p "$BUILD_LNX" "$REPO_WSL/build"

cp "$REPO_WSL/src/mgpu_ui/mgpu_ui.cpp" "$BUILD_LNX/"
cp "$REPO_WSL/src/mgpu_ui/build.bat"  "$BUILD_LNX/"

cd "$BUILD_LNX" || exit 1
cmd.exe /c build.bat "$REPO_WIN" > build.log 2>&1
rc=$?

echo "[build] cl exit code: $rc"
echo "---------------- build.log ----------------"
iconv -f GBK -t UTF-8 -c build.log 2>/dev/null || cat build.log
echo "-------------------------------------------"

if [ -f mgpu_ui.exe ]; then
    cp mgpu_ui.exe "$REPO_WSL/build/"
    echo "[build] exe -> $REPO_WSL/build/mgpu_ui.exe"
else
    echo "[build] NO EXE PRODUCED"
fi
exit $rc
