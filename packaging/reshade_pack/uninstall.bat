@echo off
rem ===========================================================================
rem  mgpu ReShade add-on uninstaller - forwards all args to uninstall.ps1
rem
rem    uninstall.bat "D:\Games\XXX\bin\x64"
rem    or drag the game .exe onto this .bat
rem ===========================================================================
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" %*
if errorlevel 1 pause
endlocal
