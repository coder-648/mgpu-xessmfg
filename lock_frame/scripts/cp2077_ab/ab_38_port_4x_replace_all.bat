@echo off
setlocal
call "%~dp0ab_port_env.bat"
if errorlevel 1 (
  echo.
  echo [AB] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
set "LOG=%AB_LOG%\ab_38_port_4x_replace_all.log"
echo.
echo [AB] TEST 38 - 4x port, all 7 shaders replaced (no Intel U-Nets kept)
echo [AB] if this is cleaner, the two kept Intel U-Nets are the problem on AMD
echo [AB] log: %LOG%
echo [AB] in game: borderless, DLSS on, lock 30 fps (120 Hz panel, 4x)
echo [AB] stop with Ctrl+Alt+End, or close this window
echo.
lock_frame.exe --game %GAME_NAME% --mode 4x --interp 3 --unlock-mfg --mfg-model=port --mfg-replace-all %CAPTURE% > "%LOG%" 2>&1
echo.
echo [AB] exit=%errorlevel%
echo [AB] log: %LOG%
pause
