@echo off
setlocal
set "GAME_DIR=E:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64"
set "LOG_DIR=%GAME_DIR%\audit_logs"
set "OUT=%LOG_DIR%\latest"
set "SUM=%OUT%\summary.txt"
if exist "%OUT%\" rmdir /s /q "%OUT%" >nul 2>&1
mkdir "%OUT%" >nul 2>&1
echo [audit] collecting: %OUT%

for %%F in (audit_1_3x_keep2x.log audit_2_3x_keep2x_ui.log audit_3_3x_debug.log audit_out_3x_keep2x.log audit_ui_hard_edge.log audit_build_info.txt) do (
  if exist "%LOG_DIR%\%%F" copy /y "%LOG_DIR%\%%F" "%OUT%\" >nul
)
for %%F in (Cyberpunk2077_fakerFG.log Cyberpunk2077_mfg_producer.log fakerFG.ini nvngx_dlssg.ini) do (
  if exist "%GAME_DIR%\%%F" copy /y "%GAME_DIR%\%%F" "%OUT%\" >nul
)

echo ==== consumer identity ==== > "%SUM%"
findstr /i /c:"[lock] lock_frame" "%LOG_DIR%\audit_1_3x_keep2x.log" >> "%SUM%" 2>nul
echo. >> "%SUM%"

echo ==== marker / pairing (A) ==== >> "%SUM%"
findstr /i /c:"pair:" "%LOG_DIR%\audit_1_3x_keep2x.log" >> "%SUM%" 2>nul
echo. >> "%SUM%"
echo ---- same, UI-patch run ---- >> "%SUM%"
findstr /i /c:"pair:" "%LOG_DIR%\audit_2_3x_keep2x_ui.log" >> "%SUM%" 2>nul
echo. >> "%SUM%"

echo ==== tag inputs (C: must NOT degrade after the fix) ==== >> "%SUM%"
findstr /i /c:"xefg-in:" /c:"frame constants:" /c:"near/far:" "%LOG_DIR%\audit_1_3x_keep2x.log" >> "%SUM%" 2>nul
findstr /i /c:"ui: mode 5" /c:"ui-patch" /c:"ui-unblend:" "%LOG_DIR%\audit_2_3x_keep2x_ui.log" >> "%SUM%" 2>nul
echo. >> "%SUM%"

echo ==== per-window breakdown (A/C: gpu/present/copy/load/slow) ==== >> "%SUM%"
findstr /i /c:"breakdown:" "%LOG_DIR%\audit_1_3x_keep2x.log" >> "%SUM%" 2>nul
echo. >> "%SUM%"

echo ==== consumer log tail (last 40 lines of the baseline run) ==== >> "%SUM%"
powershell -NoProfile -Command "Get-Content -LiteralPath '%LOG_DIR%\audit_1_3x_keep2x.log' -Tail 40" >> "%SUM%" 2>nul
echo. >> "%SUM%"

echo ==== out-of-game layout run (fakerFG NOT injected into the consumer) ==== >> "%SUM%"
if exist "%LOG_DIR%\audit_out_3x_keep2x.log" (
  findstr /i /c:"[lock] lock_frame" /c:"pair:" /c:"xefg-in:" /c:"breakdown:" "%LOG_DIR%\audit_out_3x_keep2x.log" >> "%SUM%" 2>nul
  echo ---- tail ---- >> "%SUM%"
  powershell -NoProfile -Command "Get-Content -LiteralPath '%LOG_DIR%\audit_out_3x_keep2x.log' -Tail 15" >> "%SUM%" 2>nul
) else (
  echo (not run) >> "%SUM%"
)
echo. >> "%SUM%"
echo ==== UI mode 4 hard-edge A/B (--ui-patch-hard-edge) ==== >> "%SUM%"
if exist "%LOG_DIR%\audit_ui_hard_edge.log" (
  findstr /i /c:"ui-patch" /c:"pair:" /c:"xefg-in:" /c:"breakdown:" "%LOG_DIR%\audit_ui_hard_edge.log" >> "%SUM%" 2>nul
) else (
  echo (not run) >> "%SUM%"
)
echo. >> "%SUM%"



echo ==== hitches / warnings ==== >> "%SUM%"
findstr /i /c:"HITCH" /c:"WARNING" /c:"FAILED" /c:"MISMATCH" "%LOG_DIR%\audit_1_3x_keep2x.log" "%LOG_DIR%\audit_2_3x_keep2x_ui.log" >> "%SUM%" 2>nul
echo. >> "%SUM%"

echo ==== D3D12 validation (D, only the --d3d-debug run) ==== >> "%SUM%"
findstr /i /c:"[d3d12 " /c:"statebefore" /c:"RESOURCE_BARRIER" /c:"INVALID_CALL" /c:"DEVICE_REMOVED" /c:"debug layer" "%LOG_DIR%\audit_3_3x_debug.log" >> "%SUM%" 2>nul
echo ---- validation message count ---- >> "%SUM%"
findstr /c:"[d3d12 " "%LOG_DIR%\audit_3_3x_debug.log" 2>nul | find /c /v "" >> "%SUM%"
echo. >> "%SUM%"

echo ==== prefetch / marker log volume (E, M) ==== >> "%SUM%"
echo pf lines (expect 0 in this build): >> "%SUM%"
findstr /c:" pf:" "%LOG_DIR%\audit_1_3x_keep2x.log" 2>nul | find /c /v "" >> "%SUM%"
echo marker: seq12 lines (M: currently one per frame): >> "%SUM%"
findstr /c:"marker: seq12" "%LOG_DIR%\audit_1_3x_keep2x.log" 2>nul | find /c /v "" >> "%SUM%"
echo. >> "%SUM%"

echo ==== producer side ==== >> "%SUM%"
echo (the producer log is append-only across game sessions; the tail is the current run) >> "%SUM%"
powershell -NoProfile -Command "Get-Content -LiteralPath '%GAME_DIR%\Cyberpunk2077_mfg_producer.log' -Tail 600 | Select-String -Pattern 'TransportSlots=|shared ready|shared: begin mapping|fr: codec capability|reclaimed SAFE slot|no safe slot|reclaimed oldest slot|consumer process is|publish: Signal FAILED|copywait=' | Select-Object -Last 25 | ForEach-Object { $_.Line }" >> "%SUM%" 2>nul
echo ---- producer log tail (last 20 lines) ---- >> "%SUM%"
powershell -NoProfile -Command "Get-Content -LiteralPath '%GAME_DIR%\Cyberpunk2077_mfg_producer.log' -Tail 20" >> "%SUM%" 2>nul
echo. >> "%SUM%"

type "%SUM%"
echo.
echo [audit] done. Whole folder for review:
echo        %OUT%
pause
