<!--
SPDX-FileCopyrightText: 2026 BB-VR contributors
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Notices

## This fork

Bloodborne VR is a modified version of [shadPS4](https://github.com/shadps4-emu/shadPS4), forked
from upstream commit `7fb1a530` ("Http2 fixes (#4908)").

- **shadPS4's code** is licensed GPL-2.0-or-later (`LICENSES/GPL-2.0-or-later.txt`). Upstream files
  changed by the fork stay under that licence; they keep their original copyright lines and gain
  the line `SPDX-FileCopyrightText: 2026 BB-VR contributors`, which marks the file as modified by
  this fork in 2026 (GPL-2.0, section 2(a)).
- **The fork's own code and assets** (every file whose only copyright line is
  `SPDX-FileCopyrightText: 2026 BB-VR contributors`) are licensed **AGPL-3.0-or-later**
  (`LICENSES/AGPL-3.0-or-later.txt`), from version 0.5.0 on. Earlier releases were distributed under
  GPL-2.0-or-later. Each file's `SPDX-License-Identifier` line states its licence.
- `vk_presenter_vr.cpp`, `vk_rasterizer_vr.cpp`, `vk_rasterizer_vr_aa.cpp` and
  `vk_rasterizer_vr_draws.cpp` (in `src/video_core/renderer_vulkan/`) add member functions to
  upstream classes, so they keep the upstream line next to the fork's.
- The fork claims no copyright in its generated tables of values read from the game's files
  (`src/core/vr/bb_weapons.h`, `c0000_anim_lengths.h`, `c0000_transform_lengths.h`,
  `lever_models.h`, `melee_moves.h`, `pouch_armor_fits.h`, `ragdoll_deaths.h`, `transform_clips.h`,
  `vr_page_art.h`, `weapon_mass_geometry.h`, `weapon_shape_moments.h`, `whip_moves.h`, and
  `src/video_core/renderer_vulkan/vr_body_table.h`, `vr_cape_table.h`, `vr_hands_table.h`,
  `vr_pouch_table.h`). These
  tables are being replaced, one by one, with data read at runtime from the user's own copy of the
  game.

Bloodborne VR is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by
Sony Interactive Entertainment, FromSoftware or the shadPS4 project. *Bloodborne* and
*PlayStation* are trademarks of their respective owners.

## Licence of a binary build

A binary built from this tree combines GPL-2.0-or-later code (shadPS4), AGPL-3.0-or-later code (this
fork) and Apache-2.0 code:

- the OpenXR loader's generated dispatch files, `xr_generated_loader.cpp` and
  `xr_generated_loader.hpp`, which are Apache-2.0 only;
- abseil-cpp, which protobuf pulls in.

