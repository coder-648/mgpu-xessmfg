@echo off
rem ===========================================================================
rem  Runs the M4 consumer probe for 30 seconds and keeps the output.
rem  Start the game FIRST (and get in-world), then double-click this.
rem  It does not touch the game: it only reads the shared mapping.
rem ===========================================================================
setlocal
cd /d "%~dp0"

if not exist bridge_probe.exe (
  echo [bridge] bridge_probe.exe missing in %CD%
  pause
  exit /b 1
)

echo ============================================================
echo  Waiting for the game's shared block, then receiving 30 s.
echo  Keep the game running and in-world.
echo ============================================================
echo.

bridge_probe.exe 30 > bridge_run.txt 2>&1
type bridge_run.txt

echo.
echo ------------------------------------------------------------
echo  Output saved to %CD%\bridge_run.txt
echo ------------------------------------------------------------
pause
