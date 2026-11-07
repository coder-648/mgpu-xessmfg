@echo off
rem ===========================================================================
rem  Generic module build script.
rem     build.bat <repo-win-path> <source-file.cpp>
rem  Must be run from the directory that contains the .cpp (sources are staged
rem  there by scripts/build_m0.sh, because cl.exe cannot use a UNC path as cwd).
rem ===========================================================================
setlocal
set "REPO=%~1"
set "SRC=%~2"
if "%REPO%"=="" (
  echo usage: build.bat ^<repo-win-path^> ^<source.cpp^>
  exit /b 1
)
if "%SRC%"=="" set "SRC=m0_probe.cpp"

set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
  echo [build] vcvars64.bat not found under "%VSDIR%"
  exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [build] vcvars64.bat failed
  exit /b 1
)

echo [build] compiler:
where cl

cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- ^
  /I"%REPO%\third_party\xess\inc" ^
  /I"%REPO%\src" ^
  "%~dp0%SRC%" ^
  /Fe:"%~n2.exe" /Fo:"%~n2.obj" ^
  /link /LIBPATH:"%REPO%\third_party\xess\lib" ^
  libxess_fg.lib libxell.lib d3d12.lib dxgi.lib

if errorlevel 1 (
  echo [build] FAILED
  exit /b 1
)

echo [build] OK -^> %CD%\%~n2.exe
exit /b 0

