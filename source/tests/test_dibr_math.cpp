// SPDX-License-Identifier: GPL-2.0-or-later

// DIBR stereo (stereo_mode 2) math (renderer_vulkan/vr_stereo_warp_math.h): the depth-to-disparity
// formula against the game's own projection (random points seen by the centre camera and by two
// eye cameras half the eye distance to each side), the near-field knee, the HUD panel shift, the
// eye offset submitted with the poses, the held vial's depth proxy, and the mono / invalid-camera
// cases.

#include <algorithm>
#include <cmath>
#include <random>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vr_stereo_warp_math.h"

namespace {

namespace VS = Vulkan::VrStereo;

constexpr double Pi = 3.14159265358979323846;

// The game's projection (D3D style, left-handed, depth 0..1 at near..far) of a render W wide.
struct Projector {
    VS::CameraParams cam;
    double width;
    double p00;

    // Pixel column of view-space point (x, z) for a camera translated `cam_x` along its right axis.
    double Column(double x, double z, double cam_x) const {
        return 0.5 * width * (1.0 + p00 * (x - cam_x) / z);
    }
    // What a D32 depth buffer stores for view depth z.
    float StoredDepth(double z) const {
        return static_cast<float>(VS::StoredDepthFromViewDepth(z, cam));
    }
};

Projector MakeProjector(int width, float vfov_deg, float aspect, float half_ipd,
                        float depth_min = 0.0f, float depth_max = 1.0f) {
    Projector p{};
    p.cam.near_z = 0.2f;
    p.cam.far_z = 3000.0f;
    p.cam.vfov = static_cast<float>(vfov_deg * Pi / 180.0);
    p.cam.aspect = aspect;
    p.cam.half_ipd = half_ipd;
    p.cam.units_per_metre = 1.0f;
    p.cam.depth_min = depth_min;
    p.cam.depth_max = depth_max;
    p.width = width;
    p.p00 = 1.0 / (std::tan(0.5 * p.cam.vfov) * aspect);
    return p;
}

// Random points inside the frustum: each one's stored depth through DepthToDisparity must move it
// exactly where the left (+D) and right (-D) eye cameras see it.
void CheckFormula(const Projector& proj, double min_z, double max_error_px) {
    const auto params = VS::MakeDisparityParams(proj.cam, static_cast<float>(proj.width), 0.0f,
                                                0.0f); // no near-field compression
    ASSERT_GT(params.k_px, 0.0f);
    std::mt19937 rng{1234};
    std::uniform_real_distribution<double> u01{0.0, 1.0};
    double worst = 0.0;
    int checked = 0;
    for (int i = 0; i < 400'000; ++i) {
        // Log-uniform depth from just past the near plane to the far plane.
        const double z =
            std::exp(std::log(min_z) + u01(rng) * (std::log(2999.0) - std::log(min_z)));
        const double half_width = z / proj.p00;
        const double x = (u01(rng) * 2.0 - 1.0) * half_width;
        const double centre = proj.Column(x, z, 0.0);
        const double left = proj.Column(x, z, -proj.cam.half_ipd);
        const double right = proj.Column(x, z, proj.cam.half_ipd);
        const double truth = left - centre;
        EXPECT_NEAR(centre - right, truth, 1e-9); // symmetric eyes
        if (truth >= VS::kMaxDisparityPx) {
            continue; // deliberately capped (nearer than the warp's reach)
        }
        const float d = VS::DepthToDisparity(proj.StoredDepth(z), params);
        worst = std::max(worst, std::abs(static_cast<double>(d) - truth));
        ++checked;
    }
    EXPECT_GT(checked, 300'000);
    EXPECT_LE(worst, max_error_px);
}

TEST(DibrMath, FormulaMatchesProjectionHeadset) {
    // Quest 3 render: 2560x1920, 116 deg, 4:3, half of a 67 mm IPD.
    CheckFormula(MakeProjector(2560, 116.0f, 4.0f / 3.0f, 0.0335f), 0.21, 0.02);
}

TEST(DibrMath, FormulaMatchesProjectionWide) {
    CheckFormula(MakeProjector(1920, 70.0f, 16.0f / 9.0f, 0.032f), 0.21, 0.02);
}

TEST(DibrMath, FormulaMatchesProjectionDepthRange) {
    // A scene pass with viewport depth [0.25, 0.75].
    CheckFormula(MakeProjector(2560, 116.0f, 4.0f / 3.0f, 0.0335f, 0.25f, 0.75f), 0.21, 0.02);
}

TEST(DibrMath, SkyAndFarPlaneAreZero) {
    const auto proj = MakeProjector(2560, 116.0f, 4.0f / 3.0f, 0.0335f);
    const auto params = VS::MakeDisparityParams(proj.cam, 2560.0f, 0.6f, 0.3f);
    EXPECT_EQ(VS::DepthToDisparity(1.0f, params), 0.0f);
    EXPECT_EQ(VS::DepthToDisparity(std::nanf(""), params), 0.0f);
    // Distant content snaps to exactly 0 (bit-exact copy) below 1/64 px.
    EXPECT_EQ(VS::DepthToDisparity(proj.StoredDepth(2000.0), params), 0.0f);
}

TEST(DibrMath, NearKneeIsMonotonicContinuousAndBounded) {
    const auto proj = MakeProjector(2560, 116.0f, 4.0f / 3.0f, 0.0335f);
    const auto params = VS::MakeDisparityParams(proj.cam, 2560.0f, 0.6f, 0.3f);
    ASSERT_GT(params.floor_px, params.knee_px);
    EXPECT_NEAR(params.knee_px, params.k_px / 0.6f, 1e-4f);
    EXPECT_NEAR(params.floor_px, params.k_px / 0.3f, 1e-4f);
    // Identity up to the knee: everything beyond 0.6 m keeps its exact disparity.
    for (float D = 0.0f; D <= params.knee_px; D += 0.25f) {
        EXPECT_EQ(VS::CompressNear(D, params), D);
    }
    // Monotonic above it (occlusion order kept) and never past the floor.
    float previous = VS::CompressNear(params.knee_px, params);
    for (float D = params.knee_px + 0.01f; D < 2000.0f; D += 0.01f) {
        const float c = VS::CompressNear(D, params);
        EXPECT_GE(c, previous);
        EXPECT_LE(c, params.floor_px);
        previous = c;
    }
    // C1 at the knee: the slope just above it is 1.
    const float h = 1e-2f;
    const float slope =
        (VS::CompressNear(params.knee_px + h, params) - VS::CompressNear(params.knee_px, params)) /
        h;
    EXPECT_NEAR(slope, 1.0f, 2e-3f);
    // Off when the floor is not nearer than the knee.
    const auto off = VS::MakeDisparityParams(proj.cam, 2560.0f, 0.6f, 0.0f);
    EXPECT_EQ(off.knee_px, 0.0f);
    EXPECT_EQ(VS::CompressNear(150.0f, off), 150.0f);
}

TEST(DibrMath, ReachIsTheNearPlaneDisparity) {
    const auto proj = MakeProjector(2560, 116.0f, 4.0f / 3.0f, 0.0335f);
    const auto knee = VS::MakeDisparityParams(proj.cam, 2560.0f, 0.6f, 0.3f);
    EXPECT_NEAR(VS::ReachPx(knee), VS::CompressNear(knee.k_px / proj.cam.near_z, knee), 1e-4f);
    EXPECT_LE(VS::ReachPx(knee), knee.floor_px);
    const auto raw = VS::MakeDisparityParams(proj.cam, 2560.0f, 0.0f, 0.0f);
    EXPECT_NEAR(VS::ReachPx(raw), std::min(raw.k_px / proj.cam.near_z, VS::kMaxDisparityPx), 1e-4f);
}

TEST(DibrMath, HudShift) {
    // K = 20.1 px*m today: a 2 m panel is +-10 px (left eye right, right eye left).
    const auto proj = MakeProjector(2560, 116.0f, 4.0f / 3.0f, 0.0335f);
    const auto params = VS::MakeDisparityParams(proj.cam, 2560.0f, 0.6f, 0.3f);
    EXPECT_NEAR(params.k_px, 20.096f, 0.01f);
    EXPECT_EQ(VS::HudShiftPx(params.k_px, 2.0f, 0), 10);
    EXPECT_EQ(VS::HudShiftPx(params.k_px, 2.0f, 1), -10);
    EXPECT_EQ(VS::HudShiftPx(params.k_px, 0.0f, 0), 0);
    EXPECT_EQ(VS::HudShiftPx(0.0f, 2.0f, 0), 0);
}

TEST(DibrMath, EyeOffsetMatchesTheBaseline) {
    // half_ipd = EffectiveIpd / 2 * world_scale: the submitted eye poses sit half the IPD out,
    // in metres, whatever the world scale.
    for (const float scale : {0.5f, 1.0f, 2.0f}) {
        VS::CameraParams cam{};
        cam.vfov = 2.0f;
        cam.aspect = 4.0f / 3.0f;
        cam.units_per_metre = scale;
        cam.half_ipd = 0.5f * 0.067f * scale;
        EXPECT_NEAR(VS::EyeOffsetMetres(cam), 0.0335f, 1e-7f);
    }
}

TEST(DibrMath, MonoControlAndInvalidCameras) {
    // Zero baseline: every pixel stays put (the mono control run, dibr_baseline_scale=0).
    auto proj = MakeProjector(2560, 116.0f, 4.0f / 3.0f, 0.0f);
    const auto mono = VS::MakeDisparityParams(proj.cam, 2560.0f, 0.6f, 0.3f);
    EXPECT_EQ(mono.k_px, 0.0f);
    for (const double z : {0.21, 0.5, 3.0, 100.0}) {
        EXPECT_EQ(VS::DepthToDisparity(proj.StoredDepth(z), mono), 0.0f);
    }
    // Invalid cameras give K = 0 (both eyes the centre image), never NaN.
    VS::CameraParams bad = MakeProjector(2560, 116.0f, 4.0f / 3.0f, 0.0335f).cam;
    bad.vfov = 0.0f;
    EXPECT_EQ(VS::MakeDisparityParams(bad, 2560.0f, 0.6f, 0.3f).k_px, 0.0f);
    bad = proj.cam;
    bad.far_z = bad.near_z;
    EXPECT_FALSE(VS::IsCameraValid(bad));
    bad = proj.cam;
    bad.depth_max = bad.depth_min;
    EXPECT_FALSE(VS::IsCameraValid(bad));
    bad = proj.cam;
    bad.half_ipd = std::nanf("");
    EXPECT_FALSE(VS::IsCameraValid(bad));
    EXPECT_EQ(VS::MakeDisparityParams(proj.cam, 0.0f, 0.6f, 0.3f).k_px, 0.0f);
}

// The held vial's depth proxy (MakeProxyParams / ProxyDisparity, mirrored by the prep shader):
// every point on the capsule's axis is stamped with exactly the disparity a depth-writing surface
// at its view depth would get; points off the axis by less than the projected radius are stamped,
// points further out are not; the vial in front of the near plane is clipped onto it.
TEST(DibrMath, DepthProxy) {
    const auto proj = MakeProjector(2560, 116.0f, 4.0f / 3.0f, 0.0225f);
    const float width = 2560.0f;
    const float height = 1920.0f;
    const auto params = VS::MakeDisparityParams(proj.cam, width, 0.6f, 0.3f);
    VS::DepthProxy proxy{.valid = true,
                         .a = {-0.20f, -0.35f, 0.45f},
                         .b = {-0.18f, -0.24f, 0.56f},
                         .radius = 0.026f};
    const auto q = VS::MakeProxyParams(proxy, proj.cam, width, height);
    ASSERT_GT(q.on, 0.0f);
    const double fx = 0.5 * width * proj.p00;
    const double fy = 0.5 * height / std::tan(0.5 * proj.cam.vfov);
    for (int i = 0; i <= 16; ++i) {
        const double t = i / 16.0;
        const double x = proxy.a[0] + (proxy.b[0] - proxy.a[0]) * t;
        const double y = proxy.a[1] + (proxy.b[1] - proxy.a[1]) * t;
        const double z = proxy.a[2] + (proxy.b[2] - proxy.a[2]) * t;
        const float px = static_cast<float>(0.5 * width + fx * x / z);
        const float py = static_cast<float>(0.5 * height - fy * y / z);
        // The same point as a depth-writing surface.
        const float expected = VS::DepthToDisparity(proj.StoredDepth(z), params);
        EXPECT_NEAR(VS::ProxyDisparity(px, py, q, params), expected, 1e-3f * expected) << t;
        // Sideways off the projected axis: inside the radius (minus a pixel), then outside.
        const float r_px = static_cast<float>(proxy.radius * fx / z);
        const float abx = q.bx - q.ax;
        const float aby = q.by - q.ay;
        const float len = std::sqrt(abx * abx + aby * aby);
        const float nx = -aby / len;
        const float ny = abx / len;
        if (i > 0 && i < 16) {
            EXPECT_GT(
                VS::ProxyDisparity(px + nx * (r_px - 1.0f), py + ny * (r_px - 1.0f), q, params),
                0.0f)
                << t;
            EXPECT_EQ(
                VS::ProxyDisparity(px + nx * (r_px + 1.0f), py + ny * (r_px + 1.0f), q, params),
                0.0f)
                << t;
        }
    }
    // Far from the capsule, or switched off: nothing.
    EXPECT_EQ(VS::ProxyDisparity(10.0f, 10.0f, q, params), 0.0f);
    VS::ProxyParams off = q;
    off.on = 0.0f;
    EXPECT_EQ(VS::ProxyDisparity(0.5f * (q.ax + q.bx), 0.5f * (q.ay + q.by), off, params), 0.0f);
    // An end in front of the near plane (0.2) is moved onto it along the capsule.
    VS::DepthProxy clipped = proxy;
    clipped.a[2] = 0.1f;
    const auto c = VS::MakeProxyParams(clipped, proj.cam, width, height);
    ASSERT_GT(c.on, 0.0f);
    EXPECT_NEAR(1.0f / c.inv_za, proj.cam.near_z, 1e-5f);
    // Wholly in front of the near plane, invalid, or not a number: off.
    clipped.b[2] = 0.15f;
    EXPECT_EQ(VS::MakeProxyParams(clipped, proj.cam, width, height).on, 0.0f);
    VS::DepthProxy bad = proxy;
    bad.valid = false;
    EXPECT_EQ(VS::MakeProxyParams(bad, proj.cam, width, height).on, 0.0f);
    bad = proxy;
    bad.b[1] = std::nanf("");
    EXPECT_EQ(VS::MakeProxyParams(bad, proj.cam, width, height).on, 0.0f);
    bad = proxy;
    bad.radius = 0.0f;
    EXPECT_EQ(VS::MakeProxyParams(bad, proj.cam, width, height).on, 0.0f);
}

TEST(DibrMath, DepthRoundTrip) {
    const auto proj = MakeProjector(2560, 116.0f, 4.0f / 3.0f, 0.0335f, 0.25f, 0.75f);
    for (const double z : {0.2, 0.3, 1.0, 10.0, 500.0, 2999.0}) {
        const double d = VS::StoredDepthFromViewDepth(z, proj.cam);
        EXPECT_NEAR(VS::ViewDepthFromStoredDepth(d, proj.cam), z, z * 1e-9);
    }
}

} // namespace
