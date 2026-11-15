#Requires -Version 7.0
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path ([IO.Path]::GetTempPath()) ('Magpie-shortcut-clear-' + [guid]::NewGuid())
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
if (!$vs) { throw 'Install Visual Studio with the C++ desktop workload.' }
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$rapid = Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE '.conan2/p') -Directory -Filter 'rapid*' |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'p/include/rapidjson/document.h') } | Select-Object -First 1
if (!$rapid) { throw 'Restore rapidjson before running shortcut tests.' }
& python (Join-Path $PSScriptRoot 'prepare_shortcut_clear_test.py') $OutputDirectory
if ($LASTEXITCODE) { throw 'Shortcut fixture extraction/static checks failed.' }
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /DNOMINMAX /DUNICODE /D_UNICODE `
    "/I$repo/src/Magpie" "/I$OutputDirectory" "/I$($rapid.FullName)/p/include" `
    (Join-Path $PSScriptRoot 'ShortcutClearTests.cpp') "/Fe:$OutputDirectory/ShortcutClearTests.exe" "/Fo:$OutputDirectory/ShortcutClearTests.obj"
if ($LASTEXITCODE) { throw 'Shortcut test compilation failed.' }
& (Join-Path $OutputDirectory 'ShortcutClearTests.exe') $OutputDirectory
if ($LASTEXITCODE) { throw 'Shortcut clear regression failed.' }
