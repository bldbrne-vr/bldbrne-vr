// SPDX-License-Identifier: GPL-2.0-or-later

// The camera shake measurement (core/vr/camera_shake.h): the angle between two camera
// orientations, the game's yaw/pitch convention, roll, and the position off the orbit axis.

#include <cmath>

#include <gtest/gtest.h>

#include "core/vr/camera_shake.h"

namespace {

namespace S = VR::Shake;
constexpr float Deg = 0.0174532925f;

// The rows of R(yaw, pitch) rolled by `roll` about the forward row (+ = right side up), as the
// camera bridge's WriteOrientation does for camera_roll.
S::Rows Rolled(float yaw, float pitch, float roll) {
    S::Rows o = S::FromYawPitch(yaw, pitch);
    const float sr = std::sin(roll), cr = std::cos(roll);
    for (int j = 0; j < 3; ++j) {
        const float right = o.r[0][j], up = o.r[1][j];
        o.r[0][j] = right * cr + up * sr;
        o.r[1][j] = up * cr - right * sr;
    }
    return o;
}

TEST(CameraShake, SameOrientationIsZero) {
    for (float yaw : {-3.0f, -1.0f, 0.0f, 0.7f, 2.9f}) {
        for (float pitch : {-1.2f, -0.3f, 0.0f, 0.5f, 1.3f}) {
            const auto a = S::FromYawPitch(yaw, pitch);
            EXPECT_LT(S::AngleDeg(a, a), 0.05f) << yaw << " " << pitch;
        }
    }
}

TEST(CameraShake, AngleOfAYawOrPitchStep) {
    const auto a = S::FromYawPitch(0.4f, 0.2f);
    EXPECT_NEAR(S::AngleDeg(a, S::FromYawPitch(0.4f + 1.0f * Deg, 0.2f)), 1.0f, 0.05f);
    EXPECT_NEAR(S::AngleDeg(a, S::FromYawPitch(0.4f, 0.2f - 2.0f * Deg)), 2.0f, 0.05f);
    EXPECT_NEAR(S::AngleDeg(a, S::FromYawPitch(0.4f + 30.0f * Deg, 0.2f)),
                30.0f * std::cos(0.2f), 0.6f); // yaw about world up, seen at a pitch
}

TEST(CameraShake, RollIsMeasured) {
    const auto level = S::FromYawPitch(1.0f, 0.3f);
    EXPECT_NEAR(S::RollDeg(level), 0.0f, 0.01f);
    const auto rolled = Rolled(1.0f, 0.3f, 3.0f * Deg);
    EXPECT_NEAR(S::RollDeg(rolled), 3.0f, 0.05f);
    EXPECT_NEAR(S::RollDeg(Rolled(-2.0f, -0.4f, -1.5f * Deg)), -1.5f, 0.05f);
    EXPECT_NEAR(S::AngleDeg(level, rolled), 3.0f, 0.05f);
}

TEST(CameraShake, FromMatrixReadsThreeRowsOfFour) {
    const auto a = S::FromYawPitch(0.9f, -0.25f);
    float m[12];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            m[i * 4 + j] = a.r[i][j];
        }
        m[i * 4 + 3] = 123.0f; // the unused fourth float of each row
    }
    EXPECT_LT(S::AngleDeg(S::FromMatrix(m), a), 0.05f);
}

TEST(CameraShake, BrokenMatricesStayFinite) {
    S::Rows zero{};
    const auto a = S::FromYawPitch(0.0f, 0.0f);
    EXPECT_TRUE(std::isfinite(S::AngleDeg(zero, a)));
    S::Rows big = a;
    for (auto& row : big.r) {
        for (float& v : row) {
            v *= 3.0f;
        }
    }
    EXPECT_TRUE(std::isfinite(S::AngleDeg(big, a)));
    EXPECT_NEAR(S::AngleDeg(big, a), 0.0f, 0.01f); // clamped
}

TEST(CameraShake, OffAxisIgnoresTheOrbitDistance) {
    const auto rows = S::FromYawPitch(0.6f, 0.35f);
    const float pivot[3] = {10.0f, 2.0f, -4.0f};
    const float* f = rows.r[2];
    for (float dist : {0.0f, 0.5f, 3.0f}) {
        const float pos[3] = {pivot[0] - f[0] * dist, pivot[1] - f[1] * dist,
                              pivot[2] - f[2] * dist};
        EXPECT_LT(S::OffAxis(pos, pivot, rows), 1e-4f) << dist;
    }
    // 4 cm up the camera's own up row and 3 cm along its right row: 5 cm off the axis.
    const float* u = rows.r[1];
    const float* r = rows.r[0];
    const float pos[3] = {pivot[0] - f[0] * 2.0f + u[0] * 0.04f + r[0] * 0.03f,
                          pivot[1] - f[1] * 2.0f + u[1] * 0.04f + r[1] * 0.03f,
                          pivot[2] - f[2] * 2.0f + u[2] * 0.04f + r[2] * 0.03f};
    EXPECT_NEAR(S::OffAxis(pos, pivot, rows), 0.05f, 1e-4f);
}

TEST(CameraShake, Distance) {
    const float a[3] = {1.0f, 2.0f, 3.0f};
    const float b[3] = {4.0f, 6.0f, 3.0f};
    EXPECT_NEAR(S::Distance(a, b), 5.0f, 1e-5f);
    EXPECT_EQ(S::Distance(a, a), 0.0f);
}

} // namespace
