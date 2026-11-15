@echo off
setlocal
cd /d "%~dp0"
set MODE=%~1
if "%MODE%"=="" set MODE=passthru
title mgpu marker norm source test (%MODE%)
set LOG=bridge_marker_norm_source_%MODE%.log

echo.
echo  MGPU MARKER NORM SOURCE TEST: %MODE%
echo  passthru = treat g_sr.color as already PQ-encoded (no re-encode)
echo  rawgamma = show source R11 values directly (gamma for visibility)
echo.
del /q marker_norm_*.on 2>nul
echo.>marker_norm_%MODE%.on
if exist "%LOG%" del /q "%LOG%"
echo Created marker_norm_%MODE%.on. Press any key to start the consumer.
pause >nul

xess_mfg_consumer.exe --game Cyberpunk2077.exe --den 1 --pool 4 --interp 1 --lock 40 --unlock-mfg --fg-rt-mode auto --fg-ui none --pub-fifo --report 40 --marker-probe --no-osd > "%LOG%" 2>&1
set RC=%ERRORLEVEL%

del /q marker_norm_%MODE%.on 2>nul
echo.
echo Consumer exited rc=%RC%
echo Log: %CD%\%LOG%
echo Send bridge_marker_norm_source_%MODE%.log + Cyberpunk2077_fakerFG.log
pause
