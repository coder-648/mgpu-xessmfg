# =============================================================================
#  mgpu ReShade add-on 安装器（强制覆盖版）
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
#  ---------------------------------------------------------------------------
#  这一版的行为：**所有文件一律覆盖成本包里的版本**
#
#  为什么：上一版装过的旧 add-on / 旧 game_capture.ini / 旧预设，会让游戏"看起来
#  装好了但跑不起来"—— 最常见的是 add-on 与 bridge 协议对不上（拒绝通信），或者
#  ini 里留着旧参数（SkipMv=1、没有 StreamHeight）导致行为不是文档里写的那个。
#  所以这里不再"存在就跳过"，全部强制覆盖。
#
#  覆盖前会把原文件改名备份（不会丢，也不会覆盖掉更早的那份原件）：
#       xxx.dll        -> xxx.dll.mgpu_bak   （已存在则 .mgpu_bak2 … 最多到 9）
#       game_capture.ini -> game_capture.ini.mgpu_bak2 …
#  uninstall.bat 会用这些备份还原。
#
#  开关：
#    -KeepReShadeIni   保留游戏里已有的 ReShade.ini（只补 EffectSearchPaths /
#                      PresetPath 两个键）。你在 ReShade 面板里调过参数的场合用这个，
#                      否则默认会用本包的 ReShade.ini 覆盖（ReShade 面板设置会重置）
#    -Force            游戏还在运行时也强行继续（默认拒绝：DLL 被占用时写入必失败）
#    -NoPause          结束时不停顿（脚本调用时用）
# =============================================================================

param(
    [Parameter(Position = 0)][string] $GameDir = '',
    [Parameter(Position = 1)][string] $Proxy   = 'dxgi',
    [switch] $KeepReShadeIni,
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
    Fail "缺 files 目录，部署包不完整（请运行解压后根目录里的 install.bat）"
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

Step "目标：$GameDir  (代理: $Proxy.dll，全部强制覆盖)"

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
        Fail "游戏（或本目录里的进程）还在运行：$names`n先关掉再装 —— DLL 被占用时写入会失败。"
    }
    Warn "有进程在运行（$names），-Force 已指定，继续（写入可能失败）"
}

# ---------------------------------------------------------------- 强制覆盖的核心
$script:Report = New-Object System.Collections.ArrayList

# 找一个还没被占用的备份名：x.mgpu_bak / x.mgpu_bak2 … x.mgpu_bak9
function Get-FreeBackup([string]$dst) {
    for ($i = 1; $i -le 9; $i++) {
        $cand = if ($i -eq 1) { $dst + '.mgpu_bak' } else { $dst + '.mgpu_bak' + $i }
        if (-not (Test-Path -LiteralPath $cand)) { return $cand }
    }
    return ''
}

