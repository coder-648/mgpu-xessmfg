@echo off
setlocal
call "%~dp0ab_port_env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_39_port_4x_capacity4.log"
echo.
echo [AB] TEST 39 - 4x port with capacity 4 and count 3 (--interp 4 --interp-start 3)
echo [AB] if this is cleaner, the 3-synthesis path needs a larger capacity pipeline
echo [AB] log: %LOG%
echo [AB] in game: borderless, DLSS on, lock 30 fps (120 Hz panel, 4x)
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
lock_frame.exe --game %GAME_NAME% --interp 4 --interp-start 3 --unlock-mfg --mfg-model=port %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
