@echo off
rem ===========================================================================
rem  TEST 51 - 4x keep2x, OLD projection (--fg-proj-rz)
rem  Same command line as ab_50; only the projection source differs. This is
rem  the reverse-Z synthetic matrix the builds before 2026-11-25 used.
rem ===========================================================================
setlocal
cd /d "%~dp0"
call "%~dp0ab__env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_51_camvm_rz_4x.log"
echo.
echo [AB] TEST 51 - 4x keep2x, OLD projection (--fg-proj-rz)
echo [AB] expect in log: proj=reversed-Z approximation (--fg-proj-rz)  view=camera
echo [AB] log: %LOG%
echo [AB] in game: same spot/turning as ab_50; lock 30 fps
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
"%LF%" --game %GAME_NAME% --mode 4x --interp 3 --unlock-mfg --mfg-model=keep2x --fg-proj-rz %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
