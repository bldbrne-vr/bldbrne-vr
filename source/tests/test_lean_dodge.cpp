// SPDX-License-Identifier: GPL-2.0-or-later

// Lean to dodge (core/vr/lean_dodge.h): synthetic head motion at 90 Hz, a still head, then a lean
// at constant speed, then still again.

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/lean_dodge.h"

namespace {

namespace L = VR::Lean;

constexpr double Dt = 1.0 / 90.0;

struct Lean {
    double start{0.5};        // s
    double duration{0.15};    // s
    float speed{1.6f};        // m/s
    float dx{1.0f}, dz{0.0f}; // direction (LOCAL, unit)
};

struct Result {
    std::vector<L::Output> fires;
    std::vector<const char*> rejections;
};

// Runs `leans` with the head facing `fwd` (LOCAL); `modify` adjusts each frame's input.
template <typename F>
Result Drive(const std::vector<Lean>& leans, float fwd_x, float fwd_z, double seconds, F&& modify,
             const L::Config& cfg = {}) {
    L::Detector detector;
    Result result;
    float x = 0.0f, z = 0.0f;
    for (double t = 0.0; t < seconds; t += Dt) {
        float vx = 0.0f, vz = 0.0f;
        for (const auto& lean : leans) {
            if (t >= lean.start && t < lean.start + lean.duration) {
                vx = lean.dx * lean.speed;
                vz = lean.dz * lean.speed;
            }
        }
        x += vx * static_cast<float>(Dt);
        z += vz * static_cast<float>(Dt);
        L::Input in{};
        in.t = t;
        in.enabled = true;
        in.head_ok = true;
        in.x = x;
        in.z = z;
        in.velocity_ok = true;
        in.vx = vx;
        in.vz = vz;
        in.fwd_x = fwd_x;
        in.fwd_z = fwd_z;
        modify(in);
        const L::Output out = detector.Update(in, cfg);
        if (out.fire) {
            result.fires.push_back(out);
        }
        if (out.rejected[0] != '\0') {
            result.rejections.push_back(out.rejected);
        }
    }
    return result;
}

Result Drive(const std::vector<Lean>& leans, float fwd_x = 0.0f, float fwd_z = -1.0f,
             double seconds = 1.5) {
    return Drive(leans, fwd_x, fwd_z, seconds, [](L::Input&) {});
}

TEST(LeanDodge, SidewaysAndBackLeansDodge) {
    const Result right = Drive({{0.5, 0.15, 1.6f, 1.0f, 0.0f}});
    ASSERT_EQ(right.fires.size(), 1u);
    EXPECT_NEAR(right.fires[0].stick_x, 1.0f, 1e-3f);
    EXPECT_NEAR(right.fires[0].stick_y, 0.0f, 1e-3f);
    EXPECT_NEAR(right.fires[0].angle_deg, 90.0f, 0.5f);
    EXPECT_GE(right.fires[0].dist, 0.15f);
    EXPECT_TRUE(right.rejections.empty());

    const Result back = Drive({{0.5, 0.15, 1.6f, 0.0f, 1.0f}}); // +z = back
    ASSERT_EQ(back.fires.size(), 1u);
    EXPECT_NEAR(back.fires[0].stick_y, -1.0f, 1e-3f);

    const Result back_left = Drive({{0.5, 0.15, 1.6f, -0.7071f, 0.7071f}});
    ASSERT_EQ(back_left.fires.size(), 1u);
    EXPECT_NEAR(back_left.fires[0].angle_deg, -135.0f, 0.5f);
}

TEST(LeanDodge, DirectionIsRelativeToTheView) {
    // Facing +x (turned right in the room): a lean toward -z is to the left.
    const Result left = Drive({{0.5, 0.15, 1.6f, 0.0f, -1.0f}}, 1.0f, 0.0f);
    ASSERT_EQ(left.fires.size(), 1u);
    EXPECT_NEAR(left.fires[0].stick_x, -1.0f, 1e-3f);
    EXPECT_NEAR(left.fires[0].stick_y, 0.0f, 1e-3f);
    // The forward vector need not be normalized or level.
    const Result right = Drive({{0.5, 0.15, 1.6f, 1.0f, 0.0f}}, 0.0f, -0.4f);
    ASSERT_EQ(right.fires.size(), 1u);
    EXPECT_NEAR(right.fires[0].stick_x, 1.0f, 1e-3f);
}

TEST(LeanDodge, ForwardSlowAndShortLeansDoNot) {
    const Result forward = Drive({{0.5, 0.15, 1.6f, 0.0f, -1.0f}});
    EXPECT_TRUE(forward.fires.empty());
    ASSERT_EQ(forward.rejections.size(), 1u);
    EXPECT_EQ(std::string(forward.rejections[0]), std::string("forward"));

    const Result slow = Drive({{0.5, 0.4, 1.0f, 1.0f, 0.0f}});
    EXPECT_TRUE(slow.fires.empty());
    EXPECT_TRUE(slow.rejections.empty()); // never fast enough to be a candidate

    const Result short_burst = Drive({{0.5, 0.06, 2.5f, 1.0f, 0.0f}});
    EXPECT_TRUE(short_burst.fires.empty());

    // Fast for long enough but not far enough within the window (a twitch).
    L::Config far{};
    far.dist_m = 0.40f;
    const Result twitch =
        Drive({{0.5, 0.15, 1.6f, 1.0f, 0.0f}}, 0.0f, -1.0f, 1.5, [](L::Input&) {}, far);
    EXPECT_TRUE(twitch.fires.empty());
    ASSERT_EQ(twitch.rejections.size(), 1u);
    EXPECT_EQ(std::string(twitch.rejections[0]), std::string("distance"));
}

TEST(LeanDodge, GatesRefuse) {
    const std::vector<Lean> lean{{0.5, 0.15, 1.6f, 1.0f, 0.0f}};
    const auto refused = [&lean](auto modify) {
        const Result r = Drive(lean, 0.0f, -1.0f, 1.5, modify);
        return r.fires.empty() && r.rejections.size() == 1 ? std::string(r.rejections[0])
                                                           : std::string("fired");
    };
    EXPECT_EQ(refused([](L::Input& in) { in.look_x = 0.5f; }), "stick");
    EXPECT_EQ(refused([](L::Input& in) { in.swing = true; }), "swing");
    EXPECT_EQ(refused([](L::Input& in) { in.circle_held = true; }), "circle");
    // The head turning at 120 deg/s while it moves.
    EXPECT_EQ(refused([](L::Input& in) {
                  const double yaw = in.t * 2.094;
                  in.fwd_x = static_cast<float>(-std::sin(yaw));
                  in.fwd_z = static_cast<float>(-std::cos(yaw));
              }),
              "yaw");
    // Disabled (seated, classic, the screen): nothing at all.
    const Result off = Drive(lean, 0.0f, -1.0f, 1.5, [](L::Input& in) { in.enabled = false; });
    EXPECT_TRUE(off.fires.empty());
    EXPECT_TRUE(off.rejections.empty());
}

TEST(LeanDodge, CooldownAndOneDodgePerLean) {
    // One long lean: one dodge.
    const Result long_lean = Drive({{0.5, 0.6, 1.6f, 1.0f, 0.0f}});
    EXPECT_EQ(long_lean.fires.size(), 1u);
    // Two leans 0.4 s apart: the second is inside the 0.6 s cooldown.
    const Result close = Drive({{0.5, 0.15, 1.6f, 1.0f, 0.0f}, {0.9, 0.15, 1.6f, -1.0f, 0.0f}});
    EXPECT_EQ(close.fires.size(), 1u);
    ASSERT_EQ(close.rejections.size(), 1u);
    EXPECT_EQ(std::string(close.rejections[0]), std::string("cooldown"));
    // 0.8 s apart: both.
    const Result apart =
        Drive({{0.5, 0.15, 1.6f, 1.0f, 0.0f}, {1.3, 0.15, 1.6f, -1.0f, 0.0f}}, 0.0f, -1.0f, 2.0);
    ASSERT_EQ(apart.fires.size(), 2u);
    EXPECT_NEAR(apart.fires[1].stick_x, -1.0f, 1e-3f);
}

TEST(LeanDodge, WithoutRuntimeVelocity) {
    // The runtime reports no velocity: the speed comes from the positions.
    const Result r = Drive({{0.5, 0.15, 1.6f, 1.0f, 0.0f}}, 0.0f, -1.0f, 1.5,
                           [](L::Input& in) { in.velocity_ok = false; });
    ASSERT_EQ(r.fires.size(), 1u);
    EXPECT_NEAR(r.fires[0].stick_x, 1.0f, 1e-3f);
}

} // namespace
