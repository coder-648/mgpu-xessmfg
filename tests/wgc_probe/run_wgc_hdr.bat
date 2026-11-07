@echo off
rem ===========================================================================
rem  HDR capture: R16G16B16A16_FLOAT (scRGB) frame pool, no GPU readback.
rem  Same zero-cost path as run_wgc_nohash, but keeps the HDR range instead of
rem  letting DWM tone-map to 8-bit SDR.
rem ===========================================================================
setlocal
cd /d "%~dp0"

if not exist wgc_probe.exe (
  echo [wgc] wgc_probe.exe missing in %CD%
  pause
  exit /b 1
)

echo ============================================================
echo  Capturing 20 s as scRGB FP16 (HDR preserved, no readback)...
echo ============================================================
echo.

wgc_probe.exe 20 nohash hdr > wgc_run_hdr.txt 2>&1
type wgc_run_hdr.txt

echo.
echo ------------------------------------------------------------
echo  Output saved to %CD%\wgc_run_hdr.txt
echo ------------------------------------------------------------
pause
