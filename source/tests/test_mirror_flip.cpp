// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_GPU_MIRROR_FLIP (scratchpad/dual-eye-perf/asymmetric_projection.h): a planar reflection's
// mirrored camera is told from the frame's by its own clip plane, and with each eye's frustum
// upside down the water's flipped read (u, 1 - v) lands where that eye sees the reflection. The
// scene blocks are built like the sewer capture's (churn_cpu2_A, frame 8351: the frame camera at
// y -48.917, its reflection's at -71.639 below the water at -60.278; guest tan 1.38385 x 1.4979;
// Quest 3 eyes, up 44 and down 55 degrees), at other head pitches and rolls too.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include <gtest/gtest.h>

#include "scratchpad/dual-eye-perf/asymmetric_projection.h"

namespace {

using NativeProjection::Transform;
using Vec3 = std::array<float, 3>;
using Block = std::array<float, 216>;

constexpr float Radians = 3.14159265358979323846f / 180.f;
constexpr float TanX = 1.38385f, TanY = 1.4979f;
constexpr float Water = -60.278f; // the mirror plane's height
constexpr float Ipd = .064f;

float Dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vec3 Add(const Vec3& a, const Vec3& b, float s = 1.f) {
    return {a[0] + s * b[0], a[1] + s * b[1], a[2] + s * b[2]};
}
Vec3 Mirror(const Vec3& p) {
    return {p[0], 2 * Water - p[1], p[2]};
}

// A camera: its position and world-to-view rows (right, up, forward; view z is forward).
struct Pose {
    Vec3 position;
    std::array<Vec3, 3> rows;
};
// The capture's heading, then a head pitch (+ = up) and roll about the view axis.
Pose Frame(float pitch_degrees, float roll_degrees) {
    const float yaw = 62.1f * Radians; // forward (-0.884, 0, 0.467) as captured
    const Vec3 right{std::cos(yaw), 0, std::sin(yaw)}, up{0, 1, 0};
    const Vec3 level{-std::sin(yaw), 0, std::cos(yaw)};
    const float p = pitch_degrees * Radians, r = roll_degrees * Radians;
    const Vec3 forward = Add(Add({}, level, std::cos(p)), up, std::sin(p));
    const Vec3 pitched_up = Add(Add({}, up, std::cos(p)), level, -std::sin(p));
    const Vec3 rolled_right = Add(Add({}, right, std::cos(r)), pitched_up, std::sin(r));
    const Vec3 rolled_up = Add(Add({}, pitched_up, std::cos(r)), right, -std::sin(r));
    return {{-209.892f, -48.917f, -55.584f}, {rolled_right, rolled_up, forward}};
}
// The game's reflection camera: the frame camera mirrored below the water, its view turned upside
// down to stay right-handed (view y negated after the world mirror).
Pose Reflection(const Pose& frame) {
    Pose mirrored{Mirror(frame.position), {}};
    for (int r = 0; r < 3; ++r) {
        const Vec3& row = frame.rows[r];
        mirrored.rows[r] = {row[0], -row[1], row[2]};
    }
    for (float& v : mirrored.rows[1])
        v = -v;
    return mirrored;
}

constexpr float NearA = 1.00003f, NearB = -.100003f; // the capture's near rows
constexpr std::array<float, 16> InverseProjection{
    TanX, 0, 0, 0, 0, TanY, 0, 0, 0, 0, 0, 1, 0, 0, 1 / NearB, -NearA / NearB};

// The world plane normal . x = d, kept where normal . x > d, as a scene block's clip plane:
// camera-relative (n, n.c - d) in view space, then into clip space, inverse(P)^T.
std::array<float, 4> ClipPlane(const Pose& pose, const Vec3& normal, float d) {
    const std::array<float, 4> view{Dot(pose.rows[0], normal), Dot(pose.rows[1], normal),
                                    Dot(pose.rows[2], normal), Dot(normal, pose.position) - d};
    std::array<float, 4> clip{};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            clip[c] += InverseProjection[4 * r + c] * view[r];
    return clip;
}

// The scene block's dwords NativeProjection reads: viewport 4..7, view 8..19, inverse P 36..51,
// P 52..67, inverse view 180..195, clip plane 196..199, camera-relative view-projection 200..215.
// `plane`: the world plane y = Water, kept above (the reflection's), else 0, 0, 0, 1.
Block SceneBlock(const Pose& pose, bool plane) {
    Block f{};
    f[4] = 1456;
    f[5] = 1576;
    f[6] = 1 / 1456.f;
    f[7] = 1 / 1576.f;
    const float a = NearA, b = NearB;
    const std::array<float, 16> p{1 / TanX, 0, 0, 0, 0, 1 / TanY, 0, 0, 0, 0, a, b, 0, 0, 1, 0};
    const auto& inverse = InverseProjection;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            f[8 + 4 * r + c] = pose.rows[r][c];
            f[180 + 4 * r + c] = pose.rows[c][r];
        }
        f[8 + 4 * r + 3] = -Dot(pose.rows[r], pose.position);
        f[180 + 4 * r + 3] = pose.position[r];
    }
    f[195] = 1;
    std::memcpy(f.data() + 36, inverse.data(), sizeof(inverse));
    std::memcpy(f.data() + 52, p.data(), sizeof(p));
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            f[200 + 4 * r + c] = c < 3
                                     ? p[4 * r] * pose.rows[0][c] + p[4 * r + 1] * pose.rows[1][c] +
                                           p[4 * r + 2] * pose.rows[2][c]
                                     : p[4 * r + 3];
    if (!plane) {
        f[199] = 1;
        return f;
    }
    const auto clip = ClipPlane(pose, {0, 1, 0}, Water);
    std::copy(clip.begin(), clip.end(), f.begin() + 196);
    return f;
}
Block WithPlane(Block f, const std::array<float, 4>& clip) {
    std::copy(clip.begin(), clip.end(), f.begin() + 196);
    return f;
}

