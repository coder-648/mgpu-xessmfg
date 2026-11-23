@echo off
rem Copy this session's logs and tag dumps into results\<timestamp>\ and build
rem a short summary.txt.  Send the whole results folder back.
call "%~dp0ui__env.bat"

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "TS=%%i"
set "OUT=%RESDIR%\%TS%"
if not exist "%OUT%\" mkdir "%OUT%" >nul 2>&1

echo [collect] -> %OUT%
if exist "%LOGDIR%\" xcopy /e /i /y "%LOGDIR%" "%OUT%\consumer_logs" >nul 2>&1
if exist "%GAME_DIR%\ui_synth_dump\" xcopy /e /i /y "%GAME_DIR%\ui_synth_dump" "%OUT%\producer_synth_dump" >nul 2>&1
for %%F in (Cyberpunk2077_fakerFG.log Cyberpunk2077_mfg_producer.log fakerFG.ini) do (
  if exist "%GAME_DIR%\%%F" copy /y "%GAME_DIR%\%%F" "%OUT%\%%F" >nul 2>&1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0summary.ps1" -Out "%OUT%\summary.txt" -UiDir "%UI_DIR%" -GameDir "%GAME_DIR%"
if errorlevel 1 ( echo [collect] summary failed & pause & exit /b 1 )

echo [collect] done:
echo   %OUT%
echo   %OUT%\summary.txt
pause
