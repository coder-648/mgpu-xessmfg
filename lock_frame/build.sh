#!/usr/bin/env bash
# ===========================================================================
#  lock_frame/ 独立构建脚本
#
#    ./lock_frame/build.sh            -> build/lock_frame.exe
#    ./lock_frame/build.sh all        -> 再构建 producer / fakerFG / ui
#
#  源文件全部在 lock_frame/ 下，不会写回 src/ 或 scripts/。
# ===========================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
STAGE_ROOT="${MGPU_STAGE:-/mnt/c/temp/mgpu}"
REPO_WIN="$(wslpath -w "$REPO")"
FXC_WIN="${MGPU_FXC:-/mnt/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe}"

WHAT="${1:-consumer}"

build_consumer() {
    local stage="$STAGE_ROOT/lock_frame"
    rm -rf "$stage"
    mkdir -p "$stage" "$REPO/build"
    cp "$HERE/lock_frame.cpp" "$stage/"
    cp "$HERE/lock_frame.rc" "$stage/"
    cp "$HERE/build.bat" "$stage/"
    cp "$HERE/osd_panel.h" "$stage/"
    cp "$HERE/version.h" "$stage/"
    cp "$HERE/include/mgpu_shared.h" "$stage/"     # v28: kMaxSlots=32
    cp -r "$HERE/modules" "$stage/"
    cp -r "$HERE/xefg_mfg" "$stage/"
    [ -d "$REPO/build/xefg_programs" ] && cp -r "$REPO/build/xefg_programs" "$stage/"
    for dll in libxess_fg.dll libxell.dll; do
        cp "$REPO/third_party/xess/bin/$dll" "$stage/" 2>/dev/null || true
    done

    [ -x "$FXC_WIN" ] || { echo "[build] fxc.exe not found"; return 1; }
    "$FXC_WIN" -nologo -T cs_5_0 -E CSMain \
        -Fo "$(wslpath -w "$stage/upsample_joint.cso")" \
        "$(wslpath -w "$HERE/shaders/upsample_joint.hlsl")" \
        || { echo "[build] fxc failed for upsample_joint"; return 1; }
    python3 "$REPO/scripts/embed_cso.py" "$stage/upsample_joint.cso" \
        "$stage/upsample_joint_cso.h" "UpsampleJoint" \
        || { echo "[build] embed failed"; return 1; }

    # Lossless MV decode (docs/design/LOSSLESS_TRANSPORT.md sections 6.1/14.3/17).
    # Encoder lives in the producer; the consumer only needs Decode - except for
    # --mv-verify, which re-encodes the decoded MV to prove the round trip is exact
    # (encode(decode(bytes)) == bytes  =>  decode(bytes) == source, the encoder being
    # injective), so the Encode entry is embedded here too.
    "$FXC_WIN" -nologo -T cs_5_0 -E Decode \
        -Fo "$(wslpath -w "$stage/fr_decode.cso")" \
        "$(wslpath -w "$HERE/shaders/fr_codec.hlsl")" \
        || { echo "[build] fxc failed for fr_codec Decode"; return 1; }
    python3 "$REPO/scripts/embed_cso.py" "$stage/fr_decode.cso" \
        "$stage/fr_decode_cso.h" "FrDecode" \
        || { echo "[build] embed failed for fr_decode"; return 1; }
    "$FXC_WIN" -nologo -T cs_5_0 -E Encode \
        -Fo "$(wslpath -w "$stage/fr_encode.cso")" \
        "$(wslpath -w "$HERE/shaders/fr_codec.hlsl")" \
        || { echo "[build] fxc failed for fr_codec Encode"; return 1; }
    python3 "$REPO/scripts/embed_cso.py" "$stage/fr_encode.cso" \
        "$stage/fr_encode_cso.h" "FrEncode" \
        || { echo "[build] embed failed for fr_encode"; return 1; }
    # D32 variants (depth: one 32-bit value per pixel) - same body, -DDEPTH32.
    "$FXC_WIN" -nologo -T cs_5_0 -E Decode -D DEPTH32 \
        -Fo "$(wslpath -w "$stage/fr_decode32.cso")" \
        "$(wslpath -w "$HERE/shaders/fr_codec.hlsl")" \
        || { echo "[build] fxc failed for fr_codec Decode (D32)"; return 1; }
    python3 "$REPO/scripts/embed_cso.py" "$stage/fr_decode32.cso" \
        "$stage/fr_decode32_cso.h" "FrDecodeD32" \
        || { echo "[build] embed failed for fr_decode32"; return 1; }
    "$FXC_WIN" -nologo -T cs_5_0 -E Encode -D DEPTH32 \
        -Fo "$(wslpath -w "$stage/fr_encode32.cso")" \
        "$(wslpath -w "$HERE/shaders/fr_codec.hlsl")" \
        || { echo "[build] fxc failed for fr_codec Encode (D32)"; return 1; }
    python3 "$REPO/scripts/embed_cso.py" "$stage/fr_encode32.cso" \
        "$stage/fr_encode32_cso.h" "FrEncodeD32" \
        || { echo "[build] embed failed for fr_encode32"; return 1; }
    # UI variants (P2): typed R10G10B10A2 / RGBA8 pixels packed to one u32 per pixel.
    for spec in "UIFMT_R10:fr_decode_ui_r10:FrDecodeUiR10" \
                "UIFMT_RGBA8:fr_decode_ui_rgba8:FrDecodeUiRgba8"; do
        IFS=: read -r def out sym <<< "$spec"
        "$FXC_WIN" -nologo -T cs_5_0 -E Decode -D "$def" \
            -Fo "$(wslpath -w "$stage/$out.cso")" \
            "$(wslpath -w "$HERE/shaders/fr_codec.hlsl")" \
            || { echo "[build] fxc failed for $out"; return 1; }
        python3 "$REPO/scripts/embed_cso.py" "$stage/$out.cso" \
            "$stage/${out}_cso.h" "$sym" \
            || { echo "[build] embed failed for $out"; return 1; }
    done

    python3 "$REPO/scripts/extract_hlsl.py" "$HERE/modules/050_shaders.inc" \
        "$stage/conv_check.hlsl" || { echo "[build] could not extract k_conv_hlsl"; return 1; }
    for entry in vs_main:vs_5_0 ps_main:ps_5_0 ps_prep_mv:ps_5_0 ps_prep_depth:ps_5_0; do
        local name="${entry%%:*}" prof="${entry##*:}"
        "$FXC_WIN" -nologo -T "$prof" -E "$name" \
            -Fo "$(wslpath -w "$stage/conv_check.cso")" \
            "$(wslpath -w "$stage/conv_check.hlsl")" \
            || { echo "[build] fxc failed for $name"; return 1; }
    done

    "$FXC_WIN" -nologo -T cs_5_0 -E CSMain \
        -Fo "$(wslpath -w "$stage/unblend_ui.cso")" \
        "$(wslpath -w "$HERE/shaders/unblend_ui.hlsl")" \
        || { echo "[build] fxc failed for unblend_ui"; return 1; }
    python3 "$REPO/scripts/embed_cso.py" "$stage/unblend_ui.cso" \
        "$stage/unblend_ui_cso.h" "UnblendUI" \
        || { echo "[build] embed failed for unblend_ui"; return 1; }

    # v29: HUDless-derived UI synthesis (present - hudless) before the unblend pass.
    "$FXC_WIN" -nologo -T cs_5_0 -E CSMain \
        -Fo "$(wslpath -w "$stage/synth_ui.cso")" \
        "$(wslpath -w "$HERE/shaders/synth_ui.hlsl")" \
        || { echo "[build] fxc failed for synth_ui"; return 1; }
    python3 "$REPO/scripts/embed_cso.py" "$stage/synth_ui.cso" \
        "$stage/synth_ui_cso.h" "SynthUI" \
        || { echo "[build] embed failed for synth_ui"; return 1; }

    # v33: dynamic reference-white calibration for the HUDless patch (mode 4).
    # v34: patch_local / patch_local_fill add the per-32x8-tile spatial tone residual.
    for spec in "patch_calib:cs_5_0:CSMain:PatchCalib" \
                "patch_gain:cs_5_0:CSMain:PatchGain" \
                "patch_local:cs_5_0:CSMain:PatchLocal" \
                "patch_local_fill:cs_5_0:CSMain:PatchLocalFill" \
                "patch_lut3d:cs_5_0:CSMain:PatchLut3D" \
                "patch_lut3d_fill:cs_5_0:CSMain:PatchLut3dFill"; do
        IFS=: read -r base prof entry sym <<< "$spec"
        "$FXC_WIN" -nologo -T "$prof" -E "$entry" \
            -Fo "$(wslpath -w "$stage/$base.cso")" \
            "$(wslpath -w "$HERE/shaders/$base.hlsl")" \
            || { echo "[build] fxc failed for $base"; return 1; }
        python3 "$REPO/scripts/embed_cso.py" "$stage/$base.cso" \
            "$stage/${base}_cso.h" "$sym" \
            || { echo "[build] embed failed for $base"; return 1; }
    done

    # Sparse unblend: per-tile UI occupancy derived from the codec headers (no protocol
    # change). One 4-byte word per 32x8 tile; the unblend writes `present` for empty tiles
    # instead of sampling UI and running the PQ math.
    "$FXC_WIN" -nologo -T cs_5_0 -E CSMain \
        -Fo "$(wslpath -w "$stage/ui_occ.cso")" \
        "$(wslpath -w "$HERE/shaders/ui_occ.hlsl")" \
        || { echo "[build] fxc failed for ui_occ"; return 1; }
    python3 "$REPO/scripts/embed_cso.py" "$stage/ui_occ.cso" \
        "$stage/ui_occ_cso.h" "UiOcc" \
        || { echo "[build] embed failed for ui_occ"; return 1; }

    ( cd "$stage" && cmd.exe /c build.bat "$REPO_WIN" > build.log 2>&1 )
    local rc=$?
    echo "[build] cl exit code: $rc"
    iconv -f GBK -t UTF-8 -c "$stage/build.log" 2>/dev/null || cat "$stage/build.log"
    if [ -f "$stage/lock_frame.exe" ]; then
        cp "$stage/lock_frame.exe" "$REPO/build/"
        echo "[build] exe -> $REPO/build/lock_frame.exe"
    else
        echo "[build] NO EXE PRODUCED"; return 1
    fi
    return $rc
}

