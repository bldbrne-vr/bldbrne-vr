// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include "core/vr/quick_items.h"

namespace VR::SummonBell {
struct Sign { int action, character; };
// Verified 1.09 map events: ActionButton -> 100111 -> 4682 -> SummonNPC.
inline constexpr Sign Signs[] = {
    {10576, 2300740},
    {10568, 2300930},
    {10564, 2300931},
    {10567, 2400910},
    {10575, 2410158},
    {10576, 2410740},
    {10566, 2420910},
    {10561, 2700920},
    {10565, 2700921},
    {10564, 2800910},
    {10568, 2800911},
    {10566, 3200910},
    {10565, 3200911},
    {10561, 3200912},
    {10567, 3400921},
    {10562, 3400922},
    {10563, 3400923},
    {10565, 3400924},
    {10562, 3400925},
    {10563, 3400926},
    {10564, 3500940},
};
inline bool Supported(int action, int character) {
    for (auto s : Signs) if (s.action == action && s.character == character) return true;
    return false;
}
struct State {
    std::uintptr_t condition{};
    int action{}, character{};
    double seen{}, started{}, committed{}, blocked_until{};
    bool pending{}, ready{}, cancel_armed{}, wait_release{};
    Quick::ShakeGate shake;
    void Cancel(double now) { pending = ready = cancel_armed = false; wait_release = true; blocked_until = now + .3; }
    // Called only after the game's own range test; false keeps its condition unsatisfied.
    bool Offer(std::uintptr_t id, int a, int c, bool accepted, bool allowed, double now) {
        if (!Supported(a,c)) return accepted;
        if (!allowed) { if (pending) { Cancel(now); return false; } return accepted; }
        if (pending && (now < seen || now-seen > .3)) Cancel(now);
        if (wait_release || now < blocked_until) return false;
        if (pending) {
            if (condition != id || action != a || character != c) return false;
            seen = now;
            if (ready) { Cancel(now); committed=now; return true; }
            return false;
        }
        if (accepted) {
            condition=id; action=a; character=c; seen=started=now;
            pending=true; ready=false; cancel_armed=false; shake.Reset(now);
        }
        return false;
    }
    Quick::Haptic Input(bool allowed, bool b, bool tracked, double now,
                         Quick::V3 angular, Quick::V3 velocity) {
        if (!b) wait_release = false;
        if (!pending) return Quick::Haptic::None;
        if (!allowed || !tracked || now < seen || now-seen > .3) {
            Cancel(now); return Quick::Haptic::PutBack;
        }
        if (!b) cancel_armed=true;
        if (b && cancel_armed) { Cancel(now); return Quick::Haptic::PutBack; }
        if (ready) return Quick::Haptic::None;
        const auto event=shake.Update({},now,tracked,angular,velocity,true);
        if (event==Quick::ShakeGate::Event::Rung) {
            ready=true; return Quick::Haptic::BellToll;
        }
        return event==Quick::ShakeGate::Event::Stroke ? Quick::Haptic::BellStrike : Quick::Haptic::None;
    }
};
inline std::atomic<bool> enabled{true}, installed{false};
inline std::mutex mutex;
inline State state;
inline double input_time{};
inline bool input_allowed{};
struct Frame { bool held{}, block_b{}; std::int64_t started_ns{}; Quick::Haptic haptic{}; };
inline Frame Update(bool allowed, bool b, bool tracked, double now,
                    Quick::V3 angular, Quick::V3 velocity) {
    std::scoped_lock lock{mutex};
    input_time=now; input_allowed=allowed && enabled.load() && installed.load();
    const bool was=state.pending;
    const auto h=state.Input(input_allowed,b,tracked,now,angular,velocity);
    return {state.pending,was || state.pending || state.wait_release || now<state.blocked_until,
            static_cast<std::int64_t>(state.started*1e9),h};
}
inline bool Active() { std::scoped_lock lock{mutex}; return state.pending; }
inline bool KeepBellInVR(double now, int clip) {
    std::scoped_lock lock{mutex};
    return state.committed>0 && now>=state.committed && now-state.committed<10 &&
           clip>=0 && clip%1000000==100111;
}
}
namespace VR::Bloodborne { void InstallSummonBell(std::uint64_t base, std::uint64_t size); }
