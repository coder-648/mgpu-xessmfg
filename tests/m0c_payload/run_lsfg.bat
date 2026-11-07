@echo off
rem ===========================================================================
rem  M0.3 kit - "game + Lossless Scaling running" variant.
rem
rem  Usage:  run_lsfg.bat [delay_seconds]        (default 45)
rem
rem  Double-click it, then switch to the game and start Lossless Scaling
rem  (LSFG x3). The countdown gives you time to get everything running before
rem  the measurement starts, because LS stops working while it is not focused.
rem
rem  Each preset is measured TWO ways:
rem    the flat-out pipelined loop (throughput ceiling)
rem    a 40 fps paced loop        (the real operating point)
rem ===========================================================================
setlocal
cd /d "%~dp0"
set "EXE=m0c_payload.exe"

if not exist "%EXE%" (
  echo [kit] %EXE% not found in %CD%
  pause
  exit /b 1
)

set DELAY=%1
if "%DELAY%"=="" set DELAY=45

echo ============================================================
echo  Measuring in %DELAY% seconds.
echo  NOW: switch to Cyberpunk 2077 and start Lossless Scaling (x3).
echo       leave the game focused - LS pauses when unfocused.
echo ============================================================
:countdown
if %DELAY% LEQ 0 goto run
echo   starting in %DELAY% s ...
rem ping instead of timeout: timeout fails when stdin is redirected
ping -n 6 127.0.0.1 >nul
set /a DELAY=DELAY-5
goto countdown

:run
echo.
echo --- [1/3] 5K DLSS Quality : flat-out + paced 40 ---
"%EXE%" --pipeline 2 --iters 40 --paced 40 > run_lsfg_q.txt 2>&1
type run_lsfg_q.txt | findstr /C:"per frame :" /C:"ceiling" /C:"handoff avg" /C:"budget used"

echo.
echo --- [2/3] 5K DLSS Quality, depth as 16-bit ---
"%EXE%" --pipeline 2 --iters 40 --paced 40 ^
  --stream color 5120 2880 r10g10b10a2 ^
  --stream mv    3413 1920 r16g16_float ^
  --stream depth 3413 1920 r16g16_float > run_lsfg_q_d16.txt 2>&1
type run_lsfg_q_d16.txt | findstr /C:"per frame :" /C:"ceiling" /C:"handoff avg" /C:"budget used"

echo.
echo --- [3/3] 5K DLSS Performance (aux 2560x1440) ---
"%EXE%" --pipeline 2 --iters 40 --paced 40 ^
  --stream color 5120 2880 r10g10b10a2 ^
  --stream mv    2560 1440 r16g16_float ^
  --stream depth 2560 1440 r32_float > run_lsfg_p.txt 2>&1
type run_lsfg_p.txt | findstr /C:"per frame :" /C:"ceiling" /C:"handoff avg" /C:"budget used"

echo.
echo ------------------------------------------------------------
echo  Logs: run_lsfg_*.txt in %CD%
echo ------------------------------------------------------------
pause
