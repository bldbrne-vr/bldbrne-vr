// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "core/vr/c0000_anim_lengths.h"

// The real lengths of the hunter's transform clips (a000_019000 .. a000_019999, the game's L1),
// from their Havok animations in chr/c0000_a00_hi.anibnd.dcx (an entry that plays another entry's
// animation has that one's length). c0000_anim_lengths.h's TAE length is wrong for them: every
// transform clip carries a ChrActionFlag 64 event over 0 .. 4.567 s, while the clips last
// 0.83 .. 3.17 s (the Saw Cleaver's 1.167 s). The VR body holds a transform's arms by these
// (core/vr/anim_classes.h). Generated offline (by a tool not included here); do not
// hand-edit the table.
namespace VR::Body::C0000 {

// Generated table: keep clang-format away.
// clang-format off
inline constexpr std::array<AnimLength, 180> TransformLengths = {{
    {19000, 1.167f}, {19001, 1.167f}, {19010, 1.167f}, {19011, 1.167f}, {19020, 2.5f},
    {19021, 2.5f}, {19030, 1.167f}, {19031, 1.167f}, {19040, 1.233f}, {19041, 1.233f},
    {19070, 1.833f}, {19071, 1.833f}, {19080, 1.867f}, {19081, 1.867f}, {19090, 1.4f},
    {19091, 1.4f}, {19092, 1.4f}, {19093, 1.4f}, {19094, 0.833f}, {19095, 0.833f}, {19100, 1.267f},
    {19101, 1.267f}, {19110, 1.267f}, {19111, 1.267f}, {19120, 1.4f}, {19121, 1.4f},
    {19130, 0.833f}, {19131, 0.833f}, {19140, 1.733f}, {19141, 1.733f}, {19150, 1.167f},
    {19151, 1.167f}, {19160, 1.5f}, {19161, 1.5f}, {19220, 1.667f}, {19221, 1.667f},
    {19230, 1.233f}, {19231, 1.233f}, {19232, 1.233f}, {19233, 1.233f}, {19234, 0.833f},
    {19235, 0.833f}, {19240, 1.233f}, {19241, 1.233f}, {19242, 1.233f}, {19243, 1.233f},
    {19244, 0.833f}, {19245, 0.833f}, {19250, 1.4f}, {19251, 1.4f}, {19252, 1.4f}, {19253, 1.4f},
    {19254, 0.833f}, {19255, 0.833f}, {19260, 1.4f}, {19261, 1.4f}, {19262, 1.4f}, {19263, 1.4f},
    {19264, 0.833f}, {19265, 0.833f}, {19270, 1.4f}, {19271, 1.4f}, {19272, 1.4f}, {19273, 1.4f},
    {19274, 0.833f}, {19275, 0.833f}, {19280, 0.833f}, {19281, 0.833f}, {19282, 0.833f},
    {19283, 0.833f}, {19284, 0.833f}, {19285, 0.833f}, {19290, 0.833f}, {19291, 0.833f},
    {19300, 1.333f}, {19301, 1.333f}, {19310, 1.6f}, {19311, 1.6f}, {19312, 1.6f}, {19313, 1.6f},
    {19314, 0.833f}, {19315, 0.833f}, {19350, 1.733f}, {19351, 1.733f}, {19352, 1.733f},
    {19353, 1.733f}, {19354, 0.833f}, {19355, 0.833f}, {19360, 1.5f}, {19361, 1.5f},
    {19370, 1.333f}, {19371, 1.333f}, {19400, 1.167f}, {19401, 1.167f}, {19410, 1.333f},
    {19411, 1.333f}, {19412, 1.333f}, {19413, 1.333f}, {19414, 0.833f}, {19415, 0.833f},
    {19500, 1.433f}, {19501, 1.433f}, {19510, 1.4f}, {19511, 1.4f}, {19520, 1.6f}, {19521, 1.6f},
    {19530, 1.467f}, {19531, 1.467f}, {19540, 1.5f}, {19541, 1.5f}, {19550, 1.333f},
    {19551, 1.333f}, {19560, 2.333f}, {19561, 2.333f}, {19570, 1.667f}, {19571, 1.667f},
    {19572, 1.667f}, {19573, 1.667f}, {19574, 0.833f}, {19575, 0.833f}, {19580, 1.233f},
    {19581, 1.233f}, {19590, 1.233f}, {19591, 1.233f}, {19592, 1.233f}, {19593, 1.233f},
    {19594, 0.833f}, {19595, 0.833f}, {19600, 1.6f}, {19601, 1.6f}, {19610, 1.6f}, {19611, 1.6f},
    {19620, 3.167f}, {19621, 3.167f}, {19630, 1.6f}, {19631, 1.6f}, {19632, 1.6f}, {19633, 1.6f},
    {19634, 0.833f}, {19635, 0.833f}, {19640, 1.233f}, {19641, 1.233f}, {19650, 1.4f},
    {19651, 1.4f}, {19652, 1.4f}, {19653, 1.4f}, {19654, 0.833f}, {19655, 0.833f}, {19660, 1.733f},
    {19661, 1.733f}, {19670, 2.2f}, {19671, 2.2f}, {19672, 2.2f}, {19673, 2.2f}, {19674, 0.833f},
    {19675, 0.833f}, {19680, 1.5f}, {19681, 1.5f}, {19690, 1.4f}, {19691, 1.4f}, {19692, 1.4f},
    {19693, 1.4f}, {19694, 0.833f}, {19695, 0.833f}, {19700, 1.867f}, {19701, 1.867f},
    {19710, 1.4f}, {19711, 1.4f}, {19712, 1.4f}, {19713, 1.4f}, {19714, 0.833f}, {19715, 0.833f},
    {19720, 1.833f}, {19721, 1.833f}, {19730, 1.7f}, {19731, 1.7f}, {19732, 1.7f}, {19733, 1.7f},
    {19734, 0.833f}, {19735, 0.833f},
}};
// clang-format on

constexpr bool TransformLengthsSorted() {
    for (std::size_t i = 1; i < TransformLengths.size(); ++i) {
        if (TransformLengths[i - 1].id >= TransformLengths[i].id) {
            return false;
        }
    }
    return true;
}
static_assert(TransformLengthsSorted());

// The clip length of transform animation `id` (a000_019xxx) in seconds, or a negative value when
// it is not one.
inline float TransformLengthSeconds(std::int32_t id) {
    const auto it = std::lower_bound(
        TransformLengths.begin(), TransformLengths.end(), id,
        [](const AnimLength& entry, std::int32_t value) { return entry.id < value; });
    return it != TransformLengths.end() && it->id == id ? it->seconds : -1.0f;
}

} // namespace VR::Body::C0000
