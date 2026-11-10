@echo off
setlocal
cd /d "%~dp0"
if not exist bridge.exe ( echo [mgpu] bridge.exe missing & pause & exit /b 1 )
set "GAMEOPT="
if not "%~1"=="" set "GAMEOPT=--game %~1"
REM E: remove the WGC delivery throttle (GraphicsCaptureSession5::MinUpdateInterval = 0).
REM Default WGC hands over at most ~1 frame per compositor interval (~16.6 ms = ~60 fps)
REM regardless of how fast the game presents. Compare with a normal run: look for the log line
REM   [bridge] WGC MinUpdateInterval = ... ms
REM and for the capture rate in the OSD: it can now exceed 60.
bridge.exe %GAMEOPT% --den 1 --pool 4 --latency 1 --interp 5 --interp-cadence --wgc-minint 0 ^
           --unlock-mfg --overlay-layered --fg-ui none --fg-proj-rz --fg-rt-mode auto > bridge_diag_wgc_nothrottle.txt 2>&1
type bridge_diag_wgc_nothrottle.txt | more
pause
