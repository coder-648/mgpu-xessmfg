@echo off
rem ===========================================================================
rem  run_3x_nomv.bat  -  lowest cost: 3x with no motion vectors at all
rem
rem  Deploy to: <TOOLS_DIR>\run_3x_nomv.bat   (next to bridge.exe + DLLs)
rem
rem  Usage:  run_3x_nomv.bat [game-exe-name]      e.g.  run_3x_nomv.bat Game.exe
rem
rem  Why this one:
rem    --no-mv        neither the game's vectors nor optical flow are used; the
rem                   add-on stops converting and copying the MV stream, the
rem                   render GPU keeps its ~7 ms, and the frame-generation GPU
rem                   skips optical flow. Best frame budget.
rem    Cost: fast camera turns show dirtier edges than run_3x_of.bat. Everything
rem    else (depth, projection/FOV) makes no measurable difference in this
rem    build (docs/findings/CONCLUSIONS.md).
rem
rem  Before starting: game running, in-world, limiter locks it to 40 fps.
rem
rem  Log: bridge_3x_nomv.txt in this folder.
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
echo  mgpu bridge - 3x, no motion vectors (cheapest)
echo  Game must be running, in-world, locked to 40 fps.
echo ============================================================
echo.

bridge.exe %GAMEOPT% --den 1 --pool 4 --interp 2 --no-mv --input-ms 33.3 ^
           --unlock-mfg --fg-ui none --fg-proj-rz --fg-rt-mode median > bridge_3x_nomv.txt 2>&1
type bridge_3x_nomv.txt

echo.
echo ------------------------------------------------------------
echo  Output saved to %CD%\bridge_3x_nomv.txt
echo ------------------------------------------------------------
pause
