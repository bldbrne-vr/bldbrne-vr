// SPDX-License-Identifier: GPL-2.0-or-later

// The 6DoF room (src/core/vr/head_room.h): the sphere of positional_limit_m above the recentered
// head, a cylinder of its radius positional_limit_down_m deep below it. The old sphere is kept bit
// for bit (down 0, and everything on or above the origin's plane); a straight duck goes 30 cm
// deeper (0.80 m; 15 cm with the first room's 0.65 m); the user's forward-leaning playtest duck is
// followed all the way (15 cm more than the sphere let it), and a deeper forward duck too;
// sideways and forward are unchanged; the clamp is the room's nearest point (continuous, never
// moves a point further than the head moved); the floor guard; and the late warp's CameraPoint is
// the camera tick's clamp.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <gtest/gtest.h>
#include "core/vr/head_room.h"
#include "video_core/renderer_vulkan/vr_late_warp_math.h"

namespace {

namespace H = VR::HeadRoom;

struct P {
    float x, y, z;
};

constexpr float Limit = 0.5f; // positional_limit_m
constexpr float Down = 0.80f; // positional_limit_down_m (0.65 until the evening of 2026-10-08)

P Clamp(P p, float limit = Limit, float down = Down) {
    H::Clamp(p.x, p.y, p.z, limit, down);
    return p;
}

// The code before 2026-10-08 (vr_session.cpp OnCameraTick), verbatim.
P OldSphere(P d, float limit_m = Limit) {
    const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const float limit = std::max(limit_m, 0.0f);
    if (length > limit && length > 0.0f) {
        const float k = limit / length;
        d = {d.x * k, d.y * k, d.z * k};
    }
    return d;
}

bool Same(const P& a, const P& b) {
    return std::memcmp(&a, &b, sizeof(P)) == 0;
}

float Dist(const P& a, const P& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                     (a.z - b.z) * (a.z - b.z));
}

bool Inside(const P& p, float limit = Limit, float down = Down, float eps = 1e-5f) {
    if (p.y >= 0.0f) {
        return std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z) <= limit + eps;
    }
    return std::hypot(p.x, p.z) <= limit + eps && p.y >= -down - eps;
}

} // namespace

TEST(HeadRoom, DownZeroIsTheOldSphereBitForBit) {
    std::mt19937 rng{7};
    std::uniform_real_distribution<float> u(-1.2f, 1.2f);
    int differ = 0;
    for (int i = 0; i < 20000; ++i) {
        const P d{u(rng), u(rng), u(rng)};
        differ += Same(Clamp(d, Limit, 0.0f), OldSphere(d)) ? 0 : 1;
        differ += Same(Clamp(d, 0.3f, 0.0f), OldSphere(d, 0.3f)) ? 0 : 1;
    }
    EXPECT_EQ(differ, 0);
    // The origin itself, and the sphere's own inside.
    EXPECT_TRUE(Same(Clamp({0, 0, 0}, Limit, 0.0f), P{0, 0, 0}));
    EXPECT_TRUE(Same(Clamp({0.1f, -0.2f, 0.3f}, Limit, 0.0f), P{0.1f, -0.2f, 0.3f}));
}

TEST(HeadRoom, OnAndAboveTheOriginItIsTheOldSphereBitForBit) {
    std::mt19937 rng{11};
    std::uniform_real_distribution<float> u(-1.2f, 1.2f);
    std::uniform_real_distribution<float> up(0.0f, 1.2f);
    int differ = 0;
    for (int i = 0; i < 20000; ++i) {
        const P d{u(rng), i % 50 == 0 ? 0.0f : up(rng), u(rng)};
        differ += Same(Clamp(d), OldSphere(d)) ? 0 : 1;
    }
    EXPECT_EQ(differ, 0);
}

