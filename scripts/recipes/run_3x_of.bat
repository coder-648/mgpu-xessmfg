@echo off
rem ===========================================================================
rem  run_3x_of.bat  -  RECOMMENDED: 3x "full cadence" using AMD optical flow
rem
rem  Deploy to: <TOOLS_DIR>\run_3x_of.bat   (next to bridge.exe + the two DLLs)
rem
rem  Usage:  run_3x_of.bat [game-exe-name]          e.g.  run_3x_of.bat Game.exe
rem
rem  Why this one:
rem    --interp 2     3 interpolated frames per real frame (3x). With a 40 fps
rem                   lock that is exactly 120 fps on a 120 Hz panel.
rem    --fg-of        motion vectors come from AMD optical flow (FidelityFX),
rem                   computed on the frame-generation GPU from the captured
rem                   colour. Measured cost ~0.3 ms. The game's own vectors cost
rem                   +7.4 ms on the render GPU and make 3x flicker, because the
rem                   3x path reuses the 2x model for every generated frame
rem                   (docs/findings/FINAL_CONFIG.md 5.17 / 5.19).
rem    --input-ms 33.3  tells XeFG the real input interval is a whole vblank
rem                   longer than the theoretical (interp+1) x vblank. Without
rem                   it XeFG drops generated frames whenever throttle + present
rem                   overshoots by a hair (5.18.1).
rem    --unlock-mfg   lifts the non-Intel 2x clamp. See docs/ops/LEGAL.md before
rem                   you use it.
rem
rem  Before starting:
rem    1. game running and in-world
rem    2. an external limiter (RTSS) locks the game to 40 fps on a 120 Hz panel
rem    3. in-game frame generation OFF, upscaler DLSS / FSR / XeSS
rem
rem  Log: bridge_3x_of.txt in this folder.
rem ===========================================================================
setlocal
cd /d "%~dp0"

rem  %1 = the game's .exe name (OPTIONAL). Dragging the game's .exe onto this file
rem  fills it in. Without it, bridge targets whatever window is in the foreground -
rem  so click into the game first.
set "GAMEOPT="
if not "%~1"=="" set "GAMEOPT=--game %~1"

if not exist bridge.exe (
  echo [mgpu] bridge.exe missing in %CD%
  pause
  exit /b 1
)

echo ============================================================
echo  mgpu bridge - 3x via optical flow (40 fps in, 120 fps out)
echo  Game must be running, in-world, locked to 40 fps.
echo ============================================================
echo.

bridge.exe %GAMEOPT% --den 1 --pool 4 --interp 2 --fg-of --input-ms 33.3 ^
           --unlock-mfg --fg-ui none --fg-proj-rz --fg-rt-mode median > bridge_3x_of.txt 2>&1
type bridge_3x_of.txt

echo.
echo ------------------------------------------------------------
echo  Output saved to %CD%\bridge_3x_of.txt
echo ------------------------------------------------------------
pause
