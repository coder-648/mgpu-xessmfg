@echo off
rem ===========================================================================
rem  version.dll proxy build (arch spoof only - no frame generation).
rem     build.bat <repo-win-path>
rem ===========================================================================
setlocal
set "REPO=%~1"
if "%REPO%"=="" ( echo usage: build.bat ^<repo^> & exit /b 1 )

set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )

if not exist obj md obj
echo [build] compiler:
where cl

cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /utf-8 /permissive- ^
  /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX ^
  /I"%REPO%\third_party\minhook\include" ^
  "%~dp0version_min.cpp" ^
  "%REPO%\third_party\minhook\src\buffer.c" ^
  "%REPO%\third_party\minhook\src\hook.c" ^
  "%REPO%\third_party\minhook\src\trampoline.c" ^
  "%REPO%\third_party\minhook\src\hde\hde64.c" ^
  /Fe:version.dll /Fo:obj\ ^
  /link /DLL /DEF:"%~dp0version.def" user32.lib

if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK -^> %CD%\version.dll
exit /b 0
