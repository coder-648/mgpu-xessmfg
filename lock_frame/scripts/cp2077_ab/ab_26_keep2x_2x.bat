@echo off
setlocal
call "%~dp0ab_port_env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_26_keep2x_2x.log"
echo.
echo [AB] TEST 26 - 2x model 18 (keep2x) - reference for ab_27
echo [AB] expect: no unlock; lock 60 fps
echo [AB] log: %LOG%
echo [AB] in game: borderless, DLSS on, lock 60 fps (120 Hz panel, 2x)
echo [AB] same spot and same turning speed as the other runs
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
lock_frame.exe --game %GAME_NAME% --mode 2x --interp 1 %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
