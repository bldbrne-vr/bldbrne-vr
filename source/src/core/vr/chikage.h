// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The Chikage's scabbard, played by hand (2026-11-19: "align it to the player hip (so
// that it is always there) ... we put the sword in, and then we slide it in with our hands, we
// pull it out and it does the crazy attack (also makes the player take a step forward like in
// game) ... And we let people 2 hand it if they want too").
//
// The game hangs the scabbard (the weapon's part 1, WP_A_1063_1) on the hunter's dummy 412
// ("S-case katana", on B_Weapon_Case_spine at his left side) in every form, and its model shares
// the sword's frame: sheathed, the two frames are one. So:
//  - the blade's tip put to the scabbard's mouth, along it, goes in (Entered); from then the
//    worn scabbard swivels on the belt, or the blade follows the case fixed to the left fist;
//  - once 74% of the blade has gone in, it is seated: it comes out bloodied
//    (2026-11-19: "it is put in the blade, it gets bloodied, it just doesn't become
//    larger"): the same blade, the game's blood on it for BloodS, and the next hit that lands
//    deals chikage_blood_pct more and spends it. The form does not change: the blooded form is
//    both hands on the handle, as the other two-handed weapons (core/vr/trick_weapons.h);
//  - leaving the mouth (Left) faster than draw_speed after being seated is the quick draw ("it
//    does the cool charged attack": the form's charged row at once, with a step forward); slower
//    it is only drawn (bloodied all the same);
//  - the worn case can slip off the blade's line. The held case stays on the left fist;
//    the sword slides along its curve until withdrawn, the grip opens, or tracking is lost;
//  - the left hand takes the scabbard off the hip (Grab: a squeeze with the hand at its upper
//    part) and holds it out, as a swordsman pulls his scabbard forward: a blade of BladeM only
//    reaches a scabbard fixed on the hip with the sword arm stretched all the way up and out
//    (measured in the mock, 2026-11-19: the hand 1.1 m from the mouth, 72 degrees off its line).
//    Let go, it hangs on the hip again.
// Both hands on the handle are the blooded form (the grab group's rule, without the hunter's
// animation: transform.h Choose).
//
// Pure (tests/test_chikage.cpp). Any one space for all points (the presenter: OpenXR LOCAL).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "core/vr/arm_ik.h"

