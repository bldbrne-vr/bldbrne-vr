// SPDX-License-Identifier: GPL-2.0-or-later

// aa_mode=taa (src/core/vr/render_taa.h): the jitter sequence and its push encoding, the vertex
// epilogue's rule, the reprojection between two render cameras, the depth maths, the colour
// clipping and blending, the stable depth, the history's reset guards, and a CPU mirror of the
// resolve on a synthetic scene (still: it converges to the edges' coverage and stops changing;
// panning: it blinks far less than the raw frames).

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/render_taa.h"

namespace {

using namespace VR::RenderTaa;

TEST(RenderTaa, HaltonIsCentredAndInRange) {
    // The classic table: (1/2, 1/3), (1/4, 2/3), (3/4, 1/9), ...
    EXPECT_NEAR(Halton(0).x, 0.0f, 1e-6f);
    EXPECT_NEAR(Halton(0).y, 1.0f / 3.0f - 0.5f, 1e-6f);
    EXPECT_NEAR(Halton(1).x, -0.25f, 1e-6f);
    EXPECT_NEAR(Halton(1).y, 2.0f / 3.0f - 0.5f, 1e-6f);
    EXPECT_NEAR(Halton(2).x, 0.25f, 1e-6f);
    EXPECT_NEAR(Halton(2).y, 1.0f / 9.0f - 0.5f, 1e-6f);
    for (const std::uint32_t phases : {8u, 16u}) {
        float mx = 0.0f, my = 0.0f;
        for (std::uint32_t i = 0; i < phases; ++i) {
            const Jitter j = Halton(i, phases);
            EXPECT_GE(j.x, -0.5f);
            EXPECT_LT(j.x, 0.5f);
            EXPECT_GE(j.y, -0.5f);
            EXPECT_LT(j.y, 0.5f);
            mx += j.x;
            my += j.y;
        }
        EXPECT_LT(std::abs(mx / phases), 0.07f) << phases;
        EXPECT_LT(std::abs(my / phases), 0.07f) << phases;
        // The cycle repeats.
        EXPECT_EQ(Halton(3, phases).x, Halton(3 + phases, phases).x);
    }
    // Any other phase count is 8.
    EXPECT_EQ(Halton(5, 12).x, Halton(5, 8).x);
}

TEST(RenderTaa, PushEncodingRoundTripsThroughTheEpilogue) {
    for (const float size : {1808.0f, 2720.0f, 4096.0f}) {
        for (std::uint32_t i = 0; i < 8; ++i) {
            const Jitter px = Halton(i);
            const Jitter ndc = JitterNdc(px, size, size);
            const Push push = EncodePush(ndc);
            // A perspective vertex at w = 3: the jitter scales with w (an exact NDC offset).
            const float w = 3.0f;
            const Clip c = EpilogueCpu(0.2f * w, -0.1f * w, w, push.shift, push.hud);
            const float got_x = (c.x / w - 0.2f) * 0.5f * size;
            const float got_y = (c.y / w + 0.1f) * 0.5f * size;
            EXPECT_NEAR(got_x, px.x, 1e-3f) << size << " " << i;
            EXPECT_NEAR(got_y, px.y, 1e-3f) << size << " " << i;
            // Full-screen passes (w == 1) are untouched by a jittered draw.
            const Clip f = EpilogueCpu(0.5f, 0.25f, 1.0f, push.shift, push.hud);
            EXPECT_EQ(f.x, 0.5f);
            EXPECT_EQ(f.y, 0.25f);
        }
    }
    EXPECT_EQ(JitterNdc({0.5f, 0.5f}, 0.0f, 100.0f).x, 0.0f);
}

TEST(RenderTaa, TheEpilogueKeepsItsOldRulesForHudAndStereoShift) {
    // HUD (w == 1, hud >= 0.55): scale toward the centre plus the offset, as before.
    const float hud = 12.0f + 0.03f; // scale 12/16, offset 0.03
    const Clip h = EpilogueCpu(0.4f, -0.2f, 1.0f, 0.0f, hud);
    EXPECT_NEAR(h.x, 0.4f * 0.75f + 0.03f, 1e-6f);
    EXPECT_NEAR(h.y, -0.2f * 0.75f, 1e-6f);
    // ... not on the screen's edge (full-screen passes).
    const Clip e = EpilogueCpu(1.0f, -1.0f, 1.0f, 0.0f, hud);
    EXPECT_EQ(e.x, 1.0f);
    EXPECT_EQ(e.y, -1.0f);
    // A HUD-phase perspective draw: the stereo shift only (stereo_mode 0).
    const Clip p = EpilogueCpu(0.3f, 0.1f, 2.0f, 0.05f, hud);
    EXPECT_NEAR(p.x, 0.35f, 1e-6f);
    EXPECT_EQ(p.y, 0.1f);
    // stereo_mode 0 scene draws (hud 0): x + shift, not times w.
    const Clip s = EpilogueCpu(0.3f, 0.1f, 2.0f, 0.05f, 0.0f);
    EXPECT_NEAR(s.x, 0.35f, 1e-6f);
    EXPECT_EQ(s.y, 0.1f);
    // Plain draws (0, 0): untouched.
    const Clip z = EpilogueCpu(0.3f, 0.1f, 2.0f, 0.0f, 0.0f);
    EXPECT_EQ(z.x, 0.3f);
    EXPECT_EQ(z.y, 0.1f);
    // The smallest HUD value (1 step, offset -0.45) is never read as a jitter, nor is a jitter
    // (the tag +- 0.01) ever read as a HUD value.
    EXPECT_FALSE(0.55f < JitterTest);
    EXPECT_TRUE(JitterTag + 0.01f < JitterTest);
    EXPECT_TRUE(JitterTag - 0.01f < 0.0f);
}

TEST(RenderTaa, CameraRowsMatchTheHook) {
    // Level, yaw 0: right +x, up +y, forward +z.
    const auto r0 = CameraRows(0.0f, 0.0f, 0.0f);
    const std::array<float, 9> id{1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i) {
        EXPECT_NEAR(r0[i], id[i], 1e-7f);
    }
    // The forward row is VrMatchCamera's forward: (sin y cos p, -sin p, cos y cos p).
    const float yaw = 0.7f, pitch = -0.3f, roll = 0.2f;
    const auto r = CameraRows(yaw, pitch, roll);
    EXPECT_NEAR(r[6], std::sin(yaw) * std::cos(pitch), 1e-6f);
    EXPECT_NEAR(r[7], -std::sin(pitch), 1e-6f);
    EXPECT_NEAR(r[8], std::cos(yaw) * std::cos(pitch), 1e-6f);
    // Orthonormal.
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) {
            float d = 0.0f;
            for (int k = 0; k < 3; ++k) {
                d += r[a * 3 + k] * r[b * 3 + k];
            }
            EXPECT_NEAR(d, a == b ? 1.0f : 0.0f, 1e-6f);
        }
    }
}

