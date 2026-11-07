#!/usr/bin/env bash
# ===========================================================================
#  Build osd_test.exe - the standalone OSD layout harness (dev tool only).
#
#  It renders four sample OSD lines on a fake "game" window so the pill, font
#  and spacing can be checked without launching a game. Same staging trick as
#  build_bridge.sh (cl cannot use a UNC working directory).
#
#      ./scripts/build_osd_test.sh
#      <STAGE_DIR>\osd\osd_test.exe        # closes after 30 s or on Esc
# ===========================================================================
set -uo pipefail

REPO_WSL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE_ROOT="${MGPU_STAGE:-/mnt/c/temp/mgpu}"  # Windows-visible staging dir (cl.exe cannot use a UNC cwd)
BUILD_LNX="$STAGE_ROOT/osd"

rm -rf "$BUILD_LNX"
mkdir -p "$BUILD_LNX"

cp "$REPO_WSL/src/bridge/osd_test.cpp" "$BUILD_LNX/"
cp "$REPO_WSL/src/bridge/osd_panel.h"  "$BUILD_LNX/"

cd "$BUILD_LNX" || exit 1
cat > build_test.bat <<'BAT'
@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )
cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- /DNOMINMAX osd_test.cpp /Fe:osd_test.exe /Fo:osd_test.obj /link user32.lib gdi32.lib
if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK -^> %CD%\osd_test.exe
exit /b 0
BAT
cmd.exe /c build_test.bat > bt.log 2>&1
rc=$?
echo "[build] cl exit code: $rc"
iconv -f GBK -t UTF-8 -c bt.log 2>/dev/null || cat bt.log
[ -f osd_test.exe ] && echo "[build] exe -> $BUILD_LNX/osd_test.exe"
exit $rc
