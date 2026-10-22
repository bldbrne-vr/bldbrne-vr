// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <array>
#include <cstdint>

namespace VR::BodyMounts {

struct ArmorPouchFit {
    std::int32_t armor;
    float outward_m;
};

inline constexpr std::array<ArmorPouchFit, 34> ArmorPouchFits{{
    {11000, 0.075f},
    {21000, 0.121f},
    {31000, 0.099f},
    {41000, 0.000f},
    {51000, 0.072f},
    {61000, 0.083f},
    {71000, 0.069f},
    {81000, 0.112f},
    {101000, 0.080f},
    {111000, 0.089f},
    {121000, 0.128f},
    {131000, 0.060f},
    {141000, 0.081f},
    {151000, 0.065f},
    {181000, 0.085f},
    {191000, 0.072f},
    {201000, 0.066f},
    {211000, 0.111f},
    {221000, 0.000f},
    {231000, 0.000f},
    {241000, 0.090f},
    {281000, 0.069f},
    {291000, 0.089f},
    {301000, 0.066f},
    {311000, 0.075f},
    {321000, 0.000f},
    {331000, 0.081f},
    {341000, 0.074f},
    {351000, 0.104f},
    {361000, 0.067f},
    {371000, 0.063f},
    {381000, 0.083f},
    {391000, 0.044f},
    {401000, 0.147f},
}};

constexpr float PouchOutwardM(std::int32_t armor) {
    for (const auto& fit : ArmorPouchFits) {
        if (fit.armor == armor) {
            return fit.outward_m;
        }
    }
    return 0.0f;
}

}
