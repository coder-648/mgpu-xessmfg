@echo off
setlocal
call "%~dp0ab_port_env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_27_port_2x.log"
echo.
echo [AB] TEST 27 - 2x model 15 (port) at capacity 3x -> count 1
echo [AB] expect: --interp-start: applying 2x once (capacity 3x), then ported MFG model route
echo [AB] log: %LOG%
echo [AB] in game: borderless, DLSS on, lock 60 fps (120 Hz panel, 2x)
echo [AB] same spot and same turning speed as the other runs
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
lock_frame.exe --game %GAME_NAME% --interp 2 --interp-start 1 --unlock-mfg --mfg-model=port %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
