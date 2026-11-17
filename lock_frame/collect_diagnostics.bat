@echo off
rem ===========================================================================
rem  收集诊断信息（不联网、不上传）：运行日志 + 系统/显卡信息 -> zip。
rem  在发布包目录双击运行即可。游戏目录里的 *_fakerFG.log 也可以手工复制进来。
rem ===========================================================================
setlocal
cd /d "%~dp0"
set "OUT=diagnostics_%RANDOM%"
mkdir "%OUT%" >nul 2>&1

copy /y lock_frame_ui*.txt "%OUT%\" >nul 2>&1
copy /y lock_frame_ui.log "%OUT%\" >nul 2>&1
copy /y *.log "%OUT%\" >nul 2>&1

echo === lock_frame version === > "%OUT%\system.txt"
lock_frame.exe --version >> "%OUT%\system.txt" 2>&1
echo. >> "%OUT%\system.txt"
echo === systeminfo === >> "%OUT%\system.txt"
systeminfo >> "%OUT%\system.txt" 2>&1
echo. >> "%OUT%\system.txt"
echo === GPU === >> "%OUT%\system.txt"
powershell -NoProfile -Command "Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,AdapterRAM | Format-List" >> "%OUT%\system.txt" 2>&1
echo. >> "%OUT%\system.txt"
echo === display modes === >> "%OUT%\system.txt"
lock_frame.exe --list-modes >> "%OUT%\system.txt" 2>&1

powershell -NoProfile -Command "Compress-Archive -Path '%OUT%' -DestinationPath '%OUT%.zip' -Force" >nul 2>&1
if exist "%OUT%.zip" (
  echo [diag] created %OUT%.zip
  rmdir /s /q "%OUT%"
) else (
  echo [diag] zip failed; files remain in %OUT%
)
