<#
.SYNOPSIS
  抓一套可复用的降采样/传输研究数据集（colour + MV + depth + meta）。

.DESCRIPTION
  对应 docs/design/LOSSLESS_TRANSPORT.md §11.5。它做四件事：

    1. 备份 <GameDir>\fakerFG.ini，并强制
         ColourStream=1, StreamPercent[Mv|Depth]=<Percent>
    2. 让你重开游戏（producer 每个会话只读一次 ini —— 不重启就不会生效）
    3. 跑 lock_frame.exe --dump N 到 <Out>\<stamp>_<Tag>\seg-<Tag>\
    4. 等到日志出现 "dump complete" 后结束消费者、还原 ini、写 manifest.json

  抓完在 WSL 里跑：
      bash scripts/analyse_dataset.sh <dataset-dir>

.PARAMETER Tag      场景标签，决定子目录名。默认 cp2077
.PARAMETER Frames   抓多少帧（1..64，程序上限 64）。默认 32
.PARAMETER Percent  StreamPercent[Mv|Depth]（100 = 全分辨率、不重采样）。默认 100
.PARAMETER OutRoot  数据集根目录，默认 <本脚本目录>\dataset
.PARAMETER GameDir  游戏的 bin\x64 目录
.PARAMETER GameExe  窗口所属进程名（只写文件名，lock_frame 按 exe 名找进程）
.PARAMETER After    先跳过多少帧再开始抓（避开菜单/加载）。默认 120
.PARAMETER Interp   插帧数（1 = 2x，2 = 3x）。默认 1
.PARAMETER TimeoutSec 等 "dump complete" 的上限。默认 900
.PARAMETER KeepIni  抓完不回滚 fakerFG.ini
.PARAMETER NoRestartPrompt 不询问重启（你确认 ini 早就对了）
.PARAMETER DryRun   只打印计划，不写文件、不启动任何进程

.NOTES
  数据集里每个 seg 目录的每个 fNN_*.raw 都是：
    4 x uint32 {width, height, dxgiFormat, rowPitchBytes} + 行数据
  与 scripts/analyse_dump.py、scripts/lossless_ratio.py 兼容。

  磁盘：每帧 ≈ 66.4 MB(colour 4K FP16) + 2 x MV/depth；Percent=100 时约 133 MB/帧，
  32 帧 ≈ 4.3 GB。脚本会先算一遍并检查剩余空间。
