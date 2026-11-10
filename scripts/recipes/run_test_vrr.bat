@echo off
setlocal
cd /d "%~dp0"
if not exist bridge.exe ( echo [mgpu] bridge.exe missing in %CD% & pause & exit /b 1 )
set "GAMEOPT="
if not "%~1"=="" set "GAMEOPT=--game %~1"
REM Test C: --vrr - fixed frame count per input, evenly spaced; the panel follows
REM (VRR / G-Sync / FreeSync) instead of us filling every vblank. Only use this with VRR
REM actually enabled: without it you get duplicate frames instead of uniform motion.
bridge.exe %GAMEOPT% --den 1 --pool 4 --latency 1 --interp 5 --interp-cadence --vrr ^
           --unlock-mfg --fg-ui none --fg-proj-rz --fg-rt-mode auto
pause
