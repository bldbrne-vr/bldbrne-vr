// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

#include "core/vr/arm_ik.h"

// Pure continuous lever interaction. No game writes, OpenXR or synthetic button presses.
// Coordinates must share one world frame, in metres. The adapter must supply a measured hinge,
// a fresh game-approved lever offer, and a space epoch changed on recenter/teleport. Animation ID
// alone does not establish lever eligibility. The existing lever bone follows the hand-driven
// hinge, with the fist on that same handle. No authored pull animation drives the hold.
// The development game adapter owns permission, pose publication and gameplay events.
namespace VR::Lever {

using Body::Vec3;

constexpr float GrabRadius = 0.22f; // forgiving reach; the held fist pins to the actual handle
constexpr float SlipDistance = 0.30f;
constexpr float MaxAge = 0.15f;
constexpr float MaxFrame = 0.10f;
constexpr float Press = 0.75f;
constexpr float Release = 0.45f;
// A held hand moving faster than this between two frames jumped (a tracking glitch): the hold
// ends (Discontinuity). Metres per second, plus 1 cm of noise per frame.
constexpr float MaxHandSpeed = 6.0f;
// A capstan is walked round, with the stick too: the hunter's own motion steps the hand in the
// game's frame once per game frame (30 Hz), and a snap turn swings it about the head (0.26 m at
// 0.5 m from the head for the default 30 degrees). Its jump check measures the tracked hand alone
// (Hand::tracking); in the game's frame a held hand may step up to this far in one frame (a warp
// or a reload of the hunter ends the hold). Metres.
constexpr float CapstanMaxStep = 1.0f;
// While a capstan is held, a squeeze by the other hand this close to its bar is the lever's too
// (the game's own turn has both hands on the bar): it never reaches the game, so it cannot
// transform the weapon or open the item arc (which would end the hold). Metres, as a reach.
constexpr float SecondHandReach = GrabRadius + 0.10f;

// How the hand drives the bone. Pull: a lever stick; the fist keeps where it caught the handle and
// the stick turns with it. Capstan: a long bar turned about a vertical hub (motion 101230, the
// Research Hall's o353010): the fist slides freely along the bar (only leaving it slips), the bar's
// angle follows the hand's angle about the hub, and a bar let go before its stop stays there
// (the game adapter parks it; nothing turns it back).
enum class Motion : std::uint8_t { Pull, Capstan };

struct Rig {
    Vec3 pivot{};
    Vec3 axis{};   // unit hinge axis; sweep's sign chooses the pull direction
    Vec3 rest{};   // pivot -> handle at angle zero (may include an axial offset)
    float sweep{}; // signed radians from rest to the completion stop, less than pi
    Vec3 shaft_start{}; // pivot -> bottom of grabbable shaft, at angle zero
    bool shaft{false};
    Motion motion{Motion::Pull};
};

inline Vec3 Rotate(Vec3 v, Vec3 axis, float angle) {
    return v * std::cos(angle) + Body::Cross(axis, v) * std::sin(angle) +
           axis * (Body::Dot(axis, v) * (1.0f - std::cos(angle)));
}

inline Vec3 HandleAt(const Rig &rig, float angle) {
    return rig.pivot + Rotate(rig.rest, rig.axis, angle);
}

// Project onto the moving shaft only when acquiring; retain that contact for the hold.
inline Vec3 ClosestGrip(const Rig &rig, float angle, Vec3 hand) {
    const auto top = HandleAt(rig, angle);
    if (!rig.shaft) return top;
    const auto bottom = rig.pivot + Rotate(rig.shaft_start, rig.axis, angle);
    const auto segment = top - bottom;
    const float length2 = Body::Dot(segment, segment);
    if (length2 < 1e-8f) return top;
    return bottom + segment * std::clamp(Body::Dot(hand - bottom, segment) / length2, 0.0f, 1.0f);
}

inline Vec3 Radial(Vec3 v, Vec3 axis) { return v - axis * Body::Dot(v, axis); }

// The rig with its lengths scaled (game units to metres and back); axis and sweep unchanged.
inline Rig Scaled(Rig rig, float factor) {
    rig.pivot = rig.pivot * factor;
    rig.rest = rig.rest * factor;
    rig.shaft_start = rig.shaft_start * factor;
    return rig;
}

// The largest handle radius (and pivot -> shaft start distance) a rig may have, metres: a lever
// stick is at most about 1.2 m long; a capstan's bar reaches 2.9 m out from its hub (o353010).
inline constexpr float MaxReach(Motion motion) { return motion == Motion::Capstan ? 5.0f : 2.0f; }

inline bool Valid(const Rig &rig) {
    if (!Body::IsFinite(rig.pivot) || !Body::IsFinite(rig.axis) || !Body::IsFinite(rig.rest) ||
        !std::isfinite(rig.sweep) ||
        (rig.motion != Motion::Pull && rig.motion != Motion::Capstan)) {
        return false;
    }
    const float reach = MaxReach(rig.motion);
    if (rig.shaft && (!Body::IsFinite(rig.shaft_start) ||
        Body::Length(Radial(rig.shaft_start, rig.axis)) < 0.05f ||
        Body::Length(rig.shaft_start) > reach)) return false;
    const float radius = Body::Length(Radial(rig.rest, rig.axis));
    return std::abs(Body::Length(rig.axis) - 1.0f) < 0.001f && radius >= 0.05f && radius <= reach &&
           std::abs(rig.sweep) >= 0.05f && std::abs(rig.sweep) < Body::Pi;
}

// Physical binding to the existing object's lever bone. These transforms and the handle
// point come from the loaded model; no clip, timer or authored keyframes drive the hold.
// parent_world includes the map object's transform and the lever bone's parent chain.
struct BoneBinding {
    Body::QsTransform parent_world{};
    Body::QsTransform rest_local{};
    Vec3 axis_local{};
    Vec3 handle_local{};
    float sweep{};
    Vec3 shaft_start_local{};
    bool shaft{false};
    Motion motion{Motion::Pull};
};

inline std::optional<Rig> BindRig(const BoneBinding &binding) {
    const auto turned = [](const Body::QsTransform &pose) {
        return Body::IsFinite(pose) && std::abs(Body::Dot(pose.q, pose.q) - 1.0f) < 0.001f;
    };
    const auto unit = [](Vec3 s) { return Body::Length(s - Vec3{1, 1, 1}) < 0.001f; };
    // A lever bone's own constant scale (Cainhurst's ~(0.81, 0.46, 0.81)) is applied to its rigid
    // mesh before the bone turns (the game's bone matrix is T * R(q) * diag(s)): the hinge stays a
    // circle and the grip is the scaled vertex, turned. A scaled parent would scale the turn
    // itself (an ellipse): unsupported, the lever keeps its normal interaction.
    const auto own = [](Vec3 s) {
        return s.x >= 0.05f && s.y >= 0.05f && s.z >= 0.05f && s.x <= 20.0f && s.y <= 20.0f &&
               s.z <= 20.0f;
    };
    if (!turned(binding.parent_world) || !unit(binding.parent_world.s) ||
        !turned(binding.rest_local) || !own(binding.rest_local.s) ||
        !Body::IsFinite(binding.handle_local) || !Body::IsFinite(binding.axis_local) ||
        std::abs(Body::Length(binding.axis_local) - 1.0f) >= 0.001f) {
        return std::nullopt;
    }
    const auto bone_world = Body::Compose(binding.parent_world, binding.rest_local);
    // A bone-local mesh vertex as drawn, relative to the hinge (the bone's own scale first).
    const auto drawn = [&](Vec3 v) { return Body::Rotate(bone_world.q, Body::Mul(bone_world.s, v)); };
    Rig rig{bone_world.t, Body::Rotate(bone_world.q, binding.axis_local),
            drawn(binding.handle_local), binding.sweep, drawn(binding.shaft_start_local),
            binding.shaft, binding.motion};
    return Valid(rig) ? std::optional<Rig>{rig} : std::nullopt;
}

inline std::optional<Body::QsTransform> BoneTarget(const BoneBinding &binding, float angle) {
    if (!BindRig(binding) || !std::isfinite(angle)) {
        return std::nullopt;
    }
    const float bounded =
        std::clamp(angle, std::min(0.0f, binding.sweep), std::max(0.0f, binding.sweep));
    auto target = binding.rest_local;
    target.q = Body::Normalize(target.q * Body::AxisAngle(binding.axis_local, bounded));
    return target;
}

// Brief passive motion after an intentional release. No clip or worker remains active
// once the endpoint is reached. The adapter retains pose ownership during settling.
constexpr float CommitProgress = 0.60f;
// A capstan completes at its stop, or let go within its last 3% (2.7 degrees of o353010's 90.3):
// short of that it stays where it was let go (parked), never carried on or turned back.
constexpr float CapstanCommitProgress = 0.97f;
constexpr float CommitFor(Motion motion) {
    return motion == Motion::Capstan ? CapstanCommitProgress : CommitProgress;
}
class Settle {
public:
    void Begin(float start, float sweep, bool commit) { BeginTo(start, commit ? sweep : 0.0f); }
    // From `start` to any angle (a lever whose own clip leaves it partway: lever_models.h end_deg).
    void BeginTo(float start, float target) {
        start_ = start;
        target_ = target;
        elapsed_ = 0;
        angle_ = start;
    }
    float Step(float dt) {
        if (!std::isfinite(dt) || dt <= 0) return angle_;
        elapsed_ = std::min(0.4f, elapsed_ + std::min(dt, 0.05f));
        const float t = elapsed_ / 0.4f;
        angle_ = start_ + (target_ - start_) * (t * t * (3.0f - 2.0f * t));
        return angle_;
    }
    bool Done() const { return elapsed_ >= 0.4f; }
    float Angle() const { return angle_; }
private:
    float start_{}, target_{}, elapsed_{}, angle_{};
};

struct Candidate {
    bool eligible{false};       // verified Operate Device lever accepted by the game at this moment
    std::uint64_t id{};         // stable object identity, never dereferenced by this tracker
    std::uint64_t generation{}; // new eligibility episode / object incarnation, NOT each frame
    std::uint64_t space_epoch{};
    float age_s{}; // monotonic time since the game's sample, not time since the last read
    Rig rig{};
    float angle{}; // current visible angle when grabbing, within the measured stops
};

inline bool Ready(const Candidate &c) {
    return c.eligible && c.id != 0 && std::isfinite(c.age_s) && c.age_s >= 0 && c.age_s <= MaxAge &&
           Valid(c.rig) && std::isfinite(c.angle) && c.angle / c.rig.sweep >= 0 &&
           c.angle / c.rig.sweep <= 1;
}

struct Hand {
    bool tracked{false};
    bool grip_valid{false}; // an absent action sample is not a release
    bool busy{false};
    bool blocked{false}; // held through a context switch or owned by another interaction
    float grip{};
    Vec3 position{}; // the game's frame, metres (the hunter's motion and turning included)
    // The same hand in the headset's tracking space, metres: its real motion alone (no locomotion,
    // no turn). A capstan's jump check reads it (CapstanMaxStep).
    Vec3 tracking{};
};

struct Input {
    bool enabled{false};
    bool gameplay{false};
    bool fallback{false}; // B takes the existing interaction path; cancel the physical hold
    float dt{};
    Candidate candidate{};
    std::array<Hand, 2> hands{}; // left, right
};

enum class End : std::uint8_t {
    None,
    Released,
    Slipped,
    Context,
    CandidateLost,
    Tracking,
    Busy,
    Discontinuity,
};

// A hold that ends by letting go or by the hand slipping off the handle's arc settles like a
// release: past CommitFor (60% for a lever stick) it carries on and completes, short of it it
// springs back (a capstan parks). The other ends (lost tracking or context, another use of the
// hand, a jump) drop it back without completing.
constexpr bool SettlesOnEnd(End end) { return end == End::Released || end == End::Slipped; }

// Why a hold ended, for the one log line each hold's end gets.
inline const char *EndName(End end) {
    switch (end) {
    case End::None: return "none";
    case End::Released: return "released (grip let go)";
    case End::Slipped: return "slipped (the hand left the handle's arc)";
    case End::Context: return "context (gameplay or input paused)";
    case End::CandidateLost: return "candidate lost (the game's offer went stale or changed)";
    case End::Tracking: return "tracking (the hand lost)";
    case End::Busy: return "busy (another use took the hand)";
    case End::Discontinuity: return "discontinuity (the hand jumped)";
    }
    return "?";
}

// Where a held hand is against the hinge, for the hold's progress log: its distance from the
// hinge axis (in the rotation plane), how far it is from where the held contact would be at the
// current angle (Slipped beyond SlipDistance), and its motion `moved` split along the pull's arc
// (positive: the pull's own direction), radially (out from the axis) and along the axis.
struct HoldGeometry {
    float plane{};      // metres from the hinge axis
    float off_arc{};    // metres from the expected contact
    float tangential{}; // metres along the pull (signed by the sweep)
    float radial{};
    float axial{};
};
inline HoldGeometry Geometry(const Rig &rig, Vec3 hand, Vec3 expected, Vec3 moved) {
    HoldGeometry g{};
    const Vec3 radial = Radial(hand - rig.pivot, rig.axis);
    g.plane = Body::Length(radial);
    g.off_arc = Body::Length(hand - expected);
    g.axial = Body::Dot(moved, rig.axis);
    if (g.plane < 1e-4f)
        return g;
    const Vec3 out = radial * (1.0f / g.plane);
    g.radial = Body::Dot(moved, out);
    g.tangential = Body::Dot(moved, Body::Cross(rig.axis, out)) * (rig.sweep < 0 ? -1.0f : 1.0f);
    return g;
}

struct Output {
    bool available{false};
    bool grabbed{false}; // one-frame knock
    bool holding{false};
    bool completed{false}; // one request; adapter must revalidate identity before accepting
    bool ratchet{false};   // new furthest detent, never repeat while hovering/reversing
    std::array<bool, 2> owns_grip{};
    int hand{-1};
    End ended{End::None};
    std::uint64_t id{};
    std::uint64_t generation{};
    float angle{};    // continuous visual command, also retained on release for adapter recovery
    float progress{}; // 0..1 along the measured swing
    float strain{};   // 0..1 sustain, always zero after completion/cancellation
    Vec3 pin{};       // moving contact point for the held fist
    Vec3 contact_rest{}; // frozen pivot-relative contact at angle zero
};

// One input-thread owner. Do not reconstruct/reset during menus or tracking loss: grip ownership
// must survive until a valid release. Game adapter owns release animation and event recovery;
// this class never rewinds gameplay events or autonomously finishes an abandoned pull.
class Tracker {
public:
    float Angle() const { return angle; }
    // While holding: where the hand that caught the handle would be at the current angle (the
    // slip check's reference; a capstan's: its contact on the bar, which slides with the hand),
    // and the held rig. For the hold's progress log only.
    Vec3 Expected() const {
        if (held.rig.motion == Motion::Capstan)
            return held.rig.pivot + Rotate(contact_rest, held.rig.axis, angle);
        return held.rig.pivot + Rotate(grab_vector, held.rig.axis, angle - held.angle);
    }
    const Rig &HeldRig() const { return held.rig; }

