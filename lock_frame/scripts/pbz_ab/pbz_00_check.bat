@echo off
rem  PBZ preflight / log summary. Safe to run after the game is closed.
setlocal
cd /d "%~dp0"
set "LOG=%~dp0logs"
echo.
echo === binaries ===
if exist "..\lock_frame.exe" ( echo lock_frame.exe  OK ) else ( echo lock_frame.exe  MISSING )
findstr /c:"1.3.1-local.camvm" "..\lock_frame.exe" >nul 2>&1 && echo lock_frame.exe  version 1.3.1-local.camvm  OK
if exist "..\fakerFG.ini" ( echo fakerFG.ini     OK ) else ( echo fakerFG.ini     MISSING )
if exist "..\dxgi.dll" ( echo dxgi.dll         OK ) else ( echo dxgi.dll         MISSING )
if exist "..\xess_mfg_producer.dll" ( echo producer         OK ) else ( echo producer         MISSING )

echo.
echo === game-side API (last lines) ===
findstr /c:"api ready (version" "..\PBZ_fakerFG.log" 2>nul | findstr /c:"last" >nul
findstr /c:"api ready (version 3, record 640 bytes)" "..\PBZ_fakerFG.log" >nul 2>&1 && echo fakerFG v3 640 bytes  OK
findstr /c:"producer: subscribed to the fakerFG record (api v3" "..\PBZ_mfg_producer.log" >nul 2>&1 && echo producer api v3          OK

echo.
echo === runs ===
if not exist "%LOG%\*.log" ( echo no logs in %LOG% & goto end )
for %%f in ("%LOG%\*.log") do (
  echo --- %%~nxf
  findstr /c:"mv: auto" /c:"upsample: MV/depth rebuilt" /c:"xefg-in:" /c:"pair: exact" "%%f" 2>nul | findstr /v /c:"pair: exact=0"
  findstr /c:"breakdown:" "%%f" 2>nul | findstr /c:"xN="
)

:end
echo.
pause