Camera MakeCam(float yaw, float pitch, std::array<float, 3> pos, float w, float h) {
    Camera c{};
    c.rows = CameraRows(yaw, pitch, 0.0f);
    c.pos = pos;
    Focal(w, h, 1.6f, w / h, c.fx, c.fy);
    return c;
}

// Projects a world point into a camera (window px, y down: ysign -1).
bool Project(const Camera& c, const std::array<float, 3>& p, float w, float h, float& u,
             float& v) {
    float view[3];
    for (int i = 0; i < 3; ++i) {
        view[i] = 0.0f;
        for (int k = 0; k < 3; ++k) {
            view[i] += c.rows[i * 3 + k] * (p[k] - c.pos[k]);
        }
    }
    if (view[2] <= 0.0f) {
        return false;
    }
    u = 0.5f * w + c.fx * view[0] / view[2];
    v = 0.5f * h - c.fy * view[1] / view[2];
    return true;
}

TEST(RenderTaa, ReprojectionIsExact) {
    const float W = 2720.0f, H = 1808.0f;
    const Camera a = MakeCam(0.3f, 0.1f, {1.0f, 2.0f, 3.0f}, W, H);
    // The same camera: the identity.
    const Reprojector same = MakeReprojector(a, a);
    for (const float u : {0.5f, 700.25f, 2719.5f}) {
        for (const float v : {0.5f, 903.0f, 1807.5f}) {
            const PixelPos q = Reproject(same, u, v, 0.25f, W, H, a.fx, a.fy, a.fx, a.fy, -1.0f);
            ASSERT_TRUE(q.valid);
            EXPECT_NEAR(q.u, u, 1e-3f);
            EXPECT_NEAR(q.v, v, 1e-3f);
        }
    }
    // A moved and turned camera: world points land where the previous camera saw them.
    const Camera b = MakeCam(0.33f, 0.08f, {1.05f, 2.01f, 3.02f}, W, H);
    const Reprojector r = MakeReprojector(b, a); // current b -> previous a
    for (const std::array<float, 3>& p : {std::array<float, 3>{2.0f, 2.5f, 8.0f},
                                          std::array<float, 3>{1.5f, 1.8f, 4.0f},
                                          std::array<float, 3>{4.0f, 3.0f, 20.0f}}) {
        float ub, vb, ua, va;
        ASSERT_TRUE(Project(b, p, W, H, ub, vb));
        ASSERT_TRUE(Project(a, p, W, H, ua, va));
        // The point's inverse depth in b.
        float zb = 0.0f;
        for (int k = 0; k < 3; ++k) {
            zb += b.rows[6 + k] * (p[k] - b.pos[k]);
        }
        const PixelPos q = Reproject(r, ub, vb, 1.0f / zb, W, H, b.fx, b.fy, a.fx, a.fy, -1.0f);
        ASSERT_TRUE(q.valid);
        EXPECT_NEAR(q.u, ua, 2e-3f);
        EXPECT_NEAR(q.v, va, 2e-3f);
        float za = 0.0f;
        for (int k = 0; k < 3; ++k) {
            za += a.rows[6 + k] * (p[k] - a.pos[k]);
        }
        EXPECT_NEAR(PrevInverseDepth(r, ub, vb, 1.0f / zb, W, H, b.fx, b.fy, -1.0f), 1.0f / za,
                    1e-5f);
    }
    // The sky (1/z = 0): a pure yaw moves it by the analytic homography, whatever the position.
    const Camera c0 = MakeCam(0.0f, 0.0f, {0, 0, 0}, W, H);
    const Camera c1 = MakeCam(0.01f, 0.0f, {5, 0, 0}, W, H);
    const PixelPos s = Reproject(MakeReprojector(c1, c0), 0.5f * W, 0.5f * H, 0.0f, W, H, c1.fx,
                                 c1.fy, c0.fx, c0.fy, -1.0f);
    ASSERT_TRUE(s.valid);
    EXPECT_NEAR(s.u, 0.5f * W + c0.fx * std::tan(0.01f), 1e-2f);
    EXPECT_NEAR(s.v, 0.5f * H, 1e-3f);
}

