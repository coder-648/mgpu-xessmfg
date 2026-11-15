#!/usr/bin/env bash
# ===========================================================================
#  build_shared_probe.sh - build tests/shared_probe/shared_probe.exe
#
#  Answers "can this machine create a cross-adapter heap from a file mapping,
#  and with which formats?" in two seconds, without launching a game.
#    ./scripts/build_shared_probe.sh && ../../build/shared_probe.exe
# ===========================================================================
set -uo pipefail
REPO_WSL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE="${MGPU_STAGE:-/mnt/c/temp/mgpu}/shared_probe"

rm -rf "$STAGE"
mkdir -p "$STAGE" "$REPO_WSL/build"
cp "$REPO_WSL/tests/shared_probe/shared_probe.cpp" "$STAGE/"

cat > "$STAGE/build.bat" <<'BAT'
@echo off
setlocal
cd /d "%~dp0"
set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )
cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- ^
  shared_probe.cpp /Fe:shared_probe.exe /Fo:shared_probe.obj ^
  /link d3d12.lib dxgi.lib
if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK -^> %CD%\shared_probe.exe
BAT

STAGE_WIN="$(wslpath -w "$STAGE")"
cd "$STAGE" || exit 1
cmd.exe /c build.bat > build.log 2>&1
rc=$?
iconv -f GBK -t UTF-8 -c build.log | grep -aE 'error|FAILED|OK -' | head -8
if [ -f "$STAGE/shared_probe.exe" ]; then
    cp "$STAGE/shared_probe.exe" "$REPO_WSL/build/" && echo "[build] copied to build/shared_probe.exe"
else
    echo "[build] no exe produced"; exit 1
fi
exit $rc
