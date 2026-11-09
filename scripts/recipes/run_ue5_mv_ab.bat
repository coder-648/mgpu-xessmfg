@echo off
rem ===========================================================================
rem  run_ue5_mv_ab.bat  -  UE5 motion-vector A/B (Phantom Blade Zero testing)
rem
rem  Deploy to: <TOOLS_DIR>\run_ue5_mv_ab.bat   (next to bridge.exe + DLLs)
rem
rem  Usage:  run_ue5_mv_ab.bat [game-exe-name] [variant] [interp]
rem
rem    variant  what changes (one thing at a time)
rem      auto     (default) nothing extra - JITTERED_MV follows the game's DLSS flag
rem      jit      --fg-jittered-mv      force XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV
rem      nojit    --fg-no-jittered-mv   force it off (the clean control for 'jit')
rem      depthinv --fg-depth-inv        tell XeFG the depth is reversed-Z (UE5 is)
rem      jitdepth --fg-jittered-mv --fg-depth-inv
rem      mvscale  --fg-mv-scale px      multiply the game's MV.Scale by the MV texture
rem                                     size (for games that report MV.Scale = 1.0)
rem      nomv     --no-mv               no motion vectors at all (cheapest)
rem      dump                           5 frames of colour/depth/MV into dump_ue5\ for
rem                                     scripts/analyse_dump.py (MV IS tagged here,
rem                                     unlike "4 - dump frames.bat" which passes --no-mv)
rem      of       --fg-of               our own FidelityFX optical flow on the captured
rem                                     colour instead of the game's MV stream (0.3 ms);
rem                                     scale is the capture size (our field is UV)
rem      ofsign   --fg-of --fg-of-sign -1   same, with the flow sign flipped (A/B for the
rem                                     (previous - current) convention)
rem      offov    --fg-of --fg-fov 58.7   our flow + the FOV that used to be clean on PBZ
rem                                     (docs/findings/HANDOFF.md 0.1: 58.7 vs 55/62)
rem      jz       --fg-of --fg-jitter-zero   flow + jitter = 0 (our colour is the
rem                                     post-upscaler image, jitter already resolved)
rem      nodepth  --fg-of --no-depth     flow, no depth stream (does disocclusion own it?)
rem      only     --fg-of --fg-debug-only-interp  present ONLY generated frames, so the
rem                                     artifact can be seen without the real frames hiding it
rem
rem    interp   interpolated frames per real frame: 1 = 2x (default), 2 = 3x, 3 = 4x
rem             interp >= 2 adds --unlock-mfg automatically (non-Intel cap)
rem
rem  RECOMMENDED FOR UE5 (measured 2026-11-09, Phantom Blade Zero):
rem      in-game DLAA (or the highest quality preset) + `... of 1`
rem    That was the only combination with no model-edge artifacts. The game's own
rem    MV stream is broken (NaN/inf + fp16 saturation + no correlation with the real
rem    motion), and the residual artifacts at DLSS Performance came from the 3x
rem    magnification, not from frame generation - the un-interpolated game shows them
rem    too at that preset. See docs/findings/UE5_MV_OPTISCALER.md sections 7 and 8.
rem
rem  Pick interp so that input * (interp+1) lands exactly on the refresh rate:
rem      120 Hz: 60 fps -> 1 (2x) | 40 fps -> 2 (3x) | 30 fps -> 3 (4x)
rem  A wrong pair is visible as judder on its own and will mask every MV difference,
rem  so check the log line `cadence: ... NOT-EXACT` is absent before judging quality.
rem
rem  The add-on must produce the MV stream: game_capture.ini SkipMv=0, StreamHeight=1440.
rem  For the "did the add-on see the game's DLSS flags" question, set Log=1 there too.
rem
rem  Background and what to look for: docs/findings/UE5_MV_OPTISCALER.md section 5.
rem  Log: bridge_ue5_<variant>_i<interp>.txt in this folder.
rem ===========================================================================
setlocal
cd /d "%~dp0"

set "GAMEOPT="
if not "%~1"=="" set "GAMEOPT=--game %~1"

set "VARIANT=%~2"
if "%VARIANT%"=="" set "VARIANT=auto"

