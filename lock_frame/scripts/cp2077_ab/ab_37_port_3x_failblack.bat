@echo off
setlocal
call "%~dp0ab_port_env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_37_port_3x_failblack.log"
echo.
echo [AB] TEST 37 - 3x port + BLACK on failed interpolation (control)
echo [AB] control: no black expected if port works at 2 syntheses
echo [AB] log: %LOG%
echo [AB] lock 30 fps for 4x, 40 fps for 3x; do NOT change the scene while judging
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
lock_frame.exe --game %GAME_NAME% --mode 3x --interp 2 --unlock-mfg --mfg-model=port --fg-debug-fail-black %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
