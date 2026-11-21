@echo off
setlocal
call "%~dp0ab__env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_01_2x_baseline.log"
echo.
echo [AB] TEST 01 - 2x baseline (control: no new flags)
echo [AB] log: %LOG%
echo [AB] in game: borderless, DLSS on, lock 60 fps (120 Hz panel)
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
lock_frame.exe --game %GAME_NAME% --mode 2x --interp 1 %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
