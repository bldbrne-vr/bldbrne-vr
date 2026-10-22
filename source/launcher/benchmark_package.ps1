# SPDX-FileCopyrightText: 2026 BB-VR contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
# Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
# You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
# keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
# licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
# Shared by release packaging and local integrity tests. Only explicit fixture files ship.
function Copy-BenchmarkPackage {
    param([Parameter(Mandatory)][string]$SourceDirectory,
          [Parameter(Mandatory)][string]$DestinationDirectory)
    $ErrorActionPreference='Stop'
    $benchmark=(Resolve-Path -LiteralPath $SourceDirectory).Path
    if((Get-Item -LiteralPath $benchmark).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Redirected benchmark package root.'}
    if((Get-Item -LiteralPath (Join-Path $benchmark 'home')).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Redirected benchmark save root.'}
    $manifest=Get-Content -LiteralPath (Join-Path $benchmark 'manifest.json') -Raw | ConvertFrom-Json
    if ($manifest.schema -ne 1 -or !$manifest.files.Count) { throw 'Invalid benchmark manifest.' }
    if($manifest.startup_navigation -and $manifest.startup_navigation -notin @('timed','presentation-v1')) {
        throw 'Unsupported benchmark startup protocol.'
    }
    if(!$manifest.game_versions.Count){throw 'Benchmark fixture has no validated game version.'}
    $dll=Join-Path $benchmark 'benchmark_xr.dll'
    if ((Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant() -ne $manifest.runtime_sha256) { throw 'Benchmark runtime hash mismatch.' }
    $runtimeManifest=Get-Content -LiteralPath (Join-Path $benchmark 'runtime.json') -Raw | ConvertFrom-Json
    if ($runtimeManifest.runtime.library_path -ne './benchmark_xr.dll') { throw 'Unexpected benchmark runtime library.' }
    $target=[IO.Path]::GetFullPath($DestinationDirectory)
    if(Test-Path -LiteralPath $target){throw 'Benchmark package destination already exists.'}
    New-Item -ItemType Directory -Path $target | Out-Null
    foreach($file in $manifest.files) {
        if($file.path -notmatch '^1000/savedata/CUSA[0-9]+/SPRJ0005/' -or $file.path -match '(^|[\\/])\.\.([\\/]|$)|:') { throw 'Invalid benchmark save path.' }
        $source=Join-Path (Join-Path $benchmark 'home') $file.path
        $relative=$file.path.Replace('/',[IO.Path]::DirectorySeparatorChar)
        $walk=Join-Path $benchmark 'home'
        foreach($part in $relative.Split([IO.Path]::DirectorySeparatorChar)) {
            $walk=Join-Path $walk $part
            if((Get-Item -LiteralPath $walk).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Redirected benchmark fixture path.' }
        }
        if((Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant() -ne $file.sha256) { throw 'Benchmark fixture hash mismatch.' }
        $dest=Join-Path (Join-Path $target 'home') $file.path
        New-Item -ItemType Directory -Path (Split-Path $dest) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $dest
    }
    foreach($name in @('manifest.json','runtime.json','benchmark_xr.dll')) {
        Copy-Item -LiteralPath (Join-Path $benchmark $name) -Destination (Join-Path $target $name)
    }
}
