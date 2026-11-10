@echo off
setlocal
cd /d "%~dp0"
if not exist bridge.exe ( echo [mgpu] bridge.exe missing & pause & exit /b 1 )
set "GAMEOPT="
if not "%~1"=="" set "GAMEOPT=--game %~1"
REM F: dynamic mode with the batch budget fixed to k vblanks (--cad-batch-rt).
REM The count comes from the game's own present interval, so the k*vblank budget cannot feed
REM back into the measurement. Expect: presents == vblanks == 120 instead of 125..141.
bridge.exe %GAMEOPT% --den 1 --pool 4 --latency 1 --interp 5 --interp-cadence --cad-batch-rt ^
           --unlock-mfg --overlay-layered --fg-ui none --fg-proj-rz --fg-rt-mode auto > bridge_diag_cad_rt.txt 2>&1
type bridge_diag_cad_rt.txt | more
pause
