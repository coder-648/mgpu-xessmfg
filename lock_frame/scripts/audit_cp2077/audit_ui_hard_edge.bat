@echo off
setlocal
cd /d "%~dp0"
call "%~dp0audit_env.bat"
if errorlevel 1 exit /b 1
set "LOG=%LOG_DIR%\audit_ui_hard_edge.log"
echo [audit] UI mode 4 A/B: --ui-patch-hard-edge (5x5 max dilation, no box feather)
echo [audit] same flags as audit_2_3x_keep2x_ui plus the hard-edge switch.
echo [audit] log: %LOG%
echo [audit] compare against audit_2_3x_keep2x_ui.log on the same scene.
echo [audit] leave the game in-world, turn the camera and move near small UI
echo [audit]   (crosshair, minimap icons) for ~60 s, then Ctrl+Alt+End.
echo.
"%LF%" --game %GAME_NAME% --mode 3x --interp 2 --unlock-mfg --mfg-model=keep2x %CAPTURE% --game-quirks apply --hudless-stream --ui-source off --ui-patch-hard-edge > "%LOG%" 2>&1
echo.
echo [audit] exit=%errorlevel%
echo [audit] log: %LOG%
pause
