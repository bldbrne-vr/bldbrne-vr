// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

// The lit Boom Hammer's explosion (its form B: the furnace lit, weapon style 5), as the game's own
// attack clips play it (c0000.anibnd a145.tae, 1.09). Physical melee invokes the attack rows
// without the clips (core/vr/bloodborne_melee.h), so the clips' fire and boom are put back here, at
// the first thing a lit heavy or charged swing hits (an enemy, a wall, the ground):
//  - heavy, judge 550 (anim 42620): FFX 252042 and the flame roar c877 at the hit. Its attack row
//    is the blast already (a 1 m sphere round the head, 12100);
//  - charged, judge 570 (anim 42630): FFX 252041 and 1032002 on the head (12100), the deep boom
//    c873, the bang c875 and the roar c877 at the hit, and the blast row 571 (a 1.2 m sphere on the
//    head, event 1 for 0.10 s). 570 and 571 share one hit list (AtkParam isShareHitList), so the
//    blast reaches the enemies round the head, not the one the swing hit again;
//  - light, judge 400 (anim 40000): no explosion in the game either; nothing here.
// The FFX 25204x are in the add-on's effects (frpg_sfxbnd_dlceffects), loaded with the weapon.
namespace VR::BoomBlast {

inline constexpr std::int32_t Variation = 2800; // the Boom Hammer's behaviour variation
inline constexpr std::int32_t LightJudge = 400;
inline constexpr std::int32_t HeavyJudge = 550;
inline constexpr std::int32_t ChargedJudge = 570;
inline constexpr std::int32_t ChargedBlastJudge = 571;
inline constexpr std::int32_t HeadDummy = 12100; // the lit head (where the clips spawn the FFX)
inline constexpr std::int64_t BlastNs = 100'000'000; // event 1 of 571: 0.633 .. 0.733 s
// The charged clip swings with c871 / c872 and booms with c873 at the blast; the generated table
// (core/vr/melee_moves.h) gives its window c873, so that boom came as the swing began.
inline constexpr std::int32_t ChargedSwingSound = 872;

struct Blast {
    std::array<std::int32_t, 2> ffx{-1, -1};
    std::array<std::int32_t, 3> sounds{-1, -1, -1}; // the clip's own (c, SoundType 1)
    std::int32_t blast_judge{-1};                   // a second attack row, or -1
    constexpr bool Valid() const {
        return ffx[0] >= 0;
    }
};

// The explosion of a swing's attack row: none for anything but the lit Boom Hammer's heavy and
// charged, a heavy window the classifier judged light (it hits as the light row) or a transform
// attack.
constexpr Blast Of(std::int32_t variation, bool form_b, std::int32_t judge, bool judged_light,
                   bool transform) {
    if (variation != Variation || !form_b || judged_light || transform) {
        return {};
    }
    if (judge == HeavyJudge) {
        return {{252042, -1}, {877, -1, -1}, -1};
    }
    if (judge == ChargedJudge) {
        return {{252041, 1032002}, {873, 875, 877}, ChargedBlastJudge};
    }
    return {};
}

// The window's own swing sound: the lit charged's swing, not its boom (see ChargedSwingSound).
constexpr std::int32_t SwingSound(std::int32_t variation, bool form_b, std::int32_t judge,
                                  std::int32_t whoosh) {
    return variation == Variation && form_b && judge == ChargedJudge ? ChargedSwingSound : whoosh;
}

// The blast row's behaviour id from the swing's (BehaviorParam_PC id = 1e8 + variation * 1000 +
// judge): the same weapon, its judge swapped; -1 when `bid` is not that judge's.
constexpr std::int32_t BlastBid(std::int32_t bid, std::int32_t judge, std::int32_t blast_judge) {
    if (bid < 100000000 || judge < 0 || blast_judge < 0 || bid % 1000 != judge) {
        return -1;
    }
    return bid - judge + blast_judge;
}

} // namespace VR::BoomBlast