// What NativeStereo::CameraBuffer gives one eye: moved by `eye` along the camera's right axis,
// remapped by `t`, the clip plane per eye for a mirrored camera.
Block EyeBlock(const Block& center, float eye, const Transform& t, bool mirror) {
    Block f = center;
    for (int r = 0; r < 3; ++r)
        f[183 + 4 * r] += center[180 + 4 * r] * eye;
    NativeProjection::Scene(f.data(), t);
    if (mirror)
        NativeProjection::EyeClipPlane(f.data(), center.data(), eye, t);
    return f;
}
// The clip position a scene shader computes: M * (world - eye position).
std::array<float, 4> Clip(const Block& f, const Vec3& world) {
    const Vec3 local{world[0] - f[183], world[1] - f[187], world[2] - f[191]};
    std::array<float, 4> clip{};
    for (int r = 0; r < 4; ++r)
        clip[r] = f[200 + 4 * r] * local[0] + f[201 + 4 * r] * local[1] +
                  f[202 + 4 * r] * local[2] + f[203 + 4 * r];
    return clip;
}
float PlaneDistance(const Block& f, const std::array<float, 4>& clip) {
    return f[196] * clip[0] + f[197] * clip[1] + f[198] * clip[2] + f[199] * clip[3];
}

// Quest 3: each eye's FOV, left/right/up/down.
std::array<float, 4> EyeFov(int eye) {
    return {(eye == 0 ? -54.f : -40.f) * Radians, (eye == 0 ? 40.f : 54.f) * Radians,
            44.f * Radians, -55.f * Radians};
}
float EyeShift(int eye) {
    return (eye == 0 ? -.5f : .5f) * Ipd;
}

TEST(MirrorFlip, TellsTheReflectionCameraFromTheFrameCamera) {
    // At any head pitch and roll: the water is level in the world, though a rolled head turns it
    // in the image (past 45 degrees it is nearer the view's right axis than its up).
    for (float pitch : {-85.f, -35.f, -20.f, 0.f, 20.f, 35.f, 85.f}) {
        for (float roll : {-120.f, -60.f, -10.f, 0.f, 10.f, 46.f, 90.f}) {
            const Pose frame = Frame(pitch, roll);
            const Block block = SceneBlock(frame, false);
            const Block reflection = SceneBlock(Reflection(frame), true);
            EXPECT_FALSE(NativeProjection::MirroredCamera(block.data())) << pitch << " " << roll;
            EXPECT_FALSE(NativeProjection::MirroredCamera(block.data(), frame.position.data()))
                << pitch << " " << roll;
            EXPECT_TRUE(NativeProjection::MirroredCamera(reflection.data()))
                << pitch << " " << roll;
            EXPECT_TRUE(NativeProjection::MirroredCamera(reflection.data(), frame.position.data()))
                << pitch << " " << roll;
        }
    }
    const Block captured = SceneBlock(Reflection(Frame(0, 0)), true);
    // The capture's clip plane: (0, 1.4979, 113.6077, -113.6115).
    EXPECT_NEAR(captured[196], 0.f, 1e-6f);
    EXPECT_NEAR(captured[197], 1.4979f, 1e-4f);
    EXPECT_NEAR(captured[198], 113.6077f, .01f);
    EXPECT_NEAR(captured[199], -113.6115f, .01f);
    EXPECT_NEAR(captured[187], -71.639f, 1e-3f);
}

