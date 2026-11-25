@echo off
rem  PBZ 2x, game MV (control for 13).
setlocal
cd /d "%~dp0"
call "%~dp0pbz__env.bat"
if errorlevel 1 (
  echo.
  echo [PBZ] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%~dp0logs\pbz_14_game_mv_2x.log"
echo.
echo [PBZ] TEST 14 - 2x, game MV (control)
echo       lock 60 fps; same spot/turning as the other runs
echo       log: %LOG%
echo.
"%LF%" --game %GAME% --mode 2x --interp 1 %CAP% > "%LOG%" 2>&1
echo.
echo [PBZ] exit=%errorlevel%
pause
