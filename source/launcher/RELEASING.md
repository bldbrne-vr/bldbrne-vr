# Releasing Bldbrne-VR (public builds and updates)

Public repository: https://github.com/bldbrne-vr/bldbrne-vr. Every release is a GitHub Release
there. Installed launchers check
`https://github.com/bldbrne-vr/bldbrne-vr/releases/latest/download/releases.win.json` on start
(launcher/main.cpp, `App::Check`), so **publishing a release is publishing the update**.
GitHub's `latest` is the newest published release that is not a draft or a pre-release.

One package holds the launcher and the game runtime (shadps4.exe) together: an update replaces
both. Users' settings and saves live in `%LocalAppData%\Bldbrne-VRData` (`BloodborneVRData` before
0.2.7; the launcher moves it once) and survive updates.

## The old installer (0.2.0 to 0.2.6) and the bridge

Up to 0.2.6 the package id was `BloodborneVR` on channel `vr-win` (installed to
`%LocalAppData%\BloodborneVR`). Since 0.2.7 it is `Bldbrne-VR` on channel `win`
(`%LocalAppData%\Bldbrne-VR`). Velopack cannot change a package id by updating, so 0.2.7 was
also built once as a **bridge** (`make_release.ps1 -Bridge`, `BBVR_BRIDGE=ON`): the old id's last
release. Old launchers update to it; its Play page offers *Install the new version*, which
downloads `Bldbrne-VR-win-Setup.exe`, installs it, and then uninstalls the old copy (a portable
copy is pointed at the new ZIP instead).

Old launchers keep reading `latest/download/releases.vr-win.json`, so **every release must carry
the bridge's feed**: `releases.vr-win.json`, `RELEASES-vr-win` and
`BloodborneVR-0.2.7-vr-win-full.nupkg`. `make_release.ps1` copies them from
`Build/launcher-bridge-feed` (written by the `-Bridge` run; otherwise download those three files
from the v0.2.7 release into it). Never upload the bridge's own Setup.exe or Portable.zip.

## Every release

1. **Pick the version** `x.y.z`, higher than the last published one (0.2.0 was the first).
2. **Commit everything.** `package.ps1` refuses a dirty checkout and records the commit in the
   package's `build-info.json`. Untracked scratch files count as dirty: list them in
   `.git/info/exclude` rather than using `-AllowDirty` for a public build.
3. **Build the runtime** (production: PGO + ThinLTO, logs compiled out, public identity):

   ```
   tools\release\build_fast.cmd RELEASE x.y.z
   ```

   It builds `tools/release/local/build-play/shadps4.exe` with `BBVR_RELEASE_VERSION`,
   so the window title shows `bldbrne-vr` / `release` instead of the remote and branch it was
   built from. (A later `build_fast.cmd PLAY` clears the stamp again.) Freeze a copy:
   `local/clean-<sha>-release-x.y.z/shadps4.exe`. Test it in the headset before shipping.
