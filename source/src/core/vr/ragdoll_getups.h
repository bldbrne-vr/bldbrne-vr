// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

// The enemies' knockdown clips (Bloodborne v1.09 files, the chr anibnds' group-0 TAE), for the
// Amygdalan Arm hook's get-up: a hooked enemy limp on the ground hands its body to the clip
// where it lies and starts to get up, so it rises on its own legs instead of the limp pose
// blending up into a standing one (core/vr/bloodborne_ragdoll.cpp ArmPhase::Down). For each chr
// that can ragdoll (ragdoll_deaths.h) with one: a2100 (else a2110), where its TAE event 226
// with arg 50 (the "down" span, from 0) ends, and the clip's last event's end; a clip down to
// its end (no get-up in it: less than 0.6 s after the span) does not count. 36 of
// 78 chrs; the rest keep the plain blend. Generated offline; do not
// hand-edit.
namespace VR::Ragdoll {

struct GetUp {
    std::int32_t chr;
    std::int32_t clip;     // the TimeAct id (group 0)
    std::uint16_t up_ms;   // the "down" span ends: it starts to get up
    std::uint16_t end_ms;  // its last event ends
};

inline constexpr std::array<GetUp, 36> GetUps{{
    {1000, 2100, 1833, 3633}, {1040, 2100, 1833, 3067}, {1050, 2110, 2167, 3067},
    {1060, 2110, 2167, 3067}, {1090, 2100, 1833, 3067}, {2000, 2100, 1833, 3067},
    {2010, 2100, 1833, 3633}, {2020, 2100, 1833, 3067}, {2050, 2100, 1833, 3067},
    {2100, 2100, 1833, 3067}, {2110, 2100, 1833, 2767}, {2170, 2100, 1833, 3067},
    {2180, 2100, 1833, 6667}, {2260, 2100, 1833, 2833}, {2330, 2100, 1833, 2833},
    {2400, 2100, 1833, 3067}, {2500, 2100, 1833, 3067}, {2600, 2100, 1833, 3067},
    {2610, 2100, 1833, 3067}, {2640, 2100, 1833, 3633}, {2700, 2100, 1833, 3067},
    {2800, 2100, 1833, 3067}, {2810, 2100, 1833, 3067}, {3010, 2110, 2167, 3067},
    {3020, 2100, 1833, 3067}, {3030, 2100, 1833, 3067}, {3040, 2100, 1833, 4167},
    {3120, 2100, 1833, 3633}, {4000, 2100, 1833, 3633}, {4010, 2100, 1900, 2833},
    {4020, 2100, 1833, 3633}, {4040, 2100, 1833, 2767}, {4120, 2100, 1833, 3633},
    {4140, 2100, 1833, 3067}, {7010, 2100, 1833, 3633}, {7500, 2100, 1833, 3067},
}};

inline const GetUp* FindGetUp(std::int32_t chr) {
    const auto it = std::lower_bound(GetUps.begin(), GetUps.end(), chr,
                                     [](const GetUp& g, std::int32_t c) { return g.chr < c; });
    return it != GetUps.end() && it->chr == chr ? &*it : nullptr;
}

} // namespace VR::Ragdoll
