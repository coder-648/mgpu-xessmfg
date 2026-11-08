@echo off
rem ===========================================================================
rem  run_smooth_oldfeed.bat  -  A/B control for the cadence frame-time feed
rem
rem  Identical to run_smooth.bat except --fg-cad-rt 0: the pacer is fed the
rem  median input interval (snapped to whole vblanks) instead of the length of
rem  the batch that was just tagged.
rem
rem  Why they differ: with a fluctuating input the multiplier alternates (a
rem  35-45 fps input needs 2.67-3.43 frames per real frame, so k goes 3,3,4,...).
rem  A 4-frame batch occupies 4 vblanks = 33.3 ms, but "median 25.0 ms" tells the
rem  pacer to squeeze those 4 frames into 3 vblanks and then idle for one - a
rem  visible hitch on every such batch, while the empty-refresh counter stays
rem  happy because every vblank still gets a frame. The default (cad:batch) feeds
rem  k * vblank so the budget always matches the batch.
rem
rem  Use this file only to compare by eye:
rem      run_smooth.bat          -> "cad:batch" in the log (default)
rem      run_smooth_oldfeed.bat  -> "quant" in the log (old behaviour)
rem  Look at how evenly objects move (strafe / pan), not at the fps counters.
rem ===========================================================================
setlocal
cd /d "%~dp0"

set "GAMEOPT="
if not "%~1"=="" set "GAMEOPT=--game %~1"

if not exist bridge.exe (
  echo [mgpu] bridge.exe missing in %CD%
  pause
  exit /b 1
)

bridge.exe %GAMEOPT% --den 2 --pool 4 --interp 5 --interp-cadence --unlock-mfg ^
           --fg-ui none --fg-rt-mode auto --fg-cad-rt 0 > bridge_smooth_old.txt 2>&1
type bridge_smooth_old.txt
pause