TEST(RenderTaa, DepthIsTheDibrPrepsAffineMap) {
    const float n = 0.1f, f = 1000.0f;
    const DepthAffine d = MakeDepthAffine(n, f, 0.0f, 1.0f);
    EXPECT_NEAR(InverseDepth(d, 0.0f), 1.0f / n, 1e-3f);
    // A point at z: depth = (1/n - 1/z) / (1/n - 1/f).
    for (const float z : {0.5f, 2.0f, 50.0f}) {
        const float depth = (1.0f / n - 1.0f / z) / (1.0f / n - 1.0f / f);
        EXPECT_NEAR(InverseDepth(d, depth), 1.0f / z, 1e-3f / z);
    }
    EXPECT_EQ(InverseDepth(d, 1.0f), 0.0f); // the sky
    // A viewport depth range 0.25 .. 0.75.
    const DepthAffine r = MakeDepthAffine(n, f, 0.25f, 0.75f);
    EXPECT_NEAR(InverseDepth(r, 0.25f), 1.0f / n, 1e-3f);
    EXPECT_EQ(InverseDepth(r, 0.75f), 0.0f);
}

TEST(RenderTaa, YCoCgClipAndBlend) {
    const Rgb c{0.8f, 0.3f, 0.1f};
    const Rgb back = YCoCgToRgb(RgbToYCoCg(c));
    EXPECT_NEAR(back.r, c.r, 1e-6f);
    EXPECT_NEAR(back.g, c.g, 1e-6f);
    EXPECT_NEAR(back.b, c.b, 1e-6f);
    // Y is symmetric in R and B (the game's red/blue swap does not change it).
    EXPECT_NEAR(RgbToYCoCg({0.8f, 0.3f, 0.1f}).r, RgbToYCoCg({0.1f, 0.3f, 0.8f}).r, 1e-7f);
    // Inside the box: unchanged.
    const Rgb mean{0.5f, 0.0f, 0.0f}, ext{0.1f, 0.1f, 0.1f};
    float outside = 0.0f;
    const Rgb in = ClipToBox({0.55f, 0.02f, -0.05f}, mean, ext, &outside);
    EXPECT_EQ(in.r, 0.55f);
    EXPECT_LE(outside, 1.0f);
    // Outside: onto the surface, on the line to the centre.
    const Rgb out = ClipToBox({0.9f, 0.1f, 0.0f}, mean, ext, &outside);
    EXPECT_NEAR(outside, 4.0f, 1e-5f);
    EXPECT_NEAR(out.r, 0.6f, 1e-6f);
    EXPECT_NEAR(out.g, 0.025f, 1e-6f);
    EXPECT_NEAR(out.b, 0.0f, 1e-6f);
    // The blend: an average while the count is low, alpha_min after.
    EXPECT_NEAR(BlendAlpha(0.0f, 0.1f), 1.0f, 1e-7f);
    EXPECT_NEAR(BlendAlpha(3.0f, 0.1f), 0.25f, 1e-7f);
    EXPECT_NEAR(BlendAlpha(50.0f, 0.1f), 0.1f, 1e-7f);
    EXPECT_NEAR(CountCap(0.1f), 10.0f, 1e-5f);
    const Rgb g{0.5f, 0.5f, 0.5f};
    const Rgb same = Blend(g, g, 0.3f);
    EXPECT_NEAR(same.g, 0.5f, 1e-6f);
    // A bright sample counts less than its alpha (anti-flicker).
    const Rgb bright = Blend({1, 1, 1}, {0, 0, 0}, 0.5f);
    EXPECT_LT(bright.g, 0.5f);
    EXPECT_GT(bright.g, 0.25f);
}

