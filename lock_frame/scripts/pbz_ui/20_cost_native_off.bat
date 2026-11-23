@echo off
rem Native 2077 baseline: remove fakerFG/producer proxies, restore the game's own
rem nvngx_dlssg.dll. Stop the game and lock_frame first. Use 21 to restore the system.
setlocal
set "GAME=E:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64"
set "B=%GAME%\_cost_native_backup"
if not exist "%B%\" mkdir "%B%"
tasklist /fi "imagename eq Cyberpunk2077.exe" 2>nul | find /i "Cyberpunk2077.exe" >nul
if not errorlevel 1 ( echo [native] close the game first & pause & exit /b 1 )
tasklist /fi "imagename eq lock_frame.exe" 2>nul | find /i "lock_frame.exe" >nul
if not errorlevel 1 ( echo [native] stop lock_frame first & pause & exit /b 1 )
for %%F in (dxgi.dll fakerFG.dll nvngx_dlssg.dll xess_mfg_producer.dll) do (
  if exist "%GAME%\%%F" if not exist "%B%\%%F" move /y "%GAME%\%%F" "%B%\%%F" >nul
  if exist "%GAME%\%%F" del /f /q "%GAME%\%%F" >nul 2>&1
)
if exist "%GAME%\nvngx_dlssg_real.dll" copy /y "%GAME%\nvngx_dlssg_real.dll" "%GAME%\nvngx_dlssg.dll" >nul
echo [native] fakerFG/producer removed; game native nvngx_dlssg restored.
echo [native] start the game, load the same save, DLSS Frame Generation ON, 40 fps.
echo [native] sample the NVIDIA GPU with 12_gpu_sample.bat native 60.
pause
