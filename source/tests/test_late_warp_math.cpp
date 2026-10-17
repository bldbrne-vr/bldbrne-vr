// SPDX-License-Identifier: GPL-2.0-or-later

// Late warp math (renderer_vulkan/vr_late_warp_math.h): the epipolar line q(r) = q0 + r g and V's
// inverse depth against a direct projection of random points for random source and view poses, the
// reduction to the 1D DIBR row (R = I, c along x), the camera point against the game camera's own
// offsets (vr_session.cpp OnCameraTick, the render-camera hook), and the scan (March, the CPU
// reference of vr_late_reproject.comp) on ray-cast scenes: exact on planes, the nearest surface at
// depth edges, holes only where the source never saw the surface, and a ray passing behind a thin
// pole sees the wall beyond it. late_warp_motion: the body point and the world's motion between two
// camera updates against the game's own embedding of the world (head motion alone moves nothing),
// and the layered scans (the hunter's body where it was, the world moved on) on a ray-cast scene
// whose world moved while the body stayed.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vr_late_warp_math.h"

namespace {

namespace VL = Vulkan::VrLate;
namespace VS = Vulkan::VrStereo;

constexpr double Pi = 3.14159265358979323846;

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

// VR::QuatFromYawPitchRoll: yaw about +Y (+ = left), then pitch about +X (+ = up), then roll.
VL::Quat YawPitchRoll(float yaw, float pitch, float roll) {
    return Mul(Mul(AxisAngle(0, 1, 0, yaw), AxisAngle(1, 0, 0, pitch)), AxisAngle(0, 0, 1, roll));
}

struct Point2 {
    double x, y;
};

// Direct projection into S (camera frame point, continuous pixels).
Point2 Project(const VL::Intrinsics& in, const VL::Vec3& p) {
    const double z = -p.z;
    return {in.cx + in.fx * p.x / z, in.cy - in.fy * p.y / z};
}

TEST(LateWarpMath, LineMatchesDirectProjection) {
    std::mt19937 rng{7};
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const auto in = VL::MakeIntrinsics(122.0f * static_cast<float>(Pi) / 180.0f, 4.0f / 3.0f,
                                       2560.0f, 1920.0f);
    double max_q = 0.0;
    double max_w = 0.0;
    int checked = 0;
    for (int i = 0; i < 20000; ++i) {
        const VL::Quat q_s = YawPitchRoll(u(rng) * 3.0f, u(rng) * 1.2f, u(rng) * 0.5f);
        const VL::Quat q_v = Mul(q_s, YawPitchRoll(u(rng) * 0.15f, u(rng) * 0.15f, u(rng) * 0.15f));
        const VL::Vec3 c_s{u(rng) * 0.3f, 1.6f + u(rng) * 0.3f, u(rng) * 0.3f};
        const VL::Vec3 c_v = c_s + VL::Vec3{u(rng) * 0.06f, u(rng) * 0.06f, u(rng) * 0.06f};
        const float upm = 1.0f + 0.5f * (u(rng) + 1.0f);
        const auto rp = VL::MakeReprojection(q_s, c_s, q_v, c_v, upm);
        const float px = 1280.0f + 1200.0f * u(rng);
        const float py = 960.0f + 900.0f * u(rng);
        const auto line = VL::LineFor(rp, in, px, py);
        if (!line.valid) {
            continue;
        }
        // The point of V's ray at V depth 1/w (V camera frame, game units), into S's frame.
        const float w = std::exp(u(rng) * 4.0f); // 1/55 .. 55 per unit
        const VL::Vec3 dv{(px - in.cx) / in.fx, -(py - in.cy) / in.fy, -1.0f};
        const VL::Vec3 point = rp.c + (rp.rsv * dv) * (1.0f / w);
        if (-point.z < 1e-2f) {
            continue;
        }
        const float r = 1.0f / -point.z;
        const Point2 direct = Project(in, point);
        max_q = std::max({max_q, std::abs(direct.x - (line.q0x + r * line.gx)),
                          std::abs(direct.y - (line.q0y + r * line.gy))});
        max_w = std::max(max_w,
                         std::abs(VL::ViewInverseDepth(line, rp.c.z, r) - w) / static_cast<double>(w));
        ++checked;
    }
    EXPECT_GT(checked, 15000);
    EXPECT_LT(max_q, 0.02);  // pixels (float: 2560 px wide)
    EXPECT_LT(max_w, 1e-3);  // relative
}

TEST(LateWarpMath, IdentityAndTheDibrRow) {
    const auto in = VL::MakeIntrinsics(1.2f, 4.0f / 3.0f, 2560.0f, 1920.0f);
    const VL::Quat q = YawPitchRoll(0.4f, -0.2f, 0.0f);
    const VL::Vec3 c{0.1f, 1.5f, -0.3f};
    // Same pose: every r maps to the pixel itself, V's inverse depth equals S's.
    const auto same = VL::MakeReprojection(q, c, q, c, 1.0f);
    for (const float px : {0.5f, 731.25f, 2559.5f}) {
        for (const float py : {0.5f, 960.0f, 1919.5f}) {
            const auto line = VL::LineFor(same, in, px, py);
            EXPECT_NEAR(line.q0x, px, 2e-3f);
            EXPECT_NEAR(line.q0y, py, 2e-3f);
            EXPECT_NEAR(line.gx, 0.0f, 1e-6f);
            EXPECT_NEAR(line.gy, 0.0f, 1e-6f);
            EXPECT_NEAR(VL::ViewInverseDepth(line, same.c.z, 0.7f), 0.7f, 1e-5f);
        }
    }
    // V half the baseline to the right (the right eye): g = (fx b, 0), q = p + K r: the 1D DIBR
    // scan's source column for an eye pixel (right eye: x - D in the eye is x in the source).
    const float b = 0.0225f;
    const auto right = VL::MakeReprojection(q, c, q, c + VL::FromQuat(q) * VL::Vec3{b, 0, 0}, 1.0f);
    const auto line = VL::LineFor(right, in, 1000.5f, 700.5f);
    EXPECT_NEAR(line.gx, in.fx * b, 1e-3f);
    EXPECT_NEAR(line.gy, 0.0f, 1e-4f);
    VS::CameraParams cam{};
    cam.vfov = 1.2f;
    cam.aspect = 4.0f / 3.0f;
    cam.half_ipd = b;
    const auto disparity = VS::MakeDisparityParams(cam, 2560.0f, 0.0f, 0.0f);
    EXPECT_NEAR(line.gx, disparity.k_px, 1e-3f);
}

TEST(LateWarpMath, CameraPointIsTheGameCameraOffset) {
    // OnCameraTick: the head's clamped offset from the origin goes into game axes through the
    // heading b (right = (cos b, 0, -sin b), forward = (sin b, 0, cos b)), the first-person eye
    // fp_forward along the view yaw (b - head yaw) is added by the render-camera hook. The camera
    // point in tracking space, turned into game axes the same way, must be that offset.
    std::mt19937 rng{3};
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (int i = 0; i < 2000; ++i) {
        const VL::Vec3 origin{u(rng) * 0.5f, 1.6f + u(rng) * 0.2f, u(rng) * 0.5f};
        const VL::Vec3 head = origin + VL::Vec3{u(rng) * 0.7f, u(rng) * 0.4f, u(rng) * 0.7f};
        const float head_yaw = u(rng) * 3.1f;
        const float base = u(rng) * 3.1f;
        const float limit = 0.5f;
        const float forward = 0.1f;
        const float scale = 1.0f + u(rng) * 0.2f;
        // The game's way.
        VL::Vec3 d = head - origin;
        const float length = VL::Length(d);
        if (length > limit) {
            d = d * (limit / length);
        }
        const float cb = std::cos(base), sb = std::sin(base);
        const float r = d.x * scale, f = -d.z * scale;
        const float view_yaw = base - head_yaw;
        const VL::Vec3 game{cb * r + sb * f + std::sin(view_yaw) * forward * scale,
                            d.y * scale, -sb * r + cb * f + std::cos(view_yaw) * forward * scale};
        // Ours, into game axes: tracking (x, y, z) -> right x, forward -z.
        const VL::Vec3 t = VL::CameraPoint(head, head_yaw, origin, limit, forward);
        const float tr = t.x * scale, tf = -t.z * scale;
        EXPECT_NEAR(cb * tr + sb * tf, game.x, 1e-4f);
        EXPECT_NEAR(t.y * scale, game.y, 1e-4f);
        EXPECT_NEAR(-sb * tr + cb * tf, game.z, 1e-4f);
    }
    // positional=0: the head never moves the camera (only the eye forward of the view yaw).
    const VL::Vec3 p = VL::CameraPoint({0.3f, 1.9f, -0.2f}, 0.0f, {0, 1.6f, 0}, 0.0f, 0.1f);
    EXPECT_NEAR(p.x, 0.0f, 1e-6f);
    EXPECT_NEAR(p.y, 0.0f, 1e-6f);
    EXPECT_NEAR(p.z, -0.1f, 1e-6f);
}

TEST(LateWarpMath, DisparityOfInverseDepthIsThePrepPass) {
    VS::CameraParams cam{};
    cam.near_z = 0.1f;
    cam.far_z = 3000.0f;
    cam.vfov = 122.0f * static_cast<float>(Pi) / 180.0f;
    cam.aspect = 4.0f / 3.0f;
    cam.half_ipd = 0.0225f;
    const auto p = VS::MakeDisparityParams(cam, 2560.0f, 0.6f, 0.3f);
    for (const double z : {0.1, 0.2, 0.31, 0.6, 1.0, 2.5, 40.0, 2999.0}) {
        const float d = static_cast<float>(VS::StoredDepthFromViewDepth(z, cam));
        EXPECT_NEAR(VL::DisparityOfInverseDepth(static_cast<float>(1.0 / z), p),
                    VS::DepthToDisparity(d, p), 2e-3f);
    }
    EXPECT_EQ(VL::DisparityOfInverseDepth(0.0f, p), 0.0f);
}

// ---- ray-cast scenes ----

struct Box {
    VL::Vec3 lo, hi;
};

// S's camera frame, metres: a wall 5 m away tilted toward the floor, open sky above y = 2.5, a
// "hand" box 0.35 - 0.45 m in front of the eye, a 3 cm pole at 1.2 m, a crate at 2.2 m.
struct Scene {
    std::vector<Box> boxes{
        {{-0.10f, -0.25f, -0.45f}, {0.06f, -0.10f, -0.35f}},
        {{0.20f, -3.0f, -1.215f}, {0.23f, 3.0f, -1.185f}},
        {{-1.2f, -1.4f, -2.6f}, {-0.6f, -0.6f, -2.2f}},
    };
    bool wall{true};