    // Read-only acquisition gate. Step still consumes every measured grip edge, even
    // without a candidate, so squeezing elsewhere cannot grab on approaching a lever.
    bool NeedsCandidate(const std::array<Hand, 2> &hands) const {
        if (active >= 0)
            return true;
        for (int h = 0; h < 2; ++h) {
            if (!down[h] && !owned[h] && hands[h].grip_valid &&
                std::isfinite(hands[h].grip) && hands[h].grip >= Press)
                return true;
        }
        return false;
    }

    // Resolve a game-thread reply against BOTH the original squeeze and the current
    // hand. Never manufacture an edge at reply time or grab after leaving the handle.
    // `pass_miss`, per hand: a verified miss by this hand returns its squeeze.
    Output ResolveGrab(Input pressed, const Input &current, std::array<bool, 2> pass_miss = {}) {
        auto edges = GrabEdges(pressed.hands);
        for (int h = 0; h < 2; ++h)
            edges[h] = edges[h] && pressed.hands[h].tracked &&
                       !pressed.hands[h].busy && !pressed.hands[h].blocked;
        pressed.candidate = current.candidate;
        pressed.enabled = pressed.enabled && current.enabled;
        pressed.gameplay = pressed.gameplay && current.gameplay;
        pressed.fallback = pressed.fallback || current.fallback;
        for (int h = 0; h < 2; ++h) {
            const auto &now = current.hands[h];
            pressed.hands[h].blocked = pressed.hands[h].blocked || !edges[h] ||
                !now.tracked || !now.grip_valid || !std::isfinite(now.grip) ||
                now.grip <= Release || now.busy || now.blocked ||
                !Body::IsFinite(now.position) ||
                Body::Length(now.position - ClosestGrip(current.candidate.rig,
                                                    current.candidate.angle, now.position)) > GrabRadius;
        }
        const auto first = Step(pressed);
        // A verified miss returns the live squeeze to ordinary controls. Context/
        // tracking failures still consume it; an actual grab owns it until release.
        for (int h = 0; h < 2; ++h)
            owned[h] = owned[h] || (edges[h] && !pass_miss[h]);
        auto out = Step(current);
        out.grabbed = first.grabbed && (out.holding || out.completed);
        return out;
    }