TEST(MirrorFlip, RefusesOtherClipPlanesAndViews) {
    const Pose frame = Frame(-15, 0);
    Block none = SceneBlock(Reflection(frame), true);
    std::fill_n(none.begin() + 196, 4, 0.f);
    EXPECT_FALSE(NativeProjection::MirroredCamera(none.data())); // no plane at all
    // The frame camera clipping by the water, kept above: it sits in front of its plane.
    EXPECT_FALSE(NativeProjection::MirroredCamera(SceneBlock(frame, true).data()));
    // An inverted view (the plain mirror view): its image needs no flip.
    Pose plain = Reflection(frame);
    for (float& v : plain.rows[1])
        v = -v;
    EXPECT_FALSE(NativeProjection::MirroredCamera(SceneBlock(plain, true).data()));
    // The plane's normal along the view's right axis.
    Block sideways = SceneBlock(Reflection(frame), true);
    std::swap(sideways[196], sideways[197]);
    EXPECT_FALSE(NativeProjection::MirroredCamera(sideways.data()));
    // A wall mirror 2 m to the camera's right: the frame camera mirrored across it, its view x
    // turned to stay right-handed (its read would flip sideways). Behind its plane, but the plane
    // is not level.
    const Vec3 across = frame.rows[0]; // level: no roll
    const float wall = Dot(across, frame.position) + 2;
    Pose mirrored{Add(frame.position, across, 4), {}};
    for (int r = 0; r < 3; ++r)
        mirrored.rows[r] = Add(frame.rows[r], across, -2 * Dot(across, frame.rows[r]));
    for (float& v : mirrored.rows[0])
        v = -v;
    const Block wall_mirror =
        WithPlane(SceneBlock(mirrored, false), ClipPlane(mirrored, Add({}, across, -1), -wall));
    EXPECT_FALSE(NativeProjection::MirroredCamera(wall_mirror.data()));
    EXPECT_FALSE(NativeProjection::MirroredCamera(wall_mirror.data(), frame.position.data()));
    // Not finite.
    Block broken = SceneBlock(Reflection(frame), true);
    broken[198] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(NativeProjection::MirroredCamera(broken.data()));
}

// The frame's render camera (the record's render_position) is the reflection camera's mirror
// image. A camera at the eye that clips away all above the water (an underwater cut or a
// refraction pass) passes every other test; only the render camera tells it apart. A record a
// little behind its frame (under half the camera's height over the water) still passes.
TEST(MirrorFlip, RenderCameraIsTheMirrorImage) {
    for (float pitch : {-30.f, 0.f, 25.f}) {
        const Pose frame = Frame(pitch, 5);
        const float height = frame.position[1] - Water;
        const Block reflection = SceneBlock(Reflection(frame), true);
        for (const Vec3& direction : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, -.6f, .8f}}) {
            const Vec3 near = Add(frame.position, direction, .45f * height);
            const Vec3 far = Add(frame.position, direction, .55f * height);
            EXPECT_TRUE(NativeProjection::MirroredCamera(reflection.data(), near.data())) << pitch;
            EXPECT_FALSE(NativeProjection::MirroredCamera(reflection.data(), far.data())) << pitch;
        }
        // At the eye, keeping what is below the water: behind its plane, level, right-handed.
        const Block below =
            WithPlane(SceneBlock(frame, false), ClipPlane(frame, {0, -1, 0}, -Water));
        EXPECT_TRUE(NativeProjection::MirroredCamera(below.data())) << pitch;
        EXPECT_FALSE(NativeProjection::MirroredCamera(below.data(), frame.position.data()))
            << pitch;
    }
}

