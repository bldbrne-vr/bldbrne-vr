// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <array>
#include <cstdint>

#include "core/vr/arm_ik.h"
#include "core/vr/c0000_skeleton.h"
#include "core/vr/reviewed_item_grips.h"

// The hunter's left-hand grip on a held item (the vial, a quick item from the arc): the fingers'
// local rotations, closed around the item's shape, written over the animation's own (a pistol grip
// at idle, an open hand without a left weapon, ...). Pure math on the c0000 skeleton.
//
// Each finger joint of the reference pose (an open, flat hand) is bent about its own bend axis: the
// finger bones run along their local +X and curl toward the palm about local -Y (measured: the
// game's own grips, VR_GRIP_LOG, turn every finger joint about -Y; the base joints add a little
// spread about Z). The thumb's base turns across the palm about its own opposition axis (the game's
// grips: 25 degrees about (-0.83, -0.31, -0.45), in both of its left and right hands), its two
// other joints curl about -Y like the fingers. A shape gives every joint's bend (radians) and the
// fingers' spread; ShapeLocal gives a joint's local, which the body blends over the animated one by
// the grip's weight.
namespace VR::Body::Grip {

enum class Kind : std::uint8_t {
    None,
    Neck,   // a bottle's neck or a slim body (Molotov, Numbing Mist): a full fist around ~3 cm
    Handle, // a knife's grip: a tight fist, the thumb along it
    Ball,   // a round stone or a jar held from below: the fingers spread round ~10 cm
    Wide,   // an urn or a wide bottle: an open claw round ~11 cm
    Flask,  // a medicine bottle's body (~6-8 cm): a loose fist
    Pinch,  // paper, a small packet: thumb on the index finger's side
    Vial,   // the Blood Vial (the game's own model in the palm): a full fist
    Bottle, // a ~5 cm bottle's body (Iosefka's Blood Vial): fingers round it, the thumb along it
    Cup,    // a small clot in the cupped palm (the coldbloods): the fingers curled up round it
    Tray,   // paper flat on the open palm (Antidote): the fingers relaxed under it
    Cradle, // a skull's back in the palm, face up (Madman's Knowledge): the fingers round it
    Book,   // a closed notebook by its side: the edge in the palm, the thumb on the front
    HookPinch, // a bell's hanger between the thumb and index pads, the bell hanging below
    RubPinch,  // a blade rub's end: the thumb on its face, the index and middle behind it
    Count,
};

[[nodiscard]] constexpr const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::None:
        return "none";
    case Kind::Neck:
        return "neck";
    case Kind::Handle:
        return "handle";
    case Kind::Ball:
        return "ball";
    case Kind::Wide:
        return "wide";
    case Kind::Flask:
        return "flask";
    case Kind::Pinch:
        return "pinch";
    case Kind::Vial:
        return "vial";
    case Kind::Bottle:
        return "bottle";
    case Kind::Cup:
        return "cup";
    case Kind::Tray:
        return "tray";
    case Kind::Cradle:
        return "cradle";
    case Kind::Book:
        return "book";
    case Kind::HookPinch:
        return "hookpinch";
    case Kind::RubPinch:
        return "rubpinch";
    case Kind::Count:
        break;
    }
    return "?";
}

// The left hand's finger bones (Havok order): thumb, index, middle, ring, little; base, middle
// and tip joints.
inline constexpr std::array<std::array<int, 3>, 5> LeftFingers = {{
    {C0000::L_Finger0, C0000::L_Finger01, C0000::L_Finger02},
    {C0000::L_Finger1, C0000::L_Finger11, C0000::L_Finger12},
    {C0000::L_Finger2, C0000::L_Finger21, C0000::L_Finger22},
    {C0000::L_Finger3, C0000::L_Finger31, C0000::L_Finger32},
    {C0000::L_Finger4, C0000::L_Finger41, C0000::L_Finger42},
}};

inline constexpr std::array<std::array<int, 3>, 5> RightFingers = {{
    {C0000::R_Finger0, C0000::R_Finger01, C0000::R_Finger02},
    {C0000::R_Finger1, C0000::R_Finger11, C0000::R_Finger12},
    {C0000::R_Finger2, C0000::R_Finger21, C0000::R_Finger22},
    {C0000::R_Finger3, C0000::R_Finger31, C0000::R_Finger32},
    {C0000::R_Finger4, C0000::R_Finger41, C0000::R_Finger42},
}};

