#!/usr/bin/env bash
# ===========================================================================
#  Build bridge.exe from WSL.
#    ./scripts/build_bridge.sh            -> build/bridge.exe
#  Sources are staged into <STAGE_DIR>\bridge (cl cannot use a UNC cwd) and the
#  XeSS runtime DLLs are copied next to the exe so it can run straight away.
#
#  The consumer is a separate program with its own source tree:
#    ./scripts/build_consumer.sh          -> build/xess_mfg_consumer.exe
# ===========================================================================
set -uo pipefail

TARGET="${1:-bridge}"
case "$TARGET" in
    bridge)   BUILD_BAT="build.bat";          OUT="bridge.exe";             LOG="build.log" ;;
    *) echo "usage: $0 [bridge]  (consumer: ./scripts/build_consumer.sh)"; exit 2 ;;
esac

REPO_WSL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE_ROOT="${MGPU_STAGE:-/mnt/c/temp/mgpu}"  # Windows-visible staging dir (cl.exe cannot use a UNC cwd)
BUILD_LNX="$STAGE_ROOT/bridge"
REPO_WIN="$(wslpath -w "$REPO_WSL")"

# bridge.cpp is only the prelude + module index; refuse to build a partial tree.
python3 "$REPO_WSL/scripts/check_bridge_modules.py" "$REPO_WSL/src/bridge" || exit 1

rm -rf "$BUILD_LNX"
mkdir -p "$BUILD_LNX" "$REPO_WSL/build"

cp "$REPO_WSL/src/bridge/bridge.cpp" "$BUILD_LNX/"
cp "$REPO_WSL/src/bridge/osd_panel.h" "$BUILD_LNX/"
cp "$REPO_WSL/src/bridge/build.bat"  "$BUILD_LNX/"
# bridge.cpp is a module index; the implementation lives in src/bridge/modules/*.inc.
cp -r "$REPO_WSL/src/bridge/modules" "$BUILD_LNX/"
for dll in libxess_fg.dll libxell.dll; do
    cp "$REPO_WSL/third_party/xess/bin/$dll" "$BUILD_LNX/" || true
done

# --upsample: the consumer-side joint MV/depth reconstruction shader. Compiled with the
# SDK's fxc and embedded as a byte array, so the exe never needs d3dcompiler at runtime
# (same convention as the producer's downsamplers).
FXC_WIN="/mnt/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe"
if [ ! -x "$FXC_WIN" ]; then
    echo "[build] fxc.exe not found (Windows SDK 10.0.26100.0)"; exit 1
fi
"$FXC_WIN" -nologo -T cs_5_0 -E CSMain \
    -Fo "$(wslpath -w "$BUILD_LNX/upsample_joint.cso")" \
    "$(wslpath -w "$REPO_WSL/src/bridge/shaders/upsample_joint.hlsl")" \
    || { echo "[build] fxc failed for upsample_joint"; exit 1; }
python3 "$REPO_WSL/scripts/embed_cso.py" "$BUILD_LNX/upsample_joint.cso" \
    "$BUILD_LNX/upsample_joint_cso.h" "UpsampleJoint" \
    || { echo "[build] embed failed for upsample_joint"; exit 1; }

cd "$BUILD_LNX" || exit 1
cmd.exe /c "$BUILD_BAT" "$REPO_WIN" > "$LOG" 2>&1
rc=$?

echo "[build] cl exit code: $rc"
echo "---------------- $LOG ----------------"
iconv -f GBK -t UTF-8 -c "$LOG" 2>/dev/null || cat "$LOG"
echo "-------------------------------------------"

if [ -f "$OUT" ]; then
    cp "$OUT" "$REPO_WSL/build/"
    echo "[build] exe -> $REPO_WSL/build/$OUT"
else
    echo "[build] NO EXE PRODUCED ($OUT)"
fi
exit $rc
