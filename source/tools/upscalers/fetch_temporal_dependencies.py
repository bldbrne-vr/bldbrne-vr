"""Fetch pinned DLSS SDK/runtime and FSR 4 assets into ignored local storage.
Does not load DLLs, change the emulator build, or deploy beside a running executable.
"""
from pathlib import Path, PurePosixPath
import argparse
import concurrent.futures
import hashlib
import json
import urllib.request

ROOT = Path(__file__).resolve().parent / "local" / "dependencies"
DLSS = ("NVIDIA/DLSS", "374959484e79a640feaba44c93ac8cfb0a03f5b5")
FSR = ("FireBurn/Q2RTX", "ae8d628fae208813172446d1e49ed94150b04658")


def read(url):
    request = urllib.request.Request(url, headers={"User-Agent": "bloodborne-vr-temporal"})
    with urllib.request.urlopen(request, timeout=90) as response:
        return response.read()


def tree(repo, revision):
    result = json.loads(read(f"https://api.github.com/repos/{repo}/git/trees/{revision}?recursive=1"))
    if result.get("truncated"):
        raise RuntimeError("Truncated dependency tree")
    return {e["path"]: e for e in result["tree"] if e["type"] == "blob"}


def blob(data):
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()


def fetch_set(name, source, paths):
    repo, revision = source
    entries = tree(repo, revision)
    selected = paths(entries)
    base = ROOT / name
    def fetch(pair):
        upstream, relative = pair
        path = PurePosixPath(relative)
        if path.is_absolute() or ".." in path.parts or "\\" in relative:
            raise RuntimeError("Invalid dependency path")
        entry = entries[upstream]
        if entry.get("mode") != "100644":
            raise RuntimeError(f"Expected regular file: {upstream}")
        output = base.joinpath(*path.parts)
        data = output.read_bytes() if output.exists() else read(
            f"https://raw.githubusercontent.com/{repo}/{revision}/{upstream}")
        if blob(data) != entry["sha"]:
            raise RuntimeError(f"Hash mismatch; refusing replacement: {output}")
        if relative.endswith(".spv") and data[:4] != b"\x03\x02\x23\x07":
            raise RuntimeError(f"Not SPIR-V: {upstream}")
        if relative.endswith(".dll") and data[:2] != b"MZ":
            raise RuntimeError(f"Not a PE binary: {upstream}")
        if not output.exists():
            output.parent.mkdir(parents=True, exist_ok=True)
            temporary = output.with_name(output.name + ".part")
            temporary.write_bytes(data)
            temporary.replace(output)
        return {"path": relative, "upstream_path": upstream, "git_blob": entry["sha"],
                "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}
    print(f"Fetching/checking {name}: {len(selected)} files", flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        files = list(pool.map(fetch, selected))
    (base / "manifest.json").write_text(json.dumps(
        {"repository": repo, "commit": revision, "files": files}, indent=2) + "\n")
    print(f"Verified {name}: {len(files)} files, {sum(f['bytes'] for f in files)/1048576:.1f} MiB", flush=True)


def dlss_files(entries):
    required = ["LICENSE.txt", "lib/Windows_x86_64/x64/nvsdk_ngx_s.lib",
                "lib/Windows_x86_64/rel/nvngx_dlss.dll"]
    required += sorted(p for p in entries if p.startswith("include/"))
    return [(p, p) for p in required]


def fsr_files(entries):
    names = ["LICENSE-FSR4-v07.txt", "rcas.spv", "spd_auto_exposure.spv"]
    # Native AA is retained for testing. 4320 is needed for tall per-eye outputs (>2160).
    for model in ("native", "performance", "balanced", "quality"):
        prefix = f"fsr4_model_v07_i8_{model}_"
        names += [prefix + suffix for suffix in
                  ("initializers.bin", "pre_weights.bin", "shader_manifest.json")]
        for tier in ("1080", "2160", "4320"):
            names += [prefix + tier + "_" + suffix + ".spv" for suffix in
                      ["pre", "post"] + [f"pass{i}" for i in range(1, 13)]]
    return [("baseq2/fsr4_shaders/" + name, name) for name in names]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--provider", choices=("all", "dlss", "fsr4"), default="all")
    args = parser.parse_args()
    if args.provider in ("all", "dlss"):
        fetch_set("dlss-sdk", DLSS, dlss_files)
    if args.provider in ("all", "fsr4"):
        fetch_set("fsr4_shaders", FSR, fsr_files)
    print("Dependencies staged only; no DLL loaded or emulator changed.")


if __name__ == "__main__":
    main()