struct Shape {
    // Per finger (thumb first): the base, middle and tip joints' bend from the open hand (rad).
    std::array<std::array<float, 3>, 5> bend{};
    // Per finger: the base joint's sideways turn (rad, + toward the little finger).
    std::array<float, 5> spread{};
    // The thumb's base: its turn across the palm (rad, + toward the fingers).
    float thumb_oppose{0.0f};
};

// The joint's bend and spread axes (its local frame) and the thumb base's opposition axis.
inline constexpr Vec3 BendAxis{0.0f, -1.0f, 0.0f};
inline constexpr Vec3 SpreadAxis{0.0f, 0.0f, 1.0f};
inline constexpr Vec3 ThumbBendAxis{0.0f, -1.0f, 0.0f};
inline constexpr Vec3 ThumbOpposeAxis{-0.83f, -0.31f, -0.45f};

constexpr float Deg = 3.14159265358979f / 180.0f;

// The shapes (degrees in the table for reading; thumb, index, middle, ring, little: base, middle,
// tip). Neck and Handle are the game's own fist round a weapon's grip (the hunter's right hand on
// the Saw Cleaver, measured), Vial its pistol grip (his left hand at idle, measured: the game's
// vial sits in it); the others open that fist round wider shapes.
[[nodiscard]] constexpr Shape ShapeOf(Kind kind) {
    Shape s{};
    const auto finger = [&s](int f, float base, float mid, float tip, float spread = 0.0f) {
        s.bend[static_cast<size_t>(f)] = {base * Deg, mid * Deg, tip * Deg};
        s.spread[static_cast<size_t>(f)] = spread * Deg;
    };
    switch (kind) {
    case Kind::Handle:
        finger(0, 0, 71, 71);
        finger(1, 55, 100, 62);
        finger(2, 71, 96, 70);
        finger(3, 84, 69, 69);
        finger(4, 97, 71, 71);
        s.thumb_oppose = 25 * Deg;
        break;
    case Kind::Neck:
        finger(0, 0, 60, 60);
        finger(1, 52, 92, 58);
        finger(2, 66, 92, 64);
        finger(3, 78, 68, 64);
        finger(4, 90, 70, 66);
        s.thumb_oppose = 25 * Deg;
        break;
    case Kind::Vial:
        // The game's pistol grip, the four fingers relaxed (up to 15%) and the thumb less
        // opposed: in game they sank into the glass (2026-10-12).
        finger(0, 0, 71, 71);
        finger(1, 9, 29, 4);
        finger(2, 36, 88, 49);
        finger(3, 51, 58, 58);
        finger(4, 63, 66, 66);
        s.thumb_oppose = 10 * Deg;
        break;
    case Kind::Flask:
        finger(0, 0, 40, 40);
        finger(1, 30, 55, 35, -3);
        finger(2, 38, 55, 38);
        finger(3, 45, 50, 40, 2);
        finger(4, 52, 50, 40, 4);
        s.thumb_oppose = 20 * Deg;
        break;
    case Kind::Wide:
        finger(0, 0, 25, 25);
        finger(1, 15, 30, 20, -8);
        finger(2, 18, 30, 20, -3);
        finger(3, 22, 30, 20, 3);
        finger(4, 26, 30, 20, 8);
        s.thumb_oppose = 15 * Deg;
        break;
    case Kind::Ball:
        finger(0, 0, 30, 30);
        finger(1, 20, 38, 25, -6);
        finger(2, 24, 38, 25, -2);
        finger(3, 28, 38, 25, 2);
        finger(4, 32, 38, 25, 6);
        s.thumb_oppose = 18 * Deg;
        break;
    case Kind::Pinch:
        finger(0, 0, 35, 30);
        finger(1, 35, 55, 30);
        finger(2, 60, 90, 60);
        finger(3, 75, 80, 65);
        finger(4, 85, 75, 65);
        s.thumb_oppose = 35 * Deg;
        break;
    case Kind::Bottle:
        // Fitted on a proxy hand (offline analysis): the thumb lies flat along the
        // body (a negative opposition), the fingers close on the 49 mm glass.
        finger(0, 0, 0, 0);
        finger(1, 35, 61, 43, -3);
        finger(2, 54, 66, 47);
        finger(3, 53, 65, 46, 2);
        finger(4, 47, 58, 41, 4);
        s.thumb_oppose = -32 * Deg;
        break;
    // Fitted the same way: contact within ~1 mm on the proxy, none deeper than 1 mm.
    case Kind::Cup:
        finger(0, 0, 18, 15);
        finger(1, 18, 30, 20, -4);
        finger(2, 15, 30, 20, -1);
        finger(3, 15, 30, 20, 2);
        finger(4, 18, 30, 20, 5);
        s.thumb_oppose = -15 * Deg;
        break;
    case Kind::Tray:
        finger(0, 0, 20, 15);
        finger(1, 15, 15, 10, -4);
        finger(2, 15, 15, 10, -1);
        finger(3, 15, 15, 10, 2);
        finger(4, 15, 15, 10, 5);
        s.thumb_oppose = -20 * Deg;
        break;
    case Kind::Cradle:
        finger(0, 0, 8, 39);
        finger(1, 27, 34, 33, -4);
        finger(2, 27, 44, 30, -1);
        finger(3, 27, 34, 33, 2);
        finger(4, 17, 21, 33, 5);
        s.thumb_oppose = -20 * Deg;
        break;
    case Kind::Book:
        finger(0, 0, 56, 58);
        finger(1, 22, 84, 57, -3);
        finger(2, 46, 64, 43, -1);
        finger(3, 49, 61, 42, 1);
        finger(4, 47, 61, 42, 3);
        s.thumb_oppose = 14 * Deg;
        break;
    case Kind::HookPinch:
        finger(0, 0, 11, 8);
        finger(1, 50, 15, 5);
        finger(2, 75, 100, 65);
        finger(3, 85, 95, 70, 2);
        finger(4, 90, 90, 70, 4);
        s.thumb_oppose = 20 * Deg;
        break;
    case Kind::RubPinch:
        // The thumb lies open along the item's face (held 2 cm into the palm, a curled one
        // went into the shell's body).
        finger(0, 0, 15, 18);
        finger(1, 44, 57, 33);
        finger(2, 46, 60, 36);
        finger(3, 85, 95, 70, 2);
        finger(4, 90, 90, 70, 4);
        s.thumb_oppose = 0;
        break;
    case Kind::None:
    case Kind::Count:
        break;
    }
    return s;
}