    // The nearest hit along origin + t dir (t > 0): distance t, or -1 for the sky.
    float Cast(const VL::Vec3& o, const VL::Vec3& d) const {
        float best = -1.0f;
        const auto take = [&](float t) {
            if (t > 1e-4f && (best < 0.0f || t < best)) {
                best = t;
            }
        };
        // Wall: n . X = n . P0, n = (0, 0.25, 1) normalised, P0 = (0, 0, -5); only below y = 2.5.
        const VL::Vec3 n{0.0f, 0.2425356f, 0.9701425f};
        const float denom = VL::Dot(n, d);
        if (wall && std::abs(denom) > 1e-6f) {
            const float t = (VL::Dot(n, VL::Vec3{0, 0, -5}) - VL::Dot(n, o)) / denom;
            if (t > 0.0f && o.y + t * d.y < 2.5f) {
                take(t);
            }
        }
        for (const Box& b : boxes) {
            float t0 = 0.0f, t1 = 1e30f;
            bool ok = true;
            const float os[3]{o.x, o.y, o.z};
            const float ds[3]{d.x, d.y, d.z};
            const float lo[3]{b.lo.x, b.lo.y, b.lo.z};
            const float hi[3]{b.hi.x, b.hi.y, b.hi.z};
            for (int k = 0; k < 3 && ok; ++k) {
                if (std::abs(ds[k]) < 1e-9f) {
                    ok = os[k] >= lo[k] && os[k] <= hi[k];
                    continue;
                }
                float ta = (lo[k] - os[k]) / ds[k];
                float tb = (hi[k] - os[k]) / ds[k];
                if (ta > tb) {
                    std::swap(ta, tb);
                }
                t0 = std::max(t0, ta);
                t1 = std::min(t1, tb);
                ok = t0 <= t1;
            }
            if (ok) {
                take(t0);
            }
        }
        return best;
    }
};

struct Render {
    int width, height;
    VL::Intrinsics in;
    std::vector<float> w; // 1/z per texel, 0 = sky
    float Fetch(int x, int y) const {
        return w[static_cast<size_t>(y) * width + x];
    }
};

Render RenderSource(const Scene& scene, int width, int height) {
    Render r{width, height, VL::MakeIntrinsics(1.2f, 4.0f / 3.0f, static_cast<float>(width),
                                               static_cast<float>(height)),
             {}};
    r.w.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const VL::Vec3 d{(x + 0.5f - r.in.cx) / r.in.fx, -(y + 0.5f - r.in.cy) / r.in.fy, -1.0f};
            const float t = scene.Cast({0, 0, 0}, d);
            r.w[static_cast<size_t>(y) * width + x] = t > 0.0f ? 1.0f / t : 0.0f;
        }
    }
    return r;
}

struct ScanStats {
    int visible{}, visible_ok{}, visible_far{};  // far from any edge: within 0.05 px
    int visible_edge{}, visible_edge_ok{};        // near an edge: within 1.5 px
    int hidden{}, hidden_hole{};
    int sky{}, sky_ok{};         // sky S saw at the rotation-only point: found there
    int sky_hidden{}, sky_hole{}; // sky S had something in front of: a hole
    double max_far_err{};
};