#>
[CmdletBinding()]
param(
    [string]$Tag = "cp2077",
    [int]$Frames = 32,
    [int]$Percent = 100,
    [string]$OutRoot = "",
    [string]$GameDir = "E:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64",
    [string]$GameExe = "Cyberpunk2077.exe",
    [int]$After = 120,
    [int]$Interp = 1,
    [int]$TimeoutSec = 900,
    [switch]$KeepIni,
    [switch]$NoRestartPrompt,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
function Say([string]$m) { Write-Host ("[capture] " + $m) }
function Die([string]$m) { Write-Host ("[capture] ERROR: " + $m) -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------- sanity -----
if ($Frames -lt 1 -or $Frames -gt 64) { Die "Frames must be 1..64 (lock_frame caps --dump at 64)" }
if ($Percent -lt 100 -or $Percent -gt 400) { Die "Percent must be 100..400" }
if (-not (Test-Path -LiteralPath $GameDir)) { Die "GameDir not found: $GameDir" }
$exePath   = Join-Path $GameDir "lock_frame.exe"
$iniPath   = Join-Path $GameDir "fakerFG.ini"
$gameExeP  = Join-Path $GameDir $GameExe
foreach ($f in @($exePath, $iniPath, $gameExeP)) {
    if (-not (Test-Path -LiteralPath $f)) { Die ("missing: " + $f) }
}
if (-not (Test-Path -LiteralPath (Join-Path $GameDir "xess_mfg_producer.dll"))) {
    Say "WARNING: xess_mfg_producer.dll not found next to the game -- the producer cannot publish"
}

$stamp   = Get-Date -Format "yyyyMMdd_HHmmss"
# 默认写到 Windows 用户目录：脚本可能被放在 WSL 里（\wsl.localhost\...），
# 4 GB 的 dump 不该走 9p。用 -OutRoot 换盘（建议和游戏同盘，例如 E:\mgpu_dataset）。
if ([string]::IsNullOrWhiteSpace($OutRoot)) {
    $OutRoot = Join-Path $env:USERPROFILE "mgpu_dataset"
}
$Dataset = Join-Path $OutRoot ($stamp + "_" + $Tag)
$Seg     = Join-Path $Dataset ("seg-" + $Tag)
if ($Seg -match ' ') {
    Say "WARNING: the output path contains a space; --dump-dir is quoted so it should work,"
    Say "         but if no files appear, re-run with -OutRoot E:\mgpu_dataset style paths"
}
$logPath = Join-Path $Dataset "lock_frame.log"
$errPath = Join-Path $Dataset "lock_frame.stderr.log"

# 每帧字节估算（4K 假设；Percent>100 时 MV/depth 按面积缩小）
$scale    = [Math]::Pow(100.0 / $Percent, 2)
$perFrame = 66.4 + 2.0 * 33.2 * $scale          # MB
$needMB   = [Math]::Round($perFrame * $Frames * 1.15, 0)
Say ("dataset  : " + $Dataset)
Say ("segment  : " + $Seg)
Say ("plan     : " + $Frames + " frames, Percent=" + $Percent + ", interp=" + $Interp + ", dump-after=" + $After)
Say ("disk     : about " + [Math]::Round($perFrame,1) + " MB/frame -> need ~" + $needMB + " MB")

# --------------------------------------------------------------- ini edit -----
# 只改这四个键，其余原样保留（FgInput / FgOutput / Passthrough / UiSource ... 都不动）。
# producer 用 GetPrivateProfileIntW 读 [fakerFG]，所以必须留在该 section 里。
$want = [ordered]@{
    "ColourStream"        = "1"
    "StreamPercent"       = "$Percent"
    "StreamPercentMv"     = "$Percent"
    "StreamPercentDepth"  = "$Percent"
}

function Read-IniValues([string]$path, [string[]]$keys) {
    $found = @{}
    foreach ($line in (Get-Content -LiteralPath $path)) {
        if ($line -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') {
            $k = $Matches[1]
            if ($keys -contains $k) { $found[$k] = $Matches[2] }
        }
    }
    return $found
}

function Write-IniValues([string]$path, $wanted) {
    $lines = @(Get-Content -LiteralPath $path)
    $seen  = @{}
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*([A-Za-z0-9_]+)\s*=') {
            $k = $Matches[1]
            if ($wanted.Contains($k)) {
                $lines[$i] = $k + "=" + $wanted[$k]
                $seen[$k]  = $true
            }
        }
    }
    $extra = @()
    foreach ($k in $wanted.Keys) { if (-not $seen.ContainsKey($k)) { $extra += ($k + "=" + $wanted[$k]) } }
    if ($extra.Count -gt 0) { $lines = $lines + $extra }
    # UTF8 (= UTF-8 with BOM in PS 5.1), same as the ini the UI writes
    Set-Content -LiteralPath $path -Value $lines -Encoding UTF8
}

$before = Read-IniValues $iniPath @($want.Keys)
$changed = @()
foreach ($k in $want.Keys) {
    $old = ""
    if ($before.ContainsKey($k)) { $old = $before[$k] }
    if ($old -ne $want[$k]) { $changed += ($k + ": " + $old + " -> " + $want[$k]) }
}
if ($changed.Count -gt 0) {
    Say "ini changes needed:"
    foreach ($c in $changed) { Say ("  " + $c) }
} else {
    Say "ini already correct for this capture"
}

# ------------------------------------------------------------- free space -----
$freeMB = -1
try {
    $qual = Split-Path -Qualifier $OutRoot -ErrorAction Stop      # e.g. "E:"
    $drv  = Get-PSDrive -Name $qual.TrimEnd(':') -ErrorAction Stop
    $freeMB = [Math]::Round($drv.Free / 1MB, 0)
    Say ("free on " + $qual + " " + $freeMB + " MB")
} catch {
    Say "WARNING: could not read free space (continuing)"
}
if ($freeMB -ge 0 -and $freeMB -lt $needMB) {
    Die ("not enough space: need ~" + $needMB + " MB, have " + $freeMB + " MB")
}


if ($DryRun) {
    Say "--- dry run: nothing written, nothing started ---"
    Say ("would run: lock_frame.exe --game " + $GameExe + " --streams=mv,depth,color,meta --interp " +
         $Interp + " --dump " + $Frames + " --dump-after " + $After + ' --dump-dir "' + $Seg + '"')
    exit 0
}

# ------------------------------------------------------- prepare dataset ------
New-Item -ItemType Directory -Path $Seg -Force | Out-Null
Copy-Item -LiteralPath $iniPath -Destination (Join-Path $Dataset "fakerFG.ini.before") -Force
if ($changed.Count -gt 0) {
    Write-IniValues $iniPath $want
    Copy-Item -LiteralPath $iniPath -Destination (Join-Path $Dataset "fakerFG.ini.after") -Force
}

