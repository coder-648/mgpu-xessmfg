# =============================================================================
#  mgpu ReShade add-on 卸载器（把 install.ps1 做的事全部还原）
#
#  用法：uninstall.bat "D:\Games\XXX\bin\x64"  [dxgi]
#        或者把游戏 exe 拖到 uninstall.bat 上
# =============================================================================

param(
    [Parameter(Position = 0)][string] $GameDir = '',
    [Parameter(Position = 1)][string] $Proxy   = 'dxgi',
    [switch] $NoPause
)

$ErrorActionPreference = 'Stop'
$here  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$files = Join-Path $here 'files'

function Info($m) { Write-Host "[卸载] $m" }
function Warn($m) { Write-Host "[注意] $m" -ForegroundColor Yellow }
function Fail($m) {
    Write-Host ""; Write-Host "[失败] $m" -ForegroundColor Red; Write-Host ""
    [void](Read-Host "按回车关闭"); exit 1
}
function Get-Sha($path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return '' }
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}

$Proxy = $Proxy.ToLower().Trim()
if ($GameDir -ne '') {
    $GameDir = $GameDir.Trim().Trim('"').Trim("'")
    if ($GameDir -match '\.exe$') { $GameDir = Split-Path -Parent $GameDir }
}
if ($GameDir -eq '' -or -not (Test-Path -LiteralPath $GameDir -PathType Container)) {
    Write-Host "把游戏的 exe 拖到 uninstall.bat 上，或者输入游戏目录"
    $GameDir = (Read-Host "游戏目录").Trim().Trim('"').Trim("'")
    if ($GameDir -match '\.exe$') { $GameDir = Split-Path -Parent $GameDir }
}
if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) { Fail "目录不存在: $GameDir" }
$GameDir = ((Resolve-Path -LiteralPath $GameDir).Path).TrimEnd('\')

# 游戏开着时，DLL 删不掉
$running = @()
foreach ($p in @(Get-Process -ErrorAction SilentlyContinue)) {
    try {
        if ($p.Path -and $p.Path.StartsWith($GameDir, [StringComparison]::OrdinalIgnoreCase)) {
            $running += $p.ProcessName
        }
    } catch { }
}
if ($running.Count -gt 0) {
    Fail ("游戏还在运行（" + (($running | Sort-Object -Unique) -join ', ') + "），先关掉再卸载")
}

# --- add-on / ini / shader / preset
$pairs = @(
    @('game_capture.addon64', 'game_capture.addon64.mgpu_bak'),
    @('game_capture.ini',     '')
)
foreach ($pair in $pairs) {
    $dst = Join-Path $GameDir $pair[0]
    $bak = ''
    if ($pair[1] -ne '') { $bak = Join-Path $GameDir $pair[1] }
    if ($bak -ne '' -and (Test-Path -LiteralPath $bak)) {
        Move-Item -LiteralPath $bak -Destination $dst -Force
        Info "$($pair[0]) 已从备份还原"
    } elseif (Test-Path -LiteralPath $dst) {
        Remove-Item -LiteralPath $dst -Force
        Info "$($pair[0]) 已删除"
    }
}

$fx = Join-Path $GameDir 'reshade-shaders\Shaders\mgpu_depth_tap.fx'
if (Test-Path -LiteralPath $fx) { Remove-Item -LiteralPath $fx -Force; Info "mgpu_depth_tap.fx 已删除" }
# 只有我们自己建出来的空目录才清理
foreach ($d in @((Join-Path $GameDir 'reshade-shaders\Shaders'), (Join-Path $GameDir 'reshade-shaders'))) {
    if (Test-Path -LiteralPath $d) {
        $left = @(Get-ChildItem -LiteralPath $d -Force -ErrorAction SilentlyContinue)
        if ($left.Count -eq 0) { Remove-Item -LiteralPath $d -Force; Info "空目录已清理: $d" }
    }
}

$preset = Join-Path $GameDir 'ReShadePreset.ini'
$presetBak = $preset + '.mgpu_bak'
if (Test-Path -LiteralPath $presetBak) {
    Move-Item -LiteralPath $presetBak -Destination $preset -Force
    Info "ReShadePreset.ini 已从备份还原"
} elseif (Test-Path -LiteralPath $preset) {
    $txt = [System.IO.File]::ReadAllText($preset)
    if ($txt -match 'MGPU_DepthTap') { Remove-Item -LiteralPath $preset -Force; Info "ReShadePreset.ini 已删除" }
}

$rini = Join-Path $GameDir 'ReShade.ini'
if (Test-Path -LiteralPath ($rini + '.mgpu_bak')) {
    Move-Item -LiteralPath ($rini + '.mgpu_bak') -Destination $rini -Force
    Info "ReShade.ini 已从备份还原"
} elseif (Test-Path -LiteralPath $rini) {
    # 没有备份 = 这个文件是我们新建的（装之前游戏目录里没有 ReShade）
    $template = Join-Path $files 'ReShade.ini'
    if ((Test-Path -LiteralPath $template) -and ((Get-Sha $rini) -eq (Get-Sha $template))) {
        Remove-Item -LiteralPath $rini -Force
        Info "ReShade.ini 已删除（安装时新建的）"
    } else {
        Warn "ReShade.ini 内容已被 ReShade 改写过（保留）"
    }
}

# --- 代理 DLL：优先还原备份，否则只在确认是我们放的那份时删除
$target = Join-Path $GameDir ($Proxy + '.dll')
$bak    = Join-Path $GameDir ($Proxy + '.dll.mgpu_bak')
$ours   = Get-Sha (Join-Path $files 'ReShade64.dll')
if (Test-Path -LiteralPath $bak) {
    Move-Item -LiteralPath $bak -Destination $target -Force
    Info "$Proxy.dll 已从备份还原"
} elseif (Test-Path -LiteralPath $target) {
    if ($ours -ne '' -and (Get-Sha $target) -eq $ours) {
        Remove-Item -LiteralPath $target -Force
        Info "$Proxy.dll 已删除（确认是本包放的 ReShade）"
    } else {
        Warn "$Proxy.dll 不是本包放的，保持原样（如需删除请手动处理）"
    }
}

Write-Host ""
Write-Host "卸载完成。" -ForegroundColor Green
Write-Host ""
if (-not $NoPause) { [void](Read-Host "按回车关闭") }
