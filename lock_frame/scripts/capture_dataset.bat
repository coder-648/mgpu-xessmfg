@echo off
rem ===========================================================================
rem  capture_dataset.bat  -  double-click wrapper for capture_dataset.ps1
rem
rem    capture_dataset.bat
rem    capture_dataset.bat -Tag street -Frames 48
rem    capture_dataset.bat -Tag hud150 -Frames 32 -Percent 150
rem    capture_dataset.bat -DryRun
rem
rem  Options and details: see the header of capture_dataset.ps1 and
rem  docs/design/LOSSLESS_TRANSPORT.md section 11.5
rem
rem  NOTE: keep this file ASCII-only (cmd.exe does not understand a UTF-8 BOM).
rem ===========================================================================
setlocal
set "HERE=%~dp0"
set "PSX=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PSX%" set "PSX=powershell.exe"
"%PSX%" -NoProfile -ExecutionPolicy Bypass -File "%HERE%capture_dataset.ps1" %*
set "RC=%ERRORLEVEL%"
echo.
echo [capture] exit code %RC%
pause
exit /b %RC%
