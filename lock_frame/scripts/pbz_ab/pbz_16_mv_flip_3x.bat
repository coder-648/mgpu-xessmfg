@echo off
rem  PBZ 3x, MV SIGN control (--mv-flip). If character edges change side/sharpen,
rem  the declared prev-cur sign is wrong for this title; it does not change the model.
setlocal
cd /d "%~dp0"
call "%~dp0pbz__env.bat"
if errorlevel 1 (
  echo.
  echo [PBZ] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%~dp0logs\pbz_16_mv_flip_3x.log"
echo.
echo [PBZ] TEST 16 - 3x, --mv-flip (sign control)
echo       lock 40 fps; same spot/turning as TEST 10
echo       log: %LOG%
echo.
"%LF%" --game %GAME% --mode 3x --interp 2 --unlock-mfg --mv-flip %CAP% > "%LOG%" 2>&1
echo.
echo [PBZ] exit=%errorlevel%
pause