TEST(RenderTaa, ResetGuards) {
    FrameFacts a{};
    a.valid = true;
    a.width = 2720;
    a.height = 1808;
    a.time_s = 1.0;
    FrameFacts b = a;
    b.time_s = 1.0167;
    EXPECT_EQ(ResetReason(a, b), Reset::None);
    EXPECT_EQ(ResetReason(FrameFacts{}, b), Reset::First);
    FrameFacts bad = b;
    bad.valid = false;
    EXPECT_EQ(ResetReason(a, bad), Reset::Record);
    FrameFacts s = b;
    s.width = 3200;
    EXPECT_EQ(ResetReason(a, s), Reset::Size);
    FrameFacts t = b;
    t.turn_serial = 1;
    EXPECT_EQ(ResetReason(a, t), Reset::Turn);
    FrameFacts sp = b;
    sp.space_serial = 4;
    EXPECT_EQ(ResetReason(a, sp), Reset::Space);
    FrameFacts ws = b;
    ws.world_scale = 2.0f;
    EXPECT_EQ(ResetReason(a, ws), Reset::Space);
    FrameFacts gap = b;
    gap.time_s = 1.2;
    EXPECT_EQ(ResetReason(a, gap), Reset::Gap);
    FrameFacts turn = b;
    turn.rows = CameraRows(0.5f, 0.0f, 0.0f);
    EXPECT_EQ(ResetReason(a, turn), Reset::Jump);
    FrameFacts small = b;
    small.rows = CameraRows(0.05f, 0.0f, 0.0f);
    EXPECT_EQ(ResetReason(a, small), Reset::None);
    FrameFacts moved = b;
    moved.pos = {1.5f, 0.0f, 0.0f};
    EXPECT_EQ(ResetReason(a, moved), Reset::Jump);
    for (int i = 0; i < static_cast<int>(Reset::Count); ++i) {
        EXPECT_NE(std::string(ResetName(static_cast<Reset>(i))), std::string("?"));
    }
}

// A vertical depth edge at fractional x = e (foreground 0.2 left of it, background 0.8 right),
// point-sampled with the jitter: the stable depth stops flipping after the first cycle.
TEST(RenderTaa, StableDepthDoesNotFlipUnderTheJitter) {
    const int W = 12;
    const float e = 5.37f;
    std::vector<float> stable(W, 0.0f);
    std::vector<std::vector<float>> outputs;
    for (std::uint32_t frame = 0; frame < 24; ++frame) {
        const Jitter j = Halton(frame);
        std::vector<float> depth(W);
        for (int q = 0; q < W; ++q) {
            depth[q] = (q + 0.5f - j.x) < e ? 0.2f : 0.8f;
        }
        std::vector<float> next(W);
        for (int p = 0; p < W; ++p) {
            std::array<float, 3> taps{};
            for (int k = -1; k <= 1; ++k) {
                taps[k + 1] = depth[std::clamp(p + k, 0, W - 1)];
            }
            const int centre = std::clamp(p + static_cast<int>(std::lround(j.x)), 0, W - 1);
            next[p] = StableDepth(taps.data(), 3, depth[centre], frame > 0, stable[p], 1e-4f);
        }
        stable = next;
        outputs.push_back(stable);
    }
    for (size_t f = 9; f < outputs.size(); ++f) {
        EXPECT_TRUE(outputs[f] == outputs[8]) << f;
    }
    // ... and it is the edge's own depth on each side (no in-between values).
    for (int p = 0; p < W; ++p) {
        EXPECT_TRUE(outputs.back()[p] == 0.2f || outputs.back()[p] == 0.8f);
    }
    EXPECT_EQ(outputs.back()[2], 0.2f);
    EXPECT_EQ(outputs.back()[9], 0.8f);
}

// ---- The CPU resolve on a synthetic scene ----

constexpr int N = 64;

// The scene (coverage 0..1 at a point): a hard edge at a slight angle and a thin rotated line
// 0.3 px wide; `shift` moves it right (panning).
float Scene(float x, float y, float shift) {
    x -= shift;
    float v = (x * 0.97f + y * 0.24f) < 20.3f ? 0.0f : 0.8f;
    const float d = std::abs((x - 10.0f) * 0.31f - (y - 4.0f) * 0.95f);
    if (d < 0.15f) {
        v = 1.0f;
    }
    return v;
}

// The ideal: the box-filtered coverage (16 x 16 samples per pixel).
std::vector<float> Ideal(float shift) {
    std::vector<float> img(N * N);
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            float s = 0.0f;
            for (int sy = 0; sy < 16; ++sy) {
                for (int sx = 0; sx < 16; ++sx) {
                    s += Scene(x + (sx + 0.5f) / 16.0f, y + (sy + 0.5f) / 16.0f, shift);
                }
            }
            img[y * N + x] = s / 256.0f;
        }
    }
    return img;
}

// One rendered frame: point samples at the jittered pixel centres (q + 0.5 - j).
std::vector<float> Render(Jitter j, float shift) {
    std::vector<float> img(N * N);
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            img[y * N + x] = Scene(x + 0.5f - j.x, y + 0.5f - j.y, shift);
        }
    }
    return img;
}

float CatmullRom1(float t, float a, float b, float c, float d) {
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * b) + (-a + c) * t + (2.0f * a - 5.0f * b + 4.0f * c - d) * t2 +
                   (-a + 3.0f * b - 3.0f * c + d) * t3);
}