// The grip's local rotation of finger `f` joint `j` (the reference pose's, bent by the shape).
// `bend_sign` / `oppose_sign` flip the axes (+1: as documented above). `sk`: the hunter's skeleton
// read at runtime (the caller has checked C0000::Active()).
// Item-specific contact poses preserve the independently reviewed thumb and finger bends.
[[nodiscard]] constexpr Shape ShapeForGoods(std::int32_t goods, Kind fallback) {
    if (const auto* p = ItemGrips::Find(goods); p && p->custom_shape) {
        return {p->bend, p->spread, p->oppose};
    }
    return ShapeOf(fallback);
}

[[nodiscard]] inline Quat ShapeLocal(const Shape& s, int f, int j, float bend_sign = 1.0f,
                                     float oppose_sign = 1.0f,
                                     const C0000::Skeleton& sk = C0000::Ref(),
                                     C0000::Side side = C0000::Left) {
    const auto& fingers = side == C0000::Right ? RightFingers : LeftFingers;
    const int bone = fingers[static_cast<size_t>(f)][static_cast<size_t>(j)];
    const auto& ref = sk.local[static_cast<size_t>(bone)].q;
    const Quat q0{ref[0], ref[1], ref[2], ref[3]};
    const float bend = s.bend[static_cast<size_t>(f)][static_cast<size_t>(j)] * bend_sign;
    Quat q = q0;
    if (f == 0) {
        if (j == 0) {
            q = q * AxisAngle(ThumbOpposeAxis, s.thumb_oppose * oppose_sign);
        }
        if (bend != 0.0f) {
            q = q * AxisAngle(ThumbBendAxis, bend);
        }
    } else {
        if (j == 0) {
            q = q * AxisAngle(SpreadAxis, s.spread[static_cast<size_t>(f)]);
        }
        q = q * AxisAngle(BendAxis, bend);
    }
    return Normalize(q);
}

} // namespace VR::Body::Grip
