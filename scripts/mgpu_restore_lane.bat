@echo off
setlocal
cd /d "%~dp0"
if exist fakerFG.ini.bak move /y fakerFG.ini.bak fakerFG.ini >nul
echo Restored fakerFG.ini. Restart Cyberpunk 2077.
pause
