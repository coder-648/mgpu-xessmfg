# =============================================================================
#  mgpu ReShade add-on 安装器
#
#  用法（三种都行）：
#    1. 把游戏的 .exe 拖到 install.bat 上
#    2. install.bat "D:\Games\XXX\bin\x64"
#    3. 直接双击 install.bat，按提示输入目录
#
#  可选第二个参数：代理 DLL 名（默认 dxgi）
#    install.bat "D:\Games\XXX" d3d11
#    可选：dxgi | d3d11 | d3d12 | opengl32 | version
#
#  只做四件事，全部可逆（uninstall.bat 还原）：
#    - 把 ReShade 6.8 作为代理 DLL 放进游戏目录（同名文件先备份为 *.mgpu_bak）
#    - 放 game_capture.addon64（+ 它旁边的 game_capture.ini）
#    - 放 MGPU_DepthTap 这个空转效果并写进预设（没有启用的效果，ReShade 不会调
#      add-on 的 begin_effects，add-on 就不会工作）
#    - 写/补 ReShade.ini 里的 EffectSearchPaths 与 PresetPath
# =============================================================================

param(
    [Parameter(Position = 0)][string] $GameDir = '',
    [Parameter(Position = 1)][string] $Proxy   = 'dxgi',
    [switch] $Force,
    [switch] $NoPause
)

$ErrorActionPreference = 'Stop'

$here  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$files = Join-Path $here 'files'

function Info($m) { Write-Host "[安装] $m" }
function Warn($m) { Write-Host "[注意] $m" -ForegroundColor Yellow }
function Step($m) { Write-Host ""; Write-Host "=== $m ===" -ForegroundColor Cyan }
function Fail($m) {
    Write-Host ""
    Write-Host "[失败] $m" -ForegroundColor Red
    Write-Host ""
    if (-not $NoPause) { [void](Read-Host "按回车关闭") }
    exit 1
}
function Get-Sha($path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return '' }
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}

# ---------------------------------------------------------------- 参数检查
if (-not (Test-Path -LiteralPath $files -PathType Container)) {
    Fail "缺 files 目录，部署包不完整（请从解压后的根目录运行）"
}

$proxyList = @('dxgi', 'd3d11', 'd3d12', 'opengl32', 'version')
$Proxy = $Proxy.ToLower().Trim()
if ($proxyList -notcontains $Proxy) {
    Fail "代理 DLL 只能是: $($proxyList -join ' / ')（收到 '$Proxy'）"
}

if ($GameDir -ne '') {
    $GameDir = $GameDir.Trim().Trim('"').Trim("'")
    if ($GameDir -match '\.exe$') {
        Info "从 exe 推出目录: $GameDir"
        $GameDir = Split-Path -Parent $GameDir
    }
}

