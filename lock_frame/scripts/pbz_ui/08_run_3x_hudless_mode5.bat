@echo off
rem Clean HUDless + synth UI, forced mode 5. Producer needs HudlessStream=1 + UiStream=1.
rem If XeFG rejects the UI format, set UiAlpha8=0 so UI and HUDless are both R10.
call "%~dp0ui__env.bat"
tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if errorlevel 1 ( echo [h5] game not running & pause & exit /b 1 )
tasklist /fi "imagename eq lock_frame.exe" 2>nul | find /i "lock_frame.exe" >nul
if not errorlevel 1 ( echo [h5] stop the old consumer first & pause & exit /b 1 )
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "TS=%%i"
set "LOG=%LOGDIR%\run_h5_%TS%.log"
set "TAG=%LOGDIR%\tag_h5_%TS%"
if not exist "%TAG%\" mkdir "%TAG%" >nul 2>&1
echo [h5] log: %LOG%
"%LF%" %COMMON% --hudless-stream --ui-mode5 --tag-dump 3 --tag-dump-after 120 --tag-dump-dir "%TAG%" > "%LOG%" 2>&1
echo exit=%errorlevel%
echo tag: %TAG%
pause
