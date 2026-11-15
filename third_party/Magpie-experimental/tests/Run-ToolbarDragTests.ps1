#Requires -Version 7.0
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path ([IO.Path]::GetTempPath()) ('Magpie-toolbar-drag-' + [guid]::NewGuid())
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
if (!$vs) { throw 'Install Visual Studio with the C++ desktop workload.' }
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$imgui = Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE '.conan2/p/b') -Directory -Filter 'imgui*' |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'p/lib/imgui.lib') } | Select-Object -First 1
$rapid = Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE '.conan2/p') -Directory -Filter 'rapid*' |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'p/include/rapidjson/document.h') } | Select-Object -First 1
if (!$imgui -or !$rapid) { throw 'Restore imgui and rapidjson before running toolbar tests.' }
& python (Join-Path $PSScriptRoot 'prepare_toolbar_drag_test.py') $OutputDirectory
if ($LASTEXITCODE) { throw 'Toolbar production extraction failed.' }
Push-Location $OutputDirectory
try {
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /DNOMINMAX /DUNICODE /D_UNICODE `
        "/I$repo/src/Magpie.Core/include" "/I$OutputDirectory" "/I$($imgui.FullName)/p/include" "/I$($rapid.FullName)/p/include" `
        (Join-Path $PSScriptRoot 'ToolbarDragTests.cpp') "$($imgui.FullName)/p/lib/imgui.lib" user32.lib imm32.lib `
        "/Fe:$OutputDirectory/ToolbarDragTests.exe" "/Fo:$OutputDirectory/ToolbarDragTests.obj"
    if ($LASTEXITCODE) { throw 'Toolbar test compilation failed.' }
    & (Join-Path $OutputDirectory 'ToolbarDragTests.exe')
    if ($LASTEXITCODE) { throw 'Toolbar drag regression failed.' }
    foreach ($test in @('parameter_input', 'overlay_window_layout')) {
        & python (Join-Path $repo "scripts/tests/test_$test.py") $OutputDirectory
        if ($LASTEXITCODE) { throw "Extract $test failed" }
        & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /DNOMINMAX `
            "/I$repo/src/Magpie.Core/include" "/I$($imgui.FullName)/p/include" "$OutputDirectory/$test.cpp" `
            "$($imgui.FullName)/p/lib/imgui.lib" user32.lib imm32.lib "/Fe:$OutputDirectory/$test.exe" "/Fo:$OutputDirectory/$test.obj"
        if ($LASTEXITCODE) { throw "Compile $test failed" }
        & "$OutputDirectory/$test.exe"
        if ($LASTEXITCODE) { throw "$test failed" }
    }
    & python (Join-Path $repo 'scripts/tests/test_profile_parameter_focus.py') $OutputDirectory
    if ($LASTEXITCODE) { throw 'Extract profile focus settings failed.' }
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 "/I$($rapid.FullName)/p/include" `
        "$OutputDirectory/parameter_focus_settings.cpp" "/Fe:$OutputDirectory/profile_focus_settings.exe" "/Fo:$OutputDirectory/profile_focus_settings.obj"
    if ($LASTEXITCODE) { throw 'Compile profile focus settings failed.' }
    & "$OutputDirectory/profile_focus_settings.exe"
    if ($LASTEXITCODE) { throw 'Profile focus settings failed.' }
} finally {
    Pop-Location
}
Write-Output "Toolbar regression artifacts: $OutputDirectory"
