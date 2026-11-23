@echo off
rem Cost baseline: UI off, HUDless stream off. Close the game first; this rewrites the ini.
call "%~dp0ui__env.bat"
tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if not errorlevel 1 ( echo [cost-off] close the game COMPLETELY first & pause & exit /b 1 )
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0apply_ui.ps1" -Mode off
echo.
echo [cost-off] UI off written. Restart the game, load the SAME save, lock 40 fps,
echo [cost-off] then run 05_run_3x_ui_off.bat and sample the NVIDIA GPU.
pause
