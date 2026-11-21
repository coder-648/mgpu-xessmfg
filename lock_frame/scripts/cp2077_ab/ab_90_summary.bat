@echo off
setlocal
call "%~dp0ab__env.bat" >nul 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ab_summary.ps1" -LogDir "%~dp0ab_logs" -Out "%~dp0ab_logs\summary.txt"
echo.
echo [AB] summary written: %~dp0ab_logs\summary.txt
pause