# 把 $Src 覆盖到 $Dst：内容不同就先备份原文件，再写、再校验。
function Install-One {
    param([string]$Src, [string]$Dst, [string]$Label)
    if (-not (Test-Path -LiteralPath $Src -PathType Leaf)) {
        Fail "缺文件: $Src（部署包不完整，请重新解压）"
    }
    $srcSha = Get-Sha $Src
    $bak = ''
    $state = '新建'

    if (Test-Path -LiteralPath $Dst -PathType Leaf) {
        if ((Get-Sha $Dst) -eq $srcSha) {
            [void]$script:Report.Add([pscustomobject]@{ 文件 = $Label; 结果 = '已是最新'; 备份 = '' })
            Info "$Label 内容已与本包一致（不重写）"
            return
        }
        $state = '覆盖'
        $bak = Get-FreeBackup $Dst
        if ($bak -ne '') {
            try {
                Copy-Item -LiteralPath $Dst -Destination $bak -Force
            } catch {
                Fail "备份 $Label 失败：$($_.Exception.Message)`n先关掉游戏再装。"
            }
            Info "$Label 旧版本 -> $(Split-Path -Leaf $bak)"
        } else {
            Warn "$Label 的备份名额用尽（*.mgpu_bak1..9 都在），直接覆盖"
        }
    }

    $dir = Split-Path -Parent $Dst
    if ($dir -ne '' -and -not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    try {
        Copy-Item -LiteralPath $Src -Destination $Dst -Force
    } catch {
        Fail "写入 $Label 失败：$($_.Exception.Message)`n（被占用 / 没有写权限 / 被杀软拦了）"
    }
    if ((Get-Sha $Dst) -ne $srcSha) {
        Fail "$Label 写入后校验不一致 —— 可能被安全软件拦截，或磁盘写入失败。"
    }
    # 新建的文件没有旧版本可备份，$bak 是空串 —— 不能直接 Split-Path -Leaf（会抛"Path 为空字符串"）
    $bakName = ''
    if ($bak -ne '') { $bakName = Split-Path -Leaf $bak }
    [void]$script:Report.Add([pscustomobject]@{ 文件 = $Label; 结果 = $state; 备份 = $bakName })
    Info "$Label 已$state"
}

# ---------------------------------------------------------------- 1. 代理 DLL
Step "1/6 ReShade 宿主（$Proxy.dll）"
$reshade = Join-Path $files 'ReShade64.dll'
$reshadeSha = Get-Sha $reshade
Install-One $reshade (Join-Path $GameDir ($Proxy + '.dll')) "$Proxy.dll"

# ---------------------------------------------------------------- 2. add-on
Step "2/6 add-on"
$addonSrc = Join-Path $files 'game_capture.addon64'
Install-One $addonSrc (Join-Path $GameDir 'game_capture.addon64') 'game_capture.addon64'

# ---------------------------------------------------------------- 3. add-on 的 ini
#  key 名和默认值都会随版本变化（例如 StreamHeight=1440 是 v0.12 才有的），
#  留着旧 ini 就会"参数看起来设了但没生效"，所以这里无条件覆盖。
Step "3/6 add-on 配置"
Install-One (Join-Path $files 'game_capture.ini') (Join-Path $GameDir 'game_capture.ini') 'game_capture.ini'

# ---------------------------------------------------------------- 4. 空转效果
Step "4/6 空转效果（让 ReShade 绑定深度）"
$fxDstDir = Join-Path $GameDir 'reshade-shaders\Shaders'
$fxSrc = Join-Path $files 'reshade-shaders\Shaders\mgpu_depth_tap.fx'
$fxDst = Join-Path $fxDstDir 'mgpu_depth_tap.fx'
Install-One $fxSrc $fxDst 'mgpu_depth_tap.fx'

# ---------------------------------------------------------------- 5. 预设 + ReShade.ini
Step "5/6 预设与 ReShade.ini"
Install-One (Join-Path $files 'ReShadePreset.ini') (Join-Path $GameDir 'ReShadePreset.ini') 'ReShadePreset.ini'

$rini = Join-Path $GameDir 'ReShade.ini'
if ($KeepReShadeIni) {
    # 只补两个键：你已经在 ReShade 面板里调过的设置保持不动
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
    if (Test-Path -LiteralPath $rini) {
        $rbak = Get-FreeBackup $rini
        if ($rbak -ne '') { Copy-Item -LiteralPath $rini -Destination $rbak -Force }
        Set-IniKey -Path $rini -Section 'GENERAL' -Key 'EffectSearchPaths' -Value '.\reshade-shaders\Shaders\**'
        Set-IniKey -Path $rini -Section 'GENERAL' -Key 'PresetPath' -Value '.\ReShadePreset.ini'
        [void]$script:Report.Add([pscustomobject]@{ 文件 = 'ReShade.ini'; 结果 = '保留（只补 2 个键）'; 备份 = 'ReShade.ini.mgpu_bak*' })
        Info "ReShade.ini 已保留（-KeepReShadeIni）：只保证 EffectSearchPaths / PresetPath"
    } else {
        Install-One (Join-Path $files 'ReShade.ini') $rini 'ReShade.ini'
    }
} else {
    # 默认：连 ReShade.ini 一起覆盖，确保 EffectSearchPaths / PresetPath / 效果列表
    # 都是本包这份。ReShade 面板里的设置会被重置（原件在 *.mgpu_bak 里）。
    Install-One (Join-Path $files 'ReShade.ini') $rini 'ReShade.ini'
}

# ------------------------------------------------------- 6/6 镜像目录（引擎重定向）
# 有些游戏进程并不从游戏根目录读文件：PRAGMATA（RE Engine）实际从
# `<游戏>\ _storage_\game_capture.addon64` 加载 add-on，于是 ini 也在那个目录里找 ——
# 只装根目录的结果是"装好了、但配置不生效、日志一行都没有"（2026-11-09 实测）。
# 规则：任何子目录里如果已经有同名 add-on 或同名代理 DLL，就把 add-on + ini 也放一份过去。
Step "6/6 镜像目录（引擎重定向）"
$mirrored = 0
foreach ($sub in @(Get-ChildItem -LiteralPath $GameDir -Directory -ErrorAction SilentlyContinue)) {
    $d = $sub.FullName
    $hit = (Test-Path -LiteralPath (Join-Path $d 'game_capture.addon64')) -or
           (Test-Path -LiteralPath (Join-Path $d ($Proxy + '.dll')))
    if (-not $hit) { continue }
    Install-One $addonSrc (Join-Path $d 'game_capture.addon64') ("镜像\{0}\game_capture.addon64" -f $sub.Name)
    Install-One (Join-Path $files 'game_capture.ini') (Join-Path $d 'game_capture.ini') ("镜像\{0}\game_capture.ini" -f $sub.Name)
    Info ("镜像目录：{0}" -f $d)
    $mirrored++
}
if ($mirrored -eq 0) { Info "没有发现需要镜像的目录（正常）" }

# ---------------------------------------------------------------- 残留扫描
Step "残留检查"
$left = New-Object System.Collections.ArrayList
$ours = @("$Proxy.dll", 'game_capture.addon64', 'game_capture.ini', 'ReShade.ini',
          'ReShadePreset.ini', 'mgpu_depth_tap.fx')
foreach ($f in @(Get-ChildItem -LiteralPath $GameDir -File -ErrorAction SilentlyContinue)) {
    if ($ours -contains $f.Name) { continue }
    if ($f.Name -like 'game_capture.*' -or $f.Name -like '*.addon64*' -or
        $f.Name -like 'ReShadePreset.ini.*' -or $f.Name -like 'MGPU_*') {
        [void]$left.Add($f.Name)
    }
}
# 同一个目录里如果有两份本包的 ReShade（换了代理名装过两次），只会有害无益
foreach ($n in $proxyList) {
    if ($n -eq 'version' -or $n -eq $Proxy) { continue }
    $other = Join-Path $GameDir ($n + '.dll')
    if ((Get-Sha $other) -eq $reshadeSha) { [void]$left.Add("$n.dll（也是本包的 ReShade）") }
}
if ($left.Count -gt 0) {
    Warn "发现其它 mgpu/ReShade 相关文件（不影响使用，但清理掉更干净）："
    foreach ($l in $left) { Write-Host "        $l" -ForegroundColor DarkGray }
    Warn "要删就自己删；uninstall.bat 只处理本包装的那几个。"
} else {
    Info "没有发现残留文件"
}

# ---------------------------------------------------------------- 汇总
Step "结果"
$script:Report | Format-Table -AutoSize | Out-String | Write-Host

Write-Host "装好了（全部按本包版本覆盖）。接下来：" -ForegroundColor Green
Write-Host "  1. 启动游戏（第一次 ReShade 会在左上角显示加载信息，按 Home 打开面板）"
Write-Host "  2. 游戏里关掉自带的帧生成，抗锯齿/超分选 DLSS / FSR2+ / XeSS（不要 TSR / FSR1 / TAA）"
Write-Host "  3. 打开 mgpu_ui.exe 选这个游戏窗口按热键开始（或 bridge.exe --game <exe>）"
Write-Host ""
Write-Host ("  日志: {0}\ReShade.log（add-on 自己的日志要 game_capture.ini 里 Log=1）" -f $GameDir)
Write-Host ("  还原: uninstall.bat `"{0}`"" -f $GameDir)
Write-Host ""
if (-not $NoPause) { [void](Read-Host "按回车关闭") }

