param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('on', 'linear0', 'patch', 'off')]
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
    'UiCodec'         = '1'
    'UiLinear'        = '1'
    'UiSynthDump'     = '0'
    'UiSynthDumpAfter'= '30'
    'UiAlpha8'        = '0'
    'UiDilate'        = '0'
    'UiSparseCodec'   = '1'
    'UiTagSynth'      = '0'
    'UiSnapshotPQ'    = '0'
    'HudlessStream'   = '1'
    'HudlessSlots'    = '8'
    'StreamPercentUi' = '100'
    'UiStream'        = '0'
    'UiSource'        = 'Off'
    'UiPatch'         = '1'
    'UiPatchWhiteNits'= '80'
    'UiSlots'         = '2'
}
if ($Mode -eq 'linear0') { $keys['UiLinear'] = '0' }
if ($Mode -eq 'patch' -or $Mode -eq 'on') {
    # v34 final: ship the HUDless scene under the UI support, tag H' only -> XeFG mode 4.
    # Stream 3 / mode 5 is sealed off: UiStream=0 + UiSource=Off.
    # The producer forces UiFullRes/UiSparseCodec/UiAlpha8/UiCodec itself; write them
    # explicitly so the ini shows the real configuration.
    $keys['UiPatch']       = '1'
    $keys['UiStream']      = '0'
    $keys['UiSource']      = 'Off'
    $keys['UiFullRes']     = '1'
    $keys['UiSparseCodec'] = '1'
    $keys['UiAlpha8']      = '0'
    $keys['UiCodec']       = '1'
    $keys['HudlessStream'] = '1'
    $keys['UiPatchWhiteNits'] = '80'
    $keys['UiSlots']       = '2'
    # Baseline: no producer-side dump; use --tag-dump / UiSynthDump=1 only for diagnostics.
    $keys['UiSynthDump']      = '0'
    $keys['UiSynthDumpAfter'] = '30'
}
if ($Mode -eq 'off') {
    $keys['UiStream'] = '0'
    $keys['UiSource'] = 'Off'
    $keys['UiPatch']  = '0'
    $keys['UiCodec']  = '0'
    $keys['HudlessStream'] = '0'
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
    Select-String -Pattern '^(FgInput|FgOutput|ColourStream|UiStream|UiPremultiplied|UiSource|UiPatch|UiFullRes|UiSlots|UiCodec|UiLinear|StreamPercentUi)=' |
    ForEach-Object { Write-Host ("  " + $_.Line) }
