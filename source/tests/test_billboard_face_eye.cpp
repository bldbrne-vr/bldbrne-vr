// SPDX-License-Identifier: GPL-2.0-or-later

// Eye-facing sprites (shader_recompiler/ir/passes/billboard_face_eye.h) with the game's camera
// rows (bloodborne_camera.cpp WriteOrientation): unchanged on the view axis, facing the eye off
// it, the same whichever way a level head turns, and the eye recovered from the view matrix.

#include <cmath>
#include <random>

#include <gtest/gtest.h>

#include "shader_recompiler/ir/passes/billboard_face_eye.h"

namespace {

using namespace Shader::Optimization::BillboardFaceEye;
using V = Vec3<float>;

struct Camera {
    V right, up, forward, eye;
};

// WriteOrientation's rows: yaw + = right, pitch + = down.
Camera Look(float yaw, float pitch, V eye) {
    const float sy = std::sin(yaw), cy = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
    return {{cy, 0, -sy}, {sy * sp, cp, cy * sp}, {sy * cp, -sp, cy * cp}, eye};
}

V Add(V a, V b, float s = 1) {
    return {a[0] + s * b[0], a[1] + s * b[1], a[2] + s * b[2]};
}
V Normalize(V v) {
    const float l = std::sqrt(Dot(v, v));
    return {v[0] / l, v[1] / l, v[2] / l};
}
void Near(V a, V b, float tolerance = 1e-5f) {
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(a[i], b[i], tolerance) << "component " << i;
    }
}

TEST(BillboardFaceEye, OnTheViewAxisNothingChanges) {
    const Camera cam = Look(0.7f, -0.2f, {1, 2, 3});
    const V center = Add(cam.eye, cam.forward, 5);
    const auto [r, u] = FaceEye(cam.right, cam.up, center, cam.eye);
    Near(r, cam.right);
    Near(u, cam.up);
}

TEST(BillboardFaceEye, OffAxisSpritesFaceTheEye) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> angle(-1.2f, 1.2f), offset(-4, 4);
    for (int n = 0; n < 1000; ++n) {
        const Camera cam = Look(angle(rng), angle(rng), {offset(rng), offset(rng), offset(rng)});
        // Anywhere within 70 degrees of the view axis.
        V center = Add(cam.eye, cam.forward, 6);
        center = Add(center, cam.right, offset(rng));
        center = Add(center, cam.up, offset(rng));
        const auto [r, u] = FaceEye(cam.right, cam.up, center, cam.eye);
        Near(Cross(r, u), Normalize(Add(center, cam.eye, -1)), 1e-4f);
        EXPECT_NEAR(Dot(r, r), 1, 1e-4f);
        EXPECT_NEAR(Dot(u, u), 1, 1e-4f);
        EXPECT_NEAR(Dot(r, u), 0, 1e-4f);
    }
}

// The bug as seen in the headset: a level head turning left and right turned the smoke.
TEST(BillboardFaceEye, TurningALevelHeadNoLongerTurnsTheSprite) {
    const V eye{0, 1.6f, 0}, center{2, 1.6f, 5};
    const Camera ahead = Look(0, 0, eye);
    const auto [r0, u0] = FaceEye(ahead.right, ahead.up, center, eye);
    for (const float yaw : {-0.6f, -0.3f, 0.2f, 0.5f, 0.9f}) {
        const Camera turned = Look(yaw, 0, eye);
        // Before: the view-plane sprite turns with the head.
        EXPECT_GT(std::abs(Dot(turned.right, ahead.right) - 1), 1e-3f);
        const auto [r, u] = FaceEye(turned.right, turned.up, center, eye);
        Near(r, r0);
        Near(u, u0);
    }
}

TEST(BillboardFaceEye, EyeFromTheViewMatrix) {
    const Camera cam = Look(-0.4f, 0.3f, {3, 1.5f, -2});
    // World-to-view rows right, up, forward; translation -row . eye.
    const V t{-Dot(cam.right, cam.eye), -Dot(cam.up, cam.eye), -Dot(cam.forward, cam.eye)};
    Near(EyeFromView(cam.right, cam.up, t), cam.eye);
}

// ---- Per-particle axes (the tessellated smoke and fire) ----

// The rows of VP = P * V that the effect shaders project with: P00, P11, an optional P02 (jitter or
// an off-centre crop) and w = view depth.
struct VP {
    V row0, row1, row3;
    float w0, w1, w3;
};
VP Project(const Camera& cam, float p00 = 0.72f, float p11 = 0.67f, float p02 = 0.0f) {
    const float t0 = -Dot(cam.right, cam.eye), t1 = -Dot(cam.up, cam.eye),
                t2 = -Dot(cam.forward, cam.eye);
    VP vp;
    for (int i = 0; i < 3; ++i) {
        vp.row0[i] = p00 * cam.right[i] + p02 * cam.forward[i];
        vp.row1[i] = p11 * cam.up[i];
        vp.row3[i] = cam.forward[i];
    }
    vp.w0 = p00 * t0 + p02 * t2;
    vp.w1 = p11 * t1;
    vp.w3 = t2;
    return vp;
}
ViewFrame<float> FromVP(const VP& vp) {
    return ViewFromViewProjection(vp.row0, vp.w0, vp.row1, vp.w1, vp.row3, vp.w3);
}
// The effect system's sprite axes: spin and size in the view plane of the camera it was built for.
std::array<V, 2> SpriteAxes(const Camera& cam, float spin, float sx, float sy) {
    const float c = std::cos(spin), s = std::sin(spin);
    V a0, a1;
    for (int i = 0; i < 3; ++i) {
        a0[i] = sx * (c * cam.right[i] + s * cam.up[i]);
        a1[i] = sy * (-s * cam.right[i] + c * cam.up[i]);
    }
    return {a0, a1};
}

