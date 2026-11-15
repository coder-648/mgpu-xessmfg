#Requires -Version 7.0
param([Parameter(Mandatory)][string]$BuildRoot)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& python "$PSScriptRoot/check_dlssnr_detail_sources.py" (Resolve-Path -LiteralPath $BuildRoot).Path
if ($LASTEXITCODE) { throw 'DLSSNR detail source syntax checks failed.' }
