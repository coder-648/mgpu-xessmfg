@echo off
rem ===========================================================================
rem  run_smooth_batchfeed.bat  -  A/B control: frameRenderTime = batch * vblank
rem
rem  Identical to run_smooth.bat except --fg-cad-rt 1. The default feeds the
rem  pacer the median input interval (snapped to whole vblanks); this one feeds
rem  "how long the batch that was just tagged actually covers" (k * vblank).
rem
rem  Theory says the second is righter: a 4-frame batch covers 33.3 ms, so a
rem  budget of 25.0 ms makes the pacer squeeze 4 frames into 3 vblanks and idle
rem  for one. Measured against the eye, it is worse:
rem
rem      --fg-cad-rt 0 (default)   xN = 3.00 / 3.00   <- stays on the ideal count
rem      --fg-cad-rt 1 (this file) xN = 3.05 / 3.38   <- drifts up, picture surges
rem
rem  Why: Present() blocks on the pacer's budget and the input interval is
rem  measured at the *next* WGC arrival, so a longer budget inflates the very
rem  number that picks k. With queue depth 1 the input rate is our own throttle,
rem  so the loop has no fixed point and the "correct" budget makes it hunt.
rem  See docs/findings/PERF_BASELINE.md 3.24.
rem
rem  Use this file only to compare by eye:
rem      run_smooth.bat            -> "quant" in the log (default, recommended)
rem      run_smooth_batchfeed.bat  -> "cad:batch" in the log (experimental)
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
           --fg-ui none --fg-rt-mode auto --fg-cad-rt 1 > bridge_smooth_batch.txt 2>&1
type bridge_smooth_batch.txt
pause
