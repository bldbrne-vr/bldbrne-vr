# SPDX-FileCopyrightText: 2026 BB-VR contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
# Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
# You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
# keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
# licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'benchmark_package.ps1')
$testRoot=Join-Path ([IO.Path]::GetTempPath()) ('bbvr-package-test-'+[guid]::NewGuid().ToString('N'))
$source=Join-Path $testRoot 'source'
$savePath='1000/savedata/CUSA00207/SPRJ0005/userdata0000'
$save=Join-Path (Join-Path $source 'home') $savePath
New-Item -ItemType Directory -Path (Split-Path $save) -Force | Out-Null
Set-Content -LiteralPath $save -Value 'synthetic packaging test'
Set-Content -LiteralPath (Join-Path $source 'benchmark_xr.dll') -Value 'synthetic DLL, never loaded'
Set-Content -LiteralPath (Join-Path $source 'runtime.json') -Value '{"runtime":{"library_path":"./benchmark_xr.dll"}}'
Set-Content -LiteralPath (Join-Path $source 'unlisted.txt') -Value 'must not ship'
$manifest=@{schema=1;game_versions=@('01.00');runtime_sha256=(Get-FileHash (Join-Path $source 'benchmark_xr.dll')).Hash.ToLowerInvariant();files=@(@{path=$savePath;sha256=(Get-FileHash $save).Hash.ToLowerInvariant()})}
function Save-Manifest { $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $source 'manifest.json') }
function Reject([string]$name) {
    $rejected=$false
    try { Copy-BenchmarkPackage $source (Join-Path $testRoot $name) } catch { $rejected=$true }
    if(!$rejected){throw "Expected rejection: $name"}
}
Save-Manifest
Copy-BenchmarkPackage $source (Join-Path $testRoot 'valid')
if((Get-ChildItem (Join-Path $testRoot 'valid') -File -Recurse).Count -ne 4){throw 'Unexpected packaged files.'}
Reject 'valid'
$manifest.startup_navigation='unknown';Save-Manifest;Reject 'unknown-startup'
$manifest.startup_navigation='presentation-v1';Save-Manifest
Copy-BenchmarkPackage $source (Join-Path $testRoot 'presentation')
if((Get-Content (Join-Path $testRoot 'presentation/manifest.json') -Raw | ConvertFrom-Json).startup_navigation -ne 'presentation-v1') {throw 'Startup protocol lost.'}
$manifest.Remove('startup_navigation');Save-Manifest
$manifest.files[0].sha256='bad';Save-Manifest;Reject 'bad-save-hash'
$manifest.files[0].sha256=(Get-FileHash $save).Hash.ToLowerInvariant()
$manifest.files[0].path='1000/savedata/CUSA00207/SPRJ0005/../../outside';Save-Manifest;Reject 'traversal'
$manifest.files[0].path=$savePath
$manifest.runtime_sha256='bad';Save-Manifest;Reject 'bad-runtime-hash'
$manifest.runtime_sha256=(Get-FileHash (Join-Path $source 'benchmark_xr.dll')).Hash.ToLowerInvariant()
$manifest.game_versions=@();Save-Manifest;Reject 'missing-version'
$manifest.game_versions=@('01.00');Save-Manifest
Set-Content -LiteralPath (Join-Path $source 'runtime.json') -Value '{"runtime":{"library_path":"../../other.dll"}}'
Reject 'redirected-runtime'
# The fixture builder must not certify an arbitrary save, even if its filename fits.
$rejected=$false
try {
    & (Join-Path $PSScriptRoot 'make_benchmark_fixture.ps1') -SaveHome (Join-Path $source 'home') `
        -TestRuntime (Join-Path $source 'benchmark_xr.dll') -OutputDirectory (Join-Path $testRoot 'untrusted-fixture') | Out-Null
} catch { $rejected=$_.Exception.Message -like 'Save is not the validated benchmark fixture*' }
if(!$rejected -or (Test-Path -LiteralPath (Join-Path $testRoot 'untrusted-fixture'))) {
    throw 'Fixture builder accepted or staged an untrusted save.'
}
Write-Output "10 benchmark packaging checks passed. Synthetic artifacts: $testRoot"
