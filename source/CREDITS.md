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

- **bbport** by deadinside28 ([github.com/deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc),
  GPL-2.0-or-later): the frame-rate hooks the game's timing runs on
  (`src/core/vr/bloodborne_frame_rate.*`, ported from `gpu/shim/bbport_frame_rate.cpp`): the
  game's frame pacer and step, and the per-frame values it ties to a 30 FPS frame, with every site
  and value as there.
  Its render-job slot repair (`gpu/games/bloodborne/hooks_jobs.cpp`, commit `0fc2921`)
  is adapted in `src/core/vr/bloodborne_render_jobs.cpp`: failed and cancelled jobs
  return their slots to the game's queue, preventing its six-slot pool from leaking.

## The camera

- **BBMCC** by Mob ([github.com/Mob100percent/BBMCC](https://github.com/Mob100percent/BBMCC),
  GPL-3.0): the research behind the camera hook sites and the camera's pitch, yaw and lock-on
  fields (`src/core/vr/bloodborne_camera.*`).
- **imedved** (the shadPS4BBMouseandFOV Cheat Engine script) and **ajungg** (BBShadPS4Mouse): the
  mouse-camera and FOV work that BBMCC builds on.
- **ID-Gamer** ([third-person-gamepad](https://github.com/ID-Gamer/bldbrne-vr/commit/273dadfe0609556d3589929aed1db6e9929f1a90),
  offered to us under the GPL): third-person gamepad play: the right stick raising and lowering
  the camera, camera distance, view tilt and the lock-on pitch cap
  (`src/core/vr/bloodborne_camera.cpp`, `src/core/vr/vr_session.cpp`), the gamepad on the VR
  page (`src/core/vr/page_pad.h`) and Echoes and Insight under the health bar.
- **bbfov** by paylanon ([github.com/paylanon/bbfov](https://github.com/paylanon/bbfov)): the
  camera's FOV field.
- **The first-person camera mod on Nexus Mods** (Bloodborne mod 287): the patch site the first
  person camera uses (`FirstPersonModRva` in `bloodborne_camera.cpp`).

## Community patches

- **emoose**: the Intel 12th Gen+ SFX workaround, distributed in
  [bbport's Bloodborne patch catalog](https://github.com/Supermedo/bloodborne_pc/blob/windows/patches/Bloodborne.xml).
  The launcher exposes it as **Skip dangerous SFX**, enabled by default.

- **Kyo**: the resolution and light-grid patch templates that this project's own resolution,
  light-grid and frame-limiter lines follow. Kyo, **illusion**, **auser1337** and **Encapsulate**
  also wrote the community patches (60 FPS++, skip intro, AA, depth of field, motion blur,
  chromatic aberration, model LOD) that the VR presets apply. Those patches are included, with
  their authors' names, in `launcher/bbvr_patch.xml`.

- **Lantern Engine by Awaken** ([github.com/Awakenchan/LanternEngine](https://github.com/Awakenchan/LanternEngine)):
  where the game's unused screen-space reflections are switched on (its "Enable Screen Space
  Reflections" patch: the sites `src/core/vr/game_reflections.cpp` writes), and the idea of
  remembering lamps in the world so their glow in the air stays steady, plus its colour
  treatment and the map-light marker in the light records (`fog_light.h`); the visibility-bitmask
  ambient occlusion (`ambient_occlusion.h`) and the moon shadows' tuning (`moon_shadow.h`) follow
  their GPL-2.0-or-later `gtao.frag` and `contact_shadow.comp`; the wet streets' reflection trace
  (`reflections.h`: a steadied normal, halvings at the crossing, a hit test in metres, the edge
  fade where the hit is) follows the ideas of their `ssr.frag`. Reimplemented here in the shader
  recompiler, no code copied.

## VR body, hands and items: ideas and tuning

These projects were inspiration only: the ideas and tuning below were studied and implemented
independently in this project. No code has been taken from them.

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

## Special thanks

- **iamwh0iam**: for help with the immersive VR interactions.
- **Dirol** (dishisolo), **BlizzardNight** and **Chip**: for awesome feedback, suggestions and bug
  reports.

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
- **AMD FSR SDK 2.4.0** (MIT-style): AMD's own FSR Upscaling 4.1, its signed Vulkan loader and
  upscaler, behind the FSR 4.1 option on AMD cards (`runtime/fsr41`).
- **SMAA** by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez
  (MIT): the `aa_mode=smaa` option.
- **Mesa** / **Intel**: the BPTC tables in `src/core/vr/bc7_decode.h` (MIT).
- **Pillow**: the VR tile icon is composited with Pillow's resampling rule (bicubic up, Lanczos
  down, `precompute_coeffs`), reimplemented in `src/core/vr/tile_icon_compose.h`. Pillow's BC1 and
  BC7 decoders are the reference in the unit tests.

## Libraries

- The **OpenXR SDK** (The Khronos Group) and every library in `externals/`; see
  [NOTICE.md](NOTICE.md).
- **Real-ESRGAN** (Xintao Wang et al.) and its ncnn Vulkan build (ncnn by Tencent, the
  realsr-ncnn-vulkan design by nihui): the launcher's sharper skies; see [NOTICE.md](NOTICE.md).