TEST(BillboardFaceEyeParticles, TheCameraComesBackFromViewProjection) {
    for (const float p02 : {0.0f, 0.03f, -0.2f}) {
        const Camera cam = Look(0.9f, -0.35f, {-234.6f, 29.9f, 143.3f});
        const auto view = FromVP(Project(cam, 0.72f, 0.67f, p02));
        Near(view.right, cam.right);
        Near(view.up, cam.up);
        Near(view.forward, cam.forward);
        Near(view.eye, cam.eye, 2e-3f);
    }
}

TEST(BillboardFaceEyeParticles, ASpriteAheadOfALevelHeadIsUnchanged) {
    const Camera cam = Look(0.4f, 0, {1, 1.6f, -3});
    const auto [a0, a1] = SpriteAxes(cam, 0.7f, 0.8f, 1.3f);
    float weight = 0;
    const auto [b0, b1] =
        FaceEyeUpright(FromVP(Project(cam)), a0, a1, Add(cam.eye, cam.forward, 4), weight);
    EXPECT_EQ(weight, 1.0f);
    Near(b0, a0, 1e-4f);
    Near(b1, a1, 1e-4f);
}

// The bug as seen in the headset: the head turns, the smoke turned with it.
TEST(BillboardFaceEyeParticles, TurningTheHeadNoLongerTurnsTheSprite) {
    const V eye{0, 1.6f, 0};
    for (const V center : {V{2, 1.6f, 5}, V{-3, 2.8f, 2}, V{0.5f, 0.3f, 4}}) {
        std::array<V, 2> first{};
        bool have = false;
        for (const float yaw : {-0.9f, -0.4f, 0.0f, 0.3f, 0.8f}) {
            for (const float pitch : {-0.4f, 0.0f, 0.3f}) {
                const Camera cam = Look(yaw, pitch, eye);
                // The effect system rebuilds the axes for every camera: same spin and size.
                const auto [a0, a1] = SpriteAxes(cam, 0.5f, 1.0f, 0.6f);
                float weight = 0;
                const auto [b0, b1] =
                    FaceEyeUpright(FromVP(Project(cam)), a0, a1, center, weight);
                EXPECT_EQ(weight, 1.0f);
                if (!have) {
                    first = {b0, b1};
                    have = true;
                }
                Near(b0, first[0], 2e-4f);
                Near(b1, first[1], 2e-4f);
            }
        }
    }
}

TEST(BillboardFaceEyeParticles, SpritesFaceTheEyeAndKeepTheirSize) {
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> angle(-1.0f, 1.0f), offset(-4, 4), spin(-3, 3);
    for (int n = 0; n < 1000; ++n) {
        const Camera cam = Look(angle(rng), 0.6f * angle(rng), {offset(rng), offset(rng), 0});
        V center = Add(cam.eye, cam.forward, 6);
        center = Add(center, cam.right, offset(rng));
        center = Add(center, cam.up, 0.5f * offset(rng));
        const auto [a0, a1] = SpriteAxes(cam, spin(rng), 0.9f, 1.4f);
        float weight = 0;
        const auto [b0, b1] = FaceEyeUpright(FromVP(Project(cam)), a0, a1, center, weight);
        EXPECT_EQ(weight, 1.0f);
        const V toward = Normalize(Add(center, cam.eye, -1));
        EXPECT_NEAR(std::abs(Dot(Normalize(Cross(b0, b1)), toward)), 1, 1e-4f);
        EXPECT_NEAR(Dot(b0, b0), 0.81f, 1e-3f);
        EXPECT_NEAR(Dot(b1, b1), 1.96f, 1e-3f);
        EXPECT_NEAR(Dot(b0, b1), Dot(a0, a1), 1e-3f);
    }
}

TEST(BillboardFaceEyeParticles, ParticlesOutOfTheViewPlaneKeepTheirOrientation) {
    const Camera cam = Look(0.2f, 0.1f, {0, 1.6f, 0});
    // A ground decal: axes along the floor, its normal straight up.
    const V a0{1, 0, 0}, a1{0, 0, 1};
    float weight = 1;
    FaceEyeUpright(FromVP(Project(cam)), a0, a1, V{1, 0, 4}, weight);
    EXPECT_EQ(weight, 0.0f);
}

} // namespace