TEST(HeadRoom, StraightDownGoesThirtyCentimetresDeeper) {
    // The scripted mock duck: 0.90 m straight down (the user: "15cm more? (so up to 30 in total)").
    const P old = OldSphere({0.0f, -0.90f, 0.0f});
    const P first = Clamp({0.0f, -0.90f, 0.0f}, Limit, 0.65f); // the first room (0.65 m)
    const P now = Clamp({0.0f, -0.90f, 0.0f});
    std::printf("  straight duck 0.90 m: the view drops %.3f m (0.65 room %.3f m, sphere %.3f m)\n",
                -now.y, -first.y, -old.y);
    EXPECT_NEAR(old.y, -0.50f, 1e-6f);
    EXPECT_NEAR(first.y, -0.65f, 1e-6f);
    EXPECT_NEAR(now.y, -0.80f, 1e-6f);
    EXPECT_NEAR(now.y - old.y, -0.30f, 1e-6f);
    EXPECT_NEAR(now.x, 0.0f, 1e-6f);
    EXPECT_NEAR(now.z, 0.0f, 1e-6f);
    // Within the room a duck is followed one to one (the first room's 0.70 m mock duck too).
    for (const float y : {-0.6f, -0.70f, -0.79f}) {
        EXPECT_NEAR(Clamp({0.0f, y, 0.0f}).y, y, 1e-6f);
    }
}

TEST(HeadRoom, TheUsersForwardLeaningDuckGetsAboutFifteenCentimetresMore) {
    // usertest_hold10.txt, 'VR evade': the neck at (-0.104, -0.560, -0.368) m (a duck that leans
    // forward), 0.68 m from the origin: the sphere scaled the whole offset back to 0.5 m.
    const P duck{-0.104f, -0.560f, -0.368f};
    const P old = OldSphere(duck);
    const P now = Clamp(duck);
    std::printf("  forward-leaning duck: drop %.3f m (sphere %.3f m, +%.1f cm), forward %.3f m "
                "(sphere %.3f m)\n",
                -now.y, -old.y, 100.0f * (old.y - now.y), -now.z, -old.z);
    EXPECT_NEAR(old.y, -0.413f, 2e-3f);
    EXPECT_NEAR(now.y, -0.560f, 1e-6f); // followed all the way
    EXPECT_GT(old.y - now.y, 0.14f);
    EXPECT_LT(old.y - now.y, 0.16f);
    // ...and the lean with it (inside the 0.5 m horizontally: not clamped either).
    EXPECT_NEAR(now.x, -0.104f, 1e-6f);
    EXPECT_NEAR(now.z, -0.368f, 1e-6f);
    // The scripted mock lean-duck: down 0.56, forward 0.37.
    const P mock = Clamp({0.0f, -0.56f, -0.37f});
    const P mock_old = OldSphere({0.0f, -0.56f, -0.37f});
    std::printf("  mock lean-duck (down 0.56, fwd 0.37): drop %.3f m (sphere %.3f m)\n", -mock.y,
                -mock_old.y);
    EXPECT_NEAR(mock.y, -0.56f, 1e-6f);
    EXPECT_NEAR(mock_old.y, -0.417f, 2e-3f);
    // The deep forward duck of the duck3 runs (down 0.75, forward 0.30): followed all the way
    // (the first room stopped it at 0.65 m).
    const P deep = Clamp({0.0f, -0.75f, -0.30f});
    EXPECT_NEAR(deep.y, -0.75f, 1e-6f);
    EXPECT_NEAR(deep.z, -0.30f, 1e-6f);
    EXPECT_NEAR(Clamp({0.0f, -0.75f, -0.30f}, Limit, 0.65f).y, -0.65f, 1e-6f);
    // Deeper than the room: the drop stops at 0.80 m, the lean is kept.
    const P deeper = Clamp({0.0f, -0.95f, -0.30f});
    EXPECT_NEAR(deeper.y, -0.80f, 1e-6f);
    EXPECT_NEAR(deeper.z, -0.30f, 1e-6f);
}

TEST(HeadRoom, SidewaysForwardAndUpAreUnchanged) {
    for (const P d : {P{0.7f, 0.0f, 0.0f}, P{-0.9f, 0.0f, 0.0f}, P{0.0f, 0.0f, -0.8f},
                      P{0.0f, 0.0f, 0.6f}, P{0.0f, 0.7f, 0.0f}, P{0.4f, 0.3f, -0.3f},
                      P{0.3f, 0.0f, 0.2f}}) {
        EXPECT_TRUE(Same(Clamp(d), OldSphere(d)));
    }
    EXPECT_NEAR(Clamp({0.7f, 0.0f, 0.0f}).x, 0.5f, 1e-6f);
    EXPECT_NEAR(Clamp({0.0f, 0.0f, -0.8f}).z, -0.5f, 1e-6f);
    // Below the origin the horizontal part never exceeds the same 0.5 m (the wall guard).
    const P low = Clamp({0.9f, -0.2f, 0.0f});
    EXPECT_NEAR(low.x, 0.5f, 1e-6f);
    EXPECT_NEAR(low.y, -0.2f, 1e-6f);
    const P diag = Clamp({0.6f, -0.3f, -0.6f});
    EXPECT_NEAR(std::hypot(diag.x, diag.z), 0.5f, 1e-5f);
}

