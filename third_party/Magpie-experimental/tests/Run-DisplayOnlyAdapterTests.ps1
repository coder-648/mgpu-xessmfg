#Requires -Version 7.0
param(
    [string]$OutputDirectory = (Join-Path $env:TEMP ('Magpie-DisplayOnlyAdapters-' + [guid]::NewGuid())),
    [string]$FmtIncludeDirectory,
    [string]$RapidJsonIncludeDirectory,
    [switch]$NativeProbe
)
$ErrorActionPreference = 'Stop'
$adapterOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $adapterOutput -Force | Out-Null
$adapterVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$adapterVs = & $adapterVswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (!$adapterVs) { throw 'Visual Studio C++ tools not found.' }
Import-Module (Join-Path $adapterVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $adapterVs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

function Find-AdapterTestInclude([string]$specified, [string]$pattern, [string]$header) {
    if ($specified) {
        if (!(Test-Path -LiteralPath (Join-Path $specified $header))) { throw "Header not found: $header" }
        return [IO.Path]::GetFullPath($specified)
    }
    foreach ($adapterCache in @((Join-Path $env:USERPROFILE '.conan2/p'), (Join-Path $env:USERPROFILE '.conan2/p/b'))) {
        if (!(Test-Path -LiteralPath $adapterCache)) { continue }
        foreach ($adapterPackage in (Get-ChildItem -LiteralPath $adapterCache -Directory -Filter $pattern)) {
            $adapterInclude = Join-Path $adapterPackage.FullName 'p/include'
            if (Test-Path -LiteralPath (Join-Path $adapterInclude $header)) { return $adapterInclude }
        }
    }
    throw "Existing dependency not found: $header. Pass its include directory explicitly."
}
$adapterFmt = Find-AdapterTestInclude $FmtIncludeDirectory 'fmt*' 'fmt/format.h'
$adapterRapid = Find-AdapterTestInclude $RapidJsonIncludeDirectory 'rapid*' 'rapidjson/document.h'
& python "$PSScriptRoot/prepare_display_only_adapter_test.py" $adapterOutput
if ($LASTEXITCODE) { throw 'Production extraction failed.' }
$adapterFlags = @('/nologo', '/std:c++20', '/EHsc', '/utf-8', '/MT', '/W4', '/WX', '/DNOMINMAX',
    '/DFMT_HEADER_ONLY', "/I$adapterFmt", "/I$adapterRapid")
& cl.exe @adapterFlags "/I$adapterOutput" "$PSScriptRoot/DisplayOnlyAdapterTests.cpp" "/Fe:$adapterOutput/DisplayOnlyAdapterTests.exe" "/Fo:$adapterOutput/DisplayOnlyAdapterTests.obj"
if ($LASTEXITCODE) { throw 'Adapter regression compilation failed.' }
& "$adapterOutput/DisplayOnlyAdapterTests.exe"
if ($LASTEXITCODE) { throw 'Adapter regression failed.' }
foreach ($adapterMutation in @('accept-display-only', 'leak-query-handle')) {
    $adapterVariant = Join-Path $adapterOutput $adapterMutation
    & cl.exe @adapterFlags "/I$adapterVariant" "/I$adapterOutput" "$PSScriptRoot/DisplayOnlyAdapterTests.cpp" "/Fe:$adapterVariant/check.exe" "/Fo:$adapterVariant/check.obj"
    if ($LASTEXITCODE) { throw "Mutation compilation failed: $adapterMutation" }
    & "$adapterVariant/check.exe"
    if (!$LASTEXITCODE) { throw "Regression was not detected: $adapterMutation" }
    Write-Host "Expected regression detected: $adapterMutation"
}
if ($NativeProbe) {
    & cl.exe @adapterFlags "/I$adapterOutput" "$PSScriptRoot/DisplayOnlyAdapterNativeProbe.cpp" "/Fe:$adapterOutput/NativeProbe.exe" "/Fo:$adapterOutput/NativeProbe.obj" /link Gdi32.lib dxgi.lib
    if ($LASTEXITCODE) { throw 'Native adapter probe compilation/link failed.' }
    & "$adapterOutput/NativeProbe.exe"
    if ($LASTEXITCODE) { throw 'Native adapter probe failed.' }
}
Write-Host "Adapter checks complete. Full Magpie build and Sunshine DLSS NR/FG acceptance remain separate. Output: $adapterOutput"
