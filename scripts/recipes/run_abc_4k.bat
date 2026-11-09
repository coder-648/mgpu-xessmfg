@echo off
rem ===========================================================================
rem  run_abc_4k.bat  -  fully automatic A/B/C/A comparison at 4K native output
rem
rem  The operator-driven version (recipes 9/10/11) cannot produce comparable
rem  numbers on this machine: two runs of the SAME config differed by 3.2 ms in
rem  `total` and by 6x in `age` (7.9 vs 48.5 ms median) - the scene moved more
rem  than the effect being measured (PERF_BASELINE 3.28.4/3.28.5).
rem
rem  This script starts bridge.exe four times back to back, 60 s each, and kills
rem  it in between, so the four segments are minutes apart instead of being
rem  separated by however long you take to double-click the next file:
rem
rem      1A  --den 1 --interp 3 --interp-cadence --unlock-mfg --fg-ui none
rem          --fg-rt-mode auto                              -> bridge_ab_1A.txt
rem      2B  ... + --input-copy-queue                       -> bridge_ab_2B.txt
rem      3C  ... + --prefetch-inputs                        -> bridge_ab_3C.txt
rem      4A  same as 1A (brackets the drift)                -> bridge_ab_4A.txt
rem
rem  Usage:   run_abc_4k.bat Cyberpunk2077.exe
rem  Setup:   game in-world, in-game frame generation OFF, upscaler on,
rem           RTSS locked at 40 fps. Once started, DO NOT touch the keyboard or
rem           alt-tab: bridge quits on focus loss (--exit-on-focus-loss) and that
rem           segment would be short/garbage.
rem
rem  Then:    python3 scripts/perf_ab.py bridge_ab_1A.txt bridge_ab_4A.txt bridge_ab_2B.txt bridge_ab_3C.txt
rem  Read:    total / present / load, and the `pf:` line in 3C (hit rate).
rem           Discard any segment whose `age` median is > 20 ms or whose window
rem           count is much smaller than the others - that is a disturbed run.
rem ===========================================================================
setlocal
cd /d "%~dp0"

set "GAMEOPT="
if not "%~1"=="" set "GAMEOPT=--game %~1"
set "COMMON=%GAMEOPT% --den 1 --pool 4 --interp 3 --interp-cadence --unlock-mfg --fg-ui none --fg-rt-mode auto"

if not exist bridge.exe (
  echo [mgpu] bridge.exe missing in %CD%
  pause
  exit /b 1
)
taskkill /f /im bridge.exe >nul 2>&1

echo [mgpu] 1/4  baseline           -^> bridge_ab_1A.txt
start "" /b bridge.exe %COMMON% > bridge_ab_1A.txt 2>&1
ping -n 62 127.0.0.1 >nul
taskkill /f /im bridge.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul

echo [mgpu] 2/4  + --input-copy-queue -^> bridge_ab_2B.txt
start "" /b bridge.exe %COMMON% --input-copy-queue > bridge_ab_2B.txt 2>&1
ping -n 62 127.0.0.1 >nul
taskkill /f /im bridge.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul

echo [mgpu] 3/4  + --prefetch-inputs  -^> bridge_ab_3C.txt
start "" /b bridge.exe %COMMON% --prefetch-inputs > bridge_ab_3C.txt 2>&1
ping -n 62 127.0.0.1 >nul
taskkill /f /im bridge.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul

echo [mgpu] 4/4  baseline again       -^> bridge_ab_4A.txt
start "" /b bridge.exe %COMMON% > bridge_ab_4A.txt 2>&1
ping -n 62 127.0.0.1 >nul
taskkill /f /im bridge.exe >nul 2>&1

echo.
echo [mgpu] done - four logs written. Check each segment before trusting it:
echo        findstr /c:"age=" bridge_ab_1A.txt   (median should be under ~20 ms)
echo        findstr /c:"pf:" bridge_ab_3C.txt    (prefetch hit rate)
echo.
pause
