"""Original magic-denial cue. Offline synthesis; no game or voice recordings.

24 kHz mono S16LE, 720 ms / 34,560 bytes. A bassy, brooding falling resonance,
with a 100 ms soft onset and no impact/noise transient. No runtime synthesis or decoding. Optional WAV
preview has exactly the runtime gain (before the game's master volume).
"""
import argparse
import math
from pathlib import Path
import struct
import wave


def generate():
    rate, count = 24000, 17280
    phase = upper = 0.0
    samples = []
    for i in range(count):
        t = i / rate
        u = i / (count - 1)
        phase += 2 * math.pi * (108 - 34 * u) / rate
        upper += 2 * math.pi * (153 - 42 * u) / rate
        tone = (math.sin(phase) + .24 * math.sin(phase * 2.01) +
                .22 * math.sin(upper) + .07 * math.sin(upper * 2))
        attack = min(t / .10, 1)
        attack = attack * attack * (3 - 2 * attack)
        tail = min((1 - u) / .28, 1)
        tail = tail * tail * (3 - 2 * tail)
        env = attack * math.exp(-t / .30) * tail
        samples.append(tone * env)
    peak = max(abs(s) for s in samples)
    return [round(s / peak * .48 * 32767) for s in samples]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preview", type=Path)
    args = parser.parse_args()
    pcm = generate()
    target = Path(__file__).resolve().parents[1] / "src/resources/vr/tool_empty.pcm"
    target.write_bytes(struct.pack("<" + "h" * len(pcm), *pcm))
    if args.preview:
        args.preview.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(args.preview), "wb") as wav:
            wav.setparams((1, 2, 24000, len(pcm), "NONE", "not compressed"))
            wav.writeframes(struct.pack("<" + "h" * len(pcm), *[round(s * .65) for s in pcm]))
    print(f"{target}: {target.stat().st_size} bytes, 720 ms")
