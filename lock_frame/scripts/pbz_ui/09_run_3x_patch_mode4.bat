@echo off
rem v33 patch mode: the render card ships the HUDless scene under the UI support;
rem the consumer composes H' = present/patch and tags HUDless ONLY -> XeFG mode 4.
rem Requires UiPatch=1 (apply_ui.ps1 patch) and a game restart after applying it.
call "%~dp0ui__env.bat"

tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if errorlevel 1 (
  echo [patch] %GAME_NAME% is not running. Start it, load a save, then retry.
  pause
  exit /b 1
)
tasklist /fi "imagename eq lock_frame.exe" 2>nul | find /i "lock_frame.exe" >nul
if not errorlevel 1 (
  echo [patch] lock_frame.exe is already running. Stop it with Ctrl+Alt+End first.
  pause
  exit /b 1
)

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "TS=%%i"
set "TAG=%LOGDIR%\tag_patch_%TS%"
set "LOG=%LOGDIR%\run_patch_%TS%.log"
if not exist "%TAG%\" mkdir "%TAG%" >nul 2>&1

echo [patch] log: %LOG%
echo [patch] tag: %TAG%
echo [patch] dump files per frame: tag_NN_backbuffer.bmp (present), tag_NN_hudless.bmp
echo [patch]   (the H' handed to XeFG), tag_NN_patch.bmp (the reconstruction material:
echo [patch]   decoded HUDless patch; black tiles are copied from present), tag_NN_meta.txt.
echo [patch] run 30-60 s in a scene with HUD text, then Ctrl+Alt+End to stop.
echo.

"%LF%" %COMMON% --tag-dump 3 --tag-dump-after 240 --tag-dump-dir "%TAG%" > "%LOG%" 2>&1
echo exit=%errorlevel%
echo log: %LOG%
echo tag: %TAG%
pause
