param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('on', 'linear0', 'off')]
    [string]$Mode
)
$ErrorActionPreference = 'Stop'

$ini = Join-Path (Split-Path -Parent $PSScriptRoot) 'fakerFG.ini'
if (-not (Test-Path -LiteralPath $ini)) { throw "fakerFG.ini not found: $ini" }

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

$lines = @(Get-Content -LiteralPath $ini -Encoding UTF8)
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
Copy-Item -LiteralPath $ini -Destination "$ini.bak_ui_$stamp"

# Common: the consumer is the one that runs XeFG; the producer only publishes.
$keys = [ordered]@{
    'FgInput'         = 'DLSSG'
    'FgOutput'        = 'Capture'
    'InjectReshade'   = '0'
    'NoOutputReal'    = '0'
    'UiPremultiplied' = '1'
    'UiFullRes'       = '1'
    'UiSlots'         = '8'
    'UiCodec'         = 'auto'
    'UiLinear'        = '1'
    'UiSynthDump'     = '0'
    'UiSynthDumpAfter'= '30'
    'UiAlpha8'        = '0'
    'UiDilate'        = '0'
    'UiSparseCodec'   = '1'
    'UiTagSynth'      = '0'
    'UiSnapshotPQ'    = '0'
    'HudlessStream'   = '0'
    'HudlessSlots'    = '8'
    'StreamPercentUi' = '100'
    'UiStream'        = '1'
    'UiSource'        = 'Auto'
}
if ($Mode -eq 'linear0') { $keys['UiLinear'] = '0' }
if ($Mode -eq 'off') {
    $keys['UiStream'] = '0'
    $keys['UiSource'] = 'Off'
}

foreach ($k in @($keys.Keys)) {
    $lines = @(Set-IniKey -Src $lines -Key $k -Value $keys[$k])
}

$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllLines($ini, $lines, $utf8)

Write-Host "[ui] mode=$Mode"
Write-Host "[ui] ini=$ini"
Write-Host "[ui] backup=$ini.bak_ui_$stamp"
Get-Content -LiteralPath $ini |
    Select-String -Pattern '^(FgInput|FgOutput|ColourStream|UiStream|UiPremultiplied|UiSource|UiFullRes|UiSlots|UiCodec|UiLinear|StreamPercentUi)=' |
    ForEach-Object { Write-Host ("  " + $_.Line) }
