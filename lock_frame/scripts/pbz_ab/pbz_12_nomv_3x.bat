@echo off
rem  PBZ 3x, ZERO MV control (--no-mv). If this is cleaner than TEST 10, the
rem  game's MV stream is actively harmful; if it is worse, MV is being consumed.
setlocal
cd /d "%~dp0"
call "%~dp0pbz__env.bat"
if errorlevel 1 (
  echo.
  echo [PBZ] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%~dp0logs\pbz_12_nomv_3x.log"
echo.
echo [PBZ] TEST 12 - 3x, zero MV (--no-mv) - control
echo       lock 40 fps; same spot/turning as the other runs
echo       log: %LOG%
echo.
"%LF%" --game %GAME% --mode 3x --interp 2 --unlock-mfg --no-mv %CAP% > "%LOG%" 2>&1
echo.
echo [PBZ] exit=%errorlevel%
pause
