#!/usr/bin/env bash
# ===========================================================================
#  build_gpu_codec.sh - build tests/gpu_codec/gpu_codec.exe (D3D12 + HLSL codec)
#
#  Answers "does the same bitstream come out of every GPU on this box?" and
#  "how many ms does encode/decode cost on each of them?" - which is what the
#  four vendor pairings (N/N, N/A, A/N, A/A) actually need.
#
#    ./scripts/build_gpu_codec.sh                  # build only
#    ./scripts/build_gpu_codec.sh --list           # build + list adapters
#    ./scripts/build_gpu_codec.sh --all --w 3840 --h 2160 --nw 2
#    ./scripts/build_gpu_codec.sh --all --raw 'E:\mgpu_dataset\...\f00_mv.raw'
#
#  Arguments after the first are passed straight to gpu_codec.exe.
# ===========================================================================
set -uo pipefail
REPO_WSL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE="${MGPU_STAGE:-/mnt/c/temp/mgpu}/gpu_codec"
FXC='C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe'

rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$REPO_WSL/tests/gpu_codec/gpu_codec.cpp" "$STAGE/"
cp "$REPO_WSL/tests/gpu_codec/codec.hlsl"    "$STAGE/"
cp "$REPO_WSL/tests/lossless_bench/fr_codec.h" "$STAGE/"

cat > "$STAGE/build.bat" <<BAT
@echo off
setlocal
cd /d "%~dp0"
set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )
"$FXC" /nologo /T cs_5_0 /E Encode /Foencode.cso codec.hlsl > fxc_enc.log 2>&1
if errorlevel 1 ( echo [build] fxc Encode FAILED & type fxc_enc.log & exit /b 1 )
"$FXC" /nologo /T cs_5_0 /E Decode /Fodecode.cso codec.hlsl > fxc_dec.log 2>&1
if errorlevel 1 ( echo [build] fxc Decode FAILED & type fxc_dec.log & exit /b 1 )
cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- ^
  gpu_codec.cpp /Fe:gpu_codec.exe /Fo:gpu_codec.obj ^
  /link d3d12.lib dxgi.lib
if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK
BAT

cd "$STAGE" || exit 1
cmd.exe /c build.bat > build.log 2>&1
rc=$?
iconv -f GBK -t UTF-8 -c build.log | grep -aE 'error|FAILED|warning C|OK' | head -20
if [ ! -f "$STAGE/gpu_codec.exe" ]; then
    echo "[build] no exe produced (see $STAGE/build.log)"; exit 1
fi
echo "[build] $STAGE/gpu_codec.exe"

if [ "$rc" -eq 0 ] && [ "$#" -gt 0 ]; then
    echo "[run] gpu_codec.exe $*"
    cmd.exe /c gpu_codec.exe "$@" 2>&1 | iconv -f GBK -t UTF-8 -c
fi
exit $rc
