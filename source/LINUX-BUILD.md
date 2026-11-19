# Building Bldbrne-VR on Linux

Community guide (AGPL-3.0-or-later, like the rest of this source tree — keep the copyright
notices and credits, and publish derived works under the same licence).

**Tested on CachyOS (Arch-based) with NVIDIA 620.70.06, SteamVR 2.19.3 and a Valve Index.**
Package names and Steam paths below are Arch/CachyOS-flavoured; other distros need their own
equivalents and are untested.

This is NOT an official build path — upstream releases target Windows.

## Prerequisites

```sh
sudo pacman -S --needed git cmake clang ninja openxr
```

- Clang 20+ (verified 23.1.5), Ninja, CMake 3.28+, git — the pacman line covers all of it
- System `openxr` package (the loader + `openxr_runtime_list` tools)
- SteamVR installed through Steam (it ships the Linux OpenXR runtime)
- Your own Bloodborne 1.09 dump (BYOD — the mod ships none of the game)
- ~15 GB free for externals + build

## 0. Populate externals

The published tree ships empty submodule dirs. After cloning the source tree (with its
submodule pins), populate them:

```sh
git submodule update --init --recursive          # parent tree first
# every external that has its own .gitmodules:
for d in externals/*/; do [ -f "$d/.gitmodules" ] && git -C "$d" submodule update --init --recursive; done
```

Verify by content (the module's key source file exists), not by directory existence.

Known gaps beyond their own submodules:

- `discord-rpc` needs `thirdparty/rapidjson` (covered by its own submodule init).
- `sirit` needs a nested `SPIRV-Headers` clone.
- `fdk-aac`'s nested lib comes from
  `android.googlesource.com/platform/external/aac` at the pinned SHA.

## 1. Configure with Clang

```sh
cmake -B build-clang -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_WITH_SYSTEM_JSONCPP=OFF
```

Notes:

- `BUILD_WITH_SYSTEM_JSONCPP=OFF` is required: a system jsoncpp auto-enables the system
  flavour and leaves a dangling link target that breaks the OpenXR loader's `export(EXPORT)`.
- GCC 16 dies in externals' C++-module scanning (`-Werror=undef` inside the libstdc++
  std.compat header while scanning pugixml); Clang configures clean.

## 2. Build

```sh
ninja -C build-clang shadps4
```

Target = the VR-capable emulator binary (`build-clang/shadps4`). The GUI launcher target is
Windows-only — on Linux drive the binary directly (`--help` lists everything).

## 3. Register SteamVR as your OpenXR runtime

```sh
<SteamVR>/bin/linux64/vrpathreg setthis
openxr_runtime_list          # should list: SteamVR/OpenXR : lighthouse, XR_KHR_vulkan_enable
```

The manifest lands in `~/.config/openxr/<version>/active_runtime.json`; the loader recurses
the registry, so no `XR_RUNTIME_JSON` is needed. SteamVR boots lazily — `vrserver` may not
exist until a client connects.

## 4. Smoke test (no game)

```sh
build-clang/shadps4 --benchmark-headset=/tmp/headset.json
```

Writes the connected headset's per-eye dimensions as JSON and exits. Valve Index native =
`[2468,2468] x [2740,2740]`. This alone proves binary → OpenXR → SteamVR → headset.

## 5. Play

```sh
build-clang/shadps4 --openxr /path/to/your/Bloodborne-1.09-dump
```

Run it outside a limited cgroup (the OpenXR frame loop lives past your shell if launched via
`systemd-run --user -p MemoryMax=infinity ...`).

## SteamVR stability notes (NVIDIA-observed)

If SteamVR's compositor dies at a VR transition (check `journalctl -k` for
`RenderThread segfault ... in libnvidia-glcore` — the game itself exits cleanly), trim GL
surfaces one at a time in SteamVR's `steamvr.settings`, relaunching between each:

```ini
[camera]
enableCamera = false
enableCameraForRoomView = false

[mirrorview]
mirrorviewfullscreen = false
```

Validated together on the box this guide was written on: the cutscene-to-first-person
takeover holds with both sections present.

## Toolchain family notes (what you may patch after updates)

- MSVC type identity: on glibc `int64_t` = `long`, NOT `unsigned long long` (they coincide on
  Windows). `std::min/std::max` over mixed `u64`/`ull` fails deduction — use explicit
  template arguments (`std::min<std::int64_t>(...)`, `std::max<u64>(...)`).
- Non-Windows `#else` halves never compile upstream, so typos surface only on Linux —
  correct the stub to the real parameter names.
- X11/X.h atom macros (None, Always, Bool, True, False) collide with engine enum members —
  clear them once per translation unit (see the `#undef` block in `openxr_presenter.h`).
- Iterate one error per rebuild; they surface one file at a time until the link.

First build ~35 min; updates are incremental passes (~260 objects, minutes).

## Packaged release (this fork)

The fork publishes a ready-made Linux portable matching this guide — binary + the
`bbvr.sh` launcher (dep-check / OpenXR registration / smoke test / play) + instructions:
see this fork's **Releases** for `Bldbrne-VR-<version>-linux-portable.tar.gz`. It is built
from this tree by `linux-portable/make-package.sh <version> <binary>` (the script refuses
to pack when game-data markers are present and verifies the packaged binary's sha256).
