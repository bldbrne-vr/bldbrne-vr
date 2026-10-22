// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <array>
#include <cstddef>
#include <vector>

namespace VR::MusicBox {
// s11012's three disconnected crank pieces (shaft, arm, knob), measured in native
// mesh coordinates. Keep the casing, lid and all original vertex/texture data.
inline constexpr std::array<float, 3> CrankMin{.0224f, -.0494f, -.018f};
inline constexpr std::array<float, 3> CrankMax{.0346f, -.0087f, .0213f};
inline bool OriginalCrankVertex(const float* p) {
    for (int j = 0; j < 3; ++j)
        if (!(p[j] >= CrankMin[j] && p[j] <= CrankMax[j])) return false;
    return true;
}
template <class Vertices, class Index>
bool RemoveOriginalCrank(const Vertices& vertices, std::vector<Index>& indices) {
    if (indices.size() % 3 != 0) return false;
    std::vector<Index> kept;
    kept.reserve(indices.size());
    unsigned removed = 0;
    for (std::size_t i = 0; i < indices.size(); i += 3) {
        bool crank = true;
        for (int j = 0; j < 3; ++j) {
            if (indices[i+j] >= vertices.size()) return false;
            crank &= OriginalCrankVertex(vertices[indices[i+j]].position);
        }
        if (crank) ++removed;
        else kept.insert(kept.end(), indices.begin()+i, indices.begin()+i+3);
    }
    // Fail closed for a different asset version; never partially strip an unknown mesh.
    if (removed != 48 || kept.empty()) return false;
    indices.swap(kept);
    return true;
}
} // namespace VR::MusicBox
