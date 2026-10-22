// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "video_core/renderer_vulkan/vr_overlay_meshes.h"

namespace Vulkan::VrOverlay {
// Original procedural metal-and-glass syringe. Nominal size is fitted to the game's vial
// bounds by the caller; its original attachment and needle stay in their existing coordinates.
inline MeshData AntiqueSyringeMesh(u32 segments = 64) {
    const std::array<ProfilePoint, 38> p{{{0, 0},
                                          {.0012f, 0},
                                          {.0020f, .003f},
                                          {.0030f, .008f},
                                          {.0030f, .014f, true},
                                          {.0050f, .014f, true},
                                          {.0060f, .017f},
                                          {.0070f, .022f},
                                          {.0155f, .027f, true},
                                          {.0170f, .029f},
                                          {.0170f, .031f, true},
                                          {.0159f, .032f, true},
                                          {.0159f, .0352f, true},
                                          {.0155f, .0362f},
                                          {.0155f, .1398f},
                                          {.0159f, .1408f, true},
                                          {.0170f, .142f},
                                          {.0170f, .145f, true},
                                          {.0155f, .146f, true},
                                          {.0155f, .148f, true},
                                          {.0170f, .149f, true},
                                          {.0170f, .152f},
                                          {.0150f, .154f, true},
                                          {.0060f, .154f, true},
                                          {.0050f, .156f},
                                          {.0050f, .166f, true},
                                          {.0165f, .166f, true},
                                          {.0180f, .168f},
                                          {.0180f, .1695f, true},
                                          {.0168f, .1700f, true},
                                          {.0168f, .1715f, true},
                                          {.0180f, .172f, true},
                                          {.0180f, .174f},
                                          {.0160f, .176f, true},
                                          {.0110f, .176f, true},
                                          {.0090f, .175f, true},
                                          {.0030f, .175f},
                                          {0, .175f}}};
    MeshData mesh = Lathe(p, segments);
    // Two slender tie rods hold the ferrules together. Their narrow profile leaves the blood
    // visible from the front and rear, while reading as assembled surgical hardware in VR.
    const std::array<ProfilePoint, 6> rod{{{0, .032f},
                                           {.00085f, .032f, true},
                                           {.00115f, .034f},
                                           {.00115f, .143f},
                                           {.00085f, .146f, true},
                                           {0, .146f}}};
    for (float side : {-1.0f, 1.0f}) {
        auto rail = Lathe(rod, 12);
        TransformMesh(rail, Mat4::Translation({side * .0164f, 0, 0}));
        AppendMesh(mesh, rail);
    }
    return mesh;
}
} // namespace Vulkan::VrOverlay
