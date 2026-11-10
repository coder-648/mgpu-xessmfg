@echo off
setlocal
cd /d "%~dp0"
if not exist bridge.exe ( echo [mgpu] bridge.exe missing in %CD% & pause & exit /b 1 )
set "GAMEOPT="
if not "%~1"=="" set "GAMEOPT=--game %~1"
REM A/B test B: motion vectors from our own FidelityFX optical flow (no game MV).
REM Use when the game MV ghosts.
bridge.exe %GAMEOPT% --den 1 --pool 4 --latency 1 --interp 5 --interp-cadence ^
           --unlock-mfg --fg-ui none --fg-proj-rz --fg-rt-mode auto --fg-of
pause
