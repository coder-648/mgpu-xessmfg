@echo off
rem ===========================================================================
rem  PBZ (Phantom Blade Zero) shared environment / preflight
rem  Place this folder next to lock_frame.exe (PBZ/platform/steam/pbz_ab).
rem  The game window is matched by --game PBZ.exe, so no --hwnd is needed.
rem
rem  NO setlocal here: this file is CALLed by the per-test scripts, and a
rem  setlocal inside a called batch discards every variable when it returns
rem  (the caller would see an empty %LF% / %GAME% / %CAP%).
rem ===========================================================================
set "PBZ_DIR=%~dp0.."
set "LF=%PBZ_DIR%\lock_frame.exe"
set "GAME=PBZ.exe"
set "LOG=%~dp0logs"
if not exist "%LOG%\" mkdir "%LOG%" >nul 2>&1

rem Same fixed part the UI emits (3x recipe); each script adds its variable.
set "CAP=--den 1 --pool 4 --latency 1 --streams=mv,depth --wgc-minint 0 --fg-rt-mode auto --report 40 --mfg-model=keep2x"

set "RC=0"
if not exist "%LF%"                    ( echo [PBZ] missing "%LF%" & set "RC=1" )
rem These are game-side files; when the scripts run from the release package's
rem extras\pbz_ab, they live in the game directory (the UI installs them there),
rem so their absence here is a note, not a failure.
if not exist "%PBZ_DIR%\fakerFG.ini"   ( echo [PBZ] note: no fakerFG.ini next to lock_frame.exe ^(fine when running from the package; the game dir has it^) )
if not exist "%PBZ_DIR%\dxgi.dll"      ( echo [PBZ] note: dxgi.dll not here ^(fine when running from the package; the game dir has it^) )
if not exist "%PBZ_DIR%\xess_mfg_producer.dll" ( echo [PBZ] note: producer dll not here ^(fine when running from the package^) )

tasklist /fi "imagename eq %GAME%" 2>nul | find /i "%GAME%" >nul
if errorlevel 1 ( echo [PBZ] %GAME% is NOT running. Start the game, load a save, DLSS FG ON. & set "RC=1" )

tasklist /fi "imagename eq lock_frame.exe" 2>nul | find /i "lock_frame.exe" >nul
if not errorlevel 1 ( echo [PBZ] lock_frame.exe is already running. Stop it ^(Ctrl+Alt+End^) first. & set "RC=1" )

if not "%RC%"=="0" exit /b 1
exit /b 0
