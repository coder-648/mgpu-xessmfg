@echo off
rem ===========================================================================
rem  Preflight for the model-15 / ported-MFG round (2026-11-22).
rem  Calls ab__env.bat (paths, game running, one lock_frame at a time), then
rem  checks the port payload next to the exe. The port falls back to the 2x
rem  model silently when these are missing, so this is checked up front.
rem ===========================================================================
call "%~dp0ab__env.bat"
if errorlevel 1 exit /b %errorlevel%

set "PROG=%AB_DIR%xefg_programs"
set "PORT_RC=0"
if not exist "%PROG%\programs.tsv" ( echo [AB] port assets missing: %PROG%\programs.tsv & set "PORT_RC=1" )
if not exist "%PROG%\shaders.tsv"  ( echo [AB] port assets missing: %PROG%\shaders.tsv & set "PORT_RC=1" )
if not exist "%PROG%\programs\3518243144275597942266.dxil" ( echo [AB] port programs missing, 54 expected under %PROG%\programs & set "PORT_RC=1" )
if not exist "%PROG%\shaders\splat_motion_aware.dxil" ( echo [AB] port replacement shaders missing under %PROG%\shaders & set "PORT_RC=1" )
if not exist "%AB_DIR%libxess_fg.dll" ( echo [AB] libxess_fg.dll missing next to lock_frame.exe & set "PORT_RC=1" )
if not "%PORT_RC%"=="0" (
  echo [AB] without the port payload every run below silently falls back to the 2x model.
  exit /b 4
)
exit /b 0
