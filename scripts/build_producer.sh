#!/usr/bin/env bash
# ===========================================================================
#  build_producer.sh - build xess_mfg_producer.dll (src/xess_mfg_producer).
#
#    ./scripts/build_producer.sh          -> build/xess_mfg_producer.dll
#
#  A plain DLL: no proxy exports, no ReShade add-on, no shaders. fakerFG loads
#  it and calls the three functions in src/mgpu_producer_api.h.
#  Design: docs/design/XESS_MFG_PRODUCER.md.
# ===========================================================================
set -uo pipefail

NAME="${1:-xess_mfg_producer}"
REL="src/xess_mfg_producer"
REPO_WSL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="$REPO_WSL/$REL"
STAGE_ROOT="${MGPU_STAGE:-/mnt/c/temp/mgpu}"
STAGE="$STAGE_ROOT/xess_mfg_producer"
REPO_WIN="$(wslpath -w "$REPO_WSL")"

if [ ! -d "$SRC_DIR" ]; then
    echo "[build] no such module dir: $SRC_DIR"; exit 1
fi

echo "[build] source dir : $SRC_DIR"
echo "[build] output     : $NAME.dll"

rm -rf "$STAGE"
mkdir -p "$STAGE" "$REPO_WSL/build"
cp "$SRC_DIR"/*.cpp "$STAGE/"

# Shaders: compiled with the SDK's fxc and embedded as byte arrays (same convention as
# build_faker_fg.sh), so the DLL never needs d3dcompiler at runtime.
FXC_WIN="/mnt/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe"
if [ -d "$SRC_DIR/shaders" ]; then
    [ -x "$FXC_WIN" ] || { echo "[build] fxc.exe not found (Windows SDK 10.0.26100.0)"; exit 1; }
    shader_specs=(
        "downsample_mv:cs_5_0:CSMain:DownsampleMV"
        "downsample_depth:cs_5_0:CSMain:DownsampleDepth"
    )
    for spec in "${shader_specs[@]}"; do
        base="${spec%%:*}"; rest="${spec#*:}"
        profile="${rest%%:*}"; rest="${rest#*:}"
        entry="${rest%%:*}"; symbol="${rest##*:}"
        hlsl="$SRC_DIR/shaders/${base}.hlsl"
        out="${base}_${entry}"
        case "$symbol" in
            DownsampleMV)    out="downsample_mv" ;;
            DownsampleDepth) out="downsample_depth" ;;
        esac
        [ -f "$hlsl" ] || { echo "[build] missing shader $hlsl"; exit 1; }
        "$FXC_WIN" -nologo -T "$profile" -E "$entry" \
            -Fo "$(wslpath -w "$STAGE/${out}.cso")" "$(wslpath -w "$hlsl")" \
            || { echo "[build] fxc failed for $out"; exit 1; }
        python3 "$REPO_WSL/scripts/embed_cso.py" "$STAGE/${out}.cso" "$STAGE/${out}_cso.h" "$symbol" \
            || { echo "[build] embed failed for $out"; exit 1; }
    done
fi

cat > "$STAGE/build.bat" <<'BAT'
@echo off
setlocal
cd /d "%~dp0"
set "REPO=%~1"
set "OUT=%~2"
if "%REPO%"=="" ( echo [build] no repo path & exit /b 1 )
set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )
if not exist obj mkdir obj
cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- ^
  /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX ^
  /I"%~dp0." ^
  /I"%REPO%\src" ^
  /I"%REPO%\src\faker_fg" ^
  "%~dp0producer.cpp" ^
  /Fe:"%OUT%" /Fo:"obj\\" ^
  /link /DLL d3d12.lib dxgi.lib
if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK -^> %CD%\%OUT%
BAT

cd "$STAGE" || exit 1
cmd.exe /c build.bat "$REPO_WIN" "$NAME.dll" > build.log 2>&1
rc=$?

echo "[build] cl exit code: $rc"
echo "---------------- build.log ----------------"
iconv -f GBK -t UTF-8 -c build.log 2>/dev/null || cat build.log
echo "-------------------------------------------"

if [ -f "$STAGE/$NAME.dll" ]; then
    cp "$STAGE/$NAME.dll" "$REPO_WSL/build/"
    echo "[build] dll -> $REPO_WSL/build/$NAME.dll"
else
    echo "[build] NO DLL PRODUCED"
fi
exit $rc
