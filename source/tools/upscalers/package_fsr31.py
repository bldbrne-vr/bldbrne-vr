"""Package the pinned, embedded-LDR FSR 3.1.5 bridge; never copy an entire build tree."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

PIN = "c64f093404125e960813f95f1996c14780f0dd69"
FILES = {"vr_fsr31_stereo.dll", "LICENSE-bridge.txt", "LICENSE-port.txt",
         "LICENSE-AMD.txt", "NOTICE-port.md", "UPSTREAM.md"}
ROOT = Path(__file__).resolve().parents[2]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def verify(folder):
    if folder.is_symlink():
        raise ValueError("Redirected FSR package")
    manifest = json.loads((folder / "package.json").read_text())
    if manifest.get("provider") != "FSR 3.1.5" or manifest.get("source_commit") != PIN:
        raise ValueError("Unexpected FSR 3.1.5 provenance")
    if set(manifest["files"]) != FILES:
        raise ValueError("Unexpected FSR package file list")
    for name, expected in manifest["files"].items():
        path = folder / name
        if path.is_symlink() or not path.is_file() or digest(path) != expected:
            raise ValueError("Missing or changed FSR package file: " + name)
    dll = (folder / "vr_fsr31_stereo.dll").read_bytes()
    if not dll.startswith(b"MZ") or b"VrFsr4GetStereoApi" not in dll:
        raise ValueError("FSR bridge export missing")
    return manifest

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path)
    parser.add_argument("--bridge", type=Path)
    parser.add_argument("--destination", type=Path)
    parser.add_argument("--check-only", type=Path)
    args = parser.parse_args()
    if args.check_only:
        verify(args.check_only)
        return
    if not args.source or not args.bridge or not args.destination:
        parser.error("--source, --bridge and --destination are required")
    source = args.source.resolve()
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "-C", str(source), "status", "--porcelain", "--untracked-files=no"], text=True).strip()
    if revision != PIN or dirty:
        raise ValueError("Use the unchanged pinned FSR Vulkan source")
    dest = args.destination
    if dest.exists():
        raise ValueError("Use a fresh package destination")
    header = source / "upstream/ffx-2.3.0/Kits/FidelityFX/upscalers/fsr3/include/ffx_fsr3upscaler.h"
    amd_notice = header.read_text().split("#pragma once", 1)[0]
    if "Permission is hereby granted" not in amd_notice or "THE SOFTWARE" not in amd_notice:
        raise ValueError("AMD license notice missing")
    dest.mkdir(parents=True)
    shutil.copy2(args.bridge, dest / "vr_fsr31_stereo.dll")
    shutil.copy2(ROOT / "LICENSE", dest / "LICENSE-bridge.txt")
    shutil.copy2(source / "LICENSE.txt", dest / "LICENSE-port.txt")
    shutil.copy2(source / "NOTICE.md", dest / "NOTICE-port.md")
    shutil.copy2(source / "UPSTREAM.md", dest / "UPSTREAM.md")
    (dest / "LICENSE-AMD.txt").write_text(amd_notice + "\n" +
        (source / "upstream/ffx-1.1.4/sdk/LICENSE.txt").read_text())
    adapter = ROOT / "tools/upscalers/adapted/fsr31_bridge"
    manifest = {"schema": 1, "provider": "FSR 3.1.5", "source_commit": PIN,
                "frame_generation": False, "shader_variant": "LDR", "constant_buffer_reuse": True,
                "adapter_files": {p.name: digest(p) for p in sorted(adapter.iterdir()) if p.is_file()},
                "files": {name: digest(dest / name) for name in sorted(FILES)}}
    (dest / "package.json").write_text(json.dumps(manifest, indent=2) + "\n")
    verify(dest)

if __name__ == "__main__":
    main()