build_producer() {
    local stage="$STAGE_ROOT/lock_frame_producer"
    rm -rf "$stage"; mkdir -p "$stage" "$REPO/build"
    cp "$HERE/producer"/*.cpp "$HERE/producer"/*.h "$stage/"
    [ -x "$FXC_WIN" ] || { echo "[build] fxc.exe not found"; return 1; }
    for spec in "downsample_mv:cs_5_0:CSMain:DownsampleMV:downsample_mv" \
                "downsample_depth:cs_5_0:CSMain:DownsampleDepth:downsample_depth" \
                "downsample_ui:cs_5_0:CSMain:DownsampleUI:downsample_ui" \
                "synth_ui_full:cs_5_0:CSMain:SynthUiFull:synth_ui_full" \
                "fr_codec:cs_5_0:Encode:FrEncode:fr_encode" \
                "fr_codec:cs_5_0:Decode:FrDecode:fr_decode" \
                "fr_codec:cs_5_0:Encode:FrEncodeD32:fr_encode32:-D DEPTH32" \
                "fr_codec:cs_5_0:Encode:FrEncodeD32Fp16:fr_encode32_fp16:-D DEPTH32 -D DEPTH_FP16" \
                "fr_codec:cs_5_0:Decode:FrDecodeD32:fr_decode32:-D DEPTH32" \
                "fr_codec:cs_5_0:Encode:FrEncodeUiR10:fr_encode_ui_r10:-D UIFMT_R10" \
                "fr_codec:cs_5_0:Encode:FrEncodeUiRgba8:fr_encode_ui_rgba8:-D UIFMT_RGBA8" \
                "fr_codec:cs_5_0:Encode:FrEncodeUiR10Sparse:fr_encode_ui_r10_sparse:-D UIFMT_R10 -D UI_SPARSE" \
                "fr_codec:cs_5_0:Encode:FrEncodeUiRgba8Sparse:fr_encode_ui_rgba8_sparse:-D UIFMT_RGBA8 -D UI_SPARSE" \
                "fr_codec:cs_5_0:Encode:FrEncodeAblateScan:fr_encode_ablate_scan:-D ABLATE_SCAN" \
                "fr_codec:cs_5_0:Encode:FrEncodeAblateAtomics:fr_encode_ablate_atomics:-D ABLATE_ATOMICS" \
                "fr_codec:cs_5_0:Encode:FrEncodeAblateLoads:fr_encode_ablate_loads:-D ABLATE_LOADS"; do
        IFS=: read -r base prof entry sym out extra <<< "$spec"
        # fr_codec is shared with the consumer, so it lives in lock_frame/shaders/
        local hlsl="$HERE/producer/shaders/$base.hlsl"
        [ "$base" = "fr_codec" ] && hlsl="$HERE/shaders/fr_codec.hlsl"
        "$FXC_WIN" -nologo -T "$prof" -E "$entry" $extra \
            -Fo "$(wslpath -w "$stage/$out.cso")" \
            "$(wslpath -w "$hlsl")" || return 1
        python3 "$REPO/scripts/embed_cso.py" "$stage/$out.cso" "$stage/${out}_cso.h" "$sym" || return 1
    done
    cat > "$stage/build.bat" <<'BAT'
@echo off
setlocal
cd /d "%~dp0"
set "REPO=%~1"
if not defined MGPU_VSDIR set "MGPU_VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "VSDIR=%MGPU_VSDIR%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist obj mkdir obj
cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- ^
  /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /I"%~dp0." /I"%REPO%\src" /I"%REPO%\src\faker_fg" ^
  "%~dp0producer.cpp" /Fe:xess_mfg_producer.dll /Fo:obj\ ^
  /link /DLL /MAP:xess_mfg_producer.map d3d12.lib dxgi.lib
if errorlevel 1 exit /b 1
exit /b 0
BAT
    ( cd "$stage" && cmd.exe /c build.bat "$REPO_WIN" > build.log 2>&1 )
    local rc=$?
    iconv -f GBK -t UTF-8 -c "$stage/build.log" 2>/dev/null || cat "$stage/build.log"
    [ -f "$stage/xess_mfg_producer.dll" ] && cp "$stage/xess_mfg_producer.dll" "$REPO/build/" && \
        echo "[build] dll -> $REPO/build/xess_mfg_producer.dll"
    return $rc
}

build_faker() {
    local stage="$STAGE_ROOT/lock_frame_faker"
    rm -rf "$stage"; mkdir -p "$stage" "$REPO/build"
    cp "$HERE/faker_fg"/*.cpp "$HERE/faker_fg"/*.h "$HERE/faker_fg"/*.def "$stage/"
    [ -x "$FXC_WIN" ] || { echo "[build] fxc.exe not found"; return 1; }
    for spec in "mv_resample:cs_5_0:CSMain:MvResample" \
                "render_ui:vs_5_0:VSMain:RenderUiVS" \
                "render_ui:ps_5_0:PSMain:RenderUiPS" \
                "render_ui_pm:ps_5_0:PSMain:RenderUiPmPS" \
                "hudless_compare:ps_5_0:PSMain:HudlessComparePS" \
                "hud_copy:cs_5_0:CSMain:HudCopy" \
                "hudless_to_pq:cs_5_0:CSMain:HudlessToPQ" \
                "marker:ps_5_0:PSMain:MarkerPS"; do
        IFS=: read -r base prof entry sym <<< "$spec"
        local out="${base}_${entry}"
        "$FXC_WIN" -nologo -T "$prof" -E "$entry" \
            -Fo "$(wslpath -w "$stage/$out.cso")" \
            "$(wslpath -w "$HERE/faker_fg/shaders/$base.hlsl")" || return 1
        python3 "$REPO/scripts/embed_cso.py" "$stage/$out.cso" "$stage/${out}_cso.h" "$sym" || return 1
    done
    cat > "$stage/build.bat" <<'BAT'
@echo off
setlocal
cd /d "%~dp0"
set "REPO=%~1"
if not defined MGPU_VSDIR set "MGPU_VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "VSDIR=%MGPU_VSDIR%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist obj mkdir obj
cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- ^
  /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /I"%~dp0." /I"%REPO%\src" /I"%REPO%\src\faker_fg" ^

  /I"%REPO%\third_party\xess\inc" /I"%REPO%\third_party\streamline\include" ^
  /I"%REPO%\\third_party\\ffx\\api\\include" ^
  /I"%REPO%\\third_party\\ffx\\include" ^
  /I"%REPO%\third_party\dlss\include" /I"%REPO%\third_party\minhook\include" ^
  /I"%REPO%\third_party\minhook\src" ^
  /I"%REPO%\lock_frame\third_party" ^
  unity.cpp "%REPO%\third_party\minhook\src\buffer.c" "%REPO%\third_party\minhook\src\hook.c" ^
  "%REPO%\third_party\minhook\src\trampoline.c" "%REPO%\third_party\minhook\src\hde\hde64.c" ^
  /Fe:fakerFG.dll /Fo:obj\ /link /DLL /DEF:fakerFG.def /MAP:fakerFG.map d3d12.lib dxgi.lib user32.lib psapi.lib ole32.lib
if errorlevel 1 exit /b 1
exit /b 0
BAT
    ( cd "$stage" && cmd.exe /c build.bat "$REPO_WIN" > build.log 2>&1 )
    local rc=$?
    iconv -f GBK -t UTF-8 -c "$stage/build.log" 2>/dev/null || cat "$stage/build.log"
    [ -f "$stage/fakerFG.dll" ] && cp "$stage/fakerFG.dll" "$REPO/build/" && \
        echo "[build] dll -> $REPO/build/fakerFG.dll"
    return $rc
}

build_ui() {
    local stage="$STAGE_ROOT/lock_frame_ui"
    rm -rf "$stage"; mkdir -p "$stage" "$REPO/build"
    cp "$HERE/ui/mgpu_ui.cpp" "$stage/lock_frame_ui.cpp"
    cp "$HERE/ui/lock_frame_ui.rc" "$stage/"
    cp "$HERE/include/mgpu_shared.h" "$stage/"     # v28: shared protocol check
    cp "$HERE/version.h" "$stage/"
    cat > "$stage/build.bat" <<'BAT'
@echo off
setlocal
cd /d "%~dp0"
set "REPO=%~1"
if not defined MGPU_VSDIR set "MGPU_VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "VSDIR=%MGPU_VSDIR%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
rc /nologo /fo "%~dp0lock_frame_ui.res" "%~dp0lock_frame_ui.rc"
if errorlevel 1 ( echo [build] rc failed & exit /b 1 )
cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- ^
  /DUNICODE /D_UNICODE /I"%~dp0." /I"%REPO%\src" "%~dp0lock_frame_ui.cpp" "%~dp0lock_frame_ui.res" ^
  /Fe:lock_frame_ui.exe /Fo:lock_frame_ui.obj ^
  /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib advapi32.lib dwmapi.lib
if errorlevel 1 exit /b 1
exit /b 0
BAT
    ( cd "$stage" && cmd.exe /c build.bat "$REPO_WIN" > build.log 2>&1 )
    local rc=$?
    iconv -f GBK -t UTF-8 -c "$stage/build.log" 2>/dev/null || cat "$stage/build.log"
    [ -f "$stage/lock_frame_ui.exe" ] && cp "$stage/lock_frame_ui.exe" "$REPO/build/" && \
        echo "[build] exe -> $REPO/build/lock_frame_ui.exe"
    return $rc
}

case "$WHAT" in
  consumer) build_consumer ;;
  producer) build_producer ;;
  faker)    build_faker ;;
  ui)       build_ui ;;
  all)
     build_producer; r1=$?
     build_faker;    r2=$?
     build_ui;       r3=$?
     build_consumer; r4=$?
     echo "[build] producer=$r1 faker=$r2 ui=$r3 consumer=$r4"
     exit $((r1|r2|r3|r4)) ;;
  *) echo "usage: $0 [consumer|producer|faker|ui|all]"; exit 2 ;;
esac
