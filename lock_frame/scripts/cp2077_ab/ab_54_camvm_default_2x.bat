@echo off
rem ===========================================================================
rem  TEST 54 - 2x keep2x, DEFAULT camera/view/projection
rem  The 2x case is the one that already matched the single-GPU result; use it
rem  to confirm the new view/projection does not disturb a known-good cadence.
rem ===========================================================================
setlocal
cd /d "%~dp0"
call "%~dp0ab__env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_54_camvm_default_2x.log"
echo.
echo [AB] TEST 54 - 2x keep2x, DEFAULT camera/view/projection (new)
echo [AB] expect in log: proj=game view-to-clip  view=camera  camera=1
echo [AB] log: %LOG%
echo [AB] in game: borderless, 4K DLAA, DLSS FG ON, lock 60 fps (120 Hz panel)
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
"%LF%" --game %GAME_NAME% --mode 2x --interp 1 --mfg-model=keep2x %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
