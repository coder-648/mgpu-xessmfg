@echo off
rem Sample the render GPU while the consumer is running. Usage:
rem   12_gpu_sample.bat on 60
rem   (then start 03/04 in another window during the 60 s)
call "%~dp0ui__env.bat"
set "LABEL=%~1"
if "%LABEL%"=="" set "LABEL=run"
set "SECS=%~2"
if "%SECS%"=="" set "SECS=60"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "TS=%%i"
set "OUT=%LOGDIR%\gpu_%LABEL%_%TS%.csv"
echo [gpu] sampling GPU 0 for %SECS%s -%OUT%
echo [gpu] start 03/04 (or 05) now in another window.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sample_gpu.ps1" -Seconds %SECS% -Out "%OUT%" -Index 0
pause