    // This hand's grip is released and its own (not owned): its next squeeze is a grab edge.
    bool CanSqueeze(int hand) const { return hand >= 0 && hand < 2 && !down[hand] && !owned[hand]; }

    std::array<bool, 2> GrabEdges(const std::array<Hand, 2> &hands) const {
        std::array<bool, 2> edges{};
        for (int h = 0; h < 2; ++h)
            edges[h] = !down[h] && !owned[h] && hands[h].grip_valid &&
                       std::isfinite(hands[h].grip) && hands[h].grip >= Press;
        return edges;
    }

    Output Step(const Input &in) {
        Output out{};
        std::array<bool, 2> edge{};
        for (int h = 0; h < 2; ++h) {
            const auto &hand = in.hands[h];
            if (!hand.grip_valid || !std::isfinite(hand.grip)) {
                down[h] = true; // no new squeeze on recovering a held controller
            } else if (hand.grip <= Release) {
                down[h] = false;
                owned[h] = false;
            } else if (hand.grip >= Press && !down[h]) {
                down[h] = true;
                edge[h] = true;
            }
        }
        const bool context = in.enabled && in.gameplay && !in.fallback && std::isfinite(in.dt) &&
                             in.dt > 0 && in.dt <= MaxFrame;
        const auto &c = in.candidate;
        const bool consumed = c.id == completed_id && c.generation == completed_generation;
        out.available = context && Ready(c) && !consumed;

        if (active >= 0) {
            const auto &hand = in.hands[active];
            // A capstan is pushed with both hands: the other hand's fresh squeeze on the held bar
            // is the lever's (owned until it lets go, never the game's); it does not hold.
            if (held.rig.motion == Motion::Capstan) {
                const int other = 1 - active;
                const auto &second = in.hands[other];
                if (edge[other] && !owned[other] && second.tracked && !second.blocked &&
                    Body::IsFinite(second.position) &&
                    Body::Length(second.position - ClosestGrip(held.rig, angle, second.position)) <=
                        SecondHandReach)
                    owned[other] = true;
            }
            End why = End::None;
            if (!context) {
                why = End::Context;
            } else if (!out.available || c.id != held.id || c.generation != held.generation ||
                       c.space_epoch != held.space_epoch || !SameRig(c.rig, held.rig)) {
                why = End::CandidateLost;
            } else if (!hand.tracked || !hand.grip_valid || !std::isfinite(hand.grip) ||
                       !Body::IsFinite(hand.position)) {
                why = End::Tracking;
            } else if (hand.busy || hand.blocked) {
                why = End::Busy;
            } else if (!down[active]) {
                why = End::Released;
            } else if (Jumped(hand, in.dt)) {
                why = End::Discontinuity;
            }
            if (why == End::None) {
                const Vec3 radial = Radial(hand.position - held.rig.pivot, held.rig.axis);
                if (Body::Length(radial) < 0.03f) {
                    why = End::Slipped; // the hinge's singularity has no usable angle
                } else {
                    const float delta =
                        std::atan2(Body::Dot(held.rig.axis, Body::Cross(previous_radial, radial)),
                                   Body::Dot(previous_radial, radial));
                    const float next = std::clamp(angle + delta, std::min(0.0f, held.rig.sweep),
                                                  std::max(0.0f, held.rig.sweep));
                    // Preserve where the real hand caught the handle, including the grab offset.
                    // A capstan's bar is long: the hand may slide along it (toward or away from
                    // the hub) and only leaving the bar slips; the contact slides with it.
                    const bool slide = held.rig.motion == Motion::Capstan;
                    const Vec3 expected =
                        slide ? ClosestGrip(held.rig, next, hand.position)
                              : held.rig.pivot + Rotate(grab_vector, held.rig.axis, next - held.angle);
                    if (Body::Length(hand.position - expected) > SlipDistance) {
                        why = End::Slipped;
                    } else {
                        angle = next;
                        if (slide)
                            contact_rest = Rotate(expected - held.rig.pivot, held.rig.axis, -angle);
                        previous = hand.position;
                        previous_tracking = hand.tracking;
                        previous_radial = radial;
                        const float progress = angle / held.rig.sweep;
                        const int detent = static_cast<int>(progress * 8.0f);
                        out.ratchet = detent > furthest_detent;
                        furthest_detent = std::max(furthest_detent, detent);
                        if (progress >= 1.0f - 1e-6f) {
                            angle = held.rig.sweep;
                            out.completed = true;
                            out.ratchet = false; // the completion clunk replaces the last click
                            completed_id = held.id;
                            completed_generation = held.generation;
                        }
                    }
                }
            }
            Fill(out);
            out.ended = why;
            if (why != End::None || out.completed) {
                active = -1;
            }
        } else if (out.available) {
            int chosen = -1;
            float nearest = GrabRadius;
            for (int h = 0; h < 2; ++h) {
                const auto &hand = in.hands[h];
                if (!edge[h] || owned[h] || !hand.tracked || hand.busy || hand.blocked ||
                    !Body::IsFinite(hand.position)) {
                    continue;
                }
                const float distance = Body::Length(hand.position - ClosestGrip(c.rig, c.angle, hand.position));
                if (distance <= nearest &&
                    Body::Length(Radial(hand.position - c.rig.pivot, c.rig.axis)) >= 0.03f) {
                    nearest = distance;
                    chosen = h;
                }
            }
            if (chosen >= 0) {
                active = chosen;
                held = c;
                angle = c.angle;
                previous = in.hands[active].position;
                previous_tracking = in.hands[active].tracking;
                contact_rest = Rotate(ClosestGrip(c.rig, c.angle, previous) - c.rig.pivot,
                                      c.rig.axis, -c.angle);
                grab_vector = previous - c.rig.pivot;
                previous_radial = Radial(grab_vector, c.rig.axis);
                furthest_detent = static_cast<int>(angle / held.rig.sweep * 8.0f);
                owned[active] = true;
                out.grabbed = true;
                Fill(out);
            }
        }
        out.holding = active >= 0;
        out.strain = out.holding ? out.progress : 0.0f;
        out.owns_grip = owned;
        return out;
    }

private:
    // The held hand moved faster than a hand can since the last frame: in the game's frame for a
    // lever stick (its hunter stands still); a capstan's in the tracking space (its hunter walks it
    // round, with the stick too, and may snap turn), with the game-frame step only bounded.
    bool Jumped(const Hand &hand, float dt) const {
        const float limit = MaxHandSpeed * dt + 0.01f;
        if (held.rig.motion != Motion::Capstan)
            return Body::Length(hand.position - previous) > limit;
        return !Body::IsFinite(hand.tracking) ||
               Body::Length(hand.tracking - previous_tracking) > limit ||
               Body::Length(hand.position - previous) > CapstanMaxStep;
    }
    static bool SameRig(const Rig &a, const Rig &b) {
        return Body::Length(a.pivot - b.pivot) < 0.001f && Body::Length(a.axis - b.axis) < 0.001f &&
               Body::Length(a.rest - b.rest) < 0.001f && std::abs(a.sweep - b.sweep) < 0.001f &&
               a.shaft == b.shaft && a.motion == b.motion &&
               (!a.shaft || Body::Length(a.shaft_start - b.shaft_start) < 0.001f);
    }

    void Fill(Output &out) const {
        out.hand = active;
        out.id = held.id;
        out.generation = held.generation;
        out.angle = angle;
        out.progress = std::clamp(angle / held.rig.sweep, 0.0f, 1.0f);
        out.contact_rest = contact_rest;
        out.pin = held.rig.pivot + Rotate(contact_rest, held.rig.axis, angle);
    }

    std::array<bool, 2> down{true, true}; // startup must observe an actual release
    std::array<bool, 2> owned{};
    int active{-1};
    Candidate held{};
    Vec3 previous{};
    Vec3 previous_tracking{}; // the held hand's tracking-space position (Hand::tracking)
    Vec3 previous_radial{};
    Vec3 grab_vector{};
    Vec3 contact_rest{};
    float angle{};
    int furthest_detent{};
    std::uint64_t completed_id{};
    std::uint64_t completed_generation{};
};

} // namespace VR::Lever
