@echo off
rem  PBZ 3x, OPTICAL FLOW: the documented correct vector source for PBZ.
setlocal
cd /d "%~dp0"
call "%~dp0pbz__env.bat"
if errorlevel 1 (
  echo.
  echo [PBZ] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%~dp0logs\pbz_11_of_3x.log"
echo.
echo [PBZ] TEST 11 - 3x, FidelityFX optical flow (--fg-of)
echo       expect in log: mv: auto -> optical flow (forced: Unreal/no-MV policy)
echo                      of=...ms in breakdown; MV/depth tag size = depth stream (1704x960)
echo       lock 40 fps; same spot/turning as the other runs
echo       log: %LOG%
echo.
"%LF%" --game %GAME% --mode 3x --interp 2 --unlock-mfg --fg-of %CAP% > "%LOG%" 2>&1
echo.
echo [PBZ] exit=%errorlevel%
pause
