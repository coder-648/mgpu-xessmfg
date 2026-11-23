@echo off
rem ===========================================================================
rem  Performance measurement segment - docs/design/PERF_OPT_2026-11-23.md sec 6
rem
rem  Usage:  run_perf.bat TAG [seconds] [extra flags]
rem    run_perf.bat base
rem    run_perf.bat ablate1 30
rem    run_perf.bat unblend_off 30 --no-sparse-unblend
rem
rem  Runs lock_frame.exe for a fixed duration, then copies the two logs that
rem  matter:  lock_frame_perf_TAG.log  and  producer_TAG.log
rem ===========================================================================
setlocal
cd /d "%~dp0"
set "TAG=%~1"
if "%TAG%"=="" set "TAG=run"
set "SECS=%~2"
if "%SECS%"=="" set "SECS=30"
set "EXTRA=%~3"

if not exist "%~dp0lock_frame.exe" goto noexe

tasklist /fi "imagename eq Cyberpunk2077.exe" | find /i "Cyberpunk2077.exe" >nul
if errorlevel 1 goto nogame

set "LOG=%~dp0lock_frame_perf_%TAG%.log"
echo [perf] tag=%TAG% duration=%SECS%s
echo [perf] log=%LOG%

lock_frame.exe --game Cyberpunk2077.exe --mode 3x --interp 2 ^
  --streams=mv,depth,ui --ui-source auto --overlay-layered --wgc-minint 0 ^
  --pool 4 --latency 1 --unlock-mfg --mfg-model=keep2x ^
  --fg-rt-mode auto --report 40 --run-sec %SECS% %EXTRA% > "%LOG%" 2>&1

echo [perf] exit=%errorlevel%
findstr /c:"game window not found" "%LOG%" >nul
if not errorlevel 1 echo [perf] HINT: the game window was not visible. Bring Cyberpunk2077 to the foreground (not minimized) and run again.
findstr /c:"none has a visible window" "%LOG%" >nul
if not errorlevel 1 echo [perf] HINT: the game process exists but has no visible window - un-minimize it.
findstr /c:"already running" "%LOG%" >nul
if not errorlevel 1 echo [perf] HINT: lock_frame.exe was already running - close it (Ctrl+Alt+End) first.
if not exist "%~dp0Cyberpunk2077_mfg_producer.log" goto noprod
copy /y "%~dp0Cyberpunk2077_mfg_producer.log" "%~dp0producer_%TAG%.log" >nul
echo [perf] producer log copied to producer_%TAG%.log
goto noprod2
:noprod
echo [perf] no Cyberpunk2077_mfg_producer.log in %cd%
:noprod2
if not exist "%~dp0Cyberpunk2077_fakerFG.log" goto done
copy /y "%~dp0Cyberpunk2077_fakerFG.log" "%~dp0faker_%TAG%.log" >nul
:done
echo [perf] done. Send producer_%TAG%.log + lock_frame_perf_%TAG%.log
goto end

:noexe
echo [perf] ERROR: lock_frame.exe not found in %cd%
pause
goto end

:nogame
echo [perf] ERROR: Cyberpunk2077.exe is not running.
echo [perf] Start the game, load a save, enable DLSS Frame Generation, retry.
pause
goto end

:end
endlocal
