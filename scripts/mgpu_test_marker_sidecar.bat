@echo off
setlocal
cd /d "%~dp0"
title mgpu marker sidecar test
set LOG=bridge_marker_sidecar.log

echo.
echo ============================================================
echo  MGPU MARKER SIDECAR TEST
echo  dir: %CD%
echo  log: %LOG%
echo ============================================================
echo.
echo  This test expects Cyberpunk 2077 to already be running.
echo  Load an in-world save, keep the game window active.
echo.
echo  Consumer:
echo    exact seq marker (16x16, one bit per 2x2 cell)
echo    scar tile rides the MV/depth publication (streams[4], GPU copy)
echo    no OSD
echo.
echo  Files used (marker is ON by default; marker_disable.on turns it off):
echo    xess_mfg_consumer.exe
echo    xess_mfg_producer.dll
echo    dxgi.dll
echo.
pause

echo.
echo ============================================================
echo  HASHES
echo ============================================================
powershell -NoProfile -Command "Get-FileHash xess_mfg_consumer.exe, xess_mfg_producer.dll, dxgi.dll -Algorithm SHA256 | Format-Table -AutoSize"
echo.

if exist "%LOG%" del /q "%LOG%"
echo Starting consumer...
echo.

rem --lock 0 (not 40): with --lock 40 the consumer drops captured frames whenever the game
rem   runs below 40 fps, which is what produced 151 hitches in a run. Cap the GAME to 40 fps
rem   in the driver instead and let the consumer follow it.
xess_mfg_consumer.exe --game Cyberpunk2077.exe --den 1 --pool 4 --interp 1 --lock 0 --unlock-mfg --fg-rt-mode auto --fg-ui none --pub-fifo --report 40 --marker-probe --no-osd > "%LOG%" 2>&1
set RC=%ERRORLEVEL%

echo.
echo ============================================================
echo  CONSUMER EXITED rc=%RC%
echo  log: %CD%\%LOG%
echo ============================================================
echo.
echo  Send these files:
echo    %LOG%
echo    Cyberpunk2077_fakerFG.log
echo    xess_mfg_producer.log
echo.
echo  Key lines to check:
echo    marker probe: 16x16 R10 scar tile ready (GPU-^>GPU into the publication)
echo    marker probe: scar tile rides the MV/depth publication (16x16)
echo    marker code: seq=... crc=... ok
echo    scar restore: seq=... slot=... at x,y (16x16)
echo    marker: decoded=... dup=... fallback=... miss=0
echo    marker decode: no contrast (lo=... hi=...) / crc=... seq=...
echo.
pause
