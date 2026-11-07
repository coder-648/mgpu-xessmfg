@echo off
rem ===========================================================================
rem  Runs the XeSS-FG bridge. Start the game FIRST and get in-world, then
rem  double-click this. A "MGPU Bridge" window appears and presents the
rem  frame-generated output; the console keeps a short log.
rem
rem  Close the console or press Ctrl+C to stop.
rem ===========================================================================
setlocal
cd /d "%~dp0"

if not exist bridge.exe (
  echo [bridge] bridge.exe missing in %CD%
  pause
  exit /b 1
)

echo ============================================================
echo  Starting XeSS-FG bridge (second GPU)...
echo  A "MGPU Bridge" window will appear. Game must be in-world.
echo ============================================================
echo.

bridge.exe %* > bridge_fg.txt 2>&1
type bridge_fg.txt

echo.
echo ------------------------------------------------------------
echo  Output saved to %CD%\bridge_fg.txt
echo ------------------------------------------------------------
pause
