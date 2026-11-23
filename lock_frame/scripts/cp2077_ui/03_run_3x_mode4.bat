@echo off
rem Mode 4 (HUDless only): render-card synth UI is transported, then unblended
rem into HUDless; UI itself is not tagged.  Requires UiStream=1 + UiFullRes=1
rem in fakerFG.ini and a game restart after applying it.
call "%~dp0ui__env.bat"

tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if errorlevel 1 (
  echo [m4] %GAME_NAME% is not running. Start it, load a save, then retry.
  pause
  exit /b 1
)
tasklist /fi "imagename eq lock_frame.exe" 2>nul | find /i "lock_frame.exe" >nul
if not errorlevel 1 (
  echo [m4] lock_frame.exe is already running. Stop it with Ctrl+Alt+End first.
  pause
  exit /b 1
)

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "TS=%%i"
set "TAG=%LOGDIR%\tag_m4_%TS%"
set "LOG=%LOGDIR%\run_m4_%TS%.log"
if not exist "%TAG%\" mkdir "%TAG%" >nul 2>&1

echo [m4] log: %LOG%
echo [m4] tag: %TAG%
echo [m4] run 30-60 s in a scene with HUD text, then Ctrl+Alt+End to stop.
echo.

"%LF%" %COMMON% --ui-mode5-auto --tag-dump 5 --tag-dump-after 240 --tag-dump-dir "%TAG%" > "%LOG%" 2>&1
echo exit=%errorlevel%
echo log: %LOG%
echo tag: %TAG%
pause
