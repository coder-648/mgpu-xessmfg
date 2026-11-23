@echo off
rem Mode 5 (HUDless + UI): tags HUDless and UI together. This is the default since
rem 2026-11-23; this script just pins it as its own A/B arm.
call "%~dp0ui__env.bat"

tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if errorlevel 1 (
  echo [m5] %GAME_NAME% is not running. Start it, load a save, then retry.
  pause
  exit /b 1
)
tasklist /fi "imagename eq lock_frame.exe" 2>nul | find /i "lock_frame.exe" >nul
if not errorlevel 1 (
  echo [m5] lock_frame.exe is already running. Stop it with Ctrl+Alt+End first.
  pause
  exit /b 1
)

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "TS=%%i"
set "TAG=%LOGDIR%\tag_m5_%TS%"
set "LOG=%LOGDIR%\run_m5_%TS%.log"
if not exist "%TAG%\" mkdir "%TAG%" >nul 2>&1

echo [m5] log: %LOG%
echo [m5] tag: %TAG%
echo [m5] run 30-60 s in the same scene as mode 4, then Ctrl+Alt+End.
echo.

"%LF%" %COMMON% --ui-mode5 --tag-dump 5 --tag-dump-after 240 --tag-dump-dir "%TAG%" > "%LOG%" 2>&1
echo exit=%errorlevel%
echo log: %LOG%
echo tag: %TAG%
pause