Apache-2.0 is compatible with GPL-3.0 but not with GPL-2.0, and GPL-3.0 section 13 permits combining
GPL-3.0 code with AGPL-3.0 code. A distributed binary is therefore licensed under
**GPL-3.0-or-later** (which shadPS4's "or later" permits), with the terms of **AGPL-3.0-or-later**
applying to the fork's parts and to the combination as AGPL-3.0 section 13 requires. A binary
release must ship all of the following:

- the licence texts in `LICENSES/`, including `GPL-3.0-or-later.txt`, `AGPL-3.0-or-later.txt` and
  `Apache-2.0.txt`;
- the notices listed below;
- the complete corresponding source, including every submodule, offered from the same place as the
  binary.

## Third-party code inside the fork's files

| Code | Where | Copyright | Licence |
|---|---|---|---|
| SMAA (Enhanced Subpixel Morphological Antialiasing): the reference implementation and its AreaTex / SearchTex lookup textures | `src/video_core/host_shaders/smaa/SMAA.hlsl`, `src/video_core/renderer_vulkan/smaa/AreaTex.h`, `smaa/SearchTex.h` (each file keeps its notice), used by `vr_smaa.comp` and `vr_smaa.cpp` | 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez | MIT |
| AMD FidelityFX Super Resolution 1 (EASU and RCAS) | `src/video_core/host_shaders/fsr/ffx_a.h`, `fsr/ffx_fsr1.h` (inherited from upstream), used by `vr_eye_upscale.comp`; the RCAS formula restated in `vr_render_taa.comp` (`taa_sharpen`) | 2021 Advanced Micro Devices, Inc. | MIT |
| Catmull-Rom sampling through bilinear taps, after MJP's `SampleTextureCatmullRom` (the corner taps dropped: 5 taps) | `src/video_core/host_shaders/vr_render_taa.comp` (`SampleHistory`) | MJP (Matt Pettineo) | MIT |
| BPTC (BC7) partition and anchor tables, the same as in Mesa's `texcompress_bptc_tmp.h` (they are the tables of the BPTC specification) | `src/core/vr/bc7_decode.h` | 2014 Intel Corporation | MIT |

The MIT licence text is in `LICENSES/MIT.txt`. The files that embed this code say so in their SPDX
lines (`GPL-2.0-or-later AND MIT`).

`LICENSES/` also keeps the upstream licence texts of Pillow, BetterVR, bbfov and the SteamVR Unity
plugin (`Pillow.txt`, `BetterVR.txt`, `bbfov.txt`, `SteamVR.txt`), which `CREDITS.md` credits as
references. They preserve the conditions that apply to those references; including them is not a
finding that any particular implementation is copied.

## Third-party libraries (`externals/`)

Each external library keeps its own licence files in its directory, and `REUSE.toml` annotates the
files that cannot carry a header. A binary release must reproduce the notices of the libraries it
links. Of note:

- **OpenXR SDK loader** (`externals/openxr`, added by this fork, statically linked): Copyright The
  Khronos Group Inc. Most loader files are `Apache-2.0 OR MIT` and are used here under MIT. The
  generated dispatch files are Apache-2.0 only (see above). The loader bundles jsoncpp (Copyright
  Baptiste Lepilleur and The JsonCpp Authors, public domain or MIT).
- **Fraunhofer FDK AAC** (`externals/aacdec/fdk-aac`, inherited from upstream): the "Software
  License for The Fraunhofer FDK AAC Codec Library for Android" (copyright 1995-2018
  Fraunhofer-Gesellschaft). Its `NOTICE` file must accompany a binary release.
- **LibreSSL** (`externals/libressl`, inherited from upstream): its `COPYING` file holds the
  OpenSSL and SSLeay licences. A binary release must display their acknowledgements: "This product
  includes software developed by the OpenSSL Project for use in the OpenSSL Toolkit
  (http://www.openssl.org/)" and "This product includes cryptographic software written by Eric
  Young (eay@cryptsoft.com)".
- **abseil-cpp** (`externals/abseil-cpp`, through protobuf, inherited from upstream): Copyright The
  Abseil Authors, Apache-2.0.

Upstream's own third-party material (`scripts/ps4_names.txt`, `src/resources`, the fonts in
`src/imgui/renderer/fonts`) is described in
`REUSE.toml` as upstream ships it.

## NVIDIA DLSS (shipped with releases from 0.2.9)

Releases carry `runtime/dlss/`:

- `nvngx_dlss.dll`: NVIDIA DLSS Super Resolution, NVIDIA's own signed runtime, redistributed
  unmodified under the NVIDIA RTX SDKs licence (`LICENSES/NVIDIA-RTX-SDKs-DLSS.txt`). DLSS and
  NVIDIA are trademarks of NVIDIA Corporation. It is loaded at run time and only when the player
  chooses DLSS (off by default).
- `vr_dlss_stereo.dll`: the bridge that calls it, a separate program loaded at run time.
  Copyright 2026 IFreemz (shadps4_dlss bridge) and bbport contributors, MIT
  (`LICENSES/DLSS-bridge-MIT.txt`); source in `tools/upscalers/adapted/dlss_bridge`.
  This software contains source code provided by NVIDIA Corporation.

