#!/usr/bin/env bash
# ===========================================================================
#  build_own_host.sh - build mgpu_host.dll (our own D3D12 host, stage 1 = probe)
#
#    ./scripts/build_own_host.sh        -> build/mgpu_host.dll
#
#  Stage 1 is deliberately a *probe*: it installs the four hooks (device, direct
#  queue, swapchain Present, ResourceBarrier) and logs what it sees, without touching
#  the game or our data path. version.dll loads it on its own thread when
#  mgpu_proxy.ini says Host=1 (never from DllMain - loader lock).
# ===========================================================================
set -uo pipefail
REPO_WSL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE_ROOT="${MGPU_STAGE:-/mnt/c/temp/mgpu}"
BUILD_LNX="$STAGE_ROOT/own_host"
REPO_WIN="$(wslpath -w "$REPO_WSL")"

rm -rf "$BUILD_LNX"
mkdir -p "$BUILD_LNX" "$REPO_WSL/build"

cp "$REPO_WSL/src/host/own_host_probe.cpp" "$BUILD_LNX/"

cat > "$BUILD_LNX/build.bat" <<'BAT'
@echo off
setlocal
set "REPO=%~1"
set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )
if not exist obj md obj
cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- ^
  /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX ^
  /I"%REPO%\third_party\minhook\include" ^
  own_host_probe.cpp ^
  "%REPO%\third_party\minhook\src\buffer.c" ^
  "%REPO%\third_party\minhook\src\hook.c" ^
  "%REPO%\third_party\minhook\src\trampoline.c" ^
  "%REPO%\third_party\minhook\src\hde\hde64.c" ^
  /Fe:mgpu_host.dll /Fo:obj\ ^
  /link /DLL user32.lib dxgi.lib d3d12.lib
if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK -^> %CD%\mgpu_host.dll
BAT

cd "$BUILD_LNX" || exit 1
cmd.exe /c build.bat "$REPO_WIN" > build.log 2>&1
rc=$?
echo "[build] cl exit code: $rc"
grep -aE 'error|warning C|OK -' build.log | head -12
if [ -f mgpu_host.dll ]; then
    cp mgpu_host.dll "$REPO_WSL/build/" && echo "[build] copied to build/mgpu_host.dll"
else
    echo "[build] no DLL produced"; exit 1
fi
