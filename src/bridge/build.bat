@echo off
rem ===========================================================================
rem  bridge.exe build: needs the XeSS-FG / XeLL import libs plus D3D11 + WinRT.
rem     build.bat <repo-win-path>
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
  /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DMGPU_FG_OF=1 ^
  /I"%REPO%\src" ^
  /I"%REPO%\third_party\xess\inc" ^
  /I"%REPO%\third_party\ffx\include" ^
  "%~dp0bridge.cpp" ^
  /Fe:bridge.exe /Fo:bridge.obj ^
  /link /LIBPATH:"%REPO%\third_party\xess\lib" ^
  /LIBPATH:"%REPO%\third_party\ffx\lib" ^
  ffx_opticalflow_x64.lib ffx_backend_dx12_x64.lib ^
  libxell.lib d3d11.lib d3d12.lib dxgi.lib dcomp.lib d3dcompiler.lib runtimeobject.lib user32.lib gdi32.lib

if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK -^> %CD%\bridge.exe
exit /b 0
