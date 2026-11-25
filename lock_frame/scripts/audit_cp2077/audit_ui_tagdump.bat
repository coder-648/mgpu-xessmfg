@echo off
setlocal
cd /d "%~dp0"
call "%~dp0audit_env.bat"
if errorlevel 1 exit /b 1
rem Space-free path on purpose: the dump dir is passed on the command line.
set "DUMP=C:\temp\lock_frame_audit\tagdump"
if exist "%DUMP%\" rmdir /s /q "%DUMP%" >nul 2>&1
mkdir "%DUMP%" >nul 2>&1
set "LOG=%LOG_DIR%\audit_ui_tagdump.log"
echo [audit] UI mode 4 - dump 8 consecutive frames of what XeFG receives:
echo [audit]   tag_XX_backbuffer.bmp  the presented picture P (UI inside)
echo [audit]   tag_XX_hudless.bmp    the rebuilt HUDless H'  (this is the reconstruction)
echo [audit]   tag_XX_patch.bmp      the transported patch (black tiles = copied from P)
echo [audit]   tag_XX_alpha.bmp      the support mask
echo [audit]   tag_XX_diff.bmp       abs(P-H') x8  (bright = still differs)
echo [audit] dir: %DUMP%
echo.
echo [audit] the dump starts at frame 300 (~8 s in) and takes 8 consecutive frames.
echo [audit] before it starts, get a SMALL UI moving: sweep the crosshair over a
echo [audit]   high-contrast edge, or hover a small icon / damage number.
echo [audit] stop with Ctrl+Alt+End after "tag-dump complete" appears in the log.
echo.
"%LF%" --game %GAME_NAME% --mode 3x --interp 2 --unlock-mfg --mfg-model=keep2x %CAPTURE% --game-quirks apply --hudless-stream --ui-source off --tag-dump 8 --tag-dump-after 300 --tag-dump-dir "%DUMP%" > "%LOG%" 2>&1
echo.
echo [audit] exit=%errorlevel%
echo [audit] log: %LOG%
echo [audit] images: %DUMP%
pause
