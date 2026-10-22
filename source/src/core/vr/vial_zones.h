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
#include <cstddef>
#include <cstdint>

#include "core/vr/body_frame.h"
#include "core/vr/body_solver.h"
#include "core/vr/c0000_skeleton.h"

// The Blood Vial's stab zones (the armed vial, X or the belt, and quick_use_stab's bloods): where
// the vial hand (the left grip) may go in, and when going in is a stab. The tester's 2026-10-08 request:
// "I want it to act even if ... I am the main body of the player or even the fore arm and the right
// wrist (so make it easier to use)". Until then a stab had to land within 18 cm of the right
// controller's grip (the palm) or of a vertical line 5 cm in front of the eyes (it moved with the
// head), and a zone only counted once the hand had been 3 cm outside it since arming.
//
// Zones (LOCAL space, metres; your height's body scale k = height / 167 cm, 1 without one; the
// reach margins times vial_zone_scale):
//  - The right arm, from the controller: the hand (the grip point to the wrist, the hunter's own
//    wrist on the controller: HunterArmInGrip), the forearm (the wrist to the elbow) and the upper
//    arm (the elbow to the shoulder), capsules of the skin's radius plus ArmReach. The shoulder is
//    the hunter's, from the neck; the elbow is bent between the body's own elbow hint (the arm
//    solver's ArmPole) and where a relaxed wrist on this controller puts it (ElbowOf).
//  - The body: a rounded-box column round the spine from the collarbones to mid-thigh, its depth
//    and width by height (BodyRows: the chest, the flanks, the belly, the hips, the front of the
//    upper thighs) plus BodyReach in front and a side reach that shrinks to nothing at the hips, so
//    a hand hanging at your side is never in it. Seated: down to the lap, and the lap (the thighs,
//    from the hips to the knees).
//  - The head: a small capsule following head tilt about the neck.
//  - Each leg: hip to knee to ankle, with knees and ankles referenced to the room floor when
//    available. Seated knees sit ahead of the lap. These are inferred targets, not tracked feet.
// The body is anchored to the neck, not the eyes (looking down at the vial moves nothing): the
// neck pivot is 7.5 cm under and 8 cm behind the eyes in the head's own axes (as
// core/vr/evasion.h's), the torso's heading is the presenter's lazy one (core/vr/belt_vial.h
// Torso: a glance down never turns it).
//
// A stab (Gate): the vial hand goes in, and
//  - it was outside that zone (by EdgeM, two frames in a row) within RecentOutS before, and has
//    gone StrokeM in from the farthest out it was in that time (a hand armed or resting inside
//    never fires, leave and come back; a hand held just outside it, 1 or 2 cm, may stab from there),
//  - its OWN motion (relative to the neck: never the other hand's, never the zone's) was toward the
//    zone at ApproachSpeed or more, at least ApproachShare of its whole speed (into it, not past
//    it), within ApproachAgeS before, and is still into it (not along it) as it goes in,
//  - the weapon hand is not swinging (its own speed under SwingGuardSpeed: the vial hand pulled to
//    the body, or riding along with the weapon hand, in a swing is no stab),
//  - the body is not lunging or dodging onto it (the neck faster than BodyMovedSpeed while the
//    hand's own approach is under BodyMovedShare of it: a hand lagging behind a lean),
//  - it stays in for InsideFrames tracked frames (a one-frame glitch never fires).
// So the weapon arm swinging onto a still vial hand, both hands swinging together, walking,
// turning, dodging or lunging with the vial held, the gun's aim pose, a throw, a hand lowered to
// hang at the side, a hand resting on the belly never fire; an entry that is refused is a near
// miss (which rule), for the log. A vial taken inside a zone (the belt's draw at the hip, a quick
// item's take) counts as entering it once the hand has moved MoveOutM from where it was taken
// (StartMoved).
//
// Pure (no OpenXR or emulator state): the presenter (renderer_vulkan/openxr_presenter.cpp
// UpdateVialGeometry, UpdateVial, UpdateQuickStab) feeds it, tests/test_vial_zones.cpp checks it.
namespace VR::VialZones {

using Body::Quat;
using Body::Vec3;

// ---- Gating ---------------------------------------------------------------------------------------
constexpr float ApproachSpeed = 0.2f; // m/s: the vial hand's own speed toward the zone ...
constexpr float ApproachShare = 0.5f; // ... at least this share of its whole speed (60 degrees) ...
constexpr float ApproachAgeS = 0.25f; // ... at most this long before it is inside
constexpr float EdgeM = 0.005f;       // outside the zone by this much ...
constexpr int OutFrames = 2;          // ... this many tracked frames in a row ...
constexpr float RecentOutS = 0.6f;    // ... at most this long before it is inside ...
constexpr float StrokeM = 0.03f;      // ... and in this far from the farthest out it was meanwhile
constexpr float EntryWindowS = 0.3f;  // (an entry may go on in this long to get StrokeM deep)
constexpr int InsideFrames = 2;       // tracked frames inside: a stab
// Combat motions are no stab: the weapon hand's own speed (relative to the neck) over this is a
// swing; the neck faster than BodyMovedSpeed with the vial hand's own approach under
// BodyMovedShare of it is the body lunging or dodging onto a lagging hand.
constexpr float SwingGuardSpeed = 1.5f;
constexpr float SwingAfterS = 0.2f; // ... and for this long after it (the swing's tail): the vial
                                    // hand's motion meanwhile is no approach either
constexpr float BodyMovedSpeed = 0.6f;
constexpr float BodyMovedShare = 0.5f;
constexpr float MoveOutM = 0.08f;     // taken inside a zone: moved this far from there = outside
constexpr float StillSpeed = 0.1f;    // near misses: the vial hand slower than this was still
constexpr float IntoMinSpeed = 0.05f; // slower than this as it goes in: no direction to check
constexpr float AfterStabS = 0.5f;    // after a stab, no other for this long (the same motion going
                                      // on into the next zone)
// Swings wait (the presenter's VialReaching): the vial hand in an arm zone after an approved entry,
// or closer than ReachMarginM outside one and moving toward it itself at ReachSpeed, closing on it
// at ReachSpeed and at least half the weapon hand's speed (not the two hands moving as one), the
// weapon hand not swinging.
constexpr float ReachMarginM = 0.15f;
constexpr float ReachSpeed = 0.3f;

// ---- The body (k = 1: a ~170 cm adult, metres) --------------------------------------------------
// The neck pivot in the head's axes: the eyes 7.5 cm above and 8 cm in front of it (evasion.h).
constexpr float NeckUp = 0.075f;
constexpr float NeckForward = 0.08f;
// The right shoulder joint from the neck pivot (the hunter's c0000 reference pose: right, up, ahead)
// and his arm bones (upper arm, forearm).
constexpr float ShoulderRight = 0.171f;
constexpr float ShoulderUp = -0.140f;
constexpr float ShoulderAhead = -0.019f;
constexpr float UpperArmM = 0.283f;
constexpr float ForearmM = 0.273f;
constexpr float SwingToPole = 0.30f; // the arm solver's (core/vr/arm_ik.h ArmParams)
// Skin radii of the right arm: the fist round the controller (grip point -> wrist), the forearm,
// the upper arm. The vial hand's grip point stops ArmReach beyond the skin (its controller is in
// the way): the zones are skin + ArmReach (x vial_zone_scale).
constexpr float HandSkin = 0.08f;
constexpr float ForearmSkin = 0.045f;
constexpr float UpperArmSkin = 0.055f;
constexpr float ArmReach = 0.06f;
// The body's reach in front (x vial_zone_scale).
constexpr float BodyReach = 0.10f;
// The body's profile down from the neck pivot (h, below it; eyes = h + 0.075): the centre of the
// cross-section ahead of the pivot, the skin's half depth and half width, the reach beyond the
// side's skin. The side reach shrinks to almost nothing at the hips and thighs: a hand hanging at
// your side (its grip ~22-26 cm out from the middle) stays outside.
struct BodyRow {
    float h, ahead, depth, width, side_reach;
};
inline constexpr std::array<BodyRow, 7> BodyRows = {{
    {0.105f, 0.02f, 0.09f, 0.14f, 0.08f}, // the collarbones (the top)
    {0.275f, 0.03f, 0.11f, 0.16f, 0.09f}, // the chest
    {0.425f, 0.03f, 0.11f, 0.15f, 0.09f}, // the belly and the flanks
    {0.525f, 0.03f, 0.11f, 0.15f, 0.07f},
    {0.625f, 0.04f, 0.11f, 0.17f, 0.02f}, // the hips
    {0.775f, 0.05f, 0.10f, 0.17f, 0.01f}, // the upper thighs (the front)
    {0.945f, 0.05f, 0.09f, 0.15f, 0.01f}, // mid-thigh (the bottom)
}};
constexpr float BodyShapePower = 3.0f; // documents the cross-section's exponent (BodyColumn)
constexpr float ChestToH = 0.345f; // labels: above it "chest", above BellyToH "belly", "thigh"
constexpr float BellyToH = 0.605f;
// Seated: the lap, a line from the hips forward to the knees LapBelow under the eyes, LapSkin round.
constexpr float LapBelow = 0.62f; // m below the eyes (seated; the caller may know it in the room)
constexpr float LapFront = 0.50f;
constexpr float LapSkin = 0.08f;

enum class Zone : std::uint8_t { Wrist, Forearm, UpperArm, Body, Lap, Head, LeftLeg, RightLeg, Count };
constexpr std::size_t ZoneCount = static_cast<std::size_t>(Zone::Count);

[[nodiscard]] constexpr bool IsArm(Zone z) {
    return z == Zone::Wrist || z == Zone::Forearm || z == Zone::UpperArm;
}
[[nodiscard]] constexpr const char* ZoneLabel(Zone z) {
    switch (z) {
    case Zone::Wrist:
        return "wrist";
    case Zone::Forearm:
        return "forearm";
    case Zone::UpperArm:
        return "upper arm";
    case Zone::Body:
        return "body";
    case Zone::Lap:
        return "lap";
    case Zone::Head:
        return "head";
    case Zone::LeftLeg:
        return "left leg";
    case Zone::RightLeg:
        return "right leg";
    case Zone::Count:
        break;
    }
    return "?";
}

struct Params {
    float k{1.0f};     // your height's body scale (core/vr/player_body.h BodyScale)
    float scale{1.0f}; // vial_zone_scale: the reach margins, 0.5 .. 2
    bool floor_known{false};
    float floor_y{0.0f}; // LOCAL room floor; keeps lower legs reachable while bending down
    bool seated{false};
    float lap_below{LapBelow}; // seated: the lap this far below the eyes (m, already x k) ...
    bool lap_y_known{false};   // ... or at this height in LOCAL (the floor and the seated eye)
    float lap_y{0.0f};
    // body_natural_arms: the elbow hint the arm solver uses (Body::ArmPole's natural pole: arms
    // folded across the chest hang their elbows lower), so the zones follow the arm that is drawn
    // (2026-10-08 review: they kept the old across-the-body pole, ~8-9 cm higher and further
    // forward for a right hand folded across the chest).
    bool natural_arms{true};
};

// The hunter's right wrist and his elbow at a relaxed wrist (the arm solver's neutral wrist, the
// bind pose: the blade about 77 degrees off the forearm) in the right controller's OpenXR grip
// frame (m): the weapon bone on the grip as the body puts it (core/vr/body_frame.h DefaultGrip with
// blade_pitch_deg and the weapon grip's trims; the controller frame's +Z is grip -Z).
// `ok` false (and both zero) until the hunter's skeleton is read at runtime
// (core/vr/c0000_skeleton.h): the arm's zone stays off until then.
struct ArmInGrip {
    Vec3 wrist{};
    Vec3 elbow{};
    bool ok{false};
};
inline ArmInGrip HunterArmInGrip(float blade_pitch_deg, float yaw_deg = 0.0f,
                                 float pitch_deg = 0.0f, float roll_deg = 0.0f) {
    namespace C = Body::C0000;
    const C::Skeleton* const sk = C::Active();
    if (sk == nullptr) {
        return {};
    }
    const auto& m = sk->model;
    const auto v = [](const std::array<float, 3>& a) { return Vec3{a[0], a[1], a[2]}; };
    const auto& wq = m[C::R_Weapon].q;
    const Quat weapon_q = Body::Normalize(Quat{wq[0], wq[1], wq[2], wq[3]});
    const Vec3 weapon = v(m[C::R_Weapon].t);
    const Quat grip = Body::DefaultGrip(C::Right, blade_pitch_deg, yaw_deg, pitch_deg, roll_deg);
    const auto in_grip = [&](Vec3 model) {
        const Vec3 c = Body::Rotate(grip, Body::Rotate(Body::Conjugate(weapon_q), model - weapon));
        return Vec3{c.x, c.y, -c.z};
    };
    return {in_grip(v(m[C::R_Hand].t)), in_grip(v(m[C::R_Forearm].t)), true};
}

// The elbow of an arm from `shoulder` to `hand` with `upper` and `fore` long bones, bent toward
// `pole` (on the line when the hand is out of reach).
inline Vec3 ElbowOf(Vec3 shoulder, Vec3 hand, Vec3 pole, float upper, float fore) {
    const Vec3 d = hand - shoulder;
    const float len = Body::Length(d);
    if (len < 1e-3f) {
        return shoulder + Body::Normalize(pole, {0.0f, -1.0f, 0.0f}) * upper;
    }
    const Vec3 dir = d * (1.0f / len);
    const float reach = std::min(len, upper + fore - 1e-3f);
    const float x = (upper * upper - fore * fore + reach * reach) / (2.0f * reach);
    const float h = std::sqrt(std::max(upper * upper - x * x, 0.0f));
    const Vec3 side = Body::Normalize(Body::Perp(pole, dir), Body::AnyPerpendicular(dir));
    return shoulder + dir * x + side * h;
}

// The neck pivot of the eyes `eye` with the head's orientation `q` (LOCAL).
inline Vec3 NeckFromEye(Vec3 eye, const Quat& q) {
    return eye - Body::Rotate(Body::Normalize(q), Vec3{0.0f, NeckUp, -NeckForward});
}

// The neck pivot and its velocity, frame by frame: the eyes' velocity less the head's turn about
// the neck (a look round moves the eyes, not the neck). No eye velocity: the pivot's own.
class NeckTracker {
public:
    struct Out {
        bool ok{false};
        Vec3 p{};
        Vec3 v{};
        Quat q{}; // head orientation, for the head target only
    };
    Out Update(bool head_ok, Vec3 eye, Vec3 eye_v, bool eye_v_ok, const Quat& q, float dt) {
        Out out{};
        if (!head_ok || !Body::IsFinite(eye) || !Body::IsFinite(q)) {
            valid_ = false;
            return out;
        }
        const Quat qn = Body::Normalize(q);
        out.ok = true;
        out.q = qn;
        out.p = NeckFromEye(eye, qn);
        const bool step = valid_ && dt > 1e-4f && dt < 0.2f;
        Vec3 w{};
        if (step) {
            Quat d = qn * Body::Conjugate(q_);
            if (d.w < 0.0f) {
                d = {-d.x, -d.y, -d.z, -d.w};
            }
            const Vec3 axis{d.x, d.y, d.z};
            const float s = Body::Length(axis);
            if (s > 1e-7f) {
                w = axis * (2.0f * std::atan2(s, d.w) / (s * dt));
            }
        }
        if (eye_v_ok && Body::IsFinite(eye_v)) {
            out.v = eye_v - Body::Cross(w, eye - out.p);
        } else if (step) {
            out.v = (out.p - p_) * (1.0f / dt);
        }
        valid_ = true;
        q_ = qn;
        p_ = out.p;
        return out;
    }
    void Reset() {
        valid_ = false;
    }

private:
    bool valid_{false};
    Quat q_{};
    Vec3 p_{};
};

struct Tracked {
    bool valid{false}; // position tracked
    Vec3 p{};
    Vec3 v{};
    bool q_valid{false};
    Quat q{};
};

// The zones this frame.
struct Geometry {
    bool body_ok{false}; // the neck and the torso's heading (the body, the lap, the shoulder)
    bool arm_ok{false};  // the right controller (the arm zones)
    Params params{};
    Vec3 neck{};
    Vec3 neck_v{};
    float neck_speed{0.0f};
    Vec3 forward{0.0f, 0.0f, -1.0f}; // the torso's heading (horizontal, unit)
    Vec3 right{1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 head_base{}, head_top{};
    std::array<Vec3, 2> hips{}, knees{}, ankles{}; // left, right
    Vec3 grip{};
    Vec3 weapon_v{};          // the right controller's velocity (LOCAL, m/s)
    float weapon_speed{0.0f}; // ... its own speed, relative to the neck
    Vec3 wrist{};
    Vec3 elbow{};
    Vec3 shoulder{};
    float lap_h{0.0f}; // seated: the lap below the neck pivot (m)
};

// `torso_forward`: the torso's heading (any length; its horizontal part is used).
inline Geometry Build(const NeckTracker::Out& neck, Vec3 torso_forward, const Tracked& weapon,
                      const ArmInGrip& arm, const Params& params) {
    Geometry g{};
    g.params = params;
    g.params.k = std::isfinite(params.k) && params.k > 0.0f ? params.k : 1.0f;
    g.params.scale = std::isfinite(params.scale) ? std::clamp(params.scale, 0.5f, 2.0f) : 1.0f;
    const float k = g.params.k;
    const Vec3 flat{torso_forward.x, 0.0f, torso_forward.z};
    const float flat_len = Body::Length(flat);
    g.body_ok = neck.ok && flat_len > 1e-3f && Body::IsFinite(neck.p) && Body::IsFinite(neck.v);
    if (g.body_ok) {
        g.neck = neck.p;
        g.neck_v = neck.v;
        g.neck_speed = Body::Length(neck.v);
        g.forward = flat * (1.0f / flat_len);
        g.right = {-g.forward.z, 0.0f, g.forward.x};
        g.lap_h = params.lap_y_known ? std::clamp(g.neck.y - params.lap_y, 0.2f, 0.95f)
                                     : std::max(params.lap_below - NeckUp, 0.2f);
        const Quat head_q = Body::Normalize(neck.q);
        g.head_base = g.neck + Body::Rotate(head_q, {0.0f, 0.075f * k, 0.0f});
        g.head_top = g.neck + Body::Rotate(head_q, {0.0f, 0.17f * k, 0.0f});
        const float hip_y = g.neck.y - (params.seated ? g.lap_h : 0.625f * k);
        const float floor = params.floor_known && std::isfinite(params.floor_y)
                                ? params.floor_y
                                : hip_y - (params.seated ? 0.50f : 0.90f) * k;
        // Keep knee below the hip when crouched, and the ankle above the floor. We do not
        // extend a standing thigh straight through the floor when the player bends down.
        const float ankle_y = floor + 0.12f * k;
        const float knee_y = std::max(ankle_y, std::min(floor + 0.50f * k, hip_y));
        for (std::size_t i = 0; i < 2; ++i) {
            const Vec3 side = g.right * ((i == 0 ? -0.12f : 0.12f) * k);
            g.hips[i] = g.neck + side + g.forward * (0.03f * k);
            g.hips[i].y = std::max(hip_y, ankle_y);
            g.knees[i] = g.neck + side +
                         g.forward * ((params.seated ? LapFront : 0.05f) * k);
            g.knees[i].y = knee_y;
            g.ankles[i] = g.knees[i];
            g.ankles[i].y = ankle_y;
        }
    }
    g.arm_ok = arm.ok && weapon.valid && weapon.q_valid && Body::IsFinite(weapon.p) &&
               Body::IsFinite(weapon.q) && Body::Dot(weapon.q, weapon.q) > 0.5f;
    if (!g.arm_ok) {
        return g;
    }
    const Quat q = Body::Normalize(weapon.q);
    g.grip = weapon.p;
    if (Body::IsFinite(weapon.v)) {
        g.weapon_v = weapon.v;
        g.weapon_speed = Body::Length(weapon.v - (g.body_ok ? g.neck_v : Vec3{}));
    }
    g.wrist = weapon.p + Body::Rotate(q, arm.wrist);
    const float upper = UpperArmM * k;
    const float fore = ForearmM * k;
    // Where a relaxed wrist on this controller puts the elbow.
    const Vec3 relaxed =
        g.wrist + Body::Rotate(q, Body::Normalize(arm.elbow - arm.wrist, {0.0f, 1.0f, 0.0f})) * fore;
    if (!g.body_ok) {
        g.elbow = relaxed; // no shoulder without the neck: the forearm alone
        g.shoulder = relaxed;
        return g;
    }
    g.shoulder = g.neck + g.right * (ShoulderRight * k) + g.up * (ShoulderUp * k) +
                 g.forward * (ShoulderAhead * k);
    const Vec3 axis = Body::Normalize(g.wrist - g.shoulder, -g.up);
    Body::BodyFrame frame{};
    frame.right = g.right;
    frame.up = g.up;
    frame.forward = g.forward;
    // The arm solver's way (core/vr/arm_ik.h SolveArm): the body's elbow hint (ArmPole), turned
    // SwingToPole of the way toward the side a relaxed wrist on this controller puts the elbow
    // (none when the two disagree by more than ~150 degrees, or the forearm runs along the
    // shoulder-wrist line).
    const Vec3 spine = g.shoulder - g.right * (ShoulderRight * k);
    Vec3 bend = Body::Normalize(
        Body::Perp(Body::ArmPole(frame, 1.0f, g.wrist - g.shoulder, g.wrist - spine,
                                 params.natural_arms),
                   axis),
        Body::AnyPerpendicular(axis));
    const Vec3 relaxed_side = Body::Perp((relaxed - g.wrist) * (1.0f / fore), axis);
    const float relaxed_len = Body::Length(relaxed_side);
    if (relaxed_len > 1e-4f) {
        const float phi = Body::SignedAngle(bend, relaxed_side * (1.0f / relaxed_len), axis);
        const float turn = phi * SwingToPole * Body::Smoothstep(0.15f, 0.5f, relaxed_len) *
                           (1.0f - Body::Smoothstep(0.5f * Body::Pi, 0.85f * Body::Pi,
                                                    std::abs(phi)));
        bend = Body::RotateAbout(bend, axis, turn);
    }
    g.elbow = ElbowOf(g.shoulder, g.wrist, bend, upper, fore);
    return g;
}

// Where a point is against a zone: the signed distance from its surface (< 0 inside), the unit
// direction into it (toward its middle), the part it is nearest (the log's name).
struct Measure {
    bool valid{false};
    float d{1.0f};
    Vec3 inward{};
    const char* label{""};
};

namespace Detail {

inline Measure Capsule(Vec3 p, Vec3 a, Vec3 b, float radius, float& t) {
    const Vec3 ab = b - a;
    const float len_sq = Body::LengthSq(ab);
    t = len_sq > 1e-8f ? std::clamp(Body::Dot(p - a, ab) / len_sq, 0.0f, 1.0f) : 0.0f;
    const Vec3 c = a + ab * t;
    const Vec3 to = c - p;
    const float dist = Body::Length(to);
    Measure m{};
    m.valid = true;
    m.d = dist - radius;
    m.inward = Body::Normalize(to, Body::Normalize(-ab, {0.0f, -1.0f, 0.0f}));
    return m;
}

inline BodyRow RowAt(float h, float k) {
    const auto& rows = BodyRows;
    const float x = h / k;
    if (x <= rows.front().h) {
        return rows.front();
    }
    for (std::size_t i = 1; i < rows.size(); ++i) {
        if (x <= rows[i].h) {
            const BodyRow& a = rows[i - 1];
            const BodyRow& b = rows[i];
            const float t = (x - a.h) / (b.h - a.h);
            const auto mix = [t](float u, float w) { return u + (w - u) * t; };
            return {x, mix(a.ahead, b.ahead), mix(a.depth, b.depth), mix(a.width, b.width),
                    mix(a.side_reach, b.side_reach)};
        }
    }
    return rows.back();
}

inline Measure BodyColumn(const Geometry& g, Vec3 p) {
    const float k = g.params.k;
    const float s = g.params.scale;
    const Vec3 rel = p - g.neck;
    const float ahead = Body::Dot(rel, g.forward);
    const float side = Body::Dot(rel, g.right);
    const float h = -Body::Dot(rel, g.up);
    const float top = BodyRows.front().h * k;
    float bottom = BodyRows.back().h * k;
    if (g.params.seated) {
        bottom = std::min(bottom, std::max(g.lap_h - 0.04f * k, top + 0.05f));
    }
    const BodyRow row = RowAt(std::clamp(h, top, bottom), k);
    const float depth = row.depth * k + BodyReach * s;
    const float width = row.width * k + row.side_reach * s;
    const float x = ahead - row.ahead * k;
    const float y = side;
    // The cross-section: |x / depth|^n + |y / width|^n = 1 (BodyShapePower; boxier than an ellipse,
    // so the front stays deep across both thighs). rho is 1 on it and grows linearly outward along
    // each ray from the middle: the surface along the ray is at r / rho.
    const float ax = std::abs(x) / depth;
    const float ay = std::abs(y) / width;
    const float rho = std::cbrt(ax * ax * ax + ay * ay * ay);
    const float r = std::sqrt(x * x + y * y);
    const float dh = rho > 1e-6f ? r * (1.0f - 1.0f / rho) : -std::min(depth, width);
    const Vec3 out_h = Body::Normalize(g.forward * (std::copysign(ax * ax, x) / depth) +
                                           g.right * (std::copysign(ay * ay, y) / width),
                                       g.forward);
    const float above = top - h;
    const float below = h - bottom;
    const float dv = std::max({above, below, 0.0f});
    Measure m{};
    m.valid = true;
    if (dv > 0.0f) {
        const Vec3 out_v = above > 0.0f ? g.up : -g.up;
        const float horizontal = std::max(dh, 0.0f);
        m.d = std::sqrt(horizontal * horizontal + dv * dv);
        m.inward = -Body::Normalize(out_h * horizontal + out_v * dv, out_v);
    } else {
        m.d = dh;
        m.inward = -out_h;
    }
    const bool upper = h < BellyToH * k;
    if (upper && std::abs(y) > 0.7f * width && x < 0.5f * depth) {
        m.label = y > 0.0f ? "right side" : "left side";
    } else {
        m.label = h < ChestToH * k ? "chest" : (upper ? "belly" : "thigh");
    }
    return m;
}

} // namespace Detail

inline Measure MeasureZone(const Geometry& g, Zone zone, Vec3 p) {
    const float k = g.params.k;
    const float reach = ArmReach * g.params.scale;
    float t = 0.0f;
    switch (zone) {
    case Zone::Wrist:
        if (g.arm_ok) {
            Measure m = Detail::Capsule(p, g.grip, g.wrist, HandSkin + reach, t);
            m.label = "wrist";
            return m;
        }
        break;
    case Zone::Forearm:
        if (g.arm_ok) {
            Measure m = Detail::Capsule(p, g.wrist, g.elbow, ForearmSkin * k + reach, t);
            m.label = t > 0.85f ? "elbow" : "forearm";
            return m;
        }
        break;
    case Zone::UpperArm:
        if (g.arm_ok && g.body_ok) {
            Measure m = Detail::Capsule(p, g.elbow, g.shoulder, UpperArmSkin * k + reach, t);
            m.label = t < 0.15f ? "elbow" : "upper arm";
            return m;
        }
        break;
    case Zone::Body:
        if (g.body_ok) {
            return Detail::BodyColumn(g, p);
        }
        break;
    case Zone::Lap:
        if (g.body_ok && g.params.seated) {
            const Vec3 hip = g.neck - g.up * g.lap_h;
            Measure m = Detail::Capsule(p, hip, hip + g.forward * (LapFront * k),
                                        LapSkin * k + BodyReach * g.params.scale, t);
            m.label = "lap";
            return m;
        }
        break;
    case Zone::Head:
        if (g.body_ok) {
            Measure m = Detail::Capsule(p, g.head_base, g.head_top,
                                        0.09f * k + 0.04f * g.params.scale, t);
            m.label = "head";
            return m;
        }
        break;
    case Zone::LeftLeg:
    case Zone::RightLeg:
        if (g.body_ok) {
            const std::size_t i = zone == Zone::LeftLeg ? 0 : 1;
            const float margin = 0.04f * g.params.scale;
            Measure thigh = Detail::Capsule(p, g.hips[i], g.knees[i], 0.075f * k + margin, t);
            Measure shin = Detail::Capsule(p, g.knees[i], g.ankles[i], 0.065f * k + margin, t);
            Measure m = thigh.d < shin.d ? thigh : shin;
            m.label = i == 0 ? "left leg" : "right leg";
            return m;
        }
        break;
    case Zone::Count:
        break;
    }
    return {};
}

// Why an entry was no stab (near misses).
enum class Refusal : std::uint8_t {
    None,
    NotOutside, // not outside it by EdgeM within RecentOutS (it was in: armed or resting there)
    Still,      // the vial hand barely moved: the zone came to it (the weapon arm, the body)
    Slow,       // toward it, but slower than ApproachSpeed
    Glancing,   // moving past it rather than into it (less than ApproachShare of its speed)
    OneFrame,   // inside for one frame only (a tracking glitch)
    Swinging,   // the weapon hand was swinging (SwingGuardSpeed)
    BodyMoved,  // the body lunged or dodged onto the hand (BodyMovedSpeed / BodyMovedShare)
    Short,      // not StrokeM in from where it was outside (a nudge at its edge)
};
[[nodiscard]] constexpr const char* RefusalName(Refusal r) {
    switch (r) {
    case Refusal::None:
        return "none";
    case Refusal::NotOutside:
        return "not outside";
    case Refusal::Still:
        return "still";
    case Refusal::Slow:
        return "slow";
    case Refusal::Glancing:
        return "glancing";
    case Refusal::OneFrame:
        return "one frame";
    case Refusal::Swinging:
        return "swinging";
    case Refusal::BodyMoved:
        return "body moved";
    case Refusal::Short:
        return "short";
    }
    return "?";
}

struct Entry {
    Zone zone{Zone::Count};
    const char* label{""};
    float inside_m{0.0f}; // how far in (m)
    float approach{0.0f}; // its own speed toward it now (m/s)
    float best{0.0f};     // the qualifying approach (m/s; 0 = none within ApproachAgeS)
    float speed{0.0f};    // its own speed (m/s)
    float out_m{0.0f};    // the farthest outside it was within RecentOutS before (m)
    float out_age{0.0f};  // s since it was last outside (large: never)
    float travel{0.0f};   // how far in from there (m)
    float weapon_speed{0.0f}; // the weapon hand's own speed (m/s)
    float body_speed{0.0f};   // the neck's speed (m/s)
    Refusal why{Refusal::None};
};

struct Result {
    bool stab{false};
    Entry hit{};
    bool refused{false}; // an entry that was no stab (a near miss: `miss`)
    Entry miss{};
    bool reaching{false}; // swings wait (an arm zone)
    std::array<Measure, ZoneCount> m{};
};

class Gate {
public:
    void Reset() {
        *this = {};
    }
    // A vial (or a quick item) taken at `from` (the belt's draw at the hip, an item's take): a
    // zone the hand is inside or at the edge of (closer than EdgeM) at the next tracked update
    // counts as entered, from outside, once the hand is in it and has moved MoveOutM from there.
    void StartMoved(Vec3 from) {
        move_from_ = from;
        move_pending_ = true;
        first_ = true;
    }

