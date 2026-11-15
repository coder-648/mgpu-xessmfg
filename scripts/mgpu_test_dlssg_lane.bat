@echo off
setlocal
cd /d "%~dp0"
title mgpu DLSSG-lane A/B
set LOG=bridge_dlssg_lane.log

if not exist fakerFG.ini.bak copy /y fakerFG.ini fakerFG.ini.bak >nul
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$p='fakerFG.ini'; $t=Get-Content -LiteralPath $p; $t = $t -replace '^FgInput=.*','FgInput=DLSSG' -replace '^FakeSlDlssg=.*','FakeSlDlssg=0'; Set-Content -LiteralPath $p -Value $t -Encoding ASCII"
echo.
echo  fakerFG is now FgInput=DLSSG, FakeSlDlssg=0.
echo  This is the lane where the title's sl.dlss_g plugin drives our NGX
echo  provider and DLSSG.Backbuffer / HUDless actually contain the picture.
echo  Press any key to start the consumer.
pause >nul
if exist "%LOG%" del /q "%LOG%"

xess_mfg_consumer.exe --game Cyberpunk2077.exe --den 1 --pool 4 --interp 1 --lock 40 --unlock-mfg --fg-rt-mode auto --fg-ui none --pub-fifo --report 40 --marker-probe --no-osd > "%LOG%" 2>&1
set RC=%ERRORLEVEL%

if exist fakerFG.ini.bak move /y fakerFG.ini.bak fakerFG.ini >nul
echo.
echo Consumer exited rc=%RC%  (fakerFG.ini restored)
echo Log: %CD%\%LOG%
echo Producer log: %CD%\Cyberpunk2077_fakerFG.log
pause