ScanStats Scan(const Scene& scene, const Render& src, const VL::Quat& q_v, const VL::Vec3& c_v) {
    ScanStats st{};
    const auto rp = VL::MakeReprojection({}, {}, q_v, c_v, 1.0f);
    float lo = 1e30f, hi = 0.0f;
    for (float v : src.w) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const float tau = 1.0f / 11.97f; // 1 px of the game's stereo disparity (K = 11.97 px m)
    const auto fetch = [&](int x, int y) { return src.Fetch(x, y); };
    // The source's depth edges: a texel whose 5x5 neighbourhood jumps by more than tau.
    const auto near_edge = [&](double qx, double qy) {
        const int cx = static_cast<int>(std::floor(qx)), cy = static_cast<int>(std::floor(qy));
        float a = 1e30f, b = 0.0f;
        for (int y = cy - 2; y <= cy + 2; ++y) {
            for (int x = cx - 2; x <= cx + 2; ++x) {
                const float v =
                    src.Fetch(std::clamp(x, 0, src.width - 1), std::clamp(y, 0, src.height - 1));
                a = std::min(a, v);
                b = std::max(b, v);
            }
        }
        return b - a > tau;
    };
    for (int y = 4; y < src.height - 4; y += 3) {
        for (int x = 4; x < src.width - 4; x += 3) {
            const float px = x + 0.5f, py = y + 0.5f;
            const auto line = VL::LineFor(rp, src.in, px, py);
            if (!line.valid) {
                continue;
            }
            const VL::Hit h = VL::March(line, fetch, src.width, src.height, lo * 0.99f,
                                        hi * 1.01f + 1e-4f, tau);
            // Ground truth: V's own ray.
            const VL::Vec3 dv{(px - src.in.cx) / src.in.fx, -(py - src.in.cy) / src.in.fy, -1.0f};
            const VL::Vec3 d = rp.rsv * dv;
            const float t = scene.Cast(rp.c, d);
            if (t < 0.0f) {
                // The sky is at infinity: the rotation-only point, inverse depth 0, where S saw
                // the sky there; else S had something in front of it (above the wall's top edge).
                // (Within 2 px of a silhouette: neither, not counted.)
                bool sky_seen = true;   // all of the 5x5 texels around it
                bool sky_hidden = true; // none of the 3x3
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int sx = std::clamp(static_cast<int>(line.q0x) + dx, 0, src.width - 1);
                        const int sy = std::clamp(static_cast<int>(line.q0y) + dy, 0, src.height - 1);
                        const bool sky_texel = src.Fetch(sx, sy) == 0.0f;
                        sky_seen = sky_seen && sky_texel;
                        sky_hidden = sky_hidden && (!sky_texel || std::abs(dx) == 2 ||
                                                    std::abs(dy) == 2);
                    }
                }
                if (!sky_seen && !sky_hidden) {
                    continue;
                }
                if (sky_seen) {
                    ++st.sky;
                    st.sky_ok += h.r == 0.0f && std::abs(h.qx - line.q0x) < 0.6f &&
                                 std::abs(h.qy - line.q0y) < 0.6f;
                } else {
                    ++st.sky_hidden;
                    st.sky_hole += h.kind == VL::Kind::Hole;
                    static int printed = 0;
                    if (h.kind != VL::Kind::Hole && printed++ < 12 &&
                        std::getenv("LATE_TEST_VERBOSE") != nullptr) {
                        std::printf("    sky miss: v(%.1f,%.1f) hit (%.2f,%.2f) r %.4f kind %d "
                                    "steps %u line q0 (%.2f,%.2f) g (%.2f,%.2f) W(q0) %.4f\n",
                                    px, py, h.qx, h.qy, h.r, static_cast<int>(h.kind), h.steps,
                                    line.q0x, line.q0y, line.gx, line.gy,
                                    src.Fetch(std::clamp(static_cast<int>(line.q0x), 0, src.width - 1),
                                              std::clamp(static_cast<int>(line.q0y), 0,
                                                         src.height - 1)));
                    }
                }
                continue;
            }
            const VL::Vec3 point = rp.c + d * t;
            const Point2 truth = Project(src.in, point);
            if (truth.x < 2 || truth.y < 2 || truth.x > src.width - 2 || truth.y > src.height - 2) {
                continue;
            }
            // Seen by S: S's ray toward it hits it first.
            const float dist = VL::Length(point);
            const float s_hit = scene.Cast({0, 0, 0}, point * (1.0f / dist));
            const bool seen = s_hit > 0.0f && s_hit > dist - 2e-3f * dist;
            if (!seen) {
                ++st.hidden;
                st.hidden_hole += h.kind == VL::Kind::Hole;
                continue;
            }
            ++st.visible;
            const double err = std::max(std::abs(h.qx - truth.x), std::abs(h.qy - truth.y));
            if (near_edge(truth.x - 0.5, truth.y - 0.5)) {
                ++st.visible_edge;
                st.visible_edge_ok += err < 1.5;
            } else {
                ++st.visible_far;
                // Within 0.1 px: bilinear 1/z is exact on planes and close across a crease (a
                // box's corner: two planes at one depth).
                st.visible_ok += err < 0.1 && h.kind == VL::Kind::Surface;
                st.max_far_err = std::max(st.max_far_err, err);
                static int printed = 0;
                if ((err >= 0.1 || h.kind != VL::Kind::Surface) && printed++ < 12 &&
                    std::getenv("LATE_TEST_VERBOSE") != nullptr) {
                    std::printf("    far miss: v(%.1f,%.1f) truth (%.2f,%.2f) r %.4f hit (%.2f,%.2f) "
                                "r %.4f kind %d steps %u line q0 (%.2f,%.2f) g (%.2f,%.2f)\n",
                                px, py, truth.x, truth.y, 1.0f / -point.z, h.qx, h.qy, h.r,
                                static_cast<int>(h.kind), h.steps, line.q0x, line.q0y, line.gx,
                                line.gy);
                }
            }
        }
    }
    return st;
}

TEST(LateWarpMath, MarchOnRayCastScenes) {
    const Scene scene{};
    const Render src = RenderSource(scene, 640, 480);
    std::mt19937 rng{11};
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    ScanStats total{};
    for (int i = 0; i < 12; ++i) {
        // The head 1 - 4 cm away, turned up to 3 degrees (a refresh or two of fast motion).
        const VL::Vec3 c_v{u(rng) * 0.04f, u(rng) * 0.03f, u(rng) * 0.03f};
        const VL::Quat q_v = YawPitchRoll(u(rng) * 0.05f, u(rng) * 0.05f, u(rng) * 0.05f);
        const ScanStats st = Scan(scene, src, q_v, c_v);
        total.visible += st.visible;
        total.visible_far += st.visible_far;
        total.visible_ok += st.visible_ok;
        total.visible_edge += st.visible_edge;
        total.visible_edge_ok += st.visible_edge_ok;
        total.hidden += st.hidden;
        total.hidden_hole += st.hidden_hole;
        total.sky += st.sky;
        total.sky_ok += st.sky_ok;
        total.sky_hidden += st.sky_hidden;
        total.sky_hole += st.sky_hole;
        total.max_far_err = std::max(total.max_far_err, st.max_far_err);
    }
    std::printf("  march: %d visible (%d away from edges, %d within 0.1 px, max err %.4f px; %d "
                "near edges, %d within 1.5 px), %d hidden in the source (%d holes), %d sky (%d "
                "ok), %d sky S did not see (%d holes)\n",
                total.visible, total.visible_far, total.visible_ok, total.max_far_err,
                total.visible_edge, total.visible_edge_ok, total.hidden, total.hidden_hole,
                total.sky, total.sky_ok, total.sky_hidden, total.sky_hole);
    EXPECT_GT(total.visible_far, 100000);
    // Planes exact; a ray grazing a nearer surface's half-pixel footprint takes it (1 in 5000).
    EXPECT_GE(total.visible_ok, total.visible_far - total.visible_far / 5000);
    EXPECT_GE(total.visible_edge_ok, total.visible_edge * 97 / 100);
    EXPECT_GT(total.hidden, 50);
    EXPECT_GE(total.hidden_hole, total.hidden * 95 / 100);
    EXPECT_GT(total.sky, 1000);
    EXPECT_GE(total.sky_ok, total.sky - total.sky / 5000);
    EXPECT_GE(total.sky_hole, total.sky_hidden * 95 / 100);
}

TEST(LateWarpMath, BehindAThinPoleSeesTheWall) {
    // A thin pole (8 mm) a hand's length from the eye and a head 4 cm to its left: for wall points
    // both cameras see, V's ray passes behind the pole as S sees it (its line in S crosses the
    // pole's footprint while farther than the pole), then comes out in front of the wall. The scan
    // goes on behind the pole and finds the wall, never a hole and never the pole.
    Scene scene{};
    scene.boxes = {{{0.020f, -3.0f, -0.360f}, {0.028f, 3.0f, -0.352f}}};
    const Render src = RenderSource(scene, 640, 480);
    const auto rp = VL::MakeReprojection({}, {}, {}, {-0.04f, 0.0f, 0.0f}, 1.0f);
    float lo = 1e30f, hi = 0.0f;
    for (float v : src.w) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const auto fetch = [&](int x, int y) { return src.Fetch(x, y); };
    const float pole_w = 1.0f / 0.356f;
    int behind = 0, ok = 0;
    for (int x = 280; x < 480; ++x) {
        const float px = x + 0.5f, py = 240.5f;
        const auto line = VL::LineFor(rp, src.in, px, py);
        const VL::Vec3 d = rp.rsv * VL::Vec3{(px - src.in.cx) / src.in.fx,
                                             -(py - src.in.cy) / src.in.fy, -1.0f};
        const float t = scene.Cast(rp.c, d);
        if (t < 0.0f) {
            continue;
        }
        const VL::Vec3 point = rp.c + d * t;
        const float dist = VL::Length(point);
        const bool seen = scene.Cast({0, 0, 0}, point * (1.0f / dist)) > dist * 0.998f;
        if (!seen || -point.z < 3.0f) {
            continue; // the wall, seen by both
        }
        // Does the line pass over the pole's footprint farther than the pole?
        bool crosses = false;
        for (float r = 1.0f / -point.z; r < pole_w && !crosses; r += 0.002f) {
            const int qx = static_cast<int>(std::floor(line.q0x + r * line.gx));
            const int qy = static_cast<int>(std::floor(line.q0y + r * line.gy));
            crosses = qx >= 0 && qx < src.width && src.Fetch(qx, qy) > 2.5f;
        }
        if (!crosses) {
            continue;
        }
        ++behind;
        const Point2 truth = Project(src.in, point);
        const VL::Hit h = VL::March(line, fetch, src.width, src.height, lo * 0.99f, hi * 1.01f,
                                    1.0f / 11.97f);
        ok += h.kind == VL::Kind::Surface && std::abs(h.qx - truth.x) < 0.05 &&
              std::abs(h.r - 1.0f / -point.z) < 1e-3f;
    }
    std::printf("  thin pole: %d wall pixels seen past it, %d found\n", behind, ok);
    EXPECT_GT(behind, 5);
    EXPECT_EQ(ok, behind);
}

