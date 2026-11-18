// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

namespace VR::Tricks {
// Unknown live rows may mean the parameter table is not ready. Retry at most once
// per 60 eligible weapon updates; cache only a resolved attack or definitive non-attack.
class StakeAttackLookup {
public:
    template <class Lookup>
    std::int32_t Resolve(Lookup&& lookup) {
        if (judge != 0) {
            return judge;
        }
        if (wait != 0) {
            --wait;
            return 0;
        }
        const auto first = lookup(570);
        const auto second = lookup(930);
        if (first == 0) {
            judge = 570;
        } else if (second == 0) {
            judge = 930;
        } else if (first >= 0 && second >= 0) {
            judge = -1;
        } else {
            wait = 59;
        }
        return judge;
    }
private:
    std::int32_t judge{0};
    unsigned wait{0};
};

// The blast kicks the stake out of form B (2026-11-07: "as if the blast itself kicked
// it out of transformed mode", "a nice kick"): the retract starts KickDelayNs after the discharge,
// inside its contact window, at the kick clock (transform.h KickClock), and the hand's recoil and
// buzz slam a second time as it locks (gun_aim.h StakeSlam). VR_STAKE_KICK=0: the earlier calm
// retract after the window (a test A/B only).
inline constexpr std::int64_t KickDelayNs = 10'000'000;
inline bool StakeKickOut() {
    static const bool on = [] {
        const char* v = std::getenv("VR_STAKE_KICK");
        return v == nullptr || v[0] != '0';
    }();
    return on;
}

// An accepted powered discharge returns to form A, hit or miss: kicked out at once (`kick`) or
// after its contact window. Entirely pose-thread owned: recovery follows the firing mechanism,
// not a damage callback.
struct StakeReturn {
    enum class Action { None, Start };
    std::uintptr_t owner{};
    std::int32_t weapon{-1}, slot{-1};
    std::int64_t opened{};
    bool kick{true};
    void Arm(std::uintptr_t who, std::int32_t id, std::int32_t where, std::int64_t at,
             bool kick_out) {
        owner = who; weapon = id; slot = where; opened = at; kick = kick_out;
    }
    Action Update(std::uintptr_t who, std::int32_t id, std::int32_t where, bool form_b,
                  std::int64_t now, bool blocked, bool window_open) {
        if (opened == 0) return Action::None;
        if (who != owner || id != weapon || where != slot || !form_b || now < opened ||
            now - opened > 2'000'000'000) {
            opened = 0;
            return Action::None;
        }
        // The kick: within the blast's window (a powered window never becomes a transform
        // attack). The calm retract preserves OpenPowered's 200 ms impulse, including when its
        // publication closes early.
        if (blocked || (kick ? now - opened < KickDelayNs
                             : window_open || now - opened < 200'000'000))
            return Action::None;
        opened = 0; // exactly one recovery per accepted discharge
        return Action::Start;
    }
};

// Retail 1.09: a116_042630 invokes behavior 101100930 / attack 1100930.
// Its sphere uses body dummy 60. The per-instance attack spec is copied to
// entry+0x54 by 0x185D988; spec+0x134/+0x138 are its dummy ids (0x1F1439D).
// The caller verifies those instruction signatures before using this layout.
inline bool RelocateStakeBlast(std::span<std::uint8_t> entry) {
    if (entry.size() < 0x190) return false;
    std::int32_t behavior{}, first{}, second{};
    float radius{};
    std::memcpy(&behavior, entry.data() + 0x8C, sizeof(behavior));
    std::memcpy(&radius, entry.data() + 0xA0, sizeof(radius));
    std::memcpy(&first, entry.data() + 0x188, sizeof(first));
    std::memcpy(&second, entry.data() + 0x18C, sizeof(second));
    if (behavior != 101100930 || (first != 60 && first != 11206) || second != -1 ||
        !std::isfinite(radius) || std::abs(radius - 1.0f) > 0.02f) return false;
    const std::int32_t tip = 11206;
    std::memcpy(entry.data() + 0x188, &tip, sizeof(tip));
    return true;
}
} // namespace VR::Tricks
