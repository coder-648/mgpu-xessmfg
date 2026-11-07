@echo off
rem ===========================================================================
rem  probe_fg.bat  -  ask the loaded libxess_fg.dll what it can do
rem
rem  Deploy to: <TOOLS_DIR>\probe_fg.bat   (next to bridge.exe)
rem
rem  No game needed: this only creates a D3D12 context on the AMD card and asks
rem  the DLL for its version and maxSupportedInterpolations, then exits.
rem
rem  Use it after dropping in a different libxess_fg.dll, or to check the MFG
rem  unlock:
rem     probe_fg.bat                          -> the one next to bridge.exe
rem     probe_fg.bat --unlock-mfg --interp 3  -> patched, should report 4x
rem     probe_fg.bat --fg-lib "D:\x\libxess_fg.dll"
rem
rem  VERDICT "2x only" = the non-Intel clamp is still active (no --unlock-mfg).
rem  VERDICT "MFG available, up to Nx" = 3x/4x are possible from this DLL.
rem
rem  Log: probe_fg.txt in this folder.
rem ===========================================================================
setlocal
cd /d "%~dp0"

if not exist bridge.exe (
  echo [mgpu] bridge.exe missing in %CD%
  pause
  exit /b 1
)

echo ============================================================
echo  libxess_fg capability probe
echo ============================================================
echo.

if "%~1"=="" (
  bridge.exe --fg-probe > probe_fg.txt 2>&1
) else (
  bridge.exe --fg-probe %* > probe_fg.txt 2>&1
)
type probe_fg.txt

echo.
echo ------------------------------------------------------------
echo  Output saved to %CD%\probe_fg.txt
echo ------------------------------------------------------------
pause