// ---- late_warp_motion ----

// The game's way (OnCameraTick, the render-camera hook): a camera update with body point `body`
// (game units), heading `base`, head `head` (tracking, absolute) renders from
//     P = body + A(base) CameraPoint(head, ...) s
// and a world point W appears in tracking space (origin-relative) at c + A(base) (W - P) / s.
struct GameUpdate {
    VL::Vec3 body;
    float base;
    VL::Vec3 head;
    float head_yaw;
    VL::Vec3 origin;
    float limit, forward, scale;

    VL::Vec3 Camera() const {
        return VL::CameraPoint(head, head_yaw, origin, limit, forward);
    }
    VL::Vec3 RenderPosition() const {
        const VL::Vec3 c = Camera();
        const float cb = std::cos(base), sb = std::sin(base);
        const float r = c.x * scale, f = -c.z * scale;
        return body + VL::Vec3{cb * r + sb * f, c.y * scale, -sb * r + cb * f};
    }
    VL::Vec3 Tracking(const VL::Vec3& w) const {
        return Camera() + VL::GameFromTracking(base, w - RenderPosition()) * (1.0f / scale);
    }
};

TEST(LateWarpMath, BodyPointAndHeadOnlyMotion) {
    std::mt19937 rng{21};
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    double max_body = 0.0, max_still = 0.0;
    for (int i = 0; i < 5000; ++i) {
        GameUpdate a{{u(rng) * 300.0f, u(rng) * 50.0f, u(rng) * 300.0f}, u(rng) * 3.1f,
                     {u(rng) * 0.5f, 1.6f + u(rng) * 0.2f, u(rng) * 0.5f}, u(rng) * 3.1f,
                     {u(rng) * 0.3f, 1.6f, u(rng) * 0.3f}, 0.4f, 0.1f, 1.0f + 0.3f * u(rng)};
        // The body point is what the hook started from.
        const VL::Vec3 b = VL::BodyPoint(a.RenderPosition(), a.base, a.Camera(), a.scale);
        max_body = std::max(max_body, static_cast<double>(VL::Length(b - a.body)));
        // Only the head moved (and turned): no motion of the world.
        GameUpdate n = a;
        n.head = a.head + VL::Vec3{u(rng) * 0.2f, u(rng) * 0.1f, u(rng) * 0.2f};
        n.head_yaw = a.head_yaw + u(rng) * 0.5f;
        const VL::Vec3 bn = VL::BodyPoint(n.RenderPosition(), n.base, n.Camera(), n.scale);
        const VL::Motion m = VL::MotionBetween(a.base, b, n.base, bn, 1.0f, a.scale);
        max_still = std::max({max_still, static_cast<double>(VL::Length(m.t)),
                              static_cast<double>(std::abs(m.yaw))});
    }
    EXPECT_LT(max_body, 2e-3);  // game units, positions up to 300
    EXPECT_LT(max_still, 1e-4); // metres / radians (float: body points up to 300 units)
}

TEST(LateWarpMath, MotionMovesTheWorldLikeTheGame) {
    // Walking and turning between two updates: a world point seen at x in the first update's
    // tracking embedding is seen at Apply(MotionBetween(alpha = 1), x) in the second's; alpha 0
    // moves nothing, and alpha 0.5 lies between.
    std::mt19937 rng{23};
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    double max_err = 0.0, max_zero = 0.0, max_half = 0.0;
    for (int i = 0; i < 5000; ++i) {
        const float scale = 1.0f + 0.3f * u(rng);
        GameUpdate a{{u(rng) * 300.0f, u(rng) * 50.0f, u(rng) * 300.0f}, u(rng) * 3.1f,
                     {u(rng) * 0.5f, 1.6f + u(rng) * 0.2f, u(rng) * 0.5f}, u(rng) * 3.1f,
                     {u(rng) * 0.3f, 1.6f, u(rng) * 0.3f}, 0.4f, 0.1f, scale};
        GameUpdate n = a;
        n.body = a.body + VL::Vec3{u(rng) * 0.2f, u(rng) * 0.05f, u(rng) * 0.2f} * scale;
        n.base = a.base + u(rng) * 0.1f;
        n.head = a.head + VL::Vec3{u(rng) * 0.02f, u(rng) * 0.02f, u(rng) * 0.02f};
        n.head_yaw = a.head_yaw + u(rng) * 0.05f;
        const VL::Vec3 ba = VL::BodyPoint(a.RenderPosition(), a.base, a.Camera(), scale);
        const VL::Vec3 bn = VL::BodyPoint(n.RenderPosition(), n.base, n.Camera(), scale);
        const VL::Motion full = VL::MotionBetween(a.base, ba, n.base, bn, 1.0f, scale);
        const VL::Motion none = VL::MotionBetween(a.base, ba, n.base, bn, 0.0f, scale);
        const VL::Motion half = VL::MotionBetween(a.base, ba, n.base, bn, 0.5f, scale);
        for (int k = 0; k < 4; ++k) {
            const VL::Vec3 w = a.RenderPosition() +
                               VL::Vec3{u(rng) * 20.0f, u(rng) * 5.0f, u(rng) * 20.0f};
            const VL::Vec3 xa = a.Tracking(w);
            const VL::Vec3 xn = n.Tracking(w);
            max_err = std::max(max_err, static_cast<double>(VL::Length(VL::Apply(full, xa) - xn)));
            max_zero = std::max(max_zero, static_cast<double>(VL::Length(VL::Apply(none, xa) - xa)));
            // Half way: within the two ends' span (the chord's midpoint, up to the turn's sagitta).
            const VL::Vec3 mid = (xa + xn) * 0.5f;
            const float span = VL::Length(xn - xa);
            const double off = VL::Length(VL::Apply(half, xa) - mid);
            max_half = std::max(max_half, off / std::max(span, 1e-3f));
        }
    }
    EXPECT_LT(max_err, 1e-3);  // metres, points up to 20 m away
    EXPECT_LT(max_zero, 1e-6);
    EXPECT_LT(max_half, 0.15); // of the point's own motion
}

TEST(LateWarpMath, MoveCameraKeepsTheWorldInView) {
    // A camera moved with the world sees the moved world exactly as the camera saw the world.
    std::mt19937 rng{29};
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    double max_err = 0.0;
    for (int i = 0; i < 2000; ++i) {
        const VL::Motion m{u(rng) * 0.3f, {u(rng) * 0.3f, u(rng) * 0.1f, u(rng) * 0.3f}};
        VL::Quat q = YawPitchRoll(u(rng) * 3.0f, u(rng) * 1.0f, u(rng) * 0.3f);
        VL::Vec3 c{u(rng), 1.0f + u(rng) * 0.3f, u(rng)};
        const VL::Vec3 x{u(rng) * 10.0f, u(rng) * 3.0f, u(rng) * 10.0f};
        const VL::Vec3 before = VL::Transpose(VL::FromQuat(q)) * (x - c);
        VL::MoveCamera(m, q, c);
        const VL::Vec3 after = VL::Transpose(VL::FromQuat(q)) * (VL::Apply(m, x) - c);
        max_err = std::max(max_err, static_cast<double>(VL::Length(after - before)));
    }
    EXPECT_LT(max_err, 1e-4);
}

