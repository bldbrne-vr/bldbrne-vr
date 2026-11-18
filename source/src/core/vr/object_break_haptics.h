// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>

namespace VR::Haptics::ObjectBreak {

// Only called for a native destruction, under the melee attack mutex. Keep the last four
// attacks per hand, as the right-hand attack ring does: a late hit from the previous attack
// must not rearm its pulse. Creation time distinguishes reused serials after a session reset.
class Gate {
public:
    bool Admit(unsigned hand, std::uint32_t serial, std::int64_t created_ns) {
        if (hand > 1 || serial == 0 || created_ns <= 0) {
            return false;
        }
        auto& history = history_[hand];
        for (const auto& key : history) {
            if (key.serial == serial && key.created_ns == created_ns) {
                return false;
            }
        }
        history[next_[hand]++ % history.size()] = {serial, created_ns};
        return true;
    }

private:
    struct Key {
        std::uint32_t serial{};
        std::int64_t created_ns{};
    };
    std::array<std::array<Key, 4>, 2> history_{};
    std::array<unsigned, 2> next_{};
};

// Bit 0 left, bit 1 right. A dual-wield left strike stays left; only a shared right-hand
// weapon can feed its supporting hand. Never infer a shared grip from transformed mode.
constexpr unsigned Hands(unsigned hand, bool two_handed, bool support_haptics) {
    return hand == 0 ? 1u : hand == 1 ? (two_handed && support_haptics ? 3u : 2u) : 0u;
}

constexpr bool Fresh(std::int64_t now, std::int64_t at) {
    return at > 0 && now >= at && now - at <= 150'000'000;
}

} // namespace VR::Haptics::ObjectBreak
