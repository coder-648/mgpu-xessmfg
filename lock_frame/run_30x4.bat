@echo off
rem ===========================================================================
rem  锁帧 30 x 4 = 120 fps（120 Hz 面板）。
rem  先把游戏锁到 30 fps（RTSS / 游戏内限制器），再运行本文件。
rem  --hwnd 由 lock_frame_ui.exe 自动填；这里用前台窗口作为目标。
rem ===========================================================================
setlocal
cd /d "%~dp0"
"%~dp0lock_frame.exe" --game Cyberpunk2077.exe --mode 30x4 --interp 3 ^
  --unlock-mfg --fg-ui none --fg-rt-mode auto --report 40 --pool 4 --latency 1 ^
  > "%~dp0lock_frame_30x4.log" 2>&1
