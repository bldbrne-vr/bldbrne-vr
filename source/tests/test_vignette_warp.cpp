// SPDX-License-Identifier: GPL-2.0-or-later

// The vignettes drawn by the late warp (vr.ini vignette_in_warp; video_core/renderer_vulkan/
// vr_vignette_warp.h, mirrored by vr_eye_upscale.comp's VR_VIGNETTE): the ring table is the ring
// mesh's, the rasterised profile is reproduced exactly along the mesh's spokes, each vignette
// keeps the overlay's own parameters and plane (so its stereo depth), each mode's shape, and the
// blend is "over" in linear light in the eyes' byte order.

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vr_overlay_meshes.h"
#include "video_core/renderer_vulkan/vr_vignette_warp.h"

namespace {

using namespace Vulkan::VrOverlay;
namespace W = Vulkan::VrVignetteWarp;

constexpr u32 Segments = 64; // BuildMesh(MeshId::Vignette)

TEST(VignetteWarp, RingTableIsTheMeshs) {
    const MeshData mesh = BuildMesh(MeshId::Vignette);
    ASSERT_EQ(mesh.vertices.size(), static_cast<size_t>(W::RingCount * (Segments + 1)));
    for (u32 i = 0; i < W::RingCount; ++i) {
        const Vertex& v = mesh.vertices[i * (Segments + 1)];
        EXPECT_NEAR(v.aux.x * 90.0f, W::RingDeg[i], 1e-4f) << i;
        EXPECT_FLOAT_EQ(v.aux.y, W::RingProfile(W::RingDeg[i])) << i;
        const float r = std::hypot(v.position.x, v.position.y);
        EXPECT_NEAR(r, std::tan(W::RingDeg[i] * DegToRad), 1e-5f * std::max(r, 1.0f)) << i;
    }
}

TEST(VignetteWarp, RasterisedProfileAlongTheSpokes) {
    // Along a spoke (the edge between a vertex of one ring and the same vertex of the next) the
    // rasteriser interpolates the vertex attributes linearly in the plane: SampleRing must agree.
    const MeshData mesh = BuildMesh(MeshId::Vignette);
    const u32 row = Segments + 1;
    for (u32 i = 0; i + 1 < W::RingCount; ++i) {
        for (u32 j : {0u, 7u, 33u}) {
            const Vertex& a = mesh.vertices[i * row + j];
            const Vertex& b = mesh.vertices[(i + 1) * row + j];
            for (const float f : {0.0f, 0.25f, 0.5f, 0.9f, 1.0f}) {
                const float x = a.position.x + (b.position.x - a.position.x) * f;
                const float y = a.position.y + (b.position.y - a.position.y) * f;
                const float want_angle = (a.aux.x + (b.aux.x - a.aux.x) * f) * 90.0f;
                const float want_profile = a.aux.y + (b.aux.y - a.aux.y) * f;
                const W::RingSample s = W::SampleRing(std::hypot(x, y));
                ASSERT_TRUE(s.inside) << i << " " << f;
                EXPECT_NEAR(s.angle_deg, want_angle, 2e-3f) << i << " " << f;
                EXPECT_NEAR(s.profile, want_profile, 2e-5f) << i << " " << f;
            }
        }
    }
}

TEST(VignetteWarp, NothingOffTheMesh) {
    EXPECT_FALSE(W::SampleRing(0.0f).inside);
    EXPECT_FALSE(W::SampleRing(std::tan(25.9f * DegToRad)).inside);
    EXPECT_TRUE(W::SampleRing(std::tan(26.01f * DegToRad)).inside);
    EXPECT_TRUE(W::SampleRing(std::tan(84.9f * DegToRad)).inside);
    EXPECT_FALSE(W::SampleRing(std::tan(85.1f * DegToRad)).inside);
}

Pose HeadAt(Vec3 p, float yaw_rad) {
    return {{0.0f, std::sin(0.5f * yaw_rad), 0.0f, std::cos(0.5f * yaw_rad)}, p};
}

W::Item ItemOf(const Pose& head, const VignetteVisual& v) {
    std::vector<DrawItem> items;
    AppendVignette(items, head, v);
    EXPECT_EQ(items.size(), 1u);
    W::Item it{};
    EXPECT_TRUE(!items.empty() && W::FromDrawItem(items.front(), it));
    return it;
}

TEST(VignetteWarp, TheOverlaysOwnParameters) {
    const Pose head = HeadAt({0.3f, 1.6f, -0.2f}, 0.7f);
    const VignetteVisual hit{.color = {0.6f, 0.004f, 0.003f},
                             .alpha = 0.3f,
                             .distance_m = 2.0f,
                             .profile_power = 3.0f,
                             .inner_deg = 31.0f,
                             .full_deg = 57.0f,
                             .wedge_focus = 0.6f,
                             .wedge_x = 1.0f,
                             .wedge_y = 0.0f,
                             .wedge_power = 4.0f};
    std::vector<DrawItem> items;
    AppendVignette(items, head, hit);
    ASSERT_EQ(items.size(), 1u);
    const Material& m = items[0].material;
    W::Item it{};
    ASSERT_TRUE(W::FromDrawItem(items[0], it));
    EXPECT_FLOAT_EQ(it.color[0], m.base.x);
    EXPECT_FLOAT_EQ(it.color[3], m.base.w * m.fade);
    EXPECT_FLOAT_EQ(it.params[0], m.params.x);
    EXPECT_FLOAT_EQ(it.params[1], m.params.y);
    EXPECT_FLOAT_EQ(it.params[2], m.params.z);
    EXPECT_FLOAT_EQ(it.params[3], m.params.w);
    EXPECT_FLOAT_EQ(it.wedge[0], m.emissive.x);
    EXPECT_FLOAT_EQ(it.wedge[3], m.emissive.w);
    EXPECT_NEAR(it.origin[3], 2.0f, 1e-5f);
    // The plane is 2 m ahead of the head, its normal pointing back at it.
    const float back[3] = {head.position.x - it.origin[0], head.position.y - it.origin[1],
                           head.position.z - it.origin[2]};
    EXPECT_NEAR(std::sqrt(back[0] * back[0] + back[1] * back[1] + back[2] * back[2]), 2.0f, 1e-4f);
    EXPECT_NEAR(W::Dot3(it.axis_z, back), 2.0f, 1e-4f);
    // Not a vignette: refused.
    std::vector<DrawItem> other;
    AppendAxes(other, Mat4{}, 0.05f);
    W::Item none{};
    EXPECT_FALSE(W::FromDrawItem(other.front(), none));
    // No opacity: AppendVignette draws nothing (nothing goes to the warp either).
    std::vector<DrawItem> zero;
    AppendVignette(zero, head, {.alpha = 0.0f});
    EXPECT_TRUE(zero.empty());
}

// The ray from `eye` toward the head-frame direction (x, y, -1) rotated with the head.
void Ray(const Pose& head, Vec3 eye_offset, float dx, float dy, float p[3], float d[3]) {
    const Mat4 r = Mat4::FromPose({head.orientation, {}});
    const Vec3 o = head.position + Vec3{r.At(0, 0) * eye_offset.x + r.At(0, 1) * eye_offset.y +
                                            r.At(0, 2) * eye_offset.z,
                                        r.At(1, 0) * eye_offset.x + r.At(1, 1) * eye_offset.y +
                                            r.At(1, 2) * eye_offset.z,
                                        r.At(2, 0) * eye_offset.x + r.At(2, 1) * eye_offset.y +
                                            r.At(2, 2) * eye_offset.z};
    const Vec3 v{dx, dy, -1.0f};
    p[0] = o.x;
    p[1] = o.y;
    p[2] = o.z;
    for (u32 k = 0; k < 3; ++k) {
        d[k] = r.At(k, 0) * v.x + r.At(k, 1) * v.y + r.At(k, 2) * v.z;
    }
}

TEST(VignetteWarp, PlaneHitIsTheTangentAndTheStereoDepthIsTheRings) {
    const Pose head = HeadAt({0.1f, 1.5f, 0.4f}, -1.1f);
    const W::Item it = ItemOf(head, {.alpha = 0.5f, .distance_m = 2.0f});
    float p[3], d[3], tx = 9.0f, ty = 9.0f;
    // From the head: the ray's own tangents.
    Ray(head, {}, 0.0f, 0.0f, p, d);
    ASSERT_TRUE(W::PlaneHit(it, p, d, tx, ty));
    EXPECT_NEAR(tx, 0.0f, 1e-5f);
    EXPECT_NEAR(ty, 0.0f, 1e-5f);
    Ray(head, {}, 0.8f, -0.3f, p, d);
    ASSERT_TRUE(W::PlaneHit(it, p, d, tx, ty));
    EXPECT_NEAR(tx, 0.8f, 1e-4f);
    EXPECT_NEAR(ty, -0.3f, 1e-4f);
    // Each eye looking at the same point of the ring sees the same t: the ring has the
    // overlay's stereo depth (2 m), whatever the eye distance.
    const float b = 0.032f;
    const float target_x = 1.4f, target_y = 0.5f; // a point on the plane, head frame (z = -2)
    for (const float side : {-1.0f, 1.0f}) {
        const float ex = side * b;
        Ray(head, {ex, 0.0f, 0.0f}, (target_x - ex) / 2.0f, target_y / 2.0f, p, d);
        ASSERT_TRUE(W::PlaneHit(it, p, d, tx, ty));
        EXPECT_NEAR(tx, target_x / 2.0f, 1e-4f) << side;
        EXPECT_NEAR(ty, target_y / 2.0f, 1e-4f) << side;
    }
    // Behind the eye: nothing.
    Ray(head, {}, 0.0f, 0.0f, p, d);
    for (float& k : d) {
        k = -k;
    }
    EXPECT_FALSE(W::PlaneHit(it, p, d, tx, ty));
}

TEST(VignetteWarp, ModesHaveTheirShapes) {
    const Pose head{};
    const std::array<float, 4> field{52.0f, 50.0f, 44.0f, 53.0f};
    const auto at = [&](const W::Item& it, float deg, float dir_x = 1.0f, float dir_y = 0.0f) {
        const float r = std::tan(deg * DegToRad);
        return W::Alpha(it, r * dir_x, r * dir_y, field);
    };
    // Mode 0 (the heal glow): clear inside 26 degrees, full from 56, rising in between.
    const W::Item heal = ItemOf(head, {.alpha = 0.35f});
    EXPECT_EQ(at(heal, 20.0f), 0.0f);
    EXPECT_NEAR(at(heal, 26.5f), 0.0f, 1e-3f);
    EXPECT_NEAR(at(heal, 60.0f), 0.35f, 1e-6f);
    float last = 0.0f;
    for (float a = 26.0f; a <= 56.0f; a += 0.5f) {
        const float v = at(heal, a);
        EXPECT_GE(v, last - 1e-7f) << a;
        last = v;
    }
    // Mode 1 (the low-HP edge): clear inside its inner angle, full from its full angle.
    const W::Item dark = ItemOf(head, {.alpha = 0.5f, .inner_deg = 30.0f, .full_deg = 50.0f});
    EXPECT_NEAR(at(dark, 29.0f), 0.0f, 1e-6f);
    EXPECT_NEAR(at(dark, 52.0f), 0.5f, 1e-6f);
    // Mode 2 (the rally rim): a band, clear well inside and well outside it.
    const W::Item rim =
        ItemOf(head, {.alpha = 0.4f, .inner_deg = 54.0f, .full_deg = 62.0f, .band = true});
    EXPECT_NEAR(at(rim, 45.0f), 0.0f, 1e-6f);
    EXPECT_NEAR(at(rim, 58.0f), 0.4f, 1e-6f);
    EXPECT_NEAR(at(rim, 70.0f), 0.0f, 1e-6f);
    // Mode 3 (fitted to the field): full past fit_full of each side's half-angle.
    const W::Item fit = ItemOf(head, {.alpha = 0.6f, .fit_inner = 0.6f, .fit_full = 1.0f});
    EXPECT_NEAR(at(fit, 51.0f, 1.0f, 0.0f), 0.6f, 1e-6f);  // right: past 50
    EXPECT_LT(at(fit, 51.0f, -1.0f, 0.0f), 0.6f);          // left: 52 is the edge there
    EXPECT_NEAR(at(fit, 26.2f, 0.0f, 1.0f), 0.0f, 1e-6f);  // up: 26.2 < 0.6 * 44
    // The wedge: stronger toward its direction than away from it.
    const W::Item wedge = ItemOf(head, {.alpha = 0.3f,
                                        .profile_power = 3.0f,
                                        .wedge_focus = 0.8f,
                                        .wedge_x = 1.0f,
                                        .wedge_y = 0.0f,
                                        .wedge_power = 4.0f});
    EXPECT_GT(at(wedge, 58.0f, 1.0f, 0.0f), 2.0f * at(wedge, 58.0f, -1.0f, 0.0f));
    EXPECT_NEAR(at(wedge, 58.0f, 1.0f, 0.0f), 0.3f, 1e-5f);
}

TEST(VignetteWarp, BlendIsOverInLinearLight) {
    W::Frame f{};
    const Pose head{};
    f.items[0] = ItemOf(head, {.color = {0.0f, 0.0f, 0.0f}, .alpha = 0.5f});
    f.count = 1;
    float p[3], d[3];
    Ray(head, {}, std::tan(70.0f * DegToRad), 0.0f, p, d); // full profile there
    const auto out = W::Apply(f, false, {0.5f, 0.5f, 0.5f}, p, d);
    const float want = W::LinearToSrgb(0.5f * W::SrgbToLinear(0.5f));
    EXPECT_NEAR(out[0], want, 1e-6f);
    EXPECT_NEAR(out[0], 0.3613f, 1e-3f); // not 0.25: linear light
    // Opaque: the colour itself.
    f.items[0] = ItemOf(head, {.color = {0.2f, 0.4f, 0.6f}, .alpha = 1.0f});
    const auto full = W::Apply(f, false, {0.9f, 0.1f, 0.3f}, p, d);
    EXPECT_NEAR(full[0], W::LinearToSrgb(0.2f), 1e-5f);
    EXPECT_NEAR(full[2], W::LinearToSrgb(0.6f), 1e-5f);
    // B8G8R8A8 bytes: the colour goes in the same order.
    const auto swapped = W::Apply(f, true, {0.3f, 0.1f, 0.9f}, p, d);
    EXPECT_NEAR(swapped[0], full[2], 1e-6f);
    EXPECT_NEAR(swapped[2], full[0], 1e-6f);
    // In the clear middle, or with none: the pixel as it was (bit for bit with none).
    Ray(head, {}, 0.0f, 0.0f, p, d);
    const auto mid = W::Apply(f, false, {0.9f, 0.1f, 0.3f}, p, d);
    EXPECT_EQ(mid[0], 0.9f); // untouched (no decode / encode)
    EXPECT_EQ(mid[1], 0.1f);
    f.count = 0;
    const auto none = W::Apply(f, false, {0.123f, 0.456f, 0.789f}, p, d);
    EXPECT_EQ(none[0], 0.123f);
    EXPECT_EQ(none[1], 0.456f);
    EXPECT_EQ(none[2], 0.789f);
}

TEST(VignetteWarp, StackedInOrder) {
    // Two vignettes: the later one over the earlier one (the overlay's draw order).
    W::Frame f{};
    const Pose head{};
    f.items[0] = ItemOf(head, {.color = {0.0f, 0.0f, 0.0f}, .alpha = 1.0f});
    f.items[1] = ItemOf(head, {.color = {1.0f, 0.0f, 0.0f}, .alpha = 0.5f});
    f.count = 2;
    float p[3], d[3];
    Ray(head, {}, std::tan(70.0f * DegToRad), 0.0f, p, d);
    const auto out = W::Apply(f, false, {0.7f, 0.7f, 0.7f}, p, d);
    EXPECT_NEAR(out[0], W::LinearToSrgb(0.5f), 1e-5f);
    EXPECT_NEAR(out[1], 0.0f, 1e-6f);
}

} // namespace
