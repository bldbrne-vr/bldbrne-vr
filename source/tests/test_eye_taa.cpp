// SPDX-License-Identifier: GPL-2.0-or-later

// vr.ini eye_taa (renderer_vulkan/vr_eye_taa_math.h): where an eye pixel of this refresh was in the
// previous refresh's eye image. Checked against plain ray casting: a surface point seen by this eye
// projects, through the previous eye's own pinhole, onto the position the history is sampled at.

#include <array>
#include <cmath>
#include <random>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vr_eye_taa_math.h"
#include "video_core/renderer_vulkan/vr_late_warp_math.h"

namespace {

namespace T = Vulkan::VrEyeTaa;

struct Pose {
    std::array<float, 4> q; // x y z w
    T::Vec3 p;
};

T::Mat3 Rot(const Pose& pose) {
    return T::Rotation(pose.q[0], pose.q[1], pose.q[2], pose.q[3]);
}

std::array<float, 4> Normalized(std::array<float, 4> q) {
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (float& v : q) {
        v /= n;
    }
    return q;
}

// A pinhole pixel's world point at depth z (metres along the view axis).
T::Vec3 WorldPoint(const Pose& pose, const T::Intrinsics& in, float px, float py, float z) {
    const T::Mat3 r = Rot(pose);
    const T::Vec3 d{(px + 0.5f - in.cx) / in.fx * z, -(py + 0.5f - in.cy) / in.fy * z, -z};
    T::Vec3 w{};
    for (int i = 0; i < 3; ++i) {
        w[i] = r[i][0] * d[0] + r[i][1] * d[1] + r[i][2] * d[2] + pose.p[i];
    }
    return w;
}

// Where the world point lands in a pinhole image (texel coordinates, centres at integers).
bool Project(const Pose& pose, const T::Intrinsics& in, const T::Vec3& w, float& qx, float& qy) {
    const T::Mat3 r = Rot(pose);
    const T::Vec3 d{w[0] - pose.p[0], w[1] - pose.p[1], w[2] - pose.p[2]};
    T::Vec3 c{};
    for (int i = 0; i < 3; ++i) {
        c[i] = r[0][i] * d[0] + r[1][i] * d[1] + r[2][i] * d[2]; // R^T d
    }
    if (c[2] >= 0.0f) {
        return false;
    }
    qx = in.cx + in.fx * (c[0] / -c[2]) - 0.5f;
    qy = in.cy - in.fy * (c[1] / -c[2]) - 0.5f;
    return true;
}

// The left eye of a 2560x1920 render with fov_margin_deg 6: fx = fy = 530 px, its crop 1178x1274.
constexpr T::Intrinsics Eye{530.0f, 530.0f, 1280.0f - 541.0f, 960.0f - 299.0f};

TEST(EyeTaa, StillHeadMapsEveryPixelOntoItself) {
    const Pose head{{0.03f, -0.6f, 0.02f, 0.8f}, {0.1f, 1.6f, -0.2f}};
    const Pose pose{Normalized(head.q), head.p};
    const T::Rigid r = T::Between(Rot(pose), pose.p, Rot(pose), pose.p);
    for (float z : {0.0f, 0.4f, 3.0f, 40.0f}) {
        float qx = 0, qy = 0;
        ASSERT_TRUE(T::Previous(r, Eye, Eye, 100.0f, 900.0f, z, qx, qy));
        EXPECT_NEAR(qx, 100.0f, 1e-3f);
        EXPECT_NEAR(qy, 900.0f, 1e-3f);
    }
}

TEST(EyeTaa, MatchesRayCastingUnderHeadMotion) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    int checked = 0;
    float worst = 0.0f;
    for (int n = 0; n < 5000; ++n) {
        const Pose cur{Normalized({0.2f * u(rng), u(rng), 0.1f * u(rng), 1.0f}),
                       {0.3f * u(rng), 1.6f + 0.1f * u(rng), 0.3f * u(rng)}};
        // One refresh earlier: up to ~1 degree of turn and a few mm (a fast head at 120 Hz).
        const Pose prev{
            Normalized({cur.q[0] + 0.004f * u(rng), cur.q[1] + 0.008f * u(rng),
                        cur.q[2] + 0.004f * u(rng), cur.q[3]}),
            {cur.p[0] + 0.004f * u(rng), cur.p[1] + 0.004f * u(rng), cur.p[2] + 0.004f * u(rng)}};
        const float px = 589.0f + 580.0f * u(rng);
        const float py = 637.0f + 630.0f * u(rng);
        const float z = 0.3f + 30.0f * (0.5f + 0.5f * u(rng));
        const T::Rigid r = T::Between(Rot(prev), prev.p, Rot(cur), cur.p);
        float qx = 0, qy = 0, ex = 0, ey = 0;
        const bool a = T::Previous(r, Eye, Eye, px, py, z, qx, qy);
        const bool b = Project(prev, Eye, WorldPoint(cur, Eye, px, py, z), ex, ey);
        ASSERT_EQ(a, b);
        if (a) {
            worst = std::max(worst, std::hypot(qx - ex, qy - ey));
            ++checked;
        }
    }
    EXPECT_GT(checked, 4900);
    EXPECT_LT(worst, 0.02f); // px
}

