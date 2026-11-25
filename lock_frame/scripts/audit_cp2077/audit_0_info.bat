@echo off
setlocal
cd /d "%~dp0"
set "GAME_DIR=E:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64"
set "LF=%GAME_DIR%\lock_frame.exe"
set "OUT=%GAME_DIR%\audit_logs\audit_build_info.txt"
if not exist "%GAME_DIR%\audit_logs\" mkdir "%GAME_DIR%\audit_logs" >nul 2>&1

echo ==== lock_frame consumer self-audit (2026-11-25 A/C/D/E/F) ==== > "%OUT%"
echo generated: %DATE% %TIME% >> "%OUT%"
echo. >> "%OUT%"
echo ==== md5 (consumer + deployed game-side files) ==== >> "%OUT%"
for %%F in ("%LF%" "%GAME_DIR%\dxgi.dll" "%GAME_DIR%\fakerFG.dll" "%GAME_DIR%\nvngx_dlssg.dll" "%GAME_DIR%\xess_mfg_producer.dll" "%GAME_DIR%\fakerFG\libxess_fg.dll" "%GAME_DIR%\fakerFG\libxell.dll" "%GAME_DIR%\fakerFG.ini") do (
  echo ---- %%~nxF >> "%OUT%"
  certutil -hashfile "%%~F" MD5 >> "%OUT%" 2>&1
)
echo. >> "%OUT%"
echo ==== deploy reference (md5 of the binaries copied for this round) ==== >> "%OUT%"
if exist "%GAME_DIR%\audit_logs\audit_deploy_info.txt" (
  type "%GAME_DIR%\audit_logs\audit_deploy_info.txt" >> "%OUT%"
) else (
  echo (audit_deploy_info.txt missing) >> "%OUT%"
)
echo. >> "%OUT%"
echo ==== lock_frame --version ==== >> "%OUT%"
"%LF%" --version >> "%OUT%" 2>&1
echo. >> "%OUT%"
echo ==== lock_frame --list-modes (first 6 non-empty lines) ==== >> "%OUT%"
"%LF%" --list-modes 2>&1 | findstr /n "^" | findstr /r "^[1-6]:" >> "%OUT%"

type "%OUT%"
echo.
echo [audit] written: %OUT%
echo [audit] md5 changes on every rebuild (PE timestamp). Compare against:
echo        %GAME_DIR%\audit_logs\audit_deploy_info.txt
pause
