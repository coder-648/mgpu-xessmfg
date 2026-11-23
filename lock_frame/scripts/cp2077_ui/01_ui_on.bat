@echo off
rem Enable the full-res UI path (linear-light subtraction, UI codec, 8 slots).
rem The producer reads fakerFG.ini once when the game starts -> restart the game.
call "%~dp0ui__env.bat"
tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if not errorlevel 1 (
  echo [ui] The game is running. Close it COMPLETELY first: the producer reads the ini once.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0apply_ui.ps1" -Mode on
if errorlevel 1 ( echo [ui] apply failed & pause & exit /b 1 )
echo.
echo [ui] ON written. Start the game, load a save, keep DLSS Frame Generation ON,
echo [ui] lock the game at 40 fps (RTSS / in-game cap), then run 03_run_3x_mode4.bat.
pause
