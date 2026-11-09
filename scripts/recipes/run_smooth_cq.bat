@echo off
rem ===========================================================================
rem  run_smooth_cq.bat  -  A/B twin of run_smooth.bat, plus --input-copy-queue
rem
rem  Everything is identical to "5 - smoothest (cadence).bat" EXCEPT the extra
rem  --input-copy-queue, so a back-to-back run of the two is a clean A/B of that
rem  one switch (docs/findings/PERF_BASELINE.md 3.28.1).
rem
rem  What the switch does: the MV/depth cross-adapter CopyTextureRegion moves to
rem  a dedicated COPY queue, so it runs on the copy engine at the same time as
rem  our colour convert draw instead of queueing behind it on the 3D engine.
rem  Expected: `gpu`/`total` down 0.2-1 ms, `mvs` shrinks (only the recording
rem  time is left), new `cq=` field ~0, `bus:` unchanged.
rem
rem  RUN ORDER (same position, same view, same in-game settings):
rem      1. double-click "5 - smoothest (cadence).bat"      -> bridge_smooth.txt
rem      2. double-click this one                           -> bridge_smooth_cq.txt
rem      3. optionally run step 1 again to bracket drift
rem  Compare the `breakdown:` lines of the two logs (or: python3 scripts/perf_ab.py).
rem
rem  STOP RULES - these mean "XeFG still needs our command list", turn it off:
rem    * `xN` no longer the same value as in the A run
rem    * any `ERR_INVALID_ARGUMENT` / `xNmiss` / black picture / device-removed
rem    * `cq=` climbing into ms (the wait is no longer free)
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

rem  PREREQUISITE: the game folder must have a current game_capture.addon64 with
rem  [capture] StreamHeight=1440 (the default since 2026-11-09).
rem
rem  Do NOT add --fg-of here (see run_smooth.bat) and do NOT combine with
rem  --no-mv / --no-depth / --dump / --no-throttle: the switch disables itself in
rem  those modes (they record clears/readbacks that a COPY list cannot take).
bridge.exe %GAMEOPT% --den 2 --pool 4 --interp 5 --interp-cadence --unlock-mfg ^
           --fg-ui none --fg-rt-mode auto --input-copy-queue > bridge_smooth_cq.txt 2>&1
type bridge_smooth_cq.txt
pause
