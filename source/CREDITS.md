<!--
SPDX-FileCopyrightText: 2026 BB-VR contributors
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Credits

Bloodborne VR builds on the work of many people. Licences and notices are in
[NOTICE.md](NOTICE.md).

## The emulator

- **The shadPS4 team and contributors**: the emulator this project modifies
  ([shadPS4](https://github.com/shadps4-emu/shadPS4)). Their README credits the projects that
  helped them.

- **Supermedo's Windows bbport**: texture cleanup progress, synchronous write-back ordering,
  and allocation-failure reclamation/retry, adapted for this renderer
  ([source commit 5f56ed6a](https://github.com/Supermedo/bloodborne_pc/commit/5f56ed6a7ca47c37049cbf737a244707a3ecc4d2),
  GPL-2.0-or-later).

## The camera

- **BBMCC** by Mob ([github.com/Mob100percent/BBMCC](https://github.com/Mob100percent/BBMCC),
  GPL-3.0): the research behind the camera hook sites and the camera's pitch, yaw and lock-on
  fields (`src/core/vr/bloodborne_camera.*`).
- **imedved** (the shadPS4BBMouseandFOV Cheat Engine script) and **ajungg** (BBShadPS4Mouse): the
  mouse-camera and FOV work that BBMCC builds on.
- **bbfov** by paylanon ([github.com/paylanon/bbfov](https://github.com/paylanon/bbfov)): the
  camera's FOV field.
- **The first-person camera mod on Nexus Mods** (Bloodborne mod 287): the patch site the first
  person camera uses (`FirstPersonModRva` in `bloodborne_camera.cpp`).

## Community patches

- **Kyo**: the resolution and light-grid patch templates that this project's own resolution,
  light-grid and frame-limiter lines follow. Kyo, **illusion**, **auser1337** and **Encapsulate**
  also wrote the community patches (60 FPS++, skip intro, AA, depth of field, motion blur,
  chromatic aberration, model LOD) that the VR presets apply. Those patches are included, with
  their authors' names, in `launcher/bbvr_patch.xml`.

## VR body, hands and items: ideas and tuning

- **FRIK** (the Fallout 4 VR body mod): the clavicle reach toward far targets and its constants
  (`src/core/vr/arm_ik.h`).
- **BetterVR** (the Breath of the Wild VR mod for Cemu): the elbow pole in body space, out to the
  side, down and back (`arm_ik.h` `BodyPole`).
- **HIGGS** (the Skyrim VR hands mod) and **Valve's SteamVR Interaction System**: throw release
  velocity taken from the hand's recent peak, and the release rule for a grip opened while the hand
  still speeds up (`src/core/vr/quick_items.h`).
- **UEVR** by praydog: submitting one symmetric render as per-eye sub-rectangles
  (`openxr_presenter.cpp`), and the live weapon "Adjust" workflow (`src/core/vr/grip_adjust.h`).
- **Half-Life: Alyx**: the design reference for the syringe's feel (`src/core/vr/inject_hold.h`).

## Algorithms and formulas

- **Christer Ericson**, *Real-Time Collision Detection* (2005), section 5.1.9: the closest points of
  two segments (`src/core/vr/evasion_hitgeom.h`, `src/core/vr/quick_items.h`).
- **Brian Karis** (Epic Games), "Physically Based Shading on Mobile": the analytic environment BRDF
  fit (`vr_overlay.frag` `EnvBRDFApprox`).
- **Inigo Quilez**: the 2D signed distance functions; **The Art of Code** (Martijn Steinrucken):
  the `Hash21` hash; both used by the procedural goggles in `vr_inframe.frag`.
- **Sebastiano Vigna**: SplitMix64 (public domain), the ragdoll's random draws
  (`src/core/vr/ragdoll_kill.h`).
- **Ken Shoemake**: uniform random rotations (`tests/test_arm_ik.cpp`).
- **MJP** (Matt Pettineo): Catmull-Rom through bilinear taps (`vr_render_taa.comp`, MIT).
- **AMD FidelityFX Super Resolution 1** (EASU, RCAS; MIT): the eye upscale and the TAA's sharpening.
- **SMAA** by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez
  (MIT): the `aa_mode=smaa` option.
- **Mesa** / **Intel**: the BPTC tables in `src/core/vr/bc7_decode.h` (MIT).
- **Pillow**: the VR tile icon is composited with Pillow's resampling rule (bicubic up, Lanczos
  down, `precompute_coeffs`), reimplemented in `src/core/vr/tile_icon_compose.h`. Pillow's BC1 and
  BC7 decoders are the reference in the unit tests.

## Libraries

- The **OpenXR SDK** (The Khronos Group) and every library in `externals/`; see
  [NOTICE.md](NOTICE.md).
