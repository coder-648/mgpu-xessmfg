@echo off
rem ===========================================================================
rem  CP2077 UI composition test - shared environment (no setlocal: callers
rem  need these variables).  Edit GAME_DIR here if the game moves.
rem ===========================================================================
set "GAME_DIR=E:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64"
set "UI_DIR=%~dp0"
set "GAME_NAME=Cyberpunk2077.exe"
set "GAME_EXE=%GAME_DIR%\Cyberpunk2077.exe"
set "LF=%GAME_DIR%\lock_frame.exe"
set "INI=%GAME_DIR%\fakerFG.ini"
set "LOGDIR=%UI_DIR%logs"
set "RESDIR=%UI_DIR%results"
if not exist "%LOGDIR%\" mkdir "%LOGDIR%" >nul 2>&1
if not exist "%RESDIR%\" mkdir "%RESDIR%" >nul 2>&1

rem The 3x recipe that is already validated for CP2077 (run_3x.bat in the game
rem folder): game/RTSS locks the input at 40 fps, consumer presents 3x = 120.
set "BASE=--game %GAME_NAME% --mode 3x --interp 2 --streams=mv,depth --overlay-layered --wgc-minint 0 --pool 4 --latency 1 --unlock-mfg --mfg-model=keep2x --fg-rt-mode auto --report 40"
set "COMMON=%BASE% --ui-source auto"
