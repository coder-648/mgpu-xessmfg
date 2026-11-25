# ============================================================================
#  ab_99_camvm_check.ps1 - verify the 2026-11-25 input-alignment build
#
#  Run it from the game directory (or pass -GameDir). It does not need the game
#  or the consumer to be running; it reads the logs they produced.
#
#    * fakerFG must be the v3 build   : "api ready (version 3, record 640 bytes)"
#    * producer must have attached v3 : "producer: subscribed ... (api v3 ...)"
#    * the consumer must have sent    : proj=game view-to-clip / view=camera
#      (or the explicit --fg-proj-rz / --fg-no-proj control, reported as WARN)
#
#  Exit code: 0 = all hard checks passed, 1 = at least one failed.
# ============================================================================
param([string]$GameDir = "")

$ErrorActionPreference = 'Continue'
if ([string]::IsNullOrWhiteSpace($GameDir)) { $GameDir = $PSScriptRoot }
$GameDir = $GameDir.TrimEnd('\')

$script:fail = 0
function Ok([string]$m)   { Write-Host "[ OK ] $m" -ForegroundColor Green }
function Bad([string]$m)  { Write-Host "[FAIL] $m" -ForegroundColor Red; $script:fail++ }
function Warn([string]$m) { Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Info([string]$m) { Write-Host "       $m" }

function LastLine([string]$path, [string]$needle) {
    if (!(Test-Path -LiteralPath $path)) { return $null }
    $m = @(Select-String -LiteralPath $path -SimpleMatch -Pattern $needle)
    if ($m.Count -eq 0) { return $null }
    return ($m | Select-Object -Last 1).Line.Trim()
}

Write-Host ""
Write-Host "camvm check - $GameDir"
Write-Host ("-" * 68)


# ---------------------------------------------------------------- binaries
$names = @('fakerFG.dll', 'dxgi.dll', 'nvngx_dlssg.dll', 'xess_mfg_producer.dll',
           'lock_frame.exe', 'lock_frame_ui.exe')
$hashes = @{}
foreach ($n in $names) {
    $p = Join-Path $GameDir $n
    if (Test-Path -LiteralPath $p) {
        $hashes[$n] = (Get-FileHash -LiteralPath $p -Algorithm MD5).Hash.ToLower()
        Info ("{0,-24} {1}" -f $n, $hashes[$n])
    } else {
        Bad "missing $n"
    }
}
if ($hashes.ContainsKey('fakerFG.dll') -and $hashes.ContainsKey('dxgi.dll') -and
    $hashes.ContainsKey('nvngx_dlssg.dll')) {
    if ($hashes['fakerFG.dll'] -eq $hashes['dxgi.dll'] -and
        $hashes['fakerFG.dll'] -eq $hashes['nvngx_dlssg.dll']) {
        Ok "fakerFG.dll / dxgi.dll / nvngx_dlssg.dll are the same binary"
    } else {
        Bad "the three proxy DLLs differ - deploy the same fakerFG.dll under all three names"
    }
}

# ---------------------------------------------------------------- fakerFG v3
$fakerLog = Join-Path $GameDir 'Cyberpunk2077_fakerFG.log'
$line = LastLine $fakerLog 'api ready (version'
if ($null -eq $line) {
    Bad "no 'api ready' line in Cyberpunk2077_fakerFG.log - start the game once with this deployment"
} else {
    Info $line
    if ($line -match 'version 3, record 640 bytes') {
        Ok "fakerFG is the v3 build (record 640 bytes)"
    } else {
        Bad ("fakerFG is stale: '" + $line + "' - expected 'version 3, record 640 bytes'; " +
             "restart the game and check dxgi.dll in this folder")
    }
}

# ---------------------------------------------------------------- producer v3
$prodLog = Join-Path $GameDir 'Cyberpunk2077_mfg_producer.log'
$line = LastLine $prodLog 'producer: subscribed to the fakerFG record'
if ($null -eq $line) {
    Warn "no 'producer: subscribed' line yet (game never started with this deployment?)"
} else {
    Info $line
    if ($line -match 'api v3') { Ok "producer attached to the v3 record" }
    else { Bad "producer attached to an old API: '$line'" }
}

# ---------------------------------------------------------------- consumer
function CandidateLogs {
    $list = @()
    $ab = Join-Path $GameDir 'ab_logs'
    if (Test-Path -LiteralPath $ab) {
        $list += @(Get-ChildItem -LiteralPath $ab -Filter '*.log' -File -ErrorAction SilentlyContinue)
    }
    $one = Join-Path $GameDir 'lock_frame_fakerFG.log'
    if (Test-Path -LiteralPath $one) { $list += @(Get-Item -LiteralPath $one) }
    return @($list | Sort-Object LastWriteTime)
}
function LinesInDefault([string]$needle) {
    $out = @()
    foreach ($f in (CandidateLogs)) {
        $rows = @(Select-String -LiteralPath $f.FullName -SimpleMatch -Pattern $needle |
                  ForEach-Object { $_.Line.Trim() })
        if ($rows.Count -eq 0) { continue }
        $isDefault = @(Select-String -LiteralPath $f.FullName -SimpleMatch -Pattern 'proj=game view-to-clip').Count -gt 0
        if ($isDefault) { $out += $rows }
    }
    return $out
}
function AllLines([string]$needle) {
    $out = @()
    foreach ($f in (CandidateLogs)) {
        $out += @(Select-String -LiteralPath $f.FullName -SimpleMatch -Pattern $needle |
                  ForEach-Object { $_.Line.Trim() })
    }
    return $out
}

if (@(CandidateLogs).Count -eq 0) {
    Warn "no consumer/AB logs (ab_logs\*.log, lock_frame_fakerFG.log) - run ab_50..ab_55 once"
} else {
    $all = @(AllLines '[lock] xefg-in:')
    if ($all.Count -eq 0) {
        Warn "no '[lock] xefg-in:' line in any log - the consumer never dispatched"
    } else {
        $default = @($all | Where-Object { $_ -match 'proj=game view-to-clip' })
        Info ("latest xefg-in   : " + $all[-1])
        if ($default.Count -gt 0) {
            $line = $default[-1]
            Info ("default xefg-in  : " + $line)
            Ok "a default-build session ran (proj=game view-to-clip)"
            if ($line -match 'view=camera') { Ok "viewMatrix comes from the camera basis (default build)" }
            else { Bad "default build used view=identity - the camera basis did not arrive" }
            if ($line -match 'mv=(\d+)x(\d+)')    { Info ("tagged MV        : " + $Matches[1] + "x" + $Matches[2]) }
            if ($line -match 'depth=(\d+)x(\d+)') { Info ("tagged depth     : " + $Matches[1] + "x" + $Matches[2]) }
            if ($line -match 'rect=(\d+)x(\d+)\+(\d+)\+(\d+)') {
                Info ("interp rect      : " + $Matches[1] + "x" + $Matches[2] + "+" + $Matches[3] + "+" + $Matches[4])
            }
            if ($line -match 'mvscale=\(([-0-9.]+), ([-0-9.]+)\)') {
                Info ("mvScale          : " + $Matches[1] + ", " + $Matches[2])
            }
        } else {
            Warn "no default-build session (proj=game view-to-clip) in the logs - run ab_50 or ab_53"
        }
        $rz = @($all | Where-Object { $_ -match 'proj=reversed-Z approximation \(--fg-proj-rz\)' })
        $id = @($all | Where-Object { $_ -match 'proj=identity' })
        Info ("control sessions : --fg-proj-rz=" + $rz.Count + ", --fg-no-proj=" + $id.Count)
        if ($rz.Count -gt 0 -and $rz[-1] -match 'view=camera') {
            Ok "the --fg-proj-rz control still used view=camera (isolates the projection)"
        }
    }

    $tmp = @(LinesInDefault '[lock] near/far:')
    if ($tmp.Count -gt 0) {
        Info $tmp[-1]
        if ($tmp[-1] -match 'camera=1') { Ok "near/far came from the producer camera (non-Unreal rule)" }
        else { Warn "near/far: camera=0 - the consumer used the matrix-derived fallback" }
    }

    $tmp = @(AllLines '[lock] initFlags=')
    if ($tmp.Count -gt 0) {
        Info $tmp[-1]
        if ($tmp[-1] -match 'inverted_depth=1') { Ok "INVERTED_DEPTH is set" }
        else { Warn "inverted_depth=0 - CP2077 needs 1; flags were frozen before the declaration arrived" }
    }

    $tmp = @(LinesInDefault '[lock] frame constants:')
    if ($tmp.Count -gt 0) {
        Info $tmp[-1]
        if ($tmp[-1] -match 'proj=game view-to-clip \(cached\)') {
            Warn "projection served from the 2 s cache - records with sl::Constants are sparse"
        }
        if ($tmp[-1] -match 'view=camera \(cached\)') {
            Warn "view served from the 2 s cache - camera basis is sparse"
        }
    }
}

Write-Host ("-" * 68)
if ($script:fail -eq 0) {
    Write-Host "RESULT: PASS ($($script:fail) failures)" -ForegroundColor Green
    exit 0
} else {
    Write-Host "RESULT: FAIL ($($script:fail) failures)" -ForegroundColor Red
    exit 1
}
