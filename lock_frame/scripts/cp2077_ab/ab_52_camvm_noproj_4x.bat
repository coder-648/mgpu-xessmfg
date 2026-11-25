@echo off
rem ===========================================================================
rem  TEST 52 - 4x keep2x, NO projection (--fg-no-proj, identity) - control
rem  If 50 and 52 look identical, the projection path is not being read at all
rem  (expected for model 18); if 50 differs from 52, the projection matters.
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
set "LOG=%AB_LOG%\ab_52_camvm_noproj_4x.log"
echo.
echo [AB] TEST 52 - 4x keep2x, identity projection (--fg-no-proj) - control
echo [AB] expect in log: proj=identity (--fg-no-proj)
echo [AB] log: %LOG%
echo [AB] in game: same spot/turning as ab_50; lock 30 fps
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
"%LF%" --game %GAME_NAME% --mode 4x --interp 3 --unlock-mfg --mfg-model=keep2x --fg-no-proj %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