TEST(LateWarpMath, EvaluatePathFitsTheGameCamera) {
    // Steady motion: exact anywhere (between updates and one step past the newest).
    std::array<VL::PathPoint, 5> pts{};
    for (int i = 0; i < 5; ++i) {
        const float t = -16.7f + 16.7f * i + (i == 2 ? 0.8f : 0.0f); // uneven ticks
        pts[i] = {t, {1.0f + 0.004f * t, 50.0f, -3.0f - 0.002f * t}, 0.3f + 0.002f * t};
    }
    double max_err = 0.0;
    for (float t = 0.0f; t <= 66.0f; t += 1.3f) {
        const VL::PathPoint at = VL::EvaluatePath(pts.data(), 5, t, 2.0f * 16.7f);
        max_err = std::max({max_err, static_cast<double>(std::abs(at.body.x - (1.0f + 0.004f * t))),
                            static_cast<double>(std::abs(at.body.z - (-3.0f - 0.002f * t))),
                            static_cast<double>(std::abs(at.base - (0.3f + 0.002f * t)))});
    }
    EXPECT_LT(max_err, 1e-5);
    // Standing still: exactly still.
    for (auto& p : pts) {
        p.body = {123.25f, 50.0f, -7.5f};
        p.base = 1.0f;
    }
    const VL::PathPoint still = VL::EvaluatePath(pts.data(), 5, 9.0f, 33.4f);
    EXPECT_NEAR(still.body.x, 123.25f, 1e-4f);
    EXPECT_NEAR(still.base, 1.0f, 1e-6f);
    // A smooth turn at 120 deg/s whose input is taken at 120 Hz XR frames, the camera ticking at
    // 60 fps with a little jitter: each update's heading is off by up to half an XR frame of
    // turning. The fit shows the turn steadier than the updates themselves.
    std::mt19937 rng{37};
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const float rate = 2.0944f / 1000.0f; // rad per ms
    double raw = 0.0, fit = 0.0;
    int samples = 0;
    for (int trial = 0; trial < 200; ++trial) {
        std::array<VL::PathPoint, 5> turn{};
        for (int i = 0; i < 5; ++i) {
            const float t = -16.67f + 16.67f * i + u(rng) * 1.5f;
            const float xr = std::floor((t + 1000.0f) / 8.333f) * 8.333f - 1000.0f; // last XR frame
            turn[i] = {t, {}, rate * xr};
        }
        for (float t = 0.0f; t < 33.0f; t += 8.333f) {
            const VL::PathPoint at = VL::EvaluatePath(turn.data(), 5, t, 2.0f * 16.67f);
            const VL::PathPoint lin = VL::EvaluatePath(turn.data(), 5, t, 1e-3f); // two nearest
            const double truth = rate * (t - 4.17f); // the staircase's mean lag
            fit += (at.base - truth) * (at.base - truth);
            raw += (lin.base - truth) * (lin.base - truth);
            ++samples;
        }
    }
    std::printf("  smooth turn: heading error rms %.3f deg interpolated, %.3f deg fitted\n",
                std::sqrt(raw / samples) * 57.2958, std::sqrt(fit / samples) * 57.2958);
    EXPECT_LT(fit, raw * 0.6);
}

// The camera 11 cm under its nominal height (the reviewer's broken-floor walk, median -11.1 cm):
// the floor round the hunter's feet is that much nearer the camera. With the dip it stays below
// the floor plane (world) right up to the legs' capsule; without it it was above (body).
TEST(LateWarpMath, FloorFollowsTheCameraDip) {
    EXPECT_NEAR(VL::SolesY(1.60f, false, -0.11f), -1.60f, 1e-6f);
    EXPECT_NEAR(VL::SolesY(1.60f, true, -0.11f), -1.49f, 1e-6f);
    EXPECT_NEAR(VL::SolesY(1.60f, true, 0.07f), -1.67f, 1e-6f);
    EXPECT_NEAR(VL::SolesY(0.40f, true, -0.30f), -0.50f, 1e-6f); // never nearer than 0.5 m
    EXPECT_NEAR(VL::SolesY(1.60f, true, std::nanf("")), -1.60f, 1e-6f);
    const auto legs = [](float soles_y) {
        VL::BodyShapes b{};
        VL::AddCapsule(b, {0.0f, soles_y + 0.95f, 0.0f}, {0.0f, soles_y, 0.0f}, 0.40f);
        b.floor_n = {0.0f, 1.0f, 0.0f};
        b.floor_d = -(soles_y + 0.08f); // LateBodyFloorM over the soles
        return b;
    };
    // The real floor 1.49 m under the camera, 0.3 m in front of the feet.
    const VL::Vec3 floor{0.0f, -1.49f, -0.30f};
    EXPECT_TRUE(VL::InBody(legs(VL::SolesY(1.60f, false, 0.0f)), floor));  // the old plane
    EXPECT_FALSE(VL::InBody(legs(VL::SolesY(1.60f, true, -0.11f)), floor)); // with the dip
    // His shin 0.3 m up stays body.
    EXPECT_TRUE(VL::InBody(legs(VL::SolesY(1.60f, true, -0.11f)), {0.0f, -1.19f, -0.25f}));
}

// An arm 0.5 m in front of the camera, a wall 0.12 m behind it (the reviewer's strafe along the
// clinic's panelling): the wall round the arm stays the world, the arm's own visible side is body.
TEST(LateWarpMath, OnlyTheArmsSideTowardTheCamera) {
    VL::BodyShapes b{};
    // A forearm held across the body in front of the chest (a guard).
    const VL::Vec3 shoulder{0.25f, -0.25f, -0.40f};
    const VL::Vec3 grip{-0.15f, -0.25f, -0.45f};
    VL::AddCapsule(b, shoulder, grip, 0.18f, 0.10f);
    const auto unit = [](const VL::Vec3& v) { return v * (1.0f / VL::Length(v)); };
    EXPECT_NEAR(b.capsules[0].behind, 0.10f, 1e-6f);
    // The forearm's surface facing the camera (5 cm off the axis toward it) and its silhouette.
    const VL::Vec3 axis = VL::ClosestOnSegment({0.05f, -0.25f, -0.42f}, shoulder, grip);
    EXPECT_TRUE(VL::InBody(b, axis - unit(axis) * 0.05f));
    EXPECT_TRUE(VL::InBody(b, axis + VL::Vec3{0.06f, 0.0f, 0.0f}));
    // An elbow bulging 8 cm away from the camera off the line: still body (behind 0.10).
    EXPECT_TRUE(VL::InBody(b, axis + unit(axis) * 0.08f));
    // The wall 12 - 17 cm behind the arm, inside the old capsule: the world.
    EXPECT_FALSE(VL::InBody(b, axis + unit(axis) * 0.12f));
    EXPECT_FALSE(VL::InBody(b, axis + unit(axis) * 0.17f));
    // A hull capsule (no limit given) keeps its whole radius.
    VL::BodyShapes hull{};
    VL::AddCapsule(hull, shoulder, grip, 0.18f);
    EXPECT_NEAR(hull.capsules[0].behind, 0.18f, 1e-6f);
    EXPECT_TRUE(VL::InBody(hull, axis + unit(axis) * 0.17f));
}

TEST(LateWarpMath, BodyShapes) {
    VL::BodyShapes shapes{};
    VL::AddCapsule(shapes, {0, 0, 0}, {0, 0, 0}, 0.45f);
    VL::AddCapsule(shapes, {0.2f, -0.2f, -0.1f}, {0.25f, -0.3f, -0.6f}, 0.14f);
    EXPECT_EQ(shapes.count, 2u);
    EXPECT_TRUE(VL::InBody(shapes, {0.1f, 0.2f, -0.3f}));     // round the eye
    EXPECT_TRUE(VL::InBody(shapes, {0.30f, -0.30f, -0.55f})); // along the arm
    EXPECT_FALSE(VL::InBody(shapes, {0.6f, -0.3f, -0.55f}));
    EXPECT_FALSE(VL::InBody(shapes, {0.0f, 0.0f, -3.0f}));
    // Above the floor plane only (y > -0.25 here).
    VL::BodyShapes legs = shapes;
    legs.floor_n = {0.0f, 1.0f, 0.0f};
    legs.floor_d = 0.25f;
    EXPECT_TRUE(VL::InBody(legs, {0.30f, -0.24f, -0.50f}));
    EXPECT_FALSE(VL::InBody(legs, {0.30f, -0.26f, -0.50f}));
    EXPECT_EQ(VL::WorldLayerW(0.5f, 2.0f), 0.5f);  // a world texel
    EXPECT_EQ(VL::WorldLayerW(-0.5f, 2.0f), 0.0f); // the body behind the ray: empty space
    EXPECT_EQ(VL::WorldLayerW(-0.5f, 0.3f), VL::kOccluderW); // the ray behind the body
    EXPECT_NEAR(shapes.reach, std::sqrt(0.25f * 0.25f + 0.09f + 0.36f) + 0.14f, 1e-5f);
    const auto in = VL::MakeIntrinsics(1.2f, 4.0f / 3.0f, 640.0f, 480.0f);
    const VL::Vec3 p = VL::Unproject(in, 400.25f, 100.5f, 0.5f);
    EXPECT_NEAR(p.z, -2.0f, 1e-6f);
    EXPECT_NEAR(in.cx + in.fx * p.x / -p.z, 400.25f, 1e-3f);
    EXPECT_NEAR(in.cy - in.fy * p.y / -p.z, 100.5f, 1e-3f);
}

