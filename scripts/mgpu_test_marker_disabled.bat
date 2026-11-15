@echo off
setlocal
cd /d "%~dp0"
title mgpu marker disabled test
set LOG=bridge_marker_disabled.log

echo.
echo  MGPU MARKER DISABLED TEST
echo  Stops drawing the marker but keeps norm/readback alive.
echo  If the blue block disappears and tile pixels become scene colours,
echo  the producer source tile was being polluted by the previous marker.
echo.
del /q marker_norm_*.on 2>nul
echo.>marker_disable.on
if exist "%LOG%" del /q "%LOG%"
echo marker_disable.on created. Press any key to start the consumer.
pause >nul

xess_mfg_consumer.exe --game Cyberpunk2077.exe --den 1 --pool 4 --interp 1 --lock 40 --unlock-mfg --fg-rt-mode auto --fg-ui none --pub-fifo --report 40 --marker-probe --no-osd > "%LOG%" 2>&1
set RC=%ERRORLEVEL%

del /q marker_disable.on 2>nul
echo.
echo Consumer exited rc=%RC%
echo Log: %CD%\%LOG%
pause
