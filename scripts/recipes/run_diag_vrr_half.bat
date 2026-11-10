@echo off
setlocal
cd /d "%~dp0"
if not exist bridge.exe ( echo [mgpu] bridge.exe missing in %CD% & pause & exit /b 1 )
set "GAMEOPT="
if not "%~1"=="" set "GAMEOPT=--game %~1"
REM D3: VRR + half resolution (GPU has plenty of headroom here).
bridge.exe %GAMEOPT% --den 2 --pool 4 --latency 1 --interp 5 --interp-cadence --vrr --unlock-mfg ^
           --overlay-layered --fg-ui none --fg-proj-rz --fg-rt-mode auto > bridge_diag_vrr_half.txt 2>&1
type bridge_diag_vrr_half.txt | more
pause
