@echo off
rem ===========================================================================
rem  TEST 50 - 4x keep2x, DEFAULT input alignment (2026-11-25)
rem  New default: projection = the game's own view_to_clip, viewMatrix = camera
rem  basis from sl::Constants (OptiScaler-equivalent). This is the reference;
rem  ab_51 / ab_52 are the old-projection and identity controls.
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
set "LOG=%AB_LOG%\ab_50_camvm_default_4x.log"
echo.
echo [AB] TEST 50 - 4x keep2x, DEFAULT camera/view/projection (new)
echo [AB] expect in log: proj=game view-to-clip  view=camera  camera=1
echo [AB] log: %LOG%
echo [AB] in game: borderless, 4K DLAA, DLSS FG ON, lock 30 fps (120 Hz panel)
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
"%LF%" --game %GAME_NAME% --mode 4x --interp 3 --unlock-mfg --mfg-model=keep2x %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
