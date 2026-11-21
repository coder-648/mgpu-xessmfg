@echo off
rem ===========================================================================
rem  CP2077 XeSS-FG A/B - shared environment + preflight (2026-11-22)
rem  Every ab_*.bat calls this. Edit GAME_DIR below if the game moves.
rem ===========================================================================
set "AB_DIR=%~dp0"
set "GAME_DIR=E:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64"
set "GAME_EXE=%GAME_DIR%\Cyberpunk2077.exe"
set "GAME_NAME=Cyberpunk2077.exe"
set "LF=%AB_DIR%lock_frame.exe"
set "AB_LOG=%AB_DIR%ab_logs"
if not exist "%AB_LOG%\" mkdir "%AB_LOG%" >nul 2>&1

rem Measured-good capture recipe (same as run_3x.bat in this folder).
set "CAPTURE=--streams=mv,depth --overlay-layered --wgc-minint 0 --pool 4 --latency 1 --fg-rt-mode auto --report 40"

set "AB_RC=0"
if not exist "%LF%"                             ( echo [AB] MISSING "%LF%" & set "AB_RC=1" )
if not exist "%GAME_EXE%"                       ( echo [AB] MISSING "%GAME_EXE%" & set "AB_RC=1" )
if not exist "%GAME_DIR%\dxgi.dll"              ( echo [AB] fakerFG is not deployed: "%GAME_DIR%\dxgi.dll" missing & set "AB_RC=1" )
if not exist "%GAME_DIR%\xess_mfg_producer.dll" ( echo [AB] producer missing: "%GAME_DIR%\xess_mfg_producer.dll" & set "AB_RC=1" )
if not exist "%GAME_DIR%\fakerFG.ini"           ( echo [AB] fakerFG.ini missing in "%GAME_DIR%" & set "AB_RC=1" )
if exist "%GAME_DIR%\version.dll"               ( echo [AB] version.dll is present in the game folder - dlssg_sm86 conflicts with this chain & set "AB_RC=1" )

if not "%AB_RC%"=="0" exit /b 1

rem fakerFG must leave FG output to the consumer (Capture), not run XeFG itself.
findstr /i /b /c:"FgOutput=XeFG" "%GAME_DIR%\fakerFG.ini" >nul 2>&1
if not errorlevel 1 ( echo [AB] fakerFG.ini says FgOutput=XeFG - this A/B needs FgOutput=Capture & exit /b 1 )

tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if errorlevel 1 ( echo [AB] %GAME_NAME% is NOT running. Start the game, load a save, turn DLSS on. & exit /b 2 )

tasklist /fi "imagename eq lock_frame.exe" 2>nul | find /i "lock_frame.exe" >nul
if not errorlevel 1 ( echo [AB] lock_frame.exe is already running. Stop that one first: Ctrl+Alt+End. & exit /b 3 )

exit /b 0