namespace VR::Chikage {

using Body::Vec3;

inline constexpr std::int32_t Family = 20; // EquipParamWeapon id / 100000

// The sword and its scabbard in the weapon's own frame (the grip at the origin, the blade along
// the axis; WP_A_1063 and WP_A_1063_1 in the user's own files, metres).
inline constexpr float SeatY = 0.118f;  // the scabbard's mouth sits here on a sheathed sword
inline constexpr float BladeM = 0.93f;  // from there to the clean blade's tip
inline constexpr float HoldY = 0.10f;   // a hand holds the scabbard about this far below its mouth
inline constexpr float GrabAlongM = 0.30f; // the left hand takes it by its upper part, this long ...
inline constexpr float GrabM = 0.13f;      // ... from within this of it
// ... unless the fist is right on a quick-item pocket holding something: within PocketFirstM of its
// centre and at most PocketFirstShare as far from it as from the scabbard (2026-11-20:
// "if we specifically target the item then the item", "very gentle for the item, scabbard should
// still win... most of the time"; the pocket reaches 17.5 cm around its centre and overlaps the upper
// part of the scabbard at the hip).
inline constexpr float PocketFirstM = 0.05f;
inline constexpr float PocketFirstShare = 0.5f;
inline constexpr float GrabPress = 0.75f, GrabRelease = 0.45f; // the squeeze

// The scabbard to the hunter's own eyes (2026-11-19: "the scabbard needs a 3d model
// or something, so we know we can do it", "I still don't see the scabbard"). Two things kept it
// out of sight: the first person mod's weapon file has the scabbard's triangles removed (the game
// draws none), and the game hangs it beside the left hip under the coat and cape, which cover
// that whole side from above. So it is drawn by us, from the game's own file, and what the coat
// hides of it shows through, dimmed. A place at the belt's front was tried and is kept for tests
// (VR_CHIKAGE_WORN=1): in the hunter's own frame (metres; +x his left, +y up, +z his back), from
// the pelvis; `into` is its line from the mouth.
inline constexpr Vec3 WornMouth{0.066f, 0.08f, -0.324f};
inline constexpr Vec3 WornInto{0.5495f, -0.3497f, 0.7593f}; // (0.55, -0.35, 0.76), unit

inline constexpr float MouthM = 0.11f, HeldMouthM = 0.14f; // ordinary capture
inline constexpr float WaistAssistM = 0.18f, HeldAssistM = 0.155f; // an inward stroke beside the rim
inline constexpr float AssistInwardSpeed = 0.08f; // m/s along the case, even for an oblique approach
inline constexpr float GuideM = 0.26f;     // visual lead-in for the held case
// ... with the blade within this of the scabbard's line: wide, for it swivels on its cord and the
// stretched arm above comes in 62 to 72 degrees off it; the mouth, the speed and the way in are
// what keep a passing blade out.
inline constexpr float EntryCos = 0.17364818f; // cos(80 degrees) for the two unit axes
inline constexpr float EntrySpeed = 3.0f; // ... and no faster than this (a swing passing by)
inline constexpr float LineM = 0.16f;     // ordinary sideways slip distance
inline constexpr float SlipMarginM = 0.02f; // room to steady a wider waist catch
inline constexpr float SeatM = 0.74f * BladeM; // arm the blood immediately at this depth
inline constexpr float BottomM = 0.03f;   // within this of all the way in: the guard meets the mouth
inline constexpr float OutM = 0.01f;      // the tip this far back out of the mouth: left
inline constexpr float PeakS = 0.08f;     // the leaving speed's memory
inline constexpr float DrawFullSpeed = 3.5f; // m/s: the quick draw at full power

// Centre of the curved steel, measured across WP_A_1063 from the guard towards the tip.
// Its matching case shares that curve when seated. A straight slide misses by ~7 cm at the tip.
inline constexpr float Bend(float u) { return u * u * (0.019f + 0.074f * u); }
// At the outside of the capture radius the rim can still be a hand's breadth away.
// Ease it onto the tip, reaching the exact fitted pose within the first 3 cm of insertion.
inline float CaptureFollow(float previous, float dt, float depth) {
    return std::clamp(std::max(previous + dt / 0.06f, depth / 0.03f), 0.0f, 1.0f);
}
inline Body::QsTransform HeldGrip(Body::Quat attachment, float scale) {
    // Half-turn about X keeps the mouth thumb-side and points the case's +X curve toward
    // the sword's bend. Mirror the transverse offsets to keep the rim fixed in the fist.
    const auto turn = Body::Normalize(attachment * Body::Quat{1, 0, 0, 0});
    return {Body::Rotate(turn, Vec3{-0.003f, SeatY + HoldY, -0.002f} * scale), turn};
}
inline Body::QsTransform SlidingPose(const Body::QsTransform& sword, float depth, float scale) {
    if (depth >= BladeM) return sword; // authored sword and case share their seated frame
    const float u = BladeM - depth;
    // Match the chord of the inserted section to the same length starting at the case mouth.
    // The divided differences of Bend also give its tangent when depth is zero.
    const float case_slope = depth * (0.019f + 0.074f * depth);
    const float blade_slope = case_slope + u * (0.038f + 0.222f * (u + depth));
    const auto turn = sword.q * Body::FromTo({case_slope, -1, 0}, {blade_slope, -1, 0});
    const Vec3 rim = sword.t + Body::Rotate(sword.q, Vec3{Bend(u), -(SeatY + u), 0} * scale);
    return {rim - Body::Rotate(turn, {0, -SeatY * scale, 0}), turn, sword.s};
}

// Invert the same curved slide when the left hand owns the case. A negative depth
// keeps the approaching tip that far outside the rim along the insertion axis.
inline Body::QsTransform SwordInHeldCase(const Body::QsTransform& held, float depth, float scale) {
    const auto relative = SlidingPose({}, std::max(depth, 0.0f), scale);
    auto target = Body::ParentFromChild(held, relative);
    if (depth < 0.0f) target.t += Body::Rotate(held.q, {0, -depth * scale, 0});
    target.s = held.s;
    return target;
}

struct Frame {
    float dt{0.0f};
    bool blade{false}; // the sword hand is known
    Vec3 grip{};       // the sword's grip point
    Vec3 axis{};       // from it toward the tip (unit)
    Vec3 side{};       // the blade curves toward local +X (unit)
    bool scabbard{false}; // the scabbard's place is known
    bool held{false};     // in the left hand (else on the hip)
    float hold{HoldY};    // held: the hand holds it this far below its mouth
    Vec3 anchor{};        // held: the hand's grip point on it; on the hip: its mouth
    Vec3 rest_axis{};     // from its mouth into it while no blade is in it (unit)
};

// Closest point on the final 6 cm of steel, restricted to the mouth's entry depths.
// The end chord follows the measured curve to within a quarter millimetre.
inline constexpr float EntrySteelM = 0.06f;
inline constexpr float EntrySteelBendM = Bend(BladeM) - Bend(BladeM - EntrySteelM);
inline bool SteelAtMouth(const Frame& f, Vec3 rel, float tip_depth, float radius) {
    const float front = f.held ? -1e-5f : -MouthM;
    if (tip_depth < front) return false;
    constexpr float chord_sq = EntrySteelM * EntrySteelM + EntrySteelBendM * EntrySteelBendM;
    const Vec3 chord = f.axis * EntrySteelM + f.side * EntrySteelBendM; // towards the tip
    float t = std::clamp(Body::Dot(rel, chord) / chord_sq, 0.0f, 1.0f);
    const float along = Body::Dot(chord, f.rest_axis);
    const float depth = tip_depth - t * along;
    if (depth < front || (!f.held && depth > MouthM)) {
        if (along == 0.0f) return false;
        t = (tip_depth - (depth < front ? front : MouthM)) / along;
        if (!(t >= 0.0f && t <= 1.0f)) return false;
    }
    return Body::LengthSq(rel - chord * t) <= radius * radius;
}

enum class Event : std::uint8_t {
    None,
    Entered,  // the tip went into the mouth
    Seated,   // crossed the blood depth
    Bottomed, // all the way in (the guard on the mouth)
    Left,     // out through the mouth (Out::speed, Out::draw)
    Slipped,  // pulled off the blade's line
};

struct Out {
    Event event{Event::None};
    bool in{false};
    float depth{0.0f}; // m of blade inside (0 .. BladeM)
    bool loaded{false}; // seated since it went in
    float build{0.0f};  // 0 .. 1 on the way to seated, 1 once seated
    float speed{0.0f};  // Left: m/s out of the mouth
    bool burst{false}; // once per seated withdrawal, cosmetic only
    bool draw{false};   // Left: the quick draw
    float power{0.0f};  // ... 0 at draw_speed .. 1 at DrawFullSpeed
    bool lunge{false};  // Left: a seated blade pulled LungeShare out at the draw's speed (once)
};

// The draw's step forward starts as the pull gets going (2026-11-20: "some percentage
// with a specific velocity is enough to trigger the movement"): a seated blade LungeShare of the
// way out of its deepest, pulled at the draw's speed. Not with an enemy within LungeClearM ahead
// (bloodborne_melee.cpp EnemyCloseAhead): "if we are very near an enemy ... it shouldn't make us
// jump forward". The attack still waits for the blade to leave.
// (0.30 for one build; the tester: "22% of the way, if it has enough velocity".)
inline constexpr float LungeShare = 0.22f;
inline constexpr float LungeClearM = 1.2f;

// The draw's dash (2026-11-20 night: "move us more and faster ... a tiny bit sooner"):
// from the pull's lunge the hunter glides DashM along his facing in DashS, fast first and easing
// (DashAt), through the game's own warp each update (bloodborne_melee.cpp ChikageDashTick); it
// stops at a wall (a body-sized sphere's sweep), where no ground is under the next step, and with
// an enemy within DashStopM ahead. No dodge, no i-frames; nothing after it (chikage_draw_step_ms
// 0 by default: the stick's step, the slow second movement).
// (1.0 m in 0.25 s for one build, the stick's slow step after it; the tester: "it feels as if it is
// doing 2 movements, one early (which feels good) and a slow one after which feels bad ... just
// make that one a bit longer": the dash alone, 1.4 m at the same pace.)
inline constexpr float DashM = 1.4f, DashS = 0.33f, DashStopM = 0.7f;
inline float DashAt(float t) { // the share of DashM covered t s in (0 .. 1, ease-out)
    if (!(t > 0.0f)) {
        return 0.0f;
    }
    const float u = std::min(t / DashS, 1.0f);
    return 1.0f - (1.0f - u) * (1.0f - u);
}

// How deep the blade is in a scabbard whose anchor is `anchor`, and how far that anchor is off the
// blade's line.
struct Fit {
    float depth{0.0f};
    float off_sq{0.0f};
};
inline Fit FitOf(const Frame& f) {
    const Vec3 to = f.anchor - f.grip;
    const float along = Body::Dot(to, f.axis);
    const float mouth = along - (f.held ? f.hold : 0.0f); // the mouth along the blade from the grip
    const float bend = Bend(std::clamp(along - SeatY, 0.0f, BladeM));
    return {SeatY + BladeM - mouth, Body::LengthSq(to - f.axis * along - f.side * bend)};
}

// The scabbard's mouth while no blade is in it.
inline Vec3 RestMouth(const Frame& f) {
    return f.held ? f.anchor - f.rest_axis * f.hold : f.anchor;
}

// The two hands while the blade sits in the case in the left hand (2026-11-20: "if I move
// my right arm, I want it to move my left arm too and vice versa ... allow some range of movement
// but then make them move in tandem"). Within TandemSlackM / TandemSlackRad of seated the sword
// hand alone settles into the case; beyond, each hand takes half of the rest, so the blade stays
// seated and the two move as one; hands more than TandemMaxM out of place are not held together
// (each at most half of it from its controller). From TandemDepthM in, over TandemRampM.
// Visual only, as the guide below: the sheathing and the draw read the tracked hands.
inline constexpr float TandemSlackM = 0.03f;
inline constexpr float TandemSlackRad = 8.0f * 3.14159265f / 180.0f;
inline constexpr float TandemMaxM = 0.5f;
inline constexpr float TandemDepthM = 0.16f, TandemRampM = 0.08f;
// Eased in over TandemInS, out over TandemReleaseS (0.08 s until the user's "it jumps
// positions" when pulling the blade out, 2026-11-20 night).
inline constexpr float TandemInS = 0.10f;
inline constexpr float TandemReleaseS = 0.15f;
// The sword hand's share of `x` out of place: all of it within `slack`, half of the rest beyond.
inline float TandemShare(float x, float slack) {
    return x <= slack ? 1.0f : (slack + 0.5f * (x - slack)) / x;
}
struct Tandem {
    bool active{false};
    Body::QsTransform move{}; // model space: what the case and the left hand are moved by
    Body::QsTransform sword{}; // the sword where it is shown (move applied to `seat`)
};
// `sword`: the sword's frame from the tracked hand; `seat`: where it would be seated in the case
// (SwordInHeldCase); `scale`: the weapon's metres.
inline Tandem TandemPose(const Body::QsTransform& sword, const Body::QsTransform& seat, float scale) {
    namespace B = Body;
    const Vec3 e = seat.t - sword.t;
    const B::Quat r = B::Normalize(seat.q * B::Conjugate(sword.q));
    const float dist = B::Length(e) / std::max(scale, 1e-3f);
    const float angle = 2.0f * std::atan2(B::Length(Vec3{r.x, r.y, r.z}), std::abs(r.w));
    if (!(dist <= TandemMaxM) || !std::isfinite(angle)) {
        return {};
    }
    const float st = TandemShare(dist, TandemSlackM);
    const float sr = TandemShare(angle, TandemSlackRad);
    // The sword's own move: st of the way and sr of the turn, about its grip ...
    const B::Quat rq = B::ScaleRotation(r, sr);
    const B::QsTransform part{sword.t + e * st - B::Rotate(rq, sword.t), rq};
    // ... and the case's: that, after undoing the whole sword -> seat (the blade stays seated).
    const B::Quat rc = B::Conjugate(r);
    const Vec3 undo_t = -B::Rotate(rc, seat.t - B::Rotate(r, sword.t));
    Tandem out{};
    out.active = true;
    out.move = {part.t + B::Rotate(part.q, undo_t), B::Normalize(part.q * rc)};
    out.sword = B::Compose(out.move, seat);
    out.sword.s = sword.s;
    return out;
}

// The guide's capture and release (s).
inline constexpr float GuideEaseS = 0.12f;

// Visual correction only. Input and quick-draw speed always use uncorrected tracking.
// Keep the last correction relative to the live sword so release follows the swing immediately.
struct HeldSwordGuide {
    Body::QsTransform correction{};
    float release{};
    float capture{};