TEST(HeadRoom, TheClampIsTheRoomsNearestPoint) {
    // Projection onto a convex set: inside points stay, the result is inside, nothing in the room
    // is nearer (the obtuse-angle test against sampled room points), and two heads never end up
    // further apart than they were (continuous across the origin's plane: no jump when a lean
    // turns into a duck).
    std::mt19937 rng{5};
    std::uniform_real_distribution<float> u(-1.3f, 1.3f);
    int outside = 0, moved_inside = 0, nearer = 0, stretched = 0;
    for (int i = 0; i < 20000; ++i) {
        const P a{u(rng), u(rng), u(rng)};
        const P ca = Clamp(a);
        outside += Inside(ca) ? 0 : 1;
        if (Inside(a, Limit, Down, 0.0f)) {
            moved_inside += Same(a, ca) ? 0 : 1;
        }
        const P s{u(rng) * 0.5f, u(rng) * 0.5f, u(rng) * 0.5f};
        if (Inside(s, Limit, Down, 0.0f)) {
            const float dot = (a.x - ca.x) * (s.x - ca.x) + (a.y - ca.y) * (s.y - ca.y) +
                              (a.z - ca.z) * (s.z - ca.z);
            nearer += dot > 1e-4f ? 1 : 0;
        }
        const P b{a.x + u(rng) * 0.05f, a.y + u(rng) * 0.05f, a.z + u(rng) * 0.05f};
        stretched += Dist(Clamp(b), ca) > Dist(a, b) + 1e-5f ? 1 : 0;
    }
    EXPECT_EQ(outside, 0);
    EXPECT_EQ(moved_inside, 0);
    EXPECT_EQ(nearer, 0);
    EXPECT_EQ(stretched, 0);
    // Across the plane, a sideways lean past the limit turning into a duck: no step.
    float worst = 0.0f;
    P last = Clamp({0.8f, 0.05f, 0.0f});
    for (int k = 1; k <= 100; ++k) {
        const P now = Clamp({0.8f, 0.05f - 0.001f * static_cast<float>(k), 0.0f});
        worst = std::max(worst, Dist(now, last));
        last = now;
    }
    EXPECT_LT(worst, 1.01e-3f);
}

TEST(HeadRoom, PositionalOffNeverMovesTheCamera) {
    for (const P d : {P{0.2f, -0.6f, 0.1f}, P{0.0f, -0.3f, 0.0f}, P{0.3f, 0.2f, 0.0f}}) {
        const P c = Clamp(d, 0.0f, Down);
        EXPECT_EQ(c.x, 0.0f);
        EXPECT_EQ(c.y, 0.0f);
        EXPECT_EQ(c.z, 0.0f);
    }
    EXPECT_EQ(H::DownLimit(0.0f, Down, 1.58f), 0.0f);
}

