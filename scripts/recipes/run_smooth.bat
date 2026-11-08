@echo off
rem ===========================================================================
rem  run_smooth.bat  -  smoothest profile: per-frame multiplier (--interp-cadence)
rem
rem  Deploy to: <TOOLS_DIR>\run_smooth.bat   (next to bridge.exe + the two DLLs)
rem  Usage:     run_smooth.bat [game-exe-name]
rem             e.g.  run_smooth.bat Cyberpunk2077.exe
rem
rem  Measured on CP2077 4K, game-in-world report windows only
rem  (docs/findings/PERF_BASELINE.md 3.17) - empty refresh periods:
rem       input 60 fps -> 2x :  2.5%      (plain fixed 2x was 6.4%)
rem       input 40 fps -> 3x :  1.5%
rem       input 30 fps -> 4x :  2.6%
rem
rem  One command covers all three: --interp-cadence asks XeFG for as many frames as
rem  the vblanks that actually elapsed, so the multiplier follows the input by itself.
rem
rem  --interp matters twice: the MFG patch only lifts the DLL as far as you ASK for, and
rem  --interp-cadence can only ask for as many frames as that ceiling allows (kmax =
rem  interp + 1). Measured empty refresh periods, 4K --den 2, same scene:
rem      --interp 2 (kmax 3): 60fps 2.5%   40fps 10.8%
rem      --interp 3 (kmax 4): 60fps 2.5%   40fps  1.5%   30fps 2.6%
rem      --interp 5 (kmax 6): 60fps 0.8%   40fps  0.4%   30fps 0.6%   <- default here
rem  --interp 5 costs load 81-85% (vs 19-51% at interp 3). If load sits above 100% in the
rem  log, drop to --interp 3 (30/40 fps) or --interp 2 (60 fps).
rem
rem  Before starting: game in-world, in-game frame generation OFF, upscaler
rem  DLSS / FSR / XeSS, and an external limiter (RTSS) at 60 / 40 / 30 fps for
rem  2x / 3x / 4x on a 120 Hz panel.
rem
rem  Log: bridge_smooth.txt
rem  Watch: cadence = 1.00 vblank/frame, out: vblanks ~ presents, arr(vb): most
rem         frames in the nominal bucket (2x / 3x / 4x).
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
           --fg-ui none --fg-rt-mode quant > bridge_smooth.txt 2>&1
type bridge_smooth.txt
pause