TEST(EyeTaa, AtInfinityOnlyTheRotationCounts) {
    // Far away the head's translation moves nothing: z = 0 (sky) equals a very far surface.
    const Pose cur{Normalized({0.1f, 0.3f, 0.0f, 0.95f}), {0.0f, 1.6f, 0.0f}};
    const Pose prev{Normalized({0.1f, 0.31f, 0.0f, 0.95f}), {0.005f, 1.6f, 0.003f}};
    const T::Rigid r = T::Between(Rot(prev), prev.p, Rot(cur), cur.p);
    float ax = 0, ay = 0, bx = 0, by = 0;
    ASSERT_TRUE(T::Previous(r, Eye, Eye, 400.0f, 500.0f, 0.0f, ax, ay));
    ASSERT_TRUE(T::Previous(r, Eye, Eye, 400.0f, 500.0f, 1.0e6f, bx, by));
    EXPECT_NEAR(ax, bx, 1e-2f);
    EXPECT_NEAR(ay, by, 1e-2f);
    // ... and a near one moves by the parallax: the head moved 5 mm sideways and 3 mm along z, a
    // few px at 0.5 m (530 px per unit of tangent).
    float nx = 0, ny = 0;
    ASSERT_TRUE(T::Previous(r, Eye, Eye, 400.0f, 500.0f, 0.5f, nx, ny));
    EXPECT_GT(std::hypot(nx - ax, ny - ay), 2.0f);
}

TEST(EyeTaa, ACropOffsetIsAPrincipalPointShift) {
    // The right eye's crop starts 30 px further right: the same direction is 30 px further left in
    // it.
    const Pose pose{Normalized({0.0f, 0.2f, 0.0f, 1.0f}), {0.0f, 1.6f, 0.0f}};
    const T::Rigid r = T::Between(Rot(pose), pose.p, Rot(pose), pose.p);
    const T::Intrinsics shifted{Eye.fx, Eye.fy, Eye.cx - 30.0f, Eye.cy};
    float qx = 0, qy = 0;
    ASSERT_TRUE(T::Previous(r, Eye, shifted, 600.0f, 600.0f, 2.0f, qx, qy));
    EXPECT_NEAR(qx, 570.0f, 1e-3f);
    EXPECT_NEAR(qy, 600.0f, 1e-3f);
}

// Regression: pass 2 used to feed the capture's 1/z directly to TAA, although TAA
// unprojects along the NEW eye's ray. Leaning 5 cm toward a surface changes that depth.
TEST(EyeTaa, HistoryUsesTheReprojectedViewDepth) {
    namespace L = Vulkan::VrLate;
    const auto rp = L::MakeReprojection({}, {}, {}, {0, 0, -0.05f}, 1.0f);
    const auto in = L::MakeIntrinsics(2.0f, 1.0f, 1024, 1024);
    const auto line = L::LineFor(rp, in, in.cx, in.cy);
    for (float z : {0.5f, 3.0f, 30.0f}) {
        const float source_w = 1.0f / z;
        // The late reprojection now carries this value through disp_tex.g to StoreEyeW.
        const float history_w = L::ViewInverseDepth(line, rp.c.z, source_w);
        EXPECT_NEAR(history_w, 1.0f / (z - 0.05f), 1e-6f);
    }
}

