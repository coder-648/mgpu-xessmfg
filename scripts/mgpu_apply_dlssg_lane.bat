@echo off
setlocal
cd /d "%~dp0"
if not exist fakerFG.ini.bak copy /y fakerFG.ini fakerFG.ini.bak >nul
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$p='fakerFG.ini'; $t=Get-Content -LiteralPath $p; $t = $t -replace '^FgInput=.*','FgInput=DLSSG' -replace '^FakeSlDlssg=.*','FakeSlDlssg=0' -replace '^TagAtEvaluate=.*','TagAtEvaluate=0' -replace '^TagHudless=.*','TagHudless=1' -replace '^TagUi=.*','TagUi=1'; Set-Content -LiteralPath $p -Value $t -Encoding ASCII"
echo.
echo  Applied: FgInput=DLSSG FakeSlDlssg=0 TagAtEvaluate=0 TagHudless=1 TagUi=1
echo  fakerFG.ini is read at process start - RESTART Cyberpunk 2077 now.
echo  Then run mgpu_test_marker_sidecar.bat.
echo  After the test run mgpu_restore_lane.bat.
echo.
pause
