@echo off
rem ===========================================================================
rem  mgpu_ui.exe build: plain Win32 + GDI, no third-party libraries.
rem     build.bat <repo-win-path>
rem  %1 is only used for /I so the shared header (src\mgpu_shared.h) is reachable -
rem  cl cannot use a UNC path as its working directory, so the sources are staged in
rem  <STAGE_DIR>\ui and the repo is passed in.
rem ===========================================================================
setlocal
set "REPO=%~1"
if "%REPO%"=="" ( echo usage: build.bat ^<repo^> & exit /b 1 )

set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )

echo [build] compiler:
where cl

cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- ^
  /DUNICODE /D_UNICODE ^
  /I"%REPO%\src" ^
  "%~dp0mgpu_ui.cpp" ^
  /Fe:mgpu_ui.exe /Fo:mgpu_ui.obj ^
  /link /SUBSYSTEM:WINDOWS ^
  user32.lib gdi32.lib shell32.lib advapi32.lib dwmapi.lib

if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK -^> %CD%\mgpu_ui.exe
exit /b 0

