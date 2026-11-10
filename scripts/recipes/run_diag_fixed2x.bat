@echo off
setlocal
cd /d "%~dp0"
if not exist bridge.exe ( echo [mgpu] bridge.exe missing in %CD% & pause & exit /b 1 )
set "GAMEOPT="
if not "%~1"=="" set "GAMEOPT=--game %~1"
REM D2: fixed 2x (no cadence). Best paired with the game locked to 60 fps.
bridge.exe %GAMEOPT% --den 1 --pool 4 --latency 1 --interp 1 --unlock-mfg ^
           --overlay-layered --fg-ui none --fg-proj-rz --fg-rt-mode auto > bridge_diag_fixed2x.txt 2>&1
type bridge_diag_fixed2x.txt | more
pause
