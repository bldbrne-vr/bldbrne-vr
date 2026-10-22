// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <limits>
#include "core/vr/lever_prompt.h"
#include "core/vr/physical_lever.h"

namespace VR::Lever {
// The "within reach" tick near a hand-pullable lever: a hand comes in at the grab radius (a
// squeeze there grabs) and goes out 5 cm beyond it, so it does not buzz on the boundary.
inline constexpr float ReachHysteresis = 0.05f;
inline bool InReach(bool was, float distance) {
    return std::isfinite(distance) && distance <= GrabRadius + (was ? ReachHysteresis : 0.0f);
}
// At most one reach tick per hand this often (a swing through the handle's reach).
inline constexpr std::int64_t ReachTickGapNs = 300'000'000;
// A grip squeeze this close to an armed or offered lever's grip or stem is an attempt at the
// lever: only such a squeeze is sent to the game for it, and a miss within it is consumed instead
// of reaching the weapon (right grip = transform). Any other squeeze keeps its ordinary use.
inline constexpr float NearMiss = GrabRadius + 0.10f;

// An armed lever (lever_prompt.h ArmMargin): near while the head or a tracked hand is within the
// handle's distance from the hinge plus the margin (metres, the rig mapped by Space). No timer.
// The handle keeps that distance at every angle, so a parked capstan's bar arms as far out.
inline float ArmRadius(const Rig &rig) { return Body::Length(rig.rest) + ArmMargin; }
// The nearest of the head and the valid hands to the hinge (metres; infinity without one).
inline float ArmDistance(const Rig &rig, Vec3 head, bool head_valid,
                         const std::array<Vec3, 2> &hands, const std::array<bool, 2> &hands_valid) {
    float best = std::numeric_limits<float>::infinity();
    const auto take = [&](bool valid, Vec3 p) {
        if (valid && Body::IsFinite(p))
            best = std::min(best, Body::Length(p - rig.pivot));
    };
    take(head_valid, head);
    take(hands_valid[0], hands[0]);
    take(hands_valid[1], hands[1]);
    return best;
}
inline bool ArmedNear(const Rig &rig, Vec3 head, bool head_valid, const std::array<Vec3, 2> &hands,
                      const std::array<bool, 2> &hands_valid) {
    return Valid(rig) && ArmDistance(rig, head, head_valid, hands, hands_valid) <= ArmRadius(rig);
}

// The game's proximity scan (lever_prompt.h ScanNs). The XR side asks only in gameplay, while a
// hand can pull and no armed lever is near, at most once per ScanNs.
inline bool ScanDue(bool wanted, std::int64_t last_ns, std::int64_t now) {
    return wanted && now > 0 && (last_ns == 0 || now - last_ns >= ScanNs || now < last_ns);
}
// The game side's scan, per bound lever instance it finds (its rig, `head` and `hands` in
// metres): how near the head or a tracked hand is to its hinge when within its arm radius,
// else infinity. The nearest such lever is armed.
inline float ScanReach(const Rig &rig, Vec3 head, const std::array<Vec3, 2> &hands,
                       const std::array<bool, 2> &tracked) {
    if (!Valid(rig))
        return std::numeric_limits<float>::infinity();
    const float d = ArmDistance(rig, head, true, hands, tracked);
    return d <= ArmRadius(rig) ? d : std::numeric_limits<float>::infinity();
}

// The XR side's view of the armed lever each frame (pure; the presenter maps the game's word,
// lever_channel.h Nearby, into metres): the decision behind arming, the game's re-check and the
// reach tick, kept out of the presenter so it can be tested.
struct ArmInput {
    std::int64_t now{};
    bool published{};            // the game published an armed lever and its rig mapped
    std::int64_t published_ns{}; // when the game last said so
    bool armed{};                // its word: grabbable now (in place, selectable, at rest)
    Rig rig{};                   // metres
    float angle{};               // where its handle stands (a parked capstan's bar: not at rest)
    bool gameplay{};             // immersive first-person gameplay, no menu or cinematic
    Vec3 head{};
    std::array<Vec3, 2> hands{};
    std::array<bool, 2> tracked{};
    // A squeeze by this hand could start a pull now (Acquisition::Begin's rules): not busy, not
    // blocked, its grip released.
    std::array<bool, 2> free{};
};
// Kept across frames; the presenter does not step it while a squeeze is pending or a lever held,
// so a hand still on the handle afterwards does not tick again.
struct ArmState {
    std::int64_t check_ns{};
    std::array<bool, 2> in_reach{};
    std::array<std::int64_t, 2> tick_ns{};
};
struct ArmOutput {
    bool near{};          // within the arm radius of the hinge
    bool request_check{}; // ask the game whether it is still armed (at most once per ArmCheckNs)
    bool armed_near{};    // armed by a recent word of the game, near, in gameplay
    std::array<float, 2> distance{std::numeric_limits<float>::infinity(),
                                  std::numeric_limits<float>::infinity()}; // to the grip or stem
    std::array<bool, 2> reaching{}; // within NearMiss: a squeeze by this hand is for the lever
    std::array<bool, 2> tick{};     // a light "within reach" tick for this hand now
};
inline ArmOutput Arm(ArmState &state, const ArmInput &in) {
    ArmOutput out{};
    if (!in.published || !Valid(in.rig)) {
        state.in_reach = {};
        return out;
    }
    out.near = ArmedNear(in.rig, in.head, true, in.hands, in.tracked);
    if (!out.near) {
        state.in_reach = {}; // walked away: nothing to do until it is near again
        return out;
    }
    if (in.now - in.published_ns > ArmCheckNs &&
        (state.check_ns == 0 || in.now - state.check_ns > ArmCheckNs)) {
        state.check_ns = in.now;
        out.request_check = true;
    }
    // Armed only on the game's recent word: a lever left long ago, or one whose area has gone,
    // is confirmed or disarmed before it is grabbable or ticks. (A word published just after
    // `now` was taken counts as recent.)
    const bool confirmed = in.published_ns > 0 && in.now - in.published_ns <= ArmConfirmNs;
    if (!in.armed || !confirmed || !in.gameplay) {
        state.in_reach = {};
        return out;
    }
    out.armed_near = true;
    for (int h = 0; h < 2; ++h) {
        if (!in.tracked[h] || !Body::IsFinite(in.hands[h])) {
            state.in_reach[h] = false;
            continue;
        }
        out.distance[h] = Body::Length(in.hands[h] - ClosestGrip(in.rig, in.angle, in.hands[h]));
        out.reaching[h] = out.distance[h] <= NearMiss;
        const bool in_reach = in.free[h] && InReach(state.in_reach[h], out.distance[h]);
        if (in_reach && !state.in_reach[h] &&
            (state.tick_ns[h] == 0 || in.now - state.tick_ns[h] >= ReachTickGapNs)) {
            out.tick[h] = true;
            state.tick_ns[h] = in.now;
        }
        state.in_reach[h] = in_reach;
    }
    return out;
}

class Acquisition {
public:
    std::int64_t Stamp() const { return ns; }
    bool Begin(const Tracker &tracker, Input input, std::uint64_t epoch, std::int64_t now) {
        if (ns || now <= 0 || !input.enabled || !input.gameplay || input.fallback ||
            !std::isfinite(input.dt) || input.dt <= 0 || input.dt > MaxFrame)
            return false;
        auto edges = tracker.GrabEdges(input.hands);
        for (int h = 0; h < 2; ++h) {
            const auto &hand = input.hands[h];
            edges[h] = edges[h] && hand.tracked && !hand.busy && !hand.blocked &&
                       Body::IsFinite(hand.position);
            input.hands[h].blocked = input.hands[h].blocked || !edges[h];
        }
        if (!edges[0] && !edges[1])
            return false;
        pressed = input;
        mask = edges;
        rejected = {};
        space = epoch;
        ns = now;
        return true;
    }
    Output Poll(Tracker &tracker, Input current, std::uint64_t epoch, std::int64_t now,
                bool replied) {
        const bool fresh = now >= ns && now - ns <= 100'000'000;
        bool live = false;
        for (int h = 0; h < 2; ++h) {
            auto &hand = current.hands[h];
            rejected[h] = rejected[h] || !hand.tracked || !hand.grip_valid ||
                          !std::isfinite(hand.grip) || hand.grip <= Release ||
                          hand.busy || hand.blocked || !Body::IsFinite(hand.position);
            hand.blocked = hand.blocked || rejected[h];
            live |= mask[h] && !rejected[h];
        }
        const bool context = ns && live && epoch == space && current.enabled &&
                             current.gameplay && !current.fallback;
        const bool valid = context && fresh;
        if (valid && !replied) {
            Output waiting{};
            waiting.owns_grip = mask;
            return waiting;
        }
        // Which squeezing hands reach for the lever the game offered, measured before an expired
        // request loses its candidate.
        const auto near = Near(current, replied);
        if (!valid || !replied)
            current.candidate.eligible = false;
        // Include movement since the captured press, rather than the last XR frame only.
        current.dt = fresh ? std::max(current.dt, static_cast<float>(now - ns) * 1e-9f)
                           : current.dt;
        // A verified miss returns the squeeze to ordinary controls, and so does a request the
        // game did not answer in time. Only a hand reaching for an offered lever (within NearMiss
        // of its grip) keeps its squeeze: the right grip would otherwise transform the weapon at
        // every attempt. Context and tracking failures (release, recenter, B, busy) consume it.
        std::array<bool, 2> pass{};
        for (int h = 0; h < 2; ++h)
            pass[h] = context && !near[h];
        auto out = tracker.ResolveGrab(pressed, current, pass);
        ns = 0;
        return out;
    }
    // The squeezing hands' nearest distance to the candidate's grip (metres; infinity without one).
    float Distance(const Input &current) const {
        float best = std::numeric_limits<float>::infinity();
        if (!Valid(current.candidate.rig))
            return best;
        for (int h = 0; h < 2; ++h) {
            const auto &p = current.hands[h].position;
            if (!mask[h] || !Body::IsFinite(p))
                continue;
            best = std::min(best, Body::Length(p - ClosestGrip(current.candidate.rig,
                                                               current.candidate.angle, p)));
        }
        return best;
    }
    // Per squeezing hand: within NearMiss of the grip the game offered for this request. Only an
    // eligible reply (the game accepted this lever) keeps a missed squeeze, and only that hand's.
    std::array<bool, 2> Near(const Input &current, bool replied) const {
        std::array<bool, 2> near{};
        if (!replied || !current.candidate.eligible || !Valid(current.candidate.rig))
            return near;
        for (int h = 0; h < 2; ++h) {
            const auto &p = current.hands[h].position;
            near[h] = mask[h] && !rejected[h] && Body::IsFinite(p) &&
                      Body::Length(p - ClosestGrip(current.candidate.rig, current.candidate.angle,
                                                   p)) <= NearMiss;
        }
        return near;
    }
private:
    std::int64_t ns{};
    std::uint64_t space{};
    Input pressed{};
    std::array<bool, 2> mask{}, rejected{};
};
} // namespace VR::Lever
