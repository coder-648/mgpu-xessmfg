@echo off
setlocal
cd /d "%~dp0"
if not exist bridge.exe ( echo [mgpu] bridge.exe missing in %CD% & pause & exit /b 1 )
set "GAMEOPT="
if not "%~1"=="" set "GAMEOPT=--game %~1"
REM D1: no frame generation (1:1). NOTE: at ~36 fps on a 120 Hz panel this looks choppy by
REM nature (each frame holds 3 vblanks). The informative variant is D1 with the game locked
REM to 60 fps (RTSS): then every frame holds exactly 2 vblanks and any hitch left is OUR path.
bridge.exe %GAMEOPT% --den 1 --pool 4 --latency 1 --no-fg --unlock-mfg ^
           --overlay-layered --fg-ui none --fg-proj-rz --fg-rt-mode auto > bridge_diag_no_fg.txt 2>&1
type bridge_diag_no_fg.txt | more
pause
