#Requires -Version 7.0
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) { $OutputDirectory = Join-Path (Split-Path $repo -Parent) '.tools/069-beta1-fix1/tests/layout' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 "/I$repo/src/Magpie.Core/include" `
    (Join-Path $PSScriptRoot 'Beta1Fix1LayoutTests.cpp') "/Fe:$OutputDirectory/Beta1Fix1LayoutTests.exe" "/Fo:$OutputDirectory/Beta1Fix1LayoutTests.obj"
if ($LASTEXITCODE) { throw 'Popup layout test compilation failed.' }
& (Join-Path $OutputDirectory 'Beta1Fix1LayoutTests.exe')
if ($LASTEXITCODE) { throw 'Popup layout tests failed.' }
& python (Join-Path $PSScriptRoot 'check_beta1_fix1_metadata.py')
if ($LASTEXITCODE) { throw 'Beta1 fix1 metadata checks failed.' }
