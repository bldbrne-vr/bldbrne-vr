[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$LauncherExe,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$RedistDirectory,
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [string]$OutputDirectory,
    [string]$ReleaseNotes,
    [string]$SignParameters,
    [switch]$AllowDirty
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repo = [IO.Path]::GetFullPath((Join-Path $projectRoot '..'))
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
Copy-Item -LiteralPath $launcher -Destination (Join-Path $payload 'BloodborneVR.exe')
Copy-Item -LiteralPath (Join-Path $runtime 'shadps4.exe') -Destination (Join-Path $payload 'runtime/shadps4.exe')
Get-ChildItem -LiteralPath $runtime -Filter '*.dll' -File | Copy-Item -Destination (Join-Path $payload 'runtime')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'bbvr_patch.xml') -Destination (Join-Path $payload 'runtime/bbvr_patch.xml')
# Upscaler files the runtime loads from its own folder, when the build has them: exactly these,
# never the whole folder (it may hold binaries that must not ship). Their licences must be
# reviewed before a public release.
$upscalers = [ordered]@{
    'dlss' = @('nvngx_dlss.dll','vr_dlss_stereo.dll')
    'fsr4' = @('vr_fsr4_stereo.dll','fsr4_shaders')
}
foreach ($folder in $upscalers.Keys) {
    $source = Join-Path $runtime $folder
    if (!(Test-Path -LiteralPath $source -PathType Container)) { continue }
    $missing = @($upscalers[$folder] | Where-Object { !(Test-Path -LiteralPath (Join-Path $source $_)) })
    if ($missing.Count) { throw "Incomplete $folder folder; missing: $($missing -join ', ')" }
    $target = Join-Path $payload "runtime/$folder"
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    foreach ($item in $upscalers[$folder]) {
        Copy-Item -LiteralPath (Join-Path $source $item) -Destination $target -Recurse
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
    Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $payload
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSES') -Destination $payload -Recurse
$licenses = Join-Path $payload 'LICENSES'
Copy-Item -LiteralPath (Join-Path $projectRoot 'externals/dear_imgui/LICENSE.txt') -Destination (Join-Path $licenses 'Dear-ImGui.txt')
Copy-Item -LiteralPath (Join-Path $projectRoot 'externals/json/LICENSE.MIT') -Destination (Join-Path $licenses 'nlohmann-json.txt')
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
$vpk = Join-Path $toolsDir 'vpk.exe'
if (!(Test-Path -LiteralPath $vpk)) {
    dotnet tool install vpk --tool-path $toolsDir --version 1.2.161
    if ($LASTEXITCODE) { throw 'Could not install the pinned packaging tool.' }
}
$arguments = @('pack','--packId','BloodborneVR','--packVersion',$Version,'--packTitle','BB-VR',
    '--packDir',$payload,'--mainExe','BloodborneVR.exe','--channel','vr-win','--runtime','win-x64',
    '--delta','None','--shortcuts','StartMenuRoot','--outputDir',$output)
if ($ReleaseNotes) { $arguments += @('--releaseNotes',(Resolve-Path -LiteralPath $ReleaseNotes).Path) }
if ($SignParameters) { $arguments += @('--signParams',$SignParameters) }
& $vpk @arguments
if ($LASTEXITCODE) { throw "Packaging failed with exit code $LASTEXITCODE" }
Get-ChildItem -LiteralPath $output -File | Select-Object Name,Length

