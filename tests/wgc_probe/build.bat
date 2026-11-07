@echo off
rem ===========================================================================
rem  WGC probe build: needs d3d11 + WinRT runtime on top of the usual libs.
rem     build.bat <repo-win-path> <source.cpp>
rem ===========================================================================
setlocal
set "REPO=%~1"
set "SRC=%~2"
if "%REPO%"=="" ( echo usage: build.bat ^<repo^> ^<src.cpp^> & exit /b 1 )
if "%SRC%"=="" set "SRC=wgc_probe.cpp"

set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )

echo [build] compiler:
where cl

cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- ^
  /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
  /I"%REPO%\src" ^
  "%~dp0%SRC%" ^
  /Fe:"%~n2.exe" /Fo:"%~n2.obj" ^
  /link d3d11.lib dxgi.lib d3d12.lib runtimeobject.lib user32.lib

if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK -^> %CD%\%~n2.exe
exit /b 0
