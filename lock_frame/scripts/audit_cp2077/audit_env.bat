@echo off
rem ===========================================================================
rem  lock_frame consumer self-audit - shared environment + preflight (2026-11-25)
rem
rem  Regression for lock_frame/docs/SELF_AUDIT_2026-11-25.md A/C/D/E/F:
rem    A  queue-side wait on the producer's fwd[slot] before reading MV/depth
rem    C  upsample pair keyed by publication (not just slot)
rem    D  record_upsample() barrier StateBefore COPY_DEST -> COMMON
rem    E  prefetch disabled when a codec stream is in use
rem    F  --lock + --jbuf no longer leaks colour textures
rem
rem  Order: audit_0_info -> audit_1_3x_keep2x -> audit_2_3x_keep2x_ui
rem         -> audit_3_3x_debug -> audit_9_report
rem
rem  Every audit_*.bat calls this. Do not add setlocal here: the caller needs
rem  GAME_NAME / LF / LOG_DIR / CAPTURE from this scope.
rem ===========================================================================
set "GAME_DIR=E:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64"
set "GAME_EXE=%GAME_DIR%\Cyberpunk2077.exe"
set "GAME_NAME=Cyberpunk2077.exe"
rem The shipped layout runs lock_frame.exe from its own folder (the release zip),
rem NOT from the game directory. If it sits next to dxgi.dll, Windows loads the
rem fakerFG proxy into the consumer process as well (second XeFG context + XeLL +
rem Present hooks). audit_out_3x_keep2x.bat sets LF_DIR to the external copy.
if not defined LF_DIR set "LF_DIR=%GAME_DIR%"
set "LF=%LF_DIR%\lock_frame.exe"
set "LOG_DIR=%GAME_DIR%\audit_logs"
if not exist "%LOG_DIR%\" mkdir "%LOG_DIR%" >nul 2>&1

rem Same capture recipe as compat_3x_default.bat, so the new logs are directly
rem comparable with the 2026-11-25 baseline runs already in compat_logs\.
set "CAPTURE=--streams=mv,depth --overlay-layered --wgc-minint 0 --pool 4 --latency 1 --fg-rt-mode auto --report 40"

set "AUDIT_RC=0"
if not exist "%LF%" ( echo [audit] MISSING lock_frame.exe & set "AUDIT_RC=1" )
if not exist "%GAME_EXE%" ( echo [audit] MISSING Cyberpunk2077.exe & set "AUDIT_RC=1" )
if not exist "%GAME_DIR%\dxgi.dll" ( echo [audit] fakerFG is not deployed ^(dxgi.dll missing^) & set "AUDIT_RC=1" )
if not exist "%GAME_DIR%\fakerFG.dll" ( echo [audit] fakerFG.dll missing & set "AUDIT_RC=1" )
if not exist "%GAME_DIR%\xess_mfg_producer.dll" ( echo [audit] xess_mfg_producer.dll missing & set "AUDIT_RC=1" )
if not exist "%GAME_DIR%\fakerFG.ini" ( echo [audit] fakerFG.ini missing & set "AUDIT_RC=1" )
if not exist "%GAME_DIR%\fakerFG\libxess_fg.dll" ( echo [audit] fakerFG\libxess_fg.dll missing & set "AUDIT_RC=1" )
if exist "%GAME_DIR%\version.dll" ( echo [audit] WARNING: version.dll present - remove it before testing & set "AUDIT_RC=1" )

findstr /i /b /c:"FgInput=DLSSG" "%GAME_DIR%\fakerFG.ini" >nul 2>&1
if errorlevel 1 ( echo [audit] fakerFG.ini is not FgInput=DLSSG & set "AUDIT_RC=1" )
findstr /i /b /c:"FgOutput=Capture" "%GAME_DIR%\fakerFG.ini" >nul 2>&1
if errorlevel 1 ( echo [audit] fakerFG.ini is not FgOutput=Capture & set "AUDIT_RC=1" )

tasklist /fi "imagename eq lock_frame.exe" 2>nul | find /i "lock_frame.exe" >nul
if not errorlevel 1 ( echo [audit] lock_frame.exe is already running - stop it with Ctrl+Alt+End & set "AUDIT_RC=1" )
tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if errorlevel 1 ( echo [audit] %GAME_NAME% is NOT running - start it, load a save, DLSS Frame Generation ON & set "AUDIT_RC=1" )

if not "%AUDIT_RC%"=="0" (
  echo.
  echo [audit] preflight failed - fix the line above, then click again.
  pause
  exit /b 1
)
exit /b 0
