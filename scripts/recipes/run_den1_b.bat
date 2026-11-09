@echo off
rem ===========================================================================
rem  run_den1_b.bat  -  A/B test of --input-copy-queue at 4K NATIVE output
rem                      (--den 1).  THIS IS THE "B" RUN.
rem
rem  Identical to run_den1_a.bat except the one extra switch, so a back-to-back
rem  pair (A -> B) differs only in where the MV/depth cross-adapter copy runs:
rem
rem      A: direct queue   convert draw -> mv+depth copies      (serial)
rem      B: copy queue     convert draw || mv+depth copies      (overlap)
rem
rem  Both still submit the copies before Present(), so XeSS-FG sees finished
rem  data; on this path the tags deliberately pass no command list (the
rem  resources stay valid until the next present, which is the documented case
rem  where that argument is optional). Verified safe at --den 2 (3.28.4);
rem  this run measures the size of the gain.
rem
rem  Read from the two logs:
rem      gpu       should drop by the overlapped amount (expect 0.5-1.5 ms)
rem      total     the same drop, plus whatever present() re-paces
rem      cq        must stay ~0 (the cross-queue wait is queued, not waited on)
rem      bus:/xN/cadence/slow   must be identical to A
rem
rem  STOP RULES (XeFG still wants our command list, drop the switch):
rem      xN differs from A, xNmiss > 0, ERR_INVALID_ARGUMENT, black picture,
rem      device removed, or cq climbing into the ms range.
rem ===========================================================================
setlocal
cd /d "%~dp0"

set "GAMEOPT="
if not "%~1"=="" set "GAMEOPT=--game %~1"

if not exist bridge.exe (
  echo [mgpu] bridge.exe missing in %CD%
  pause
  exit /b 1
)

bridge.exe %GAMEOPT% --den 1 --pool 4 --interp 3 --interp-cadence --unlock-mfg ^
           --fg-ui none --fg-rt-mode auto --input-copy-queue > bridge_den1_B.txt 2>&1
type bridge_den1_B.txt
pause
