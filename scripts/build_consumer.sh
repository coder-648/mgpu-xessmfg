#!/usr/bin/env bash
# ===========================================================================
#  Build xess_mfg_consumer.exe from WSL.
#    ./scripts/build_consumer.sh          -> build/xess_mfg_consumer.exe
#
#  The consumer has its own source tree (src/xess_mfg_consumer): its entry .cpp,
#  modules and shaders are never compiled into bridge.exe. Only the ABI headers
#  under src/ (mgpu_shared.h, xefg_*.h) are shared.
#
#  Sources are staged into <STAGE_DIR>\consumer (cl cannot use a UNC cwd) and the
#  XeSS runtime DLLs are copied next to the exe so it can run straight away.
# ===========================================================================
set -uo pipefail

OUT="xess_mfg_consumer.exe"
LOG="build_consumer.log"

REPO_WSL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE_ROOT="${MGPU_STAGE:-/mnt/c/temp/mgpu}"  # Windows-visible staging dir (cl.exe cannot use a UNC cwd)
BUILD_LNX="$STAGE_ROOT/consumer"
REPO_WIN="$(wslpath -w "$REPO_WSL")"

# unity.cpp is the translation unit; modules/ holds the .cpp units it includes.
python3 "$REPO_WSL/scripts/check_bridge_modules.py" "$REPO_WSL/src/xess_mfg_consumer" || exit 1

rm -rf "$BUILD_LNX"
mkdir -p "$BUILD_LNX" "$REPO_WSL/build"

cp "$REPO_WSL/src/xess_mfg_consumer/unity.cpp" "$BUILD_LNX/"
cp "$REPO_WSL/src/xess_mfg_consumer/consumer_internal.h" "$BUILD_LNX/"
cp "$REPO_WSL/src/xess_mfg_consumer/osd_panel.h" "$BUILD_LNX/"
cp "$REPO_WSL/src/xess_mfg_consumer/build.bat"  "$BUILD_LNX/"
# unity.cpp includes modules/*.cpp in dependency order (fakerFG-style unity build).
cp -r "$REPO_WSL/src/xess_mfg_consumer/modules" "$BUILD_LNX/"
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
    "$(wslpath -w "$REPO_WSL/src/xess_mfg_consumer/shaders/upsample_joint.hlsl")" \
    || { echo "[build] fxc failed for upsample_joint"; exit 1; }
python3 "$REPO_WSL/scripts/embed_cso.py" "$BUILD_LNX/upsample_joint.cso" \
    "$BUILD_LNX/upsample_joint_cso.h" "UpsampleJoint" \
    || { echo "[build] embed failed for upsample_joint"; exit 1; }

# The convert/prepare shaders are compiled at RUNTIME from a string in
# modules/shaders.cpp, so cl cannot catch a broken one - a bad ps_main made the
# consumer fail to start with only a log line. Validate every entry point here.
python3 "$REPO_WSL/scripts/extract_hlsl.py" \
    "$REPO_WSL/src/xess_mfg_consumer/modules/shaders.cpp" "$BUILD_LNX/conv_check.hlsl" \
    || { echo "[build] could not extract k_conv_hlsl"; exit 1; }
for entry in vs_main:vs_5_0 ps_main:ps_5_0 ps_prep_mv:ps_5_0 ps_prep_depth:ps_5_0; do
    name="${entry%%:*}"; prof="${entry##*:}"
    "$FXC_WIN" -nologo -T "$prof" -E "$name" \
        -Fo "$(wslpath -w "$BUILD_LNX/conv_check.cso")" \
        "$(wslpath -w "$BUILD_LNX/conv_check.hlsl")" \
        || { echo "[build] fxc failed for $name"; exit 1; }
done
echo "[build] convert/prepare shaders validated (4 entry points)"

cd "$BUILD_LNX" || exit 1
cmd.exe /c build.bat "$REPO_WIN" > "$LOG" 2>&1
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