// The history at a horizontal offset (panning moves it along x only).
float SampleX(const std::vector<float>& img, int y, float u) {
    const float x = u - 0.5f;
    const int x0 = static_cast<int>(std::floor(x));
    const float t = x - static_cast<float>(x0);
    const auto at = [&](int xi) { return img[y * N + std::clamp(xi, 0, N - 1)]; };
    return CatmullRom1(t, at(x0 - 1), at(x0), at(x0 + 1), at(x0 + 2));
}

struct Resolver {
    std::vector<float> hist = std::vector<float>(N * N, 0.0f);
    std::vector<float> count = std::vector<float>(N * N, 0.0f);
    bool first = true;

    std::vector<float> Run(const std::vector<float>& frame, Jitter j, float motion_px) {
        std::vector<float> out(N * N), nh(N * N), nc(N * N);
        Params p{};
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                PixelIn in{};
                int i = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int qx = std::clamp(x + dx, 0, N - 1);
                        const int qy = std::clamp(y + dy, 0, N - 1);
                        const float v = frame[qy * N + qx];
                        in.taps[i] = {v, v, v};
                        in.dx[i] = static_cast<float>(dx) - j.x;
                        in.dy[i] = static_cast<float>(dy) - j.y;
                        ++i;
                    }
                }
                const float u_prev = x + 0.5f - motion_px;
                in.has_history = !first && u_prev > 0.0f && u_prev < static_cast<float>(N);
                if (in.has_history) {
                    const float h = SampleX(hist, y, u_prev);
                    in.history = {h, h, h};
                    in.n = count[y * N + std::clamp(static_cast<int>(u_prev), 0, N - 1)];
                }
                const PixelOut o = ResolvePixel(in, p);
                out[y * N + x] = o.rgb.g;
                nh[y * N + x] = o.rgb.g;
                nc[y * N + x] = o.n;
            }
        }
        hist = nh;
        count = nc;
        first = false;
        return out;
    }
};

// Flicker: how much the brightness of a fixed WORLD point changes from frame to frame while the
// view pans (the scene moves `speed` px per frame). Each frame is sampled at the point's screen
// position (linear between pixel centres); the mean over the points of the temporal standard
// deviation, over the world points whose ideal coverage is partial (the edges and the line).
double Flicker(const std::vector<std::vector<float>>& frames, const std::vector<float>& shifts,
               const std::vector<float>& ideal_world) {
    double sum = 0.0;
    int points = 0;
    for (int y = 2; y < N - 2; ++y) {
        for (int x = 2; x < N - 2; ++x) {
            const float id = ideal_world[y * N + x];
            if (!(id > 0.02f && id < 0.78f) && !(id > 0.82f && id < 0.98f)) {
                continue;
            }
            bool inside = true;
            double m1 = 0.0, m2 = 0.0;
            for (size_t f = 0; f < frames.size(); ++f) {
                const float u = static_cast<float>(x) + 0.5f + shifts[f] - 0.5f; // pixel index
                const int x0 = static_cast<int>(std::floor(u));
                if (x0 < 0 || x0 + 1 >= N) {
                    inside = false;
                    break;
                }
                const float t = u - static_cast<float>(x0);
                const double v =
                    frames[f][y * N + x0] * (1.0f - t) + frames[f][y * N + x0 + 1] * t;
                m1 += v;
                m2 += v * v;
            }
            if (!inside) {
                continue;
            }
            const double n = static_cast<double>(frames.size());
            sum += std::sqrt(std::max(m2 / n - (m1 / n) * (m1 / n), 0.0));
            ++points;
        }
    }
    return points > 0 ? sum / points : 0.0;
}

TEST(RenderTaa, StillCameraConvergesAndStopsChanging) {
    Resolver r{};
    const std::vector<float> ideal = Ideal(0.0f);
    std::vector<float> prev, out;
    double rms = 0.0;
    for (std::uint32_t f = 0; f < 48; ++f) {
        const Jitter j = Halton(f);
        prev = out;
        out = r.Run(Render(j, 0.0f), j, 0.0f);
        if (f >= 40) {
            double s = 0.0;
            for (int i = 0; i < N * N; ++i) {
                s += (out[i] - prev[i]) * (out[i] - prev[i]);
            }
            rms = std::max(rms, std::sqrt(s / (N * N)));
        }
    }
    // The mean absolute error against the box-filtered coverage on the edge pixels (the
    // reconstruction filter is a little wider than a box: a few percent).
    double err = 0.0, raw_err = 0.0;
    int edge = 0;
    const std::vector<float> raw = Render({}, 0.0f);
    for (int i = 0; i < N * N; ++i) {
        if (ideal[i] > 0.02f && ideal[i] < 0.78f) {
            err += std::abs(out[i] - ideal[i]);
            raw_err += std::abs(raw[i] - ideal[i]);
            ++edge;
        }
    }
    ASSERT_GT(edge, 40);
    err /= edge;
    raw_err /= edge;
    EXPECT_LT(err, 0.5 * raw_err) << err << " vs raw " << raw_err;
    EXPECT_LT(err, 0.12) << err;
    // Steady state: the frame-to-frame change is under 4/255 RMS (no visible shimmer at rest).
    EXPECT_LT(rms, 4.0 / 255.0) << rms;
}