TEST(EyeTaa, WalkingRequiresGameMotionInTheHistoryTransform) {
    // One source pixel of a static distant railing. Head tracking is unchanged, but
    // walking moved the camera 2 cm right between refreshes.
    const Pose prev{{0, 0, 0, 1}, {0, 0, 0}};
    const Pose cur{{0, 0, 0, 1}, {0.02f, 0, 0}};
    const auto point = WorldPoint(cur, Eye, 700, 500, 20);
    float expected_x = 0, expected_y = 0, actual_x = 0, actual_y = 0;
    ASSERT_TRUE(Project(prev, Eye, point, expected_x, expected_y));
    namespace L = Vulkan::VrLate;
    const auto rp = L::MakeReprojection({}, {}, {}, {}, 1);
    const auto pv = T::WorldEye({}, {}, 0, rp, 1, 0);
    const auto cv = T::WorldEye({}, {0.02f, 0, 0}, 0, rp, 1, 0);
    const auto rigid = T::Between(pv, cv);
    ASSERT_TRUE(T::Previous(rigid, Eye, Eye, 700, 500, 20, actual_x, actual_y));
    EXPECT_NEAR(actual_x, expected_x, 1e-4f);
    EXPECT_NEAR(actual_y, expected_y, 1e-4f);
}

TEST(EyeTaa, WorldViewMatchesWalkingTurningAndHeadMotionInBothEyes) {
    namespace L = Vulkan::VrLate;
    std::mt19937 rng(26);
    std::uniform_real_distribution<float> u(-1, 1);
    for (int n = 0; n < 1000; ++n) {
        const float scale = 1 + 0.4f * u(rng);
        const float base = 3 * u(rng), shown_base = base + 0.1f * u(rng);
        const L::Vec3 body{20 * u(rng), 2, 20 * u(rng)};
        const L::Vec3 shown_body = body + L::Vec3{0.1f * u(rng), 0.01f, 0.1f * u(rng)};
        const auto qs = L::YawQuat(u(rng));
        const auto qv = qs * L::Quat{0.04f, 0, 0, std::sqrt(1 - 0.04f * 0.04f)};
        const L::Vec3 cs{0.02f, 0.1f, -0.1f}, cv{0.025f, 0.09f, -0.12f};
        const auto render = body + L::GameFromTracking(base, cs) * scale;
        auto moved_q = qs;
        auto moved_c = cs;
        L::MoveCamera(L::MotionBetween(base, body, shown_base, shown_body, 1, scale), moved_q,
                      moved_c);
        const auto rp = L::MakeReprojection(moved_q, moved_c, qv, cv, scale);
        for (float eye : {-0.032f, 0.032f}) {
            const auto view = T::WorldEye(qs, render, base, rp, scale, eye);
            const auto rv = L::FromQuat(qv);
            const auto right = L::GameFromTracking(shown_base, rv * L::Vec3{1, 0, 0});
            const auto expected =
                shown_body * (1 / scale) + L::GameFromTracking(shown_base, cv) + right * eye;
            EXPECT_NEAR(view.position[0], expected.x, 1e-4f);
            EXPECT_NEAR(view.position[1], expected.y, 1e-4f);
            EXPECT_NEAR(view.position[2], expected.z, 1e-4f);
            for (int col = 0; col < 3; ++col) {
                const auto axis =
                    L::GameFromTracking(shown_base, {rv.m[0][col], rv.m[1][col], rv.m[2][col]});
                EXPECT_NEAR(view.axes[0][col], axis.x, 1e-5f);
                EXPECT_NEAR(view.axes[1][col], axis.y, 1e-5f);
                EXPECT_NEAR(view.axes[2][col], axis.z, 1e-5f);
            }
        }
    }
}

