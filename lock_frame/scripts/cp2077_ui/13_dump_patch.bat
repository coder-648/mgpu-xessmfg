@echo off
rem ============================================================================
rem  13_dump_patch.bat - visual dump for the current v34 support-patch path.
rem
rem  Runs the consumer with tag-dump for 5 frames after 120 frames. The game may
rem  stay running; only lock_frame.exe must be stopped (Ctrl+Alt+End) because the
rem  console consumer owns the dump directory.
rem
rem  Output per dumped frame (tag_NN_*.bmp are 8-bit views of R10/HDR):
rem    tag_NN_backbuffer.bmp  WGC present P (UI inside)
rem    tag_NN_hudless.bmp     H' handed to XeFG (present outside, patch inside)
rem    tag_NN_patch.bmp       decoded patch / HUDless material
rem    tag_NN_ui.bmp          UI plane when stream 3 carries one
rem    tag_NN_meta.txt        frame / mode / sizes / patch flag
rem
rem  What to compare visually:
rem    1) backbuffer vs hudless: HUD must be gone in H'
rem    2) hudless vs patch: patch should cover only the HUD support (black tiles are
rem       present/copy regions)
rem    3) look at the HUD edge in hudless: no white outline / tone step
rem    4) compare two dumped frames while moving: edge must not shimmer
rem ============================================================================
call "%~dp0ui__env.bat"

tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if errorlevel 1 (
  echo [dump] %GAME_NAME% is not running. Start it, load a save, then retry.
  pause
  exit /b 1
)
tasklist /fi "imagename eq lock_frame.exe" 2>nul | find /i "lock_frame.exe" >nul
if not errorlevel 1 (
  echo [dump] lock_frame.exe is already running. Stop it with Ctrl+Alt+End first.
  pause
  exit /b 1
)

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "TS=%%i"
set "TAG=%LOGDIR%\tag_visual_%TS%"
set "LOG=%LOGDIR%\run_visual_%TS%.log"
if not exist "%TAG%\" mkdir "%TAG%" >nul 2>&1

echo [dump] log : %LOG%
echo [dump] tag : %TAG%
echo [dump] files: tag_XX_backbuffer.bmp / hudless.bmp / patch.bmp / ui.bmp / meta.txt
echo [dump] wait ~2-4 s in a HUD-heavy scene, then Ctrl+Alt+End to stop.
echo.

"%LF%" %COMMON% --hudless-stream --tag-dump 5 --tag-dump-after 120 --tag-dump-dir "%TAG%" > "%LOG%" 2>&1
echo exit=%errorlevel%
echo log: %LOG%
echo tag: %TAG%
start "" explorer "%TAG%"
pause
