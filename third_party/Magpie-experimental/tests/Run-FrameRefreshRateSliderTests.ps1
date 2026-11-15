#Requires -Version 7.0
param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
if(!$OutputDirectory) { $OutputDirectory=Join-Path (Split-Path $repo -Parent) '.tools/069-beta2-integer-rates/slider-tests' }
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$imgui=Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE '.conan2/p/b') -Directory -Filter 'imgui*' |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'p/lib/imgui.lib') } | Select-Object -First 1
if(!$imgui) { throw 'Existing ImGui dependency not found.' }
& python (Join-Path $PSScriptRoot 'prepare_frame_refresh_rate_slider_test.py') $OutputDirectory
if($LASTEXITCODE) { throw 'Production rate widget extraction failed.' }
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX "/I$($imgui.FullName)/p/include" "/I$repo/src/Magpie.Core/include" `
    "$OutputDirectory/FrameRefreshRateSliderTests.cpp" "$($imgui.FullName)/p/lib/imgui.lib" user32.lib imm32.lib `
    "/Fe:$OutputDirectory/FrameRefreshRateSliderTests.exe" "/Fo:$OutputDirectory/FrameRefreshRateSliderTests.obj"
if($LASTEXITCODE) { throw 'Rate widget test compilation failed.' }
& "$OutputDirectory/FrameRefreshRateSliderTests.exe"
if($LASTEXITCODE) { throw 'Production rate widget regression failed.' }
