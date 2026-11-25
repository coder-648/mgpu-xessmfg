@echo off
rem  PBZ 3x on the NATIVE render grid: no joint MV/depth resample to 3840x2160.
rem  Requires fakerFG.ini to publish both streams 1:1 at render size:
rem      CodecStreamPercent=100 / MvCodecPercent=100 / DepthCodecPercent=100
rem  (UI: MV/depth transfer ratio = 100%, depth codec = 100%), then RESTART THE GAME.
setlocal
cd /d "%~dp0"
call "%~dp0pbz__env.bat"
if errorlevel 1 (
  echo.
  echo [PBZ] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
findstr /b /c:"DepthCodecPercent=100" "%~dp0..\fakerFG.ini" >nul 2>&1
if errorlevel 1 (
  echo.
  echo [PBZ] fakerFG.ini is not on 1:1 depth. In the UI set:
  echo         MV/depth transfer ratio = 100%%
  echo         depth codec             = 100%% ^(bit-exact^)
  echo       then fully restart the game and run this script again.
  echo.
  pause
  exit /b 1
)
set "LOG=%~dp0logs\pbz_20_render_grid_3x.log"
echo.
echo [PBZ] TEST 20 - 3x, native render grid (--no-upsample, depth 100)
echo       expect in log: xefg-in: mv=2562x1441 depth=2562x1441 (no "rebuilt ... to 3840")
echo       lock 40 fps; same spot/turning as TEST 10
echo       log: %LOG%
echo.
"%LF%" --game %GAME% --mode 3x --interp 2 --unlock-mfg --no-upsample %CAP% > "%LOG%" 2>&1
echo.
echo [PBZ] exit=%errorlevel%
pause
