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
#include <bitset>
#include <cmath>
#include <cstddef>
#include <optional>

#include "core/vr/arm_ik.h"
#include "core/vr/body_frame.h"
#include "core/vr/c0000_skeleton.h"

// VR body: one pose copy's edits of the hunter's local pose, in this order (all on a host copy of
// the 108 animated locals; the caller writes back only the bones in BodyOutput::written):
//  1. Torso yaw. The torso turns from the legs by your head's excess over a deadzone from the
//     legs' heading (TorsoYaw::UpdateRelative) and turns UpperTurn (43) by delta = clamp(that,
//     +-torso_max) * body weight, about model up. The legs keep the game's heading.
//  2. Upper body under the eye (BodyParams::posture). The hunter's eye (dummy 58 midpoint on Head,
//     host FK without the head's scale) is brought to the camera's mono eye, body_back metres
//     behind it along the torso's forward, low-passed (align_tau), times the body weight.
//     Lean (the default): the trunk leans from the waist (UpperTurn, Spine, Spine1, Spine2 turn
//     about the torso's right and forward axes, LeanShare) within lean_*_max of vertical (forward:
//     more in a duck, LeanForwardLimit); height
//     differences within vertical_deadband are left alone and the rest moves UpperPos (42) down
//     (never up by default). Shift (the old way): UpperPos slides by the whole offset,
//     horizontally at most align_max, vertically -0.6 .. +0.15 m.
//  3. Arms. Both arms through SolveArm (core/vr/arm_ik.h), effector = the weapon bone on the
//     controller grip, with the body-feel design's parameters: the elbow hint from where the hand
//     is (ArmPole), 15% clavicle reach (down x0.4; a controller still out of reach turns it up to
//     shoulder_reach further), 10-150 degrees of elbow flexion, 10% stretch,
//     the wrist roll and swing turning the elbow (0.35, at most 75 degrees), forearm twist helpers
//     1.0 / 0.5, elbow and shoulder helpers 0.5. Each arm blends with the game's animation by its
//     weight. Natural arms (BodyParams::natural_arms, the default since 2026-10-08): only the roll
//     beyond the forearm's range (forearm_pronation / forearm_supination) turns the elbow, 0.8 of
//     it, at most 100 degrees; the twist helpers follow continuously up to wrist_twist_max; the
//     across-the-chest hint hangs lower; the elbows stay out of the trunk (TrunkKeepOut).
//  4. Hiding, last: local scale 0.01 on Neck and Head (no_head); Spine 0.01 with the clavicles and
//     axillas x100 (arms_only: the arms keep their size, the torso, head and back weapons vanish).
// Weights gate posture/arms; rig_offset independently translates the entire hunter.
// When BodyOutput::ok is false (a non-finite input, pose or
// result) the host copy may be partly edited and nothing may be written.
//
// Pure (no emulator state): core/vr/bloodborne_body.cpp runs it at the pose copy, the unit tests
// (tests/test_body_solver.cpp) on the c0000 reference pose.
namespace VR::Body {

// ---- Torso yaw -------------------------------------------------------------------------------

// The torso's yaw. Relative (UpdateRelative, the default since 2026-10-05): `yaw` is the torso's
// turn from the legs (the character's heading): 0 while the head looks within `deadzone` of the
// legs' heading, beyond it the excess, followed with time constant `tau`; when the legs turn with
// the view (a snap turn, the VR heading) the torso comes with them. World (Update, the old
// behaviour): `yaw` is the torso's world heading, which stays put while the head looks around
// within `deadzone` of it and outside follows the head until the head is back at the deadzone's
// edge; after a snap turn it stayed up to the deadzone (25 degrees) behind the legs, twisting the
// waist until the head turned further.
struct TorsoYaw {
    bool valid{false};
    float yaw{0.0f};

