@echo off
setlocal
call "%~dp0ab__env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_41_keep2x_4x_colour_shared.log"
set "CAP2=--streams=mv,depth,color --overlay-layered --wgc-minint 0 --pool 4 --latency 1 --fg-rt-mode auto --report 40"
echo.
echo [AB] TEST 41 - 4x keep2x, colour from the GAME back buffer (no WGC)
echo [AB] probe A1: run use_colour_v30.bat + restart first; then this. Colour comes from the producer
echo [AB] log: %LOG%
echo [AB] in game: borderless, DLSS on, lock 30 fps (120 Hz panel, 4x)
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
lock_frame.exe --game %GAME_NAME% --mode 4x --interp 3 --unlock-mfg --mfg-model=keep2x --fg-colour shared %CAP2% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
