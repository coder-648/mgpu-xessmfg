#!/usr/bin/env bash
# ===========================================================================
#  build_capture_dll.sh - build mgpu_capture.dll (capture core, no ReShade)
#
#    ./scripts/build_capture_dll.sh      -> build/mgpu_capture.dll
#
#  Same capture sources (unity.cpp = game_capture.cpp + four input sources), compiled
#  against src/host/shim/reshade.hpp (a minimal shim) with -DMGPU_OWN_HOST, so the
#  per-frame trigger moves from ReShade's begin_effects to the upscaler's evaluate.
#  Everything cl.exe needs is copied into a Windows directory first: the repo lives on
#  the WSL filesystem and cl cannot read sources or /I includes over UNC.
# ===========================================================================
set -uo pipefail
REPO_WSL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE="${MGPU_STAGE:-/mnt/c/temp/mgpu}/capture_dll"

rm -rf "$STAGE"
mkdir -p "$STAGE" "$REPO_WSL/build"

cp "$REPO_WSL/src/game_capture/"*.cpp "$REPO_WSL/src/game_capture/"*.h "$STAGE/"
cp "$REPO_WSL/src/host/mgpu_host_glue.cpp" "$STAGE/"
mkdir -p "$STAGE/shim" "$STAGE/wrap/generated"
cp "$REPO_WSL/src/host/shim/"*.hpp "$STAGE/shim/"
cp "$REPO_WSL/src/host/wrap/"*.h "$REPO_WSL/src/host/wrap/"*.cpp "$STAGE/wrap/"
cp "$REPO_WSL/src/host/wrap/generated/"*.h "$REPO_WSL/src/host/wrap/generated/"*.cpp "$STAGE/wrap/generated/"
[ -f "$REPO_WSL/src/mgpu_shared.h" ] && cp "$REPO_WSL/src/mgpu_shared.h" "$STAGE/"
cp "$REPO_WSL/src/"*.h "$STAGE/"

cp -r "$REPO_WSL/third_party/minhook" "$STAGE/"
mkdir -p "$STAGE/xess" "$STAGE/dlss" "$STAGE/ffxapi"
cp -r "$REPO_WSL/third_party/xess/inc/." "$STAGE/xess/"
cp -r "$REPO_WSL/third_party/dlss/include/." "$STAGE/dlss/"
cp -r "$REPO_WSL/third_party/ffx/api/include/." "$STAGE/ffxapi/"

cat > "$STAGE/build.bat" <<'BAT'
@echo off
setlocal
cd /d "%~dp0"
set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )
if not exist obj md obj
cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- ^
  /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DMGPU_OWN_HOST ^
  /I. /Ishim /Iwrap /Iminhook\include /Ixess /Idlss /Iffxapi ^
  unity.cpp mgpu_host_glue.cpp wrap\wrap_hooks.cpp wrap\generated\wrapped_d3d12.cpp ^
  minhook\src\buffer.c minhook\src\hook.c minhook\src\trampoline.c minhook\src\hde\hde64.c ^
  /Fe:mgpu_capture.dll /Fo:obj\ ^
  /link /DLL user32.lib dxgi.lib d3d12.lib
if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK -^> %CD%\mgpu_capture.dll
BAT

STAGE_WIN="$(wslpath -w "$STAGE")"
"$REPO_WSL/scripts/rcmd.sh" capdll_build "cmd.exe /c \"$STAGE_WIN\\build.bat\""
sleep 25
grep -aE 'error|FAILED|OK -' "$REPO_WSL/temp/cmd/capdll_build.log" | head -20
if [ -f "$STAGE/mgpu_capture.dll" ]; then
    cp "$STAGE/mgpu_capture.dll" "$REPO_WSL/build/" && echo "[build] copied to build/mgpu_capture.dll"
else
    echo "[build] no DLL produced"; exit 1
fi
