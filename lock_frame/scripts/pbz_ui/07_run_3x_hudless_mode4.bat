@echo off
rem Clean HUDless only, mode 4. Producer must have HudlessStream=1 (it is applied by 01).
call "%~dp0ui__env.bat"
tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if errorlevel 1 ( echo [h4] game not running & pause & exit /b 1 )
tasklist /fi "imagename eq lock_frame.exe" 2>nul | find /i "lock_frame.exe" >nul
if not errorlevel 1 ( echo [h4] stop the old consumer first & pause & exit /b 1 )
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "TS=%%i"
set "LOG=%LOGDIR%\run_h4_%TS%.log"
set "TAG=%LOGDIR%\tag_h4_%TS%"
if not exist "%TAG%\" mkdir "%TAG%" >nul 2>&1
echo [h4] log: %LOG%
"%LF%" %BASE% --hudless-stream --ui-source off --ui-mode5-auto --tag-dump 3 --tag-dump-after 120 --tag-dump-dir "%TAG%" > "%LOG%" 2>&1
echo exit=%errorlevel%
echo tag: %TAG%
pause
