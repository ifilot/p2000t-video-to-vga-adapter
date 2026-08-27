# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

param(
    [Parameter(Mandatory = $true)] [string] $Stage,
    [Parameter(Mandatory = $true)] [string] $Version,
    [Parameter(Mandatory = $true)] [string] $Output,
    [Parameter(Mandatory = $true)] [string] $WorkDirectory
)

$ErrorActionPreference = "Stop"
$Stage = (Resolve-Path $Stage).Path
$Output = [System.IO.Path]::GetFullPath($Output)
$WorkDirectory = [System.IO.Path]::GetFullPath($WorkDirectory)
$installerScript = Join-Path $PSScriptRoot "windows-installer/setup.nsi"
$assetsDirectory = (Resolve-Path (Join-Path $PSScriptRoot "../assets")).Path
$licenseFile = (Resolve-Path (
    Join-Path $Stage "licenses/GPL-3.0-or-later.txt")).Path
$outputDirectory = Split-Path $Output -Parent

if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "Invalid semantic version: $Version"
}
if ([System.IO.Path]::GetExtension($Output) -ne ".exe") {
    throw "Windows installer output must have an .exe extension: $Output"
}
$viewerExecutable = Join-Path $Stage "p2000t-vid2vga-capture.exe"
if (-not (Test-Path $viewerExecutable -PathType Leaf)) {
    throw "Deployed viewer executable is missing from: $Stage"
}

New-Item -ItemType Directory -Force $WorkDirectory, $outputDirectory |
    Out-Null

$compiler = Get-Command makensis.exe -ErrorAction SilentlyContinue
if (-not $compiler) {
    $compiler = Get-Command makensis -ErrorAction SilentlyContinue
}
if ($compiler) {
    $compilerPath = $compiler.Source
} else {
    $compilerPath = $null
    foreach ($knownCompiler in @(
        (Join-Path ${env:ProgramFiles(x86)} "NSIS/makensis.exe"),
        (Join-Path $env:ProgramFiles "NSIS/makensis.exe")
    )) {
        if (Test-Path $knownCompiler -PathType Leaf) {
            $compilerPath = $knownCompiler
            break
        }
    }
    if (-not $compilerPath) {
        throw "NSIS compiler makensis.exe was not found."
    }
}

& $compilerPath `
    "/DStage=$Stage" `
    "/DAppVersion=$Version" `
    "/DAssetsDirectory=$assetsDirectory" `
    "/DLicenseFile=$licenseFile" `
    "/DInstallerOutput=$Output" `
    $installerScript
if ($LASTEXITCODE -ne 0) {
    throw "makensis failed with exit code $LASTEXITCODE"
}
if (-not (Test-Path $Output -PathType Leaf) -or
    (Get-Item $Output).Length -eq 0) {
    throw "Windows installer was not created: $Output"
}
