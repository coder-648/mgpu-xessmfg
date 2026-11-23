param(
    [Parameter(Mandatory = $true)][string]$Ini,
    [Parameter(Mandatory = $true)][int]$UiSynthDump,
    [int]$After = 30
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Ini)) { throw "fakerFG.ini not found: $Ini" }

function Set-IniKey {
    param([string[]]$Src, [string]$Key, [string]$Value)
    $out = New-Object System.Collections.Generic.List[string]
    $found = $false
    foreach ($line in $Src) {
        if ($line -match ('^\s*' + [regex]::Escape($Key) + '\s*=')) {
            [void]$out.Add("$Key=$Value")
            $found = $true
        } else {
            [void]$out.Add($line)
        }
    }
    if (-not $found) { [void]$out.Add("$Key=$Value") }
    return $out.ToArray()
}

$lines = @(Get-Content -LiteralPath $Ini -Encoding UTF8)
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
Copy-Item -LiteralPath $Ini -Destination "$Ini.bak_dump_$stamp"
$lines = @(Set-IniKey -Src $lines -Key 'UiSynthDump' -Value "$UiSynthDump")
$lines = @(Set-IniKey -Src $lines -Key 'UiSynthDumpAfter' -Value "$After")
$lines = @(Set-IniKey -Src $lines -Key 'UiPatch' -Value '1')
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllLines($Ini, $lines, $utf8)
Write-Host "[dump] UiSynthDump=$UiSynthDump UiSynthDumpAfter=$After UiPatch=1"
Write-Host "[dump] ini=$Ini"
Write-Host "[dump] backup=$Ini.bak_dump_$stamp"