TEST(RenderTaa, PanningFlickersFarLessThanTheRawFrames) {
    const std::vector<float> ideal_world = Ideal(0.0f);
    for (const float speed : {0.1f, 0.3f, 0.7f}) {
        Resolver r{};
        std::vector<std::vector<float>> taa, raw;
        std::vector<float> shifts;
        for (std::uint32_t f = 0; f < 40; ++f) {
            const float shift = speed * static_cast<float>(f);
            const Jitter j = Halton(f);
            const std::vector<float> out = r.Run(Render(j, shift), j, speed);
            if (f < 16) {
                continue; // converging
            }
            taa.push_back(out);
            raw.push_back(Render({}, shift));
            shifts.push_back(shift);
        }
        const double f_taa = Flicker(taa, shifts, ideal_world);
        const double f_raw = Flicker(raw, shifts, ideal_world);
        EXPECT_GT(f_raw, 0.05) << speed;
        EXPECT_LE(3.0 * f_taa, f_raw) << "speed " << speed << ": " << f_taa << " vs raw " << f_raw;
    }
}

TEST(RenderTaa, LumaWeightCodesAndGlintEnergy) {
    // taa_luma_weight round-trips through push flags2 bits 0..7.
    for (const float k : {0.0f, 0.3f, 0.5f, 1.0f}) {
        EXPECT_NEAR(LumaWeight(LumaCode(k)), k, 0.51f / 255.0f);
        EXPECT_LE(LumaCode(k), 255u);
    }
    EXPECT_EQ(LumaCode(2.0f), 255u);
    EXPECT_EQ(LumaCode(-1.0f), 0u);
    // k = 1 is round 1's blend exactly; k = 0 is the plain mix.
    const Rgb c{0.9f, 0.7f, 0.2f}, h{0.1f, 0.3f, 0.4f};
    const Rgb old = Blend(c, h, 0.25f);
    const Rgb k1 = Blend(c, h, 0.25f, 1.0f);
    EXPECT_NEAR(old.r, k1.r, 1e-7f);
    EXPECT_NEAR(old.g, k1.g, 1e-7f);
    const Rgb k0 = Blend(c, h, 0.25f, 0.0f);
    EXPECT_NEAR(k0.r, 0.25f * 0.9f + 0.75f * 0.1f, 1e-6f);
    EXPECT_NEAR(k0.b, 0.25f * 0.2f + 0.75f * 0.4f, 1e-6f);
    // A sub-pixel glint: bright (1.0) in 1 of the 8 jitter phases, dark (0.1) in the others. The
    // converged history over a cycle: its mean (the glint's energy) and its swing (the flicker).
    // The plain blend keeps the true average (0.2125); round 1's full weighting dims it by 22%; a
    // mild weight dims it less. The plain blend at alpha 0.05 swings less than round 1's (k = 1,
    // alpha 0.1): the longer history, not the weighting, is what keeps glints steady.
    struct Cycle {
        double mean, swing;
    };
    const auto converged = [](float k, float alpha) {
        Rgb hist{0.1f, 0.1f, 0.1f};
        double sum = 0.0, lo = 1.0, hi = 0.0;
        int n = 0;
        for (int f = 0; f < 8 * 80; ++f) {
            const float v = (f % 8 == 3) ? 1.0f : 0.1f;
            hist = Blend({v, v, v}, hist, alpha, k);
            if (f >= 8 * 70) {
                sum += hist.g;
                lo = std::min(lo, static_cast<double>(hist.g));
                hi = std::max(hi, static_cast<double>(hist.g));
                ++n;
            }
        }
        return Cycle{sum / n, hi - lo};
    };
    const double truth = (1.0 + 7 * 0.1) / 8.0;
    EXPECT_NEAR(converged(0.0f, 0.1f).mean, truth, 0.002);
    EXPECT_NEAR(converged(0.0f, 0.05f).mean, truth, 0.002);
    EXPECT_LT(converged(1.0f, 0.1f).mean, 0.8 * truth);
    EXPECT_GT(converged(0.3f, 0.1f).mean, converged(1.0f, 0.1f).mean + 0.02);
    EXPECT_LT(converged(0.0f, 0.05f).swing, converged(1.0f, 0.1f).swing);
    EXPECT_LT(converged(0.0f, 0.1f).swing, 1.7 * converged(1.0f, 0.1f).swing);
}

