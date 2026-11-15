#Requires -Version 7.0
param([string]$OutputDirectory = (Join-Path $env:TEMP ('Magpie-CursorRefresh-' + [guid]::NewGuid())))
$ErrorActionPreference = 'Stop'
$cursorRepo = Split-Path $PSScriptRoot -Parent
$cursorOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $cursorOutput -Force | Out-Null
$cursorVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$cursorVs = & $cursorVswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $cursorVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $cursorVs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$cursorRapid = Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE '.conan2/p') -Directory -Filter 'rapid*' |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'p/include/rapidjson/document.h') } | Select-Object -First 1
if (!$cursorRapid) { throw 'Existing rapidjson dependency not found.' }
& python "$PSScriptRoot/prepare_cursor_refresh_test.py" $cursorOutput
if ($LASTEXITCODE) { throw 'Production fixture/binding checks failed.' }
foreach ($cursorTest in @('CursorRefreshTests','CursorConfigTests','CursorInputSamplingTests')) {
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX /DUNICODE /D_UNICODE "/I$cursorOutput" "/I$cursorRepo/src/Magpie.Core/include" "/I$cursorRepo/src/Magpie" "/I$($cursorRapid.FullName)/p/include" `
        "$PSScriptRoot/$cursorTest.cpp" "/Fe:$cursorOutput/$cursorTest.exe" "/Fo:$cursorOutput/$cursorTest.obj"
    if ($LASTEXITCODE) { throw "Isolated test compilation failed: $cursorTest" }
    & "$cursorOutput/$cursorTest.exe"
    if ($LASTEXITCODE) { throw "Test failed: $cursorTest" }
}
Write-Host "Cursor policy/config checks complete. These do not establish native UI or actual FG/GPU behavior. Output: $cursorOutput"
