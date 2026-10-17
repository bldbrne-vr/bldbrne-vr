// SPDX-License-Identifier: GPL-2.0-or-later

// Solid weapons (core/vr/solid_weapon.h) against a world of walls (planes) and a pole (a
// cylinder), swept as the game's cast does (a sweep starting inside or touching reports nothing):
// in free air the weapon is the controller's; driven into a wall it stops at it and slides along
// it; a slash stops turning at the wall; a fast turn past a thin pole is caught; the bare hand
// stops at a wall and turns while it touches; a corner keeps it out of both walls; pushed or swung
// on into a wall it never goes through; caught on something far from the controller it comes back
// to it; starting in a wall it is put at the wall; random motion among walls, a floor, a pole and
// a bar never leaves it inside; two weapons crossing are pushed apart and keep to their sides, a
// push into a wall refused; what it costs, pressed, sliding and dragged.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/melee_moves.h"
#include "core/vr/solid_weapon.h"
#include "core/vr/sparks.h"
#include "core/vr/weapon_wall.h"

namespace {

namespace S = VR::Solid;
using V = VR::Body::Vec3;
using Q = VR::Body::Quat;
namespace B = VR::Body;

struct Plane {
    V point;
    V n; // out of the solid side
};
struct Pole {
    V c;
    V axis; // unit
    float r;
};

struct World {
    std::vector<Plane> planes;
    std::vector<Pole> poles;
    int casts{0};
    bool flip{false}; // the cast's normals into the surface (the game's sign is not known)

