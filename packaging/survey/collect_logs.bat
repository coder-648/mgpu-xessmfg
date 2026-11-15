@echo off
rem Collect the logs of one survey run into a timestamped zip next to this file.
setlocal
set STAMP=%DATE:~0,4%%DATE:~5,2%%DATE:~8,2%-%TIME:~0,2%%TIME:~3,2%%TIME:~6,2%
set STAMP=%STAMP: =0%
set OUT=mgpu_logs_%STAMP%.zip
if not exist mgpu_capture.log if not exist mgpu_proxy.log (
  echo No mgpu logs here. Run this from the game directory that holds version.dll.
  pause & exit /b 1
)
if exist "C:\Windows\System32\tar.exe" (
  tar -a -c -f "%OUT%" mgpu_capture.log mgpu_proxy.log mgpu_capture.ini mgpu_proxy.ini 2>nul
) else (
  powershell -NoProfile -Command "Compress-Archive -Path 'mgpu_capture.log','mgpu_proxy.log','mgpu_capture.ini','mgpu_proxy.ini' -DestinationPath '%OUT%' -Force"
)
echo Created %OUT%
explorer .
pause
