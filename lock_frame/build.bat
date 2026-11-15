@echo off
rem ===========================================================================
rem  lock_frame.exe build: WGC 颜色 + marker 精确配对 + 锁帧 30x4 / 40x3。
rem     build.bat <repo-win-path>
rem  需要 XeSS-FG / XeLL 的 import lib，以及 D3D11/WinRT。
rem ===========================================================================
setlocal
set "REPO=%~1"
if "%REPO%"=="" ( echo usage: build.bat ^<repo^> & exit /b 1 )

set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )

echo [build] compiler:
where cl

if not exist "%~dp0modules" (
  echo [build] modules\ is missing - lock_frame.cpp is only the module index
  exit /b 1
)

cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- ^
  /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DMGPU_FG_OF=1 ^
  /I"%~dp0." ^
  /I"%REPO%\src" ^
  /I"%REPO%\third_party\xess\inc" ^
  /I"%REPO%\third_party\ffx\include" ^
  "%~dp0lock_frame.cpp" ^
  /Fe:lock_frame.exe /Fo:lock_frame.obj ^
  /link /MAP:lock_frame.map /LIBPATH:"%REPO%\third_party\xess\lib" ^
  /LIBPATH:"%REPO%\third_party\ffx\lib" ^
  ffx_opticalflow_x64.lib ffx_backend_dx12_x64.lib ^
  libxell.lib d3d11.lib d3d12.lib dxgi.lib dcomp.lib d3dcompiler.lib runtimeobject.lib user32.lib gdi32.lib

if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK -^> %CD%\lock_frame.exe
exit /b 0
