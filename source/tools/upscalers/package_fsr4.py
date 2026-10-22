"""Validate and package the pinned FSR runtime next to a playable executable.
Stage assets with fetch_temporal_dependencies.py --provider fsr4 and prepare_fsr4_assets.py.
No downloads or GPU work are performed here; incomplete packages fail before copying.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from fetch_temporal_dependencies import FSR, fsr_files

HERE = Path(__file__).resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validated_files(assets, bridge):
    manifest = json.loads((assets / "manifest.json").read_text())
    if (manifest["repository"], manifest["commit"]) != FSR:
        raise ValueError("Unexpected FSR asset source revision")
    required = {name for _, name in fsr_files({})}
    entries = {entry["path"]: entry for entry in manifest["files"]}
    if set(entries) != required or len(entries) != len(manifest["files"]):
        raise ValueError("Incomplete or duplicate FSR asset manifest")
    files = {}
    for name in sorted(required):
        source = assets / name
        data = source.read_bytes()
        if len(data) != entries[name]["bytes"] or digest(source) != entries[name]["sha256"]:
            raise ValueError(f"FSR asset hash/size mismatch: {name}")
        if name.endswith(".spv") and data[:4] != bytes.fromhex("03022307"):
            raise ValueError(f"Invalid SPIR-V: {name}")
        files["fsr4_shaders/" + name] = source
    guards = json.loads((assets / "opt/pass11-guard.json").read_text())
    expected = {name for name in required if name.endswith("_pass11.spv")}
    fixed = {entry["name"]: entry for entry in guards["files"]}
    if guards["recipe"] != "bbport-pass11-guard-v1" or set(fixed) != expected or len(fixed) != len(guards["files"]):
        raise ValueError("Incomplete guarded pass-11 shaders")
    if guards["rewrite_sha256"] != digest(HERE / "prepare_fsr4_assets.py"):
        raise ValueError("Guarded shaders were made with a different recipe; regenerate them")
    for name in sorted(expected):
        source = assets / "opt" / name
        if fixed[name]["source_sha256"] != digest(assets / name) or fixed[name]["fixed_sha256"] != digest(source):
            raise ValueError(f"Guarded shader hash mismatch: {name}")
        files["fsr4_shaders/opt/" + name] = source
    # Optional (FAST bit 37): bbport's shared-memory post passes, plus the fenced SPD
    # auto-exposure pass that bit 37 also requests. Packaged only when complete and made by
    # the current recipe; without them the original passes run.
    post_manifest = assets / "opt/post-lds.json"
    if post_manifest.is_file():
        posts = json.loads(post_manifest.read_text())
        expected_posts = {name for name in required if name.endswith("_post.spv")}
        rewritten = {entry["name"]: entry for entry in posts["files"]}
        if posts["recipe"] != "bbport-post-lds-v2-spirv" or set(rewritten) != expected_posts or \
                len(rewritten) != len(posts["files"]) or \
                posts.get("spd", {}).get("name") != "spd_auto_exposure.spv":
            raise ValueError("Incomplete optimized post passes; rerun prepare_fsr4_post_lds.py")
        if posts["rewrite_sha256"] != digest(HERE / "prepare_fsr4_post_lds.py"):
            raise ValueError("Optimized post passes were made with a different recipe; regenerate them")
        rewritten[posts["spd"]["name"]] = posts["spd"]
        for name in sorted(expected_posts) + [posts["spd"]["name"]]:
            source = assets / "opt" / name
            if rewritten[name]["source_sha256"] != digest(assets / name) or \
                    rewritten[name]["fixed_sha256"] != digest(source) or \
                    source.read_bytes()[:4] != bytes.fromhex("03022307"):
                raise ValueError(f"Optimized post pass hash mismatch: {name}")
            files["fsr4_shaders/opt/" + name] = source
        files["fsr4_shaders/opt/post-lds.json"] = post_manifest
    else:
        print("FSR4 optimized post passes not prepared (prepare_fsr4_post_lds.py); "
              "FAST bit 37 keeps the original post pass")
    if bridge.read_bytes()[:2] != b"MZ":
        raise ValueError("Invalid FSR bridge DLL")
    files["vr_fsr4_stereo.dll"] = bridge
    files["fsr4_shaders/manifest.json"] = assets / "manifest.json"
    files["fsr4_shaders/opt/pass11-guard.json"] = assets / "opt/pass11-guard.json"
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=HERE / "local/dependencies/fsr4_shaders")
    parser.add_argument("--bridge", type=Path, default=HERE / "local/fsr4-bridge-build/vr_fsr4_stereo.dll")
    parser.add_argument("--destination", type=Path, default=HERE / "local/runtimes/fsr4")
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    files = validated_files(args.assets.resolve(), args.bridge.resolve())
    if args.check_only:
        print(f"FSR4_PACKAGE_VALID: {len(files)} verified files")
        return
    # Never update a runtime while an emulator could be using it.
    subprocess.run(["powershell", "-NoProfile", "-Command",
        "if (Get-Process shadps4* -ErrorAction SilentlyContinue) { exit 1 }"],
        check=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    destination = args.destination.resolve()
    # Optional files of an earlier package must not outlive their manifest: the host enables
    # bit 37's post pass when opt/post-lds.json exists, and its fenced SPD when
    # opt/spd_auto_exposure.spv exists too. The manifest goes first and comes back last.
    sources = {source.resolve() for source in files.values()}
    opt = destination / "fsr4_shaders/opt"
    manifest_name = "fsr4_shaders/opt/post-lds.json"
    for stale in [opt / "post-lds.json", opt / "spd_auto_exposure.spv", *opt.glob("*_post.spv")]:
        name = "fsr4_shaders/opt/" + stale.name
        if stale.resolve() not in sources and (name not in files or name == manifest_name):
            stale.unlink(missing_ok=True)
    if manifest_name in files:
        files[manifest_name] = files.pop(manifest_name)
    hashes = {}
    for name, source in files.items():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.resolve() != target.resolve():
            shutil.copy2(source, target)
        hashes[name] = digest(target)
        if hashes[name] != digest(source):
            raise ValueError(f"Package copy mismatch: {name}")
    (destination / "package.json").write_text(json.dumps({
        "asset_repository": FSR[0], "asset_commit": FSR[1], "files": hashes
    }, indent=2) + "\n")
    print(f"FSR4_PACKAGE_OK: {len(files)} verified files -> {destination}")


if __name__ == "__main__":
    main()
