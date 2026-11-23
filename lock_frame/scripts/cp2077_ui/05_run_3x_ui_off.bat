@echo off
rem Baseline for A/B: old HUDless-only path, no UI stream.  Requires the game
rem to have been restarted after 02_ui_off.bat.
call "%~dp0ui__env.bat"

tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if errorlevel 1 (
  echo [base] %GAME_NAME% is not running. Start it, load a save, then retry.
  pause
  exit /b 1
)
tasklist /fi "imagename eq lock_frame.exe" 2>nul | find /i "lock_frame.exe" >nul
if not errorlevel 1 (
  echo [base] lock_frame.exe is already running. Stop it with Ctrl+Alt+End first.
  pause
  exit /b 1
)

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "TS=%%i"
set "LOG=%LOGDIR%\run_base_%TS%.log"

echo [base] log: %LOG%
echo [base] run 30-60 s in the same scene, then Ctrl+Alt+End.
echo.

"%LF%" %BASE% --ui-source off > "%LOG%" 2>&1
echo exit=%errorlevel%
echo log: %LOG%
pause
