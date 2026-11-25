@echo off
setlocal
cd /d "%~dp0"
rem ===========================================================================
rem  Same 3x baseline as audit_1, but the consumer runs from its own folder
rem  (C:\temp\lock_frame_audit) - the layout the release zip ships with, where
rem  dxgi.dll (fakerFG) is NOT next to lock_frame.exe and therefore is not
rem  injected into the consumer process.
rem ===========================================================================
set "LF_DIR=C:\temp\lock_frame_audit"
call "%~dp0audit_env.bat"
if errorlevel 1 exit /b 1
set "LOG=%LOG_DIR%\audit_out_3x_keep2x.log"
echo [audit] out-of-game 3x / keep2x / default capture
echo [audit] consumer: %LF%
echo [audit] log: %LOG%
echo [audit] after this run, check that NO new "==== fakerFG" session appears in
echo [audit]   lock_frame_fakerFG.log for this start time (proves the proxy is out).
echo [audit] leave the game in-world, turn the camera for ~60 s, then Ctrl+Alt+End.
echo.
"%LF%" --game %GAME_NAME% --mode 3x --interp 2 --unlock-mfg --mfg-model=keep2x %CAPTURE% --game-quirks apply > "%LOG%" 2>&1
echo.
echo [audit] exit=%errorlevel%
echo [audit] log: %LOG%
pause
