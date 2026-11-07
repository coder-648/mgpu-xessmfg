@echo off
rem ===========================================================================
rem  run_2x_quality.bat  -  2x, best edges (game's own motion vectors + jitter)
rem
rem  Deploy to: <TOOLS_DIR>\run_2x_quality.bat   (next to bridge.exe + DLLs)
rem
rem  Usage:  run_2x_quality.bat [game-exe-name]   e.g.  run_2x_quality.bat Game.exe
rem
rem  Why this one:
rem    --interp 1      one interpolated frame per real frame (2x).
rem    no --no-mv      the game's own motion vectors are tagged. At 2x this is
rem                    the cleanest edge result - the vectors describe what the
rem                    game actually moved.
rem    no --input-ms   the 2x path does not drop frames the way 3x does; leave
rem                    the interval at the measured one unless you see drops.
rem    jitter          the game's own jitter is used automatically - do NOT pass
rem                    --fg-jitter-zero (deprecated, prints a notice and does
rem                    nothing now).
rem
rem  The add-on side must produce the MV stream: game_capture.ini SkipMv=0.
rem
rem  Before starting:
rem    1. game running and in-world
rem    2. limiter locks the game to half the panel rate (60 fps on 120 Hz)
rem    3. in-game frame generation OFF, upscaler DLSS / FSR / XeSS
rem
rem  Log: bridge_2x_quality.txt in this folder.
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
echo  mgpu bridge - 2x quality (60 fps in, 120 fps out)
echo  Game must be running, in-world, locked to 60 fps.
echo  game_capture.ini needs SkipMv=0.
echo ============================================================
echo.

bridge.exe %GAMEOPT% --den 1 --pool 4 --interp 1 ^
           --fg-ui none --fg-proj-rz --fg-rt-mode median > bridge_2x_quality.txt 2>&1
type bridge_2x_quality.txt

echo.
echo ------------------------------------------------------------
echo  Output saved to %CD%\bridge_2x_quality.txt
echo ------------------------------------------------------------
pause
