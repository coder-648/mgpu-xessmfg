@echo off
rem Disable producer-side P/Hmask/U/Scaling dump after 14_dump_producer_on.bat.
call "%~dp0ui__env.bat"

tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if not errorlevel 1 (
  echo [dump] close %GAME_NAME% first, then run this again.
  pause
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0set_ini_dump.ps1" -Ini "%INI%" -UiSynthDump 0 -After 30
if errorlevel 1 ( pause & exit /b 1 )
echo [dump] producer dump disabled (UiSynthDump=0). Restart the game to apply.
pause
