# SPDX-FileCopyrightText: 2026 BB-VR contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
# Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
# You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
# keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
# licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
# Only the approved game mixes belong beside the emulator. The dry takes, tools and
# voice design files in assets/voice are authoring inputs.
function Copy-WeaponVoiceRecordings([string]$source, [string]$destination) {
    $keys = @(
        'amygdalan_arm', 'blade_of_mercy', 'bloodletter', 'boom_hammer',
        'burial_blade', 'chikage', 'holy_moonlight_sword', 'kirkhammer',
        'kos_parasite', 'logarius_wheel', 'ludwigs_holy_blade', 'rakuyo',
        'rifle_spear', 'simons_bowblade', 'tonitrus', 'whirligig_saw'
    )
    New-Item -ItemType Directory -Path $destination -Force -ErrorAction Stop | Out-Null
    foreach ($key in $keys) {
        $file = Get-Item -LiteralPath (Join-Path $source "$key.mp3") -ErrorAction Stop
        if ($file.PSIsContainer -or !$file.Length) { throw "Empty or invalid weapon recording: $key.mp3" }
        Copy-Item -LiteralPath $file.FullName -Destination $destination -ErrorAction Stop
        $file
    }
}

function Assert-PackagedWeaponVoices([string]$package, [IO.FileInfo[]]$recordings) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($package)
    try {
        foreach ($file in $recordings) {
            $entry = $zip.GetEntry("lib/app/runtime/voice/$($file.Name)")
            if (!$entry -or $entry.Length -ne $file.Length) { throw "Missing or incomplete packaged voice: $($file.Name)" }
            $stream = $entry.Open()
            try {
                if ((Get-FileHash -InputStream $stream).Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) {
                    throw "Packaged voice differs from approved recording: $($file.Name)"
                }
            } finally { $stream.Dispose() }
        }
    } finally { $zip.Dispose() }
}
