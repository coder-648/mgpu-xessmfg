@echo off
rem Cost measurement: UI on (only UI stream, no HUDless stream).
call "%~dp0ui__env.bat"
tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if not errorlevel 1 ( echo [cost-on] close the game COMPLETELY first & pause & exit /b 1 )
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0apply_ui.ps1" -Mode on
echo.
echo [cost-on] UI on written. Restart the game, load the SAME save, lock 40 fps,
echo [cost-on] then run 03_run_3x_mode4.bat and sample the NVIDIA GPU.
pause
