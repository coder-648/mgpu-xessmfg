@echo off
setlocal
call "%~dp0ab__env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_04_3x_far.log"
echo.
echo [AB] TEST 04 - 3x + finite far for infinite projections (--fg-far-finite auto)
echo [AB] log: %LOG%
echo [AB] in game: borderless, DLSS on, lock 40 fps (120 Hz panel)
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
lock_frame.exe --game %GAME_NAME% --mode 3x --interp 2 --unlock-mfg --mfg-model=keep2x --fg-far-finite auto %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
