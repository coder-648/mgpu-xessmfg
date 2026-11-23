param(
    [int]$Seconds = 60,
    [string]$Out = "",
    [int]$Index = 0
)
$ErrorActionPreference = 'Stop'
if ($Out -eq "") { $Out = Join-Path (Get-Location) "gpu_sample.csv" }
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("timestamp,gpu_util_pct,mem_used_mib,power_w,sm_clock_mhz,temp_c")
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) {
    $v = & nvidia-smi --query-gpu=timestamp,utilization.gpu,memory.used,power.draw,clocks.sm,temperature.gpu `
        --format=csv,noheader,nounits -i $Index 2>$null
    if ($v) { $lines.Add(($v -join "")) }
    Start-Sleep -Milliseconds 1000
}
[System.IO.File]::WriteAllLines($Out, $lines, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "[gpu] wrote $Out ($($lines.Count - 1) samples)"
