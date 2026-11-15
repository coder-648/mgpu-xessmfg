@echo off
rem ===========================================================================
rem  锁帧 40 x 3 = 120 fps（120 Hz 面板）。
rem  先把游戏锁到 40 fps（RTSS / 游戏内限制器），再运行本文件。
rem ===========================================================================
setlocal
cd /d "%~dp0"
"%~dp0lock_frame.exe" --game Cyberpunk2077.exe --mode 40x3 --interp 2 ^
  --unlock-mfg --fg-ui none --fg-rt-mode auto --report 40 --pool 4 --latency 1 ^
  > "%~dp0lock_frame_40x3.log" 2>&1
