# SPDX-FileCopyrightText: 2026 BB-VR contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
# Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
# You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
# keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
# licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
# One release of the launcher and the game, ready to upload to a GitHub Release of
# bldbrne-vr/bldbrne-vr (the launcher's update feed: releases/latest/download).
#   .\launcher\make_release.ps1 -Version 0.2.8 -Runtime <shadps4.exe> [-ReleaseNotes notes.md]
# Then: a new GitHub Release tagged v<version>, not a draft or pre-release, with every file of
# Build/launcher-release/<version> attached. Installed launchers offer it on their next start.
#
# Launchers up to 0.2.6 were installed as BloodborneVR (update channel vr-win). They update to the
# bridge (0.2.7, built once with -Bridge), which moves them to the Bldbrne-VR installer. Every
# release carries the bridge's update feed (BridgeFeed) so they still find it at latest/download.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [Parameter(Mandatory)][string]$Runtime,
    [string]$ReleaseNotes,
    [string]$BenchmarkDirectory,
    # Also build the bridge at this version and keep its feed in BridgeFeed (0.2.7 only).
    [switch]$Bridge,
    [string]$BridgeFeed,
    # DLSS Super Resolution's folder (default: the verified one in temporal-staging); -NoDlss leaves it out.
    [string]$Dlss,
    [switch]$NoDlss,
    # Verified FSR 4 runtime; built with tools/upscalers/build_fsr4.cmd.
    [string]$Fsr4,
    [switch]$NoFsr4,
    [string]$Fsr31
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runtimeExe = (Resolve-Path -LiteralPath $Runtime).Path
if (!$BridgeFeed) { $BridgeFeed = Join-Path $repo 'Build/launcher-bridge-feed' }
$BridgeFeed = [IO.Path]::GetFullPath($BridgeFeed)
if (!$Bridge -and !(Test-Path -LiteralPath (Join-Path $BridgeFeed 'releases.vr-win.json'))) {
    throw "No bridge feed in $BridgeFeed. Download releases.vr-win.json, RELEASES-vr-win and the BloodborneVR-*-vr-win-full.nupkg from the bridge's GitHub Release (v0.2.7) into it."
}
$work = Join-Path $repo "Build/launcher-stage-$Version"
if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }

# The launcher, stamped with the version (package.ps1 checks it).
function Build-Launcher([string]$dir, [bool]$bridge) {
    $flag = if ($bridge) { 'ON' } else { 'OFF' }
    cmake -S (Join-Path $repo 'launcher') -B $dir -A x64 "-DBBVR_VERSION=$Version" "-DBBVR_BRIDGE=$flag" | Out-Host
    if ($LASTEXITCODE) { throw 'Launcher configure failed.' }
    cmake --build $dir --config Release | Out-Host
    if ($LASTEXITCODE) { throw 'Launcher build failed.' }
    # The tests run where the source tree has them (the public source does not).
    $tests = Join-Path $dir 'Release/launcher_tests.exe'
    if (Test-Path -LiteralPath $tests) {
        & $tests | Out-Host
        if ($LASTEXITCODE) { throw 'Launcher tests failed.' }
    }
    $benchmarkTests = Join-Path $dir 'Release/benchmark_tests.exe'
    if (Test-Path -LiteralPath $benchmarkTests) {
        & $benchmarkTests $repo | Out-Host
        if ($LASTEXITCODE) { throw 'Benchmark tests failed.' }
    }
    $packageTests = Join-Path $PSScriptRoot 'benchmark_package_tests.ps1'
    if (Test-Path -LiteralPath $packageTests) {
        & $packageTests | Out-Host
        if (!$?) { throw 'Benchmark packaging tests failed.' }
    }

    Join-Path $dir 'Release/Bldbrne-VR.exe'
}
$launcher = Build-Launcher (Join-Path $work 'launcher') $false

# The runtime folder: shadps4.exe and DLSS Super Resolution (NVIDIA's signed nvngx_dlss.dll, our
# MIT bridge, NVIDIA's licence; package.ps1 checks them). FSR 4 is staged below; DLSS 5 is not included.
$runtimeDir = Join-Path $work 'runtime'
New-Item -ItemType Directory -Path $runtimeDir -Force | Out-Null
Copy-Item -LiteralPath $runtimeExe -Destination $runtimeDir
if (!$NoDlss) {
    if (!$Dlss) { $Dlss = Join-Path $repo 'tools/upscalers/local/runtimes/dlss' }
    $dlssTarget = Join-Path $runtimeDir 'dlss'
    New-Item -ItemType Directory -Path $dlssTarget -Force | Out-Null
    foreach ($file in @('nvngx_dlss.dll', 'vr_dlss_stereo.dll', 'LICENSE-NVIDIA.txt')) {
        Copy-Item -LiteralPath (Join-Path $Dlss $file) -Destination $dlssTarget
    }
}

