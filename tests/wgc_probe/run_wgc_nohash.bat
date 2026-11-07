@echo off
rem ===========================================================================
rem  Same capture, but WITHOUT the per-frame CPU readback (the hash check).
rem  This is the production path: the frame stays on the GPU.
rem  Compare GPU usage against run_wgc.bat to see what the readback costs.
rem ===========================================================================
setlocal
cd /d "%~dp0"

if not exist wgc_probe.exe (
  echo [wgc] wgc_probe.exe missing in %CD%
  pause
  exit /b 1
)

echo ============================================================
echo  Capturing 20 s with NO GPU-to-CPU readback...
echo ============================================================
echo.

wgc_probe.exe 20 nohash > wgc_run_nohash.txt 2>&1
type wgc_run_nohash.txt

echo.
echo ------------------------------------------------------------
echo  Output saved to %CD%\wgc_run_nohash.txt
echo ------------------------------------------------------------
pause