// A fixture frame: valid, still, 60 ms into the session.
T::Frame StillFrame() {
    T::Frame f{};
    f.valid = true;
    f.turn = 1;
    f.space = 2;
    f.capture_ns = 1'000'000'000;
    f.display_ns = 1'060'000'000;
    f.units_per_metre = 1;
    return f;
}

TEST(EyeTaa, WalkingAndSmoothTurningKeepHistoryButDiscontinuitiesDoNot) {
    const T::Frame prev = StillFrame();
    auto cur = prev;
    cur.display_ns += 8'333'333;
    EXPECT_TRUE(T::CanAccumulate(prev, cur)); // same game frame, another XR refresh
    cur.capture_ns += 16'666'667;
    cur.body.x += 0.05f;
    cur.base_yaw += 0.03f;
    EXPECT_TRUE(T::CanAccumulate(prev, cur));
    const auto continuous = cur;
    cur.turn++;
    EXPECT_FALSE(T::CanAccumulate(prev, cur));
    cur = continuous;
    cur.space++;
    EXPECT_FALSE(T::CanAccumulate(prev, cur));
    cur = continuous;
    cur.body.x = 5;
    EXPECT_FALSE(T::CanAccumulate(prev, cur));
    cur = continuous;
    cur.display_ns += 100'000'000;
    EXPECT_FALSE(T::CanAccumulate(prev, cur));
    cur = continuous;
    cur.capture_ns = prev.capture_ns - 1;
    EXPECT_FALSE(T::CanAccumulate(prev, cur));
    cur = continuous;
    cur.valid = false; // game-owned arms/cinematic/unknown camera
    EXPECT_FALSE(T::CanAccumulate(prev, cur));
    cur = continuous;
    cur.units_per_metre = 1.1f;
    EXPECT_FALSE(T::CanAccumulate(prev, cur));
}

// eye_taa_follow=0 is the baseline filter exactly: head-moved history only while the world
// stands still (no late motion now or then, the same turn, recenter and heading, the body point
// within 1 mm), whether or not the game world can place the eyes.
TEST(EyeTaa, FollowOffIsTodaysRule) {
    const T::Frame prev = StillFrame();
    auto cur = prev;
    cur.display_ns += 8'333'333;
    for (const bool valid : {true, false}) {
        cur.valid = valid;
        auto c = T::Choose(false, true, prev, cur);
        EXPECT_EQ(c.history, T::History::Head);
        EXPECT_FALSE(c.moving);
        auto walked = cur;
        walked.body.z += 2e-3f;
        EXPECT_EQ(T::Choose(false, true, prev, walked).history, T::History::None);
        auto small = cur;
        small.body.z += 0.9e-3f; // within today's 1 mm
        EXPECT_EQ(T::Choose(false, true, prev, small).history, T::History::Head);
        auto turned = cur;
        turned.base_yaw += 3e-4f;
        EXPECT_EQ(T::Choose(false, true, prev, turned).history, T::History::None);
        auto moved = cur;
        moved.motion = true;
        EXPECT_EQ(T::Choose(false, true, prev, moved).history, T::History::None);
        auto snapped = cur;
        snapped.turn++;
        EXPECT_EQ(T::Choose(false, true, prev, snapped).history, T::History::None);
        // Today's rule has no gap guard.
        auto late = cur;
        late.display_ns += 500'000'000;
        EXPECT_EQ(T::Choose(false, true, prev, late).history, T::History::Head);
        EXPECT_EQ(T::Choose(false, false, prev, cur).history, T::History::None);
    }
}

