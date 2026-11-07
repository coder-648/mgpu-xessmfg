@echo off
rem ===========================================================================
rem  Deploy the M3 depth probe.
rem
rem     deploy_capture.bat <repo-win-path> [game-dir]
rem
rem  Reversible actions:
rem    1. restore ReShade dxgi.dll from dxgi.dll.back if the loader is missing
rem    2. disable the previous project's add-ons AND the M2 probe (.addon64 -> .off)
rem    3. install game_capture.addon64
rem    4. install reshade-shaders\Shaders\mgpu_depth_tap.fx (old one kept as .bak)
rem    5. make sure ReShadePreset.ini enables MGPU_DepthTap@mgpu_depth_tap.fx
rem ===========================================================================
setlocal
set "REPO=%~1"
set "GAMEDIR=%~2"
if "%REPO%"=="" (
  echo usage: deploy_capture.bat ^<repo-win-path^> [game-dir]
  exit /b 1
)
if "%GAMEDIR%"=="" (
  echo usage: deploy_capture.bat ^<repo-win-path^> ^<game-dir^>
  echo        game-dir = the folder that directly contains the game's .exe
  echo                   e.g. ^<GAME_DIR^>\bin\x64
  exit /b 1
)
if not exist "%GAMEDIR%\*.exe" (
  echo [deploy] no .exe in: %GAMEDIR%
  echo [deploy] pass the folder that directly contains the game executable
  exit /b 1
)
echo [deploy] game dir : %GAMEDIR%

if exist "%GAMEDIR%\dxgi.dll" goto have_dxgi
if not exist "%GAMEDIR%\dxgi.dll.back" goto no_dxgi
copy /y "%GAMEDIR%\dxgi.dll.back" "%GAMEDIR%\dxgi.dll" >nul
echo [deploy] restored ReShade dxgi.dll
:have_dxgi

for %%F in (nvngx.dll_mgpu_bridge.addon64 mgpu_bridge.addon64 m2_probe.addon64 m3_depth.addon64) do (
  if exist "%GAMEDIR%\%%F" (
    if exist "%GAMEDIR%\%%F.off" del /f /q "%GAMEDIR%\%%F.off" >nul 2>&1
    ren "%GAMEDIR%\%%F" "%%F.off"
    echo [deploy] disabled: %%F
  )
)

if not exist "%REPO%\build\game_capture.addon64" (
  echo [deploy] missing %REPO%\build\game_capture.addon64 - build it first
  exit /b 1
)
copy /y "%REPO%\build\game_capture.addon64" "%GAMEDIR%\game_capture.addon64" >nul
echo [deploy] installed game_capture.addon64

if not exist "%GAMEDIR%\reshade-shaders\Shaders" mkdir "%GAMEDIR%\reshade-shaders\Shaders"
if exist "%GAMEDIR%\reshade-shaders\Shaders\mgpu_depth_tap.fx" (
  if exist "%GAMEDIR%\reshade-shaders\Shaders\mgpu_depth_tap.fx.bak" del /f /q "%GAMEDIR%\reshade-shaders\Shaders\mgpu_depth_tap.fx.bak" >nul 2>&1
  ren "%GAMEDIR%\reshade-shaders\Shaders\mgpu_depth_tap.fx" "mgpu_depth_tap.fx.bak"
  echo [deploy] previous .fx kept as mgpu_depth_tap.fx.bak
)
copy /y "%REPO%\effects\mgpu_depth_tap.fx" "%GAMEDIR%\reshade-shaders\Shaders\mgpu_depth_tap.fx" >nul
echo [deploy] installed reshade-shaders\Shaders\mgpu_depth_tap.fx

> "%GAMEDIR%\ReShadePreset.ini" echo Techniques=MGPU_DepthTap@mgpu_depth_tap.fx
>>"%GAMEDIR%\ReShadePreset.ini" echo TechniqueSorting=MGPU_DepthTap@mgpu_depth_tap.fx
echo [deploy] preset now enables MGPU_DepthTap@mgpu_depth_tap.fx

echo.
echo [deploy] add-ons in folder:
dir /b "%GAMEDIR%\*.addon64" 2>nul
echo.
echo [deploy] start the game, get in-world with DLSS on, stay 30 s, then quit
echo          log: %GAMEDIR%\game_capture.log
exit /b 0

:no_dxgi
echo [deploy] neither dxgi.dll nor dxgi.dll.back present - install ReShade 6.8 first
exit /b 1
