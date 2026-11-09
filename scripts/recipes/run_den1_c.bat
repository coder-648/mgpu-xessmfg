@echo off
rem ===========================================================================
rem  run_den1_c.bat  -  4K NATIVE output + --prefetch-inputs.  THE "C" RUN.
rem
rem  Compare against run_den1_a.bat ("A" = same command, no switch) and/or
rem  run_den1_b.bat ("B" = --input-copy-queue only).  --prefetch-inputs implies
rem  --input-copy-queue and additionally copies slot N+1 during frame N's Present,
rem  waiting for the producer's cross-adapter fence on the copy queue itself.
rem
rem  Why a separate profile: the --den 2 recipes present 1080p, so the colour
rem  convert draw is only 1080p and the overlap window for --input-copy-queue
rem  (min(convert draw, mv+depth copies)) is 0.2-0.5 ms - below this machine's
rem  2-10 ms noise (measured: PERF_BASELINE.md 3.28.4, total -0.35 ms while the
rem  scene drift was -1.40 ms). At --den 1 the convert draw is 3840x2160, i.e.
rem  4x bigger, so the same switch has ~1-1.5 ms of room.
rem
rem  --interp 3, not 5: 4K native output has 4x the colour cost, and 3.8
rem  measured 4K native 3x@40 at load 76-98% already. kmax 6 (--interp 5) there
rem  would only produce drops and noise. With --interp-cadence a 40 fps input
rem  still runs 3x (25 ms = 3 vblanks); kmax 4 just leaves room for a 4-vblank
rem  gap.
rem
rem  If the A log shows load > 100%% or slow > 0, drop BOTH files to --interp 2
rem  and run again - a saturated budget makes the comparison meaningless.
rem
rem  Setup: game in-world, in-game frame generation OFF, upscaler DLSS/FSR/XeSS,
rem  RTSS locked at 40 fps, 120 Hz panel.  Same position and view for all runs.
rem
rem  Run order:  this file   -> bridge_den1_C.txt
rem              run_den1_b.bat -> bridge_den1_B.txt    (same + --input-copy-queue)
rem              this file again -> brackets the drift (A B A)
rem  Then: python3 scripts/perf_ab.py bridge_den1_C.txt bridge_den1_A2.txt bridge_den1_B.txt
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

bridge.exe %GAMEOPT% --den 1 --pool 4 --interp 3 --interp-cadence --unlock-mfg ^
           --fg-ui none --fg-rt-mode auto --prefetch-inputs > bridge_den1_C.txt 2>&1
type bridge_den1_C.txt
pause
