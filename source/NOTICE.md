<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Notices

## This fork

Bloodborne VR is a modified version of [shadPS4](https://github.com/shadps4-emu/shadPS4), forked
from upstream commit `7fb1a530` ("Http2 fixes (#4908)"). shadPS4 is licensed GPL-2.0-or-later and
the fork's own code is under the same licence (`LICENSES/GPL-2.0-or-later.txt`).

- This fork contains changes made in 2026. Upstream copyright and license notices
  are retained; removing personal attribution does not change the existing licenses.
- `vk_presenter_vr.cpp`, `vk_rasterizer_vr.cpp`, `vk_rasterizer_vr_aa.cpp` and
  `vk_rasterizer_vr_draws.cpp` (in `source/src/video_core/renderer_vulkan/`) add member functions to
  upstream classes, so they keep the upstream line next to the fork's.
- The fork claims no copyright in its generated tables of values read from the game's files
  (`source/src/core/vr/bb_weapons.h`, `c0000_anim_lengths.h`, `c0000_transform_lengths.h`,
  `melee_moves.h`, `ragdoll_deaths.h`, `transform_clips.h`, `vr_page_art.h`, `whip_moves.h`, and
  `source/src/video_core/renderer_vulkan/vr_body_table.h`, `vr_cape_table.h`, `vr_hands_table.h`). These
  tables are being replaced, one by one, with data read at runtime from the user's own copy of the
  game.

Bloodborne VR is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by
Sony Interactive Entertainment, FromSoftware or the shadPS4 project. *Bloodborne* and
*PlayStation* are trademarks of their respective owners.

## Licence of a binary build

A binary built from this tree combines GPL-2.0-or-later code with Apache-2.0 code:

- the OpenXR loader's generated dispatch files, `xr_generated_loader.cpp` and
  `xr_generated_loader.hpp`, which are Apache-2.0 only;
- abseil-cpp, which protobuf pulls in.

Apache-2.0 is compatible with GPL-3.0 but not with GPL-2.0. A distributed binary is therefore
licensed under **GPL-3.0-or-later**, which shadPS4's "or later" permits. A binary release must
ship all of the following:

- the GPL-3.0 and Apache-2.0 licence texts, plus the texts in `LICENSES/`;
- the notices listed below;
- the complete corresponding source, including every submodule, offered from the same place as the
  binary.

## Third-party code inside the fork's files

| Code | Where | Copyright | Licence |
|---|---|---|---|
| SMAA (Enhanced Subpixel Morphological Antialiasing): the reference implementation and its AreaTex / SearchTex lookup textures | `source/src/video_core/host_shaders/smaa/SMAA.hlsl`, `source/src/video_core/renderer_vulkan/smaa/AreaTex.h`, `smaa/SearchTex.h` (each file keeps its notice), used by `vr_smaa.comp` and `vr_smaa.cpp` | 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez | MIT |
| AMD FidelityFX Super Resolution 1 (EASU and RCAS) | `source/src/video_core/host_shaders/fsr/ffx_a.h`, `fsr/ffx_fsr1.h` (inherited from upstream), used by `vr_eye_upscale.comp`; the RCAS formula restated in `vr_render_taa.comp` (`taa_sharpen`) | 2021 Advanced Micro Devices, Inc. | MIT |
| Catmull-Rom sampling through bilinear taps, after MJP's `SampleTextureCatmullRom` (the corner taps dropped: 5 taps) | `source/src/video_core/host_shaders/vr_render_taa.comp` (`SampleHistory`) | MJP (Matt Pettineo) | MIT |
| BPTC (BC7) partition and anchor tables, the same as in Mesa's `texcompress_bptc_tmp.h` (they are the tables of the BPTC specification) | `source/src/core/vr/bc7_decode.h` | 2014 Intel Corporation | MIT |

The MIT licence text is in `LICENSES/MIT.txt`. The files that embed this code say so in their SPDX
lines (`GPL-2.0-or-later AND MIT`).

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

Inherited third-party material in `scripts/ps4_names.txt`, `src/resources` and
`src/imgui/renderer/fonts` retains the accompanying notices and `REUSE.toml` annotations.

## Additional license texts and verification status

`LICENSES/` also retains GPL version 3, Apache-2.0, and the upstream license texts
for Pillow, BetterVR, bbfov and the SteamVR Unity plugin. These texts preserve
conditions relevant to credited references; their inclusion is not a finding that
any particular implementation is copied or that the whole release is cleared.

Patch provenance and final skull provenance remain under review. The project author
reports independently recreated logic; attribution metadata still needs reconciliation.
The staged source snapshot contains no EXEs or DLLs. Corresponding-source obligations
for any later distributed interaction DLL remain unresolved. The build also links FDK AAC,
which needs a separate GPL compatibility review; merely including its notice does
not settle compatibility. All actual runtime libraries and their transitive notices
must be checked against the final binary package.
