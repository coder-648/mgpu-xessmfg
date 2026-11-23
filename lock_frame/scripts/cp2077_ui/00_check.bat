@echo off
rem Preflight: files, config, processes.  Run this before every test session.
call "%~dp0ui__env.bat"
set "RC=0"

echo ============================================================
echo  CP2077 UI composition - preflight
echo ============================================================
echo game dir : %GAME_DIR%
echo consumer : %LF%
echo ini      : %INI%
echo.

if not exist "%GAME_EXE%"                        ( echo [FAIL] missing %GAME_EXE% & set "RC=1" )
if not exist "%GAME_DIR%\dxgi.dll"               ( echo [FAIL] missing dxgi.dll & set "RC=1" )
if not exist "%GAME_DIR%\fakerFG.dll"            ( echo [FAIL] missing fakerFG.dll & set "RC=1" )
if not exist "%GAME_DIR%\nvngx_dlssg.dll"        ( echo [FAIL] missing nvngx_dlssg.dll & set "RC=1" )
if not exist "%GAME_DIR%\xess_mfg_producer.dll"  ( echo [FAIL] missing xess_mfg_producer.dll & set "RC=1" )
if not exist "%LF%"                              ( echo [FAIL] missing lock_frame.exe & set "RC=1" )
if not exist "%INI%"                             ( echo [FAIL] missing fakerFG.ini & set "RC=1" )
if not exist "%GAME_DIR%\fakerFG\libxess_fg.dll" ( echo [FAIL] missing fakerFG\libxess_fg.dll & set "RC=1" )
if not exist "%GAME_DIR%\libxell.dll"            ( echo [FAIL] missing libxell.dll & set "RC=1" )
if exist "%GAME_DIR%\version.dll"                ( echo [FAIL] version.dll is present - it conflicts with this chain & set "RC=1" )

echo.
echo --- deployed binaries -------------------------------------
dir /b "%GAME_DIR%\dxgi.dll" "%GAME_DIR%\fakerFG.dll" "%GAME_DIR%\nvngx_dlssg.dll" "%GAME_DIR%\xess_mfg_producer.dll" 2>nul
for %%F in ("%GAME_DIR%\dxgi.dll" "%GAME_DIR%\fakerFG.dll" "%GAME_DIR%\nvngx_dlssg.dll" "%GAME_DIR%\xess_mfg_producer.dll" "%LF%") do @echo   %%~nxF  %%~tF  %%~zF bytes

echo.
echo --- byte identity (dxgi == fakerFG == nvngx_dlssg) -------
fc /b "%GAME_DIR%\fakerFG.dll" "%GAME_DIR%\dxgi.dll" >nul 2>&1
if errorlevel 1 ( echo [FAIL] dxgi.dll != fakerFG.dll & set "RC=1" ) else ( echo [ok] dxgi.dll == fakerFG.dll )
fc /b "%GAME_DIR%\fakerFG.dll" "%GAME_DIR%\nvngx_dlssg.dll" >nul 2>&1
if errorlevel 1 ( echo [FAIL] nvngx_dlssg.dll != fakerFG.dll & set "RC=1" ) else ( echo [ok] nvngx_dlssg.dll == fakerFG.dll )

echo.
echo --- fakerFG.ini (producer reads it once per game session) --
findstr /b /c:"FgInput=" /c:"FgOutput=" /c:"ColourStream=" /c:"UiStream=" /c:"UiPremultiplied=" /c:"UiSource=" /c:"UiPatch=" /c:"UiFullRes=" /c:"UiSlots=" /c:"UiCodec=" /c:"UiLinear=" /c:"StreamPercentUi=" "%INI%" 2>nul
if errorlevel 1 ( echo [FAIL] UI keys not found - run 01_ui_on.bat & set "RC=1" )

echo.
echo --- processes ----------------------------------------------
tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if errorlevel 1 ( echo [info] game is NOT running - start it, load a save, DLSS FG on ) else ( echo [ok] game is running )
tasklist /fi "imagename eq lock_frame.exe" 2>nul | find /i "lock_frame.exe" >nul
if errorlevel 1 ( echo [ok] no old consumer running ) else ( echo [warn] lock_frame.exe is already running - stop it with Ctrl+Alt+End )

echo.
if "%RC%"=="0" (echo [check] OK) else (echo [check] FAILED - fix the items above)
pause
exit /b %RC%
