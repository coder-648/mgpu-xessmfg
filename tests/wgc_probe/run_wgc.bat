@echo off
rem ===========================================================================
rem  Runs the WGC capture probe for 20 s against the running game and keeps the
rem  output. Start Cyberpunk FIRST, get in-world, then double-click this.
rem  It only reads the composed window - it does not touch the game.
rem ===========================================================================
setlocal
cd /d "%~dp0"

if not exist wgc_probe.exe (
  echo [wgc] wgc_probe.exe missing in %CD%
  pause
  exit /b 1
)

echo ============================================================
echo  Capturing the game window for 20 s...
echo ============================================================
echo.

wgc_probe.exe 20 > wgc_run.txt 2>&1
type wgc_run.txt

echo.
echo ------------------------------------------------------------
echo  Output saved to %CD%\wgc_run.txt
echo ------------------------------------------------------------
pause
