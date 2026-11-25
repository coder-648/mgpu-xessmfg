@echo off
setlocal
cd /d "%~dp0"
call "%~dp0audit_env.bat"
if errorlevel 1 exit /b 1
set "LOG=%LOG_DIR%\audit_1_3x_keep2x.log"
echo [audit] 1/3  3x / keep2x / default capture   (baseline A)
echo [audit] same flags as compat_3x_default.bat, so it compares 1:1 with the
echo [audit]   2026-11-25 logs already in compat_logs\.
echo [audit] log: %LOG%
echo [audit] leave the game in-world, turn the camera for ~60 s, then Ctrl+Alt+End.
echo.
"%LF%" --game %GAME_NAME% --mode 3x --interp 2 --unlock-mfg --mfg-model=keep2x %CAPTURE% --game-quirks apply > "%LOG%" 2>&1
echo.
echo [audit] exit=%errorlevel%
echo [audit] log: %LOG%
pause
