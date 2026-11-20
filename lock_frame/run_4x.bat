@echo off
rem ===========================================================================
rem  固定 4x = 120 fps（120 Hz 面板；游戏锁 30 fps）。
rem  4x 需要 --unlock-mfg（只对实测过的 libxess_fg/libxell 版本有效）。
rem  --game 按需要改成你的游戏 exe；也可以用 lock_frame_ui.exe 自动选窗口。
rem ===========================================================================
setlocal
cd /d "%~dp0"
"%~dp0lock_frame.exe" --game Cyberpunk2077.exe --mode 4x --interp 3 ^
  --unlock-mfg --fg-rt-mode auto --report 40 --pool 4 --latency 1 ^
  > "%~dp0lock_frame_4x.log" 2>&1
