# SPDX-FileCopyrightText: 2026 BB-VR contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
# Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
# You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
# keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
# licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
"""A touch of reverb on the weapon voice lines (2026-11-23: "these voices need a touch of
reverb"). Each dry recording in dry/<key>.mp3 (what make_recordings.py writes) becomes <key>.mp3 in
this folder, the file the game plays: the same stone-hall room on every line, mixed in quietly
after the voice, mono (the game mixes voice lines centred). Free to rerun: no credits, and the
approved takes stay exactly as recorded.

The room: 20 ms pre-delay, six early reflections, then a damped tail (RT60 --rt60 s, dark above
~4.5 kHz, no boom below ~180 Hz), convolved with the voice and mixed in at --wet: 0.17, about
-15 dB under the voice (the tester: "A bit less reverb please"; 0.25 and 1.4 s were first).
Needs ffmpeg (with libmp3lame) on PATH or --ffmpeg, and numpy.
usage: python -I add_reverb.py [--only key,key] [--wet 0.17] [--rt60 1.2]
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
RATE = 44100


def room(rt60, seed=7):
    """The impulse response (mono, unit energy): early reflections, then the damped tail."""
    rng = np.random.default_rng(seed)
    n = int(RATE * (rt60 * 1.6 + 0.1))
    ir = np.zeros(n)
    for ms, gain in ((23, 0.5), (31, -0.42), (43, 0.35), (59, -0.3), (71, 0.24), (89, -0.2)):
        ir[int(RATE * ms / 1000)] += gain
    start = int(RATE * 0.025)
    t = np.arange(n - start) / RATE
    ir[start:] += rng.standard_normal(n - start) * np.exp(-6.91 * t / rt60) * 0.35
    ir = np.concatenate([np.zeros(int(RATE * 0.02)), ir])  # pre-delay
    # Dark stone: off above ~4.5 kHz, and no boom below ~180 Hz.
    spectrum = np.fft.rfft(ir)
    f = np.fft.rfftfreq(len(ir), 1 / RATE)
    spectrum *= 1 / np.sqrt(1 + (f / 4500.0) ** 4)
    spectrum *= (f / 180.0) ** 2 / np.sqrt(1 + (f / 180.0) ** 4)
    ir = np.fft.irfft(spectrum, len(ir))
    return ir / np.sqrt(np.sum(ir ** 2))


def decode(ffmpeg, path):
    raw = subprocess.run([ffmpeg, '-v', 'error', '-i', str(path), '-f', 'f32le', '-ac', '1',
                          '-ar', str(RATE), '-'], capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def encode(ffmpeg, samples, path):
    subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'f32le', '-ar', str(RATE), '-ac', '1',
                    '-i', '-', '-c:a', 'libmp3lame', '-b:a', '96k', '-id3v2_version', '0',
                    str(path)], input=samples.astype(np.float32).tobytes(), check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--only', default='')
    parser.add_argument('--wet', type=float, default=0.17)
    parser.add_argument('--rt60', type=float, default=1.2)
    parser.add_argument('--ffmpeg', default=shutil.which('ffmpeg'))
    args = parser.parse_args()
    if not args.ffmpeg:
        sys.exit('ffmpeg not found: give --ffmpeg.')
    only = {k.strip() for k in args.only.split(',') if k.strip()}
    ir = room(args.rt60)
    for dry_file in sorted((HERE / 'dry').glob('*.mp3')):
        if only and dry_file.stem not in only:
            continue
        dry = decode(args.ffmpeg, dry_file)
        size = 1 << int(np.ceil(np.log2(len(dry) + len(ir))))
        wet = np.fft.irfft(np.fft.rfft(dry, size) * np.fft.rfft(ir, size), size)
        out = np.concatenate([dry, np.zeros(len(ir))]) + args.wet * wet[:len(dry) + len(ir)]
        # The tail ends where it falls under -60 dB; peaks kept under -1 dBFS.
        loud = np.nonzero(np.abs(out) > 1e-3)[0]
        out = out[:loud[-1] + 1] if len(loud) else out
        peak = np.max(np.abs(out))
        if peak > 0.891:
            out *= 0.891 / peak
        encode(args.ffmpeg, out, HERE / f'{dry_file.stem}.mp3')
        print(f'{dry_file.stem}: {len(dry) / RATE:.1f} s -> {len(out) / RATE:.1f} s')


if __name__ == '__main__':
    main()
