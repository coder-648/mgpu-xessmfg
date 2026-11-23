param([Parameter(Mandatory = $true)][string]$Csv)
$rows = @(Import-Csv -LiteralPath $Csv)
if ($rows.Count -eq 0) { throw "no rows in $Csv" }
$u = @($rows | ForEach-Object { [double]$_.gpu_util_pct })
$p = @($rows | ForEach-Object { [double]$_.power_w })
$m = @($rows | ForEach-Object { [double]$_.mem_used_mib })
"file : $Csv"
"n    : $($rows.Count)"
"util : mean {0:N2}%  p95 {1:N2}%  max {2:N2}%" -f (($u | Measure-Object -Average).Average), (($u | Sort-Object)[[int]($u.Count * 0.95)]), (($u | Measure-Object -Maximum).Maximum)
"power: mean {0:N1} W max {1:N1} W" -f (($p | Measure-Object -Average).Average), (($p | Measure-Object -Maximum).Maximum)
"vram : mean {0:N0} MiB" -f (($m | Measure-Object -Average).Average)
