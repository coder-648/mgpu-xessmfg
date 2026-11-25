@echo off
rem  PBZ 3x, PORTED model 15 (--mfg-model=port). Requires xefg_programs\ next to
rem  lock_frame.exe; without it the consumer silently falls back to keep2x.
setlocal
cd /d "%~dp0"
call "%~dp0pbz__env.bat"
if errorlevel 1 (
  echo.
  echo [PBZ] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "PROG=%~dp0..\xefg_programs"
if not exist "%PROG%\programs.tsv" (
  echo [PBZ] missing %PROG%\programs.tsv - copy build\xefg_programs next to lock_frame.exe.
  pause
  exit /b 1
)
set "LOG=%~dp0logs\pbz_18_port_3x.log"
echo.
echo [PBZ] TEST 18 - 3x, ported model 15 (--mfg-model=port)
echo       expect in log: model_port_applied / --unlock-mfg: ported MFG model route active
echo       compare character edges with TEST 10 (keep2x)
echo       lock 40 fps; same spot/turning as TEST 10
echo       log: %LOG%
echo.
"%LF%" --game %GAME% --mode 3x --interp 2 --unlock-mfg --mfg-model=port %CAP% > "%LOG%" 2>&1
echo.
echo [PBZ] exit=%errorlevel%
pause
