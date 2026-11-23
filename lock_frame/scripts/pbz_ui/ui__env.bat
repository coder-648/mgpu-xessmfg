@echo off
rem ===========================================================================
rem  PBZ UI composition test - shared environment (no setlocal: callers
rem  need these variables).
rem ===========================================================================
set "GAME_DIR=F:\steam\steamapps\common\PBZ\platform\steam"
set "UI_DIR=%~dp0"
set "GAME_NAME=PBZ.exe"
set "GAME_BASE=PBZ"
set "GAME_EXE=%GAME_DIR%\PBZ.exe"
set "LF=%GAME_DIR%\lock_frame.exe"
set "INI=%GAME_DIR%\fakerFG.ini"
set "LOGDIR=%UI_DIR%logs"
set "RESDIR=%UI_DIR%results"
if not exist "%LOGDIR%\" mkdir "%LOGDIR%" >nul 2>&1
if not exist "%RESDIR%\" mkdir "%RESDIR%" >nul 2>&1

rem Same 3x recipe as CP2077; PBZ renders internally at ~2562x1441 and presents 4K.
set "BASE=--game %GAME_NAME% --mode 3x --interp 2 --streams=mv,depth --overlay-layered --wgc-minint 0 --pool 4 --latency 1 --unlock-mfg --mfg-model=keep2x --fg-rt-mode auto --report 40"
set "COMMON=%BASE% --ui-source auto"