// FAST_GPU_MIRROR_ABOVE: at the Yharnam sewer gate the game mirrors the eye about a plane 1.65 m
// above it, and the mirrored camera keeps what is above its plane. With the render camera it is
// still the eye's mirror image and needs the same flip; without one, or for the eye's own camera
// with that plane (its image is twice the distance away), it is refused.
TEST(MirrorFlip, ReflectionOnTheKeptSideOfItsPlane) {
    for (float pitch : {-30.f, -15.f, 0.f, 25.f}) {
        for (float roll : {0.f, 10.f}) {
            Pose frame = Frame(pitch, roll);
            frame.position[1] = Water - 1.652f;
            const Block reflection = SceneBlock(Reflection(frame), true);
            EXPECT_FALSE(NativeProjection::MirroredCamera(reflection.data(), frame.position.data()))
                << pitch << " " << roll;
            EXPECT_TRUE(
                NativeProjection::MirroredCamera(reflection.data(), frame.position.data(), true))
                << pitch << " " << roll;
            EXPECT_FALSE(NativeProjection::MirroredCamera(reflection.data(), nullptr, true))
                << pitch << " " << roll;
            const Block at_eye = WithPlane(SceneBlock(frame, false), ClipPlane(frame, {0, 1, 0}, Water));
            EXPECT_FALSE(NativeProjection::MirroredCamera(at_eye.data(), frame.position.data(), true))
                << pitch << " " << roll;
            // The usual reflection below its plane still passes with either_side.
            const Pose above = Frame(pitch, roll);
            EXPECT_TRUE(NativeProjection::MirroredCamera(SceneBlock(Reflection(above), true).data(),
                                                         above.position.data(), true))
                << pitch << " " << roll;
        }
    }
}

TEST(MirrorFlip, FlippedIsTheEyeFrustumUpsideDown) {
    for (int eye = 0; eye < 2; ++eye) {
        const auto fov = EyeFov(eye);
        const Transform t = NativeProjection::Make(fov, TanX, TanY, false);
        const Transform swapped =
            NativeProjection::Make({fov[0], fov[1], -fov[3], -fov[2]}, TanX, TanY, false);
        const Transform flipped = NativeProjection::Flipped(t);
        ASSERT_TRUE(t.valid && swapped.valid && flipped.valid);
        EXPECT_NEAR(flipped.x_scale, swapped.x_scale, 1e-6f);
        EXPECT_NEAR(flipped.y_scale, swapped.y_scale, 1e-6f);
        EXPECT_NEAR(flipped.x_offset, swapped.x_offset, 1e-6f);
        EXPECT_NEAR(flipped.y_offset, swapped.y_offset, 1e-6f);
        EXPECT_EQ(flipped.x_offset, t.x_offset); // x untouched
        EXPECT_NEAR(t.y_offset, .1932f, 1e-3f);  // Quest 3: 2 * y_offset of NDC was the error
    }
    // A vertically symmetric eye keeps its bits (y_offset may be -0).
    const Transform level = NativeProjection::Make({-.9f, .7f, .8f, -.8f}, TanX, TanY, false);
    const Transform flipped = NativeProjection::Flipped(level);
    EXPECT_EQ(std::memcmp(&level.y_offset, &flipped.y_offset, sizeof(float)), 0);
    EXPECT_TRUE(flipped == level);
}

