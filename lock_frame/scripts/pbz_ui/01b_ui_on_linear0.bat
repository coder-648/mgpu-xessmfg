@echo off
rem Same as 01_ui_on.bat but UiLinear=0 (old PQ-code-space subtraction) for A/B.
call "%~dp0ui__env.bat"
tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if not errorlevel 1 (
  echo [ui] The game is running. Close it COMPLETELY first: the producer reads the ini once.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0apply_ui.ps1" -Mode linear0
if errorlevel 1 ( echo [ui] apply failed & pause & exit /b 1 )
echo.
echo [ui] ON with UiLinear=0 written. Restart the game before running mode 4/5.
pause
