@echo off
setlocal
cd /d "%~dp0"
title mgpu marker norm red test
set LOG=bridge_marker_norm_red.log

echo.
echo  MGPU MARKER NORM RED TEST
echo  Produces a pure red R10 tile. If the on-screen block is blue, the R10
echo  channels are being swapped between producer and consumer. If it is red,
echo  producer/consumer channels are correct and the blue comes from the source.
echo.
if exist marker_norm_red.on del /q marker_norm_red.on
if exist marker_norm_gray.on del /q marker_norm_gray.on
if exist marker_norm_green.on del /q marker_norm_green.on
echo.>marker_norm_red.on
if exist "%LOG%" del /q "%LOG%"
echo.
echo marker_norm_red.on created. Press any key to start the consumer.
echo You do NOT need to restart the game with this build.
pause >nul

echo Starting consumer with producer norm debug red...
xess_mfg_consumer.exe --game Cyberpunk2077.exe --den 1 --pool 4 --interp 1 --lock 40 --unlock-mfg --fg-rt-mode auto --fg-ui none --pub-fifo --report 40 --marker-probe --no-osd > "%LOG%" 2>&1
set RC=%ERRORLEVEL%

del /q marker_norm_red.on 2>nul
echo.
echo Consumer exited rc=%RC%
echo Log: %CD%\%LOG%
echo Send bridge_marker_norm_red.log + Cyberpunk2077_fakerFG.log
pause
