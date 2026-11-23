@echo off
rem v33: enable the HUDless support patch (UiPatch=1). The producer reads fakerFG.ini
rem once when the game starts -> close the game first, then restart it afterwards.
call "%~dp0ui__env.bat"
tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if not errorlevel 1 (
  echo [patch] The game is running. Close it COMPLETELY first: the producer reads the ini once.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0apply_ui.ps1" -Mode patch
if errorlevel 1 ( echo [patch] apply failed & pause & exit /b 1 )
echo.
echo [patch] UiPatch=1 written. Start the game, load a save, keep DLSS Frame Generation ON,
echo [patch] lock the game at 40 fps, then run 09_run_3x_patch_mode4.bat.
pause
