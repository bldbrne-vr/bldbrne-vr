// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/vr/visceral_channel.h"
#include "core/vr/visceral_thrust.h"
#include "core/vr/melee_window.h"

namespace VR::Melee::Thrust {

struct RetryHold {
    std::uint32_t window{};
    std::int64_t until{};
    bool Holds(std::uint32_t serial, std::int64_t now) const {
        return window != 0 && window == serial && now < until && until - now <= 120'000'000;
    }
};

// A brief reservation of an unresolved forward stab, never the entire stagger.
// Both channels must refer to the same opening. Stale input, a pull-back, a slash,
// a stopped hand or a changed target releases ordinary combat immediately.
inline bool ForwardIntent(const Combat::ThrustState &hand, const Combat::VisceralOpen &target,
                          std::int64_t now) {
    const auto fresh = [now](std::int64_t stamp) {
        return stamp > 0 && now >= stamp && now - stamp < 100'000'000;
    };
    const auto verdict = static_cast<Verdict>(hand.motion_verdict);
    return hand.enabled && hand.forward && hand.motion != 0 && fresh(hand.ns) && fresh(target.ns) &&
           target.open && target.target != nullptr && hand.target == target.target &&
           hand.opportunity == target.serial && hand.motion_age >= 0.0f &&
           hand.motion_age < 0.25f && (verdict == Verdict::Pending || verdict == Verdict::Thrust);
}

// Only defer a new off-hand attack. A held stroke that closes must not become a late hit;
// untouched strokes retain the normal short-window grace, and live attacks keep their identity.
inline LatchStep StepOffhand(LatchState &latch, LatchInput in, std::uint32_t &held, bool intent) {
    const bool reserve =
        intent && in.have && in.open && (!latch.active || latch.serial != in.serial);
    if (reserve)
        held = in.serial;
    if (in.have && in.serial == held && held != 0) {
        in.min_frames = 0;
        in.gates = in.gates && in.open;
    }
    return reserve ? LatchStep::Wait : StepLatch(latch, in);
}

// Only a rejected native check starts this. At most three extra checks over 120 ms,
// for the same moving hand, player and opening. No native pointers are dereferenced.
class ReachRetry {
  public:
    RetryHold hold{};
    void Begin(const void *player, const Combat::ThrustState &hand,
               const Combat::VisceralOpen &target, std::uint32_t window, std::int64_t now) {
        hold = {window, now + 120'000'000};
        owner = player;
        victim = target.target;
        opportunity = target.serial;
        motion = hand.motion;
        thrust = hand.thrust_serial;
        started = last = now;
        attempts = 1;
    }
    bool Keep(const void *player, const Combat::ThrustState &hand,
              const Combat::VisceralOpen &target, std::int64_t now) const {
        if (owner == nullptr || player != owner || target.target != victim ||
            target.serial != opportunity || hand.motion != motion || hand.thrust_serial != thrust ||
            static_cast<Verdict>(hand.motion_verdict) != Verdict::Thrust ||
            !ForwardIntent(hand, target, now) || now < started || now - started >= 120'000'000) {
            return false;
        }
        return true;
    }
    bool Due(std::int64_t now) {
        if (!Active() || now >= hold.until || attempts >= 4 || now < last ||
            now - last < 30'000'000)
            return false;
        last = now;
        ++attempts;
        return true;
    }
    bool Active() const { return owner != nullptr; }
    bool AcceptWindow(std::uint32_t serial, bool open, bool belongs) {
        if (hold.window == 0 && belongs)
            hold.window = serial;
        return hold.window == 0 ? !open : serial == hold.window && open;
    }
    void Reset() { *this = {}; }

  private:
    const void *owner{};
    const void *victim{};
    std::uint32_t opportunity{}, motion{}, thrust{}, attempts{};
    std::int64_t started{}, last{};
};

} // namespace VR::Melee::Thrust
