@echo off
rem ===========================================================================
rem  TEST 55 - 2x keep2x, OLD projection (--fg-proj-rz) - 2x control
rem  Compare with ab_54. Only the projection source changes.
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
set "LOG=%AB_LOG%\ab_55_camvm_rz_2x.log"
echo.
echo [AB] TEST 55 - 2x keep2x, OLD projection (--fg-proj-rz)
echo [AB] expect in log: proj=reversed-Z approximation (--fg-proj-rz)
echo [AB] log: %LOG%
echo [AB] in game: same spot/turning as ab_54; lock 60 fps
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
"%LF%" --game %GAME_NAME% --mode 2x --interp 1 --mfg-model=keep2x --fg-proj-rz %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
