@echo off
setlocal
call "%~dp0ab__env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_40_keep2x_4x_tearing.log"
set "CAP2=--streams=mv,depth --wgc-minint 0 --pool 4 --latency 1 --fg-rt-mode auto --report 40"
echo.
echo [AB] TEST 40 - 4x keep2x + ALLOW_TEARING presentation
echo [AB] presentation probe: does the DWM/compositor path change the perceived edges? (no restart)
echo [AB] log: %LOG%
echo [AB] in game: borderless, DLSS on, lock 30 fps (120 Hz panel, 4x)
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
lock_frame.exe --game %GAME_NAME% --mode 4x --interp 3 --unlock-mfg --mfg-model=keep2x --tearing %CAP2% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
