// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <iterator>

#include "core/vr/inframe.h"
#include "core/vr/quick_items.h"

namespace VR::StoredItems {

inline InFrame::MeshId Holder(int goods) {
    using enum InFrame::MeshId;
    switch (goods) {
    case 111:  // Silencing Blank
    case 200:  // Beckoning Bell
    case 205:  // Small Resonant Bell
    case 225:  // Sinister Resonant Bell
    case 1201: // Delayed Molotov
    case 1251: // Delayed Rope Molotov
    case 1411: // Monocular
    case 1500: // Madman's Knowledge
    case 1501: // Great One's Wisdom
    case 2020: // Beast Roar
    case 2050: // Choir Bell
    case 2070: // Tiny Tonitrus
    case 2080: // Executioner's Gloves
    case 2090: // Shaman Bone Blade
    case 2110: // Messenger's Gift
    case 2130: // Accursed Brew
    case 9000: // Kos Lights
        return Count;
    case 1110: // Beast Blood Pellet
    case 1300: // Fire Paper
    case 1320: // Bolt Paper
    case 1410: // Shining Coins
    case 2120: // Blacksky Eye
        return QuickHolsterLow;
    default:
        return QuickHolster;
    }
}

struct Fit {
    unsigned key; // twice the goods ID, plus one for the chest
    InFrame::MeshId mesh;
    unsigned model;
    std::array<float, 12> local;
    std::array<float, 3> offset; // metres along slot right, up, out
    std::array<float, 4> sphere; // pivot in mesh space, radius after fit scaling
};

inline constexpr Fit Fits[] = {
#include "core/vr/stored_item_fits.inc"
};

static_assert(std::ranges::is_sorted(Fits, {}, &Fit::key));

// The parts of a stored item share a mesh-local coordinate system. Bake its rotation,
// scale and pivot once, then follow the live mount, including body and held-item scale.
inline void Apply(InFrame::List& list, unsigned first, int goods, int slot, Quick::V3 right,
                  Quick::V3 up, Quick::V3 back, float body_scale, float model_scale) {
    if (first >= list.count || goods <= 0 || (slot != 1 && slot != 2))
        return;
    const unsigned key = static_cast<unsigned>(goods) * 2u + (slot == 2);
    const auto* fit = std::lower_bound(std::begin(Fits), std::end(Fits), key,
        [](const Fit& f, unsigned value) { return f.key < value; });
    const auto& item = list.items[first];
    if (fit == std::end(Fits) || fit->key != key || fit->mesh != item.mesh ||
        (item.mesh == InFrame::MeshId::Model && fit->model != item.tex_key))
        return; // A fallback primitive has a different pivot and bounds.
    const auto shift = (right * fit->offset[0] + up * fit->offset[1] - back * fit->offset[2]) * body_scale;
    const float offset[]{shift.x, shift.y, shift.z};
    const auto& l = fit->local;
    const auto& s = fit->sphere;
    std::array<float, 12> mv;
    std::array<float, 4> bounds;
    for (unsigned row = 0; row < 3; ++row) {
        const auto* m = item.mv.data() + 4 * row;
        auto* dst = mv.data() + 4 * row;
        for (unsigned col = 0; col < 4; ++col)
            dst[col] = m[0] * l[col] + m[1] * l[4 + col] + m[2] * l[8 + col];
        dst[3] += m[3] + offset[row];
        bounds[row] = m[0] * s[0] + m[1] * s[1] + m[2] * s[2] + m[3] + offset[row];
    }
    bounds[3] = s[3] * model_scale + .003f * body_scale;
    for (auto n = first; n < list.count; ++n) {
        list.items[n].mv = mv;
        list.items[n].cull_sphere = bounds;
    }
}

} // namespace VR::StoredItems
