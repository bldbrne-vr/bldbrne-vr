Bldbrne-VR — Linux portable (community build)

WHAT THIS IS
  A community Linux build of the Night of the Hunt VR mod's shadPS4 VR binary, plus a
  launcher script. Upstream ships Windows installers; this package is the Linux equivalent
  of the 'Portable (zip)'. The launcher (Bldbrne-VR.exe GUI) is Windows-only — on Linux this
  package drives the emulator binary directly via the bbvr.sh menu.

  Like everywhere: NO game files are included. You need your own Bloodborne 1.09 dump
  (tested with CUSA03173/CUSA00900 contents). The dump stays personal-use-only — do not
  redistribute game data.

TESTED ON
  CachyOS (Arch-based), NVIDIA 620.70.06, SteamVR 2.19.3, Valve Index.
  Other distros are untested; report what happens. The binary needs a modern glibc plus
  libudev.so.1 and libuuid.so.1 (option 1 in the launcher checks for you).

REQUIREMENTS
  - SteamVR installed and at least launched once (it provides the OpenXR runtime)
  - An OpenXR-compatible headset (Valve Index verified; Quest via Link/SteamVR untested)
  - Your Bloodborne 1.09 game dump folder (contains eboot.bin)
  - Optional (nicer): the distro 'openxr' package for the runtime verification tool

INSTALL / RUN
  1. Extract this archive anywhere (e.g. ~/apps/bldbrne-vr).
  2. ./bbvr.sh  →  1) Check dependencies.
  3. ./bbvr.sh  →  2) Set up SteamVR's OpenXR runtime (one time per system). This also
     applies the recommended SteamVR trims (frunk camera + desktop mirror view off):
     known NVIDIA compositor crash triggers. Reversible by deleting the two sections in
     ~/.local/share/Steam/config/steamvr.settings.
  4. ./bbvr.sh  →  3) Headset smoke test. A JSON with your headset's per-eye dimensions
     (Index native = [2468,2468] x [2740,2740]) proves the whole chain works.
  5. ./bbvr.sh  →  4) Play in VR. The first run asks for the game folder and remembers it
     in bbvr.conf.

  Or shortcut the menu: ./bbvr.sh --vr /path/to/dump

CONTROLS / SETTINGS
  Same as the Windows build — motion controls (swing-to-attack, trigger-aim, lean/duck
  dodges, two-handing), foveated DLSS/FSR per eye, Simplified Rendering for weaker GPUs.
  Config and saves live in ~/.local/share/shadps4/ (VR saves are separate from flat saves).

TROUBLESHOOTING
  - SteamVR compositor dies at the cutscene-to-first-person transition (NVIDIA-observed):
    option 2 installs the standard trims automatically; if it still happens, check the
    sections exist in ~/.local/share/Steam/config/steamvr.settings and that SteamVR was
    fully restarted afterwards.
  - Frozen SteamVR after a crash: kill vrserver vrcompositor vrmonitor vrdashboard
    vrwebhelper vrstartup by exact name (pkill -x <name>), then relaunch SteamVR.
  - 'vrserver' missing right after setup: SteamVR boots lazily; the runtime appears when a
    client connects.

LICENCE / SOURCE
  This build is of the AGPL-3.0-or-later source at
  https://github.com/Dr-Bongjoose/bldbrne-vr (tag v0.9.61-linux.1 — the same tag you
  downloaded this release from), which forks Bldbrne-VR by pkalogiros and contributors.
  Upstream: https://github.com/bldbrne-vr/bldbrne-vr (Windows releases).
  Binary patches and the build recipe: source/LINUX-BUILD.md in the same repository.
