@echo off
setlocal
call "%~dp0ab__env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_42_keep2x_4x_upscaler_mv.log"
set "CAP2=--streams=mv,depth --overlay-layered --wgc-minint 0 --pool 4 --latency 1 --fg-rt-mode auto --report 40"
echo.
echo [AB] TEST 42 - 4x keep2x, MV/depth from the UPSCALER lane
echo [AB] probe B1: run use_upscaler_mv.bat + restart first; compare MV quality with ab_24 (DLSSG lane)
echo [AB] log: %LOG%
echo [AB] in game: borderless, DLSS on, lock 30 fps (120 Hz panel, 4x)
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
lock_frame.exe --game %GAME_NAME% --mode 4x --interp 3 --unlock-mfg --mfg-model=keep2x %CAP2% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
