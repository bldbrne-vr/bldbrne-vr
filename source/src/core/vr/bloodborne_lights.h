// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot): the light sets' camera-attached lights in VR.
//
// A light set (the 288-byte rows the light-set pass reads, FS 0xd920b17569d) has up to four
// directional lights. Light 0 (the moon or sun) is a world direction. Lights 1-3 may instead be
// fill and shine lights fixed to the camera: the set builders build their direction from the
// set's two angles and turn it with the camera's world matrix. On a flat screen the camera turns
// with the hunter; in VR the camera is your head, so every glance swung those highlights across
// the surfaces (2026-10-20, Bloodstarved Beast's entrance and elsewhere). With
// body_fill_lights=1 (off by default: it lost too much atmosphere, the tester kept the original
// look) the builders get the camera's
// matrix without your head's look: levelled, and turned back by the head's yaw, so the hunter's
// heading (stick turns) still turns them as in the flat game while looking around leaves them in
// place. Nothing else reads that matrix there, so world lights, lamps and the moon are untouched.
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled): checks both builders' prologues and hooks them. Logs one line.
void InstallLights(VAddr base, u64 size);

} // namespace VR::Bloodborne
