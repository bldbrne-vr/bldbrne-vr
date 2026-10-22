# SPDX-FileCopyrightText: 2026 BB-VR contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
# Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
# You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
# keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
# licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$LauncherExe,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$RedistDirectory,
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [string]$OutputDirectory,
    [string]$ReleaseNotes,
    [string]$BenchmarkDirectory,
    [string]$SignParameters,
    [string]$VpkTool,
    [string]$PackId = 'Bldbrne-VR',
    [string]$Channel = 'win',
    [string]$PackTitle = 'Bldbrne-VR',
    [switch]$AllowDirty
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$launcher = (Resolve-Path -LiteralPath $LauncherExe).Path
$runtime = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$redist = (Resolve-Path -LiteralPath $RedistDirectory).Path
if (!(Test-Path -LiteralPath (Join-Path $runtime 'shadps4.exe') -PathType Leaf)) {
    throw 'RuntimeDirectory must contain shadps4.exe.'
}
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repo "Build/launcher-release/$Version" }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw "Output directory already exists: $output. Use a new directory." }
$dirty = @(git -C $repo status --porcelain)
if ($dirty.Count -and !$AllowDirty) { throw 'The checkout has local changes. Commit the release sources or use -AllowDirty for a local test package.' }
$work = Join-Path $repo ("Build/launcher-pack-" + [guid]::NewGuid().ToString('N'))
$payload = Join-Path $work 'payload'
New-Item -ItemType Directory -Path $payload -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $payload 'runtime') -Force | Out-Null
Copy-Item -LiteralPath $launcher -Destination (Join-Path $payload 'Bldbrne-VR.exe')
Copy-Item -LiteralPath (Join-Path $runtime 'shadps4.exe') -Destination (Join-Path $payload 'runtime/shadps4.exe')
Get-ChildItem -LiteralPath $runtime -Filter '*.dll' -File | Copy-Item -Destination (Join-Path $payload 'runtime')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'bbvr_patch.xml') -Destination (Join-Path $payload 'runtime/bbvr_patch.xml')
# Optional benchmark payload: only manifest-listed saves and the verified test runtime.
if ($BenchmarkDirectory) {
    . (Join-Path $PSScriptRoot 'benchmark_package.ps1')
    Copy-BenchmarkPackage -SourceDirectory $BenchmarkDirectory -DestinationDirectory (Join-Path $payload 'benchmark')
}
# Upscaler files the runtime loads from its own folder, when the build has them: exactly these,
# never the whole folder (it may hold binaries that must not ship). DLSS ships from 0.2.9 (its
# licences: NOTICE.md); FSR 4 includes its verified model/shader assets.
$upscalers = [ordered]@{
    'dlss' = @('nvngx_dlss.dll','vr_dlss_stereo.dll')
    'fsr4' = @('vr_fsr4_stereo.dll','fsr4_shaders','package.json')
    'fsr31' = @('vr_fsr31_stereo.dll','package.json','LICENSE-bridge.txt','LICENSE-port.txt','LICENSE-AMD.txt','NOTICE-port.md','UPSTREAM.md')
}
foreach ($folder in $upscalers.Keys) {
    $source = Join-Path $runtime $folder
    if (!(Test-Path -LiteralPath $source -PathType Container)) { continue }
    $missing = @($upscalers[$folder] | Where-Object { !(Test-Path -LiteralPath (Join-Path $source $_)) })
    if ($missing.Count) { throw "Incomplete $folder folder; missing: $($missing -join ', ')" }
    if ($folder -eq 'fsr4') {
        & python (Join-Path $repo 'tools/upscalers/package_fsr4.py') `
            --assets (Join-Path $source 'fsr4_shaders') `
            --bridge (Join-Path $source 'vr_fsr4_stereo.dll') --check-only
        if ($LASTEXITCODE) { throw 'FSR 4 assets failed verification; rebuild its runtime.' }
    }
    if ($folder -eq 'fsr31') {
        & python (Join-Path $repo 'tools/upscalers/package_fsr31.py') --check-only $source
        if ($LASTEXITCODE) { throw 'FSR 3.1.5 package verification failed.' }
    }
    $target = Join-Path $payload "runtime/$folder"
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    foreach ($item in $upscalers[$folder]) {
        Copy-Item -LiteralPath (Join-Path $source $item) -Destination $target -Recurse
    }
}
# DLSS: only NVIDIA's own signed runtime, and only Super Resolution: a bridge exporting the DLSS 5
# entry point would make the launcher offer DLSS 5, which is not released.
$dlssPayload = Join-Path $payload 'runtime/dlss'
if (Test-Path -LiteralPath $dlssPayload) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $dlssPayload 'nvngx_dlss.dll')
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'CN=NVIDIA Corporation') {
        throw 'nvngx_dlss.dll must be the original NVIDIA-signed runtime.'
    }
    if (Select-String -LiteralPath (Join-Path $dlssPayload 'vr_dlss_stereo.dll') -Pattern 'VrDlssSetNeuralConfig' -SimpleMatch -Quiet) {
        throw 'The DLSS bridge exports DLSS 5 (VrDlssSetNeuralConfig): package the Super Resolution bridge.'
    }
    if (Test-Path -LiteralPath (Join-Path $runtime 'dlss/nvngx_dlssnr.dll')) {
        Write-Warning 'nvngx_dlssnr.dll (DLSS 5) is in the source folder; it is not packaged.'
    }
    if (!(Test-Path -LiteralPath (Join-Path $runtime 'dlss/LICENSE-NVIDIA.txt'))) {
        throw 'The DLSS folder needs LICENSE-NVIDIA.txt (the NVIDIA RTX SDKs licence) to ship nvngx_dlss.dll.'
    }
}
$crt = @('msvcp140.dll','msvcp140_1.dll','msvcp140_2.dll','msvcp140_atomic_wait.dll',
    'msvcp140_codecvt_ids.dll','vcruntime140.dll','vcruntime140_1.dll')
