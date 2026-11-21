@echo off
setlocal
call "%~dp0ab_port_env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_25_port_4x.log"
echo.
echo [AB] TEST 25 - 4x model 15 (port), depth on - same-scene compare with ab_24
echo [AB] expect: ported MFG model route active; one optical flow + 3 syntheses
echo [AB] log: %LOG%
echo [AB] in game: borderless, DLSS on, lock 30 fps (120 Hz panel, 4x)
echo [AB] same spot and same turning speed as the other runs
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
lock_frame.exe --game %GAME_NAME% --mode 4x --interp 3 --unlock-mfg --mfg-model=port %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
