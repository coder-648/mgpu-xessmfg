@echo off
rem Restore the fakerFG/producer system after 20_cost_native_off.bat.
setlocal
set "GAME=E:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64"
set "B=%GAME%\_cost_native_backup"
if not exist "%B%\" ( echo [restore] no backup & pause & exit /b 1 )
tasklist /fi "imagename eq Cyberpunk2077.exe" 2>nul | find /i "Cyberpunk2077.exe" >nul
if not errorlevel 1 ( echo [restore] close the game first & pause & exit /b 1 )
for %%F in (dxgi.dll fakerFG.dll nvngx_dlssg.dll xess_mfg_producer.dll) do (
  if exist "%B%\%%F" copy /y "%B%\%%F" "%GAME%\%%F" >nul
)
echo [restore] system files restored. Restart the game before running 03/04.
pause