    S::Hit operator()(V a, V b, float r) {
        ++casts;
        S::Hit best{};
        for (const Plane& p : planes) {
            const float sa = B::Dot(p.n, a - p.point);
            const float sb = B::Dot(p.n, b - p.point);
            if (sa <= r || sb >= r) {
                continue; // starts inside or touching (no information), or never reaches it
            }
            const float f = (sa - r) / (sa - sb);
            if (f > 1e-3f && (!best.hit || f < best.f)) {
                best = {true, f, p.n};
            }
        }
        for (const Pole& p : poles) {
            const V A = B::Perp(a - p.c, p.axis);
            const V D = B::Perp(b - a, p.axis);
            const float R = p.r + r;
            const float qa = B::Dot(D, D);
            const float qb = 2.0f * B::Dot(A, D);
            const float qc = B::Dot(A, A) - R * R;
            if (qc <= 0.0f || qa <= 1e-12f) {
                continue;
            }
            const float disc = qb * qb - 4.0f * qa * qc;
            if (disc < 0.0f) {
                continue;
            }
            const float f = (-qb - std::sqrt(disc)) / (2.0f * qa);
            if (f > 1e-3f && f <= 1.0f && (!best.hit || f < best.f)) {
                const V at = a + (b - a) * f;
                best = {true, f, B::Normalize(B::Perp(at - p.c, p.axis))};
            }
        }
        if (flip) {
            best.n = -best.n;
        }
        return best;
    }
    // How far a sphere of radius `r` at `x` is inside anything (<= 0: outside).
    float Depth(V x, float r) const {
        float d = -1e9f;
        for (const Plane& p : planes) {
            d = std::max(d, r - B::Dot(p.n, x - p.point));
        }
        for (const Pole& p : poles) {
            d = std::max(d, p.r + r - B::Length(B::Perp(x - p.c, p.axis)));
        }
        return d;
    }
};

// A 1 m blade along the weapon bone's -Y (from 15 cm out), 3 cm thick; the hand 5 cm.
S::Shape Blade() {
    S::Shape s{};
    s.hand_r = 0.05f;
    s.line = true;
    s.base = {0.0f, -0.15f, 0.0f};
    s.tip = {0.0f, -1.0f, 0.0f};
    s.line_r = 0.03f;
    return s;
}

// The bone turned so the blade points along `dir`.
Q Pointing(V dir) {
    return B::FromTo({0.0f, -1.0f, 0.0f}, dir);
}

const V Chest{-1.0f, 1.0f, 0.0f};

// A wall whose face is the plane x = `x`, solid beyond it.
Plane WallAtX(float x) {
    return {{x, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}};
}

TEST(SolidWeapon, InFreeAirTheWeaponIsTheControllers) {
    World w;
    S::Weapon weapon;
    const S::Config c{};
    for (int i = 0; i <= 20; ++i) {
        const float t = static_cast<float>(i) / 20.0f;
        const S::Pose target{{-0.5f + t, 1.0f + 0.2f * t, 0.1f * t},
                             Pointing(B::Normalize(V{1.0f, t, 0.0f}))};
        const S::Result r = weapon.Step(target, Chest, Blade(), c, w);
        EXPECT_FALSE(r.contact.any);
        EXPECT_FALSE(r.through);
        EXPECT_NEAR(B::Length(r.pose.p - target.p), 0.0f, 1e-6f);
        EXPECT_NEAR(B::AngleBetween(r.pose.q, target.q), 0.0f, 1e-4f);
    }
}

TEST(SolidWeapon, ABladeDrivenIntoAWallStopsAtItAndSlidesAlongIt) {
    World w;
    w.planes.push_back(WallAtX(1.0f));
    S::Weapon weapon;
    const S::Config c{};
    const S::Shape s = Blade();
    const Q along = Pointing({1.0f, 0.0f, 0.0f});
    S::Result r{};
    for (int i = 0; i <= 10; ++i) { // the hand from x -0.5 to +0.3: the tip would reach 1.3
        r = weapon.Step({{-0.5f + 0.08f * static_cast<float>(i), 1.0f, 0.0f}, along}, Chest, s, c,
                        w);
    }
    const float tip = S::At(r.pose, s.tip).x;
    EXPECT_LE(tip, 1.0f - s.line_r - c.skin + 1e-4f); // out of the wall, resting skin off it
    EXPECT_GE(tip, 1.0f - s.line_r - 0.02f);          // ... at it
    EXPECT_TRUE(r.contact.any);
    EXPECT_TRUE(r.contact.line);
    EXPECT_NEAR(r.contact.normal.x, -1.0f, 1e-4f);
    EXPECT_NEAR(r.off, 0.3f - r.pose.p.x, 1e-4f); // the controller went on, the hand did not
    EXPECT_FALSE(r.through);
    // Along the wall: it follows the controller's way up, still at the wall.
    for (int i = 1; i <= 10; ++i) {
        r = weapon.Step({{0.3f, 1.0f + 0.03f * static_cast<float>(i), 0.0f}, along}, Chest, s, c,
                        w);
    }
    EXPECT_NEAR(r.pose.p.y, 1.3f, 0.01f);
    EXPECT_LE(S::At(r.pose, s.tip).x, 1.0f - s.line_r + 1e-4f);
    EXPECT_GE(S::At(r.pose, s.tip).x, 1.0f - s.line_r - 0.02f);
}

// The cast's normal into the surface instead of out of it: the same stop and slide.
TEST(SolidWeapon, ACastsNormalOfEitherSignStopsItTheSame) {
    World w;
    w.flip = true;
    w.planes.push_back(WallAtX(1.0f));
    S::Weapon weapon;
    const S::Config c{};
    const S::Shape s = Blade();
    const Q along = Pointing({1.0f, 0.0f, 0.0f});
    S::Result r{};
    for (int i = 0; i <= 10; ++i) {
        r = weapon.Step({{-0.5f + 0.08f * static_cast<float>(i), 1.0f, 0.0f}, along}, Chest, s, c,
                        w);
    }
    EXPECT_LE(S::At(r.pose, s.tip).x, 1.0f - s.line_r + 1e-4f);
    EXPECT_NEAR(r.contact.normal.x, -1.0f, 1e-4f); // out of the wall, toward the weapon
    for (int i = 1; i <= 10; ++i) {
        r = weapon.Step({{0.3f, 1.0f + 0.03f * static_cast<float>(i), 0.0f}, along}, Chest, s, c,
                        w);
    }
    EXPECT_NEAR(r.pose.p.y, 1.3f, 0.01f);
}

TEST(SolidWeapon, ASlashIntoAWallStopsTurningAtIt) {
    World w;
    w.planes.push_back(WallAtX(0.6f));
    S::Weapon weapon;
    const S::Config c{};
    const S::Shape s = Blade();
    const V grip{0.0f, 1.0f, 0.0f};
    S::Result r{};
    for (int i = 0; i <= 18; ++i) { // from straight up to pointing at the wall, 5 degrees a step
        const float a = static_cast<float>(i) * 5.0f * B::Deg;
        r = weapon.Step({grip, Pointing({std::sin(a), std::cos(a), 0.0f})}, Chest, s, c, w);
    }
    const V tip = S::At(r.pose, s.tip);
    EXPECT_LE(tip.x, 0.6f - s.line_r + 1e-4f);
    EXPECT_GE(tip.x, 0.6f - s.line_r - 0.03f);
    const float turned = std::atan2(tip.x - r.pose.p.x, tip.y - r.pose.p.y) / B::Deg;
    EXPECT_GT(turned, 25.0f);
    EXPECT_LT(turned, 40.0f); // sin(35 deg) of a 1 m blade reaches 0.57 m
    EXPECT_TRUE(r.contact.any);
}

// Pressed on 10 degrees past where it stopped and swept 60 degrees along the wall (about the
// wall's normal) over a second: the blade sweeps along with it, its tip on the wall, never in it.
TEST(SolidWeapon, APressedBladeSweptAlongAWallGoesAlong) {
    World w;
    w.planes.push_back(WallAtX(0.6f));
    S::Weapon weapon;
    const S::Config c{};
    const S::Shape s = Blade();
    const V grip{0.0f, 1.0f, 0.0f};
    S::Result r{};
    for (int i = 0; i <= 18; ++i) {
        const float a = static_cast<float>(i) * 5.0f * B::Deg;
        r = weapon.Step({grip, Pointing({std::sin(a), std::cos(a), 0.0f})}, Chest, s, c, w);
    }
    const V t0 = S::At(r.pose, s.tip) - r.pose.p;
    const float a = std::atan2(t0.x, t0.y) + 10.0f * B::Deg;
    for (int i = 1; i <= 60; ++i) {
        const float sweep = static_cast<float>(i) * B::Deg;
        const V dir{std::sin(a), std::cos(a) * std::cos(sweep), std::cos(a) * std::sin(sweep)};
        r = weapon.Step({grip, Pointing(dir)}, Chest, s, c, w);
        EXPECT_LE(S::At(r.pose, s.tip).x, 0.6f - s.line_r + 1e-4f) << i;
    }
    const V t1 = S::At(r.pose, s.tip) - r.pose.p;
    EXPECT_GT(std::atan2(t1.z, t1.y) / B::Deg, 55.0f);
    EXPECT_GE(S::At(r.pose, s.tip).x, 0.6f - s.line_r - c.skin - 0.01f); // on the wall
}

// Held pressed into the wall at that angle for 2 s: the hand stays where it stopped (a stopped
// turn backs off by turning, never by walking the hand away from the controller).
TEST(SolidWeapon, ABladePressedIntoAWallNeverWalksTheHandAway) {
    World w;
    w.planes.push_back(WallAtX(0.6f));
    S::Weapon weapon;
    const S::Config c{};
    const S::Shape s = Blade();
    const V grip{0.0f, 1.0f, 0.0f};
    S::Result r{};
    for (int i = 0; i <= 18; ++i) {
        const float a = static_cast<float>(i) * 5.0f * B::Deg;
        r = weapon.Step({grip, Pointing({std::sin(a), std::cos(a), 0.0f})}, Chest, s, c, w);
    }
    const V settled = r.pose.p;
    for (int i = 0; i < 180; ++i) {
        r = weapon.Step({grip, Pointing({1.0f, 0.0f, 0.0f})}, Chest, s, c, w);
    }
    EXPECT_LT(B::Length(r.pose.p - settled), 0.005f);
    EXPECT_LT(r.off, 0.03f);
    EXPECT_LE(S::At(r.pose, s.tip).x, 0.6f - s.line_r + 1e-4f);
}

// What it costs: a weapon in free air, three sweeps an update; held against a wall, a few dozen.
TEST(SolidWeapon, TheSweepsAnUpdateTakes) {
    World w;
    S::Weapon weapon;
    const S::Config c{};
    const S::Shape s = Blade();
    const S::Quat along = Pointing({1.0f, 0.0f, 0.0f});
    weapon.Step({{-0.5f, 1.0f, 0.0f}, along}, Chest, s, c, w);
    w.casts = 0;
    weapon.Step({{-0.48f, 1.0f, 0.0f}, along}, Chest, s, c, w); // 2 cm: one substep
    EXPECT_LE(w.casts, 3);
    w.planes.push_back(WallAtX(1.0f));
    for (int i = 0; i < 10; ++i) {
        weapon.Step({{-0.45f + 0.08f * static_cast<float>(i), 1.0f, 0.0f}, along}, Chest, s, c, w);
    }
    w.casts = 0;
    weapon.Step({{0.4f, 1.0f, 0.0f}, along}, Chest, s, c, w); // pressed on into it
    EXPECT_LE(w.casts, 60);
}

// Pressed 20 cm into a wall and slid along it at 3 m/s (60 updates a second), and dragged along
// the floor at a run with the tip held 7 cm under it: a dozen or two sweeps an update, and it
// keeps up (the substeps go along the surface, not into it). Jammed into a corner: as few.
TEST(SolidWeapon, PressedOrDraggedItStaysCheapAndKeepsUp) {
    const S::Config c{};
    const S::Shape s = Blade();
    {
        World w;
        w.planes.push_back(WallAtX(1.0f));
        S::Weapon weapon;
        const Q along = Pointing({1.0f, 0.0f, 0.0f});
        S::Result r{};
        for (int i = 0; i <= 20; ++i) {
            r = weapon.Step({{-0.5f + 0.035f * static_cast<float>(i), 1.0f, 0.0f}, along}, Chest,
                            s, c, w);
        }
        for (int i = 1; i <= 30; ++i) {
            w.casts = 0;
            r = weapon.Step({{0.2f, 1.0f + 0.05f * static_cast<float>(i), 0.0f}, along}, Chest, s,
                            c, w);
            EXPECT_LE(w.casts, 32) << i;
            EXPECT_LE(S::At(r.pose, s.tip).x, 1.0f - s.line_r + 1e-4f) << i;
        }
        EXPECT_NEAR(r.pose.p.y, 2.5f, 0.01f);
    }
    {
        World w;
        w.planes.push_back({{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}); // the floor
        S::Weapon weapon;
        const float a = 60.0f * B::Deg; // the tip 0.8 - sin(60) = 7 cm under the floor
        const Q down = Pointing({0.0f, -std::sin(a), std::cos(a)});
        S::Result r{};
        for (int i = 0; i <= 20; ++i) { // lowered into it
            const float aa = a * static_cast<float>(i) / 20.0f;
            r = weapon.Step({{0.0f, 0.8f, 0.0f}, Pointing({0.0f, -std::sin(aa), std::cos(aa)})},
                            {0.0f, 1.45f, -0.1f}, s, c, w);
        }
        for (int i = 1; i <= 60; ++i) {
            w.casts = 0;
            const float z = 0.1f * static_cast<float>(i); // 6 m/s
            r = weapon.Step({{0.0f, 0.8f, z}, down}, {0.0f, 1.45f, z - 0.1f}, s, c, w);
            EXPECT_LE(w.casts, 32) << i;
            EXPECT_GE(S::At(r.pose, s.tip).y, s.line_r - 1e-4f) << i;
        }
        EXPECT_NEAR(r.pose.p.z, 6.0f, 0.01f);
    }
    {
        // Jammed: the controller's hand 20 cm into a corner, the blade pointing into it, trembling.
        World w;
        w.planes.push_back(WallAtX(1.0f));
        w.planes.push_back({{0.0f, 0.0f, 0.6f}, {0.0f, 0.0f, -1.0f}});
        S::Weapon weapon;
        const Q into = Pointing(B::Normalize(V{1.0f, 0.0f, 1.0f}));
        for (int i = 1; i <= 60; ++i) {
            w.casts = 0;
            const float shake = (i % 2 == 0 ? 1.0f : -1.0f) * 0.002f;
            const S::Result r =
                weapon.Step({{1.2f + shake, 1.0f, 0.8f - shake}, into}, Chest, s, c, w);
            if (i > 1) {
                EXPECT_LE(w.casts, 48) << i;
            }
            EXPECT_LE(r.pose.p.x, 1.0f - s.hand_r + 1e-4f) << i;
            EXPECT_LE(r.pose.p.z, 0.6f - s.hand_r + 1e-4f) << i;
        }
    }
}

TEST(SolidWeapon, AFastTurnPastAThinPoleIsCaught) {
    World w;
    w.poles.push_back({{0.5f, 1.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, 0.02f});
    S::Weapon weapon;
    const S::Config c{};
    const S::Shape s = Blade();
    const V grip{0.0f, 1.0f, 0.0f};
    weapon.Step({grip, Pointing({0.0f, 1.0f, 0.0f})}, Chest, s, c, w);
    // Straight up to straight out in one update: both ends clear of the pole, the way not.
    const S::Result r = weapon.Step({grip, Pointing({1.0f, 0.0f, 0.0f})}, Chest, s, c, w);
    EXPECT_TRUE(r.contact.any);
    const V tip = S::At(r.pose, s.tip);
    const float turned = std::atan2(tip.x - grip.x, tip.y - grip.y) / B::Deg;
    EXPECT_LT(turned, 45.0f); // stopped before the pole (at 45 degrees the blade is on it)
}

// A bar thinner than the blade's step, on the tip's way between two checks: the tip's sweep
// catches it (the line at either end passes it by).
TEST(SolidWeapon, AThinBarOnTheTipsWayIsCaught) {
    World w;
    S::Weapon weapon;
    const S::Config c{};
    const S::Shape s = Blade();
    const V grip{0.0f, 1.0f, 0.0f};
    const float a0 = 0.0f;
    const float a1 = 0.075f; // radians: the tip 7.5 cm, one substep
    weapon.Step({grip, Pointing({std::sin(a0), std::cos(a0), 0.0f})}, Chest, s, c, w);
    const V mid = grip + V{std::sin(0.5f * a1), std::cos(0.5f * a1), 0.0f} * std::cos(0.5f * a1);
    w.poles.push_back({mid, {0.0f, 0.0f, 1.0f}, 0.003f});
    const S::Result r =
        weapon.Step({grip, Pointing({std::sin(a1), std::cos(a1), 0.0f})}, Chest, s, c, w);
    EXPECT_TRUE(r.contact.any);
    EXPECT_TRUE(r.contact.line);
}

TEST(SolidWeapon, TheBareHandStopsAtAWall) {
    World w;
    w.planes.push_back(WallAtX(0.5f));
    S::Weapon weapon;
    const S::Config c{};
    S::Shape hand{};
    hand.hand_r = 0.05f;
    S::Result r{};
    for (int i = 0; i <= 10; ++i) {
        r = weapon.Step({{0.07f * static_cast<float>(i), 1.0f, 0.0f}, Q{}}, Chest, hand, c, w);
    }
    EXPECT_LE(r.pose.p.x, 0.5f - hand.hand_r + 1e-4f);
    EXPECT_GE(r.pose.p.x, 0.5f - hand.hand_r - 0.02f);
    EXPECT_TRUE(r.contact.any);
    EXPECT_FALSE(r.contact.line);
}

// The bare hand pressed on a wall turns with the controller (only its way is stopped).
TEST(SolidWeapon, TheBareHandTurnsWhileItTouchesAWall) {
    World w;
    w.planes.push_back(WallAtX(0.5f));
    S::Weapon weapon;
    const S::Config c{};
    S::Shape hand{};
    hand.hand_r = 0.05f;
    for (int i = 0; i <= 10; ++i) {
        weapon.Step({{0.07f * static_cast<float>(i), 1.0f, 0.0f}, Q{}}, Chest, hand, c, w);
    }
    const Q turned = B::FromTo({0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
    const S::Result r = weapon.Step({{0.75f, 1.0f, 0.0f}, turned}, Chest, hand, c, w);
    EXPECT_TRUE(r.contact.any);
    EXPECT_NEAR(B::AngleBetween(r.pose.q, turned), 0.0f, 1e-4f);
    EXPECT_LE(r.pose.p.x, 0.5f - hand.hand_r + 1e-4f);
}

TEST(SolidWeapon, ACornerKeepsItOutOfBothWalls) {
    World w;
    w.planes.push_back(WallAtX(1.0f));
    w.planes.push_back({{0.0f, 0.0f, 0.6f}, {0.0f, 0.0f, -1.0f}}); // the face z = 0.6
    S::Weapon weapon;
    const S::Config c{};
    const S::Shape s = Blade();
    const Q along = Pointing({1.0f, 0.0f, 0.0f});
    S::Result r{};
    for (int i = 0; i <= 12; ++i) { // toward the corner, diagonally
        const float t = static_cast<float>(i) * 0.05f;
        r = weapon.Step({{-0.5f + t, 1.0f, t}, along}, Chest, s, c, w);
    }
    const V tip = S::At(r.pose, s.tip);
    EXPECT_LE(tip.x, 1.0f - s.line_r + 1e-4f);
    EXPECT_LE(r.pose.p.z, 0.6f - s.hand_r + 1e-4f);
    EXPECT_LE(tip.z, 0.6f - s.line_r + 1e-4f);
}

// The controller pushed on 1 m past where the weapon stopped, then swung on past the wall: the
// weapon stays at the wall, never in it; back out, it comes with the controller.
TEST(SolidWeapon, PushedOrSwungOnIntoAWallItNeverGoesThrough) {
    World w;
    w.planes.push_back(WallAtX(1.0f));
    S::Weapon weapon;
    const S::Config c{};
    const S::Shape s = Blade();
    const Q along = Pointing({1.0f, 0.0f, 0.0f});
    S::Result r{};
    for (int i = 0; i <= 20; ++i) {
        r = weapon.Step({{-0.5f + 0.075f * static_cast<float>(i), 1.0f, 0.0f}, along}, Chest, s,
                        c, w);
        EXPECT_FALSE(r.through);
        EXPECT_FALSE(r.back);
        EXPECT_LE(S::At(r.pose, s.tip).x, 1.0f - s.line_r + 1e-4f) << i;
        EXPECT_LE(r.pose.p.x, 1.0f - s.hand_r + 1e-4f) << i;
    }
    EXPECT_GT(r.off, 0.9f); // the controller 1 m on
    // Swung on: from pointing at the wall down to pointing back at the hunter, the hand 1 m in it.
    for (int i = 0; i <= 36; ++i) {
        const float a = static_cast<float>(i) * 5.0f * B::Deg;
        r = weapon.Step({{1.0f, 1.0f, 0.0f}, Pointing({std::cos(a), -std::sin(a), 0.0f})}, Chest, s,
                        c, w);
        EXPECT_FALSE(r.through);
        EXPECT_LE(S::At(r.pose, s.tip).x, 1.0f - s.line_r + 1e-4f) << i;
        EXPECT_LE(r.pose.p.x, 1.0f - s.hand_r + 1e-4f) << i;
    }
    for (int i = 1; i <= 6; ++i) { // back out over a few updates
        const float k = static_cast<float>(i) / 6.0f;
        r = weapon.Step({{1.0f - 1.2f * k, 1.0f, 0.0f},
                         B::Slerp(Pointing({-1.0f, 0.0f, 0.0f}), along, k)},
                        Chest, s, c, w);
        EXPECT_LE(S::At(r.pose, s.tip).x, 1.0f - s.line_r + 1e-4f) << i;
    }
    r = weapon.Step({{-0.2f, 1.0f, 0.0f}, along}, Chest, s, c, w);
    EXPECT_NEAR(r.pose.p.x, -0.2f, 1e-4f); // with the controller again
    EXPECT_NEAR(B::AngleBetween(r.pose.q, along), 0.0f, 1e-3f);
}

// Caught under a bar while the controller rises past it (the hunter walking on): it stays caught,
// out of the bar, until the controller is let_go_m off; then, the weapon there clear from the
// chest, it is back at the controller.
TEST(SolidWeapon, CaughtFarFromTheControllerItComesBack) {
    World w;
    w.poles.push_back({{0.6f, 1.25f, 0.0f}, {0.0f, 0.0f, 1.0f}, 0.03f});
    S::Weapon weapon;
    const S::Config c{};
    const S::Shape s = Blade();
    const Q along = Pointing({1.0f, 0.0f, 0.0f});
    S::Result r{};
    bool back = false;
    for (int i = 0; i <= 30; ++i) {
        const S::Pose target{{0.0f, 1.0f + 0.03f * static_cast<float>(i), 0.0f}, along};
        r = weapon.Step(target, Chest, s, c, w);
        back = back || r.back;
        if (!back) {
            EXPECT_LE(S::At(r.pose, s.tip).y, 1.25f - 0.03f - s.line_r + 1e-3f) << i; // under it
            EXPECT_NEAR(r.pose.p.x, 0.0f, 1e-3f) << i; // never sliding back along itself
            EXPECT_LE(r.off, c.let_go_m + 0.03f + 1e-4f) << i;
        }
    }
    EXPECT_TRUE(back);
    EXPECT_NEAR(r.pose.p.y, 1.9f, 1e-4f); // with the controller, over the bar
    EXPECT_FALSE(r.through);
}

// Where it starts in a wall (the first update, after the game had the arm): put at the wall at
// once, reached from the chest: the hand where the controller is (clear), the blade turned out
// only as far as it must; the hand itself in the wall: at the wall.
TEST(SolidWeapon, StartingInAWallItIsPutAtTheWall) {
    World w;
    w.planes.push_back(WallAtX(1.0f));
    S::Weapon weapon;
    const S::Config c{};
    const S::Shape s = Blade();
    const Q along = Pointing({1.0f, 0.0f, 0.0f});
    S::Result r = weapon.Step({{0.5f, 1.0f, 0.0f}, along}, Chest, s, c, w); // the tip at 1.5
    EXPECT_FALSE(r.through);
    EXPECT_NEAR(r.pose.p.x, 0.5f, 1e-4f);
    EXPECT_LE(S::At(r.pose, s.tip).x, 1.0f - s.line_r + 1e-4f);
    EXPECT_GE(S::At(r.pose, s.tip).x, 1.0f - s.line_r - 0.05f); // its tip at the wall
    r = weapon.Step({{0.5f, 1.0f, 0.0f}, along}, Chest, s, c, w); // and stays there
    EXPECT_LE(S::At(r.pose, s.tip).x, 1.0f - s.line_r + 1e-4f);
    weapon.Reset();
    r = weapon.Step({{1.2f, 1.0f, 0.0f}, along}, Chest, s, c, w);
    EXPECT_FALSE(r.through);
    EXPECT_LE(r.pose.p.x, 1.0f - s.hand_r + 1e-4f);
    EXPECT_GE(r.pose.p.x, 1.0f - s.hand_r - 0.02f);
    EXPECT_LE(S::At(r.pose, s.tip).x, 1.0f - s.line_r + 1e-4f);
}

// In a crevice (a 60 cm box about the hand: no way for the 1 m blade): the controller's until it
// can be reached; the box's lid off, it is turned out of the box (up) and solid.
TEST(SolidWeapon, InACreviceItWaitsUntilItCanBeReached) {
    World w;
    for (const V n : {V{1, 0, 0}, V{-1, 0, 0}, V{0, 1, 0}, V{0, 0, 1}, V{0, 0, -1}, V{0, -1, 0}}) {
        w.planes.push_back({V{0.0f, 1.0f, 0.0f} - n * 0.3f, n});
    }
    S::Weapon weapon;
    const S::Config c{};
    const S::Shape s = Blade();
    const S::Pose target{{0.0f, 1.0f, 0.0f}, Pointing({1.0f, 0.0f, 0.0f})};
    const V chest{0.0f, 1.0f, 0.1f}; // in the box too
    S::Result r = weapon.Step(target, chest, s, c, w);
    EXPECT_TRUE(r.through);
    EXPECT_NEAR(B::Length(r.pose.p - target.p), 0.0f, 1e-6f);
    w.planes.pop_back(); // the lid
    r = weapon.Step(target, chest, s, c, w);
    EXPECT_FALSE(r.through);
    const V tip = S::At(r.pose, s.tip);
    EXPECT_LE(std::abs(tip.x), 0.3f - s.line_r + 1e-4f);
    EXPECT_LE(std::abs(tip.z), 0.3f - s.line_r + 1e-4f);
    EXPECT_GE(tip.y, 0.7f + s.line_r - 1e-4f);
    EXPECT_GT(tip.y, 1.5f); // out of the top
}

TEST(SolidWeapon, TwoWeaponsCrossingArePushedApart) {
    S::Shape a = Blade();
    S::Shape b = Blade();
    b.line_r = 0.025f;
    S::Pose pa{{0.0f, 1.0f, 0.0f}, Pointing({1.0f, 0.0f, 0.0f})};
    S::Pose pb{{0.5f, 1.01f, -0.5f}, Pointing({0.0f, 0.0f, 1.0f})}; // across it, 1 cm apart
    const S::Touch t = S::Separate(pa, a, pb, b);
    ASSERT_TRUE(t.any);
    EXPECT_NEAR(t.depth, a.line_r + b.line_r - 0.01f, 1e-4f);
    float u = 0.0f;
    float v = 0.0f;
    S::Closest(S::At(pa, a.base), S::At(pa, a.tip), S::At(pb, b.base), S::At(pb, b.tip), u, v);
    const float gap = B::Length(B::Lerp(S::At(pa, a.base), S::At(pa, a.tip), u) -
                                B::Lerp(S::At(pb, b.base), S::At(pb, b.tip), v));
    EXPECT_NEAR(gap, a.line_r + b.line_r, 1e-4f);
    EXPECT_LT(t.normal.y, 0.0f); // a pushed down, b up (a was below)
    // Apart already: nothing.
    EXPECT_FALSE(S::Separate(pa, a, pb, b).any);
}

// Pressed on across each other (one blade lowered through the other), two weapons keep to the
// sides they met on: the upper one stays above, the two lines their thicknesses apart.
TEST(SolidWeapon, TwoWeaponsPressedOnKeepToTheirSides) {
    S::Shape a = Blade();
    S::Shape b = Blade();
    b.line_r = 0.025f;
    const float reach = a.line_r + b.line_r;
    V side{};
    bool met = false;
    for (int i = 0; i <= 30; ++i) { // b's controller from 20 cm above a to 10 cm below it
        S::Pose pa{{0.0f, 1.0f, 0.0f}, Pointing({1.0f, 0.0f, 0.0f})};
        S::Pose pb{{0.5f, 1.2f - 0.01f * static_cast<float>(i), -0.5f},
                   Pointing({0.0f, 0.0f, 1.0f})};
        const S::Touch t = S::Separate(pa, a, pb, b, met ? &side : nullptr);
        met = t.any;
        if (t.any) {
            side = t.normal;
            EXPECT_NEAR(pb.p.y - pa.p.y, reach, 1e-4f) << i;
        } else {
            EXPECT_GE(pb.p.y - pa.p.y, reach - 1e-4f) << i;
        }
    }
    EXPECT_TRUE(met);
    // Too far across (past CrossM): they let each other pass.
    S::Pose pa{{0.0f, 1.0f, 0.0f}, Pointing({1.0f, 0.0f, 0.0f})};
    S::Pose pb{{0.5f, 1.0f - S::CrossM - 0.05f, -0.5f}, Pointing({0.0f, 0.0f, 1.0f})};
    EXPECT_FALSE(S::Separate(pa, a, pb, b, &side).any);
}

// One weapon pushed by the other (Separate) only moves when the way is clear: a blade on the floor
// pushed down stays on it, pushed up it goes.
TEST(SolidWeapon, APushIntoAWallIsRefused) {
    World w;
    w.planes.push_back({{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}); // the floor
    S::Weapon weapon;
    const S::Config c{};
    const S::Shape s = Blade();
    const Q flat = Pointing({1.0f, 0.0f, 0.0f});
    S::Result r{};
    for (int i = 0; i <= 10; ++i) { // lowered onto the floor
        r = weapon.Step({{0.0f, 0.3f - 0.03f * static_cast<float>(i), 0.0f}, flat}, Chest, s, c, w);
    }
    ASSERT_TRUE(r.contact.any);
    S::Pose pose = r.pose;
    EXPECT_FALSE(weapon.Push({0.0f, -0.03f, 0.0f}, s, c, w, pose));
    EXPECT_NEAR(B::Length(pose.p - r.pose.p), 0.0f, 1e-6f);
    EXPECT_TRUE(weapon.Push({0.0f, 0.03f, 0.0f}, s, c, w, pose));
    EXPECT_NEAR(pose.p.y, r.pose.p.y + 0.03f, 1e-5f);
    r = weapon.Step({{0.0f, 0.0f, 0.0f}, flat}, Chest, s, c, w); // from where the push left it
    EXPECT_GE(r.pose.p.y, s.hand_r - 1e-4f);
}

// The gun slid off past the blade's tip, then a little under its level and away: no touch once
// the lines are no longer over each other (the side they met on kept only while they are).
TEST(SolidWeapon, TwoWeaponsThatSlidApartNoLongerTouch) {
    S::Shape blade = Blade();
    blade.line_r = 0.02f;
    S::Shape gun{};
    gun.line = true;
    gun.tip = {0.0f, -0.3f, 0.0f};
    gun.line_r = 0.02f;
    V side{};
    bool met = false;
    const float steps[][2] = {{0.95f, 1.03f}, {0.98f, 1.03f}, {1.06f, 0.999f}, {1.2f, 0.995f},
                              {1.4f, 0.99f}};
    for (const auto& st : steps) {
        S::Pose a{{0.0f, 1.0f, 0.0f}, Pointing({1.0f, 0.0f, 0.0f})};
        S::Pose b{{st[0], st[1], -0.15f}, Pointing({0.0f, 0.0f, 1.0f})};
        const S::Touch k = S::Separate(a, blade, b, gun, met ? &side : nullptr);
        met = k.any;
        side = k.normal;
        if (st[0] > 1.0f + blade.line_r + gun.line_r) {
            EXPECT_FALSE(k.any) << st[0];
        }
    }
}

// Random motion and swings (now and then a fast one) among two walls, a floor, a pole and a bar,
// 20000 updates: the weapon is never inside anything, and an update never takes many sweeps.
TEST(SolidWeapon, RandomMotionNeverLeavesItInside) {
    World w;
    w.planes.push_back({{0.7f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}});
    w.planes.push_back({{0.0f, 0.0f, -0.8f}, {0.0f, 0.0f, 1.0f}});
    w.planes.push_back({{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}});
    w.poles.push_back({{0.2f, 0.0f, -0.4f}, {0.0f, 1.0f, 0.0f}, 0.03f});
    w.poles.push_back({{0.0f, 1.3f, -0.3f}, {1.0f, 0.0f, 0.0f}, 0.02f});
    const V safe{-0.3f, 1.45f, 0.3f};
    S::Shape s = Blade();
    s.line_r = 0.02f;
    const S::Config c{};
    std::uint32_t seed = 12345;
    const auto r01 = [&] {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return static_cast<float>(seed >> 8) * (1.0f / 16777216.0f);
    };
    const auto rs = [&] { return r01() * 2.0f - 1.0f; };
    float worst = 0.0f;
    int most = 0;
    for (int run = 0; run < 50; ++run) {
        S::Weapon weapon;
        V p{-0.1f + 0.3f * rs(), 1.1f + 0.2f * rs(), 0.2f * rs()};
        V dir = B::Normalize(V{rs(), rs(), rs()}, {0.0f, -1.0f, 0.0f});
        V vel{};
        V spin{};
        for (int i = 0; i < 400; ++i) {
            const float k = r01() < 0.03f ? 12.0f : 2.0f;
            vel = vel * 0.9f + V{rs(), rs(), rs()} * (0.01f * k);
            spin = spin * 0.9f + V{rs(), rs(), rs()} * (0.03f * k);
            p = p + vel;
            p = {std::clamp(p.x, -0.6f, 1.2f), std::clamp(p.y, 0.2f, 1.8f),
                 std::clamp(p.z, -1.2f, 0.4f)};
            if (B::LengthSq(spin) > 1e-12f) {
                dir = B::Normalize(B::Rotate(B::AxisAngle(spin, B::Length(spin)), dir), dir);
            }
            w.casts = 0;
            const S::Result r = weapon.Step({p, Pointing(dir)}, safe, s, c, w);
            most = std::max(most, w.casts);
            ASSERT_FALSE(r.through);
            float d = w.Depth(r.pose.p, s.hand_r);
            for (int j = 0; j <= 20; ++j) {
                d = std::max(d, w.Depth(B::Lerp(S::At(r.pose, s.base), S::At(r.pose, s.tip),
                                                static_cast<float>(j) / 20.0f),
                                        s.line_r));
            }
            worst = std::max(worst, d);
        }
    }
    EXPECT_LE(worst, 0.002f); // never more than 2 mm in (float error, the cast's own tolerance)
    EXPECT_LE(most, c.max_casts + 80);
}

// A blade seen in the world on a hand: its line in the hand's own frame (the dual blades' left
// blade, measured from the game's dummies through the pose the hand held).
TEST(SolidWeapon, ALineSeenInTheWorldIsPutOnTheHand) {
    const S::Pose hand{{0.3f, 1.1f, -0.2f}, Pointing(B::Normalize(V{1.0f, 1.0f, 0.0f}))};
    const V base_local{0.0f, -0.12f, 0.01f};
    const V tip_local{0.02f, -0.7f, 0.0f};
    const auto at = [&](V local) { return S::At(hand, local); };
    S::Shape s{};
    ASSERT_TRUE(S::LineOnHand(hand, at(base_local), at(tip_local), 0.1f, 0.5f, s));
    EXPECT_TRUE(s.line);
    EXPECT_NEAR(B::Length(s.base - base_local), 0.0f, 1e-5f);
    EXPECT_NEAR(B::Length(s.tip - tip_local), 0.0f, 1e-5f);
    S::Shape no{};
    const V near_base = base_local + V{0.05f, 0.0f, 0.0f};
    EXPECT_FALSE(S::LineOnHand(hand, at(base_local), at(near_base), 0.1f, 0.5f, no)); // too short
    EXPECT_FALSE(S::LineOnHand(hand, at({0.0f, -0.9f, 0.0f}), at({0.0f, -1.5f, 0.0f}), 0.1f, 0.5f,
                               no)); // not on this hand
    EXPECT_FALSE(S::LineOnHand(hand, at(tip_local), at(base_local), 0.1f, 0.5f, no)); // reversed
    EXPECT_FALSE(no.line);
}

TEST(SolidWeapon, AttachedPartUsesItsOwnDummiesEvenWhenReversed) {
    S::Shape line{};
    ASSERT_TRUE(S::PartLine([](int suffix, V& p) {
        if (suffix != 100 && suffix != 110) {
            return false;
        }
        p = {0.0f, suffix == 100 ? -0.4f : -0.05f, 0.0f};
        return true;
    }, line));
    EXPECT_NEAR(line.base.y, -0.05f, 1e-6f);
    EXPECT_NEAR(line.tip.y, -0.4f, 1e-6f);
}

TEST(SolidWeapon, AttachedPartCanUseOtherDummiesOrASingleClawTip) {
    S::Shape line{};
    ASSERT_TRUE(S::PartLine([](int suffix, V& p) {
        if (suffix != 101 && suffix != 121) {
            return false;
        }
        p = {0.02f, suffix == 101 ? -0.04f : -0.18f, 0.0f};
        return true;
    }, line));
    EXPECT_NEAR(line.tip.y, -0.18f, 1e-6f);
    S::Shape claw{};
    ASSERT_TRUE(S::PartLine([](int suffix, V& p) {
        p = {0.0f, -0.08f, 0.0f};
        return suffix == 120;
    }, claw));
    EXPECT_FLOAT_EQ(B::Length(claw.base), 0.0f);
    EXPECT_NEAR(claw.tip.y, -0.08f, 1e-6f);
}

TEST(SolidWeapon, MissingOrRemotePartDoesNotCreateACollider) {
    S::Shape line{};
    EXPECT_FALSE(S::PartLine([](int, V&) { return false; }, line));
    EXPECT_FALSE(line.line);
    EXPECT_FALSE(S::PartLine([](int, V& p) {
        p = {0.0f, -4.0f, 0.0f};
        return true;
    }, line));
    EXPECT_FALSE(line.line);
}

TEST(SolidWeapon, DifferentPairedWeaponLengthsStayInContactWhileRubbing) {
    for (const float length : {0.08f, 0.18f, 0.4f, 0.55f}) {
        S::Shape line{};
        ASSERT_TRUE(S::PartLine([&](int suffix, V& p) {
            p = {0.0f, suffix == 100 ? 0.0f : -length, 0.0f};
            return suffix == 100 || suffix == 110;
        }, line));
        V side{};
        for (int frame = 0; frame < 90; ++frame) {
            S::Pose a{{0.03f, 0.01f * std::sin(frame * 0.1f), 0.0f}, {}};
            S::Pose b{};
            const auto touch = S::Separate(a, line, b, line, frame ? &side : nullptr);
            ASSERT_TRUE(touch.any);
            EXPECT_TRUE(B::IsFinite(touch.point));
            EXPECT_NEAR(a.p.x - b.p.x, 2.0f * line.line_r, 1e-6f);
            side = touch.normal;
        }
    }
}

struct PairedModelFixture {
    int variation;
    std::array<int, 4> attachments;
    int right_part, left_part;
    V right100, right110, left100, left110;
};

constexpr std::array<PairedModelFixture, 4> PairedModels{{
    {400, {-1, -1, 300, 340}, 2, 3,
     {0, -0.6143105f, 0}, {0, -0.2318783f, 0},
     {0, -0.6143105f, 0}, {0, -0.2318783f, 0}},
    {900, {-1, 304, 344, -1}, 1, 2,
     {0.0261498f, -0.4211060f, 0}, {0, -0.1669979f, 0},
     {-0.0524178f, -0.3852558f, 0}, {0, -0.1669979f, 0}},
    {2700, {-1, -1, 300, 340}, 2, 3,
     {0.0317622f, -0.9210604f, 0}, {0, -0.2698944f, 0},
     {0.0136882f, 0.4630299f, 0.0000001f}, {0.0043644f, 0.1987080f, 0}},
    {101, {-1, -1, 304, 344}, 2, 3,
     {-0.0032417f, 0.0015452f, 0.0025793f}, {-0.0143258f, 0.0015452f, 0.0025793f},
     {-0.0032417f, 0.0015452f, 0.0025793f}, {-0.0143258f, 0.0015452f, 0.0025793f}},
}};

int ModelPartLine(const PairedModelFixture& f, int hand, S::Shape& s) {
    return S::AttachedPartLine(hand, [&](int part) { return f.attachments[part]; },
                              [&](int id, V& p) {
        const int group = id / 1000;
        const int suffix = id % 1000;
        if (suffix != 100 && suffix != 110) {
            return false;
        }
        if (group == 10 + f.right_part) {
            p = suffix == 100 ? f.right100 : f.right110;
            return true;
        }
        if (group == 10 + f.left_part) {
            p = suffix == 100 ? f.left100 : f.left110;
            return true;
        }
        return false;
    }, s);
}

TEST(SolidWeapon, RetailPairedWeaponAttachmentsProduceBothColliders) {
    for (const auto& f : PairedModels) {
        SCOPED_TRACE(f.variation);
        for (int hand = 0; hand < 2; ++hand) {
            S::Shape part{};
            EXPECT_EQ(ModelPartLine(f, hand, part), 10 + (hand ? f.right_part : f.left_part));
            EXPECT_TRUE(part.line);
            EXPECT_GE(B::Length(part.tip - part.base), 0.005f);
        }
    }
}

TEST(SolidWeapon, KosParasiteWithoutAttackRowsStillCollidesAndSparks) {
    const auto& f = PairedModels.back();
    const auto* moves = VR::Melee::FindMoves(f.variation);
    ASSERT_NE(moves, nullptr);
    for (int row = 3; row < 6; ++row) {
        ASSERT_FALSE(moves->rows[row].Valid());
    }
    S::Shape left{}, part{}, right{};
    ASSERT_GE(ModelPartLine(f, 0, left), 0);
    ASSERT_GE(ModelPartLine(f, 1, part), 0);
    ASSERT_LT(B::Length(part.tip - part.base), 0.05f);
    S::BladeOrPartLine(false, 0.0f, 0.0f, 0.05f, part, right);
    ASSERT_TRUE(right.line);
    EXPECT_NEAR(B::Length(right.base - part.base), 0.0f, 1e-6f);
    EXPECT_NEAR(B::Length(right.tip - part.tip), 0.0f, 1e-6f);
    VR::WeaponWall::Contact contact;
    VR::Sparks::Field sparks;
    float carry = 0.0f;
    constexpr float dt = 1.0f / 90.0f;
    for (int frame = 0; frame < 90; ++frame) {
        S::Pose a{{0, 0.03f, 0}, {}}, b{};
        const auto touch = S::Separate(a, right, b, left);
        ASSERT_TRUE(touch.any);
        const V slide{0.2f, 0, 0};
        const auto out = contact.Step({dt, true, touch.normal, slide});
        ASSERT_TRUE(out.scraping);
        sparks.Scrape(carry, touch.point, touch.normal, slide, dt);
    }
    EXPECT_GT(sparks.Count(), 0u);
}

TEST(SolidWeapon, BladeReachTakesPriorityAndMissingGeometryClearsTheFallback) {
    S::Shape part{}, chosen{};
    ASSERT_GE(ModelPartLine(PairedModels.back(), 1, part), 0);
    chosen.hand_r = 0.07f;
    chosen.line_r = 0.03f;
    S::BladeOrPartLine(true, 0.1f, 0.4f, 0.05f, part, chosen);
    ASSERT_TRUE(chosen.line);
    EXPECT_FLOAT_EQ(chosen.base.y, -0.1f);
    EXPECT_FLOAT_EQ(chosen.tip.y, -0.4f);
    EXPECT_FLOAT_EQ(chosen.hand_r, 0.07f);
    EXPECT_FLOAT_EQ(chosen.line_r, 0.03f);
    S::BladeOrPartLine(true, 0.1f, 0.1f, 0.05f, part, chosen);
    EXPECT_NEAR(B::Length(chosen.tip - part.tip), 0.0f, 1e-6f);
    const auto missing = [](int, V&) { return false; };
    EXPECT_EQ(S::AttachedPartLine(1, [](int) { return -1; }, missing, part), -1);
    EXPECT_FALSE(part.line);
    S::BladeOrPartLine(false, 0.0f, 0.0f, 0.05f, part, chosen);
    EXPECT_FALSE(chosen.line);
}

TEST(SolidWeapon, AttachedPartLookupRejectsTheOtherHandAndHiddenParts) {
    S::Shape s{};
    int reads = 0;
    const auto point = [&](int, V& p) {
        ++reads;
        p = {0, -0.1f, 0};
        return true;
    };
    EXPECT_EQ(S::AttachedPartLine(1, [](int) { return 344; }, point, s), -1);
    EXPECT_EQ(S::AttachedPartLine(0, [](int) { return 304; }, point, s), -1);
    EXPECT_EQ(S::AttachedPartLine(0, [](int) { return -1; }, point, s), -1);
    EXPECT_EQ(reads, 0);
    EXPECT_FALSE(s.line);
}

} // namespace

// A crossing's handedness must come from the approach, not argument order or blade axes.
TEST(SolidWeapon, ApproachingBladeYieldsFromEitherSideAndEitherHand) {
    const auto shape = Blade();
    for (const float sign : {-1.0f, 1.0f}) {
        for (const bool swap : {false, true}) {
            S::Pose fixed{{0, 1, 0}, Pointing({1, 0, 0})};
            S::Pose moving{{.5f, 1, -.5f}, Pointing({0, 0, 1})};
            const auto old_fixed = fixed;
            auto old_moving = moving;
            old_moving.p.y += sign * .08f;
            auto a = swap ? moving : fixed, b = swap ? fixed : moving;
            auto old_a = swap ? old_moving : old_fixed, old_b = swap ? old_fixed : old_moving;
            const auto hit = S::Separate(a, shape, b, shape, nullptr, S::CrossM, &old_a, &old_b);
            ASSERT_TRUE(hit.any);
            const auto& held = swap ? b : a;
            const auto& yielded = swap ? a : b;
            EXPECT_NEAR(B::Length(held.p - fixed.p), 0, 1e-6f);
            EXPECT_NEAR(yielded.p.y - held.p.y, sign * 2 * shape.line_r, 1e-6f);
        }
    }
}

TEST(SolidWeapon, FirstContactThatAlreadyCrossedKeepsApproachSide) {
    const auto shape = Blade();
    S::Pose a{{0, 1, 0}, Pointing({1, 0, 0})};
    S::Pose b{{.5f, .99f, -.5f}, Pointing({0, 0, 1})};
    const auto old_a = a;
    auto old_b = b;
    old_b.p.y = 1.08f;
    const auto hit = S::Separate(a, shape, b, shape, nullptr, S::CrossM, &old_a, &old_b);
    ASSERT_TRUE(hit.any);
    EXPECT_FLOAT_EQ(a.p.y, 1);
    EXPECT_NEAR(b.p.y, 1 + 2 * shape.line_r, 1e-6f);
}

TEST(SolidWeapon, RubbingAndHoldingRetainTheYieldingHandWithoutDrift) {
    const auto shape = Blade();
    S::Pose old_a{{0, 1, 0}, Pointing({1, 0, 0})};
    S::Pose old_b{{.5f, 1.08f, -.5f}, Pointing({0, 0, 1})};
    V side{};
    float share = .5f;
    for (int frame = 0; frame < 180; ++frame) {
        S::Pose a = old_a;
        S::Pose b{{.5f + .1f * std::sin(frame * .1f), .99f, -.5f}, old_b.q};
        const auto target_a = a, target_b = b;
        const auto hit = S::Separate(a, shape, b, shape, frame ? &side : nullptr,
                                     S::CrossM, &old_a, &old_b, share);
        ASSERT_TRUE(hit.any);
        EXPECT_FLOAT_EQ(a.p.y, 1);
        EXPECT_NEAR(b.p.y, 1 + 2 * shape.line_r, 1e-6f);
        side = hit.normal;
        share = hit.a_share;
        old_a = target_a;
        old_b = target_b;
    }
}

TEST(SolidWeapon, BothHandsPressingShareByNormalMotion) {
    const auto shape = Blade();
    S::Pose a{{0, 1, 0}, Pointing({1, 0, 0})};
    S::Pose b{{.5f, 1.02f, -.5f}, Pointing({0, 0, 1})};
    auto old_a = a, old_b = b;
    old_a.p.y -= .01f;
    old_b.p.y += .03f;
    const auto hit = S::Separate(a, shape, b, shape, nullptr, S::CrossM, &old_a, &old_b);
    ASSERT_TRUE(hit.any);
    EXPECT_NEAR(hit.a_share, .25f, 1e-5f);
    EXPECT_NEAR(1 - a.p.y, hit.depth * .25f, 1e-6f);
    EXPECT_NEAR(b.p.y - 1.02f, hit.depth * .75f, 1e-6f);
}

TEST(SolidWeapon, RotationAtTheContactCountsEvenWithAStationaryGrip) {
    const auto shape = Blade();
    S::Pose a{{0, 1, 0}, Pointing({1, 0, 0})};
    S::Pose b{{.5f, 1, -.5f}, Pointing({0, 0, 1})};
    const auto old_a = a;
    auto old_b = b;
    old_b.q = Pointing({0, .2f, 1});
    const auto hit = S::Separate(a, shape, b, shape, nullptr, S::CrossM, &old_a, &old_b);
    ASSERT_TRUE(hit.any);
    EXPECT_FLOAT_EQ(hit.a_share, 0);
    EXPECT_FLOAT_EQ(a.p.y, 1);
    EXPECT_GT(b.p.y, 1.03f);
}

TEST(SolidWeapon, CommonTranslationAndTinyNoiseKeepTheContactSplit) {
    const auto shape = Blade();
    const V side{0, -1, 0};
    for (const float dy : {0.0f, -.1f, .1f}) {
        S::Pose a{{0, 1, 0}, Pointing({1, 0, 0})};
        S::Pose b{{.5f, 1.01f, -.5f}, Pointing({0, 0, 1})};
        auto old_a = a, old_b = b;
        old_a.p.y -= dy;
        old_b.p.y -= dy + .00001f;
        const auto hit = S::Separate(a, shape, b, shape, &side, S::CrossM,
                                     &old_a, &old_b, 0);
        ASSERT_TRUE(hit.any);
        EXPECT_FLOAT_EQ(hit.a_share, 0);
        EXPECT_FLOAT_EQ(a.p.y, 1);
    }
}

TEST(SolidWeapon, ApproachHistoryStillReleasesBeyondCrossingLimit) {
    const auto shape = Blade();
    S::Pose a{{0, 1, 0}, Pointing({1, 0, 0})};
    S::Pose b{{.5f, 1 - S::CrossM - .01f, -.5f}, Pointing({0, 0, 1})};
    auto old_a = a, old_b = b;
    old_b.p.y = 1.08f;
    EXPECT_FALSE(S::Separate(a, shape, b, shape, nullptr, S::CrossM, &old_a, &old_b).any);
}

// At exact contact, closest-point subtraction can leave a tiny sideways residual. It must
// not replace the remembered above/below side, or the next inward frame flips the weapons.
TEST(SolidWeapon, RubbingThroughCoincidentLinesKeepsApproachSide) {
    for (const float world_x : {0.0f, 10.0f, 100.0f}) {
        for (const float sign : {-1.0f, 1.0f}) {
            for (const bool swap : {false, true}) {
                for (int angle = 1; angle < 180; ++angle) {
                    SCOPED_TRACE(::testing::Message() << world_x << ", " << sign << ", "
                                                     << swap << ", " << angle);
                    const float radians = angle * B::Deg;
                    S::Shape cane{}, pistol{};
                    cane.line = pistol.line = true;
                    cane.base = {-0.5f, 0, 0};
                    cane.tip = {0.5f, 0, 0};
                    pistol.base = {-0.15f * std::cos(radians), 0,
                                   -0.15f * std::sin(radians)};
                    pistol.tip = -pistol.base;
                    S::Pose old_cane{{world_x, 1, 20}, {}};
                    S::Pose old_pistol{{world_x + 0.123f, 1 + sign * 0.08f, 20}, {}};
                    V side{};
                    float share = 0.5f;
                    bool met = false;
                    for (const float gap : {0.02f, 0.01f, 0.0f, -0.001f, -0.01f}) {
                        const S::Pose target_cane = old_cane;
                        const S::Pose target_pistol{{world_x + 0.123f, 1 + sign * gap, 20}, {}};
                        auto a = swap ? target_pistol : target_cane;
                        auto b = swap ? target_cane : target_pistol;
                        const auto old_a = swap ? old_pistol : old_cane;
                        const auto old_b = swap ? old_cane : old_pistol;
                        const auto hit = S::Separate(a, swap ? pistol : cane,
                                                     b, swap ? cane : pistol,
                                                     met ? &side : nullptr, S::CrossM,
                                                     &old_a, &old_b, share);
                        ASSERT_TRUE(hit.any);
                        const auto& held = swap ? b : a;
                        const auto& moving = swap ? a : b;
                        ASSERT_NEAR(B::Length(held.p - target_cane.p), 0, 1e-5f);
                        ASSERT_NEAR(sign * (moving.p.y - held.p.y),
                                    cane.line_r + pistol.line_r, 1e-5f);
                        side = hit.normal;
                        share = hit.a_share;
                        met = hit.any;
                        old_cane = target_cane;
                        old_pistol = target_pistol;
                    }
                }
            }
        }
    }
}

// A millimetre of wrist noise must not reassign an existing centimetres-deep correction
// to the other hand. Both controllers still move normally while the gun rubs the blade.
TEST(SolidWeapon, RubbingNoiseDoesNotTransferTheWholeCorrectionBetweenHands) {
    const auto shape = Blade();
    for (const float sign : {-1.0f, 1.0f}) {
        for (const bool swap : {false, true}) {
            S::Pose old_fixed{{0, 1, 0}, Pointing({1, 0, 0})};
            S::Pose old_moving{{.5f, 1 + sign * .08f, -.5f}, Pointing({0, 0, 1})};
            V side{};
            float share = .5f;
            for (int frame = 0; frame < 120; ++frame) {
                const float fixed_noise = (frame % 4 == 1 || frame % 4 == 2) ? .001f : 0;
                const float moving_noise = frame % 4 >= 2 ? -.001f : 0;
                S::Pose fixed{{0, 1 + sign * fixed_noise, 0}, old_fixed.q};
                S::Pose moving{{.5f + .05f * std::sin(frame * .1f),
                                1 + sign * (-.04f + moving_noise), -.5f}, old_moving.q};
                auto a = swap ? moving : fixed, b = swap ? fixed : moving;
                const auto old_a = swap ? old_moving : old_fixed;
                const auto old_b = swap ? old_fixed : old_moving;
                const auto hit = S::Separate(a, shape, b, shape, frame ? &side : nullptr,
                                             S::CrossM, &old_a, &old_b, share);
                ASSERT_TRUE(hit.any);
                const auto& held = swap ? b : a;
                const auto& yielded = swap ? a : b;
                ASSERT_NEAR(B::Length(held.p - fixed.p), 0, 1e-5f) << frame;
                ASSERT_NEAR(sign * (yielded.p.y - held.p.y), 2 * shape.line_r, 1e-5f);
                old_fixed = fixed;
                old_moving = moving;
                side = hit.normal;
                share = hit.a_share;
            }
        }
    }
}
