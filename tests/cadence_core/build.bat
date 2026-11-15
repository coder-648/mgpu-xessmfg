@echo off
rem ===========================================================================
rem  cadence_core 仿真：任意上限（J = 2..9）的动态逐帧控制律
rem     build.bat            -> cadence_sim.exe，然后运行
rem  只依赖 C++17 标准库，不需要 D3D/Windows SDK 头文件。
rem ===========================================================================
setlocal
cd /d "%~dp0"
if not exist "%TEMP%" mkdir "%TEMP%"
where cl >nul 2>nul
if errorlevel 1 (
  echo [build] cl not found - run this from a Visual Studio developer prompt
  exit /b 1
)
cl /nologo /std:c++17 /EHsc /O2 /W4 /utf-8 /permissive- sim.cpp /Fe:cadence_sim.exe
if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
cadence_sim.exe
exit /b %errorlevel%