TEST(EyeTaa, FollowKeepsHistoryWhileWalkingAndRunsTheMovingPathOnlyThen) {
    const T::Frame prev = StillFrame();
    auto cur = prev;
    cur.display_ns += 8'333'333;
    // Standing: the game-world eyes, today's filter (the corrected transform only).
    auto c = T::Choose(true, true, prev, cur);
    EXPECT_EQ(c.history, T::History::World);
    EXPECT_FALSE(c.moving);
    // Walking, smooth turning, late motion: kept, the depth-checked path.
    for (int k = 0; k < 3; ++k) {
        auto m = cur;
        m.capture_ns += 16'666'667;
        if (k == 0) {
            m.body.x += 0.07f;
        } else if (k == 1) {
            m.base_yaw += 0.02f;
        } else {
            m.motion = true;
        }
        c = T::Choose(true, true, prev, m);
        EXPECT_EQ(c.history, T::History::World) << k;
        EXPECT_TRUE(c.moving) << k;
    }
    // ... unless discontinuous: a snap turn, a teleport.
    auto snap = cur;
    snap.turn++;
    snap.base_yaw += 0.5f;
    EXPECT_EQ(T::Choose(true, true, prev, snap).history, T::History::None);
    auto teleport = cur;
    teleport.body.x += 3.0f;
    EXPECT_EQ(T::Choose(true, true, prev, teleport).history, T::History::None);
    EXPECT_EQ(T::Choose(true, false, prev, cur).history, T::History::None);
}

// Third person, a menu, an arm the game animates (item use, an untracked controller): the game
// world cannot place the eyes, so today's head-moved history while the world stands still, not
// none; never across a display gap (a cinematic that stopped the warps: stale).
TEST(EyeTaa, FollowFallsBackToTodaysRuleWhenTheFrameCannotBePlaced) {
    T::Frame prev = StillFrame();
    auto cur = prev;
    cur.display_ns += 8'333'333;
    for (const int invalid : {0, 1, 2}) {
        auto p = prev;
        auto c = cur;
        p.valid = invalid != 1;
        c.valid = invalid != 0;
        if (invalid == 2) {
            c.valid = p.valid = false;
        }
        auto choice = T::Choose(true, true, p, c);
        EXPECT_EQ(choice.history, T::History::Head) << invalid;
        EXPECT_FALSE(choice.moving);
        auto walked = c;
        walked.body.x += 0.05f;
        EXPECT_EQ(T::Choose(true, true, p, walked).history, T::History::None);
        auto gap = c;
        gap.display_ns += 100'000'000;
        EXPECT_EQ(T::Choose(true, true, p, gap).history, T::History::None);
    }
    // A game hitch (capture gap > 70 ms) while standing: kept, head-moved.
    auto hitch = cur;
    hitch.capture_ns += 90'000'000;
    EXPECT_EQ(T::Choose(true, true, prev, hitch).history, T::History::Head);
}

TEST(EyeTaa, EyeWClassesRoundTrip) {
    for (const float w : {0.0f, 1e-3f, 0.025f, 0.5f, 3.0f, 40.0f}) {
        const auto plain = T::DecodeEyeW(w);
        EXPECT_EQ(plain.w, w);
        EXPECT_FALSE(plain.clamp_only);
        const auto clamp = T::DecodeEyeW(T::EncodeClampOnly(w));
        EXPECT_NEAR(clamp.w, w, 4e-7f * (w + 2.0f));
        EXPECT_TRUE(clamp.clamp_only);
    }
    EXPECT_LT(T::DecodeEyeW(-1.0f).w, 0.0f); // the HUD, no surface
    EXPECT_FALSE(T::DecodeEyeW(-1.0f).clamp_only);
}

// Pure rotation moves every depth alike: no parallax, so the moving path's depth test never fires.
TEST(EyeTaa, ParallaxIsZeroUnderRotationAndGrowsWithTranslation) {
    const T::Vec3 none{0, 0, 0};
    EXPECT_EQ(T::ParallaxPx(none, 532, 0.3f, -0.2f), 0.0f);
    // Strafing 37 mm per refresh (4.5 m/s at 120 Hz): 532 * 0.037 px per 1/m.
    EXPECT_NEAR(T::ParallaxPx({0.037f, 0, 0}, 532, 0, 0), 19.684f, 1e-3f);
    // Walking forward: none at the centre, growing with the angle off it.
    EXPECT_NEAR(T::ParallaxPx({0, 0, -0.037f}, 532, 0, 0), 0.0f, 1e-6f);
    EXPECT_NEAR(T::ParallaxPx({0, 0, -0.037f}, 532, 0.5f, 0), 532 * 0.5f * 0.037f, 1e-3f);
}

