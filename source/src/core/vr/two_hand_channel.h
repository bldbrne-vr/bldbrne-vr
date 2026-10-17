// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <mutex>

#include "core/vr/two_hand_grip.h"

// The two-handed grip's game side (core/vr/two_hand_grip.h) for the presenter: the right weapon,
// its arm style and the game's own two-handed grip (L_Weapon in R_Weapon's frame, from the game's
// animation), read by the VR body at every fresh pose copy (bloodborne_body.cpp), with the right
// hand's grip (the weapon bone on its controller) the body uses. The presenter reads it every
// input frame. Mutex (small struct, one writer).
namespace VR::TwoHand {

struct GameStatus {
    std::int64_t ns{0};       // steady clock of the reading (0: never)
    std::int32_t family{-1};  // the right weapon's family (EquipParamWeapon id / 100000)
    std::int32_t style{-1};   // its arm style (3: form B two-handed)
    bool two_handed{false};   // TwoHandedForm(family, style)
    bool measured{false};     // `grip` is the game's (else DefaultGrip)
    GameGrip grip{DefaultGrip};
    BoneOnGrip right{};       // the right weapon bone on its controller (DefaultGrip + trims)
    BoneOnGrip left{};        // ... the left one
    float scale{1.0f};        // world_scale (game units per metre)
    std::uint32_t readings{0}; // plausible readings of the game's grip for this weapon
};

namespace Detail {
inline std::mutex& StatusMutex() {
    static std::mutex m;
    return m;
}
inline GameStatus& StatusSlot() {
    static GameStatus s;
    return s;
}
} // namespace Detail

inline void PublishGameStatus(const GameStatus& status) {
    std::scoped_lock lock{Detail::StatusMutex()};
    Detail::StatusSlot() = status;
}

// False before the first reading.
inline bool LatestGameStatus(GameStatus& out) {
    std::scoped_lock lock{Detail::StatusMutex()};
    out = Detail::StatusSlot();
    return out.ns != 0;
}

} // namespace VR::TwoHand
