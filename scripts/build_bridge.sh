#!/usr/bin/env bash
# ===========================================================================
#  Build bridge.exe on the Windows side, from WSL.
#  Sources are staged into <STAGE_DIR>\bridge (cl cannot use a UNC cwd) and the
#  XeSS runtime DLLs are copied next to the exe so it can run straight away.
# ===========================================================================
set -uo pipefail

REPO_WSL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE_ROOT="${MGPU_STAGE:-/mnt/c/temp/mgpu}"  # Windows-visible staging dir (cl.exe cannot use a UNC cwd)
BUILD_LNX="$STAGE_ROOT/bridge"
REPO_WIN="$(wslpath -w "$REPO_WSL")"

rm -rf "$BUILD_LNX"
mkdir -p "$BUILD_LNX" "$REPO_WSL/build"

cp "$REPO_WSL/src/bridge/bridge.cpp" "$BUILD_LNX/"
cp "$REPO_WSL/src/bridge/osd_panel.h" "$BUILD_LNX/"
cp "$REPO_WSL/src/bridge/build.bat"  "$BUILD_LNX/"
for dll in libxess_fg.dll libxell.dll; do
    cp "$REPO_WSL/third_party/xess/bin/$dll" "$BUILD_LNX/" || true
done

cd "$BUILD_LNX" || exit 1
cmd.exe /c build.bat "$REPO_WIN" > build.log 2>&1
rc=$?

echo "[build] cl exit code: $rc"
echo "---------------- build.log ----------------"
iconv -f GBK -t UTF-8 -c build.log 2>/dev/null || cat build.log
echo "-------------------------------------------"

if [ -f bridge.exe ]; then
    cp bridge.exe "$REPO_WSL/build/"
    echo "[build] exe -> $REPO_WSL/build/bridge.exe"
else
    echo "[build] NO EXE PRODUCED"
fi
exit $rc