TEST(LateWarpMath, HunterBodyFollowsWhatIsDrawn) {
    // 2026-10-08 review: in hands_only (the default) the invisible torso, legs and arms held the
    // world inside them head-locked between game frames (the next stair tread inside the legs'
    // 0.40 m). The capture camera at the eye looking down -z (rs identity), 1 unit per metre.
    VL::HunterPose h{};
    h.head = {0.0f, 0.0f, 0.0f};
    h.yaw = 0.0f; // facing -z
    h.soles_y = VL::SolesY(1.6f, false, 0.0f);
    h.own = {true, true};
    h.grip = {VL::Vec3{-0.25f, -0.55f, -0.35f}, VL::Vec3{0.25f, -0.50f, -0.40f}};
    h.blade = {VL::Vec3{0.0f, 0.0f, -1.0f}, VL::Vec3{0.0f, 0.0f, -1.0f}};
    const VL::Mat3 id = VL::FromQuat({0.0f, 0.0f, 0.0f, 1.0f});
    const VL::Vec3 cam = h.head;
    const auto whole = VL::HunterBodyShapes(h, VL::BodyDrawn::Whole, id, cam, 1.0f);
    const auto forearms = VL::HunterBodyShapes(h, VL::BodyDrawn::Forearms, id, cam, 1.0f);
    const auto hands = VL::HunterBodyShapes(h, VL::BodyDrawn::Hands, id, cam, 1.0f);
    EXPECT_EQ(whole.count, 7u);    // eye, torso, legs, 2 arms, weapon, gun
    EXPECT_EQ(forearms.count, 4u); // 2 forearms, weapon, gun
    EXPECT_EQ(hands.count, 2u);    // weapon, gun
    // The next stair tread: 17 cm above his soles, 25 cm ahead (inside the legs' 0.40 m).
    const VL::Vec3 tread{0.0f, h.soles_y + 0.17f, -0.25f};
    EXPECT_TRUE(VL::InBody(whole, tread));
    EXPECT_FALSE(VL::InBody(forearms, tread));
    EXPECT_FALSE(VL::InBody(hands, tread));
    // A railing beside his chest (23 cm off the torso's axis), a door frame 20 cm beside his eye.
    const VL::Vec3 railing{0.20f, -0.30f, 0.12f};
    const VL::Vec3 frame{-0.20f, 0.0f, 0.0f};
    EXPECT_TRUE(VL::InBody(whole, railing));
    EXPECT_FALSE(VL::InBody(hands, railing));
    EXPECT_TRUE(VL::InBody(whole, frame));
    EXPECT_FALSE(VL::InBody(hands, frame));
    EXPECT_FALSE(VL::InBody(forearms, frame));
    // The hands themselves (the grip, and the back of the hand 8 cm behind it) stay his body in
    // every mode (the weapon's and the gun's capsules), and so does the blade.
    for (const auto* shapes : {&whole, &forearms, &hands}) {
        EXPECT_TRUE(VL::InBody(*shapes, h.grip[0]));
        EXPECT_TRUE(VL::InBody(*shapes, h.grip[1]));
        EXPECT_TRUE(VL::InBody(*shapes, h.grip[1] + VL::Vec3{0.0f, 0.0f, 0.08f}));
        EXPECT_TRUE(VL::InBody(*shapes, h.grip[1] + VL::Vec3{0.0f, 0.0f, -1.2f}));
    }
    // arms_only: the forearm (15 cm from the grip toward the shoulder) is his, the upper arm by
    // the shoulder is not (the whole body's arm capsule has it).
    const VL::Vec3 shoulder_r{0.18f, -0.22f, 0.0f};
    const VL::Vec3 to_sh = shoulder_r - h.grip[1];
    const float len = VL::Length(to_sh);
    const VL::Vec3 dir = to_sh * (1.0f / len);
    EXPECT_TRUE(VL::InBody(forearms, h.grip[1] + dir * 0.15f));
    EXPECT_FALSE(VL::InBody(forearms, h.grip[1] + dir * (len * 0.9f)));
    EXPECT_TRUE(VL::InBody(whole, h.grip[1] + dir * (len * 0.9f)));
    // A hand the game animates (not yours) has no capsules at all.
    VL::HunterPose game = h;
    game.own = {false, false};
    EXPECT_EQ(VL::HunterBodyShapes(game, VL::BodyDrawn::Hands, id, cam, 1.0f).count, 0u);
    EXPECT_EQ(VL::HunterBodyShapes(game, VL::BodyDrawn::Whole, id, cam, 1.0f).count, 3u);
    // The floor plane is set in every mode (8 cm over his soles).
    EXPECT_FALSE(VL::InBody(whole, {0.0f, h.soles_y + 0.07f, -0.2f}));
}

struct MovedStats {
    int body{}, body_ok{};             // V sees the hunter's hand (it stayed): found within 0.1 px
    int world{}, world_ok{};           // ... the world (it moved), away from edges: within 0.1 px
    int edge{}, edge_ok{};             // ... near a depth edge of S: within 1.5 px
    int hidden{}, hidden_hole{};       // ... what S never saw: the plain scan's answer
    int fallback{};                    // the plain scan (a ray behind the hand all along)
    double max_err{};
};