4. **Package**:

   ```
   .\launcher\make_release.ps1 -Version x.y.z -Runtime <frozen shadps4.exe> -ReleaseNotes notes.md
   ```

   First build the verified FSR 4 runtime with `tools\upscalers\build_fsr4.cmd`
   (fetch and prepare its pinned assets as described by that script). `-Fsr4 <folder>` selects
   another verified runtime; `-NoFsr4` explicitly creates a package without it.

   It builds the launcher stamped with the version, runs its tests, stages shadps4.exe, DLSS
   and FSR 4 (including asset notices), adds the Visual C++ runtime and the licences, and
   writes `Build/launcher-release/x.y.z/`:

   | File | What it is |
   | --- | --- |
   | `Bldbrne-VR-win-Setup.exe` | The installer (the README's download link) |
   | `Bldbrne-VR-win-Portable.zip` | No-install copy |
   | `Bldbrne-VR-x.y.z-full.nupkg` | The update package |
   | `releases.win.json`, `RELEASES`, `assets.win.json` | The update feed (`win` is the default channel, so two names carry no channel) |
   | `releases.vr-win.json`, `RELEASES-vr-win`, `BloodborneVR-0.2.7-vr-win-full.nupkg` | The bridge's feed, for launchers up to 0.2.6 |

   The release notes (Markdown) are shown by the launcher for the update.
5. **Test the update locally** before publishing: install the previous version's Setup.exe
   to a test folder, then point it at the new package folder:

   ```
   <old Setup.exe> --silent --installto <test dir>
   <test dir>\current\Bldbrne-VR.exe --update-smoke <Build\launcher-release\x.y.z> <marker.txt>
   ```

   Success: `marker.txt` contains `x.y.z` (the updated launcher restarted and wrote its version).
   Then `<test dir>\Update.exe --uninstall --silent`. Also check an old launcher still reaches
   the bridge: install 0.2.6's `BloodborneVR-vr-win-Setup.exe` the same way and run
   `current\BloodborneVR.exe --update-smoke` on the new folder: its marker must say `0.2.7`.
6. **Publish**: tag `vx.y.z`, title `Bldbrne-VR x.y.z`, the notes, **every file** of the folder
   (nine with the bridge's feed), not a pre-release, marked latest:

   ```
   gh release create vx.y.z <files> --repo bldbrne-vr/bldbrne-vr --title "Bldbrne-VR x.y.z" --notes "" --latest
   ```

   The installer and asset file names stay the same every version, so the README's
   `releases/latest/download/Bldbrne-VR-win-Setup.exe` link always gives the newest installer.
7. **Check**: open the installed launcher; it should offer the new version. Download the README
   link once to confirm it serves the new Setup.exe.

## Rules

- Never upload game executables, ROMs, firmware or a player's personal save profile.
  An explicitly selected dedicated benchmark save is the sole save-data exception;
  `-BenchmarkDirectory` copies only its hash-verified manifest entries and test runtime.
- Never ship the logging (USE) build or a build without the RELEASE stamp.
- A bad release: publish a fixed higher version. Do not delete or re-tag a published one
  (installed launchers may already be on it; Velopack never downgrades).
- The installer is not code-signed (SmartScreen warns). `package.ps1 -SignParameters` takes
  signtool arguments if a certificate is ever bought.
- GPL-2.0-or-later: every published binary's source (its commit) must be available from the
  public repository.

## Optional launcher benchmark

Build `launcher/benchmark_runtime` with CMake (Vulkan SDK, the repository's OpenXR
headers and zlib-ng). Create a fresh fixture directory using
`launcher/make_benchmark_fixture.ps1 -SaveHome <dedicated-fixture-home>
-TestRuntime <benchmark_xr.dll> -OutputDirectory <new-folder>`. This records the
source and binary hashes. Use a dedicated, tested Central Yharnam save, never a
player's live profile. The fixture's validated game versions must match APP_VER
(the tested CUSA03173 game reports 01.00, distinct from VERSION 01.09).

Pass `-BenchmarkDirectory <folder>` to make_release.ps1. Without it, the launcher
shows the benchmark as unavailable. The optional payload is isolated under
`benchmark/`; it is not registered as the user's OpenXR runtime. No firmware or
game binaries are included. Save files are copied to fresh private homes for each
run, and normal settings change only through Apply recommendation.

The runtime must support `--benchmark-headset` and the sparse `BBVR_BENCHMARK=1`
measurement records. These records survive BBVR_NO_LOGS; general logging stays
compiled out in releases. Validate the actual release binary, not just a logging
build, before shipping this feature. The test runtime's fixed FOV and static scene
produce a rendering estimate; they do not certify headset/compositor latency,
motion stability, or all game areas. See tests/vr_launcher_benchmark_manual.txt.

The user-facing scan takes its target from the active headset refresh rate, or
90 FPS when the runtime cannot report it. A manual target is also available.
The simulated display uses that target rate while game rendering remains uncapped.
The search first measures native and DIBR bases with extra reserve, reducing
resolution as needed, then measures the supported native temporal upscalers at
candidate output sizes. It currently treats DIBR without temporal upscaling as a
fallback; do not describe the result as an exhaustive global optimum.

Each recommendation requires repeatable valid measurements, frame-pacing and GPU
memory headroom. Unreliable samples get bounded retries; if repeatability still
cannot be established, the adaptive search stops instead of treating uncertainty
as evidence that lower resolution is required. Fixed research matrices continue
collecting all requested cases. The report retains rejected measurements and
reasons. Apply is explicit and rechecks hardware, runtime, provider files and
headset settings before persisting the chosen mode, graphics and target rate.

A repeatable native result below two-thirds of the target prioritizes a DIBR test at the same output resolution. This only changes benchmark order; it never switches the user's active mode without Apply. The threshold is a search heuristic, and DIBR still must pass the measured target and headroom checks.

Benchmark recommendations allow an 8% throughput tolerance (82.8 FPS for a 90 Hz target). Near-target results still need two consistent valid repeats, acceptable ordered frame pacing and median GPU time within the tolerated frame budget, and the existing GPU memory reserve. Results with target headroom rank ahead of near-target results. Near target does not mean a locked headset refresh rate; the adaptive base-resolution reserve remains stricter.

Quick scan is now the launcher default; thorough research is opt-in (CLI --thorough).
Quick mode screens at most three real configurations, then spends remaining runs
confirming promising options, with at most five game runs of up to 65 seconds (ending early when sufficient stable evidence is available). Measured elapsed time reserves a confirmation run before screening another option. Preparation
counts toward the 280-second stop deadline; owned-child shutdown allows five more
seconds. Slow disk preparation/headset discovery can exceed the nominal UI estimate.
No single probe can be recommended. If startup or background work prevents enough
valid evidence, the scan reports no winner rather than relaxing measurement gates.
The best confirmed tested option is not a claim of a globally optimal setting.

Near-target eligibility uses the requested 8% FPS tolerance. Ordered frame-time
telemetry now allows at most 5% slow frames and 5% time spent in slow frames, with
no contiguous slow burst longer than 100 ms. An isolated GPU-window p99 spike no
longer overrides steady published frame pacing; GPU median, memory reserve,
repeat consistency and measurement validity remain checked. Existing 250ms+ stall
rejection remains. Reports retain p99/p999 and include slow-frame/time fractions
and longest slow burst at the tolerated rate. These are explicit policy thresholds,
not claims that all users perceive judder identically. Comfortable-base search
still asks for additional reserve before spending it on upscaling.

FSR 3.1.5 is a separate optional provider (graphics provider ID 3). Package the
pinned Vulkan bridge with tools/upscalers/package_fsr31.py and pass
its verified output directory to make_release.ps1 -Fsr31. The package includes
embedded LDR shaders, constant-buffer reuse, provenance and redistribution
notices; frame generation is not included. It installs under fsr31/, independently
of fsr4/. The game FSR row cycles Off / FSR 4 / FSR 3.1.5; the launcher has a
separate FSR 3.1.5 choice. Both FSR versions currently use the native-stereo path.
Missing packages disable their launcher choices. The benchmark can select FSR
3.1.5 Quality or Performance and validates its actual provider/eye telemetry.
The quick scan's time budget may prevent testing every available provider.
FSR 4.1.1 remains an isolated diagnostic experiment, not a release replacement;
require correctness and matched game performance evidence before changing FSR 4.

PERF SCAN opens a modal from the sidebar or Graphics settings. It detects the
headset before offering Run and displays its active refresh rate and recommended
per-eye resolution. Missing-headset detection offers retry. The completion view
shows suggested settings, explicit Apply, and Re-run. The FPS report is always
visible, including excluded repeats and their reasons, with exact tested settings. Closing the modal during a scan leaves it running; Cancel stops it.

Adaptive scans now seed base resolution from headset pixel count, active target
rate and a conservative dedicated-memory budget (2/3/4 million pixels per eye
for <=8/<=12/>12 GiB at90Hz, reduced for higher refresh). This is a search heuristic,
not a GPU-speed prediction; supported scale remains50-100%. Scans start with
DLSS Quality on NVIDIA or FSR3.1.5 Quality elsewhere when available, falling back
to retained FSR4 or a base control. The game default for <=8GiB remains DIBR;
the scan tests temporal native stereo first at a conservative output size.
Quick scans can step resolution up or down and compare FSR3.1.5 when
DLSS lacks margin. At equal output and performance eligibility, temporal Quality
candidates get preference over unscaled native. Two valid repeats, pacing,
memory reserve and8% tolerance still govern recommendations. No saved user
settings change until Apply; FSR providers remain native-stereo only.

Startup recovery is declared per benchmark package. The fixture builder
accepts -StartupNavigation presentation-v1 only for a newly built runtime with the
BbvrBenchmarkStartupV1 export. The launcher verifies the runtime hash and inspects
that capability without executing DLL initialization. Missing protocol metadata
retains timed navigation; unknown protocols or unsupported runtimes are rejected.
Presentation-aware navigation uses flat/projection submission state, not title-menu
recognition; alpha-blended projection overlays do not count as gameplay. The local
build uses presentation-v1 after five successful normal launches and a 25-second
injected startup-stall test. New release fixtures should use -StartupNavigation
presentation-v1 with the matching runtime. Legacy packages retain timed navigation.
