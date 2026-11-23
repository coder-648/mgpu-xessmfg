@echo off
rem Baseline: stream 3 off (old HUDless-only path). Restart the game afterwards.
call "%~dp0ui__env.bat"
tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if not errorlevel 1 (
  echo [ui] The game is running. Close it COMPLETELY first: the producer reads the ini once.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0apply_ui.ps1" -Mode off
if errorlevel 1 ( echo [ui] apply failed & pause & exit /b 1 )
echo.
echo [ui] OFF written (UiStream=0, UiSource=Off). Restart the game, then run
echo [ui] 05_run_3x_ui_off.bat as the baseline.
pause