    bool Pose(const Frame& f, const Out& o, float aim, const Body::QsTransform& case_pose,
              const Body::QsTransform& tracked, float scale, Body::QsTransform& result) {
        if (!f.blade || !f.held) {
            release = capture = 0.0f;
            return false;
        }
        if (o.in || aim > 0.0f) {
            const Vec3 tip = f.grip + f.axis * (SeatY + BladeM) + f.side * Bend(BladeM);
            const float depth = o.in ? o.depth : Body::Dot(tip - RestMouth(f), f.rest_axis);
            const auto fitted = SwordInHeldCase(case_pose, depth, scale);
            // (Over GuideEaseS, 0.08 s until 2026-11-20 night: "it still jumps a tiny bit".)
            capture = std::min(o.in ? 1.0f : aim, capture + std::clamp(f.dt, 0.0f, 0.025f) / GuideEaseS);
            constexpr float align_depth = 0.16f;
            // With the case at an angle, forcing its exact rotation can put the hilt
            // far outside the sword hand's reach. Keep the steel crossing the rim, and
            // take up the remaining angle as the player's hands line up.
            const float allowance = 0.045f * scale;
            const float hilt_offset_sq = Body::LengthSq(fitted.t - tracked.t);
            if (depth >= align_depth && capture >= 1.0f && hilt_offset_sq <= allowance * allowance) {
                result = fitted;
            } else {
                // Guide the point on the steel that belongs at the rim. Blending the hilt
                // positions instead sweeps that point through an arc, away from the opening.
                const float u = BladeM - std::clamp(depth, 0.0f, BladeM);
                const Vec3 pivot = Vec3{Bend(u), -(SeatY + u), 0} * scale;
                const Vec3 lever = Body::Rotate(tracked.q, pivot);
                const Vec3 contact = Body::Lerp(tracked.t + lever,
                    fitted.t + Body::Rotate(fitted.q, pivot), capture);
                // At the rim keep the hilt near the controller. Ease into the authored
                // curve as actual inward travel gives the blade leverage inside the case.
                const auto aimed = Body::FromTo(lever, contact - tracked.t) * tracked.q;
                const float t = std::clamp(depth / align_depth, 0.0f, 1.0f);
                float align = capture * t * t * (3.0f - 2.0f * t);
                if (align > 0.0f && hilt_offset_sq > allowance * allowance) {
                    align *= allowance / std::sqrt(hilt_offset_sq);
                }
                result.q = Body::Slerp(aimed, fitted.q, align);
                result.t = contact - Body::Rotate(result.q, pivot);
            }
            correction = Body::LocalFromModel(tracked, result);
            release = 1.0f;
        } else {
            capture = 0.0f;
            release = std::max(0.0f, release - f.dt / GuideEaseS);
            if (release <= 0.0f) return false;
            const float weight = release * release * (3.0f - 2.0f * release);
            result = Body::Compose(tracked, Body::Blend({}, correction, weight));
        }
        result.s = tracked.s;
        return true;
    }
};

// The left hand on the scabbard: how far below the mouth it is along it, and whether it is at its
// upper part (within GrabM of the first GrabAlongM).
struct Reach {
    bool close{false};
    float along{0.0f};
};
inline Reach ReachOf(Vec3 hand, Vec3 mouth, Vec3 into) {
    const Vec3 to = hand - mouth;
    const float along = std::clamp(Body::Dot(to, into), 0.0f, GrabAlongM);
    return {Body::LengthSq(to - into * along) <= GrabM * GrabM, along};
}

// The left hand's hold: taken with a squeeze begun at the scabbard, kept until it opens.
// A hit or a dodge (the arm handed to the game's animation, its tracking lost a moment) keeps the
// scabbard in the hand and the blade in it, this long at most (2026-11-20: "if we get
// hit or dodge the left hand shouldn't drop it").
inline constexpr float HoldThroughS = 2.0f;

struct Grab {
    bool held{false};
    bool down{false};
    float lost{0.0f}; // s the arm has been away from the controller while held
    // True when it was taken in this step. `hand_ok`: the hand may hold it at all (no item, the
    // clean form); `tracked`: the arm follows its controller (else a hold already made is kept,
    // HoldThroughS at most, while the squeeze holds).
    bool Step(bool close, float squeeze, bool hand_ok, bool tracked = true, float dt = 0.0f) {
        const bool was = down;
        down = down ? squeeze > GrabRelease : squeeze >= GrabPress;
        if (!hand_ok || !down) {
            held = false;
            lost = 0.0f;
            return false;
        }
        if (!tracked) {
            lost += std::max(dt, 0.0f);
            if (lost > HoldThroughS) held = false;
            return false;
        }
        lost = 0.0f;
        if (!held && !was && close) {
            held = true;
            return true;
        }
        return false;
    }
};

class Sheath {
public:
    // `draw_speed`: m/s out of the mouth from which a seated blade's leaving is the quick draw.
    Out Step(const Frame& f, float draw_speed) {
        Out out{};
        if (!f.blade || !f.scabbard || !(f.dt > 0.0f)) {
            // A hit or a dodge with the scabbard in the left hand: a blade resting in it (not on
            // its way out) stays in it a moment (HoldThroughS), nothing measured meanwhile; the
            // first look back is no draw.
            if (in_ && f.held && depth_ >= deepest_ - 0.05f && f.dt > 0.0f &&
                lost_ + f.dt <= HoldThroughS) {
                lost_ += f.dt;
                out.in = true;
                out.depth = depth_;
                out.loaded = loaded_;
                out.build = loaded_ ? 1.0f : std::clamp(depth_ / SeatM, 0.0f, 1.0f);
                return out;
            }
            const bool was = in_;
            Reset();
            out.event = was ? Event::Slipped : Event::None;
            return out;
        }
        const bool back = lost_ > 0.0f;
        lost_ = 0.0f;
        const Vec3 tip = f.grip + f.axis * (SeatY + BladeM) + f.side * Bend(BladeM);
        const float capture = f.held ? HeldMouthM : MouthM;
        if (!in_) {
            const Vec3 mouth = RestMouth(f);
            // The tip's speed against the mouth (the body carries both).
            const Vec3 rel = tip - mouth;
            const Vec3 motion = rel - rel_;
            const float max_step = EntrySpeed * f.dt;
            const float inward = Body::Dot(motion, f.rest_axis);
            // Switching between hand and hip establishes a new mouth position.
            const bool same_mount = have_rel_ && span_held_ == f.held;
            const bool feeding = same_mount && inward >= AssistInwardSpeed * f.dt;
            const float motion_sq = Body::LengthSq(motion);
            const bool approaching = !same_mount || (inward >= 0.0f && motion_sq <= max_step * max_step);
            rel_ = rel;
            have_rel_ = true;
            const float distance_sq = Body::LengthSq(rel);
            const bool in_capture = distance_sq <= capture * capture;
            const float assist = f.held ? HeldAssistM : WaistAssistM;
            const bool at_rim = in_capture || (feeding && distance_sq <= assist * assist);
            // Reject distant poses before doing any fit.
            const float reach = assist + (EntrySteelM + EntrySteelBendM);
            const float tip_depth = Body::Dot(rel, f.rest_axis);
            if (wait_for_clear_ && !in_capture) {
                // A rejected held fit must clear the contact before it can catch again.
                wait_for_clear_ = f.held && distance_sq <= reach * reach &&
                                  SteelAtMouth(f, rel, tip_depth, assist);
            }
            const bool near_steel = distance_sq <= reach * reach && feeding &&
                                    inward * inward >= 0.0625f * motion_sq; // >= 1/4 inward
            const bool eligible = (at_rim || near_steel || (f.held && distance_sq < GuideM * GuideM)) &&
                                  !wait_for_clear_ && approaching && Body::Dot(f.axis, f.rest_axis) >= EntryCos;
            aim_ = 0.0f;
            if (f.held && eligible && tip_depth <= 0.0f) {
                const float proximity = std::clamp((GuideM - std::sqrt(distance_sq)) /
                                                   (GuideM - HeldMouthM), 0.0f, 1.0f);
                aim_ = proximity * proximity * (3.0f - 2.0f * proximity);
            }
            // Prefer the ordinary tip catch. The extra steel needs a little translation
            // from either hand: wrist rotation alone can sweep the tip a long way. The span
            // is only compared on the same mount: the held anchor is the left hand's grip,
            // HoldY below the mouth, the hip's is the mouth, so letting go is no feed.
            const Vec3 span = f.anchor - f.grip;
            const bool tip_inside = at_rim && (f.held ? tip_depth >= -1e-5f : std::abs(tip_depth) <= MouthM);
            if (eligible && (tip_inside ||
                             (near_steel &&
                              Body::Dot(hand_span_ - span, f.rest_axis) >=
                                  0.5f * AssistInwardSpeed * f.dt &&
                              SteelAtMouth(f, rel, tip_depth, assist)))) {
                in_ = true;
                loaded_ = bottomed_ = false;
                peak_ = 0.0f;
                const Fit fit = f.held ? Fit{tip_depth, 0.0f} : FitOf(f);
                depth_ = deepest_ = std::clamp(fit.depth, 0.0f, BladeM);
                // A wide waist catch can start beyond the usual slip line. Give just that
                // catch room to slide; centred draws keep their original release distance.
                constexpr float ordinary_offset = LineM - SlipMarginM;
                if (fit.off_sq > ordinary_offset * ordinary_offset) {
                    const float line = std::sqrt(fit.off_sq) + SlipMarginM;
                    slip_line_sq_ = line * line;
                } else {
                    slip_line_sq_ = LineM * LineM;
                }
                last_axis_ = f.held ? f.rest_axis : f.axis;
                if (f.held) {
                    // Measure from this catch, wherever the two grips and the rim met.
                    // Either hand can feed the blade by closing the distance between them.
                    held_distance_origin_ = depth_ + Body::Length(span);
                    held_axis_origin_ = tip_depth + Body::Dot(span, f.rest_axis);
                    closing_by_distance_ = false;
                } else {
                    // At an oblique catch the tip can be near the rim yet short of it
                    // along the blade. A repeated pose there must not count as withdrawal.
                    waist_exit_depth_ = std::min(-MouthM, fit.depth) - OutM;
                }
                held_ = f.held;
                out.event = Event::Entered;
            }
            hand_span_ = span;
            span_held_ = f.held;
        } else {
            if (f.held != held_) {
                Reset();
                out.event = Event::Slipped;
                return out;
            }
            const Vec3 span = f.anchor - f.grip;
            // Measure translation against the previous blade direction. Turning the wrist alone
            // or switching anchors must not generate draw speed.
            const Vec3 motion = span - hand_span_;
            const float axial = Body::Dot(motion, last_axis_);
            const Vec3 previous_axis = last_axis_;
            float travel = axial;
            Fit fit{};
            if (f.held) {
                const float by_distance = held_distance_origin_ - Body::Length(span);
                if (!closing_by_distance_) {
                    // Keep an already straight slide (including an arcing withdrawal) on
                    // its rail. Rotation of the case by itself cannot advance that rail.
                    const float swept = axial + Body::Dot(span, f.rest_axis - last_axis_);
                    travel = std::clamp(swept, std::min(0.0f, axial), std::max(0.0f, axial));
                    held_axis_origin_ += swept - travel;
                    fit.depth = held_axis_origin_ - Body::Dot(span, f.rest_axis);
                    // When the hands have genuinely closed farther than the rail credits,
                    // follow their separation for the rest of this catch, in both directions.
                    // A sideways tug or wrist jitter cannot switch a straight draw.
                    if (by_distance > fit.depth + 0.015f && by_distance > depth_) {
                        closing_by_distance_ = true;
                    }
                }
                if (closing_by_distance_) {
                    fit.depth = by_distance;
                    travel = depth_ - std::clamp(fit.depth, 0.0f, BladeM);
                }
            } else {
                fit = FitOf(f);
            }
            float depth = std::min(fit.depth, BladeM);
            if (f.held && travel > 0.0f)
                travel = std::min(travel, std::max(0.0f, depth_ - depth));
            const float leaving = travel / f.dt;
            hand_span_ = span;
            span_held_ = f.held;
            last_axis_ = f.held ? f.rest_axis : f.axis;
            // Multiple body poses can use the same tracking sample. A stationary copy does
            // not interrupt an outward stroke, but a pause or reversal does.
            if (loaded_ && leaving >= 0.5f * draw_speed) {
                fast_travel_ += travel;
                pause_ = 0.0f;
            } else if (fast_travel_ > 0.0f &&
                       ((pause_ += f.dt) >= 0.06f || leaving < -0.5f * draw_speed)) {
                fast_travel_ = 0.0f;
            }
            peak_ = std::max(leaving, peak_ * std::exp(-f.dt / PeakS));
            // The first look back after a hit or a dodge (HoldThroughS): its jump is no pull. It is
            // the new baseline, and only fresh outward motion after it counts (a code review,
            // 2026-11-20: the next pose would otherwise lunge the 1.4 m dash, the hands still).
            if (back) {
                fast_travel_ = 0.0f;
                peak_ = 0.0f;
                finish_speed_ = 0.0f;
            }
            const bool pulling = fast_travel_ >= 0.12f && peak_ >= draw_speed;
            // A real draw can turn into a slash just before the tip clears the rim.
            // Finish the last few centimetres along the case at the measured pull speed.
            // A sideways tug from rest cannot start this, even with a loaded blade.
            if (f.held && finish_speed_ == 0.0f && pulling && depth > 0.0f && depth <= 0.18f &&
                travel >= 0.5f * draw_speed * f.dt &&
                Body::LengthSq(motion - previous_axis * axial) >= f.dt * f.dt) {
                finish_speed_ = peak_;
            }
            if (finish_speed_ > 0.0f) {
                if (leaving < -0.5f * draw_speed) finish_speed_ = 0.0f;
                else depth = std::min(depth, depth_ - finish_speed_ * f.dt);
            }
            depth_ = std::max(depth, 0.0f);
            deepest_ = std::max(deepest_, depth_);
            if (loaded_ && !lunge_sent_ && pulling && deepest_ > 0.0f &&
                deepest_ - depth_ >= LungeShare * deepest_ && !back) {
                out.lunge = lunge_sent_ = true;
            }
            const bool at_mouth = !f.held && deepest_ < 3.0f * OutM && depth >= waist_exit_depth_;
            // A cutting draw arcs out of the case. Accept this departure after a real pull,
            // even if the tip crosses the side boundary before it crosses the mouth plane.
            const bool off_line = fit.off_sq > slip_line_sq_;
            const bool curved_exit = (finish_speed_ > 0.0f && depth <= 0.0f) ||
                                     (pulling && (off_line || depth <= (f.held ? 0.0f : 0.08f)));
            if (off_line && !curved_exit) {
                Reset();
                out.event = Event::Slipped;
                return out;
            }
            // Lead the splash by up to 18 ms of measured outward travel. A slow draw
            // still gushes at exit. Neither a pause nor a reversal can replay the cue.
            const float lead = std::min(0.055f, std::max(leaving, finish_speed_) * 0.018f);
            if (pulling && !burst_sent_ && lead > 0.0f &&
                depth <= (f.held ? 0.0f : 0.08f) + lead) {
                out.burst = burst_sent_ = true;
            }
            // Give a newly caught tip room for a small hand adjustment before backing out.
            const float exit_margin = f.held && deepest_ < 0.04f ? 0.04f : OutM;
            if ((depth < -exit_margin && !at_mouth) || curved_exit) {
                out.event = Event::Left;
                out.speed = std::max(peak_, finish_speed_);
                out.draw = loaded_ && out.speed >= draw_speed && !back;
                out.power = out.draw ? std::clamp((out.speed - draw_speed) /
                                                      std::max(DrawFullSpeed - draw_speed, 0.1f),
                                                  0.0f, 1.0f)
                                     : 0.0f;
                out.loaded = loaded_;
                out.burst |= loaded_ && !burst_sent_;
                Reset();
                rel_ = tip - RestMouth(f);
                have_rel_ = true;
                // An assisted exit can finish at the rim. A repeated pose there must not
                // immediately seize the blade again while the player finishes the cut.
                wait_for_clear_ = Body::LengthSq(rel_) <= capture * capture;
                return out;
            }
            if (!loaded_ && depth >= SeatM) {
                loaded_ = true;
                out.event = Event::Seated;
            } else if (!bottomed_ && depth >= BladeM - BottomM) {
                bottomed_ = true;
                out.event = Event::Bottomed;
            } else if (bottomed_ && depth < BladeM - 3.0f * BottomM) {
                bottomed_ = false; // the next time it is pushed home clacks again
            }
        }
        out.in = in_;
        out.depth = in_ ? depth_ : 0.0f;
        out.loaded = loaded_;
        out.build = !in_ ? 0.0f : loaded_ ? 1.0f : std::clamp(depth_ / SeatM, 0.0f, 1.0f);
        return out;
    }