# Validate the pinned assets and guarded shaders before staging any FSR runtime.
if (!$NoFsr4) {
    if (!$Fsr4) { $Fsr4 = Join-Path $repo 'tools/upscalers/local/runtimes/fsr4' }
    & python (Join-Path $repo 'tools/upscalers/package_fsr4.py') `
        --assets (Join-Path $Fsr4 'fsr4_shaders') `
        --bridge (Join-Path $Fsr4 'vr_fsr4_stereo.dll') `
        --destination (Join-Path $runtimeDir 'fsr4')
    if ($LASTEXITCODE) { throw 'FSR 4 staging failed. Build its runtime first, or use -NoFsr4.' }
}

# Optional verified FSR 3.1.5 package; never collect unrelated build files.
if ($Fsr31) {
    & python (Join-Path $repo 'tools/upscalers/package_fsr31.py') --check-only $Fsr31
    if ($LASTEXITCODE) { throw 'FSR 3.1.5 package verification failed.' }
    $target = Join-Path $runtimeDir 'fsr31'
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    foreach ($file in @('vr_fsr31_stereo.dll','package.json','LICENSE-bridge.txt','LICENSE-port.txt','LICENSE-AMD.txt','NOTICE-port.md','UPSTREAM.md')) {
        Copy-Item -LiteralPath (Join-Path $Fsr31 $file) -Destination $target
    }
}

# The Visual C++ runtime of the newest installed Visual Studio.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -property installationPath
$redist = Get-ChildItem -Path (Join-Path $vs 'VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT') -Directory |
    Sort-Object FullName | Select-Object -Last 1
if (!$redist) { throw 'No Visual C++ redistributable found.' }

$output = Join-Path $repo "Build/launcher-release/$Version"
$package = @{
    LauncherExe = $launcher
    RuntimeDirectory = $runtimeDir
    RedistDirectory = $redist.FullName
    Version = $Version
    OutputDirectory = $output
}
if ($BenchmarkDirectory) { $package.BenchmarkDirectory = (Resolve-Path -LiteralPath $BenchmarkDirectory).Path }
if ($ReleaseNotes) { $package.ReleaseNotes = (Resolve-Path -LiteralPath $ReleaseNotes).Path }
& (Join-Path $PSScriptRoot 'package.ps1') @package

if ($Bridge) {
    # The same runtime under the old id and channel. Its Start menu entry is named apart from the
    # new installer's while both exist. Only its update feed is published, never its installer.
    $bridgePackage = $package.Clone()
    $bridgePackage.LauncherExe = Build-Launcher (Join-Path $work 'launcher-bridge') $true
    $bridgePackage.OutputDirectory = Join-Path $repo "Build/launcher-release/$Version-bridge"
    $bridgePackage.PackId = 'BloodborneVR'
    $bridgePackage.Channel = 'vr-win'
    $bridgePackage.PackTitle = 'Bldbrne-VR (old installer)'
    & (Join-Path $PSScriptRoot 'package.ps1') @bridgePackage
    if (Test-Path -LiteralPath $BridgeFeed) { Remove-Item -LiteralPath $BridgeFeed -Recurse -Force }
    New-Item -ItemType Directory -Path $BridgeFeed -Force | Out-Null
    foreach ($name in @('releases.vr-win.json', 'RELEASES-vr-win', "BloodborneVR-$Version-vr-win-full.nupkg")) {
        Copy-Item -LiteralPath (Join-Path $bridgePackage.OutputDirectory $name) -Destination $BridgeFeed
    }
}

# The bridge's feed beside the new files: the JSON names exactly the packages that must be there.
$feed = Get-Content -LiteralPath (Join-Path $BridgeFeed 'releases.vr-win.json') -Raw | ConvertFrom-Json
$carried = @('releases.vr-win.json', 'RELEASES-vr-win') + @($feed.Assets | ForEach-Object { $_.FileName })
foreach ($name in $carried) {
    $file = Join-Path $BridgeFeed $name
    if (!(Test-Path -LiteralPath $file)) { throw "The bridge feed lacks $name." }
    Copy-Item -LiteralPath $file -Destination $output
}
Get-ChildItem -LiteralPath $output -File | Select-Object Name,Length
Write-Host "Upload every file of $output to a GitHub Release tagged v$Version."
