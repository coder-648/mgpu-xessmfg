@echo off
rem  PBZ 3x, DEFAULT: the game's own motion vectors (what the UI currently does).
setlocal
cd /d "%~dp0"
call "%~dp0pbz__env.bat"
if errorlevel 1 (
  echo.
  echo [PBZ] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%~dp0logs\pbz_10_game_mv_3x.log"
echo.
echo [PBZ] TEST 10 - 3x, game MV (default)
echo       expect in log: mv: auto -> the producer's motion vectors
echo                      xefg-in: mv=3840x2160 (upsampled), mvscale=(1.000, 1.000)
echo       lock 40 fps; same spot/turning as the other runs
echo       log: %LOG%
echo.
"%LF%" --game %GAME% --mode 3x --interp 2 --unlock-mfg %CAP% > "%LOG%" 2>&1
echo.
echo [PBZ] exit=%errorlevel%
pause