    void Reset() {
        in_ = loaded_ = bottomed_ = have_rel_ = wait_for_clear_ = burst_sent_ = lunge_sent_ = false;
        depth_ = deepest_ = peak_ = fast_travel_ = pause_ = finish_speed_ = 0.0f;
        aim_ = 0.0f;
    }

    float AimWeight() const { return aim_; }

    void CancelHeld() {
        Reset();
        wait_for_clear_ = true;
    }

    bool In() const {
        return in_;
    }
    bool Loaded() const {
        return loaded_;
    }
    float Depth() const {
        return depth_;
    }

private:
    bool in_{false};
    bool loaded_{false};
    bool bottomed_{false};
    bool burst_sent_{false};
    bool have_rel_{false};
    bool wait_for_clear_{false};
    float depth_{0.0f};
    float slip_line_sq_{LineM * LineM};
    float waist_exit_depth_{};
    float held_distance_origin_{0.0f}, held_axis_origin_{0.0f};
    bool closing_by_distance_{false};
    float deepest_{0.0f};
    float peak_{0.0f};
    float finish_speed_{0.0f};
    float fast_travel_{0.0f};
    float pause_{0.0f};
    bool held_{false};
    Vec3 hand_span_{}, last_axis_{};
    bool span_held_{false}; // the mount (f.held) hand_span_ was measured on
    Vec3 rel_{};
    float aim_{};
    float lost_{0.0f}; // s the hands have been away while it is in (HoldThroughS)
    bool lunge_sent_{false};
};

// The quick draw opens the form's charged row at once, whatever the stroke (the clean blade's
// judge 170: 185, its stagger, stamina 60; the game's own quick draw, the blooded form's L2, is
// 192 for 50). That window's hits x this: all of it at a full-speed draw, DrawSlowShare of it at
// chikage_draw_speed. The blood's own bonus comes on top (BloodScale).
inline constexpr float DrawSlowShare = 0.7f;
// (The blooded form's charged row is 225: its draw is held to the game's own 192.)
inline constexpr float BloodedDrawShare = 192.0f / 225.0f;
inline float DrawDamageScale(float power, bool blooded_form = false) {
    return (DrawSlowShare + (1.0f - DrawSlowShare) * std::clamp(power, 0.0f, 1.0f)) *
           (blooded_form ? BloodedDrawShare : 1.0f);
}

// A blade this deep is in the scabbard for the swings (none is an attack meanwhile); a tip only
// resting at the mouth is not (a guard held across the hip must not eat the next swing).
inline constexpr float SheathedM = 0.03f;
// The quick draw's window opens within this of its event, or not at all (a draw as a menu opens
// must not strike when it closes).
inline constexpr std::int64_t DrawFreshNs = 250'000'000;
inline bool FreshDraw(std::int64_t pose, std::int64_t now, std::int64_t reset,
                      std::int32_t source_weapon, std::int32_t current_weapon) {
    return source_weapon >= 0 && source_weapon == current_weapon && pose > reset &&
           now >= pose && now - pose < DrawFreshNs;
}

// A recognized draw can cross the headset's face zone on its way out. Give the normal swing
// gate a few frames to reopen. The grace runs from the original draw, never from a retry.
enum class DrawRequest : std::uint8_t { None, New, WaitingForFace };
inline constexpr std::int64_t DrawFaceGraceNs = 70'000'000;
inline bool TakeDraw(DrawRequest& request, std::int64_t age_ns, bool swings,
                     bool face_block, bool can_follow) {
    const bool within_grace = can_follow && age_ns >= 0 && age_ns < DrawFaceGraceNs;
    const bool eligible = request == DrawRequest::New ||
                          (request == DrawRequest::WaitingForFace && within_grace);
    request = eligible && !swings && face_block && within_grace
                  ? DrawRequest::WaitingForFace : DrawRequest::None;
    return eligible && swings;
}
// The blade sliding in the scabbard: the sword hand's rasp, by its speed (m/s) along it.
inline float SlideRasp(float speed) {
    return std::clamp(std::abs(speed) * 0.22f, 0.0f, 0.32f);
}

// The scabbard's blood on the clean blade: it lasts this long, or until a hit lands with it.
inline constexpr float BloodS = 12.0f;
inline float BloodScale(float pct) {
    return 1.0f + std::clamp(pct, 0.0f, 100.0f) * 0.01f;
}
// The game's own burst of blood at the guard as the blade leaves the scabbard: the effect its
// transform plays on the clean blade's dummy (a000_19020's SpawnFFX 7107 on 10120), in a slot of
// our own for the blood's first BloodBurstS. It is a short gush, not a coat: the coat is ours.
inline constexpr std::int32_t BloodFfx = 7107;
// (7107 is a red flare more than flying blood: the tester, 2026-11-20 night, "looks like a red lens
// flare very strong one. I don't mind it I just need it smaller": one of it, mid-blade, for
// FlareS; then "I'd like it to be 3": one on each of the blade's three dummies). The flying blood is the game's other Chikage blood, the B->A transform's shake-off
// (a000_19230's SpawnFFX 7108), off the tip and mid-blade: on the draw and on every hit.)
inline constexpr float FlareS = 0.6f;
inline constexpr std::int32_t FlingFfx = 7108;
// Three locations on the blade itself: lower blade, mid-blade and tip.
inline constexpr std::array<std::int32_t, 3> BloodDummies{10110, 10130, 10100};
// The draw's red flare: on these of BloodDummies (2026-11-26: "the chikage has a 3x
// glow when it is pulled from the scabbard, can we make that be 2x instead of 3x?"): mid-blade
// and the tip, out in front. (All three until then; {0, 1, 2} brings the third back.)
inline constexpr std::array<std::size_t, 2> FlareAt{1, 2};
inline constexpr std::array<std::int32_t, 3> BloodSlots{30320, 30322, 30321};
// (1.2 s, the mid-blade emitter 0.22 s, until 2026-11-20, the user: "MORE BLOOD": all three for
// most of it, and longer.)
// (1.8 s / 0.9 s until later that evening: "add more blood splashing when we pull it out": all
// three the whole burst, and the same three again in slots of their own for its first DoubleGushS.)
inline constexpr float BloodBurstS = 2.2f;
inline constexpr float ExtraGushS = 2.2f; // the mid-blade emitter
inline constexpr float DoubleGushS = 0.8f;
inline constexpr std::array<std::int32_t, 3> DoubleSlots{30323, 30324, 30325};
// Every Chikage hit that lands (2026-11-20: "make the chikage bloody the enemies more
// with its attacks"): the same gush off the blade, in the enemy at that moment, for HitGushS x
// (1 + level) / 2. Level 1 a light hit (the tip and the guard), 2 a heavy (all three), 3 a charged
// one or the quick draw (all three, twice over). bloodborne_melee.cpp MeleeDamage notes it,
// bloodborne_transform.cpp plays it.
inline constexpr float HitGushS = 0.3f;
inline std::atomic<std::int64_t> g_hit_ns{0};
inline std::atomic<int> g_hit_level{0};
inline void NoteHit(std::int64_t now, int level) {
    g_hit_level.store(level, std::memory_order_relaxed);
    g_hit_ns.store(now, std::memory_order_release);
}
// A quick draw whose attack did damage (bloodborne_melee.cpp MeleeDamage): the scabbard goes back
// to the hip, the left hand free for the gun (2026-11-20: "after a succesful attack from
// the scabbard ... I want the scabbard going away, but if it's a miss we are still holding it").
inline std::atomic<std::int64_t> g_draw_hit_ns{0};
inline float HitGushFor(int level) { // 0.3 / 0.6 / 0.9 s ("a lot of blood ... with the scabbard")
    return HitGushS * static_cast<float>(std::clamp(level, 1, 3));
}
// The game's own blood coating while the scabbard's blood lasts (2026-11-20: "apply the
// blood aura effect the game has in base game"): SpEffect 204, what the transform a000_19020 adds
// (SpEffectVfx 204: sfx 20242 on the weapon, a weapon-enchant look). It has no stats and no HP
// drain (that is 6740, valid in form B only); its endurance is 0.03 s, so it is added again
// whenever it is gone, as the clip's event does.
inline constexpr std::int32_t AuraSpEffect = 204;
// The quick draw's capsule: at least this wide (the Chikage's swings are 14 cm round the steel;
// the tester, 2026-11-20: the draw "I hear the sound, and the enemy is not hit": 1 hit in 10 draws).
// (0.26 until that night's run measured every draw's nearest enemy: 3 hits within 0-20 cm, 3 misses
// that passed 32-45 cm off one, 4 with none within 4 m: 0.40 takes those near misses.)
inline constexpr float DrawRadiusM = 0.42f; // (0.40 for one build; the tester: "let's do 42cm")
// ... widened by the swing (2026-11-20 night: "IT SHOULD NOT MISS", "it needs to
// properly account for the swing"): `travel_m` is how far the blade moved since the game's last
// test of the capsule; DrawRadiusM + half of it leaves no gap between two tests' capsules, so the
// whole band the blade swept is covered at DrawRadiusM. At most DrawSweepMaxM.
inline constexpr float DrawSweepMaxM = 1.3f;
inline float DrawSweepRadius(float travel_m) {
    return std::min(DrawRadiusM + 0.5f * std::max(travel_m, 0.0f), DrawSweepMaxM);
}

// The sword's own voice, from the game's transform clips (a000_19020's PlaySound events, c-sounds
// played at the weapon): the blade going home in its scabbard, and coming out of it with its
// blood.
inline constexpr std::int32_t SheatheSound = 114426000;
inline constexpr std::int32_t DrawSound = 114426200;

// The coat on the steel (the blade's own model drawn again over it, as the split blades' hone
// shine: bloodborne_quickitems.cpp AddBladeShine, vr_inframe.frag): how much of it shows `left`
// s before the blood is gone: in over BloodInS, thinning over its last BloodOutS.
inline constexpr float BloodInS = 0.25f, BloodOutS = 2.0f;
// The draw's glint on the coat (2026-11-20: "a bit more.. flashy? like to feel cooler";
// later: "too blinding", "dimmer and a little smaller"): for its first BloodFlashS a thin wet
// highlight runs along the steel's edges from mid-blade to the tip (BloodSweepS), a small bead
// there as it ends; lit by the scene only, no light of its own (vr_inframe.frag uv.x -2092).
// (0.6 s / 0.3 s until 2026-11-20 night: a glint, quick, not a glow.)
inline constexpr float BloodFlashS = 0.32f, BloodSweepS = 0.22f;
// ... from this long after the blood came: the blade out in front, not still at the hip (the user,
// 2026-11-20: "fire a little bit forward ... in front of me").
inline constexpr float BloodFlashDelayS = 0.08f;
// The flash's strength (0 .. 1) and where its band is along the blade (0 the guard .. 1 the tip,
// a little past it once done), `since` s after the blood came.
struct BloodFlashState {
    float strength{0.0f};
    float sweep{-1.0f};
};
inline BloodFlashState BloodFlash(float since) {
    if (!(since >= 0.0f) || since >= BloodFlashS) {
        return {};
    }
    const float fade = 1.0f - since / BloodFlashS;
    return {fade * fade * (3.0f - 2.0f * fade), std::min(since / BloodSweepS, 1.2f)};
}
inline float BloodCoat(float left) {
    if (!(left > 0.0f)) {
        return 0.0f;
    }
    return std::clamp((BloodS - left) / BloodInS, 0.0f, 1.0f) *
           std::clamp(left / BloodOutS, 0.0f, 1.0f);
}

} // namespace VR::Chikage