TEST(RenderTaa, OccluderTestFiresOnlyWhenTheWholeNeighbourhoodMovedInFront) {
    // 1/z: the beast at 15 m (0.0667) in front of the wall at 20 m (0.05).
    const float beast = 1.0f / 15.0f, wall = 1.0f / 20.0f, dr = 0.01f;
    // The beast arrived: the history 2x2 (even its nearest) is the wall, all of the 3x3 the beast.
    EXPECT_TRUE(OccluderArrived(wall, beast, dr));
    // A static silhouette: the 3x3 still holds a background tap (its farthest = the wall).
    EXPECT_FALSE(OccluderArrived(wall, wall, dr));
    // The same surface, a little depth noise (the jitter on a slope): never.
    EXPECT_FALSE(OccluderArrived(0.05f, 0.05f * 1.04f, dr));
    // The history nearer than the 3x3 (the disocclusion case is the other test).
    EXPECT_FALSE(OccluderArrived(beast, wall, dr));
    // Sky in the 3x3: nothing to test; sky in the history and the beast now: fires.
    EXPECT_FALSE(OccluderArrived(wall, 0.0f, dr));
    EXPECT_TRUE(OccluderArrived(0.0f, beast, dr));
    // The resolve then starts over at the current frame, like a disocclusion.
    PixelIn in{};
    for (int i = 0; i < 9; ++i) {
        in.taps[i] = {0.6f, 0.6f, 0.6f};
        in.dx[i] = static_cast<float>(i % 3 - 1);
        in.dy[i] = static_cast<float>(i / 3 - 1);
    }
    in.has_history = true;
    in.history = {0.62f, 0.62f, 0.62f}; // inside the clip box: the clip alone would keep it
    in.n = 9.0f;
    in.occluder = true;
    const PixelOut out = ResolvePixel(in, Params{});
    EXPECT_NEAR(out.rgb.g, 0.6f, 1e-6f);
    EXPECT_NEAR(out.n, 1.0f, 1e-6f);
    in.occluder = false;
    EXPECT_GT(ResolvePixel(in, Params{}).rgb.g, 0.6f + 1e-4f);
}

TEST(RenderTaa, SharpenLiftReachesDarkTextures) {
    for (const float l : {0.0f, 0.1f, 0.25f, 0.5f}) {
        EXPECT_NEAR(Lift(LiftCode(l) << 16), l, 0.51f / 510.0f);
    }
    EXPECT_EQ(LiftCode(1.0f), 255u);
    // A dark clinic texture (0.04 .. 0.22, gamma-encoded): RCAS alone barely sharpens it (its
    // output must stay above black); the lift lets it.
    const float dark0 = RcasLobe(0.04f, 0.22f, 0.12f, 1.0f, 0.0f);
    const float dark25 = RcasLobe(0.04f, 0.22f, 0.12f, 1.0f, 0.25f);
    EXPECT_GT(dark0, -0.06f);
    EXPECT_LT(dark25, 2.5f * dark0);
    // A bright Dream texture (0.55 .. 0.8): nearly the same either way (white's limit binds).
    const float bright0 = RcasLobe(0.55f, 0.8f, 0.7f, 1.0f, 0.0f);
    const float bright25 = RcasLobe(0.55f, 0.8f, 0.7f, 1.0f, 0.25f);
    EXPECT_NEAR(bright25, bright0, 0.02f);
    // Never past RCAS's own limit, scaled by the amount; never positive.
    for (const float amount : {0.25f, 0.6f, 1.0f}) {
        for (const float lift : {0.0f, 0.5f}) {
            const float lobe = RcasLobe(0.0f, 0.01f, 0.005f, amount, lift);
            EXPECT_GE(lobe, -0.1875f * amount - 1e-6f);
            EXPECT_LE(lobe, 0.0f);
        }
    }
}

TEST(RenderTaa, FilterAndNearFieldCodes) {
    // taa_filter_px: 3 bits (push flags 5..7), sigma 0.2 .. 0.55 in 0.05 steps.
    EXPECT_EQ(FilterCode(0.3f), 2u);
    EXPECT_EQ(FilterCode(0.0f), 0u);
    EXPECT_EQ(FilterCode(9.0f), 7u);
    for (std::uint32_t c = 0; c < 8; ++c) {
        EXPECT_EQ(FilterCode(FilterSigma(c)), c);
        EXPECT_LE(FilterCode(FilterSigma(c)) << 5, 0xE0u);
    }
    EXPECT_NEAR(FilterWeight(1.0f, 0.0f, FilterK(2)), std::exp(-0.5f / 0.09f), 1e-6f);
    // The near field: bits 16..31, clear of the low flags; full at near_m, none from 1.5 near_m.
    const std::uint32_t nc = NearCode(1.2f, 0.5f);
    EXPECT_EQ(nc & 0xFFFFu, 0u);
    EXPECT_EQ(nc >> 24, 24u);
    EXPECT_EQ((nc >> 16) & 255u, 128u);
    EXPECT_NEAR(NearAlpha(0.1f, 0.6f, nc), 128.0f / 255.0f, 1e-6f);
    EXPECT_NEAR(NearAlpha(0.1f, 1.2f, nc), 128.0f / 255.0f, 1e-6f);
    EXPECT_NEAR(NearAlpha(0.1f, 1.5f, nc), 0.5f * 128.0f / 255.0f, 1e-5f);
    EXPECT_FLOAT_EQ(NearAlpha(0.1f, 1.8f, nc), 0.1f);
    EXPECT_FLOAT_EQ(NearAlpha(0.1f, 30.0f, nc), 0.1f);
    EXPECT_FLOAT_EQ(NearAlpha(0.1f, 0.0f, nc), 0.1f);          // the sky (1/z = 0)
    EXPECT_FLOAT_EQ(NearAlpha(0.1f, 0.5f, NearCode(0.0f, 0.5f)), 0.1f); // off
    EXPECT_FLOAT_EQ(NearAlpha(0.7f, 0.5f, nc), 0.7f);          // never lowers alpha
}

