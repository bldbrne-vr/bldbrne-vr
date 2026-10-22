// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <bitset>
#include <cmath>

#include "core/vr/body_frame.h"

namespace VR::Bow {

// WP_A_1709_2's shaft points along -Y; its nock is 32.24 cm behind the
// part origin (+Y), verified against the retail FLVER. The hand grips the
// nock, so moving only the weapon bone preserves the anatomical hand pose.
constexpr float NativeNockM = 0.3224f;

// The bow fist holds the bow by its outer side (the left one for a bow in the left hand, its
// handle pointing that way), loosely, always as at full draw. The bow sits BowShiftM toward the
// string hand's side of the fist, and the nocked arrow runs along that side of the bow: just
// above the fist and clear of it. The presenter's draw, nock zone and shot line start from the
// same rest (ArrowRest).
constexpr float BowShiftM = 0.015f;     // the bow's grip off the fist, toward the string side
constexpr float ArrowRestUpM = 0.02f;   // along the bow's upper limb
constexpr float ArrowRestSideM = 0.045f; // across from the fist, to the string hand's side

// The unit direction across the bow's limb toward the string hand's side (zero: unknown).
inline Body::Vec3 Across(Body::Vec3 up, Body::Vec3 string_side) {
    const Body::Vec3 u = Body::Normalize(up);
    const Body::Vec3 side = string_side - u * Body::Dot(string_side, u);
    return Body::LengthSq(side) > 1e-6f ? Body::Normalize(side) : Body::Vec3{};
}

// The rest for the bow fist at `grip`: `up` along the bow's upper limb, `string_side` toward the
// string hand's side of the body (the hunter's right for a bow in the left hand). Only its part
// across the limb counts; none known: just above the fist.
inline Body::Vec3 ArrowRest(Body::Vec3 grip, Body::Vec3 up, Body::Vec3 string_side) {
    return grip + Body::Normalize(up) * ArrowRestUpM + Across(up, string_side) * ArrowRestSideM;
}

// An arrow carried in the string hand (not on the string) is held between thumb and index, this
// far toward the hand's top from the grip, its nock CarryBackM behind the fist (the fist round
// the shaft, not its very end), and points where the hand points: the weapon bone's blade axis
// (the grip's -Z, a sword's line) tilted down by bow_arrow_pitch_deg toward the controller's
// pointing ray.
constexpr float CarryLiftM = 0.015f;
constexpr float CarryBackM = 0.07f;

// The arrow in the hand and on the string is shown at this size (the native shaft, 78 cm, looks
// long in VR). Its weapon bone carries the scale (Havok: a parent's scale is not applied to its
// children's translations, so only the mesh shrinks, about its part origin); NativeArrow keeps
// the nock in the fingers. The shot itself is the game's own arrow.
constexpr float ArrowScale = 0.9f;

inline Body::QsTransform NativeArrow(Body::Vec3 nock, Body::Quat orientation,
                                     Body::Vec3 scale = {1, 1, 1}) {
    return {nock - Body::Rotate(orientation, {0, NativeNockM * scale.y, 0}), orientation, scale};
}

// Aim the native part's -Y shaft through the bow rest, keeping the bow's roll.
// The native bow and arrow share this forward axis.
inline Body::Quat AlongString(Body::Quat bow, Body::Vec3 line) {
    if (Body::LengthSq(line) < 1e-6f || !Body::IsFinite(line)) {
        return bow;
    }
    return Body::Normalize(Body::FromTo(Body::Rotate(bow, {0, -1, 0}), line) * bow);
}

// Weapon grips use +X up, but WP_A_1709_3's limbs run along Z. Keep their
// plane upright in the bow hand before steering the shaft onto the draw line.
inline Body::Quat BowMeshOrientation(Body::Quat grip, Body::Vec3 line) {
    constexpr Body::Quat mesh_to_grip{0, 0.7071067812f, 0, 0.7071067812f};
    return AlongString(Body::Normalize(grip * mesh_to_grip), line);
}

// The native attachment frame relative to its weapon bone. Dummies 300 / 340
// are read from the player's skeleton, including any rotation and position.
inline Body::QsTransform Attachment(const Body::C0000::DummyPoly& d) {
    const Body::Vec3 z{d.forward[0], d.forward[1], d.forward[2]};
    const Body::Vec3 y{d.up[0], d.up[1], d.up[2]};
    return {{d.position[0], d.position[1], d.position[2]},
            Body::FromBasis(Body::Normalize(Body::Cross(y, z)), y, z), {1, 1, 1}};
}

// Native part animations have their own translated/rotated roots. These are read
// from the live weapon skeletons, not assumed to match the FLVER bind axes.
struct NativeGeometry {
    Body::QsTransform bow_root{};
    Body::Vec3 bow_grip{};
    Body::QsTransform arrow_root{};
};

// The bow in the fist: always held as when shooting, but it is the hand's, turning with the wrist.
// Its grip on the hand is the one of the last full draw (learned there), before any shot the bow
// facing where the hand points. Nocking turns nothing (the string hand beside the bow gives no
// line to face); drawing back eases it onto the arrow's line, fully from DrawAlignFarM, and after
// the release it stays in that grip: no flip either way.
constexpr float DrawAlignNearM = 0.25f; // the draw (rest to nock) below this: the hand's grip
constexpr float DrawAlignFarM = 0.45f;  // ... from here: along the arrow (and learned)
struct BowHold {
    bool learned{false};
    Body::Quat local{}; // the bow (BowMeshOrientation's frame) in the bow attachment's frame
};
inline Body::Quat HeldBow(BowHold& hold, Body::Quat grip, Body::Vec3 pointing, bool nocked,
                          Body::Vec3 rest, Body::Vec3 nock) {
    const Body::Quat q = Body::Normalize(grip);
    const Body::Quat rigid = hold.learned ? Body::Normalize(q * hold.local)
                                          : BowMeshOrientation(q, pointing);
    if (!nocked) {
        return rigid;
    }
    const float w = Body::Smoothstep(DrawAlignNearM, DrawAlignFarM, Body::Length(rest - nock));
    if (w <= 0.0f) {
        return rigid;
    }
    const Body::Quat aligned = BowMeshOrientation(q, rest - nock);
    if (w >= 1.0f) {
        hold.local = Body::Normalize(Body::Conjugate(q) * aligned);
        hold.learned = true;
        return aligned;
    }
    return Body::Normalize(Body::Slerp(rigid, aligned, w));
}

// Only weapon bones change: the fingers remain at their solved nock/rest points.
// `carry_pitch_rad`: the carried arrow's tilt down from the blade axis (bow_arrow_pitch_deg).
// `hold`: the bow's grip on the fist (HeldBow), kept by the caller across frames.
// With no arrow, keep correcting the bow alone: firing must not restore its native grip.
inline std::bitset<Body::C0000::BoneCount> PoseNativeArrow(
    Body::QsTransform* locals, const Body::C0000::Skeleton& sk, int bow_hand, bool nocked,
    const NativeGeometry& geometry, float carry_pitch_rad, BowHold& hold, bool arrow_present = true) {
    namespace B = Body;
    namespace C = Body::C0000;
    std::bitset<C::BoneCount> written;
    if (!locals || bow_hand < 0 || bow_hand > 1) return written;
    const auto& bd = bow_hand == C::Left ? sk.left_weapon_base : sk.right_weapon_base;
    const auto& ad = bow_hand == C::Left ? sk.right_weapon_base : sk.left_weapon_base;
    const auto bow = B::ModelOf(locals, C::Parents.data(), C::BoneCount, bd.bone);
    if (!bow) return written;
    nocked = nocked && arrow_present;
    const auto ba = Attachment(bd), aa = Attachment(ad);
    auto bp = B::Compose(*bow, ba);
    const auto write = [&](int bone, const B::QsTransform& part, const B::QsTransform& attach) {
        const auto parent = B::ModelOf(locals, C::Parents.data(), C::BoneCount, C::Parents[bone]);
        if (!parent) return;
        auto model = B::ParentFromChild(part, attach);
        model.s = part.s; // the part's scale on its bone (ParentFromChild keeps none)
        locals[bone] = B::LocalFromModel(*parent, model);
        written.set(bone);
    };
    // The carried arrow follows the controller, after root compensation: pinched at its nock,
    // tilted down about the hand's side axis (the attachment's +Z) from the blade line (-Y)
    // toward its pointing direction (-X is the hand's down).
    const auto pointing = [&](const B::QsTransform& hand) {
        return B::Rotate(hand.q, {-std::sin(carry_pitch_rad), -std::cos(carry_pitch_rad), 0});
    };
    B::QsTransform ap{};
    if (arrow_present) {
        const auto arrow = B::ModelOf(locals, C::Parents.data(), C::BoneCount, ad.bone);
        if (!arrow) return written;
        ap = B::Compose(*arrow, aa);
    }
    auto q = B::Normalize(ap.q * B::AxisAngle({0, 0, 1}, -carry_pitch_rad));
    B::Vec3 nock = ap.t + B::Rotate(ap.q, {CarryLiftM, 0, 0}) -
                   B::Rotate(q, {0, -1, 0}) * CarryBackM;
    // The string hand's side of the body, from the collarbones (no assumption about the frames'
    // handedness): the arrow's side of the bow.
    B::Vec3 string_side{};
    const auto lc = B::ModelOf(locals, C::Parents.data(), C::BoneCount, C::L_Clavicle);
    const auto rc = B::ModelOf(locals, C::Parents.data(), C::BoneCount, C::R_Clavicle);
    if (lc && rc && B::LengthSq(rc->t - lc->t) > 1e-6f) {
        string_side = B::Normalize(bow_hand == C::Left ? rc->t - lc->t : lc->t - rc->t);
    }
    // The bow, on the string or not: the hand's, held as when shooting (HeldBow), its grip just
    // off the fist toward the string side.
    const B::Vec3 up = B::Rotate(bp.q, {1, 0, 0});
    const B::Vec3 rest = ArrowRest(bp.t, up, string_side);
    const B::Quat held = HeldBow(hold, bp.q, pointing(bp), nocked, rest, ap.t);
    if (nocked) {
        // On the string: from the string hand's nock onto the rest beside the bow fist.
        nock = ap.t;
        q = BowMeshOrientation(bp.q, rest - ap.t);
    }
    const B::Vec3 fist = bp.t;
    bp.q = B::Normalize(held * B::Conjugate(geometry.bow_root.q));
    bp.t = fist + Across(up, string_side) * BowShiftM - B::Rotate(bp.q, geometry.bow_grip);
    write(bd.bone, bp, ba);
    if (!arrow_present) return written;
    auto arrow_frame = NativeArrow(nock, q, {ArrowScale, ArrowScale, ArrowScale});
    // Desired mesh frame = attachment * animated root (the root's translation unscaled: Havok).
    const B::Vec3 arrow_scale = arrow_frame.s;
    arrow_frame = B::ParentFromChild(arrow_frame, geometry.arrow_root);
    arrow_frame.s = arrow_scale;
    write(ad.bone, arrow_frame, aa);
    return written;
}

// Position the native string's centre in its own part skeleton. Its limbs and
// attachment remain the game's; only the string centre follows the physical nock.
inline bool MoveString(Body::QsTransform* local, const std::int16_t* parents, int count,
                       int bone, Body::Vec3 target) {
    if (!local || !parents || bone < 0 || bone >= count || !Body::IsFinite(target)) return false;
    const int parent = parents[bone];
    if (parent < 0) {
        local[bone].t = target;
        return true;
    }
    const auto model = Body::ModelOf(local, parents, count, parent);
    if (!model) return false;
    local[bone].t = Body::Rotate(Body::Conjugate(model->q), target - model->t);
    return true;
}

// An occluded string hand (typically at the cheek) stays nocked relative to the
// tracked bow. No advancing animation or disappearing hand while aiming.
class NockedTracking {
public:
    void Apply(Body::HandsSample& hands) {
        const int bi = hands.bow_hand;
        if (!hands.bow_nocked || bi < 0 || bi > 1 ||
            (have && (space != hands.space_serial || bi != side))) {
            have = false;
        }
        if (!hands.bow_nocked || bi < 0 || bi > 1) return;
        const int si = 1 - bi;
        const bool bow_ok = Body::IsTracked(hands.hand[bi]);
        const bool string_ok = Body::IsTracked(hands.hand[si]);
        if (have) {
            if (!bow_ok) hands.hand[bi] = cached[bi];
            if (!string_ok) {
                const auto offset = cached[si].p - cached[bi].p;
                hands.hand[si] = cached[si];
                hands.hand[si].p = hands.hand[bi].p + offset;
                hands.hand[si].v = {};
            }
        }
        if ((bow_ok && string_ok) || have) {
            cached = hands.hand;
            space = hands.space_serial;
            side = bi;
            have = true;
        }
    }
private:
    std::array<Body::TrackedHand, 2> cached{};
    std::uint32_t space{};
    int side{-1};
    bool have{false};
};

} // namespace VR::Bow
