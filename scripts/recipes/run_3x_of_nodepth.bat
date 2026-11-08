@echo off
rem ===========================================================================
rem  run_3x_of_nodepth.bat  -  A/B test for the cross-adapter bus (see
rem                            docs/findings/PERF_BASELINE.md section 5, group B)
rem
rem  Deploy to: <TOOLS_DIR>\run_3x_of_nodepth.bat   (next to bridge.exe)
rem  Usage:     run_3x_of_nodepth.bat [game-exe-name]
rem
rem  Identical to run_3x_of.bat except --no-depth: the depth stream is no longer
rem  copied from the render card to the frame-generation card.
rem
rem  Why it matters (measured):
rem    --no-inputs measured "gpu" 25.2 -> 21.6 ms at 4K/DLAA, i.e. the whole
rem    cross-adapter copy is 3.6 ms per frame. MV and depth are each half of that
rem    traffic (2160p = 33 MB each), so dropping depth is worth ~1.8 ms per frame
rem    (FINAL_CONFIG 4.5). Nothing else changes: the frame loop, the cadence and
rem    the multiplier stay exactly as in run_3x_of.bat.
rem
rem  What to compare (both runs, same scene, same limiter):
rem    [bridge]   breakdown: ... gpu=... total=... cpu=...
rem    [bridge]   bus: mv ... + depth ... (not sent) = ... MB/frame
rem  Expect: bus: drops by ~half, gpu: drops by ~1.8 ms, "gen N/N" still matches
rem  and the picture looks the same (depth only feeds disocclusion clamping).
rem
rem  If you see edges smearing when objects move fast, depth IS buying you
rem  something on that title - go back to run_3x_of.bat and report it, because the
rem  documentation currently says it does not (PERF_BASELINE.md section 6).
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

echo ============================================================
echo  mgpu bridge - 3x via optical flow, depth stream OFF (A/B)
echo  Game must be running, in-world, locked to 40 fps.
echo ============================================================
echo.

bridge.exe %GAMEOPT% --den 1 --pool 4 --interp 2 --fg-of --no-depth ^
           --unlock-mfg --fg-ui none --fg-proj-rz --fg-rt-mode quant > bridge_3x_of_nodepth.txt 2>&1
type bridge_3x_of_nodepth.txt

echo.
echo ------------------------------------------------------------
echo  Output saved to %CD%\bridge_3x_of_nodepth.txt
echo  Compare "bus:" and "gpu=" against run_3x_of.bat.
echo ------------------------------------------------------------
pause
