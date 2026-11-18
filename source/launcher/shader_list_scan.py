# SPDX-FileCopyrightText: 2026 BB-VR contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Release check: a shader list holds none of the game's bytes.

Every 32-byte run of the list, at every byte offset, is compared with every 4-byte-aligned
32-byte run of the game's shader archives (decompressed) and of eboot.bin. A match is confirmed
byte for byte. Filler is not counted: runs of fewer than 6 distinct byte values (zeros, repeated
words) and runs of small integers only (every word under 0x10000: counts, indices, binding
numbers; a GCN instruction word never is). Also counts the OrbShdr footer magic.

    python -I launcher/shader_list_scan.py <list folder> <game folder>
    python -I launcher/shader_list_scan.py --self-test <game folder>

Exit 0: no game bytes. The self-test plants 4 KB of a game shader archive in a list at an odd
offset and must find every run of it.
"""
import pathlib
import sys
import tempfile
import zlib

import numpy as np

W = 32
C = (np.arange(1, W // 4 + 1, dtype=np.uint64) * np.uint64(0x9E3779B97F4A7C15)) | np.uint64(1)


def hashes(words: np.ndarray) -> np.ndarray:
    n = len(words) - W // 4 + 1
    if n <= 0:
        return np.zeros(0, np.uint64)
    h = np.zeros(n, np.uint64)
    for i in range(W // 4):
        h += words[i:i + n].astype(np.uint64) * C[i]
        h ^= h >> np.uint64(29)
    return h


def words_at(data: bytes, align: int) -> np.ndarray:
    body = data[align:]
    return np.frombuffer(body[: len(body) // 4 * 4], dtype="<u4")


def dcx_payload(raw: bytes) -> bytes:
    if raw[:4] != b"DCX\0":
        return raw
    at = raw.find(b"DCA\0")
    return zlib.decompressobj().decompress(raw[raw.find(b"\x78", at):])


def filler(run: bytes) -> bool:
    if len(set(run)) < 6:
        return True
    return all(int.from_bytes(run[i:i + 4], "little") < 0x10000 for i in range(0, W, 4))


def game_sources(game: pathlib.Path):
    archives = sorted((game / "dvdroot_ps4" / "shader").glob("*.dcx"))
    if not archives or not (game / "eboot.bin").is_file():
        raise SystemExit(f"{game}: no dvdroot_ps4/shader/*.dcx or eboot.bin")
    for p in archives:
        yield p.name, dcx_payload(p.read_bytes())
    yield "eboot.bin", (game / "eboot.bin").read_bytes()


def scan(folder: pathlib.Path, game: pathlib.Path) -> int:
    files = [p for p in sorted(folder.rglob("*")) if p.is_file()]
    blob = b"".join(p.read_bytes() for p in files)
    print(f"list: {len(files)} files, {len(blob)} bytes, OrbShdr {blob.count(b'OrbShdr')}")
    per_align = [hashes(words_at(blob, a)) for a in range(4)]
    keys = np.concatenate(per_align)
    offs = np.concatenate([a + 4 * np.arange(len(h), dtype=np.int64)
                           for a, h in enumerate(per_align)])
    order = np.argsort(keys, kind="stable")
    keys, offs = keys[order], offs[order]
    total = blob.count(b"OrbShdr")
    for name, data in game_sources(game):
        h = hashes(words_at(data, 0))
        idx = np.minimum(np.searchsorted(keys, h), len(keys) - 1)
        found = 0
        for g in np.nonzero(keys[idx] == h)[0]:
            run = data[4 * g: 4 * g + W]
            if filler(run):
                continue
            lo, hi = np.searchsorted(keys, h[g], "left"), np.searchsorted(keys, h[g], "right")
            for k in range(lo, hi):
                o = int(offs[k])
                if blob[o: o + W] == run:
                    if found < 5:
                        print(f"  {name}+{4 * g:#x} at list byte {o:#x}: {run.hex()}")
                    found += 1
        print(f"{name}: {len(data)} bytes, {found} matching runs")
        total += found
    print("NO GAME BYTES" if total == 0 else f"GAME BYTES FOUND: {total}")
    return 0 if total == 0 else 1


def self_test(game: pathlib.Path) -> int:
    name, data = next(game_sources(game))
    chunk = data[len(data) // 2 // 4 * 4:][:4096]
    with tempfile.TemporaryDirectory() as tmp:
        (pathlib.Path(tmp) / "records.bin").write_bytes(b"xyz" + chunk)
        caught = scan(pathlib.Path(tmp), game) == 1
    print("SELF-TEST OK" if caught else "SELF-TEST FAILED: the planted bytes were not found")
    return 0 if caught else 1


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--self-test":
        sys.exit(self_test(pathlib.Path(sys.argv[2])))
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    sys.exit(scan(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])))
