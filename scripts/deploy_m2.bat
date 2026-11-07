@echo off
rem ===========================================================================
rem  Deploy the M2 probe into a game folder.
rem
rem     deploy_m2.bat <repo-win-path> [game-dir]
rem
rem  What it does (all reversible):
rem    1. restores ReShade's dxgi.dll from dxgi.dll.back if the loader is missing
rem    2. renames any old "nvngx.dll_mgpu_bridge.addon64" to ".off" so the buggy
rem       previous project cannot load and fight over the NGX hooks
rem    3. copies build\m2_probe.addon64 into the game folder
rem ===========================================================================
setlocal
set "REPO=%~1"
set "GAMEDIR=%~2"
if "%REPO%"=="" (
  echo usage: deploy_m2.bat ^<repo-win-path^> [game-dir]
  exit /b 1
)
if "%GAMEDIR%"=="" (
  echo usage: deploy_m2.bat ^<repo-win-path^> ^<game-dir^>
  echo        game-dir = the folder that directly contains the game's .exe
  exit /b 1
)
if not exist "%GAMEDIR%\*.exe" (
  echo [deploy] no .exe in: %GAMEDIR%
  exit /b 1
)
echo [deploy] game dir : %GAMEDIR%

if exist "%GAMEDIR%\dxgi.dll" goto have_dxgi
if not exist "%GAMEDIR%\dxgi.dll.back" goto no_dxgi
copy /y "%GAMEDIR%\dxgi.dll.back" "%GAMEDIR%\dxgi.dll" >nul
echo [deploy] restored ReShade dxgi.dll from dxgi.dll.back
:have_dxgi

rem the previous project shipped under two different names - disable both
for %%F in (nvngx.dll_mgpu_bridge.addon64 mgpu_bridge.addon64) do (
  if exist "%GAMEDIR%\%%F" (
    if exist "%GAMEDIR%\%%F.off" del /f /q "%GAMEDIR%\%%F.off" >nul 2>&1
    ren "%GAMEDIR%\%%F" "%%F.off"
    echo [deploy] disabled old add-on: %%F
  )
)

if not exist "%REPO%\build\m2_probe.addon64" (
  echo [deploy] missing %REPO%\build\m2_probe.addon64 - build it first
  exit /b 1
)
copy /y "%REPO%\build\m2_probe.addon64" "%GAMEDIR%\m2_probe.addon64" >nul
echo [deploy] installed m2_probe.addon64

echo.
echo [deploy] add-ons now in the folder:
dir /b "%GAMEDIR%\*.addon64" 2>nul
dir /b "%GAMEDIR%\*.addon64.off" 2>nul
echo.
echo [deploy] start the game, load a save, make sure DLSS is enabled,
echo          then read %GAMEDIR%\m2_probe.log
exit /b 0

:no_dxgi
echo [deploy] neither dxgi.dll nor dxgi.dll.back present - install ReShade 6.8 first
exit /b 1