MovedStats ScanMoved(const Scene& all, const Render& src, const VL::BodyShapes& shapes,
                     const VL::Motion& m, const VL::Quat& q_v, const VL::Vec3& c_v) {
    MovedStats st{};
    // The hand (the scene's first box) stays; the wall, the pole and the crate are the world.
    Scene body{};
    body.boxes = {all.boxes[0]};
    body.wall = false;
    Scene world = all;
    world.boxes.erase(world.boxes.begin());
    const auto plain = VL::MakeReprojection({}, {}, q_v, c_v, 1.0f);
    VL::Quat q_w{};
    VL::Vec3 c_w{};
    VL::MoveCamera(m, q_w, c_w);
    const auto moved = VL::MakeReprojection(q_w, c_w, q_v, c_v, 1.0f);
    // The capture's texels classified, and each layer's range.
    std::vector<float> signed_w(src.w.size());
    float lo = 1e30f, hi = 0.0f, wlo = 1e30f, whi = 0.0f, blo = 1e30f, bhi = 0.0f;
    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            const float w = src.Fetch(x, y);
            const bool b = w > 0.0f && VL::InBody(shapes, VL::Unproject(src.in, x + 0.5f, y + 0.5f, w));
            signed_w[static_cast<size_t>(y) * src.width + x] = b ? -w : w;
            lo = std::min(lo, w);
            hi = std::max(hi, w);
            (b ? blo : wlo) = std::min(b ? blo : wlo, w);
            (b ? bhi : whi) = std::max(b ? bhi : whi, w);
        }
    }
    const float tau = 1.0f / 11.97f;
    const auto at = [&](int x, int y) { return signed_w[static_cast<size_t>(y) * src.width + x]; };
    const auto fetch_plain = [&](int x, int y) { return src.Fetch(x, y); };
    const auto fetch_body = [&](int x, int y) { return at(x, y) < 0.0f ? -at(x, y) : 0.0f; };
    const auto fetch_world = [&](int x, int y, float r) { return VL::WorldLayerW(at(x, y), r); };
    const auto near_edge = [&](double qx, double qy) {
        const int cx = static_cast<int>(std::floor(qx)), cy = static_cast<int>(std::floor(qy));
        float a = 1e30f, b = 0.0f;
        for (int y = cy - 2; y <= cy + 2; ++y) {
            for (int x = cx - 2; x <= cx + 2; ++x) {
                const float v =
                    src.Fetch(std::clamp(x, 0, src.width - 1), std::clamp(y, 0, src.height - 1));
                a = std::min(a, v);
                b = std::max(b, v);
            }
        }
        return b - a > tau;
    };
    // The world's motion undone: x -> R_y(-yaw) (x - t).
    const auto unmove = [&](const VL::Vec3& x) { return VL::RotateY(-m.yaw, x - m.t); };
    for (int y = 4; y < src.height - 4; y += 3) {
        for (int x = 4; x < src.width - 4; x += 3) {
            const float px = x + 0.5f, py = y + 0.5f;
            const auto line = VL::LineFor(plain, src.in, px, py);
            const auto wline = VL::LineFor(moved, src.in, px, py);
            if (!line.valid || !wline.valid) {
                continue;
            }
            // The layered scans, as vr_late_reproject.comp combines them.
            VL::Hit h{};
            bool found = false;
            if (blo <= bhi) {
                h = VL::March(line, fetch_body, src.width, src.height, blo * 0.99f,
                              bhi * 1.01f + 1e-4f, tau, true);
                found = h.kind == VL::Kind::Surface || h.kind == VL::Kind::Edge;
            }
            if (!found && wlo <= whi) {
                h = VL::March(wline, fetch_world, src.width, src.height, std::max(wlo * 0.99f - 1e-4f, 0.0f),
                              whi * 1.01f + 1e-4f, tau, true);
                found = !h.occluded && h.kind != VL::Kind::Hole;
            }
            if (!found) {
                ++st.fallback;
                h = VL::March(line, fetch_plain, src.width, src.height, lo * 0.99f,
                              hi * 1.01f + 1e-4f, tau);
            }
            // Ground truth: V's ray against the hand where it was and the world moved.
            const VL::Vec3 d = plain.rsv * VL::Vec3{(px - src.in.cx) / src.in.fx,
                                                    -(py - src.in.cy) / src.in.fy, -1.0f};
            const float tb = body.Cast(plain.c, d);
            const VL::Vec3 o_w = unmove(plain.c);
            const VL::Vec3 d_w = VL::RotateY(-m.yaw, d);
            const float tw = world.Cast(o_w, d_w);
            if (tb < 0.0f && tw < 0.0f) {
                continue; // the sky
            }
            const bool on_body = tb > 0.0f && (tw < 0.0f || tb < tw);
            // The point in S's frame (the world's as S saw it).
            const VL::Vec3 point = on_body ? plain.c + d * tb : o_w + d_w * tw;
            const Point2 truth = Project(src.in, point);
            if (truth.x < 2 || truth.y < 2 || truth.x > src.width - 2 || truth.y > src.height - 2) {
                continue;
            }
            const float dist = VL::Length(point);
            const float s_hit = all.Cast({0, 0, 0}, point * (1.0f / dist));
            const bool seen = s_hit > 0.0f && s_hit > dist - 2e-3f * dist;
            if (!seen) {
                // What S never saw: the plain scan's answer (a hole, or the background as S saw
                // it beside the edge), never the hand.
                ++st.hidden;
                const VL::Hit plain_hit = VL::March(line, fetch_plain, src.width, src.height,
                                                    lo * 0.99f, hi * 1.01f + 1e-4f, tau);
                st.hidden_hole += std::abs(h.qx - plain_hit.qx) < 1e-3f &&
                                  std::abs(h.qy - plain_hit.qy) < 1e-3f;
                continue;
            }
            const double err = std::max(std::abs(h.qx - truth.x), std::abs(h.qy - truth.y));
            if (near_edge(truth.x - 0.5, truth.y - 0.5)) {
                ++st.edge;
                st.edge_ok += err < 1.5;
                continue;
            }
            const bool ok = err < 0.1 && h.kind == VL::Kind::Surface;
            if (on_body) {
                ++st.body;
                st.body_ok += ok;
            } else {
                ++st.world;
                st.world_ok += ok;
            }
            st.max_err = std::max(st.max_err, err);
        }
    }
    return st;
}

TEST(LateWarpMath, LayeredMarchOnAMovedWorld) {
    // The hunter's hand (0.35 - 0.45 m) stays where it was while the world (the wall, the pole,
    // the crate) moved: a strafe of 3 - 8 cm and a turn of up to 2 degrees (a 40 fps frame of
    // running or turning), the head moved a little too. The hand's texels are classified by a
    // body capsule round it.
    const Scene scene{};
    const Render src = RenderSource(scene, 640, 480);
    VL::BodyShapes shapes{};
    VL::AddCapsule(shapes, {-0.02f, -0.175f, -0.36f}, {-0.02f, -0.175f, -0.44f}, 0.12f);
    std::mt19937 rng{31};
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    MovedStats total{};
    for (int i = 0; i < 10; ++i) {
        const VL::Motion m{u(rng) * 0.035f,
                           {u(rng) * 0.08f, u(rng) * 0.01f, u(rng) * 0.08f}};
        const VL::Vec3 c_v{u(rng) * 0.02f, u(rng) * 0.02f, u(rng) * 0.02f};
        const VL::Quat q_v = YawPitchRoll(u(rng) * 0.03f, u(rng) * 0.03f, u(rng) * 0.03f);
        const MovedStats st = ScanMoved(scene, src, shapes, m, q_v, c_v);
        total.body += st.body;
        total.body_ok += st.body_ok;
        total.world += st.world;
        total.world_ok += st.world_ok;
        total.edge += st.edge;
        total.edge_ok += st.edge_ok;
        total.hidden += st.hidden;
        total.hidden_hole += st.hidden_hole;
        total.fallback += st.fallback;
        total.max_err = std::max(total.max_err, st.max_err);
    }
    std::printf("  moved world: hand %d (%d within 0.1 px), world %d (%d), near edges %d (%d "
                "within 1.5 px), hidden in the source %d (%d as the plain scan), %d plain fallbacks, max err "
                "%.4f px\n",
                total.body, total.body_ok, total.world, total.world_ok, total.edge, total.edge_ok,
                total.hidden, total.hidden_hole, total.fallback, total.max_err);
    EXPECT_GT(total.body, 3000);
    EXPECT_GT(total.world, 100000);
    EXPECT_GE(total.body_ok, total.body - total.body / 1000);
    EXPECT_GE(total.world_ok, total.world - total.world / 2000);
    // Near a depth edge a ray the world scan takes for unseen (a hole) shows the background as S
    // saw it (not moved on: off by the motion) instead of a streak.
    EXPECT_GE(total.edge_ok, total.edge * 93 / 100);
    EXPECT_GT(total.hidden, 200);
    EXPECT_GE(total.hidden_hole, total.hidden * 97 / 100);
}

// ---- late_warp_edge_blend (vr_dibr_warp.comp EdgeColour) ----

// ParallaxPerInverseDepth bounds how far apart in S two points on one V ray land, per unit of
// their S inverse-depth difference, for every pixel of V (direct projection, random poses).
TEST(LateWarpMath, EdgeBlendParallaxBoundsEveryPixel) {
    std::mt19937 rng{41};
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const float width = 2560.0f, height = 1920.0f;
    const auto in = VL::MakeIntrinsics(122.0f * static_cast<float>(Pi) / 180.0f, 4.0f / 3.0f,
                                       width, height);
    int checked = 0, near_bound = 0;
    double worst = 0.0; // the largest measured / bound
    for (int i = 0; i < 4000; ++i) {
        const VL::Quat q_s = YawPitchRoll(u(rng) * 3.0f, u(rng) * 1.2f, u(rng) * 0.5f);
        const VL::Quat q_v = Mul(q_s, YawPitchRoll(u(rng) * 0.05f, u(rng) * 0.05f, u(rng) * 0.05f));
        const VL::Vec3 c_s{u(rng) * 0.3f, 1.6f + u(rng) * 0.3f, u(rng) * 0.3f};
        const VL::Vec3 c_v = c_s + VL::Vec3{u(rng) * 0.02f, u(rng) * 0.02f, u(rng) * 0.02f};
        const auto rp = VL::MakeReprojection(q_s, c_s, q_v, c_v, 1.0f);
        const float bound = VL::ParallaxPerInverseDepth(rp, in, width, height);
        ASSERT_GE(bound, 0.0f);
        for (int k = 0; k < 8; ++k) {
            const float px = 0.5f * width + 0.5f * width * u(rng);
            const float py = 0.5f * height + 0.5f * height * u(rng);
            const auto line = VL::LineFor(rp, in, px, py);
            ASSERT_TRUE(line.valid);
            const double g = std::hypot(line.gx, line.gy);
            worst = std::max(worst, g / std::max(bound, 1e-9f));
            near_bound += g > 0.5 * bound ? 1 : 0;
            // two surfaces on V's ray (1.5 m and 25 m from V) land |g| dr apart in S
            const VL::Vec3 dv{(px - in.cx) / in.fx, -(py - in.cy) / in.fy, -1.0f};
            const VL::Vec3 dir = rp.rsv * dv;
            const VL::Vec3 a = rp.c + dir * 1.5f;
            const VL::Vec3 b = rp.c + dir * 25.0f;
            const double dr = 1.0 / -a.z - 1.0 / -b.z;
            const Point2 pa = Project(in, a);
            const Point2 pb = Project(in, b);
            EXPECT_LE(std::hypot(pa.x - pb.x, pa.y - pb.y),
                      bound * std::abs(dr) * (1.0 + 1e-4) + 2e-3);
            ++checked;
        }
    }
    EXPECT_EQ(checked, 32000);
    EXPECT_LE(worst, 1.0 + 1e-4); // never above the corners' maximum
    EXPECT_GT(near_bound, checked / 10); // and not a loose bound
    // rotation only: no parallax at all
    const VL::Quat q = YawPitchRoll(0.3f, 0.1f, 0.0f);
    const auto rot = VL::MakeReprojection(q, {0.0f, 1.6f, 0.0f}, Mul(q, YawPitchRoll(0.02f, 0, 0)),
                                          {0.0f, 1.6f, 0.0f}, 1.0f);
    EXPECT_FLOAT_EQ(VL::ParallaxPerInverseDepth(rot, in, width, height), 0.0f);
}

