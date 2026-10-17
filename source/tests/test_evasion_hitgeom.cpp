// SPDX-License-Identifier: GPL-2.0-or-later

// Physical evasion's hit log geometry (core/vr/evasion_hitgeom.h): segment distances, an attack
// capsule swept past a hurtbox (miss, hit, tunnelling through it in one frame), spheres and the
// classification table.

#include <cmath>
#include <string>

#include <gtest/gtest.h>

#include "core/vr/evasion_hitgeom.h"

namespace {

namespace E = VR::Evade;

TEST(EvasionHitGeom, SegmentDistances) {
    // Parallel, 1 m apart.
    EXPECT_NEAR(E::SegSegDist2({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}), 1.0f, 1e-6f);
    // Crossing (skew lines 0.5 apart).
    EXPECT_NEAR(E::SegSegDist2({-1, 0, 0}, {1, 0, 0}, {0, -1, 0.5f}, {0, 1, 0.5f}), 0.25f, 1e-6f);
    // Endpoint to endpoint.
    EXPECT_NEAR(E::SegSegDist2({0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}), 1.0f, 1e-6f);
    // Degenerate: point to segment, point to point.
    EXPECT_NEAR(E::SegSegDist2({0.5f, 2, 0}, {0.5f, 2, 0}, {0, 0, 0}, {1, 0, 0}), 4.0f, 1e-6f);
    EXPECT_NEAR(E::SegSegDist2({0, 0, 0}, {0, 0, 0}, {0, 3, 4}, {0, 3, 4}), 25.0f, 1e-5f);
    // Closest points.
    E::Vec3 c1, c2;
    E::SegSegClosest({-1, 0, 0}, {1, 0, 0}, {0.25f, -1, 0.5f}, {0.25f, 1, 0.5f}, c1, c2);
    EXPECT_NEAR(c1.x, 0.25f, 1e-5f);
    EXPECT_NEAR(c2.z, 0.5f, 1e-5f);
}

// A blade (capsule r 0.1, 1 m long, held horizontally along x) swept along +z past Spine02
// (r 0.144, vertical at x = 0, from y 1.21 to 1.34): it passes the hurtbox at `offset` m to the
// side (the hurtbox moved by the lean).
E::Sweep BladePast(float offset) {
    const E::Vec3 a{-1.2f + offset, 1.27f, -0.6f};
    const E::Vec3 b{-0.2f + offset, 1.27f, -0.6f};
    return E::SweptCapsule(a, b, 0.1f, {0.0f, 0.0f, 1.2f}, {0.0f, 1.21f, 0.0f}, {0.0f, 1.34f, 0.0f},
                           0.144f);
}

TEST(EvasionHitGeom, SweptCapsuleMissesAndHits) {
    // The blade's tip is at x = -0.2 + offset: with no lean it passes 0.2 m from the hurtbox's
    // axis, 0.2 - 0.1 - 0.144 < 0: a hit.
    EXPECT_TRUE(BladePast(0.0f).hit);
    // Lean 0.30 m away from the tip: 0.5 - 0.244 = 0.256 m clear.
    const E::Sweep miss = BladePast(-0.30f);
    EXPECT_FALSE(miss.hit);
    EXPECT_NEAR(miss.clearance, 0.50f - 0.244f, 0.01f);
    // A small lean (0.03 m) is still a hit: 0.23 - 0.244 < 0.
    EXPECT_TRUE(BladePast(-0.03f).hit);
}

TEST(EvasionHitGeom, TunnellingInOneFrameIsAHit) {
    // A thin fast thrust: from 0.6 m in front to 0.6 m behind in one frame, straight through.
    const E::Sweep s =
        E::SweptCapsule({-0.05f, 1.27f, -0.6f}, {0.05f, 1.27f, -0.6f}, 0.03f, {0.0f, 0.0f, 1.2f},
                        {0.0f, 1.21f, 0.0f}, {0.0f, 1.34f, 0.0f}, 0.144f);
    EXPECT_TRUE(s.hit);
    EXPECT_NEAR(s.t, 0.5f, 0.1f);
}

TEST(EvasionHitGeom, SphereIsADegenerateCapsule) {
    // A sphere of 0.2 m passing 0.3 m beside the Head01 capsule (r 0.094): 0.006 m clear.
    const E::Vec3 c{0.3f + 0.094f + 0.2f + 0.006f, 1.58f, -1.0f};
    const E::Sweep s = E::SweptCapsule(c, c, 0.2f, {0.0f, 0.0f, 2.0f}, {0.0f, 1.56f, 0.0f},
                                       {0.0f, 1.60f, 0.0f}, 0.094f);
    EXPECT_FALSE(s.hit);
    EXPECT_NEAR(s.clearance, 0.306f, 0.01f);
    EXPECT_NEAR(s.point.z, 0.0f, 0.05f);
}

TEST(EvasionHitGeom, Classification) {
    // (vanilla, leaned, game)
    EXPECT_EQ(E::Classify(true, false, false), E::HitClass::Evaded);
    EXPECT_EQ(E::Classify(false, true, true), E::HitClass::Traded);
    EXPECT_EQ(E::Classify(false, false, true), E::HitClass::Traded); // the model missed it
    EXPECT_EQ(E::Classify(true, true, true), E::HitClass::Both);
    EXPECT_EQ(E::Classify(false, false, false), E::HitClass::None);
    // The capsules met but the game did not count it (a grab, i-frames): never an evasion.
    EXPECT_EQ(E::Classify(true, true, false), E::HitClass::Ignored);
    EXPECT_EQ(std::string(E::HitClassName(E::HitClass::Evaded)), std::string("EVADED"));
    EXPECT_EQ(std::string(E::HitClassName(E::HitClass::Ignored)), std::string("IGNORED"));
}

} // namespace