TEST(HeadRoom, DownLimitAndTheFloor) {
    // The user's eye (fp_eye_height_m 1.58): the whole 0.80 m, the eye 0.78 m over his feet
    // (0.43 m above the margin: the near plane's 0.1 m and his chin far from the floor).
    EXPECT_NEAR(H::DownLimit(Limit, Down, 1.58f), 0.80f, 1e-6f);
    EXPECT_NEAR(H::DownLimit(Limit, Down, 1.55f), 0.80f, 1e-6f);
    EXPECT_NEAR(1.58f - H::DownLimit(Limit, Down, 1.58f), 0.78f, 1e-6f);
    // The guard binds only with an eye under 1.15 m (0.80 + the 0.35 m margin).
    EXPECT_NEAR(H::DownLimit(Limit, Down, 1.15f), 0.80f, 1e-6f);
    EXPECT_NEAR(H::DownLimit(Limit, Down, 1.10f), 0.75f, 1e-6f);
    // 0 = the old sphere.
    EXPECT_EQ(H::DownLimit(Limit, 0.0f, 1.58f), 0.0f);
    // A room far deeper than the hunter's eye: kept FloorMarginM over his feet.
    EXPECT_NEAR(H::DownLimit(Limit, 1.5f, 1.58f), 1.58f - H::FloorMarginM, 1e-6f);
    EXPECT_NEAR(H::DownLimit(Limit, Down, 0.9f), 0.9f - H::FloorMarginM, 1e-6f);
    // No meaningful eye height: no guard.
    EXPECT_NEAR(H::DownLimit(Limit, Down, 0.0f), 0.80f, 1e-6f);
    // The deepest drop the camera follows (physical evasion's duck cap stays within it).
    EXPECT_NEAR(H::MaxDrop(Limit, H::DownLimit(Limit, Down, 1.58f)), 0.80f, 1e-6f);
    EXPECT_NEAR(H::MaxDrop(Limit, 0.0f), 0.5f, 1e-6f);
    // Every clamped drop keeps the eye over the floor by the margin.
    const float down = H::DownLimit(Limit, 1.5f, 1.0f);
    const P c = Clamp({0.0f, -1.4f, 0.0f}, Limit, down);
    EXPECT_GE(1.0f + c.y, H::FloorMarginM - 1e-6f);
}

TEST(HeadRoom, TheHurtboxesDuckCap) {
    // Physical evasion's counted duck (bloodborne_evasion.cpp DuckCap): evasion_duck_m 0.75 within
    // the camera's 0.80 m room (the user's eye 1.58 m); 0.60 / 0.65 before.
    EXPECT_NEAR(H::DuckCap(0.75f, Limit, Down, 1.58f), 0.75f, 1e-6f);
    EXPECT_NEAR(H::DuckCap(0.60f, Limit, 0.65f, 1.58f), 0.60f, 1e-6f);
    // Never deeper than the camera goes: a larger evasion_duck_m stops at the room...
    EXPECT_NEAR(H::DuckCap(1.0f, Limit, Down, 1.58f), 0.80f, 1e-6f);
    EXPECT_NEAR(H::DuckCap(0.75f, Limit, 0.65f, 1.58f), 0.65f, 1e-6f);
    // ...the floor guard's room (a low eye), or the sphere's radius without a down room.
    EXPECT_NEAR(H::DuckCap(0.75f, Limit, Down, 1.0f), 1.0f - H::FloorMarginM, 1e-6f);
    EXPECT_NEAR(H::DuckCap(0.75f, Limit, 0.0f, 1.58f), 0.5f, 1e-6f);
    // positional=0 (no room at all): the cap is the radius 0.
    EXPECT_NEAR(H::DuckCap(0.75f, 0.0f, Down, 1.58f), 0.0f, 1e-6f);
}

TEST(HeadRoom, TheLateWarpsCameraPointIsTheCameraTicksClamp) {
    namespace VL = Vulkan::VrLate;
    std::mt19937 rng{3};
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    int differ = 0, differ_old = 0;
    for (int i = 0; i < 5000; ++i) {
        const VL::Vec3 origin{u(rng) * 0.5f, 1.6f + u(rng) * 0.2f, u(rng) * 0.5f};
        const VL::Vec3 head = origin + VL::Vec3{u(rng) * 0.8f, u(rng) * 0.9f, u(rng) * 0.8f};
        // No forward eye: the head's part alone, bit for bit.
        const VL::Vec3 c = VL::CameraPoint(head, u(rng) * 3.1f, origin, Limit, 0.0f, Down);
        const P want = Clamp({head.x - origin.x, head.y - origin.y, head.z - origin.z});
        differ += Same(P{c.x, c.y, c.z}, want) ? 0 : 1;
        // The default (no down room) is the sphere the late warp used before.
        const VL::Vec3 o = VL::CameraPoint(head, 0.0f, origin, Limit, 0.0f);
        const P old = OldSphere({head.x - origin.x, head.y - origin.y, head.z - origin.z});
        differ_old += Same(P{o.x, o.y, o.z}, old) ? 0 : 1;
    }
    EXPECT_EQ(differ, 0);
    EXPECT_EQ(differ_old, 0);
}
