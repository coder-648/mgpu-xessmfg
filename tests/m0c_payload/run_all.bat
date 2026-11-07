@echo off
rem ===========================================================================
rem  M0.3 payload measurement kit - all presets, flat-out AND paced at 40 fps.
rem
rem  Usage:  run_all.bat [delay_seconds]      (default 0)
rem
rem  With a delay, double-click it and then switch to the game, so the numbers
rem  include the game's own bus usage.
rem
rem  NOTE: no ( ) blocks around the countdown - %DELAY% would be expanded once
rem  at parse time and the loop would never end.
rem ===========================================================================
setlocal
cd /d "%~dp0"
set "EXE=m0c_payload.exe"

if not exist "%EXE%" goto noexe

set DELAY=%1
if "%DELAY%"=="" set DELAY=0
if %DELAY% LEQ 0 goto go

echo Measuring in %DELAY% seconds - switch to the game now, leave it focused.
:countdown
if %DELAY% LEQ 0 goto go
echo   starting in %DELAY% s ...
rem ping instead of timeout: timeout fails when stdin is redirected
ping -n 6 127.0.0.1 >nul
set /a DELAY=DELAY-5
goto countdown

:go
echo ============================================================
echo  1/4  Cyberpunk 2077 @ 5K, DLSS Quality  (colour 5K + aux 3413x1920)
echo ============================================================
"%EXE%" --pipeline 2 --iters 40 --paced 40 > run_cp2077_q_p2.txt 2>&1
type run_cp2077_q_p2.txt | findstr /C:"per frame :" /C:"ceiling" /C:"handoff avg" /C:"budget used"

echo.
echo ============================================================
echo  2/4  same, ring depth 3
echo ============================================================
"%EXE%" --pipeline 3 --iters 40 --paced 40 > run_cp2077_q_p3.txt 2>&1
type run_cp2077_q_p3.txt | findstr /C:"per frame :" /C:"ceiling" /C:"handoff avg" /C:"budget used"

echo.
echo ============================================================
echo  3/4  Cyberpunk 2077 @ 5K, DLSS Performance (aux 2560x1440)
echo ============================================================
"%EXE%" --pipeline 2 --iters 40 --paced 40 --stream color 5120 2880 r10g10b10a2 --stream mv 2560 1440 r16g16_float --stream depth 2560 1440 r32_float > run_cp2077_p_p2.txt 2>&1
type run_cp2077_p_p2.txt | findstr /C:"per frame :" /C:"ceiling" /C:"handoff avg" /C:"budget used"

echo.
echo ============================================================
echo  4/4  colour only at 5K (lower bound if aux is skipped)
echo ============================================================
"%EXE%" --pipeline 2 --iters 40 --paced 40 --stream color 5120 2880 r10g10b10a2 > run_color_only.txt 2>&1
type run_color_only.txt | findstr /C:"per frame :" /C:"ceiling" /C:"handoff avg" /C:"budget used"

echo.
echo ------------------------------------------------------------
echo  Done. Full logs in %CD%\run_*.txt   and  m0c_payload.log
echo ------------------------------------------------------------
pause
exit /b 0

:noexe
echo [kit] %EXE% not found in %CD%
pause
exit /b 1
