@echo off
rem ===========================================================================
rem  TEST 53 - 3x keep2x, DEFAULT camera/view/projection
rem  Cross-check at the middle multiplier (port model 15 was better at 3x, but
rem  this round stays on keep2x so the only variable is the input alignment).
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
set "LOG=%AB_LOG%\ab_53_camvm_default_3x.log"
echo.
echo [AB] TEST 53 - 3x keep2x, DEFAULT camera/view/projection (new)
echo [AB] expect in log: proj=game view-to-clip  view=camera  camera=1
echo [AB] log: %LOG%
echo [AB] in game: borderless, 4K DLAA, DLSS FG ON, lock 40 fps (120 Hz panel)
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
"%LF%" --game %GAME_NAME% --mode 3x --interp 2 --unlock-mfg --mfg-model=keep2x %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