TEST(LateWarpMath, EdgeBlendScaleAndWeight) {
    const float width = 2560.0f, height = 1920.0f;
    const auto in = VL::MakeIntrinsics(1.9f, 4.0f / 3.0f, width, height);
    const VL::Quat q = YawPitchRoll(0.3f, 0.1f, 0.0f);
    // 2.5 mm sideways between the capture and V (a slow head sway over ~3 refreshes)
    const auto rp = VL::MakeReprojection(q, {0.0f, 1.6f, 0.0f}, q, {0.0025f, 1.6f, 0.0f}, 1.0f);
    const float scale = VL::EdgeBlendScale(1.0f, false, rp, in, width, height);
    EXPECT_GT(scale, 0.0f);
    // the centre's parallax is fx * 2.5 mm per unit of 1/z; the corners' bound is no smaller
    EXPECT_GE(scale, in.fx * 0.0025f * 0.999f);
    // off: no threshold, or the world moving with the game camera
    EXPECT_LT(VL::EdgeBlendScale(0.0f, false, rp, in, width, height), 0.0f);
    EXPECT_LT(VL::EdgeBlendScale(1.0f, true, rp, in, width, height), 0.0f);
    EXPECT_FLOAT_EQ(VL::EdgeBlendWeight(-1.0f, 0.0f, 0.0f), 0.0f);
    // rotation only: every edge blended
    const auto rot = VL::MakeReprojection(q, {0.0f, 1.6f, 0.0f}, Mul(q, YawPitchRoll(0.02f, 0, 0)),
                                          {0.0f, 1.6f, 0.0f}, 1.0f);
    const float rot_scale = VL::EdgeBlendScale(1.0f, false, rot, in, width, height);
    EXPECT_FLOAT_EQ(rot_scale, 0.0f);
    EXPECT_FLOAT_EQ(VL::EdgeBlendWeight(rot_scale, 0.0f, 10.0f), 1.0f);
    // a far tree against the sky (20 m: 1/z 0.05): fully blended; a railing 3 m against a wall
    // 6 m; a near weapon (0.5 m) against a wall 3 m: the nearest texel
    EXPECT_FLOAT_EQ(VL::EdgeBlendWeight(scale, 0.0f, 0.05f), 1.0f);
    const float railing = VL::EdgeBlendWeight(scale, 1.0f / 6.0f, 1.0f / 3.0f);
    EXPECT_GT(railing, 0.0f);
    EXPECT_FLOAT_EQ(VL::EdgeBlendWeight(scale, 1.0f / 3.0f, 2.0f), 0.0f);
    // 1 up to half the threshold, 0 from the threshold, linear and non-increasing between
    EXPECT_FLOAT_EQ(VL::EdgeBlendWeight(2.0f, 0.0f, 0.25f), 1.0f);
    EXPECT_FLOAT_EQ(VL::EdgeBlendWeight(2.0f, 0.0f, 0.375f), 0.5f);
    EXPECT_FLOAT_EQ(VL::EdgeBlendWeight(2.0f, 0.0f, 0.5f), 0.0f);
    EXPECT_FLOAT_EQ(VL::EdgeBlendWeight(2.0f, 0.0f, 3.0f), 0.0f);
    float prev = 1.0f;
    for (int k = 0; k <= 100; ++k) {
        const float a = VL::EdgeBlendWeight(scale, 0.1f, 0.1f + 0.01f * static_cast<float>(k));
        EXPECT_LE(a, prev);
        prev = a;
    }
}

// The flip: an anti-aliased silhouette (fg 1 at 1/z 0.5, its edge texel half covered, bg 0 at 1/z
// 0.05: the depth buffer is not anti-aliased, so the half texel has the fg's depth) seen by one V
// pixel while V slides over the capture by a sub-pixel phase every refresh. The nearest texel
// jumps; the blend at small parallax slides; at large parallax it is the nearest texel exactly
// (no smearing across the silhouette).
TEST(LateWarpMath, EdgeBlendSlidesTheEdgeInsteadOfFlipping) {
    std::array<float, 32> colour{};
    std::array<float, 32> w{};
    for (int x = 0; x < 32; ++x) {
        colour[x] = x < 15 ? 1.0f : x == 15 ? 0.5f : 0.0f;
        w[x] = x <= 15 ? 0.5f : 0.05f;
    }
    const float k_px = 30.0f, tau = 1.0f; // the eye disparity test (SrcFiltered)
    const auto sample = [&](float q, float scale) {
        const int i = static_cast<int>(std::floor(q));
        const float f = q - static_cast<float>(i);
        if (std::abs(w[i + 1] - w[i]) * k_px <= tau) {
            return colour[i] + (colour[i + 1] - colour[i]) * f; // one surface: bilinear
        }
        return VL::EdgeColour({colour[i], colour[i + 1], colour[i], colour[i + 1]},
                              {w[i], w[i + 1], w[i], w[i + 1]}, f, 0.0f, scale);
    };
    const auto crawl = [&](float scale) {
        // V's pixels 13..18 over 40 refreshes, the phase sliding 0.05 px per refresh
        double sum = 0.0;
        int n = 0;
        for (int p = 13; p <= 18; ++p) {
            for (int t = 1; t + 1 < 40; ++t) {
                const auto at = [&](int k) {
                    return sample(static_cast<float>(p) + 0.05f * static_cast<float>(k) - 1.0f,
                                  scale);
                };
                const double d2 = at(t + 1) - 2.0 * at(t) + at(t - 1);
                sum += d2 * d2;
                ++n;
            }
        }
        return std::sqrt(sum / n);
    };
    const double nearest = crawl(-1.0f);
    const double blended = crawl(0.5f);  // 0.45 px of parallax across the edge: fully blended
    const double far = crawl(100.0f);    // 45 px: the nearest texel
    EXPECT_GT(nearest, 0.05);
    EXPECT_LT(blended, 0.1 * nearest);
    EXPECT_EQ(far, nearest);
    for (float q = 13.0f; q < 18.0f; q += 0.01f) {
        EXPECT_EQ(sample(q, 100.0f), sample(q, -1.0f));
        // blended colours stay within the two texels they mix
        const int i = static_cast<int>(std::floor(q));
        const float c = sample(q, 0.5f);
        EXPECT_GE(c, std::min(colour[i], colour[i + 1]) - 1e-6f);
        EXPECT_LE(c, std::max(colour[i], colour[i + 1]) + 1e-6f);
    }
    // at a texel centre (no weight on the neighbour) the blend is that texel
    EXPECT_FLOAT_EQ(VL::EdgeColour({0.5f, 0.0f, 0.5f, 0.0f}, {0.5f, 0.05f, 0.5f, 0.05f}, 0.0f, 0.0f,
                                   0.5f),
                    0.5f);
    // 2D: the footprint's spread over the texels with weight
    EXPECT_FLOAT_EQ(VL::EdgeColour({1.0f, 0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.05f, 0.05f}, 0.25f,
                                   0.25f, 0.5f),
                    0.75f * 0.75f);
}

} // namespace
