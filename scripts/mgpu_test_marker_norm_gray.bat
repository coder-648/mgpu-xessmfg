@echo off
setlocal
cd /d "%~dp0"
title mgpu marker norm grey test
set LOG=bridge_marker_norm_gray.log

echo.
echo  MGPU MARKER NORM GREY TEST
echo  Creates marker_norm_gray.on, then runs the normal marker sidecar test.
echo  If the centre block turns grey, producer normalization + consumer copy
echo  are working; if it stays blue, the old colour path is still active.
echo.
if exist marker_norm_gray.on del /q marker_norm_gray.on
echo.>marker_norm_gray.on
if exist "%LOG%" del /q "%LOG%"
echo.
echo marker_norm_gray.on created. Restart Cyberpunk 2077 now, load a save,
echo then press any key here to start the consumer.
pause >nul

echo Starting consumer with producer norm debug grey...
xess_mfg_consumer.exe --game Cyberpunk2077.exe --den 1 --pool 4 --interp 1 --lock 40 --unlock-mfg --fg-rt-mode auto --fg-ui none --pub-fifo --report 40 --marker-probe --no-osd > "%LOG%" 2>&1
set RC=%ERRORLEVEL%

del /q marker_norm_gray.on 2>nul
echo.
echo Consumer exited rc=%RC%
echo Log: %CD%\%LOG%
echo Send bridge_marker_norm_gray.log + Cyberpunk2077_fakerFG.log
pause
