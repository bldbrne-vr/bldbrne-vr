// SPDX-License-Identifier: GPL-2.0-or-later

// Render pitch bias (vr.ini render_pitch_bias_deg, core/vr/render_tilt.h): the vertical FOV is the
// old formula bit for bit without the bias and covers the eyes as the pitched render sees them with
// it (the keystone at the top corners included); the render pitch after the pitch limit and its
// clamp; the gate; the late warp's room at rest against a direct rotation; and the whole chain: the
// render camera the hook writes (WriteOrientation's rows from the view yaw and the render pitch)
// is exactly the camera record's orientation (QuatFromYawPitchRoll of the record's head yaw /
// pitch / roll, through the game's axes), and the late warp (vr_late_warp_math.h MakeReprojection
// and LineFor) puts every world point on the eye pixel it has without the bias: nothing you see
// moves, at rest or with the head moved since the capture.

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

#include <gtest/gtest.h>

#include "core/vr/render_tilt.h"
#include "video_core/renderer_vulkan/vr_late_warp_math.h"

namespace {

namespace VL = Vulkan::VrLate;
namespace T = VR::Tilt;

constexpr float Deg = T::Deg;

struct Fov {
    float angle_left, angle_right, angle_up, angle_down;
};

// The Quest 3 on Link (the user's log): left eye L-54 R40 U44 D-55, the right one mirrored.
std::array<Fov, 2> Quest3() {
    return {Fov{-54.0f * Deg, 40.0f * Deg, 44.0f * Deg, -55.0f * Deg},
            Fov{-40.0f * Deg, 54.0f * Deg, 44.0f * Deg, -55.0f * Deg}};
}

// vr_session.cpp ComputeVfov's cover() before the bias (1c3f3249).
float LegacyCover(const std::array<Fov, 2>& eyes, float aspect, float extra) {
    float half_v = 0.0f;
    float half_h = 0.0f;
    for (const auto& fov : eyes) {
        half_v = std::max({half_v, fov.angle_up, -fov.angle_down});
        half_h = std::max({half_h, fov.angle_right, -fov.angle_left});
    }
    constexpr float Pi = 3.14159265358979323846f;
    constexpr float DegToRad = Pi / 180.0f;
    const float from_h =
        std::atan(std::tan(std::min(half_h + extra, 1.5f)) / std::max(aspect, 0.1f));
    return std::clamp(std::max(half_v + extra, from_h), 20.0f * DegToRad, 70.0f * DegToRad);
}

VL::Quat AxisAngle(float x, float y, float z, float angle) {
    const float s = std::sin(0.5f * angle);
    return {x * s, y * s, z * s, std::cos(0.5f * angle)};
}

VL::Quat Mul(const VL::Quat& a, const VL::Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// VR::QuatFromYawPitchRoll: yaw about +Y (+ = left), then pitch about +X (+ = up), then roll about
// the view axis (+Z).
VL::Quat YawPitchRoll(float yaw, float pitch, float roll) {
    return Mul(Mul(AxisAngle(0, 1, 0, yaw), AxisAngle(1, 0, 0, pitch)), AxisAngle(0, 0, 1, roll));
}

// bloodborne_camera.cpp WriteOrientation: the game camera's rows (right, up, forward) for view yaw
// y (+ = right), pitch p (+ = down) and roll r (+ = the right side up).
std::array<VL::Vec3, 3> GameRows(float y, float p, float r) {
    const float sy = std::sin(y), cy = std::cos(y), sp = std::sin(p), cp = std::cos(p);
    VL::Vec3 right{cy, 0.0f, -sy};
    VL::Vec3 up{sy * sp, cp, cy * sp};
    const VL::Vec3 forward{sy * cp, -sp, cy * cp};
    if (r != 0.0f) {
        const float sr = std::sin(r), cr = std::cos(r);
        const VL::Vec3 r2 = right * cr + up * sr;
        const VL::Vec3 u2 = up * cr - right * sr;
        right = r2;
        up = u2;
    }
    return {right, up, forward};
}

struct Px {
    double x, y;
};

// The game's projection of a game-world direction g for a camera with `rows` (pixel y down).
Px GameProject(const VL::Intrinsics& in, const std::array<VL::Vec3, 3>& rows, const VL::Vec3& g) {
    const double x = VL::Dot(rows[0], g), y = VL::Dot(rows[1], g), z = VL::Dot(rows[2], g);
    return {in.cx + in.fx * x / z, in.cy - in.fy * y / z};
}

// A tracking-space direction d in the camera q's image (camera frame: x right, y up, -z forward).
Px TrackingProject(const VL::Intrinsics& in, const VL::Quat& q, const VL::Vec3& d) {
    const VL::Vec3 c = VL::Transpose(VL::FromQuat(q)) * d;
    return {in.cx + in.fx * c.x / -c.z, in.cy - in.fy * c.y / -c.z};
}

TEST(RenderTilt, NoBiasIsTheOldFovBitForBit) {
    std::mt19937 rng{11};
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (int i = 0; i < 20000; ++i) {
        std::array<Fov, 2> eyes{};
        for (auto& e : eyes) {
            e = {-(20.0f + 50.0f * u(rng)) * Deg, (20.0f + 50.0f * u(rng)) * Deg,
                 (20.0f + 50.0f * u(rng)) * Deg, -(20.0f + 50.0f * u(rng)) * Deg};
        }
        const float aspect = 0.5f + 2.0f * u(rng);
        const float margin = 12.0f * u(rng) * Deg;
        const float a = T::CoverHalfVfov(eyes, aspect, 0.0f, margin);
        const float b = LegacyCover(eyes, aspect, margin);
        EXPECT_EQ(a, b);
    }
    // The user's today: 122 deg at margin 6 on 4:3.
    EXPECT_NEAR(2.0f * T::CoverHalfVfov(Quest3(), 4.0f / 3.0f, 0.0f, 6.0f * Deg) / Deg, 122.0f,
                1e-3f);
}

TEST(RenderTilt, BiasedFovCoversThePitchedEyes) {
    const auto eyes = Quest3();
    // 5 deg with margin 8: the higher of 44 + 5 and 55 - 5, plus 8 = 58 (116 deg).
    const float half = T::CoverHalfVfov(eyes, 2720.0f / 1808.0f, 5.0f * Deg, 8.0f * Deg);
    EXPECT_NEAR(half / Deg, 58.0f, 1e-3f);
    // 5.5 deg (the geometric centre (55 - 44) / 2) with margin 6: 55.5 (R4's first sizing).
    EXPECT_NEAR(T::CoverHalfVfov(eyes, 2848.0f / 1728.0f, 5.5f * Deg, 6.0f * Deg) / Deg, 55.5f,
                1e-3f);
    // Never less than the eyes themselves, whatever the bias (the warp's crop uses the render's
    // intrinsics unpitched): a 15 deg bias with no margin still covers 55 down.
    EXPECT_GE(T::CoverHalfVfov(eyes, 1.5f, 15.0f * Deg, 0.0f) / Deg, 59.0f - 1e-3f);
    EXPECT_GE(T::CoverHalfVfov(eyes, 1.5f, 15.0f * Deg, -20.0f * Deg) / Deg, 55.0f - 1e-3f);
    // A narrow aspect: the outer edges at the keystoned top row decide (66.7 deg here).
    const float narrow = T::CoverHalfVfov(eyes, 0.9f, 5.0f * Deg, 8.0f * Deg);
    const float wide_tan = T::KeystoneTanX(std::tan(54.0f * Deg), std::tan(44.0f * Deg),
                                           -std::tan(55.0f * Deg), 5.0f * Deg);
    EXPECT_NEAR(narrow, std::atan(std::tan(std::atan(wide_tan) + 8.0f * Deg) / 0.9f), 1e-5f);
    EXPECT_GT(narrow / Deg, 58.5f);
}

TEST(RenderTilt, KeystoneAndRestRoomsMatchARotation) {
    // ToRender against rotating the direction by the render's pitch (head frame -> render frame).
    std::mt19937 rng{5};
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (int i = 0; i < 5000; ++i) {
        const float bias = 12.0f * Deg * u(rng);
        const float x = 1.5f * u(rng), y = 1.4f * u(rng);
        // The render's orientation in the head's frame: pitched down (about +X) by the bias.
        const VL::Quat render = AxisAngle(1, 0, 0, -bias);
        const VL::Vec3 d = VL::Transpose(VL::FromQuat(render)) * VL::Vec3{x, y, -1.0f};
        const auto t = T::ToRender(x, y, bias);
        EXPECT_NEAR(t[0], d.x / -d.z, 1e-4f);
        EXPECT_NEAR(t[1], d.y / -d.z, 1e-4f);
    }
    // The rest rooms of the review presets (Quest 3).
    const auto eyes = Quest3();
    {
        // Today: 2560x1920, 122 deg, no bias: 13.4 at the sides, 17 up, 6 down.
        const auto r = T::RestRooms(eyes, 122.0f * Deg, 4.0f / 3.0f, 0.0f);
        EXPECT_NEAR(r.edge[0], 13.43f, 0.02f);
        EXPECT_NEAR(r.edge[1], 13.43f, 0.02f);
        EXPECT_NEAR(r.edge[2], 17.0f, 1e-3f);
        EXPECT_NEAR(r.edge[3], 6.0f, 1e-3f);
        EXPECT_NEAR(r.side_mid[0], r.edge[0], 1e-3f);
    }
    {
        // 5 deg, margin 8 (116 deg) on 2720x1808: 9 up, 8 down; the sides 13.3 at the eyes' middle
        // row, 10.9 at the top-outer corner (the keystone), 16.5 at the bottom-outer one.
        const float vfov = 2.0f * T::CoverHalfVfov(eyes, 2720.0f / 1808.0f, 5.0f * Deg, 8.0f * Deg);
        const auto r = T::RestRooms(eyes, vfov, 2720.0f / 1808.0f, 5.0f * Deg);
        EXPECT_NEAR(r.edge[2], 9.0f, 1e-3f);
        EXPECT_NEAR(r.edge[3], 8.0f, 1e-3f);
        EXPECT_NEAR(r.side_mid[0], 13.33f, 0.05f);
        EXPECT_NEAR(r.edge[0], 10.95f, 0.05f);
        EXPECT_NEAR(r.edge[0], r.edge[1], 1e-3f);
    }
}

// The defaults since the TILT 2560 review (render_pitch_bias_deg 5, render_pitch_min_margin_deg
// 8): with your vr.ini's fov_margin_deg=6 every preset gets exactly the reviewed room (9 up / 8
// down), and without the bias nothing changes.
TEST(RenderTilt, DefaultBiasRaisesTheMarginToTheReviewedRoom) {
    const float bias = 5.0f * Deg;
    EXPECT_EQ(T::MarginDeg(6.0f, 8.0f, bias), 8.0f);  // yours: raised while the bias applies
    EXPECT_EQ(T::MarginDeg(6.0f, 8.0f, 0.0f), 6.0f);  // no bias (menu panel, AER): as before
    EXPECT_EQ(T::MarginDeg(10.0f, 8.0f, bias), 10.0f); // a larger margin is kept
    EXPECT_EQ(T::MarginDeg(3.0f, 8.0f, -bias), 8.0f);  // any bias
    EXPECT_EQ(T::MarginDeg(6.0f, 0.0f, bias), 6.0f);   // min 0: fov_margin_deg alone
    const auto eyes = Quest3();
    // The review: 2720x1808 with fov_margin_deg=8 and the bias, against the default rule on 6.
    const float reviewed = 2.0f * T::CoverHalfVfov(eyes, 2720.0f / 1808.0f, bias, 8.0f * Deg);
    const float by_rule = 2.0f * T::CoverHalfVfov(eyes, 2720.0f / 1808.0f, bias,
                                                  T::MarginDeg(6.0f, 8.0f, bias) * Deg);
    EXPECT_EQ(reviewed, by_rule);
    EXPECT_NEAR(by_rule / Deg, 116.0f, 1e-3f);
    // The tilt presets (2720x1808, 3200x2128) and 16:9 (2560x1440): 9 up and 8 down at rest, the
    // sides never below the review's 11.0 at the top-outer corners.
    for (const float aspect : {2720.0f / 1808.0f, 3200.0f / 2128.0f, 16.0f / 9.0f}) {
        const float vfov = 2.0f * T::CoverHalfVfov(eyes, aspect, bias,
                                                   T::MarginDeg(6.0f, 8.0f, bias) * Deg);
        const auto r = T::RestRooms(eyes, vfov, aspect, bias);
        EXPECT_NEAR(r.edge[2], 9.0f, 1e-3f) << aspect;
        EXPECT_NEAR(r.edge[3], 8.0f, 1e-3f) << aspect;
        EXPECT_GT(r.edge[0], 10.9f) << aspect;
        EXPECT_NEAR(r.edge[0], r.edge[1], 1e-3f) << aspect;
    }
    {
        // A 4:3 preset (2560x1920, 3200x2400) with the bias: the same 9 / 8, but the sides drop
        // to 10.8 at the eyes' middle and 8.4 at the corners (13.4 without the bias): why the
        // headset launchers moved to the tilt presets. Still at least the margin (8).
        const float vfov = 2.0f * T::CoverHalfVfov(eyes, 4.0f / 3.0f, bias,
                                                   T::MarginDeg(6.0f, 8.0f, bias) * Deg);
        const auto r = T::RestRooms(eyes, vfov, 4.0f / 3.0f, bias);
        EXPECT_NEAR(r.edge[2], 9.0f, 1e-3f);
        EXPECT_NEAR(r.edge[3], 8.0f, 1e-3f);
        EXPECT_NEAR(r.side_mid[0], 10.8f, 0.06f);
        EXPECT_NEAR(r.edge[0], 8.42f, 0.03f);
        EXPECT_GE(r.edge[0], 8.0f);
    }
    // Without the bias (the menu panel's frozen frames, AER): margin 6 as before, 17 up / 6 down.
    const float off = 2.0f * T::CoverHalfVfov(eyes, 4.0f / 3.0f, 0.0f,
                                              T::MarginDeg(6.0f, 8.0f, 0.0f) * Deg);
    EXPECT_NEAR(off / Deg, 122.0f, 1e-3f);
}

TEST(RenderTilt, RenderPitchInAndOut) {
    // The render looks lower; the head's own pitch comes back by adding the bias (the record's
    // pitch_bias is head - render, exactly, after the clamp).
    for (float head = -80.0f; head <= 80.0f; head += 0.5f) {
        for (const float bias : {0.0f, 2.0f, 5.0f, -4.0f}) {
            const float h = head * Deg, b = bias * Deg;
            const float render = T::RenderPitch(h, b);
            const float recorded_bias = h - render;
            EXPECT_NEAR(render + recorded_bias, h, 1e-7f);
            if (std::abs(head - bias) <= 87.0f) {
                EXPECT_NEAR(render, h - b, 1e-6f);
            }
            EXPECT_LE(std::abs(render), 87.0f * Deg + 1e-6f);
        }
    }
    EXPECT_EQ(T::RenderPitch(0.3f, 0.0f), 0.3f);
    // At the pitch limit (80 down) a 5 deg bias looks 85 down; 85 + 5 is clamped to 87.
    EXPECT_NEAR(T::RenderPitch(-80.0f * Deg, 5.0f * Deg) / Deg, -85.0f, 1e-4f);
    EXPECT_NEAR(T::RenderPitch(-85.0f * Deg, 5.0f * Deg) / Deg, -87.0f, 1e-4f);
}

TEST(RenderTilt, Gate) {
    EXPECT_TRUE(T::BiasWanted(5.0f, true, false, true));
    EXPECT_FALSE(T::BiasWanted(0.0f, true, false, true));  // off
    EXPECT_FALSE(T::BiasWanted(5.0f, false, false, true)); // AER / mono: nothing undoes it
    EXPECT_FALSE(T::BiasWanted(5.0f, true, true, true));   // the menu panel's frozen frame
    EXPECT_FALSE(T::BiasWanted(5.0f, true, false, false)); // no late warp showing the eyes
}

// The camera hook's render camera is the record's orientation, and the late warp puts every world
// point back where your head sees it: the same eye pixel as without the bias.
TEST(RenderTilt, LateWarpUndoesTheBiasExactly) {
    std::mt19937 rng{3};
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const auto eyes = Quest3();
    const float aspect = 2720.0f / 1808.0f;
    double worst_record = 0.0, worst_warp = 0.0, worst_eye = 0.0, least_shift = 1e9;
    int checked = 0;
    for (int i = 0; i < 4000; ++i) {
        const float base_yaw = 3.0f * u(rng);
        const float head_yaw = 3.0f * u(rng);
        const float head_pitch = 80.0f * Deg * u(rng); // after the pitch limit
        const float roll = 0.35f * u(rng);              // camera_roll
        // The head at display time: moved a little since the capture (or not at all).
        const VL::Quat q_head = YawPitchRoll(head_yaw, head_pitch, roll);
        const VL::Quat q_v = (i % 2 == 0)
                                 ? q_head
                                 : Mul(q_head, YawPitchRoll(0.12f * u(rng), 0.12f * u(rng),
                                                            0.05f * u(rng)));
        // A world direction near what the eye sees (tracking space).
        const VL::Vec3 dv_local{0.9f * u(rng), 0.8f * u(rng), -1.0f};
        const VL::Vec3 d = VL::FromQuat(q_v) * dv_local;
        std::array<Px, 2> eye_px{};
        for (int k = 0; k < 2; ++k) {
            const float bias = k == 0 ? 0.0f : 5.0f * Deg;
            // OnCameraTick: the vertical FOV for this bias (both with margin 8 here, so the eye
            // pixels compare one for one), the render pitch and the record.
            const float vfov = 2.0f * T::CoverHalfVfov(eyes, aspect, 5.0f * Deg, 8.0f * Deg);
            const auto in = VL::MakeIntrinsics(vfov, aspect, 2720.0f, 1808.0f);
            const float render_pitch = T::RenderPitch(head_pitch, bias);
            const float record_bias = head_pitch - render_pitch;
            // The hook writes the render camera (game axes): view yaw base - head, pitch + = down.
            const auto rows = GameRows(VL::WrapPi(base_yaw - head_yaw), -render_pitch, roll);
            const Px game = GameProject(in, rows, VL::GameFromTracking(base_yaw, d));
            // The record's orientation (what the late warp starts from) sees d at the same pixel.
            const VL::Quat q_s = YawPitchRoll(head_yaw, render_pitch, roll);
            const Px rec = TrackingProject(in, q_s, d);
            worst_record = std::max({worst_record, std::abs(game.x - rec.x), std::abs(game.y - rec.y)});
            // The head's own pitch comes back from the record.
            EXPECT_NEAR(render_pitch + record_bias, head_pitch, 1e-6f);
            // The eye pixel (V's image: the render's intrinsics, at your head): where d is.
            const Px pv = TrackingProject(in, q_v, d);
            eye_px[k] = pv;
            // The late warp at that eye pixel reads the render where the game drew d.
            const auto rp = VL::MakeReprojection(q_s, {}, q_v, {}, 1.0f);
            const VL::Line line = VL::LineFor(rp, in, static_cast<float>(pv.x),
                                              static_cast<float>(pv.y));
            ASSERT_TRUE(line.valid);
            worst_warp = std::max({worst_warp, std::abs(line.q0x - game.x),
                                   std::abs(line.q0y - game.y)});
            if (k == 1) {
                // The bias did move the render: d sits elsewhere in it than without the bias.
                const VL::Quat q_s0 = YawPitchRoll(head_yaw, head_pitch, roll);
                const Px unbiased = TrackingProject(in, q_s0, d);
                least_shift = std::min(least_shift, std::hypot(unbiased.x - rec.x,
                                                               unbiased.y - rec.y));
            }
        }
        // The same eye pixel with and without the bias.
        worst_eye = std::max({worst_eye, std::abs(eye_px[0].x - eye_px[1].x),
                              std::abs(eye_px[0].y - eye_px[1].y)});
        ++checked;
    }
    EXPECT_EQ(checked, 4000);
    EXPECT_LT(worst_record, 0.02);
    EXPECT_LT(worst_warp, 0.02);
    EXPECT_LT(worst_eye, 1e-9);
    EXPECT_GT(least_shift, 20.0); // 5 deg is ~80 render px: never a no-op
}

// Where your head looks (no motion since the capture), the render's centre row is 5 deg below the
// eye's: the eye's forward ray sits f tan(5 deg) above the render's centre, and the late warp maps
// the eye's centre there.
TEST(RenderTilt, EyeForwardSitsAboveTheRenderCentre) {
    const float aspect = 2720.0f / 1808.0f;
    const float vfov = 2.0f * T::CoverHalfVfov(Quest3(), aspect, 5.0f * Deg, 8.0f * Deg);
    const auto in = VL::MakeIntrinsics(vfov, aspect, 2720.0f, 1808.0f);
    const VL::Quat q_head = YawPitchRoll(0.4f, -0.3f, 0.0f);
    const VL::Quat q_s = YawPitchRoll(0.4f, T::RenderPitch(-0.3f, 5.0f * Deg), 0.0f);
    const auto rp = VL::MakeReprojection(q_s, {}, q_head, {}, 1.0f);
    const VL::Line line = VL::LineFor(rp, in, in.cx, in.cy);
    EXPECT_NEAR(line.q0x, in.cx, 1e-3f);
    EXPECT_NEAR(in.cy - line.q0y, in.fy * std::tan(5.0f * Deg), 0.05f);
}

} // namespace
