@echo off
rem  PBZ 2x, OPTICAL FLOW (t = 0.5). Compare with 11 (3x OF): if 2x is clean,
rem  the remaining edges are the keep2x model at t != 0.5, not the vector source.
setlocal
cd /d "%~dp0"
call "%~dp0pbz__env.bat"
if errorlevel 1 (
  echo.
  echo [PBZ] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%~dp0logs\pbz_13_of_2x.log"
echo.
echo [PBZ] TEST 13 - 2x, optical flow (--fg-of)
echo       lock 60 fps; same spot/turning as the other runs
echo       log: %LOG%
echo.
"%LF%" --game %GAME% --mode 2x --interp 1 --fg-of %CAP% > "%LOG%" 2>&1
echo.
echo [PBZ] exit=%errorlevel%
pause