// Each eye looks at the water at W, where the reflection of a point Q above it appears; the water
// shader reads the reflection image at W's screen position flipped, (x, -y) in NDC. The
// reflection camera of that eye, upside down, must have drawn Q exactly there, at any head roll
// (60 degrees too).
TEST(MirrorFlip, WaterReadLandsOnTheReflectionInEachEye) {
    for (float pitch : {-30.f, -20.f, 0.f, 20.f}) {
        for (float roll : {0.f, 8.f, 60.f}) {
            const Pose frame = Frame(pitch, roll);
            const Block center = SceneBlock(frame, false);
            const Block reflection = SceneBlock(Reflection(frame), true);
            ASSERT_TRUE(NativeProjection::MirroredCamera(reflection.data(), frame.position.data()));
            for (int eye = 0; eye < 2; ++eye) {
                const Transform t = NativeProjection::Make(EyeFov(eye), TanX, TanY, false);
                const Block view = EyeBlock(center, EyeShift(eye), t, false);
                const Block flipped =
                    EyeBlock(reflection, EyeShift(eye), NativeProjection::Flipped(t), true);
                const Block same = EyeBlock(reflection, EyeShift(eye), t, true);
                const Vec3 position{view[183], view[187], view[191]};
                int checked = 0;
                for (float ahead : {4.f, 9.f, 25.f}) {
                    for (float side : {-6.f, 0.f, 5.f}) {
                        for (float height : {.5f, 4.f, 15.f}) {
                            const Vec3 level{frame.rows[2][0], 0, frame.rows[2][2]};
                            Vec3 q = Add(Add(frame.position, level, ahead), frame.rows[0], side);
                            q[1] = Water + height;
                            // W: where the eye's ray to Q's mirror image meets the water.
                            const Vec3 image = Mirror(q);
                            const float s = (position[1] - Water) / (position[1] - image[1]);
                            const Vec3 w = Add(position, Add(image, position, -1), s);
                            const auto seen = Clip(view, w);
                            const auto drawn = Clip(flipped, q);
                            const auto old = Clip(same, q);
                            if (seen[3] <= .1f || drawn[3] <= .1f)
                                continue;
                            const float x = seen[0] / seen[3], y = seen[1] / seen[3];
                            // Float precision far off the eye image (NDC 38 at roll 60).
                            const float near = 1e-4f * std::max({1.f, std::abs(x), std::abs(y)});
                            EXPECT_NEAR(drawn[0] / drawn[3], x, near) << pitch << " " << eye;
                            EXPECT_NEAR(drawn[1] / drawn[3], -y, near) << pitch << " " << eye;
                            // The same eye frustum (before): 2 * y_offset off, in every eye.
                            EXPECT_NEAR(old[1] / old[3] - drawn[1] / drawn[3], 2 * t.y_offset,
                                        near);
                            ++checked;
                        }
                    }
                }
                EXPECT_GT(checked, 10) << pitch << " " << roll << " " << eye;
            }
        }
    }
}

// The water clip plane per eye: points on the water clip at 0 and every point keeps the distance
// the game's plane gives it from the reflection camera (y - Water), also with the head rolled,
// where the eye's move along its right axis crosses the plane.
TEST(MirrorFlip, ClipPlaneFollowsEachEye) {
    for (float pitch : {-20.f, 0.f, 15.f}) {
        for (float roll : {0.f, 12.f, -70.f}) {
            const Block reflection = SceneBlock(Reflection(Frame(pitch, roll)), true);
            const Pose pose = Reflection(Frame(pitch, roll));
            for (int eye = 0; eye < 2; ++eye) {
                const Transform t = NativeProjection::Flipped(
                    NativeProjection::Make(EyeFov(eye), TanX, TanY, false));
                const Block f = EyeBlock(reflection, EyeShift(eye), t, true);
                Block old = f; // before: the center's plane on the eye's clip position
                std::memcpy(old.data() + 196, reflection.data() + 196, 4 * sizeof(float));
                float worst_old = 0;
                for (float ahead : {-3.f, 2.f, 10.f}) {
                    for (float side : {-4.f, 3.f}) {
                        for (float height : {0.f, 1.f, 6.f}) {
                            Vec3 point =
                                Add(Add(pose.position, pose.rows[2], ahead), pose.rows[0], side);
                            point[1] = Water + height;
                            const auto clip = Clip(f, point);
                            EXPECT_NEAR(PlaneDistance(f, clip), height, 2e-3f * (1 + height))
                                << pitch << " " << roll << " " << eye;
                            worst_old =
                                std::max(worst_old, std::abs(PlaneDistance(old, clip) - height));
                        }
                    }
                }
                EXPECT_GT(worst_old, .05f); // the unmapped plane clipped elsewhere
            }
        }
    }
}

TEST(MirrorFlip, PassRunsFromTheMirroredCameraToTheNextSceneCamera) {
    NativeProjection::MirrorPass pass;
    const std::array<unsigned, 2> half{728, 788}, eye{1456, 1576};
    EXPECT_FALSE(pass.Contains(half, true));
    EXPECT_FALSE(pass.Camera(false, eye));    // the frame camera: no pass
    EXPECT_TRUE(pass.Camera(true, half));     // the reflection's G-buffer starts it
    EXPECT_FALSE(pass.Camera(true, half));    // its other draws keep it
    EXPECT_TRUE(pass.Contains(half, true));   // its lights, sky and composite
    EXPECT_FALSE(pass.Contains(half, false)); // a mono draw (a clear) is not of it
    EXPECT_FALSE(pass.Contains(eye, true));   // nor one of the eye images
    EXPECT_FALSE(pass.Camera(false, eye));    // the frame camera ends it
    EXPECT_FALSE(pass.Contains(half, true));  // the main pass's half-size passes are not of it
}

} // namespace
