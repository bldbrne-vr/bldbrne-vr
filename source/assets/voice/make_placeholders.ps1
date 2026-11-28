# SPDX-FileCopyrightText: 2026 BB-VR contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
# Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
# You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
# keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
# licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
# Robotic placeholder recordings of lines.md, spoken by Windows' own voices: one <key>.wav each
# (22.05 kHz mono, 16-bit), to try the weapon voice before the real recordings exist.
# Keep placeholders outside assets/voice, which holds the approved recordings. Use a test
# build's voice folder (beside its shadps4.exe), or a folder given to the emulator as VR_VOICE_DIR:
#   .\assets\voice\make_placeholders.ps1 -Out <build dir>\voice
param([Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = 'Stop'
if ((Resolve-Path -LiteralPath $PSScriptRoot).Path -eq [IO.Path]::GetFullPath($Out).TrimEnd('\')) {
    throw 'Not into assets/voice: the release would ship the placeholders.'
}
Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$voices = @($synth.GetInstalledVoices() | Where-Object { $_.Enabled } | ForEach-Object { $_.VoiceInfo })
if (!$voices.Count) { throw 'No Windows speech voice is installed.' }
$male = $voices | Where-Object { $_.Gender -eq 'Male' } | Select-Object -First 1
$female = $voices | Where-Object { $_.Gender -eq 'Female' } | Select-Object -First 1
$format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(22050,
    [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
New-Item -ItemType Directory -Path $Out -Force | Out-Null
$count = 0
foreach ($row in Get-Content -LiteralPath (Join-Path $PSScriptRoot 'lines.md') -Encoding UTF8) {
    # The Lines table's rows: | key | voice | line |
    if ($row -notmatch '^\|\s*([a-z_]+)\s*\|\s*([^|]+?)\s*\|\s*(.+?)\s*\|\s*$') { continue }
    $key = $Matches[1]; $voice = $Matches[2]; $text = $Matches[3]
    if ($key -eq 'key') { continue }
    $text = ($text -replace '\s*\[[^\]]*\]\s*', ' ').Trim() # the [audio tags]: ElevenLabs' only
    $who = if ($voice -match 'noble' -and $female) { $female } elseif ($male) { $male } else { $voices[0] }
    $synth.SelectVoice($who.Name)
    $synth.Rate = if ($voice -match 'Beast|assassin|fisherman|scholar') { -2 } elseif ($voice -match 'tinkerer') { 2 } else { 0 }
    $synth.SetOutputToWaveFile((Join-Path $Out "$key.wav"), $format)
    $synth.Speak($text)
    $synth.SetOutputToNull()
    $count++
}
$synth.Dispose()
"$count placeholder lines written to $Out"
