@echo off
rem ===========================================================================
rem  ab_99_camvm_check.bat - wrapper for ab_99_camvm_check.ps1
rem  Reads the logs written by the last session; the game may stay closed.
rem  Optional: ab_99_camvm_check.bat -GameDir "E:\...\bin\x64"
rem ===========================================================================
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ab_99_camvm_check.ps1" %*
echo.
pause
