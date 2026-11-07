@echo off
rem ===========================================================================
rem  mgpu ReShade add-on installer - forwards all args to install.ps1
rem
rem    install.bat "D:\Games\XXX\bin\x64"
rem    install.bat "D:\Games\XXX" d3d11
rem    or drag the game .exe onto this .bat
rem ===========================================================================
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
if errorlevel 1 pause
endlocal