// taa_near_m is metres; the shader's depth is game units (world_scale game units per metre).
TEST(RenderTaa, NearFieldIsConvertedToGameUnits) {
    EXPECT_FLOAT_EQ(NearGameUnits(1.2f, 1.0f), 1.2f); // Bloodborne: one game unit per metre
    EXPECT_FLOAT_EQ(NearGameUnits(1.2f, 2.0f), 2.4f);
    EXPECT_FLOAT_EQ(NearGameUnits(1.2f, 0.5f), 0.6f);
    EXPECT_FLOAT_EQ(NearGameUnits(1.2f, 0.0f), 1.2f); // no scale: metres as they are
    EXPECT_EQ(NearCode(NearGameUnits(1.2f, 2.0f), 0.8f) >> 24, 48u); // 2.4 units in 0.05 steps
    EXPECT_EQ(NearCode(NearGameUnits(1.2f, 1.0f), 0.8f), NearCode(1.2f, 0.8f)); // unchanged at 1
}

TEST(RenderTaa, SharpenFadesOutInTheNearField) {
    // The skeptic (2026-10-08): sharpen 0.7 with lift 0.5 made the blade blink 2.5-12x SMAA's,
    // because the near field is mostly this frame's jittered image. The sharpening (and its lift)
    // is none where the near field's alpha is full and full where it is none.
    const std::uint32_t nc = NearCode(1.2f, 0.8f);
    for (const float z : {0.3f, 0.8f, 1.2f}) {
        EXPECT_FLOAT_EQ(SharpenFade(z, nc), 0.0f);
        EXPECT_FLOAT_EQ(RcasLobe(0.04f, 0.22f, 0.12f, 0.7f * SharpenFade(z, nc),
                                 0.5f * SharpenFade(z, nc)),
                        0.0f);
    }
    EXPECT_NEAR(SharpenFade(1.5f, nc), 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(SharpenFade(1.8f, nc), 1.0f);
    EXPECT_FLOAT_EQ(SharpenFade(30.0f, nc), 1.0f);
    EXPECT_FLOAT_EQ(SharpenFade(0.0f, nc), 1.0f);                  // the sky
    EXPECT_FLOAT_EQ(SharpenFade(0.5f, NearCode(0.0f, 0.8f)), 1.0f); // near field off
    // The two rules meet: wherever the near field's alpha is raised at all, the sharpening is
    // reduced by the same smoothstep (fade = 1 - raised / near_alpha).
    const float na = static_cast<float>((nc >> 16) & 255u) / 255.0f;
    for (float z = 1.0f; z <= 2.0f; z += 0.05f) {
        EXPECT_NEAR(SharpenFade(z, nc), 1.0f - NearAlpha(0.0f, z, nc) / na, 1e-5f);
    }
    // Far scenery keeps the full sharpening (the dark clinic's lifted lobe).
    EXPECT_LT(RcasLobe(0.04f, 0.22f, 0.12f, 0.7f * SharpenFade(5.0f, nc),
                       0.5f * SharpenFade(5.0f, nc)),
              -0.05f);
}

TEST(RenderTaa, NearFieldFilterHidesTheJitter) {
    // A hard vertical edge (0 left of x = 0.3, 1 right of it) through the 3x3, sampled at the 8
    // Halton phases: the near field is >= 80% the current frame, so its filtered value's swing over
    // the phases is the flicker. 0.45 px swings less than taa_filter_px 0.35; never below 0.45.
    EXPECT_NEAR(0.5f / NearFilterK(0.35f), 0.45f * 0.45f, 1e-5f);
    EXPECT_NEAR(0.5f / NearFilterK(0.5f), 0.5f * 0.5f, 1e-5f);
    const auto swing = [](float k) {
        float lo = 1.0f, hi = 0.0f;
        for (std::uint32_t ph = 0; ph < 8; ++ph) {
            const Jitter j = Halton(ph);
            float s = 0.0f, w = 0.0f;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    // tap q holds the scene at q + 0.5 - jitter (the output centre at 0)
                    const float sx = static_cast<float>(dx) - j.x;
                    const float v = sx > 0.3f ? 1.0f : 0.0f;
                    const float wt = FilterWeight(sx, static_cast<float>(dy) - j.y, k);
                    s += v * wt;
                    w += wt;
                }
            }
            lo = std::min(lo, s / w);
            hi = std::max(hi, s / w);
        }
        return hi - lo;
    };
    const float narrow = swing(FilterK(FilterCode(0.35f)));
    const float wide = swing(NearFilterK(0.35f));
    EXPECT_GT(narrow, 0.05f);
    EXPECT_LT(wide, narrow);
}

} // namespace