foreach ($file in $crt) {
    $source = Join-Path $redist $file
    if (!(Test-Path -LiteralPath $source)) { throw "Missing redistributable: $source" }
    Copy-Item -LiteralPath $source -Destination $payload
    Copy-Item -LiteralPath $source -Destination (Join-Path $payload 'runtime')
}
foreach ($file in @('LICENSE','NOTICE.md','CREDITS.md')) {
    Copy-Item -LiteralPath (Join-Path $repo $file) -Destination $payload
}
Copy-Item -LiteralPath (Join-Path $repo 'LICENSES') -Destination $payload -Recurse
$licenses = Join-Path $payload 'LICENSES'
if ($BenchmarkDirectory) {
    Copy-Item -LiteralPath (Join-Path $repo 'externals/zlib-ng/LICENSE.md') -Destination (Join-Path $licenses 'Benchmark-zlib-ng.txt')
    Copy-Item -LiteralPath (Join-Path $repo 'externals/openxr/LICENSE') -Destination (Join-Path $licenses 'Benchmark-OpenXR.txt')
}
Copy-Item -LiteralPath (Join-Path $repo 'externals/dear_imgui/LICENSE.txt') -Destination (Join-Path $licenses 'Dear-ImGui.txt')
Copy-Item -LiteralPath (Join-Path $repo 'externals/json/LICENSE.MIT') -Destination (Join-Path $licenses 'nlohmann-json.txt')
Copy-Item -LiteralPath (Join-Path $repo 'externals/pugixml/LICENSE.md') -Destination (Join-Path $licenses 'pugixml.txt')
if (Test-Path -LiteralPath $dlssPayload) {
    Copy-Item -LiteralPath (Join-Path $runtime 'dlss/LICENSE-NVIDIA.txt') -Destination (Join-Path $licenses 'NVIDIA-RTX-SDKs-DLSS.txt')
    Copy-Item -LiteralPath (Join-Path $repo 'tools/upscalers/adapted/dlss_bridge/LICENSE.txt') -Destination (Join-Path $licenses 'DLSS-bridge-MIT.txt')
}
if (Test-Path -LiteralPath (Join-Path $payload 'runtime/fsr4')) {
    $provider = Join-Path $repo 'tools/upscalers/upstream/fsr-vulkan'
    Copy-Item -LiteralPath (Join-Path $provider 'LICENSE.txt') -Destination (Join-Path $licenses 'FSR-Vulkan-MIT.txt')
    Copy-Item -LiteralPath (Join-Path $provider 'NOTICE.md') -Destination (Join-Path $licenses 'FSR-Vulkan-NOTICE.md')
    Copy-Item -LiteralPath (Join-Path $runtime 'fsr4/fsr4_shaders/LICENSE-FSR4-v07.txt') -Destination (Join-Path $licenses 'FSR4-v07.txt')
}
$licenseText = (Invoke-WebRequest -UseBasicParsing 'https://raw.githubusercontent.com/velopack/velopack/1.2.161/LICENSE').Content
[IO.File]::WriteAllText((Join-Path $licenses 'Velopack.txt'), $licenseText)
$versionFile = Join-Path $work 'version.txt'
$probe = Start-Process -FilePath $launcher -ArgumentList @('--version-file', ('"' + $versionFile + '"')) -PassThru -Wait -WindowStyle Hidden
if ($probe.ExitCode -ne 0 -or [IO.File]::ReadAllText($versionFile) -ne $Version) {
    throw 'The native executable version must match -Version. Rebuild with -DBBVR_VERSION=<version>.'
}
$manifest = [ordered]@{
    version = $Version
    source_commit = (git -C $repo rev-parse HEAD)
    source_dirty = [bool]$dirty.Count
    input_runtime_sha256 = (Get-FileHash -LiteralPath (Join-Path $runtime 'shadps4.exe') -Algorithm SHA256).Hash
    input_launcher_sha256 = (Get-FileHash -LiteralPath $launcher -Algorithm SHA256).Hash
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $payload 'build-info.json') -Encoding UTF8
$toolsDir = Join-Path $repo 'Build/launcher-tools'
$vpk = if ($VpkTool) { (Resolve-Path -LiteralPath $VpkTool).Path } else { Join-Path $toolsDir 'vpk.exe' }
if (!(Test-Path -LiteralPath $vpk)) {
    dotnet tool install vpk --tool-path $toolsDir --version 1.2.161
    if ($LASTEXITCODE) { throw 'Could not install the pinned packaging tool.' }
}
$arguments = @('pack','--packId',$PackId,'--packVersion',$Version,'--packTitle',$PackTitle,
    '--packDir',$payload,'--mainExe','Bldbrne-VR.exe','--channel',$Channel,'--runtime','win-x64',
    '--delta','None','--shortcuts','StartMenuRoot','--outputDir',$output,
    '--icon',(Join-Path $repo 'launcher/assets/app-icon.ico'))
if ($ReleaseNotes) { $arguments += @('--releaseNotes',(Resolve-Path -LiteralPath $ReleaseNotes).Path) }
if ($SignParameters) { $arguments += @('--signParams',$SignParameters) }
& $vpk @arguments
if ($LASTEXITCODE) { throw "Packaging failed with exit code $LASTEXITCODE" }
Remove-Item -LiteralPath $work -Recurse -Force # the staging copy (it used to stay behind)
Get-ChildItem -LiteralPath $output -File | Select-Object Name,Length

