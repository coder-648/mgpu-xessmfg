#!/usr/bin/env bash
# ===========================================================================
#  build_xefg_smoke.sh - build tests/xefg_smoke/xefg_smoke.exe on Windows
#
#  The harness loads whichever libxess_fg.dll you name, so the same binary tests the
#  real runtime (baseline) and our stub. Sources are staged into a Windows-visible
#  directory because cl.exe cannot use a UNC path as its working directory.
# ===========================================================================
set -uo pipefail
REPO_WSL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE="${MGPU_STAGE:-/mnt/c/temp/mgpu}/xefg_smoke"
STAGE_WIN="$(wslpath -w "$STAGE")"

rm -rf "$STAGE"
mkdir -p "$STAGE" "$REPO_WSL/build"
cp "$REPO_WSL/tests/xefg_smoke/xefg_smoke.cpp" "$STAGE/"
# Headers are copied next to the sources: the repo lives on the WSL filesystem, and a
# UNC include path is the one thing cl.exe will not take from a Windows-visible cwd.
cp -r "$REPO_WSL/third_party/xess/inc/." "$STAGE/"

cat > "$STAGE/build.bat" <<'BAT'
@echo off
setlocal
cd /d "%~dp0"
set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )
cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- ^
  /I. ^
  xefg_smoke.cpp /Fe:xefg_smoke.exe /Fo:xefg_smoke.obj ^
  /link d3d12.lib dxgi.lib
if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK -^> %CD%\xefg_smoke.exe
BAT

# cmd.exe needs a Windows cwd; the batch file does the cd itself, so this only has to name it.
"$REPO_WSL/scripts/rcmd.sh" xefg_build "cmd.exe /c \"$STAGE_WIN\\build.bat\""
sleep 6
tail -4 "$REPO_WSL/temp/cmd/xefg_build.log"
if [ -f "$STAGE/xefg_smoke.exe" ]; then
    cp "$STAGE/xefg_smoke.exe" "$REPO_WSL/build/" && echo "[build] copied to build/xefg_smoke.exe"
else
    echo "[build] no exe produced"; exit 1
fi
