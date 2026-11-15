#!/usr/bin/env bash
# ===========================================================================
#  Build the version.dll proxy (arch spoof that arms before slInit).
#    ./scripts/build_proxy.sh            -> build/version.dll
#
#  Why a separate module: Streamline evicts sl.dlss_g inside slInit, which runs
#  before D3D is created, so a ReShade add-on is always too late to answer
#  NvAPI_GPU_GetArchInfo. See src/version_proxy/version_min.cpp (the first, much
#  larger version.cpp is retired - kept in src/version_proxy/legacy/).
# ===========================================================================
set -uo pipefail

REPO_WSL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE_ROOT="${MGPU_STAGE:-/mnt/c/temp/mgpu}"
BUILD_LNX="$STAGE_ROOT/version_proxy"
REPO_WIN="$(wslpath -w "$REPO_WSL")"

rm -rf "$BUILD_LNX"
mkdir -p "$BUILD_LNX" "$REPO_WSL/build"

cp "$REPO_WSL/src/version_proxy/version_min.cpp" "$BUILD_LNX/"
cp "$REPO_WSL/src/version_proxy/version.def" "$BUILD_LNX/"
cp "$REPO_WSL/src/version_proxy/build.bat"   "$BUILD_LNX/"

cd "$BUILD_LNX" || exit 1
cmd.exe /c build.bat "$REPO_WIN" > build.log 2>&1
rc=$?

echo "[build] cl exit code: $rc"
echo "---------------- build.log ----------------"
iconv -f GBK -t UTF-8 -c build.log 2>/dev/null || cat build.log
echo "-------------------------------------------"

if [ -f version.dll ]; then
    cp version.dll "$REPO_WSL/build/version.dll"
    echo "[build] dll -> $REPO_WSL/build/version.dll"
else
    echo "[build] NO DLL PRODUCED"
fi
exit $rc
