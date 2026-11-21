param(
    [string]$LogDir = ".",
    [string]$Out = "summary.txt"
)
$ErrorActionPreference = "SilentlyContinue"
$buf = New-Object System.Collections.Generic.List[string]
function Emit([string]$s) { $buf.Add($s); Write-Host $s }

Emit ("== CP2077 XeSS-FG A/B summary  " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Emit ("   log dir: " + $LogDir)

$files = Get-ChildItem -LiteralPath $LogDir -Filter *.log | Sort-Object Name
if (-not $files) { Emit "   (no .log files yet)" }

foreach ($f in $files) {
    Emit ""
    Emit ("=== " + $f.Name + "   " + $f.LastWriteTime.ToString("MM-dd HH:mm") + "   " + [math]::Round($f.Length/1KB) + " KB")
    $lines = Get-Content -LiteralPath $f.FullName
    if (-not $lines) { Emit "   (empty)"; continue }

    # which model route this run actually took
    if ($lines -match 'ported MFG model route active') { Emit "   MODEL : port (model 15 -> 16)" }
    elseif ($lines -match '2x-model route active') { Emit "   MODEL : keep2x (model 18 reused)" }
    else { Emit "   MODEL : no unlock / unknown" }

    $lines | Where-Object { $_ -match 'shared protocol|libxess_fg version|MFG available|running [0-9]+x|model_port_applied|shader_port_applied|shader_replacement_set|model_port_rejected|model_port_assets' } |
        Select-Object -First 8 | ForEach-Object { Emit ("   " + $_) }
    $lines | Where-Object { $_ -match 'near/far:|XeFG tuning:|projection: from the game' } |
        Select-Object -First 6 | ForEach-Object { Emit ("   " + $_) }

    foreach ($k in @('MV missing this frame -> holding FG on','MV back -> FG on','-> 1X','MISMATCH_INPUT_RESOURCES','device removed','shader_supplied','shader_native_lookup','zn=10.000 zf=10000','matrix=2 cached')) {
        Emit ("   count[" + $k + "] = " + @($lines | Where-Object { $_ -like ("*" + $k + "*") }).Count)
    }

    $bus = @($lines | Where-Object { $_ -match 'bus: mv ' })
    if ($bus.Count -gt 0) { Emit ("   " + $bus[-1].Trim()) }
    $fr = @($lines | Where-Object { $_ -match '\[lock\] frame ' })
    if ($fr.Count -gt 0) { $fr | Select-Object -Last 3 | ForEach-Object { Emit ("   " + $_) } }
    $st = @($lines | Where-Object { $_ -match 'xefg: (2X|1X) present status' })
    if ($st.Count -gt 0) { $st | Select-Object -Last 3 | ForEach-Object { Emit ("   " + $_) } }
    Emit ("   xefg warn/error lines = " + @($lines | Where-Object { $_ -match 'xefg\[error\]|xefg\[warn\]' }).Count)
}

$buf | Set-Content -LiteralPath $Out -Encoding UTF8
