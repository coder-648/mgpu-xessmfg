@echo off
setlocal
cd /d "%~dp0"
if not exist bridge.exe ( echo [mgpu] bridge.exe missing in %CD% & pause & exit /b 1 )
set "GAMEOPT="
if not "%~1"=="" set "GAMEOPT=--game %~1"
REM A/B test A: same profile as the UI plus --fg-jittered-mv (XeFG removes the
REM TAA jitter from the game velocity). Compares against B - optical flow.
bridge.exe %GAMEOPT% --den 1 --pool 4 --latency 1 --interp 5 --interp-cadence ^
           --unlock-mfg --fg-ui none --fg-proj-rz --fg-rt-mode auto --fg-jittered-mv
pause
