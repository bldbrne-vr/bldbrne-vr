# SPDX-FileCopyrightText: 2026 BB-VR contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
# Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
# You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
# keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
# licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
[CmdletBinding()]
param([Parameter(Mandatory)][string]$SaveHome,
      [Parameter(Mandatory)][string]$TestRuntime,
      [Parameter(Mandatory)][string]$OutputDirectory,
      [string]$RuntimeSource=(Join-Path $PSScriptRoot 'benchmark_runtime/mockxr.cpp'),
      [string[]]$GameVersions=@('01.00'),
      [ValidateSet('timed','presentation-v1')][string]$StartupNavigation='timed')
$ErrorActionPreference='Stop'
$homeSource=(Resolve-Path -LiteralPath $SaveHome).Path
$runtimeBinary=(Resolve-Path -LiteralPath $TestRuntime).Path
$runtimeSourceFile=(Resolve-Path -LiteralPath $RuntimeSource).Path
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $output){throw 'Use a new benchmark package directory.'}
$save=Join-Path $homeSource '1000/savedata/CUSA00207/SPRJ0005'
if(!(Test-Path -LiteralPath (Join-Path $save 'userdata0000'))){throw 'Expected the validated Yharnam fixture, not a live user profile.'}
foreach($root in @($homeSource,$save)) {
 if((Get-Item -LiteralPath $root).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Fixture root must not be redirected.'}
}
$items=@(Get-ChildItem -LiteralPath $save -Recurse -File)
if(!$items.Count){throw 'Empty fixture.'}
foreach($item in (Get-ChildItem -LiteralPath $save -Recurse)) {
 if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Fixture must not contain junctions or symlinks.'}
}
# A file named userdata0000 is not evidence of the fixed benchmark scene.
$trusted=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'benchmark_fixture.json') -Raw | ConvertFrom-Json
if($items.Count -ne $trusted.files.Count){throw 'Save is not the validated benchmark fixture (file count differs).'}
foreach($version in $GameVersions) {
 if($version -notin $trusted.game_versions){throw 'This fixture has not been validated for that game version.'}
}
foreach($item in $items) {
 $relative=$item.FullName.Substring($homeSource.Length).TrimStart('\','/').Replace('\','/')
 $expected=@($trusted.files | Where-Object { $_.path -ceq $relative })
 if($expected.Count -ne 1 -or (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -cne $expected[0].sha256) {
  throw "Save is not the validated benchmark fixture: $relative"
 }
}
New-Item -ItemType Directory -Path $output | Out-Null
$files=@()
foreach($item in $items) {
 $relative=$item.FullName.Substring($homeSource.Length).TrimStart('\','/').Replace('\','/')
 $target=Join-Path (Join-Path $output 'home') $relative
 New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
 Copy-Item -LiteralPath $item.FullName -Destination $target
 $files+=@{path=$relative;sha256=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()}
}
Copy-Item -LiteralPath $runtimeBinary -Destination (Join-Path $output 'benchmark_xr.dll')
@{file_format_version='1.0.0';runtime=@{name='Bldbrne-VR benchmark';library_path='./benchmark_xr.dll'}} |
 ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'runtime.json') -Encoding utf8
@{schema=1;startup_navigation=$StartupNavigation;scene='Central Yharnam fixed-view fixture';game_versions=$GameVersions;files=$files;
 runtime_source_sha256=(Get-FileHash -LiteralPath $runtimeSourceFile -Algorithm SHA256).Hash.ToLowerInvariant();
 runtime_sha256=(Get-FileHash -LiteralPath (Join-Path $output 'benchmark_xr.dll')).Hash.ToLowerInvariant();
 scope='Static rendering estimate; fixed Quest-like FOV; no compositor or motion-quality certification'} |
 ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'manifest.json') -Encoding utf8
Write-Output "Created isolated benchmark package: $output"