# producer 每会话只读一次 ini：改过就必须重开游戏，否则抓到的是旧档位。
if ($changed.Count -gt 0 -and -not $NoRestartPrompt) {
    Write-Host ""
    Write-Host "==============================================================" -ForegroundColor Yellow
    Write-Host " fakerFG.ini 改过了 —— 现在请：" -ForegroundColor Yellow
    Write-Host "   1. 完全退出 Cyberpunk 2077（不是回主菜单）"
    Write-Host "   2. 重新启动，进到世界里，站在会动的地方（走/平移，速度要有变化）"
    Write-Host "   3. 确认游戏是无边框/窗口化全屏，超分开着，游戏自己的帧生成开着"
    Write-Host "   4. 回到这里按回车，脚本会启动 lock_frame.exe 开始抓"
    Write-Host "==============================================================" -ForegroundColor Yellow
    Write-Host ""
    [void](Read-Host "准备好后按回车")
}

# 已有一个消费者在跑会抢同一个 mailbox，必须先关掉
$stale = Get-Process -Name "lock_frame" -ErrorAction SilentlyContinue
if ($stale) {
    Say ("closing " + $stale.Count + " running lock_frame.exe (only one consumer may attach)")
    $stale | Stop-Process -Force
    Start-Sleep -Milliseconds 800
}

$gp = Get-Process -Name ([System.IO.Path]::GetFileNameWithoutExtension($GameExe)) -ErrorAction SilentlyContinue
if (-not $gp) {
    Die ("game is not running: " + $GameExe + " -- start it first (the producer lives in the game process)")
}
Say ("game pid " + @($gp)[0].Id)

# ---------------------------------------------------------------- capture -----
# --game 只接受 **exe 名**（lock_frame 用 CreateToolhelp32Snapshot 比对 szExeFile），
# 传全路径会匹配不到进程；GameDir 只用来定位二进制。
$argline = '--game "' + $GameExe + '" --streams=mv,depth,color,meta' +
           ' --interp ' + $Interp +
           ' --dump ' + $Frames +
           ' --dump-after ' + $After +
           ' --dump-dir "' + $Seg + '"'
Say ("run: lock_frame.exe " + $argline)

if (Test-Path -LiteralPath $logPath)    { Remove-Item -LiteralPath $logPath -Force }
if (Test-Path -LiteralPath $errPath)    { Remove-Item -LiteralPath $errPath -Force }

$proc = Start-Process -FilePath $exePath -ArgumentList $argline `
                      -WorkingDirectory $GameDir `
                      -RedirectStandardOutput $logPath `
                      -RedirectStandardError $errPath `
                      -PassThru
Say ("consumer pid " + $proc.Id + " -> log " + $logPath)

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$done = $false
$exitEarly = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    $alive = $true
    try { $alive = -not $proc.HasExited } catch { $alive = $false }
    if (-not $alive) { $exitEarly = $true; break }
    if (Test-Path -LiteralPath $logPath) {
        $tail = @(Get-Content -LiteralPath $logPath -Tail 120 -ErrorAction SilentlyContinue)
        foreach ($line in $tail) { if ($line -like "*dump complete*") { $done = $true; break } }
        if ($done) { break }
    }
    $metas = @(Get-ChildItem -LiteralPath $Seg -Filter "*_meta.txt" -ErrorAction SilentlyContinue)
    if ($metas.Count -ge $Frames) {
        # 文件写到一半时 count 也会先到位；等一拍再看 "dump complete"，或者再等 2 秒
        Start-Sleep -Seconds 2
        $done = $true
        if (Test-Path -LiteralPath $logPath) {
            $tail2 = @(Get-Content -LiteralPath $logPath -Tail 40 -ErrorAction SilentlyContinue)
            foreach ($line in $tail2) { if ($line -like "*dump complete*") { $done = $true } }
        }
        break
    }
}

if ($exitEarly) { Say "consumer exited before the dump finished (see the log below)" }

if (-not $done) {
    Say "did not see 'dump complete' before the timeout / exit"
}
try { if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force } } catch { }
Start-Sleep -Milliseconds 600

if (Test-Path -LiteralPath $errPath) {
    $errTxt = Get-Content -LiteralPath $errPath -Raw -ErrorAction SilentlyContinue
    if ($errTxt) { Add-Content -LiteralPath $logPath -Value $errTxt }
    Remove-Item -LiteralPath $errPath -Force -ErrorAction SilentlyContinue
}


# -------------------------------------------------------------- validate ------
$metas = @(Get-ChildItem -LiteralPath $Seg -Filter "*_meta.txt" | Sort-Object Name)
$raws  = @(Get-ChildItem -LiteralPath $Seg -Filter "*.raw")
$checks = New-Object System.Collections.ArrayList
function Add-Check([string]$name, [bool]$ok, [string]$detail) {
    [void]$checks.Add([ordered]@{ check = $name; ok = $ok; detail = $detail })
}

