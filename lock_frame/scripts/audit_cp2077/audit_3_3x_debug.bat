@echo off
setlocal
cd /d "%~dp0"
call "%~dp0audit_env.bat"
if errorlevel 1 exit /b 1
set "LOG=%LOG_DIR%\audit_3_3x_debug.log"
echo [audit] 3/3  3x / keep2x / --d3d-debug  (validates D: barrier StateBefore)
echo [audit] the debug layer costs fps; this run is about VALIDATION MESSAGES,
echo [audit]   not about the frame rate. 30-45 s in-world is enough.
echo [audit] log: %LOG%
echo [audit] look for "statebefore", "RESOURCE_BARRIER", "INVALID_CALL", "DEVICE_REMOVED".
echo.
"%LF%" --game %GAME_NAME% --mode 3x --interp 2 --unlock-mfg --mfg-model=keep2x %CAPTURE% --game-quirks apply --d3d-debug > "%LOG%" 2>&1
echo.
echo [audit] exit=%errorlevel%
echo [audit] log: %LOG%
pause
