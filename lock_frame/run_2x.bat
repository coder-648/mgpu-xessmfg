@echo off
rem ===========================================================================
rem  固定 2x = 120 fps（120 Hz 面板；游戏锁 60 fps）。
rem  2x 不需要 --unlock-mfg。
rem  --game 按需要改成你的游戏 exe；也可以用 lock_frame_ui.exe 自动选窗口。
rem ===========================================================================
setlocal
cd /d "%~dp0"
"%~dp0lock_frame.exe" --game Cyberpunk2077.exe --mode 2x --interp 1 ^
  --fg-ui none --fg-rt-mode auto --report 40 --pool 4 --latency 1 ^
  > "%~dp0lock_frame_2x.log" 2>&1