    void Reset(float value) {
        valid = true;
        yaw = WrapPi(value);
    }
    // `head`: the head's yaw from the legs (radians). Returns the torso's turn from the legs.
    float UpdateRelative(float head, float deadzone, float tau, float dt) {
        if (!valid) {
            Reset(0.0f);
        }
        const float h = WrapPi(head);
        const float target = h > deadzone ? h - deadzone : (h < -deadzone ? h + deadzone : 0.0f);
        const float k = tau > 0.0f ? 1.0f - std::exp(-std::max(dt, 0.0f) / tau) : 1.0f;
        yaw = WrapPi(yaw + WrapPi(target - yaw) * k);
        return yaw;
    }
    float Update(float head, float deadzone, float tau, float dt) {
        if (!valid) {
            Reset(head);
        }
        const float d = WrapPi(head - yaw);
        const float excess = d > deadzone ? d - deadzone : (d < -deadzone ? d + deadzone : 0.0f);
        if (excess != 0.0f) {
            const float k = tau > 0.0f ? 1.0f - std::exp(-std::max(dt, 0.0f) / tau) : 1.0f;
            yaw = WrapPi(yaw + excess * k);
        }
        return yaw;
    }
};

// ---- Arms ------------------------------------------------------------------------------------

// The body frame turned by `yaw` (radians) about model up.
inline BodyFrame TurnedFrame(float yaw) {
    const Quat q = AxisAngle({0.0f, 1.0f, 0.0f}, yaw);
    BodyFrame f{};
    f.right = Rotate(q, f.right);
    f.forward = Rotate(q, f.forward);
    return f;
}

// Elbow hint (model space, from the shoulder) by where the hand is, from the body-feel design. In
// the torso frame (out to the arm's side, up, forward):
//   neutral (0.7, -1.0, -0.3); hand above the shoulder (1.0, -0.2, 0); above and behind the head
//   (0.6, 0.6, 0.6); across the body, more than 0.10 m past the midline (0.2, -0.6, 0.8; natural:
//   (0.3, -1.0, 0.45), the human reference's folded elbows); behind the back (0.9, -0.3, -0.6).
// Blended with smoothsteps of the hand's height over the shoulder, its distance behind it and how
// far it is past the midline, so the hint moves continuously. A hand close in front of the chest
// (less than 0.25-0.40 m in front of the shoulder, and below it: fading out from 0.08 m under the
// shoulder to 0.02 m over it) takes the across hint from the midline on (0-0.12 m past it): arms
// folded over the chest keep their elbows in front of it instead of down at its depth, while a
// hand raised across the face or over the head keeps its elbow out to the side, out of the view.
// `side` = +1 right arm, -1 left. SolveArm turns it further by the wrist roll and swing and
// low-passes the elbow side.
inline Vec3 ArmPole(const BodyFrame& f, float side, Vec3 hand_from_shoulder, Vec3 hand_from_spine,
                    bool natural = false) {
    const Vec3 out = f.right * side;
    const float up = Dot(hand_from_shoulder, f.up);
    const float forward = Dot(hand_from_shoulder, f.forward);
    const float across = -Dot(hand_from_spine, out);
    const float w_up = Smoothstep(-0.05f, 0.15f, up);
    const float w_back = Smoothstep(0.05f, 0.20f, -forward);
    const float near_chest = Smoothstep(0.0f, 0.12f, across) *
                             (1.0f - Smoothstep(0.25f, 0.40f, forward)) *
                             (1.0f - Smoothstep(-0.08f, 0.02f, up));
    const float w_across = std::max(Smoothstep(0.10f, 0.25f, across), near_chest) * (1.0f - w_back);
    constexpr Vec3 neutral{0.7f, -1.0f, -0.3f};
    constexpr Vec3 raised{1.0f, -0.2f, 0.0f};
    constexpr Vec3 behind_head{0.6f, 0.6f, 0.6f};
    // Natural: arms folded across the chest hang their elbows lower and less far forward.
    const Vec3 across_body = natural ? Vec3{0.3f, -1.0f, 0.45f} : Vec3{0.2f, -0.6f, 0.8f};
    constexpr Vec3 behind_back{0.9f, -0.3f, -0.6f};
    const Vec3 low = Lerp(neutral, behind_back, w_back);
    const Vec3 high = Lerp(raised, behind_head, w_back);
    const Vec3 p = Lerp(Lerp(low, high, w_up), across_body, w_across);
    return out * p.x + f.up * p.y + f.forward * p.z;
}

// The hunter's trunk for the natural arms' elbows (ArmParams::keepOut): an elliptic cylinder along
// the spine from 0.15 m under Spine (the waist) to the shoulders' height (0.44 m over it, faded in
// over 8 cm at each end), its axis 2.5 cm in front of the spine bones, keeping the elbow joint's
// centre 0.20 m to either side and 0.145 m in front of or behind it (his chest and coat plus the
// arm's own thickness at the elbow; the human reference's elbows, arms folded over the chest or
// hanging at the sides, all lie outside it).
inline ElbowKeepOut TrunkKeepOut(Vec3 spine, Vec3 neck, const BodyFrame& f) {
    ElbowKeepOut k{};
    k.enabled = true;
    k.base = spine;
    k.up = Normalize(neck - spine, f.up);
    k.right = Normalize(Perp(f.right, k.up), f.right);
    k.forward = Cross(k.right, k.up);
    return k;
}

// The arm solver's parameters for the hunter (body-feel design; arm_ik.h defaults where they
// agree): the pole is set per frame.
inline ArmParams HunterArmParams(bool natural = false) {
    ArmParams p{};
    if (natural) {
        p.naturalRoll = true;
        p.trackTwist = true;
        p.maxTwistRate = 720.0f * Deg;
        p.maxWristTwist = 150.0f * Deg;
        p.swingTuckScale = 1.0f;
    }
    p.limits.minFlex = 10.0f * Deg;
    p.limits.maxFlex = 150.0f * Deg;
    p.limits.maxStretch = 0.10f;
    p.limits.maxTargetFactor = 2.25f;
    p.clavicleReach = 0.15f;
    p.clavicleStart = 0.5f;
    p.clavicleRange = 0.85f;
    p.clavicleDownScale = 0.4f;
    p.rollToPole = 0.35f;
    p.maxPoleTurn = 75.0f * Deg;
    return p;
}

// ---- Hiding ----------------------------------------------------------------------------------

enum class HideMode : unsigned char {
    None,     // nothing hidden (debug)
    Head,     // no_head: Neck and Head
    ArmsOnly, // arms_only: Spine (the torso, head and back weapons), the arms kept at full size
};

constexpr float HiddenScale = 0.01f;   // never 0: later inverses would divide by it
constexpr float RestoreScale = 100.0f; // a child of a hidden bone back to full size

struct HiddenBone {
    int bone;
    float scale; // local scale written
};

// The bones a mode writes and their local scales.
inline std::array<HiddenBone, 5> HiddenBones(HideMode mode, int& count) {
    namespace C = C0000;
    switch (mode) {
    case HideMode::Head:
        count = 2;
        return {{{C::Neck, HiddenScale}, {C::Head, HiddenScale}, {}, {}, {}}};
    case HideMode::ArmsOnly:
        count = 5;
        return {{{C::Spine, HiddenScale},
                 {C::L_Clavicle, RestoreScale},
                 {C::R_Clavicle, RestoreScale},
                 {C::Support_L_Axilla, RestoreScale},
                 {C::Support_R_Axilla, RestoreScale}}};
    case HideMode::None:
        break;
    }
    count = 0;
    return {};
}

// ---- One pose copy ---------------------------------------------------------------------------

// How the upper body is brought under the eye (step 2).
enum class PostureMode : unsigned char {
    // The whole upper body (UpperPos) slides against the hips: the eye lands on the camera, the
    // waist shears (the chest pushed back over the hips walking forward, squeezed into them when
    // the eye is lower). The behaviour before 2026-10-05.
    Shift,
    // The trunk leans from the waist (UpperTurn, Spine, Spine1, Spine2 turn about the torso's
    // right and forward axes, a smooth curve) to bring the eye over the camera's eye; nothing
    // slides, the trunk keeps its length. Height differences within `vertical_deadband` are left
    // alone (the walk's own bob, the hunter's eye a few cm above the camera); beyond it the trunk
    // still moves down / up by the rest (ducking, standing up from a crouch).
    Lean,
};

struct BodyParams {
    bool torso_follow{true};
    bool torso_relative{true}; // TorsoYaw::UpdateRelative (false: the old world-anchored filter)
    float torso_deadzone{25.0f * Deg};
    float torso_max{75.0f * Deg};
    float torso_tau{0.25f}; // s
    bool align{true};
    PostureMode posture{PostureMode::Lean};
    float align_max{0.20f}; // m, horizontal (Shift)
    // m, the deepest UpperPos goes down (both postures); bloodborne_body.cpp raises it to the
    // camera's deepest drop (head_room.h, positional_limit_down_m: 0.80 m by default).
    float align_down{0.60f};
    float align_up{0.15f};
    float back{0.0f};       // m behind the eye along the torso's forward
    float align_tau{0.05f}; // s
    // Lean: the trunk (waist -> neck) never leans further than this forward, backward and
    // sideways (radians, from vertical) by the lean; the animation's own lean beyond them is left
    // as it is. A horizontal offset the limits leave stays (the eye residual).
    float lean_fwd_max{35.0f * Deg};
    float lean_back_max{20.0f * Deg};
    float lean_side_max{20.0f * Deg};
    // Lean, a duck (DuckWeight): a real duck bends at the hips and brings the head all the way
    // down. With the camera's eye more than lean_low_from below the animation's eye the forward
    // limit rises toward lean_fwd_low_max and the vertical deadband fades out, all of it from
    // lean_low_full (smoothstep, so nothing steps): the shoulders stay under a head that is low
    // and forward and his eyes come down to yours (the hands on the controllers). Standing (the
    // eye within lean_low_from of his, or above it: a walk or run) nothing changes.
    // lean_fwd_low_max <= lean_fwd_max: no duck posture (the old limit and deadband), bit for bit.
    float lean_fwd_low_max{70.0f * Deg};
    float lean_low_from{0.10f}; // m
    float lean_low_full{0.35f}; // m
    // Lean: height offsets of the eye within this are not followed (m); the rest is, low-passed
    // with vertical_tau: down at most align_down, up at most lean_up_max (the walk and run
    // animations carry the eye 3-20 cm lower than the idle stance: pulling the trunk up to the
    // camera stretched it, so by default it is never pulled up and hangs under the camera).
    float vertical_deadband{0.08f};
    float lean_up_max{0.0f};
    float vertical_tau{0.25f}; // s
    HideMode hide{HideMode::Head};
    // Both arms (upper arm and forearm) this many times their length: your height's body scale
    // (core/vr/player_body.h; 1 = no height set, the arms exactly as before).
    float arm_scale{1.0f};
    // Your height in the game (core/vr/player_body.h, the Height row; model units). stand_drop:
    // the camera's standing eye this far below the hunter's (a game height shorter than him): the
    // Lean posture brings his upper body down by all of it (no deadband on that part, the duck
    // weight and the deadband measured from it, align_down that much deeper), so his eyes are
    // yours standing straight and a duck goes on from there. rise_stretch: the camera's standing
    // eye above his (taller): his trunk is never pulled up, so his arms may stretch this much more
    // (a fraction of their length, on top of the IK's own maxStretch) toward a hand out of reach,
    // overhead too; a hand within reach keeps the arm's length (below his eye, arms shorter than
    // his stretch back out to his own full reach: player_body.h ShortStretch). 0 and 0 = as
    // before, bit for bit.
    float stand_drop{0.0f};
    float rise_stretch{0.0f};
    // Arms: a controller beyond the arm's reach (with its stretch) turns the clavicle further
    // toward it by up to this (radians; ArmParams::clavicleCatchUp; 0 = off, as before).
    float shoulder_reach{35.0f * Deg};
    // Natural arms (2026-10-08, the tester: crossed hands and hands brought to the chest "don't fold
    // like real human hands"): the forearm takes the wrist's roll up to forearm_pronation palm down
    // and forearm_supination palm up before the elbow moves (ArmParams::naturalRoll), the twist
    // helpers follow the twist continuously up to wrist_twist_max (ArmParams::trackTwist), and the
    // elbow hint folds arms across and against the chest the way a body does (ArmPole). false =
    // the arms before it, bit for bit.
    bool natural_arms{true};
    float forearm_pronation{75.0f * Deg};
    float forearm_supination{90.0f * Deg};
    float wrist_twist_max{150.0f * Deg};
};

// Lean: the share of the trunk lean each joint takes, waist up (UpperTurn at the hips, Spine,
// Spine1, Spine2 under the collar): most of it low, so the trunk bends like a back.
inline constexpr std::array<int, 4> LeanJoints = {C0000::UpperTurn, C0000::Spine, C0000::Spine1,
                                                  C0000::Spine2};
inline constexpr std::array<float, 4> LeanShare = {0.4f, 0.25f, 0.2f, 0.15f};

// Lean: how much of a duck the camera's eye `drop` metres below the animation's eye is (negative:
// above it): 0 standing, rising over lean_low_from .. lean_low_full of drop to 1; always 0 without
// the duck posture (lean_fwd_low_max <= lean_fwd_max).
inline float DuckWeight(const BodyParams& p, float drop) {
    if (!(p.lean_fwd_low_max > std::max(p.lean_fwd_max, 0.0f)) || !std::isfinite(drop)) {
        return 0.0f;
    }
    const float from = std::max(p.lean_low_from, 0.0f);
    const float full = std::max(p.lean_low_full, from + 1e-3f);
    return Smoothstep(from, full, drop);
}

// Lean: the forward limit (radians from vertical) at that drop: lean_fwd_max standing, rising to
// lean_fwd_low_max (at most 85 degrees) with the DuckWeight.
inline float LeanForwardLimit(const BodyParams& p, float drop) {
    const float stand = std::max(p.lean_fwd_max, 0.0f);
    const float low = std::min(std::max(p.lean_fwd_low_max, stand), 85.0f * Deg);
    const float w = DuckWeight(p, drop);
    return w > 0.0f ? stand + (low - stand) * w : stand;
}

struct BodyInput {
    float dt{0.0f};       // s since the last solve (0 = no smoothing: filters start at the target)
    float chr_yaw{0.0f};  // the character's heading (ChrYaw of W at the pose copy)
    float head_yaw{0.0f}; // the view's heading (the anchor's view_yaw)
    Vec3 eye_m{};         // the camera's mono eye, model space
    Vec3 rig_offset{};     // shared camera/body translation in model axes, before IK
    float body_weight{0.0f};
    std::array<std::optional<ModelTarget>, 2> target{}; // C0000::Left, C0000::Right
    std::array<float, 2> arm_weight{};
    float two_hand{0.0f}; // left target follows the actually solved right weapon
    bool hide{false};
};

struct BodyOutput {
    bool ok{true};             // false: nothing may be written (see `fail`)
    const char* fail{nullptr}; // the guard that failed
    std::bitset<C0000::BoneCount> written{};
    float torso_yaw{0.0f};    // the filtered torso heading
    float torso_delta{0.0f};  // the UpperTurn yaw applied (weighted), radians
    float lean_fwd{0.0f};     // the trunk lean applied (weighted), radians: + = forward
    float lean_right{0.0f};   // + = to the hunter's right
    float lean_fwd_limit{0.0f}; // the forward limit this solve (LeanForwardLimit; Lean only)
    float duck{0.0f};           // the DuckWeight this solve (Lean only)
    Vec3 lean_eye{};          // how far the lean moved the eye, model space (evasion)
    Vec3 shift{};             // the UpperPos shift applied (weighted), model space
    Vec3 eye{};               // the hunter's eye before the shift, model space
    float eye_residual{0.0f}; // |eye after the shift - its target| (clamps, filter, weight)
    // The camera against his eye after the shift and lean (metres, model space): ahead (+) /
    // behind (-) along the torso's forward, above (+) / below (-). The body cutaway's boost
    // (core/vr/body_cut_boost.h): the camera behind or below his eye is inside his collar.
    float cam_ahead{0.0f};
    float cam_above{0.0f};
    float cam_side{0.0f};
    std::array<ArmResult, 2> arm{};
    std::array<Vec3, 2> pole{};
    int hidden{0};
};

class BodySolver {
public:
    // `pose`: the 108 animated locals (C0000 order) on entry, the edited pose on exit.
    BodyOutput Solve(QsTransform* pose, const BodyInput& in, const BodyParams& p) {
        namespace C = C0000;
        BodyOutput out{};
        const auto parents = C::Parents.data();
        const auto fail = [&](const char* why) {
            out.ok = false;
            out.fail = why;
            out.written.reset();
            Reset();
            return out;
        };
        if (pose == nullptr) {
            return fail("no pose");
        }
        // The hunter's skeleton, read at runtime (core/vr/c0000_skeleton.h).
        const C0000::Skeleton* const sk = C0000::Active();
        if (sk == nullptr) {
            return fail("no skeleton");
        }
        if (!std::isfinite(in.dt) || !std::isfinite(in.chr_yaw) || !std::isfinite(in.head_yaw) ||
            !IsFinite(in.eye_m) || !IsFinite(in.rig_offset) || !std::isfinite(in.body_weight) ||
            !std::isfinite(in.arm_weight[0]) || !std::isfinite(in.arm_weight[1])) {
            return fail("input");
        }
        for (const auto& t : in.target) {
            if (t && (!IsFinite(t->p) || !IsFinite(t->q))) {
                return fail("hand target");
            }
        }
        for (int i = 0; i < C::BoneCount; ++i) {
            if (!IsFinite(pose[i])) {
                return fail("animated pose");
            }
        }
        // A visceral camera setback is a rigid move of the complete skeleton.
        // Apply it before posture and IK: eye/hand targets already contain this
        // displacement, so shoulders and feet follow once, without extra lean,
        // arm stretch, or a second, independently smoothed body transition.
        if (LengthSq(in.rig_offset) > 0.0f) {
            static_assert(C::Parents[C::Master] == -1);
            pose[C::Master].t += in.rig_offset;
            out.written.set(C::Master);
        }
        const float dt = std::clamp(in.dt, 0.0f, 0.1f);
        const float body_w = std::clamp(in.body_weight, 0.0f, 1.0f);
        const Vec3 up{0.0f, 1.0f, 0.0f};
        // Model frames of the current edit (member storage: this may run on a guest thread's
        // stack, which must stay small).
        const auto fk = [&] { LocalToModel(pose, parents, C::BoneCount, model.data()); };
        fk();

        // 1. Torso yaw.
        float torso_from_legs = 0.0f;
        if (p.torso_relative) {
            torso_from_legs = torso.UpdateRelative(WrapPi(in.head_yaw - in.chr_yaw),
                                                   p.torso_deadzone, p.torso_tau, dt);
            out.torso_yaw = WrapPi(in.chr_yaw + torso_from_legs);
        } else {
            if (!torso.valid) {
                torso.Reset(in.chr_yaw);
            }
            out.torso_yaw = torso.Update(in.head_yaw, p.torso_deadzone, p.torso_tau, dt);
            torso_from_legs = WrapPi(out.torso_yaw - in.chr_yaw);
        }
        if (p.torso_follow && body_w > 0.0f) {
            const float max = std::clamp(p.torso_max, 0.0f, Pi);
            out.torso_delta = std::clamp(torso_from_legs, -max, max) * body_w;
        }
        if (out.torso_delta != 0.0f) {
            const Quat turned = AxisAngle(up, out.torso_delta) * Normalize(model[C::UpperTurn].q);
            pose[C::UpperTurn].q = Normalize(Conjugate(Normalize(model[C::UpperPos].q)) * turned);
            out.written.set(static_cast<std::size_t>(C::UpperTurn));
            fk();
        }

        // 2. Upper body under the eye.
        const Vec3 eye_local{sk->eye_center_on_head[0], sk->eye_center_on_head[1],
                             sk->eye_center_on_head[2]};
        const auto eye_now = [&] {
            const QsTransform& head = model[C::Head];
            return head.t + Rotate(Normalize(head.q), eye_local);
        };
        out.eye = eye_now();
        const Vec3 forward = Rotate(AxisAngle(up, out.torso_delta), {0.0f, 0.0f, -1.0f});
        const Vec3 goal = in.eye_m - forward * p.back;
        // UpperPos (42) is a child of Root: its translation is in Root's frame (the parent's scale
        // never applies to a translation).
        const auto move_upper = [&](Vec3 by) {
            pose[C::UpperPos].t += Rotate(Conjugate(Normalize(model[C::Root].q)), by);
            out.written.set(static_cast<std::size_t>(C::UpperPos));
            fk();
        };
        const float smooth =
            dt > 0.0f && p.align_tau > 0.0f ? 1.0f - std::exp(-dt / p.align_tau) : 1.0f;
        if (p.posture == PostureMode::Shift) {
            Vec3 raw = goal - out.eye;
            const float horizontal = std::sqrt(raw.x * raw.x + raw.z * raw.z);
            const float max_h = std::max(p.align_max, 0.0f);
            if (horizontal > max_h) {
                const float k = max_h / horizontal;
                raw.x *= k;
                raw.z *= k;
            }
            // A game height shorter than the hunter (stand_drop): the camera's deepest duck is
            // that much further below his eye, so the shift may go that much deeper too.
            const float stand_shift =
                std::isfinite(p.stand_drop) && p.stand_drop > 0.0f ? p.stand_drop : 0.0f;
            raw.y = std::clamp(raw.y, -(std::max(p.align_down, 0.0f) + stand_shift),
                               std::max(p.align_up, 0.0f));
            if (!shift_valid || dt <= 0.0f || p.align_tau <= 0.0f) {
                shift = raw;
                shift_valid = true;
            } else {
                shift = shift + (raw - shift) * smooth;
            }
            if (p.align && body_w > 0.0f) {
                out.shift = shift * body_w;
                if (LengthSq(out.shift) > 0.0f) {
                    move_upper(out.shift);
                }
            }
            out.eye_residual = Length(out.eye + out.shift - goal);
            const Vec3 cam = in.eye_m - (out.eye + out.shift);
            out.cam_ahead = Dot(cam, forward);
            out.cam_above = cam.y;
            out.cam_side = Dot(cam, Cross(up, forward));
        } else {
            // The lean: the horizontal offset of the goal from the eye, along the torso's forward
            // and right, turned into the trunk angles that move the eye there (each joint's share
            // times its height under the eye is the eye's travel per radian).
            const Vec3 right = Cross(up, forward);
            const Vec3 e = goal - out.eye;
            float lever = 0.0f;
            for (std::size_t k = 0; k < LeanJoints.size(); ++k) {
                lever +=
                    LeanShare[k] *
                    std::max(out.eye.y - model[static_cast<std::size_t>(LeanJoints[k])].t.y, 0.0f);
            }
            lever = std::max(lever, 0.1f);
            // The animation's own trunk angles (waist -> neck): the limits are on the result.
            const Vec3 trunk = model[C::Neck].t - model[C::UpperTurn].t;
            const float trunk_up = std::max(Dot(trunk, up), 0.05f);
            const float anim_fwd = std::atan2(Dot(trunk, forward), trunk_up);
            const float anim_side = std::atan2(Dot(trunk, right), trunk_up);
            const auto angle = [lever](float offset, float anim, float back_max, float fwd_max) {
                const float lo = std::min(0.0f, -std::max(back_max, 0.0f) - anim);
                const float hi = std::max(0.0f, std::max(fwd_max, 0.0f) - anim);
                return std::clamp(std::asin(std::clamp(offset / lever, -1.0f, 1.0f)), lo, hi);
            };
            // A game height shorter than the hunter (stand_drop): his stance is that much lower,
            // and the duck is measured from it.
            const float stand =
                std::isfinite(p.stand_drop) && p.stand_drop > 0.0f ? p.stand_drop : 0.0f;
            const float drop = stand > 0.0f ? out.eye.y - goal.y - stand : out.eye.y - goal.y;
            out.duck = DuckWeight(p, drop);
            out.lean_fwd_limit = LeanForwardLimit(p, drop);
            const float fwd_goal =
                angle(Dot(e, forward), anim_fwd, p.lean_back_max, out.lean_fwd_limit);
            const float side_goal =
                angle(Dot(e, right), anim_side, p.lean_side_max, p.lean_side_max);
            if (!lean_valid || dt <= 0.0f || p.align_tau <= 0.0f) {
                lean_fwd = fwd_goal;
                lean_side = side_goal;
                lean_valid = true;
            } else {
                lean_fwd += (fwd_goal - lean_fwd) * smooth;
                lean_side += (side_goal - lean_side) * smooth;
            }
            if (p.align && body_w > 0.0f) {
                out.lean_fwd = lean_fwd * body_w;
                out.lean_right = lean_side * body_w;
                if (out.lean_fwd != 0.0f || out.lean_right != 0.0f) {
                    // Waist up, each joint about its own position (the model frames of the joints
                    // above follow after each fk()).
                    for (std::size_t k = 0; k < LeanJoints.size(); ++k) {
                        const auto j = static_cast<std::size_t>(LeanJoints[k]);
                        const Quat turn =
                            Normalize(AxisAngle(right, out.lean_fwd * LeanShare[k]) *
                                      AxisAngle(-forward, out.lean_right * LeanShare[k]));
                        const Quat turned = Normalize(turn * Normalize(model[j].q));
                        pose[j].q = Normalize(
                            Conjugate(Normalize(model[static_cast<std::size_t>(parents[j])].q)) *
                            turned);
                        out.written.set(j);
                        fk();
                    }
                }
            }
            const Vec3 leaned = eye_now();
            out.lean_eye = leaned - out.eye;
            // The height: what is left beyond the deadband, slowly.
            const float dy = goal.y - leaned.y;
            // A duck follows all the way down: the deadband (for the walk's bob) fades out.
            const float band = std::max(p.vertical_deadband, 0.0f) * (1.0f - out.duck);
            float vy_goal = 0.0f;
            if (stand > 0.0f) {
                // The lowered stance in full, the deadband (the walk's bob) about it; never above
                // his own stance, at most align_down below the lowered one.
                const float rel = dy + stand;
                vy_goal = std::clamp((rel > band ? rel - band : (rel < -band ? rel + band : 0.0f)) -
                                         stand,
                                     -(std::max(p.align_down, 0.0f) + stand),
                                     std::clamp(p.lean_up_max, 0.0f, std::max(p.align_up, 0.0f)));
            } else {
                vy_goal = std::clamp(dy > band ? dy - band : (dy < -band ? dy + band : 0.0f),
                                     -std::max(p.align_down, 0.0f),
                                     std::clamp(p.lean_up_max, 0.0f, std::max(p.align_up, 0.0f)));
            }
            if (!vertical_valid || dt <= 0.0f || p.vertical_tau <= 0.0f) {
                vertical = vy_goal;
                vertical_valid = true;
            } else {
                vertical += (vy_goal - vertical) * (1.0f - std::exp(-dt / p.vertical_tau));
            }
            if (p.align && body_w > 0.0f && vertical != 0.0f) {
                out.shift = Vec3{0.0f, vertical * body_w, 0.0f};
                move_upper(out.shift);
            }
            out.eye_residual = Length(leaned + out.shift - goal);
            const Vec3 cam = in.eye_m - (leaned + out.shift);
            out.cam_ahead = Dot(cam, forward);
            out.cam_above = cam.y;
            out.cam_side = Dot(cam, Cross(up, forward));
        }

        // 3. Arms (SolveArm walks the clavicle's parent chain itself; the two arms do not share
        // bones, so the shoulder and spine frames below hold for both).
        const BodyFrame frame = TurnedFrame(out.torso_delta);
        const Vec3 spine = model[C::Spine2].t;
        // Natural arms: the trunk the elbows stay out of, along the edited spine (Spine -> Neck).
        ElbowKeepOut keep_out{};
        if (p.natural_arms) {
            keep_out = TrunkKeepOut(model[C::Spine].t, model[C::Neck].t, frame);
        }
        const bool coupled = in.two_hand > 0.0f && in.target[C::Right] && in.target[C::Left] &&
                             in.arm_weight[C::Right] > 0.0f;
        for (int pass = 0; pass < 2; ++pass) {
            const int side = coupled ? 1 - pass : pass; // weapon first, then its support hand
            const auto i = static_cast<std::size_t>(side);
            const float w = std::clamp(in.arm_weight[i], 0.0f, 1.0f);
            // Freeze the last displayed local pose when tracking takes over.
            // Re-blending from the game's new animation each frame lets an idle/
            // recovery clip switch kick the hand sideways during this return.
            if (w < previous_weight[i]) {
                returning[i] = false;
            }
            if (have_previous_pose && w > previous_weight[i] && !returning[i]) {
                return_pose[i] = previous_pose;
                return_start[i] = previous_weight[i];
                returning[i] = true;
            }
            if (w >= 1.0f || w <= 0.0f || !in.target[i])
                returning[i] = false;
            previous_weight[i] = w;
            if (!(w > 0.0f) || !in.target[i]) {
                arms[i].Reset();
                continue;
            }
            const ArmRig rig = side == C::Right ? HunterRightArm(*sk) : HunterLeftArm(*sk);
            const Vec3 shoulder = model[static_cast<std::size_t>(rig.upperArm)].t;
            ModelTarget target = *in.target[i];
            if (coupled && side == C::Left && out.arm[C::Right].status == ArmStatus::Solved) {
                // Reach limits and arm blending may move the visible right weapon away from
                // its controller target. Carry the support hand by that same rigid transform.
                const auto actual = ModelOf(pose, parents, C::BoneCount, C::R_Weapon);
                if (actual) {
                    const auto& requested = *in.target[C::Right];
                    const Quat turn = Normalize(actual->q * Conjugate(Normalize(requested.q)));
                    const float hold = std::clamp(in.two_hand, 0.0f, 1.0f);
                    target.p = Lerp(target.p, actual->t + Rotate(turn, target.p - requested.p), hold);
                    target.q = Slerp(target.q, Normalize(turn * target.q), hold);
                }
            }
            out.pole[i] =
                ArmPole(frame, rig.side, target.p - shoulder, target.p - spine, p.natural_arms);
            ArmParams params = HunterArmParams(p.natural_arms);
            if (p.natural_arms) {
                params.rollFreePron = std::clamp(p.forearm_pronation, 0.0f, Pi);
                params.rollFreeSup = std::clamp(p.forearm_supination, 0.0f, Pi);
                params.maxWristTwist = std::clamp(p.wrist_twist_max, 0.0f, Pi);
                params.keepOut = keep_out;
            }
            params.pole = out.pole[i];
            params.weight = w;
            if (returning[i]) {
                params.blendFrom = return_pose[i].data();
                params.weight =
                    std::clamp((w - return_start[i]) / (1.0f - return_start[i]), 0.0f, 1.0f);
            }
            params.lengthScale = p.arm_scale;
            if (std::isfinite(p.rise_stretch) && p.rise_stretch > 0.0f) {
                // A game height taller than the hunter: the arm reaches the further hands.
                params.limits.maxStretch += p.rise_stretch;
            }
            params.clavicleCatchUp = std::max(p.shoulder_reach, 0.0f);
            out.arm[i] = SolveArm(pose, parents, C::BoneCount, rig, {target.p, target.q, 1.0f},
                                  params, arms[i], dt);
            const ArmResult& r = out.arm[i];
            for (int k = 0; k < r.writtenCount; ++k) {
                out.written.set(static_cast<std::size_t>(r.written[static_cast<std::size_t>(k)]));
            }
        }

        // 4. Hiding.
        if (in.hide) {
            int count = 0;
            const auto bones = HiddenBones(p.hide, count);
            for (int k = 0; k < count; ++k) {
                const HiddenBone& h = bones[static_cast<std::size_t>(k)];
                pose[h.bone].s = {h.scale, h.scale, h.scale};
                out.written.set(static_cast<std::size_t>(h.bone));
            }
            out.hidden = count;
        }

        // Guard: every written bone finite, a unit rotation and a positive scale.
        for (int i = 0; i < C::BoneCount; ++i) {
            if (!out.written.test(static_cast<std::size_t>(i))) {
                continue;
            }
            const QsTransform& x = pose[i];
            const float n = Dot(x.q, x.q);
            if (!IsFinite(x) || !(n > 0.98f && n < 1.02f) || !(x.s.x > 0.0f) || !(x.s.y > 0.0f) ||
                !(x.s.z > 0.0f)) {
                return fail("result");
            }
        }
        std::copy_n(pose, C::BoneCount, previous_pose.begin());
        have_previous_pose = true;
        return out;
    }

    void Reset() {
        have_previous_pose = false;
        returning = {};
        previous_weight = {};
        torso = {};
        shift = {};
        shift_valid = false;
        lean_fwd = lean_side = vertical = 0.0f;
        lean_valid = vertical_valid = false;
        for (ArmState& a : arms) {
            a.Reset();
        }
    }

    bool have_previous_pose{false};
    std::array<float, 2> previous_weight{}, return_start{};
    std::array<bool, 2> returning{};
    std::array<QsTransform, C0000::BoneCount> previous_pose{};
    std::array<std::array<QsTransform, C0000::BoneCount>, 2> return_pose{};
    TorsoYaw torso{};
    Vec3 shift{}; // the filtered (unweighted) UpperPos shift (PostureMode::Shift)
    bool shift_valid{false};
    // PostureMode::Lean: the filtered (unweighted) trunk angles and height move.
    float lean_fwd{0.0f};
    float lean_side{0.0f};
    bool lean_valid{false};
    float vertical{0.0f};
    bool vertical_valid{false};
    std::array<ArmState, 2> arms{};
    std::array<QsTransform, C0000::BoneCount> model{}; // scratch: FK of the pose being edited
};

} // namespace VR::Body