if ($GameDir -eq '' -or -not (Test-Path -LiteralPath $GameDir -PathType Container)) {
    Write-Host ""
    Write-Host "把游戏的 exe 拖到 install.bat 上，或者现在输入游戏目录"
    Write-Host "（目录要直接包含游戏的 exe，例如 ...\Cyberpunk 2077\bin\x64）"
    $GameDir = Read-Host "游戏目录"
    $GameDir = $GameDir.Trim().Trim('"').Trim("'")
    if ($GameDir -match '\.exe$') { $GameDir = Split-Path -Parent $GameDir }
}
if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) {
    Fail "目录不存在: $GameDir"
}
$GameDir = (Resolve-Path -LiteralPath $GameDir).Path
$GameDir = $GameDir.TrimEnd('\')

$exes = @(Get-ChildItem -LiteralPath $GameDir -Filter *.exe -File -ErrorAction SilentlyContinue)
if ($exes.Count -eq 0) {
    Warn "这个目录里没有 .exe —— 确认选的是游戏 exe 所在目录（不是上一级）"
}

Step "目标：$GameDir  (代理: $Proxy.dll)"


# ---------------------------------------------------------------- 游戏是否在跑
$running = @()
foreach ($p in @(Get-Process -ErrorAction SilentlyContinue)) {
    try {
        if ($p.Path -and $p.Path.StartsWith($GameDir, [StringComparison]::OrdinalIgnoreCase)) {
            $running += $p.ProcessName
        }
    } catch { }
}
if ($running.Count -gt 0) {
    $names = ($running | Sort-Object -Unique) -join ', '
    if (-not $Force) {
        Fail "游戏还在运行（$names）。先关掉游戏再装：DLL 被占用时写入会失败。"
    }
    Warn "游戏在运行（$names），-Force 已指定，继续（可能失败）"
}

# ---------------------------------------------------------------- 1. 代理 DLL
$reshade = Join-Path $files 'ReShade64.dll'
if (-not (Test-Path -LiteralPath $reshade)) { Fail "缺 files\ReShade64.dll" }
$reshadeSha = Get-Sha $reshade

$target = Join-Path $GameDir ($Proxy + '.dll')
$bak    = Join-Path $GameDir ($Proxy + '.dll.mgpu_bak')

if (Test-Path -LiteralPath $target) {
    $cur = Get-Sha $target
    if ($cur -eq $reshadeSha) {
        Info "$Proxy.dll 已经是本包里的 ReShade（无需改动）"
    } else {
        if (-not (Test-Path -LiteralPath $bak)) {
            Copy-Item -LiteralPath $target -Destination $bak -Force
            Info "原 $Proxy.dll 已备份为 $Proxy.dll.mgpu_bak"
        } else {
            Warn "$Proxy.dll.mgpu_bak 已存在（保留更早那份备份，不覆盖）"
        }
        Copy-Item -LiteralPath $reshade -Destination $target -Force
        Info "ReShade -> $Proxy.dll"
    }
} else {
    Copy-Item -LiteralPath $reshade -Destination $target -Force
    Info "ReShade -> $Proxy.dll（新建）"
}

# ---------------------------------------------------------------- 2. add-on + ini
$addonSrc = Join-Path $files 'game_capture.addon64'
if (-not (Test-Path -LiteralPath $addonSrc)) {
    Fail "缺 files\game_capture.addon64（先跑 scripts/build_addon.sh src/game_capture game_capture）"
}
$addonDst = Join-Path $GameDir 'game_capture.addon64'
$addonBak = Join-Path $GameDir 'game_capture.addon64.mgpu_bak'
if ((Test-Path -LiteralPath $addonDst) -and ((Get-Sha $addonDst) -ne (Get-Sha $addonSrc))) {
    Copy-Item -LiteralPath $addonDst -Destination $addonBak -Force
    Info "旧 add-on 备份为 game_capture.addon64.mgpu_bak"
}
Copy-Item -LiteralPath $addonSrc -Destination $addonDst -Force
Info "game_capture.addon64 已放入"

# add-on 读的是「自己旁边的同名 .ini」
$iniSrc = Join-Path $files 'game_capture.ini'
$iniDst = Join-Path $GameDir 'game_capture.ini'
if (-not (Test-Path -LiteralPath $iniDst)) {
    Copy-Item -LiteralPath $iniSrc -Destination $iniDst -Force
    Info "game_capture.ini 已写入（Color=0, StreamPercent=100）"
} elseif ($Force) {
    Copy-Item -LiteralPath $iniSrc -Destination $iniDst -Force
    Info "game_capture.ini 已被 -Force 覆盖"
} else {
    Info "game_capture.ini 已存在（保留；加 -Force 可覆盖）"
}

# ---------------------------------------------------------------- 3. 效果 + 预设
$fxDstDir = Join-Path $GameDir 'reshade-shaders\Shaders'
New-Item -ItemType Directory -Force -Path $fxDstDir | Out-Null
Copy-Item -LiteralPath (Join-Path $files 'reshade-shaders\Shaders\mgpu_depth_tap.fx') `
          -Destination (Join-Path $fxDstDir 'mgpu_depth_tap.fx') -Force
Info "mgpu_depth_tap.fx -> reshade-shaders\Shaders\"

$presetDst  = Join-Path $GameDir 'ReShadePreset.ini'
$presetSrc  = Join-Path $files 'ReShadePreset.ini'
$needPreset = $true
if (Test-Path -LiteralPath $presetDst) {
    $txt = [System.IO.File]::ReadAllText($presetDst)
    if ($txt -match 'MGPU_DepthTap') {
        $needPreset = $false
        Info "ReShadePreset.ini 里已经有 MGPU_DepthTap（保留）"
    } else {
        Copy-Item -LiteralPath $presetDst -Destination ($presetDst + '.mgpu_bak') -Force
        Info "原 ReShadePreset.ini 备份为 .mgpu_bak"
    }
}
if ($needPreset) {
    Copy-Item -LiteralPath $presetSrc -Destination $presetDst -Force
    Info "ReShadePreset.ini 已写入（启用 MGPU_DepthTap）"
}

# ---------------------------------------------------------------- 4. ReShade.ini
#  ReShade 自己会重写这个文件，所以只保证两个键：
#    [GENERAL] EffectSearchPaths 包含我们的 shader 目录
#    [GENERAL] PresetPath 指向 ReShadePreset.ini
function Set-IniKey {
    param([string]$Path, [string]$Section, [string]$Key, [string]$Value)
    $lines = @()
    if (Test-Path -LiteralPath $Path) { $lines = [System.IO.File]::ReadAllLines($Path) }
    $out = New-Object System.Collections.ArrayList
    $inTarget = $false
    $sectionSeen = $false
    $keyDone = $false
    foreach ($ln in $lines) {
        $trim = $ln.Trim()
        if ($trim -match '^\[(.+)\]$') {
            if ($inTarget -and -not $keyDone) { [void]$out.Add("$Key=$Value"); $keyDone = $true }
            $inTarget = ($Matches[1] -ieq $Section)
            if ($inTarget) { $sectionSeen = $true }
            [void]$out.Add($ln)
            continue
        }
        if ($inTarget -and $trim -match ('^' + [Regex]::Escape($Key) + '=')) {
            [void]$out.Add("$Key=$Value")
            $keyDone = $true
            continue
        }
        [void]$out.Add($ln)
    }
    if (-not $sectionSeen) {
        if ($out.Count -gt 0) { [void]$out.Add('') }
        [void]$out.Add("[$Section]")
        [void]$out.Add("$Key=$Value")
    } elseif (-not $keyDone) {
        [void]$out.Add("$Key=$Value")
    }
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllLines($Path, $out.ToArray(), $utf8)
}

$rini = Join-Path $GameDir 'ReShade.ini'
if (-not (Test-Path -LiteralPath $rini)) {
    Copy-Item -LiteralPath (Join-Path $files 'ReShade.ini') -Destination $rini -Force
    Info "ReShade.ini 已写入（全新）"
} else {
    Copy-Item -LiteralPath $rini -Destination ($rini + '.mgpu_bak') -Force
    $cur = [System.IO.File]::ReadAllText($rini)
    $hasFx = $false
    if ($cur -match '(?im)^EffectSearchPaths=(.*)$') {
        if ($Matches[1] -match 'reshade-shaders') { $hasFx = $true }
    }
    if (-not $hasFx) {
        $val = '.\reshade-shaders\Shaders\**'
        if ($cur -match '(?im)^EffectSearchPaths=(.+)$') {
            $val = $Matches[1].TrimEnd() + ',.\reshade-shaders\Shaders\**'
        }
        Set-IniKey -Path $rini -Section 'GENERAL' -Key 'EffectSearchPaths' -Value $val
        Info "ReShade.ini: EffectSearchPaths 已补上 shader 目录"
    }
    if ($cur -notmatch '(?im)^PresetPath=') {
        Set-IniKey -Path $rini -Section 'GENERAL' -Key 'PresetPath' -Value '.\ReShadePreset.ini'
        Info "ReShade.ini: PresetPath 已补上"
    }
    Info "ReShade.ini 已检查（原文件备份为 ReShade.ini.mgpu_bak）"
}

# ---------------------------------------------------------------- 校验 + 收尾
Step "校验"
$bad = 0
if ((Get-Sha $target) -ne $reshadeSha) { Warn "代理 DLL 校验不一致（写盘被拦？）"; $bad++ }
if ((Get-Sha $addonDst) -ne (Get-Sha $addonSrc)) { Warn "add-on 校验不一致"; $bad++ }
if (-not (Test-Path -LiteralPath (Join-Path $fxDstDir 'mgpu_depth_tap.fx'))) { Warn "shader 没落地"; $bad++ }
if ($bad -eq 0) { Info "文件全部就位" }

Write-Host ""
Write-Host "装好了。接下来：" -ForegroundColor Green
Write-Host "  1. 启动游戏（第一次 ReShade 会弹一个加载提示条，按 Home 打开面板）"
Write-Host "  2. 游戏里关掉帧生成，抗锯齿选 DLSS（不要 TSR / FSR）"
Write-Host "  3. 打开 mgpu_ui.exe 选这个游戏窗口，按热键开始（或用 bridge.exe --game <exe>）"
Write-Host ""
Write-Host ("  ReShade / add-on 日志: {0}\ReShade.log" -f $GameDir)
Write-Host ("  还原: 运行 uninstall.bat `"{0}`"" -f $GameDir)
Write-Host ""
if (-not $NoPause) { [void](Read-Host "按回车关闭") }

