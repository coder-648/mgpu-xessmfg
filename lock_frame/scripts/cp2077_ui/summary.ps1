param(
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$UiDir,
    [Parameter(Mandatory = $true)][string]$GameDir
)
$ErrorActionPreference = 'Stop'
$logDir = Join-Path $UiDir 'logs'

$patterns = @(
    'ui-unblend',
    'ui-patch',
    'ui: mode',
    'ui: ',
    'ui-synth',
    'tag-dump',
    'ui_quality',
    'pair:',
    'bus:',
    'headroom',
    'cadence:',
    'coded ui',
    'fr: coded',
    'UI source',
    'HUDless patch',
    'UiPatch',
    'UI [0-9]+x[0-9]+',
    'transport plan',
    'ui tag',
    'hudless',
    'WARNING',
    'rejected'
)

function Dump-Log {
    param([string]$Title, [string]$Path, [int]$Max = 160)
    Add-Content -LiteralPath $Out -Value ""
    Add-Content -LiteralPath $Out -Value ("=== " + $Title + " ===")
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) {
        Add-Content -LiteralPath $Out -Value "(missing)"
        return
    }
    $f = Get-Item -LiteralPath $Path
    Add-Content -LiteralPath $Out -Value ("file: " + $f.FullName)
    Add-Content -LiteralPath $Out -Value ("size: " + $f.Length + " bytes, mtime: " + $f.LastWriteTime)
    $hits = @(Select-String -LiteralPath $f.FullName -Pattern $patterns -ErrorAction SilentlyContinue |
              Select-Object -First $Max)
    if ($hits.Count -eq 0) {
        Add-Content -LiteralPath $Out -Value "(no matching lines)"
    } else {
        foreach ($h in $hits) { Add-Content -LiteralPath $Out -Value $h.Line }
        if ($hits.Count -ge $Max) { Add-Content -LiteralPath $Out -Value ("... (first $Max matches only)") }
    }
}

function Latest([string]$Filter) {
    $it = Get-ChildItem -LiteralPath $logDir -Filter $Filter -File -ErrorAction SilentlyContinue |
          Sort-Object LastWriteTime | Select-Object -Last 1
    if ($it) { return $it.FullName }
    return $null
}

function Session-Lines([string]$Path) {
    $all = @(Get-Content -LiteralPath $Path)
    $start = 0
    for ($i = $all.Count - 1; $i -ge 0; $i--) {
        if ($all[$i] -match '^==== ') { $start = $i; break }
    }
    if ($start -gt 0) { return @($all[$start..($all.Count - 1)]) }
    return $all
}

function Dump-SessionLog {
    param([string]$Title, [string]$Path, [int]$Max = 160)
    Add-Content -LiteralPath $Out -Value ""
    Add-Content -LiteralPath $Out -Value ("=== " + $Title + " (last session) ===")
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) {
        Add-Content -LiteralPath $Out -Value "(missing)"
        return
    }
    $f = Get-Item -LiteralPath $Path
    Add-Content -LiteralPath $Out -Value ("file: " + $f.FullName)
    Add-Content -LiteralPath $Out -Value ("size: " + $f.Length + " bytes, mtime: " + $f.LastWriteTime)
    $lines = @(Session-Lines $f.FullName)
    $hits = @($lines | Select-String -Pattern $patterns -ErrorAction SilentlyContinue |
              Select-Object -First $Max)
    if ($hits.Count -eq 0) {
        Add-Content -LiteralPath $Out -Value "(no matching lines)"
    } else {
        foreach ($h in $hits) { Add-Content -LiteralPath $Out -Value $h.Line }
        if ($hits.Count -ge $Max) { Add-Content -LiteralPath $Out -Value ("... (first $Max matches only)") }
    }
}

Set-Content -LiteralPath $Out -Value ("CP2077 UI composition summary  " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Add-Content -LiteralPath $Out -Value ("ui dir   : " + $UiDir)
Add-Content -LiteralPath $Out -Value ("game dir : " + $GameDir)
Add-Content -LiteralPath $Out -Value ""

$run4 = Latest 'run_m4_*.log'
$run5 = Latest 'run_m5_*.log'
$patch = Latest 'run_patch_*.log'
$base = Latest 'run_base_*.log'
Dump-Log 'mode 4 run (full-res synth UI -> HUDless)' $run4 200
Dump-Log 'mode 5 run (HUDless + UI)' $run5 200
Dump-Log 'v33 patch run (HUDless scene under UI -> mode 4)' $patch 200
Dump-Log 'baseline run (UI stream off)' $base 120
Dump-SessionLog 'producer log (game side)' (Join-Path $GameDir 'Cyberpunk2077_mfg_producer.log') 160
Dump-SessionLog 'fakerFG log (game side)' (Join-Path $GameDir 'Cyberpunk2077_fakerFG.log') 100

$tagDirs = @()
foreach ($pat in @('tag_m4_*', 'tag_m5_*', 'tag_patch_*')) {
    $d = Get-ChildItem -LiteralPath $logDir -Filter $pat -Directory -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime | Select-Object -Last 1
    if ($d) { $tagDirs += $d }
}
foreach ($d in $tagDirs) {
    Add-Content -LiteralPath $Out -Value ""
    Add-Content -LiteralPath $Out -Value ("=== tag dump: " + $d.Name + " ===")
    $metas = @(Get-ChildItem -LiteralPath $d.FullName -Filter 'tag_*_meta.txt' -File -ErrorAction SilentlyContinue |
               Sort-Object Name | Select-Object -First 3)
    foreach ($m in $metas) {
        Add-Content -LiteralPath $Out -Value ("--- " + $m.Name)
        Get-Content -LiteralPath $m.FullName | ForEach-Object { Add-Content -LiteralPath $Out -Value $_ }
    }
    $bmp = @(Get-ChildItem -LiteralPath $d.FullName -Filter '*.bmp' -File -ErrorAction SilentlyContinue |
             Sort-Object Name | Select-Object -First 6)
    foreach ($b in $bmp) {
        Add-Content -LiteralPath $Out -Value ("bmp: " + $b.Name + "  " + $b.Length + " bytes")
    }
}

Add-Content -LiteralPath $Out -Value ""
Add-Content -LiteralPath $Out -Value "=== how to read ==="
Add-Content -LiteralPath $Out -Value "consumer: 'ui-unblend: HUDless rebuilt' = the path is active."
Add-Content -LiteralPath $Out -Value "consumer: 'ui: mode 5' = HUDless+UI tagged (default since 2026-11-23); 'mode 4' only with --no-ui-mode5."
Add-Content -LiteralPath $Out -Value "consumer: 'ui-synth: coverage' / 'coded ui' lines are in the producer log."
Add-Content -LiteralPath $Out -Value "producer: 'UI source = HUDless/Synth', 'UI 3840x2160 ... -> 3840x2160' = full-res."
Add-Content -LiteralPath $Out -Value "tag meta: ui_quality_bp is informational (no longer gates mode 5); linear 1 = linear-light; derived 1 = render-card synth."
Add-Content -LiteralPath $Out -Value "v33 patch: meta 'patch 1' and 'reconstruct_bmp' point at the support patch; tag_XX_patch.bmp black tiles are the ones copied from present."
Add-Content -LiteralPath $Out -Value "v33 patch: consumer must NOT print 'ui: mode 5'; producer must print 'UI source = HUDless patch under UI'."
Write-Host "[summary] $Out"