$noMv     = 0
$noDepth  = 0
$noColour = 0
$mvSizes  = @{}
$dSizes   = @{}
$cSizes   = @{}
foreach ($m in $metas) {
    $txt = Get-Content -LiteralPath $m.FullName -Raw
    if ($txt -match 'flags\s+no_mv\s+(\d+)\s+no_depth\s+(\d+)') {
        if ($Matches[1] -ne "0") { $noMv++ }
        if ($Matches[2] -ne "0") { $noDepth++ }
    }
    if ($txt -match 'present\s+colour=(\d+)\s+depth=(\d+)\s+mv=(\d+)') {
        if ($Matches[1] -ne "1") { $noColour++ }
    }
    if ($txt -match '(?m)^mv\s+(\d+)x(\d+)')       { $mvSizes[$Matches[1] + "x" + $Matches[2]] = 1 }
    if ($txt -match '(?m)^depth\s+(\d+)x(\d+)')    { $dSizes[$Matches[1] + "x" + $Matches[2]] = 1 }
    if ($txt -match '(?m)^colour\s+(\d+)x(\d+)')   { $cSizes[$Matches[1] + "x" + $Matches[2]] = 1 }
}

Add-Check "frames captured" ($metas.Count -ge $Frames) ("got " + $metas.Count + " / " + $Frames)
Add-Check "mv not skipped (no_mv=0)"  ($noMv -eq 0)    ($noMv.ToString() + " meta file(s) say no_mv 1")
Add-Check "depth not skipped"         ($noDepth -eq 0) ($noDepth.ToString() + " meta file(s) say no_depth 1")
Add-Check "colour present"            ($noColour -eq 0)($noColour.ToString() + " meta file(s) without colour")
Add-Check "raw files >= 3 per frame"  ($raws.Count -ge 3 * $metas.Count) ($raws.Count.ToString() + " raw file(s)")

Say ""
Say "checks:"
foreach ($c in $checks) {
    $mark = "OK  "
    if (-not $c.ok) { $mark = "FAIL" }
    Write-Host ("  [" + $mark + "] " + $c.check + " - " + $c.detail)
}
if (@($checks | Where-Object { -not $_.ok }).Count -gt 0) {
    Say "the dataset is usable but incomplete; --dump cannot answer the MV question without MV"
}

# -------------------------------------------------------------- manifest ------
$manifest = [ordered]@{
    tool          = "lock_frame/scripts/capture_dataset.ps1"
    captured_at   = (Get-Date).ToString("s")
    tag           = $Tag
    frames        = $Frames
    percent       = $Percent
    interp        = $Interp
    dump_after    = $After
    game_dir      = $GameDir
    game_exe      = $GameExe
    command       = ("lock_frame.exe " + $argline)
    segment       = $Seg
    meta_count    = $metas.Count
    raw_count     = $raws.Count
    mv_formats    = @($mvSizes.Keys)
    depth_formats = @($dSizes.Keys)
    colour_formats= @($cSizes.Keys)
    ini_before    = (Read-IniValues (Join-Path $Dataset "fakerFG.ini.before") @($want.Keys))
    ini_changed   = ($changed.Count -gt 0)
    checks        = $checks
}
$manifestPath = Join-Path $Dataset "manifest.json"
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Say ("manifest -> " + $manifestPath)

$readme = @()
$readme += "dataset: " + $Dataset
$readme += "segment: " + $Seg + "  (" + $metas.Count + " frames, Percent=" + $Percent + ")"
$readme += ""
$readme += "WSL / Linux 分析（三件套）："
$readme += '  bash scripts/analyse_dataset.sh "' + $Dataset + '"'
$readme += ""
$readme += "单独跑："
$readme += '  python3 scripts/lossless_ratio.py "' + $Seg + '"        # 快速一维近似 + VERDICT'
$readme += '  bash tests/lossless_bench/run.sh "' + $Seg + '"        # 精确码流 + bit-exact 回环'
$readme += '  python3 scripts/analyse_dump.py "' + $Seg + '"        # 配对 / MV 语义（需要 numpy）'
$readme += ""
$readme += "判据（docs/design/LOSSLESS_TRANSPORT.md §8）："
$readme += "  MV word p5 >= 2.0  -> 继续 gate 2（EPE）"
$readme += "  MV word p5 <  1.5  -> 放弃这条路"
Set-Content -LiteralPath (Join-Path $Dataset "README.txt") -Value $readme -Encoding UTF8

# --------------------------------------------------------------- restore ------
if (-not $KeepIni -and $changed.Count -gt 0) {
    Copy-Item -LiteralPath (Join-Path $Dataset "fakerFG.ini.before") -Destination $iniPath -Force
    Say "fakerFG.ini restored (producer reads it once per session, so restart the game to go back)"
}

Say ""
Say "done. dataset:"
Say ("  " + $Dataset)
Say "next (WSL):"
Say ('  bash scripts/analyse_dataset.sh "' + $Dataset + '"')

