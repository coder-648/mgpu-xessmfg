#!/usr/bin/env bash
# ===========================================================================
#  Build one test module on the Windows side, from WSL.
#
#    ./scripts/build_m0.sh                 # default module: m0_probe
#    ./scripts/build_m0.sh m0b_transport   # module in tests/<name>/
#
#  Why the copy step: cl.exe cannot use a UNC path as its working directory, so
#  sources are staged into <STAGE_DIR>\<module>, built there, and the artifacts
#  are copied back into <repo>/build/.
#
#  Per PLAN.md 补充 1, all terminal output goes to a file and is then read,
#  instead of relying on live console echoes.
# ===========================================================================
set -uo pipefail

MODULE="${1:-m0_probe}"
REPO_WSL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="$REPO_WSL/tests/$MODULE"
STAGE_ROOT="${MGPU_STAGE:-/mnt/c/temp/mgpu}"  # Windows-visible staging dir (cl.exe cannot use a UNC cwd)
BUILD_LNX="$STAGE_ROOT/$MODULE"
REPO_WIN="$(wslpath -w "$REPO_WSL")"

if [ ! -d "$SRC_DIR" ]; then
    echo "[build] no such module dir: $SRC_DIR"
    exit 1
fi

echo "[build] module      : $MODULE"
echo "[build] repo (wsl)  : $REPO_WSL"
echo "[build] repo (win)  : $REPO_WIN"

rm -rf "$BUILD_LNX"
mkdir -p "$BUILD_LNX" "$REPO_WSL/build"

shopt -s nullglob
srcs=("$SRC_DIR"/*.cpp)
shopt -u nullglob
if [ ${#srcs[@]} -eq 0 ]; then
    echo "[build] no .cpp in $SRC_DIR"
    exit 1
fi
cp "${srcs[@]}" "$BUILD_LNX/"
cp "$SRC_DIR/build.bat" "$BUILD_LNX/"

# XeSS runtime DLLs must sit next to the exe at run time.
for dll in libxess_fg.dll libxell.dll libxess.dll; do
    src="$REPO_WSL/third_party/xess/bin/$dll"
    [ -f "$src" ] && cp "$src" "$BUILD_LNX/"
done

cd "$BUILD_LNX" || exit 1
first_src="$(basename "${srcs[0]}")"

cmd.exe /c build.bat "$REPO_WIN" "$first_src" > build.log 2>&1
rc=$?

echo "[build] cl exit code: $rc"
echo "---------------- build.log ----------------"
cat build.log
echo "-------------------------------------------"

exe="${first_src%.cpp}.exe"
if [ -f "$exe" ]; then
    cp "$exe" "$REPO_WSL/build/"
    echo "[build] exe -> $REPO_WSL/build/$exe"
else
    echo "[build] NO EXE PRODUCED"
fi

exit $rc

