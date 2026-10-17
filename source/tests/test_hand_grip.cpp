// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <string_view>

#include <gtest/gtest.h>

#include "c0000_test_skeleton.h"

#include "core/vr/hand_grip.h"

namespace B = VR::Body;
namespace G = VR::Body::Grip;

namespace {

// The turn of a joint's grip local from its reference local (axis, degrees).
B::Quat FromReference(const G::Shape& s, int f, int j) {
    const int bone = G::LeftFingers[static_cast<size_t>(f)][static_cast<size_t>(j)];
    const auto& r = B::C0000::Test::TestSkeleton().local[static_cast<size_t>(bone)].q;
    B::Quat d = B::Conjugate(B::Quat{r[0], r[1], r[2], r[3]}) * G::ShapeLocal(s, f, j);
    if (d.w < 0.0f) {
        d = {-d.x, -d.y, -d.z, -d.w};
    }
    return d;
}

float Degrees(const B::Quat& q) {
    return B::AngleOf(q) / B::Deg;
}

} // namespace

TEST(HandGrip, BonesAreTheLeftFingers) {
    EXPECT_EQ(G::LeftFingers[0][0], B::C0000::L_Finger0);
    EXPECT_EQ(G::LeftFingers[4][2], B::C0000::L_Finger42);
    for (const auto& finger : G::LeftFingers) {
        for (const int bone : finger) {
            EXPECT_GE(bone, B::C0000::L_Finger0);
            EXPECT_LE(bone, B::C0000::L_Finger42);
        }
    }
}

TEST(HandGrip, NoneIsTheOpenHand) {
    const G::Shape s = G::ShapeOf(G::Kind::None);
    for (int f = 0; f < 5; ++f) {
        for (int j = 0; j < 3; ++j) {
            EXPECT_NEAR(Degrees(FromReference(s, f, j)), 0.0f, 1e-3f) << f << j;
        }
    }
}

// The game's own fist (its right hand on the Saw Cleaver, VR_GRIP_LOG): every finger joint turned
// about local -Y by these angles. Handle reproduces it on the left hand.
TEST(HandGrip, HandleIsTheGamesFist) {
    const G::Shape s = G::ShapeOf(G::Kind::Handle);
    constexpr float Measured[4][3] = {{55, 100, 62}, {71, 96, 70}, {84, 69, 69}, {97, 71, 71}};
    for (int f = 1; f < 5; ++f) {
        for (int j = 0; j < 3; ++j) {
            const B::Quat d = FromReference(s, f, j);
            EXPECT_NEAR(Degrees(d), Measured[f - 1][j], 0.5f) << f << j;
            // About -Y: the curl toward the palm.
            const B::Vec3 axis = B::Normalize(B::Vec3{d.x, d.y, d.z});
            EXPECT_LT(axis.y, -0.99f) << f << j;
        }
    }
    // The thumb's base: the game's opposition turn.
    EXPECT_NEAR(Degrees(FromReference(s, 0, 0)), 25.0f, 0.5f);
}

TEST(HandGrip, ShapesAreValid) {
    for (int k = 0; k < static_cast<int>(G::Kind::Count); ++k) {
        const G::Shape s = G::ShapeOf(static_cast<G::Kind>(k));
        for (int f = 0; f < 5; ++f) {
            for (int j = 0; j < 3; ++j) {
                const B::Quat q = G::ShapeLocal(s, f, j);
                EXPECT_NEAR(B::Dot(q, q), 1.0f, 1e-4f) << k << f << j;
                // No joint bends backwards or past a fist.
                EXPECT_GE(s.bend[static_cast<size_t>(f)][static_cast<size_t>(j)], 0.0f);
                EXPECT_LE(s.bend[static_cast<size_t>(f)][static_cast<size_t>(j)], 110.0f * G::Deg);
            }
            EXPECT_LE(std::abs(s.spread[static_cast<size_t>(f)]), 12.0f * G::Deg);
        }
        EXPECT_NE(std::string_view{G::KindName(static_cast<G::Kind>(k))}, std::string_view{"?"});
    }
}

// Wider items open the hand: every finger of Neck is closed at least as far as Flask, Flask as
// Ball and Ball as Wide (summed over the joints).
TEST(HandGrip, WiderItemsOpenTheHand) {
    const auto closure = [](G::Kind kind, int f) {
        const G::Shape s = G::ShapeOf(kind);
        const auto& b = s.bend[static_cast<size_t>(f)];
        return b[0] + b[1] + b[2];
    };
    for (int f = 1; f < 5; ++f) {
        EXPECT_GE(closure(G::Kind::Neck, f), closure(G::Kind::Flask, f)) << f;
        EXPECT_GE(closure(G::Kind::Flask, f), closure(G::Kind::Ball, f)) << f;
        EXPECT_GE(closure(G::Kind::Ball, f), closure(G::Kind::Wide, f)) << f;
    }
}
