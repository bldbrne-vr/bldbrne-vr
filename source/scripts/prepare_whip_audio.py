# SPDX-FileCopyrightText: 2026 BB-VR contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
# Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
# You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
# keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
# licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
"""Prepare the CC0 Beast Cutter sounds; see src/resources/vr/CHAIN_AUDIO_LICENSE.txt.
Usage: python scripts/prepare_whip_audio.py --sources <download-directory>
Requires ffmpeg on PATH. Does not download anything. Output: embedded S16LE mono, 24 kHz.
"""
import argparse, array, hashlib, json, math, subprocess, sys, wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RATE = 24000

def prepare(source, start, end, rms_db, loop):
    raw = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(source), '-af',
        f'atrim=start={start}:end={end},asetpts=PTS-STARTPTS,highpass=f=80,lowpass=f=9500' +
        (',acompressor=threshold=0.08:ratio=4:attack=2:release=40:makeup=1' if loop else ''),
        '-ac', '1', '-ar', str(RATE), '-f', 's16le', '-'])
    a = array.array('h', raw)
    if sys.byteorder != 'little': a.byteswap()
    x = [v / 32768 for v in a]
    mean = sum(x) / len(x)
    x = [v - mean for v in x]
    if loop:
        # 15 ms overlap across the seam. Runtime envelopes handle the start/stop.
        n = int(.015 * RATE)
        for i in range(n):
            t = i / (n - 1)
            x[-n + i] = x[-n + i] * (1 - t) + x[i] * t
        x = x[n:]
    else:
        for i in range(int(.002 * RATE)): x[i] *= i / (.002 * RATE - 1)
        n = int(.030 * RATE)
        for i in range(n): x[-n+i] *= (1 - i / (n-1)) ** 2
    rms = math.sqrt(sum(v*v for v in x) / len(x))
    gain = min(10 ** (rms_db/20) / rms, 10 ** (-3/20) / max(abs(v) for v in x))
    y = array.array('h', (round(v * gain * 32767) for v in x))
    if sys.byteorder != 'little': y.byteswap()
    pcm = y.tobytes()
    # Oversample to check inter-sample peaks, not just PCM sample peaks.
    over = subprocess.check_output(['ffmpeg','-v','error','-f','s16le','-ar',str(RATE),
        '-ac','1','-i','pipe:0','-ar','192000','-f','f32le','pipe:1'],input=pcm)
    z = array.array('f',over)
    if sys.byteorder != 'little': z.byteswap()
    true_peak = max(abs(v) for v in z)
    assert true_peak < 10 ** (-2/20), true_peak
    return pcm, dict(seconds=len(x)/RATE, rms_db=20*math.log10(rms*gain),
        sample_peak_db=20*math.log10(max(abs(v) for v in x)*gain),
        true_peak_db=20*math.log10(true_peak), loop=loop,
        source_start_s=start, source_end_s=end, source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        pcm_sha256=hashlib.sha256(pcm).hexdigest())

if __name__ == '__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--sources',type=Path,required=True)
    args=parser.parse_args()
    dest=ROOT/'src/resources/vr'; dest.mkdir(exist_ok=True)
    results={}
    for name,source,start,end,rms,loop in [
        ('clink','chain_clink_source.wav',.275,.405,-17,False)]:
        pcm,info=prepare(args.sources/source,start,end,rms,loop)
        (dest/('beast_chain_'+name+'.pcm')).write_bytes(pcm)
        with wave.open(str(args.sources/('beast_chain_'+name+'.wav')),'wb') as w:
            w.setparams((1,2,RATE,0,'NONE','not compressed'));w.writeframes(pcm)
        results[name]=info
    (args.sources/'normalization.json').write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps(results,indent=2))
