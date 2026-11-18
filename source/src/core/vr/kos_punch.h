// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <span>
#include "core/vr/melee_moves.h"

namespace VR::Melee {
// Without Milkweed, the Parasite's form-B attack rows belong to the LEFT hand.
// Keep the native right-hand punches available in either stance instead of falling
// back to R1 and playing a whole-body animation. Keep the stance itself: it still
// controls the left part, transform, and item occupancy. The Milkweed replacement
// (variation 3800) keeps its own tendril rows.
inline WeaponMoves WithKosPunchRows(WeaponMoves moves) {
    if (moves.variation == 101) {
        for (std::size_t k = 0; k < 3; ++k) {
            moves.rows[3 + k] = moves.rows[k];
            // Form B hides part 0; part 2 is the right fist (wp_a_1712_2,
            // native dummy 100, attached to the same right-hand dummy 304).
            moves.rows[3 + k].dmy1 = 12100;
        }
    }
    return moves;
}

inline bool KosPunchInFormB(std::int32_t variation, bool form_b, std::int32_t judge) {
    return variation == 101 && form_b && (judge == 0 || judge == 150 || judge == 170);
}

// Same per-instance spec layout as RelocateStakeBlast; the caller must verify
// the retail copy/store signatures. Never modify the shared game attack params.
inline bool RelocateKosPunch(std::span<std::uint8_t> entry) {
    if (entry.size() < 0x190) return false;
    std::int32_t bid{}, first{}, second{};
    std::memcpy(&bid, entry.data() + 0x8C, sizeof(bid));
    std::memcpy(&first, entry.data() + 0x188, sizeof(first));
    std::memcpy(&second, entry.data() + 0x18C, sizeof(second));
    if ((bid != 100101000 && bid != 100101150 && bid != 100101170) ||
        (first != 10100 && first != 12100) || second != -1) return false;
    const std::int32_t right_fist = 12100;
    std::memcpy(entry.data() + 0x188, &right_fist, sizeof(right_fist));
    return true;
}

} // namespace VR::Melee
