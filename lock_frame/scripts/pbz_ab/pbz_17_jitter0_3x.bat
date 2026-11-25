@echo off
rem  PBZ 3x, JITTER control (--fg-jitter-zero). Only an A/B: on CP2077 zero jitter
rem  was worse, so do not keep it unless this title clearly improves.
setlocal
cd /d "%~dp0"
call "%~dp0pbz__env.bat"
if errorlevel 1 (
  echo.
  echo [PBZ] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%~dp0logs\pbz_17_jitter0_3x.log"
echo.
echo [PBZ] TEST 17 - 3x, --fg-jitter-zero (jitter contract control)
echo       lock 40 fps; same spot/turning as TEST 10
echo       log: %LOG%
echo.
"%LF%" --game %GAME% --mode 3x --interp 2 --unlock-mfg --fg-jitter-zero %CAP% > "%LOG%" 2>&1
echo.
echo [PBZ] exit=%errorlevel%
pause
