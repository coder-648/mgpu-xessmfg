@echo off
setlocal
call "%~dp0ab_port_env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_12_port_3x_nodepth.log"
echo.
echo [AB] TEST 12 - 3x ported model + NO depth stream
echo [AB] depth probe: if the picture breaks, model 15 reads depth (that is a valid answer)
echo [AB] log: %LOG%
echo [AB] in game: borderless, DLSS on, lock 40 fps (120 Hz panel)
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
lock_frame.exe --game %GAME_NAME% --mode 3x --interp 2 --unlock-mfg --mfg-model=port --no-depth %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