- `nvngx.dll_dlssnr.dll` and the DLSS 5 chain inside `vr_dlss_stereo.dll`
  (`tools/upscalers/adapted/dlss5_bridge`): derived from OptiScaler-DLSSNR
  (https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass), GPL-3.0
  (`LICENSES/DLSS5-bridge-GPL-3.0.txt`), with RenoDX's attribution
  (`LICENSES/DLSS5-RenoDX-attribution.txt`). With that chain the bridge as a whole is GPL-3.0.

NVIDIA's DLSS 5 runtime (`nvngx_dlssnr.dll`) is not shipped and is never downloaded by the
launcher. A player with an RTX 50-series card may add their own copy in the launcher's DLSS 5 tab
(found on their PC or picked by hand); only NVIDIA's signed, unmodified file is accepted, and it is
copied to the player's data folder (`user/dlss5`).

## Real-ESRGAN (the launcher's sharper skies)

Releases carry `realesrgan/` next to the launcher: `realesrgan-ncnn-vulkan.exe`, its OpenMP
runtime (`vcomp140.dll`, Microsoft's redistributable) and the `realesrgan-x4plus` model, exactly as
in the official Real-ESRGAN v0.2.5.0 Windows release (pinned by SHA-256 in
`launcher/CMakeLists.txt`). Real-ESRGAN and its models: Copyright (c) 2021 Xintao Wang,
BSD-3-Clause (`LICENSES/Real-ESRGAN-BSD-3-Clause.txt`); realesrgan-ncnn-vulkan: MIT
(`LICENSES/Real-ESRGAN-ncnn-vulkan-MIT.txt`); ncnn: Tencent, BSD-3-Clause
(`LICENSES/ncnn-BSD-3-Clause.txt`). It is a separate program the launcher runs only when the
player asks for sharper skies; it upscales textures from the player's own game folder, and the
images it makes stay on the player's PC (`user/sky_detail`). Nothing derived from the game ships.

## AMD FSR 4 (optional runtime)

FSR-enabled releases carry `runtime/fsr4/vr_fsr4_stereo.dll` and `fsr4_shaders/`.
The GPL-2.0-or-later bridge is under `tools/upscalers/adapted/fsr4_bridge`.
The pinned MIT provider is under `tools/upscalers/upstream/fsr-vulkan`;
its copyright and notices ship in `LICENSES/FSR-Vulkan-MIT.txt` and
`LICENSES/FSR-Vulkan-NOTICE.md`.
The v07 model assets retain AMD's MIT notice in
`runtime/fsr4/fsr4_shaders/LICENSE-FSR4-v07.txt`. The package includes the pinned
asset manifest and hashes, guarded pass-11 shaders and optimized post/SPD shaders.
FSR is loaded only when selected; it remains off by default. AMD and FidelityFX
are trademarks of Advanced Micro Devices, Inc.

## AMD FSR Upscaling 4.1 (optional runtime, AMD cards)

FSR 4.1 releases carry `runtime/fsr41/`: our GPL-2.0-or-later bridge
`vr_fsr41_stereo.dll` (`tools/upscalers/adapted/fsr41_bridge`) and AMD's own
signed `amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll`, unmodified, from
AMD FSR SDK v2.4.0 (GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, commit
`7539ae8b6568ad29ed78a753404351daf3b5cca7`). The SDK's license lists those DLLs under its
permissive MIT-style terms; it ships as `runtime/fsr41/LICENSE-AMD-FSR-SDK.md`, and
`package.json` records each file's SHA-256. The bridge is built against the SDK's MIT-licensed
API headers only and loads AMD's DLLs at run time. It is offered on AMD cards only and loaded
only when selected.

## Optional benchmark runtime

`benchmark/benchmark_xr.dll` is built from
`launcher/benchmark_runtime/mockxr.cpp`, this project's MockXR test runtime. The fixture manifest records the source and DLL
SHA-256 hashes. This test runtime uses the repository's OpenXR headers and Vulkan
headers and statically links zlib-ng; their existing notices apply, with copies in
`LICENSES/Benchmark-OpenXR.txt` and `LICENSES/Benchmark-zlib-ng.txt` when packaged.
It is selected only in benchmark child processes, never registered globally.