set "INTERP=%~3"
if "%INTERP%"=="" set "INTERP=1"

set "MVOPT="
if /i "%VARIANT%"=="auto"     set "MVOPT="
if /i "%VARIANT%"=="jit"      set "MVOPT=--fg-jittered-mv"
if /i "%VARIANT%"=="nojit"    set "MVOPT=--fg-no-jittered-mv"
if /i "%VARIANT%"=="depthinv" set "MVOPT=--fg-depth-inv"
if /i "%VARIANT%"=="jitdepth" set "MVOPT=--fg-jittered-mv --fg-depth-inv"
if /i "%VARIANT%"=="mvscale"  set "MVOPT=--fg-mv-scale px"
if /i "%VARIANT%"=="of"       set "MVOPT=--fg-of"
if /i "%VARIANT%"=="ofsign"   set "MVOPT=--fg-of --fg-of-sign -1"
if /i "%VARIANT%"=="offov"    set "MVOPT=--fg-of --fg-fov 58.7"
if /i "%VARIANT%"=="jz"       set "MVOPT=--fg-of --fg-jitter-zero"
if /i "%VARIANT%"=="nodepth"  set "MVOPT=--fg-of --no-depth"
if /i "%VARIANT%"=="only"     set "MVOPT=--fg-of --fg-debug-only-interp"
if /i "%VARIANT%"=="nomv"     set "MVOPT=--no-mv"

set "DUMPOPT="
rem The dump dir is passed without quotes: cmd cannot nest the quotes inside set "...",
rem and this folder name has no spaces. Quote it on the command line if yours does.
if /i "%VARIANT%"=="dump"     set "DUMPOPT=--dump 5 --dump-after 300 --dump-dir %CD%\dump_ue5"

if /i "%VARIANT%"=="auto"     goto known
if /i "%VARIANT%"=="jit"      goto known
if /i "%VARIANT%"=="nojit"    goto known
if /i "%VARIANT%"=="depthinv" goto known
if /i "%VARIANT%"=="jitdepth" goto known
if /i "%VARIANT%"=="mvscale"  goto known
if /i "%VARIANT%"=="of"       goto known
if /i "%VARIANT%"=="ofsign"   goto known
if /i "%VARIANT%"=="offov"    goto known
if /i "%VARIANT%"=="jz"       goto known
if /i "%VARIANT%"=="nodepth"  goto known
if /i "%VARIANT%"=="only"     goto known
if /i "%VARIANT%"=="nomv"     goto known
if /i "%VARIANT%"=="dump"     goto known
echo [mgpu] unknown variant "%VARIANT%" - use auto/jit/nojit/depthinv/jitdepth/mvscale/nomv/of/ofsign/offov/jz/nodepth/only/dump
pause
exit /b 1
:known

set "MFGOPT="
if not "%INTERP%"=="1" set "MFGOPT=--unlock-mfg"

if not exist bridge.exe (
  echo [mgpu] bridge.exe missing in %CD%
  pause
  exit /b 1
)

echo ============================================================
echo  mgpu bridge - UE5 MV A/B
echo  variant = %VARIANT%   interp = %INTERP%  (= %INTERP% interpolated per real frame)
echo  flags   = %MVOPT% %MFGOPT%
echo  Game must be running, in-world, and locked so that
echo  fps * (interp+1) == refresh rate (120 Hz: 60/40/30 fps).
echo  game_capture.ini needs SkipMv=0 and Log=1.
echo ============================================================
echo.

bridge.exe %GAMEOPT% --den 1 --pool 4 --interp %INTERP% %MVOPT% %MFGOPT% %DUMPOPT% ^
           --fg-ui none --fg-proj-rz --fg-rt-mode quant > bridge_ue5_%VARIANT%_i%INTERP%.txt 2>&1
type bridge_ue5_%VARIANT%_i%INTERP%.txt

echo.
echo ------------------------------------------------------------
echo  Output saved to %CD%\bridge_ue5_%VARIANT%_i%INTERP%.txt
echo  Look for: "dlss feature flags" / "initFlags=" / "mvscale=" /
echo            "cadence:" (must not say NOT-EXACT)
echo ------------------------------------------------------------
pause
