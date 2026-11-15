@echo off
rem ===========================================================================
rem  Two-GPU frame generation: start the consumer (the window that is displayed).
rem
rem  1. edit GAME_EXE below to the game's process image name
rem  2. keep the game running, borderless/windowed fullscreen, at the SAME
rem     resolution as the monitor (1:1 - the consumer does not guess)
rem  3. cap the GAME to 40 fps in the driver (25 ms = 3 x 8.33 ms @120 Hz)
rem  4. double-click this file
rem
rem  Extra switches (add after --no-osd): --no-depth  --no-mv  --report 40
rem ===========================================================================
setlocal
cd /d "%~dp0"

set "GAME_EXE=Cyberpunk2077.exe"
set "LOG=mgpu_consumer.log"

xess_mfg_consumer.exe --game %GAME_EXE% --den 1 --pool 4 --interp 1 --lock 0 ^
  --unlock-mfg --fg-rt-mode auto --fg-ui none --pub-fifo --report 40 ^
  --no-osd > "%LOG%" 2>&1

echo.
echo consumer exited, log: %CD%\%LOG%
pause
