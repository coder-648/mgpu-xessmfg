@echo off
setlocal
call "%~dp0ab_port_env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_11_port_3x_far.log"
echo.
echo [AB] TEST 11 - 3x ported model + finite far
echo [AB] expect: near/far zn=0.020 zf=75000 (derived ...)
echo [AB] log: %LOG%
echo [AB] in game: borderless, DLSS on, lock 40 fps (120 Hz panel)
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
lock_frame.exe --game %GAME_NAME% --mode 3x --interp 2 --unlock-mfg --mfg-model=port --fg-far-finite auto %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
