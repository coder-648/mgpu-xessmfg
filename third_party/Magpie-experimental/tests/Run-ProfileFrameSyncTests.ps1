#Requires -Version 7.0
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) { $OutputDirectory = Join-Path (Split-Path $repo -Parent) '.tools/069-beta1-fix2/tests/profile-sync' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$rapid = Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE '.conan2/p') -Directory -Filter 'rapid*' |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'p/include/rapidjson/document.h') } | Select-Object -First 1
if (!$rapid) { throw 'Restore rapidjson before running profile frame-sync tests.' }
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 "/I$repo/src/Magpie" "/I$repo/src/Magpie.Core/include" "/I$($rapid.FullName)/p/include" `
    (Join-Path $PSScriptRoot 'ProfileFrameSyncTests.cpp') "/Fe:$OutputDirectory/ProfileFrameSyncTests.exe" "/Fo:$OutputDirectory/ProfileFrameSyncTests.obj"
if ($LASTEXITCODE) { throw 'Profile frame-sync compilation failed.' }
& (Join-Path $OutputDirectory 'ProfileFrameSyncTests.exe')
if ($LASTEXITCODE) { throw 'Profile frame-sync regression failed.' }
