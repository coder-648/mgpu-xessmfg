@echo off
rem ===========================================================================
rem  run_dump.bat  -  capture N frames of colour / depth / MV for offline analysis
rem
rem  Usage:  run_dump.bat [game-exe-name] [frames] [dump-dir]
rem      e.g. run_dump.bat Game.exe 5 D:\mgpu_dump
rem
rem  The dump stalls each dumped frame on purpose (it waits for the GPU), so the
rem  gpu / show numbers of such a run are meaningless - only the data is.
rem
rem  Afterwards, in WSL:
rem      ./scripts/analyse_dump.sh <dump-dir>
rem
rem  IMPORTANT for a meaningful pairing test: the picture must contain motion
rem  whose SPEED CHANGES (walk/pan and let the speed vary). With constant motion
rem  "one frame behind" and "one frame ahead" look identical.
rem
rem  Useful pairings (see docs/findings/QUALITY_VARIABLES.md):
rem    - game MV vs optical flow: run once with SkipMv=0 (MV tagged), once with
rem      --fg-of, and compare f<NN>_mv.raw pixel by pixel.
rem
rem  Setup: game running, in-world, upscaler on, frame generation off.
rem  Log: bridge_dump.txt in this folder.
rem ===========================================================================
setlocal
cd /d "%~dp0"

rem  %1 = the game's .exe name (OPTIONAL; drag the .exe here to fill it in)
rem  %2 = how many frames to dump (default 5)
rem  %3 = output folder (default <this folder>\dump)
rem  Without %1, bridge targets whatever window is in the foreground.
set "GAMEOPT="
if not "%~1"=="" set "GAMEOPT=--game %~1"
set "FRAMES=%~2"
if "%FRAMES%"=="" set "FRAMES=5"
set "DUMPDIR=%~3"
if "%DUMPDIR%"=="" set "DUMPDIR=%CD%\dump"

if not exist bridge.exe (
  echo [mgpu] bridge.exe missing in %CD%
  pause
  exit /b 1
)

echo ============================================================
echo  mgpu bridge - dump %FRAMES% frames to %DUMPDIR%
echo  Game must be running and MOVING.
echo ============================================================
echo.

bridge.exe %GAMEOPT% --den 1 --pool 4 --interp 2 --no-mv --unlock-mfg --fg-ui none ^
           --dump %FRAMES% --dump-after 300 --dump-dir "%DUMPDIR%" > bridge_dump.txt 2>&1
type bridge_dump.txt

echo.
echo ------------------------------------------------------------
echo  Look for the "dump 0..%FRAMES%" lines above, then run
echo    ./scripts/analyse_dump.sh "%DUMPDIR%"
echo ------------------------------------------------------------
pause