TEST(EyeTaa, OnlyClearDisocclusionsAndTheBodyAreDropped) {
    const float k = 19.7f; // px per 1/m (strafing at 4.5 m/s)
    const float gate = 0.05f;
    for (const float z : {0.6f, 2.0f, 20.0f, 100.0f}) {
        const float e = 1 / z;
        EXPECT_TRUE(T::TapUsable(e, e, k, gate));
        EXPECT_TRUE(T::TapUsable(e * 0.5f, e, k, gate)); // farther: colour clamp, as today
        EXPECT_TRUE(T::TapUsable(0, e, k, gate));        // sky behind: as today
        EXPECT_TRUE(T::TapUsable(T::HistUnknown, e, k, gate)); // a fill's
        EXPECT_FALSE(T::TapUsable(T::HistNone, e, k, gate));   // body, HUD
        // Nearer, by less than the gate's parallax: the same surface (a slope) or too close to
        // matter.
        EXPECT_TRUE(T::TapUsable(e + 0.9f * gate / k, e, k, gate));
        // Nearer by more (and beyond half-float rounding): it hid this surface a refresh ago.
        EXPECT_FALSE(T::TapUsable(e + std::max(1.1f * gate / k, 5e-3f * e), e, k, gate));
        EXPECT_FALSE(T::TapUsable(2 * e, e, k, gate));
        // Pure rotation: nothing is uncovered.
        EXPECT_TRUE(T::TapUsable(2 * e, e, 0, gate));
    }
    EXPECT_FALSE(T::TapUsable(0.1f, 0, k, gate)); // something in front of the sky a refresh ago
    EXPECT_TRUE(T::TapUsable(0, 0, k, gate));
}

float Half(float x) { // round to the nearest half float (normals), as the RGBA16F history stores
    if (x == 0.0f) {
        return 0.0f;
    }
    int e = 0;
    const float m = std::frexp(x, &e);
    return std::ldexp(std::nearbyint(std::ldexp(m, 11)), e - 11);
}

TEST(EyeTaa, HalfFloatHistoryDepthIsNeverADisocclusion) {
    for (const float k : {0.0f, 5.0f, 19.7f, 60.0f}) {
        for (float z = 0.31f; z < 2000.0f; z *= 1.07f) {
            const float e = 1 / z;
            EXPECT_TRUE(T::TapUsable(Half(e), e, k, 0.05f)) << z << " " << k;
        }
    }
}

// A floor 1.58 m below the eye (fx 532): 1/z is affine in screen y on a plane, 1/(fx h) per row.
// Walking or strafing at 4.5 m/s, the 2x2 footprint's nearer taps stay under the gate at EVERY
// distance, so the floor keeps the baseline sharp sample (the branch's 2% test lost it beyond
// ~8 m and flagged the floor as an edge beyond ~17 m).
TEST(EyeTaa, ASlopedFloorKeepsTheSharpHistoryAtEveryDistance) {
    const float fx = 532, h = 1.58f, step = 1 / (fx * h);
    for (const T::Vec3 t : {T::Vec3{0.037f, 0, 0}, T::Vec3{0, 0, -0.037f},
                            T::Vec3{0.026f, 0, -0.026f}}) {
        for (float z = 1.0f; z < 400.0f; z *= 1.1f) {
            const float e = 1 / z;
            const float uy = -h / z; // the floor point's tangent below the horizon
            const float k = T::ParallaxPx(t, fx, 0.4f, uy);
            for (const float fy : {0.0f, 0.3f, 0.5f, 0.99f}) {
                // Taps on the rows above (farther) and below (nearer) the sample point.
                const std::array<float, 4> stored{
                    Half(std::max(e - fy * step, 0.0f)), Half(std::max(e - fy * step, 0.0f)),
                    Half(e + (1 - fy) * step), Half(e + (1 - fy) * step)};
                std::array<float, 4> weights{};
                bool all = false;
                ASSERT_TRUE(T::MovingWeights(stored, e, k, 0.05f, 0.4f, fy, weights, all));
                EXPECT_TRUE(all) << z << " " << fy << " k " << k;
            }
        }
    }
}