    Result Update(const Geometry& g, const Tracked& vial, float dt) {
        Result r{};
        dt = std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.1f) : 0.0f;
        const bool hand = vial.valid && Body::IsFinite(vial.p) && Body::IsFinite(vial.v);
        const Vec3 own = hand ? vial.v - (g.body_ok ? g.neck_v : Vec3{}) : Vec3{};
        const float speed = Body::Length(own);
        const bool moved_out = move_pending_ && Body::Length(vial.p - move_from_) >= MoveOutM;
        since_stab_ = std::min(since_stab_ + dt, Never);
        swing_age_ = g.arm_ok && g.weapon_speed > SwingGuardSpeed
                         ? 0.0f
                         : std::min(swing_age_ + dt, Never);
        const bool swinging = swing_age_ < SwingAfterS;
        for (std::size_t i = 0; i < ZoneCount; ++i) {
            State& s = zones_[i];
            s.out_age = std::min(s.out_age + dt, Never);
            s.peak_age = std::min(s.peak_age + dt, Never);
            s.approach_age = std::min(s.approach_age + dt, Never);
            s.entry_age = std::min(s.entry_age + dt, Never);
            const Zone zone = static_cast<Zone>(i);
            const Measure m = hand ? MeasureZone(g, zone, vial.p) : Measure{};
            r.m[i] = m;
            if (!m.valid) {
                s.seen = false;
                s.settle_s = 0.0f;
                s.inside = 0;
                s.out = 0;
                s.approved = false;
                s.pending = false;
                s.taken = false;
                continue;
            }
            if (!s.seen) {
                // The first look (armed, tracked again, the zone back): inside already is no
                // entry; it has to leave and come back.
                s.seen = true;
                s.inside = m.d < 0.0f ? InsideFrames : 0;
            }
            const float approach = Body::Dot(own, m.inward);
            const bool new_target = zone == Zone::Head || zone == Zone::LeftLeg ||
                                    zone == Zone::RightLeg;
            // Relative motion alone can come from a lean onto a stationary hand. For the new
            // targets the hand must also move inward in room space, independently of the head.
            const bool hand_inward = !new_target || Body::Dot(vial.v, m.inward) >= ApproachSpeed;
            if (!swinging && hand_inward && approach >= ApproachSpeed &&
                approach >= ApproachShare * speed) {
                s.approach_age = 0.0f;
                s.best = approach;
            }
            if (m.d > EdgeM) {
                s.taken = false; // out of it: the usual rules from here
                if (++s.out >= OutFrames) {
                    // The farthest out within RecentOutS (a stale one gives way to now).
                    if (s.out_age > RecentOutS || s.peak_age > RecentOutS || m.d >= s.out_m) {
                        s.out_m = m.d;
                        s.peak_age = 0.0f;
                    }
                    s.out_age = 0.0f;
                }
            } else {
                s.out = 0;
                if (first_ && move_pending_) {
                    s.taken = true; // taken in it or at its edge
                }
            }
            const auto entry = [&](Refusal why) {
                Entry e{};
                e.zone = zone;
                e.label = m.label;
                e.inside_m = -m.d;
                e.approach = approach;
                e.best = s.approach_age <= ApproachAgeS ? s.best : 0.0f;
                e.speed = speed;
                e.out_m = s.out_m;
                e.out_age = s.out_age;
                e.travel = s.out_age <= RecentOutS ? s.out_m - m.d : 0.0f;
                e.weapon_speed = g.weapon_speed;
                e.body_speed = g.neck_speed;
                e.why = why;
                return e;
            };
            const auto refuse = [&](Refusal why) {
                s.pending = false;
                // A hand drifting across the zone's edge (never 1 cm out, no stab's speed) is no
                // near miss worth a log line (a hand resting at the edge would repeat it).
                const bool drift = (why == Refusal::Still || why == Refusal::Slow) &&
                                   s.out_m < 2.0f * EdgeM;
                if (!r.refused && !drift) {
                    r.refused = true;
                    r.miss = entry(why);
                }
            };
            if (m.d < 0.0f) {
                if (s.taken) {
                    if (!moved_out) {
                        s.inside = 0;
                        continue; // still near where it was taken
                    }
                    s.taken = false; // moved on into it: an entry from outside, now, deep enough
                    s.inside = 0;
                    s.out_age = 0.0f;
                    s.out_m = m.d + StrokeM + 1e-4f;
                    s.peak_age = 0.0f;
                }
                const bool first_in = ++s.inside == 1;
                const bool moved = s.approach_age <= ApproachAgeS;
                const float best = moved ? std::max(s.best, approach) : approach;
                const bool body_moved =
                    g.neck_speed >= BodyMovedSpeed && best < BodyMovedShare * g.neck_speed;
                const bool deep = s.out_m - m.d >= StrokeM;
                if (first_in) {
                    s.approved = false;
                    s.entry_age = 0.0f;
                    s.settle_s = 0.0f;
                    // After a stab, the same motion going on into the next zone is nothing.
                    s.pending = since_stab_ >= AfterStabS;
                    // Going in, not along it, as it goes in (a stab slowing down at the body
                    // keeps its direction; one stopped there has none).
                    const bool into = speed < IntoMinSpeed || approach >= ApproachShare * speed;
                    if (!s.pending) {
                    } else if (s.out_age > RecentOutS) {
                        refuse(Refusal::NotOutside);
                    } else if (!moved && speed < StillSpeed) {
                        refuse(Refusal::Still);
                    } else if (swinging) {
                        refuse(Refusal::Swinging);
                    } else if (!moved) {
                        refuse(Refusal::Slow);
                    } else if (body_moved) {
                        refuse(Refusal::BodyMoved);
                    } else if (!into) {
                        refuse(Refusal::Glancing);
                    } else if (deep) {
                        s.approved = true;
                        s.pending = false;
                    }
                } else if (s.pending) {
                    // Not StrokeM in yet as it went in: it may still go on in.
                    if (swinging) {
                        refuse(Refusal::Swinging);
                    } else if (body_moved) {
                        refuse(Refusal::BodyMoved);
                    } else if (deep && moved) {
                        s.approved = true;
                        s.pending = false;
                    } else if (!moved || s.entry_age > EntryWindowS) {
                        refuse(Refusal::Short);
                    }
                }
                // Head contact must settle continuously, and promptly after the stroke.
                // Never leave a pass-through gesture armed to inject when the hand stops later.
                // Combat that starts during this brief wait cancels it until a fresh entry.
                if (zone == Zone::Head) {
                    if (swinging || body_moved || s.entry_age > EntryWindowS) {
                        s.approved = false;
                        s.pending = false;
                    }
                    s.settle_s = s.approved && speed <= 0.25f
                                     ? s.settle_s + (first_in ? 0.0f : dt) : 0.0f;
                }
                const bool settled = zone != Zone::Head || s.settle_s >= 0.05f;
                if (s.approved && s.inside >= InsideFrames && settled && !r.stab) {
                    r.stab = true;
                    r.hit = entry(Refusal::None);
                }
            } else {
                if (s.inside == 1 && s.approved && !r.refused) {
                    r.refused = true;
                    r.miss = entry(Refusal::OneFrame);
                } else if (s.pending) {
                    refuse(Refusal::Short);
                }
                s.inside = 0;
                s.settle_s = 0.0f;
                s.approved = false;
                s.pending = false;
            }
            if (IsArm(zone) && !swinging) {
                // The vial hand closing on the arm itself: its own motion toward it, and toward
                // the zone's own motion too (the weapon hand's; the upper arm's half the neck's).
                const Vec3 zone_v =
                    zone == Zone::UpperArm ? (g.weapon_v + g.neck_v) * 0.5f : g.weapon_v;
                const float closing = hand ? Body::Dot(vial.v - zone_v, m.inward) : 0.0f;
                if ((s.approved && m.d < 0.0f) ||
                    (m.d < ReachMarginM && approach >= ReachSpeed && closing >= ReachSpeed &&
                     closing >= 0.5f * g.weapon_speed)) {
                    r.reaching = true;
                }
            }
        }
        if (hand) {
            first_ = false;
        }
        if (r.stab) {
            // One stab: every zone needs a new entry, from outside it, for the next one (the
            // hand leaves first), and none for AfterStabS (the same motion going on).
            for (State& s : zones_) {
                s.approved = false;
                s.pending = false;
                s.taken = false;
                s.out = 0;
                s.out_age = Never;
                s.peak_age = Never;
                s.out_m = 0.0f;
            }
            move_pending_ = false;
            since_stab_ = 0.0f;
        }
        return r;
    }

private:
    static constexpr float Never = 1000.0f;
    struct State {
        int inside{0};             // tracked frames inside in a row
        int out{0};                // tracked frames outside by EdgeM in a row
        float out_age{Never};      // s since the last such frame (with OutFrames before it)
        float out_m{0.0f};         // the farthest out within RecentOutS (m) ...
        float peak_age{Never};     // ... s ago
        float approach_age{Never}; // s since its own motion last qualified
        float best{0.0f};          // that approach (m/s)
        float settle_s{0.0f};      // continuous settled head contact after an approved entry
        float entry_age{Never};    // s since it went in
        bool approved{false};      // this entry is a stab once it stays InsideFrames
        bool pending{false};       // this entry is not decided yet (not StrokeM in yet)
        bool taken{false};         // StartMoved: in it or at its edge at the take
        bool seen{false};          // measured since the reset (or since it was last invalid)
    };
    std::array<State, ZoneCount> zones_{};
    Vec3 move_from_{};
    bool move_pending_{false};
    bool first_{true};
    float since_stab_{Never};
    float swing_age_{Never}; // s since the weapon hand was last swinging (SwingGuardSpeed)
};

} // namespace VR::VialZones
