#!/usr/bin/env bash
# ===========================================================================
#  build_faker_fg.sh - build fakerFG.dll (src/faker_fg).
#
#    ./scripts/build_faker_fg.sh          -> build/fakerFG.dll
#
#  fakerFG is the FG provider + XeFG backend + (from P1) the DXGI proxy that
#  loads ReShade; see docs/design/FAKER_FG.md. It is a plain DLL, not a ReShade
#  add-on: it is loaded by the game itself, under a proxy name.
#
#  Multi-file module: sources are staged into <STAGE>/faker_fg and compiled
#  through unity.cpp when it exists (same convention as build_addon.sh), so cl
#  never sees a UNC working directory.
# ===========================================================================
set -uo pipefail

NAME="${1:-fakerFG}"
REL="src/faker_fg"
REPO_WSL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="$REPO_WSL/$REL"
STAGE_ROOT="${MGPU_STAGE:-/mnt/c/temp/mgpu}"
STAGE="$STAGE_ROOT/faker_fg"
REPO_WIN="$(wslpath -w "$REPO_WSL")"

if [ ! -d "$SRC_DIR" ]; then
    echo "[build] no such module dir: $SRC_DIR"; exit 1
fi

echo "[build] source dir : $SRC_DIR"
echo "[build] output     : $NAME.dll"
echo "[build] repo (win) : $REPO_WIN"

rm -rf "$STAGE"
mkdir -p "$STAGE" "$REPO_WSL/build"

shopt -s nullglob
srcs=("$SRC_DIR"/*.cpp)
hdrs=("$SRC_DIR"/*.h)
defs=("$SRC_DIR"/*.def)
shopt -u nullglob
if [ ${#srcs[@]} -eq 0 ]; then
    echo "[build] no .cpp in $SRC_DIR"; exit 1
fi
cp "${srcs[@]}" "$STAGE/"
if [ ${#hdrs[@]} -gt 0 ]; then cp "${hdrs[@]}" "$STAGE/"; fi
if [ ${#defs[@]} -gt 0 ]; then cp "${defs[@]}" "$STAGE/"; fi

first_src="$(basename "${srcs[0]}")"
if [ -f "$SRC_DIR/unity.cpp" ]; then first_src="unity.cpp"; fi

# Shaders: compiled with the SDK's fxc and embedded as byte arrays, so the DLL never
# needs d3dcompiler at runtime. The generated header is staged next to the sources
# (compiled with /I"<stage>"), not written into the repository.
FXC_WIN="/mnt/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe"
if [ -d "$SRC_DIR/shaders" ]; then
    if [ ! -x "$FXC_WIN" ]; then
        echo "[build] fxc.exe not found (Windows SDK 10.0.26100.0)"; exit 1
    fi
    # file : profile : entry : symbol   (one CSO + one header per entry)
    shader_specs=(
        "mv_resample:cs_5_0:CSMain:MvResample"
        "render_ui:vs_5_0:VSMain:RenderUiVS"
        "render_ui:ps_5_0:PSMain:RenderUiPS"
        "render_ui_pm:ps_5_0:PSMain:RenderUiPmPS"
        "hudless_compare:ps_5_0:PSMain:HudlessComparePS"
        "hud_copy:cs_5_0:CSMain:HudCopy"
        "marker:ps_5_0:PSMain:MarkerPS"
    )
    for spec in "${shader_specs[@]}"; do
        base="${spec%%:*}"; rest="${spec#*:}"
        profile="${rest%%:*}"; rest="${rest#*:}"
        entry="${rest%%:*}"; symbol="${rest#*:}"
        hlsl="$SRC_DIR/shaders/${base}.hlsl"
        out="${base}_${entry}"
        if [ ! -f "$hlsl" ]; then
            echo "[build] missing shader $hlsl"; exit 1
        fi
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
set "SRC=%~2"
set "OUT=%~3"
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
  /I"%REPO%\third_party\xess\inc" ^
  /I"%REPO%\third_party\streamline\include" ^
  /I"%REPO%\third_party\dlss\include" ^
  /I"%REPO%\third_party\minhook\include" ^
  /I"%REPO%\third_party\minhook\src" ^
  "%SRC%" ^
  "%REPO%\third_party\minhook\src\buffer.c" ^
  "%REPO%\third_party\minhook\src\hook.c" ^
  "%REPO%\third_party\minhook\src\trampoline.c" ^
  "%REPO%\third_party\minhook\src\hde\hde64.c" ^
  /Fe:"%OUT%" /Fo:"obj\\" ^
  /link /DLL /DEF:"%~dp0fakerFG.def" /MAP:"%~dp0fakerFG.map" d3d12.lib dxgi.lib user32.lib psapi.lib
if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK -^> %CD%\%OUT%
BAT

cd "$STAGE" || exit 1
cmd.exe /c build.bat "$REPO_WIN" "$first_src" "$NAME.dll" > build.log 2>&1
rc=$?

echo "[build] cl exit code: $rc"
echo "---------------- build.log ----------------"
iconv -f GBK -t UTF-8 -c build.log 2>/dev/null || cat build.log
echo "-------------------------------------------"

if [ -f "$STAGE/$NAME.dll" ]; then
    cp "$STAGE/$NAME.dll" "$REPO_WSL/build/"
    cp -f "$STAGE/fakerFG.map" "$REPO_WSL/build/$NAME.map" 2>/dev/null
    echo "[build] dll -> $REPO_WSL/build/$NAME.dll"
else
    echo "[build] NO DLL PRODUCED"
fi
exit $rc
