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
    # Public release label; the launcher supplies Velopack's sortable package version.
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+(-[0-9A-Za-z]+(\.[0-9A-Za-z]+)*)?$')][string]$Version,
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
$versionFile = Join-Path $work 'version.json'
$probe = Start-Process -FilePath $launcher -ArgumentList @('--release-version-file', ('"' + $versionFile + '"')) -PassThru -Wait -WindowStyle Hidden
if ($probe.ExitCode -ne 0 -or !(Test-Path -LiteralPath $versionFile -PathType Leaf)) {
    throw 'The launcher could not report a package version. Rebuild with a supported -DBBVR_VERSION.'
}
$releaseVersion = [IO.File]::ReadAllText($versionFile) | ConvertFrom-Json
if ($releaseVersion.version -cne $Version -or
    $releaseVersion.package_version -cnotmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z]+(\.[0-9A-Za-z]+)*)?$') {
    throw 'The native executable must report the requested release label and a valid package version. Rebuild with -DBBVR_VERSION=<version>.'
}
New-Item -ItemType Directory -Path (Join-Path $payload 'runtime') -Force | Out-Null
Copy-Item -LiteralPath $launcher -Destination (Join-Path $payload 'Bldbrne-VR.exe')
Copy-Item -LiteralPath (Join-Path $runtime 'shadps4.exe') -Destination (Join-Path $payload 'runtime/shadps4.exe')
Get-ChildItem -LiteralPath $runtime -Filter '*.dll' -File | Copy-Item -Destination (Join-Path $payload 'runtime')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'bbvr_patch.xml') -Destination (Join-Path $payload 'runtime/bbvr_patch.xml')
. (Join-Path $PSScriptRoot 'weapon_voice_package.ps1')
$voiceRecordings = @(Copy-WeaponVoiceRecordings (Join-Path $repo 'assets/voice') (Join-Path $payload 'runtime/voice'))
# The startup shader build's lists, when the runtime has them:
# shader_list (the game's) and shader_list_dlc (The Old Hunters', read only when the add-on is
# installed), each records.bin and keys/<g|c>_0x<hash>.key, nothing else. They hold no game code;
# check a new list with launcher/shader_list_scan.py before it ships.
function Copy-ShaderList([string]$name) {
    $shaderList = Join-Path $runtime $name
    if (!(Test-Path -LiteralPath $shaderList -PathType Container)) { return $null }
    $records = Join-Path $shaderList 'records.bin'
    if (!(Test-Path -LiteralPath $records -PathType Leaf)) { throw "$name has no records.bin." }
    $head = [byte[]]::new(8)
    $stream = [IO.File]::OpenRead($records)
    try { $read = $stream.Read($head, 0, 8) } finally { $stream.Dispose() }
    if ($read -ne 8 -or [Text.Encoding]::ASCII.GetString($head, 4, 4) -ne 'BBSR') {
        throw "$name/records.bin is not a shader list (no BBSR record)."
    }
    $keys = @(Get-ChildItem -LiteralPath (Join-Path $shaderList 'keys') -File -ErrorAction SilentlyContinue)
    $others = @(Get-ChildItem -LiteralPath $shaderList -Recurse -File | Where-Object {
        $_.FullName -ne $records -and !($_.DirectoryName -eq (Join-Path $shaderList 'keys') -and
            $_.Name -cmatch '^[cg]_0x[0-9a-f]{16}\.key$') })
    if ($others.Count) { throw "$name holds files that are not part of a list: $($others.FullName -join ', ')" }
    $size = (Get-ChildItem -LiteralPath $shaderList -Recurse -File | Measure-Object Length -Sum).Sum
    if ($size -gt 64MB) { throw "$name is $([math]::Round($size / 1MB)) MB; a list is a few MB." }
    $target = Join-Path $payload "runtime/$name"
    New-Item -ItemType Directory -Path (Join-Path $target 'keys') -Force | Out-Null
    Copy-Item -LiteralPath $records -Destination $target
    $keys | Copy-Item -Destination (Join-Path $target 'keys')
    return [ordered]@{
        records_sha256 = (Get-FileHash -LiteralPath $records -Algorithm SHA256).Hash
        keys = $keys.Count
    }
}
$shaderListInfo = Copy-ShaderList 'shader_list'
$dlcShaderListInfo = Copy-ShaderList 'shader_list_dlc'
# Sharper skies' upscaler (sky_upscale.h): exactly the files the launcher build copies next to
# itself from Real-ESRGAN's pinned official release (launcher/CMakeLists.txt). The images it makes
# from the player's game exist only on their PC.
$upscaler = Join-Path (Split-Path -Parent $launcher) 'realesrgan'
$upscalerFiles = @('realesrgan-ncnn-vulkan.exe','vcomp140.dll','models/realesrgan-x4plus.param','models/realesrgan-x4plus.bin')
$missingUpscaler = @($upscalerFiles | Where-Object { !(Test-Path -LiteralPath (Join-Path $upscaler $_)) })
if ($missingUpscaler.Count) { throw "The launcher build has no complete realesrgan folder; missing: $($missingUpscaler -join ', ')" }
New-Item -ItemType Directory -Path (Join-Path $payload 'realesrgan/models') -Force | Out-Null
foreach ($file in $upscalerFiles) {
    Copy-Item -LiteralPath (Join-Path $upscaler $file) -Destination (Join-Path $payload "realesrgan/$file")
}
# Optional benchmark payload: only manifest-listed saves and the verified test runtime.
if ($BenchmarkDirectory) {
    . (Join-Path $PSScriptRoot 'benchmark_package.ps1')
    Copy-BenchmarkPackage -SourceDirectory $BenchmarkDirectory -DestinationDirectory (Join-Path $payload 'benchmark')
}
# Upscaler files the runtime loads from its own folder, when the build has them: exactly these,
# never the whole folder (it may hold binaries that must not ship). DLSS ships from 0.2.9 (its
# licences: NOTICE.md); FSR 4 includes its verified model/shader assets. The DLSS bridge carries
# DLSS 5's chain and its forwarder (nvngx.dll_dlssnr.dll, ours, GPL-3.0); NVIDIA's DLSS 5 runtime
# (nvngx_dlssnr.dll) never ships: the player adds their own (the launcher's DLSS 5 tab).
$upscalers = [ordered]@{
    'dlss' = @('nvngx_dlss.dll','vr_dlss_stereo.dll','nvngx.dll_dlssnr.dll')
    'fsr4' = @('vr_fsr4_stereo.dll','fsr4_shaders','package.json')
    'fsr31' = @('vr_fsr31_stereo.dll','package.json','LICENSE-bridge.txt','LICENSE-port.txt','LICENSE-AMD.txt','NOTICE-port.md','UPSTREAM.md')
    # FSR 4.1 (2026-10-31): our bridge with AMD's signed FSR SDK 2.4.0 Vulkan loader and upscaler
    # (MIT-style, LICENSE-AMD-FSR-SDK.md); package_fsr41.py pins every file's SHA-256.
    'fsr41' = @('vr_fsr41_stereo.dll','amd_fidelityfx_loader_vk.dll','amd_fidelityfx_upscaler_vk.dll','LICENSE-AMD-FSR-SDK.md','LICENSE-bridge.txt','package.json')
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
    if ($folder -eq 'fsr41') {
        if (!(Test-Path -LiteralPath (Join-Path $runtime 'fsr31/vr_fsr31_stereo.dll') -PathType Leaf)) {
            throw 'FSR 4.1 requires the adjacent FSR 3.1.5 fallback package.'
        }
        & python (Join-Path $repo 'tools/upscalers/package_fsr41.py') --check-only $source
        if ($LASTEXITCODE) { throw 'FSR 4.1 package verification failed.' }
        foreach ($dll in @('amd_fidelityfx_loader_vk.dll','amd_fidelityfx_upscaler_vk.dll')) {
            $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $source $dll)
            if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'CN=Advanced Micro Devices') {
                throw "$dll must be AMD's original signed file."
            }
        }
    }
    $target = Join-Path $payload "runtime/$folder"
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    foreach ($item in $upscalers[$folder]) {
        Copy-Item -LiteralPath (Join-Path $source $item) -Destination $target -Recurse
    }
}
# DLSS: only NVIDIA's own signed Super Resolution runtime. DLSS 5: our bridge (it must take the
# player's file, VrDlssSetNeuralRuntime) and forwarder, never NVIDIA's DLSS 5 runtime.
$dlssPayload = Join-Path $payload 'runtime/dlss'
if (Test-Path -LiteralPath $dlssPayload) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $dlssPayload 'nvngx_dlss.dll')
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'CN=NVIDIA Corporation') {
        throw 'nvngx_dlss.dll must be the original NVIDIA-signed runtime.'
    }
    if (!(Select-String -LiteralPath (Join-Path $dlssPayload 'vr_dlss_stereo.dll') -Pattern 'VrDlssSetNeuralRuntime' -SimpleMatch -Quiet)) {
        throw 'The DLSS bridge cannot take the player''s DLSS 5 file (no VrDlssSetNeuralRuntime): build it with VR_DLSSNR_CHAIN=ON.'
    }
    if (!(Select-String -LiteralPath (Join-Path $dlssPayload 'vr_dlss_stereo.dll') -Pattern 'VrDlssSetInputReductionPercent' -SimpleMatch -Quiet)) {
        throw 'Rebuild vr_dlss_stereo.dll from this checkout to include dynamic input resolution.'
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
Copy-Item -LiteralPath (Join-Path $repo 'externals/miniz/LICENSE') -Destination (Join-Path $licenses 'miniz.txt')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'assets/controllers/README.md') `
    -Destination (Join-Path $licenses 'Launcher-controller-art.md')
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
    # DLSS 5's chain and forwarder: GPL-3.0 (from OptiScaler-DLSSNR, with RenoDX's attribution).
    $nr = Join-Path $repo 'tools/upscalers/upstream/dlssnr-reference'
    Copy-Item -LiteralPath (Join-Path $nr 'LICENSE') -Destination (Join-Path $licenses 'DLSS5-bridge-GPL-3.0.txt')
    Copy-Item -LiteralPath (Join-Path $nr 'Licenses/RenoDX_ATTRIBUTION.txt') -Destination (Join-Path $licenses 'DLSS5-RenoDX-attribution.txt')
}
# NVIDIA's DLSS 5 runtime is the player's own: never in a package, under any name or folder.
$neural = @(Get-ChildItem -LiteralPath $payload -Recurse -File | Where-Object {
    $_.Name -ieq 'nvngx_dlssnr.dll' -or $_.VersionInfo.ProductName -eq 'NVIDIA DLSSNR' })
if ($neural.Count) { throw "NVIDIA's DLSS 5 runtime must not ship: $($neural.FullName -join ', ')" }
if (Test-Path -LiteralPath (Join-Path $payload 'runtime/fsr4')) {
    $provider = Join-Path $repo 'tools/upscalers/upstream/fsr-vulkan'
    Copy-Item -LiteralPath (Join-Path $provider 'LICENSE.txt') -Destination (Join-Path $licenses 'FSR-Vulkan-MIT.txt')
    Copy-Item -LiteralPath (Join-Path $provider 'NOTICE.md') -Destination (Join-Path $licenses 'FSR-Vulkan-NOTICE.md')
    Copy-Item -LiteralPath (Join-Path $runtime 'fsr4/fsr4_shaders/LICENSE-FSR4-v07.txt') -Destination (Join-Path $licenses 'FSR4-v07.txt')
}
$licenseText = (Invoke-WebRequest -UseBasicParsing 'https://raw.githubusercontent.com/velopack/velopack/1.2.161/LICENSE').Content
[IO.File]::WriteAllText((Join-Path $licenses 'Velopack.txt'), $licenseText)
# The upscaler's licences, at the versions of its release (Real-ESRGAN v0.2.5.0, built from
# Real-ESRGAN-ncnn-vulkan v0.2.0 with ncnn 20220420).
$upscalerLicenses = [ordered]@{
    'Real-ESRGAN-BSD-3-Clause.txt' = 'https://raw.githubusercontent.com/xinntao/Real-ESRGAN/v0.2.5.0/LICENSE'
    'Real-ESRGAN-ncnn-vulkan-MIT.txt' = 'https://raw.githubusercontent.com/xinntao/Real-ESRGAN-ncnn-vulkan/v0.2.0/LICENSE'
    'ncnn-BSD-3-Clause.txt' = 'https://raw.githubusercontent.com/Tencent/ncnn/20220420/LICENSE.txt'
}
foreach ($name in $upscalerLicenses.Keys) {
    $licenseText = (Invoke-WebRequest -UseBasicParsing $upscalerLicenses[$name]).Content
    [IO.File]::WriteAllText((Join-Path $licenses $name), $licenseText)
}
$manifest = [ordered]@{
    version = $Version
    package_version = $releaseVersion.package_version
    source_commit = (git -C $repo rev-parse HEAD)
    source_dirty = [bool]$dirty.Count
    input_runtime_sha256 = (Get-FileHash -LiteralPath (Join-Path $runtime 'shadps4.exe') -Algorithm SHA256).Hash
    input_launcher_sha256 = (Get-FileHash -LiteralPath $launcher -Algorithm SHA256).Hash
}
if ($shaderListInfo) { $manifest.shader_list = $shaderListInfo }
if ($dlcShaderListInfo) { $manifest.shader_list_dlc = $dlcShaderListInfo }
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $payload 'build-info.json') -Encoding UTF8
$toolsDir = Join-Path $repo 'Build/launcher-tools'
$vpk = if ($VpkTool) { (Resolve-Path -LiteralPath $VpkTool).Path } else { Join-Path $toolsDir 'vpk.exe' }
if (!(Test-Path -LiteralPath $vpk)) {
    dotnet tool install vpk --tool-path $toolsDir --version 1.2.161
    if ($LASTEXITCODE) { throw 'Could not install the pinned packaging tool.' }
}
$arguments = @('pack','--packId',$PackId,'--packVersion',$releaseVersion.package_version,'--packTitle',$PackTitle,
    '--packDir',$payload,'--mainExe','Bldbrne-VR.exe','--channel',$Channel,'--runtime','win-x64',
    '--delta','None','--shortcuts','StartMenuRoot','--outputDir',$output,
    '--icon',(Join-Path $repo 'launcher/assets/app-icon.ico'))
if ($ReleaseNotes) { $arguments += @('--releaseNotes',(Resolve-Path -LiteralPath $ReleaseNotes).Path) }
if ($SignParameters) { $arguments += @('--signParams',$SignParameters) }
& $vpk @arguments
if ($LASTEXITCODE) { throw "Packaging failed with exit code $LASTEXITCODE" }
$fullPackage = @(Get-ChildItem -LiteralPath $output -File -Filter '*-full.nupkg')
if ($fullPackage.Count -ne 1) { throw 'Expected one full package to verify the weapon recordings.' }
Assert-PackagedWeaponVoices $fullPackage[0].FullName $voiceRecordings
Remove-Item -LiteralPath $work -Recurse -Force # the staging copy (it used to stay behind)
Get-ChildItem -LiteralPath $output -File | Select-Object Name,Length

