@echo off
setlocal
call "%~dp0ab_port_env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_35_keep2x_4x_failblack.log"
echo.
echo [AB] TEST 35 - 4x keep2x + BLACK on failed interpolation (control)
echo [AB] control: no black expected; proves the feature works
echo [AB] log: %LOG%
echo [AB] lock 30 fps for 4x, 40 fps for 3x; do NOT change the scene while judging
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
lock_frame.exe --game %GAME_NAME% --mode 4x --interp 3 --unlock-mfg --mfg-model=keep2x --fg-debug-fail-black %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
