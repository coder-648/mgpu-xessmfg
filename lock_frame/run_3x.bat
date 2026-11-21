@echo off
rem ===========================================================================
rem  CP2077 固定 3x = 120 fps（120 Hz 面板；游戏自身/RTSS 锁 40 fps）
rem
rem  这就是当前定稿配方（与 C:\temp\mgpu\cp2077_port_run\run_3x.bat 一致）：
rem    --interp 2  --unlock-mfg  --mfg-model=keep2x
rem    --overlay-layered  --wgc-minint 0  --streams=mv,depth
rem
rem  用法：游戏先进到主菜单/存档，再运行本脚本（lock_frame.exe 放在同一目录）。
rem ===========================================================================
setlocal
cd /d "%~dp0"

if not exist "%~dp0lock_frame.exe" (
  echo [3x] lock_frame.exe not found in %cd%
  pause
  exit /b 1
)

tasklist /fi "imagename eq Cyberpunk2077.exe" | find /i "Cyberpunk2077.exe" >nul
if errorlevel 1 (
  echo [3x] Cyberpunk2077.exe is not running - start the game first, then run this.
  pause
  exit /b 1
)

set "LOG=%~dp0lock_frame_3x.log"
echo [3x] log: %LOG%

lock_frame.exe --game Cyberpunk2077.exe --mode 3x --interp 2 ^
  --streams=mv,depth --overlay-layered --wgc-minint 0 ^
  --pool 4 --latency 1 --unlock-mfg --mfg-model=keep2x ^
  --fg-rt-mode auto --report 40 > "%LOG%" 2>&1

echo exit=%errorlevel%
echo log: %LOG%
pause
