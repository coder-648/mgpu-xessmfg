@echo off
setlocal
echo [AB] ---- paths ----
echo [AB] folder : %~dp0
call "%~dp0ab__env.bat"
set "RC=%errorlevel%"
echo.
echo [AB] ---- files ----
for %%F in ("%~dp0lock_frame.exe") do echo [AB] exe       : %%~nxF   %%~tF   %%~zF bytes
for %%F in ("%GAME_DIR%\dxgi.dll" "%GAME_DIR%\xess_mfg_producer.dll" "%GAME_DIR%\fakerFG.ini" "%GAME_DIR%\fakerFG\libxess_fg.dll" "%GAME_DIR%\fakerFG\libxell.dll") do if exist "%%~fF" (for %%G in ("%%~fF") do echo [AB] game side : %%~nxG   %%~tG) else (echo [AB] game side : MISSING %%~nxF)
echo.
if exist "%~dp0xefg_programs\programs.tsv" (echo [AB] port payload: present ^(xefg_programs^)) else (echo [AB] port payload: MISSING ^(ab_10..15 would fall back to keep2x^))
echo [AB] ---- fakerFG.ini ----
findstr /i /b /c:"FgInput=" /c:"FgOutput=" /c:"Passthrough=" /c:"StreamPercent=" /c:"MvCodec=" /c:"DepthCodec=" "%GAME_DIR%\fakerFG.ini" 2>nul
echo.
echo [AB] ---- processes ----
tasklist /fi "imagename eq Cyberpunk2077.exe" 2>nul | find /i "Cyberpunk2077.exe" >nul && echo [AB] game        : running || echo [AB] game        : NOT running
tasklist /fi "imagename eq lock_frame.exe" 2>nul | find /i "lock_frame.exe" >nul && echo [AB] lock_frame  : running ^(stop it: Ctrl+Alt+End^) || echo [AB] lock_frame  : not running
echo.
echo [AB] ---- last producer log lines ----
powershell -NoProfile -Command "Get-Content -LiteralPath '%GAME_DIR%\Cyberpunk2077_mfg_producer.log' -Tail 5" 2>nul
echo.
if "%RC%"=="0" (echo [AB] READY - preflight passed) else (echo [AB] NOT READY - preflight exit=%RC%   [1=files or ini  2=game not running  3=lock_frame already running])
pause
