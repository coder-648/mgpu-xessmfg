@echo off
setlocal
cd /d "%~dp0"
call "%~dp0audit_env.bat"
if errorlevel 1 exit /b 1
set "LOG=%LOG_DIR%\audit_2_3x_keep2x_ui.log"
echo [audit] 2/3  3x / keep2x / UI patch path (mode 4: --hudless-stream)
echo [audit] this is the path the shipped UI uses when UI composition is on
echo [audit]   (producer UiPatch=1 -> stream 4 patch -> consumer composes H').
echo [audit] log: %LOG%
echo [audit] leave the game in-world, turn the camera for ~60 s, then Ctrl+Alt+End.
echo.
"%LF%" --game %GAME_NAME% --mode 3x --interp 2 --unlock-mfg --mfg-model=keep2x %CAPTURE% --game-quirks apply --hudless-stream --ui-source off > "%LOG%" 2>&1
echo.
echo [audit] exit=%errorlevel%
echo [audit] log: %LOG%
pause
