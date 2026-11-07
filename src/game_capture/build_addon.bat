@echo off
rem ===========================================================================
rem  Build a ReShade add-on DLL (*.addon64).
rem
rem     build_addon.bat <repo-win-path> <source.cpp> <output-name.addon64>
rem
rem  Mirrors tests/*/build.bat but links as a DLL, pulls in MinHook and adds the
rem  ReShade / DLSS / MinHook include paths. Must run from the staging dir that
rem  holds the .cpp (cl cannot use a UNC path as its working directory).
rem ===========================================================================
setlocal
set "REPO=%~1"
set "SRC=%~2"
set "OUT=%~3"
if "%REPO%"=="" goto usage
if "%SRC%"==""  goto usage
if "%OUT%"==""  set "OUT=addon.addon64"

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

if not exist obj mkdir obj

echo [build] compiler:
where cl

cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- ^
  /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX ^
  /I"%~dp0." ^
  /I"%REPO%\src" ^
  /I"%REPO%\third_party\xess\inc" ^
  /I"%REPO%\third_party\ffx\api\include" ^
  /I"%REPO%\third_party\reshade\include" ^
  /I"%REPO%\third_party\dlss\include" ^
  /I"%REPO%\third_party\minhook\include" ^
  /I"%REPO%\third_party\minhook\src" ^
  "%~dp0%SRC%" ^
  "%REPO%\third_party\minhook\src\buffer.c" ^
  "%REPO%\third_party\minhook\src\hook.c" ^
  "%REPO%\third_party\minhook\src\trampoline.c" ^
  "%REPO%\third_party\minhook\src\hde\hde64.c" ^
  /Fe:"%OUT%" /Fo:"obj\\" ^
  /link /DLL d3d12.lib dxgi.lib

if errorlevel 1 (
  echo [build] FAILED
  exit /b 1
)

echo [build] OK -^> %CD%\%OUT%
exit /b 0

:usage
echo usage: build_addon.bat ^<repo-win-path^> ^<source.cpp^> ^<output.addon64^>
exit /b 1