TEST(EyeTaa, MovingWeightsDropTheOccluderAndKeepTheRest) {
    std::array<float, 4> weights{};
    bool all = true;
    // A rail 20 m away (0.05) that hid the wall 40 m away (0.025) at the left taps.
    ASSERT_TRUE(T::MovingWeights({0.05f, 0.025f, 0.05f, 0.025f}, 0.025f, 7.0f, 0.05f, 0.5f, 0.3f,
                                 weights, all));
    EXPECT_FALSE(all);
    EXPECT_EQ(weights[0], 0.0f);
    EXPECT_NEAR(weights[1], 0.7f, 1e-6f);
    EXPECT_EQ(weights[2], 0.0f);
    EXPECT_NEAR(weights[3], 0.3f, 1e-6f);
    // On the rail itself the wall's taps stay (farther: colour clamp, as today).
    ASSERT_TRUE(T::MovingWeights({0.05f, 0.025f, 0.05f, 0.025f}, 0.05f, 7.0f, 0.05f, 0.5f, 0.3f,
                                 weights, all));
    EXPECT_TRUE(all);
    // Under a quarter of the footprint left: none.
    EXPECT_FALSE(T::MovingWeights({0.05f, 0.05f, 0.05f, 0.025f}, 0.025f, 7.0f, 0.05f, 0.5f, 0.4f,
                                  weights, all));
    EXPECT_FALSE(T::MovingWeights({T::HistNone, T::HistNone, T::HistNone, T::HistNone}, 0.05f,
                                  0.0f, 0.05f, 0.5f, 0.3f, weights, all));
    // A fill (k = 0: its depth is a guess) drops only the body's taps.
    ASSERT_TRUE(T::MovingWeights({0.05f, T::HistNone, 0.5f, 0.025f}, 0.025f, 0.0f, 0.05f, 0.5f,
                                 0.5f, weights, all));
    EXPECT_FALSE(all);
    EXPECT_NEAR(weights[0] + weights[2] + weights[3], 1.0f, 1e-6f);
    EXPECT_EQ(weights[1], 0.0f);
}

TEST(EyeTaa, TheEdgeCapNeedsParallax) {
    EXPECT_FALSE(T::ParallaxEdge(0.025f, 0.05f, 0.0f, 0.05f)); // rotation: never
    EXPECT_FALSE(T::ParallaxEdge(0.025f, 0.05f, 1.9f, 0.05f)); // 0.0475 px
    EXPECT_TRUE(T::ParallaxEdge(0.025f, 0.05f, 2.1f, 0.05f));
    EXPECT_FALSE(T::ParallaxEdge(0.5f, 0.5f, 100.0f, 0.05f)); // one surface
}

TEST(EyeTaa, WorldOriginDoesNotQuantizeHeadMotion) {
    namespace L = Vulkan::VrLate;
    for (float origin : {0.0f, 10000.0f, 1000000.0f}) {
        const auto a = L::MakeReprojection({}, {}, {}, {}, 1);
        const auto b = L::MakeReprojection({}, {}, {}, {0.004f, 0, 0}, 1);
        const auto prev = T::WorldEye({}, {origin, 0, 0}, 0, a, 1, -0.032f);
        const auto cur = T::WorldEye({}, {origin, 0, 0}, 0, b, 1, -0.032f);
        const auto r = T::Between(prev, cur);
        EXPECT_NEAR(r.t[0], 0.004f, 1e-7f);
        EXPECT_NEAR(r.t[1], 0, 1e-7f);
        EXPECT_NEAR(r.t[2], 0, 1e-7f);
    }
}

} // namespace
