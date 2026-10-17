// SPDX-License-Identifier: GPL-2.0-or-later

// VR body frames and hands channel (core/vr/body_frame.h, core/vr/seqlock.h,
// core/vr/body_presets.h). Covers: the tracking -> game mapping against the camera bridge's own
// 6DoF formula and its orientation counterpart, W rows -> character frame (validation, yaw), the
// anchor mapping (the character's translation cancels, a yaw after the pose copy shows as an arc),
// model <-> world <-> tracking round trips, the arm pair-sync rule, the eye of a pair-locked stereo
// pair (a strafing character under the comfort camera: both eyes see the hand where it is; the
// pair lock decided once at the pose copy and followed by the camera under real frame pacing; the
// W drift scaled by the copy interval), the default weapon grip on the bind pose, the
// tracking-loss hold (glitches checked against the runtime's velocities: real swings never held,
// spikes held, recentres not; the eased regain, one step per stereo pair), the scripted presets
// of the mock runtime, every preset solved on the c0000 reference
// pose through the mapping, and a threaded seqlock stress test (a writer at 1 kHz, four readers: no
// torn read may ever be returned). VR_BODY_REPLAY=<file> also replays a VR_BODY_RECORD file
// through the mapping and the arm solver (finite, deterministic).

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS // std::getenv, std::fopen
#endif

#include "core/vr/pose_refresh.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "c0000_test_skeleton.h"

#include "core/vr/arm_ik.h"
#include "core/vr/body_frame.h"
#include "core/vr/body_presets.h"
#include "core/vr/c0000_skeleton.h"
#include "core/vr/motion_camera.h"
#include "core/vr/seqlock.h"

namespace {

namespace B = VR::Body;
namespace C = VR::Body::C0000;
namespace P = VR::BodyPresets;
using B::Quat;
using B::Vec3;

constexpr float kDeg = B::Deg;

float Distance(Vec3 a, Vec3 b) {
    return B::Length(a - b);
}

// W rows for a character at `t` facing world yaw `yaw` (the game's convention: forward =
// (sin yaw, 0, cos yaw); the model faces -Z, so forward = -column 2).
std::array<float, 12> RowsFor(float yaw, Vec3 t) {
    const float s = std::sin(yaw), c = std::cos(yaw);
    // Columns: model +X -> (-c, 0, s), +Y -> up, +Z -> -forward.
    return {-c, 0.0f, -s, t.x, 0.0f, 1.0f, 0.0f, t.y, s, 0.0f, -c, t.z};
}

B::ChrFrame FrameFor(float yaw, Vec3 t) {
    B::ChrFrame frame{};
    B::ChrFrameFromRows(RowsFor(yaw, t), frame);
    return frame;
}

// The anchor a first-person frame leaves: the mono eye at `eye_model` in the character's model
// space, the camera placed for a head at `head`.
B::Anchor AnchorFor(const B::ChrFrame& frame, Vec3 eye_model, Vec3 head, float base_yaw,
                    float scale = 1.0f) {
    B::Anchor a{};
    a.valid = true;
    a.eye_world = B::ToWorld(frame, eye_model);
    a.chr_t = frame.t;
    a.eye_rel = a.eye_world - frame.t;
    a.head_k = head;
    a.base_yaw = base_yaw;
    a.world_scale = scale;
    return a;
}

B::TrackedHand HandAt(Vec3 p, Quat q) {
    B::TrackedHand h{};
    h.position_valid = h.position_tracked = h.orientation_valid = h.orientation_tracked = true;
    h.p = p;
    h.q = q;
    return h;
}

Quat ToQuat(const std::array<float, 4>& q) {
    return {q[0], q[1], q[2], q[3]};
}

Vec3 ToVec(const std::array<float, 3>& v) {
    return {v[0], v[1], v[2]};
}

} // namespace

TEST(BodyFrame, TrackingToWorldMatchesCameraBridge) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> angle(-3.2f, 3.2f);
    std::uniform_real_distribution<float> offset(-1.0f, 1.0f);
    std::uniform_real_distribution<float> scale(0.5f, 2.0f);
    float worst = 0.0f;
    float worst_inverse = 0.0f;
    for (int i = 0; i < 10000; ++i) {
        const float b = angle(rng);
        const float s = scale(rng);
        const Vec3 d{offset(rng), offset(rng), offset(rng)};
        // VR::OnCameraTick's render offset (vr_session.cpp), copied.
        const float cb = std::cos(b), sb = std::sin(b);
        const float r = d.x * s, f = -d.z * s;
        const Vec3 bridge{cb * r + sb * f, d.y * s, -sb * r + cb * f};
        worst = std::max(worst, Distance(B::TrackingToWorld(d, b, s), bridge));
        worst_inverse = std::max(
            worst_inverse, Distance(B::WorldToTracking(B::TrackingToWorld(d, b, s), b, s), d));
    }
    EXPECT_LT(worst, 1e-6f);
    EXPECT_LT(worst_inverse, 1e-5f);
}

TEST(BodyFrame, ForwardAndYawSign) {
    for (const float b : {0.0f, 0.4f, -1.3f, 2.9f}) {
        // Tracking forward is the game heading b.
        const Vec3 forward = B::TrackingToWorld({0.0f, 0.0f, -1.0f}, b, 1.5f);
        EXPECT_LT(Distance(forward, Vec3{std::sin(b), 0.0f, std::cos(b)} * 1.5f), 1e-6f);
        // A grip turned 30 degrees left points 30 degrees left of the heading, like the view
        // (view_yaw = base_yaw - head_yaw, head_yaw + = left).
        const Quat q = B::AxisAngle({0.0f, 1.0f, 0.0f}, 30.0f * kDeg);
        const Vec3 blade = B::Rotate(B::TrackingToWorldRotation(q, b), {0.0f, 0.0f, 1.0f});
        const float v = b - 30.0f * kDeg;
        EXPECT_LT(Distance(blade, Vec3{std::sin(v), 0.0f, std::cos(v)}), 1e-5f);
    }
    // Rotations agree with the position mapping for any grip: the image of a grip-local vector
    // v is the world rotation applied to P v (P = diag(1, 1, -1)).
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    float worst = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        const Quat q = B::Normalize(Quat{u(rng), u(rng), u(rng), u(rng)});
        const float b = 3.0f * u(rng);
        const Vec3 v{u(rng), u(rng), u(rng)};
        const Vec3 by_position = B::TrackingToWorld(B::Rotate(q, v), b, 1.0f);
        const Vec3 by_rotation = B::Rotate(B::TrackingToWorldRotation(q, b), Vec3{v.x, v.y, -v.z});
        worst = std::max(worst, Distance(by_position, by_rotation));
    }
    EXPECT_LT(worst, 1e-5f);
}

TEST(BodyFrame, ChrFrameRows) {
    for (const float yaw : {0.0f, 0.7f, -2.5f, 3.1f}) {
        B::ChrFrame frame{};
        ASSERT_TRUE(B::ChrFrameFromRows(RowsFor(yaw, {1.0f, 2.0f, 3.0f}), frame));
        EXPECT_TRUE(frame.valid);
        EXPECT_NEAR(B::WrapPi(B::ChrYaw(frame) - yaw), 0.0f, 1e-5f);
        EXPECT_LT(Distance(frame.t, Vec3{1.0f, 2.0f, 3.0f}), 1e-7f);
        // The quaternion is the same rotation as the rows.
        const Vec3 v{0.3f, -0.2f, 0.9f};
        EXPECT_LT(Distance(B::Rotate(frame.q, v), B::RotateToWorld(frame, v)), 1e-5f);
        // The model's right arm (-X) is on the character's right: forward x right = up... the
        // character's right in the world is (cos yaw, 0, -sin yaw) (the game's right axis).
        const Vec3 right = B::RotateToWorld(frame, {-1.0f, 0.0f, 0.0f});
        EXPECT_LT(Distance(right, Vec3{std::cos(yaw), 0.0f, -std::sin(yaw)}), 1e-5f);
    }
    B::ChrFrame frame{};
    auto rows = RowsFor(0.3f, {});
    rows[0] *= 1.05f; // scaled row
    EXPECT_FALSE(B::ChrFrameFromRows(rows, frame));
    EXPECT_FALSE(frame.valid);
    rows = RowsFor(0.3f, {});
    rows[1] = 0.2f; // not orthogonal
    EXPECT_FALSE(B::ChrFrameFromRows(rows, frame));
    rows = RowsFor(0.3f, {});
    for (int i = 0; i < 3; ++i) {
        rows[static_cast<size_t>(i)] = -rows[static_cast<size_t>(i)]; // a reflection
    }
    EXPECT_FALSE(B::ChrFrameFromRows(rows, frame));
    rows = RowsFor(0.3f, {});
    rows[7] = std::nanf("");
    EXPECT_FALSE(B::ChrFrameFromRows(rows, frame));
}

TEST(BodyFrame, TranslationCancels) {
    // The hand placed at the pose copy, rendered after the character (and the camera that follows
    // it) moved: it keeps its place relative to the view.
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    float worst = 0.0f;
    for (int i = 0; i < 1000; ++i) {
        const float yaw = 3.0f * u(rng);
        const B::ChrFrame k = FrameFor(yaw, {10.0f * u(rng), u(rng), 10.0f * u(rng)});
        const Vec3 eye_model{0.05f * u(rng), 1.6f, -0.1f};
        const Vec3 head{0.1f * u(rng), 1.6f + 0.1f * u(rng), 0.1f * u(rng)};
        const float b = 3.0f * u(rng);
        const B::Anchor a = AnchorFor(k, eye_model, head, b, 1.2f);
        const B::TrackedHand hand = HandAt(head + Vec3{0.3f * u(rng), -0.4f, -0.4f}, Quat{});
        const B::ModelTarget target = B::HandToModel(a, k, hand, Quat{});
        const Vec3 moved{2.0f * u(rng), 0.5f * u(rng), 2.0f * u(rng)};
        const B::ChrFrame next = FrameFor(yaw, k.t + moved);
        const Vec3 camera = a.eye_world + moved;
        const Vec3 seen = B::ToWorld(next, target.p) - camera;
        worst = std::max(worst, Distance(seen, B::TrackingToWorld(hand.p - head, b, 1.2f)));
    }
    EXPECT_LT(worst, 1e-4f);
}

TEST(BodyFrame, YawAfterCopyShowsAsArc) {
    // [U] W turning after the pose copy rotates the hand about the character's origin: the
    // W-drift probe's reason to exist. 2 degrees at a 0.5 m radius is about 1.7 cm.
    const B::ChrFrame k = FrameFor(0.5f, {3.0f, 0.0f, -2.0f});
    const B::Anchor a = AnchorFor(k, {0.0f, 1.6f, -0.1f}, {0.0f, 1.6f, 0.0f}, 0.5f);
    const B::TrackedHand hand = HandAt({0.2f, 1.3f, -0.5f}, Quat{});
    const B::ModelTarget target = B::HandToModel(a, k, hand, Quat{});
    const B::ChrFrame turned = FrameFor(0.5f + 2.0f * kDeg, k.t);
    const float error = Distance(B::ToWorld(turned, target.p), B::ToWorld(k, target.p));
    const float radius = std::hypot(target.p.x, target.p.z);
    EXPECT_NEAR(error, 2.0f * std::sin(1.0f * kDeg) * radius, 1e-5f);
    const B::FrameDrift drift = B::Drift(k, turned);
    EXPECT_NEAR(drift.yaw, 2.0f * kDeg, 1e-5f);
    EXPECT_NEAR(drift.angle, 2.0f * kDeg, 1e-4f);
    EXPECT_NEAR(drift.distance, 0.0f, 1e-6f);
}

TEST(BodyFrame, RoundTrips) {
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    float worst_world = 0.0f;
    float worst_tracking = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        const B::ChrFrame frame = FrameFor(3.0f * u(rng), {50.0f * u(rng), u(rng), 50.0f * u(rng)});
        const Vec3 m{u(rng), 2.0f * u(rng), u(rng)};
        worst_world = std::max(worst_world, Distance(B::ToModel(frame, B::ToWorld(frame, m)), m));
        const B::Anchor a = AnchorFor(frame, {0.0f, 1.6f, -0.1f}, {u(rng), 1.6f, u(rng)},
                                      3.0f * u(rng), 0.8f + 0.4f * (u(rng) + 1.0f));
        const B::TrackedHand hand = HandAt(a.head_k + Vec3{u(rng), u(rng), u(rng)} * 0.6f, Quat{});
        const B::ModelTarget target = B::HandToModel(a, frame, hand, Quat{});
        worst_tracking =
            std::max(worst_tracking, Distance(B::ModelToTracking(a, frame, target.p), hand.p));
    }
    EXPECT_LT(worst_world, 1e-4f);
    EXPECT_LT(worst_tracking, 1e-4f);
}

TEST(BodyFrame, ReuseHandsTruthTable) {
    // The second eye of any stereo pair (AER or stereo_mode 0) takes its first eye's hand sample;
    // mono and DIBR start a pair every frame.
    B::Anchor a{};
    for (const bool paired : {false, true}) {
        for (const bool pair_first : {false, true}) {
            for (const bool aer : {false, true}) {
                a.paired = paired;
                a.pair_first = pair_first;
                a.aer = aer;
                a.eye = aer ? (pair_first ? 0 : 1) : -1;
                EXPECT_EQ(B::ReuseHands(a), paired && pair_first);
            }
        }
    }
}

TEST(BodyFrame, NextEyeRelSources) {
    const B::ChrFrame now = FrameFor(0.3f, {4.0f, 1.0f, -2.0f});
    B::Anchor last = AnchorFor(FrameFor(0.3f, {3.9f, 1.0f, -2.0f}), {0.0f, 1.6f, -0.1f},
                               {0.0f, 1.6f, 0.0f}, 0.3f);
    last.steady_ns = 1'000'000'000;
    B::Anchor first = last;
    first.paired = true;
    first.pair_first = true;
    first.eye_rel = {0.01f, 1.6f, 0.02f};
    first.steady_ns = last.steady_ns - 16'666'667;
    const Vec3 drift{0.02f, 0.0f, 0.0f};
    const std::int64_t soon = last.steady_ns + 10'000'000;
    B::EyeSource source{};
    // Mono / DIBR: always the anchor's own eye.
    last.paired = false;
    EXPECT_LT(Distance(B::NextEyeRel(last, &first, now, soon, drift, source), last.eye_rel), 1e-7f);
    EXPECT_TRUE(source == B::EyeSource::Anchor);
    // A pair's first eye: the next frame is its locked second eye, the frozen eye against W then.
    last.paired = true;
    last.pair_first = true;
    last.lock_ns = 25'000'000;
    EXPECT_LT(Distance(B::NextEyeRel(last, &first, now, soon, drift, source),
                       last.eye_world - now.t - drift),
              1e-6f);
    EXPECT_TRUE(source == B::EyeSource::Locked);
    // Too late for the lock, the lock off or no valid W: that second eye follows the body.
    for (const auto& [lock, at, frame] :
         {std::tuple{std::int64_t{25'000'000}, last.steady_ns + 30'000'000, now},
          std::tuple{std::int64_t{0}, soon, now}, std::tuple{last.lock_ns, soon, B::ChrFrame{}}}) {
        last.lock_ns = lock;
        EXPECT_LT(Distance(B::NextEyeRel(last, &first, frame, at, drift, source), last.eye_rel),
                  1e-7f);
        EXPECT_TRUE(source == B::EyeSource::Anchor);
    }
    // A pair's second eye: the next frame starts a pair, with the camera following the body like
    // the last pair's first eye.
    last.pair_first = false;
    EXPECT_LT(Distance(B::NextEyeRel(last, &first, now, soon, drift, source), first.eye_rel),
              1e-7f);
    EXPECT_TRUE(source == B::EyeSource::PairFirst);
    // Without a recent pair-first anchor: the anchor's own eye.
    first.steady_ns = last.steady_ns - 200'000'000;
    EXPECT_LT(Distance(B::NextEyeRel(last, &first, now, soon, drift, source), last.eye_rel), 1e-7f);
    EXPECT_TRUE(source == B::EyeSource::Anchor);
    EXPECT_LT(Distance(B::NextEyeRel(last, nullptr, now, soon, drift, source), last.eye_rel),
              1e-7f);
}

namespace {

// One run of the stereo pair simulation below.
struct PairRun {
    int mode{1};        // stereo_mode: 1 AER, 0 shifted frames, 2 DIBR (no pairs)
    bool late_w{false}; // W moves after the pose copy (the copy reads last frame's W)
    bool decide{true};  // the camera follows the body's pair lock decision
    // W steps once per game frame, at the pose copy (the loop thread: the next copy only after
    // this frame's camera), instead of moving on until the camera.
    bool w_per_frame{false};
    int frames{120};
    int skip{30}; // frames before the error is measured
    std::function<double(int)> frame_ms{[](int) { return 1000.0 / 60.0; }};
    std::function<double(int)> gap_ms{[](int) { return 4.0; }}; // pose copy -> render camera
};

struct PairStats {
    std::array<float, 2> worst{};       // the hand error, first eye (or mono) / second eye, m
    std::array<float, 2> worst_plain{}; // the same with the plain anchor eye_rel
    int over_20mm{0};                   // second-eye frames more than 20 mm off
    int second{0};                      // second-eye frames measured
    int decided{0};                     // second-eye ticks that followed the body's decision
};

// A character strafing at 4.5 m/s under the dodge comfort camera with the pair lock
// (core/vr/motion_camera.h). Each frame: the pose copy (the pose hook's Gather: EyeTracker places
// the hand for the eye that frame is rendered from and decides a second eye's pair lock), then
// the render camera `gap_ms` later (following that decision, as ComfortEye passes it) and its
// anchor (BodyRenderTick: the lock window 1.5 game frames from the session's frame-time EMA). The
// error: where the frame shows the hand against TrackingToWorld(hand - head) from its eye.
PairStats SimulatePairs(const PairRun& run) {
    namespace M = VR::Motion;
    M::ComfortCamera camera{};
    M::ComfortConfig cfg{};
    cfg.pair_lock = true;
    const M::Phase phase{};
    const float yaw = 0.4f;
    const Vec3 velocity = Vec3{std::cos(yaw), 0.0f, -std::sin(yaw)} * 4.5f; // to the right
    const Vec3 head{0.05f, 1.6f, 0.02f};
    const B::TrackedHand hand = HandAt(head + Vec3{0.25f, -0.35f, -0.4f}, Quat{});
    const Vec3 want = B::TrackingToWorld(hand.p - head, yaw, 1.0f);
    B::EyeTracker eyes{};
    B::Anchor last{};
    std::optional<B::PairLock> published;
    float frame_s = 1.0f / 60.0f; // VR::GameFrameSeconds
    double t = 1.0, t_render_prev = 1.0;
    Vec3 c_render{5.0f, 0.0f, 3.0f};
    PairStats stats{};
    for (int k = 0; k < run.frames; ++k) {
        double t_copy = t + run.frame_ms(k) * 1e-3;
        while (run.w_per_frame && t_copy < t_render_prev + 0.001) {
            t_copy += 1.0 / 60.0;
        }
        const double t_render = t_copy + run.gap_ms(k) * 1e-3;
        const Vec3 c_prev = c_render;
        c_render =
            c_render +
            velocity * static_cast<float>(run.w_per_frame ? t_copy - t : t_render - t_render_prev);
        t = t_copy;
        const auto copy_ns = static_cast<std::int64_t>(t * 1e9);
        const auto render_ns = static_cast<std::int64_t>(t_render * 1e9);
        const float render_dt =
            std::clamp(static_cast<float>(t_render - t_render_prev), 0.0f, 0.1f);
        t_render_prev = t_render;
        const B::ChrFrame w_copy = FrameFor(yaw, run.late_w ? c_prev : c_render);
        // Pose copy k.
        std::optional<B::ModelTarget> target, plain;
        const B::EyePlacement eye = eyes.Place(last.valid ? &last : nullptr, w_copy, copy_ns);
        if (last.valid) {
            B::Anchor a = last;
            a.eye_rel = eye.eye_rel;
            target = B::HandToModel(a, w_copy, hand, Quat{});
            plain = B::HandToModel(last, w_copy, hand, Quat{});
        }
        if (eye.decided) {
            published = eye.lock;
        }
        // Render tick k.
        const bool pair_first = run.mode == 2 || k % 2 == 0;
        frame_s += (std::clamp(render_dt, 1.0f / 60.0f, 1.0f / 15.0f) - frame_s) *
                   (1.0f - std::exp(-render_dt / 0.5f));
        cfg.pair_max_ns = static_cast<std::int64_t>(1.5 * frame_s * 1e9);
        M::LockDecision lock{};
        if (run.decide && published) {
            lock = {true, published->first_ns, published->decided_ns, published->locked};
        }
        const M::ComfortOut out = camera.Step({c_render.x, c_render.y + 1.6f, c_render.z},
                                              pair_first, render_ns, phase, cfg, nullptr, &lock);
        const Vec3 eye_pos{out.position.x, out.position.y, out.position.z};
        if (target && k >= run.skip) {
            const B::ChrFrame w_render = FrameFor(yaw, c_render);
            const size_t e = pair_first ? 0 : 1;
            const float error = Distance(B::ToWorld(w_render, target->p) - eye_pos, want);
            stats.worst[e] = std::max(stats.worst[e], error);
            stats.worst_plain[e] = std::max(
                stats.worst_plain[e], Distance(B::ToWorld(w_render, plain->p) - eye_pos, want));
            if (e == 1) {
                ++stats.second;
                stats.over_20mm += error > 0.02f ? 1 : 0;
                stats.decided += out.decided ? 1 : 0;
            }
        }
        // Its anchor (taken after the camera).
        last = {};
        last.valid = true;
        last.steady_ns = render_ns + 100'000;
        last.eye_world = eye_pos;
        last.chr_t = c_render;
        last.eye_rel = eye_pos - c_render;
        last.head_k = head;
        last.base_yaw = yaw;
        last.pair_first = pair_first;
        last.paired = run.mode != 2;
        last.lock_ns = run.mode != 2 ? static_cast<std::int64_t>(1.5 * frame_s * 1e9) : 0;
    }
    return stats;
}

} // namespace

TEST(BodyFrame, PairLockedEyesSeeTheHandWhereItIs) {
    // Steady 60 Hz, the camera 4 ms after the pose copy: both eyes must see the hand where it is
    // (under 5 mm); the plain anchor eye_rel puts it v * dt (7.5 cm) off, in opposite directions
    // in the two eyes. W final at the pose copy, and W moved after it (the copy reads last frame's
    // W: the drift prediction covers it). Mode 2 (DIBR) starts a pair every frame and keeps the
    // anchor's eye.
    for (const bool late_w : {false, true}) {
        for (const int mode : {1, 0, 2}) {
            PairRun run{};
            run.mode = mode;
            run.late_w = late_w;
            const PairStats r = SimulatePairs(run);
            std::printf("  mode %d%s: hand error first eye %.2f mm, second eye %.2f mm (plain "
                        "anchor %.1f / %.1f mm)\n",
                        mode, late_w ? " (W after the copy)" : "", r.worst[0] * 1000.0f,
                        r.worst[1] * 1000.0f, r.worst_plain[0] * 1000.0f,
                        r.worst_plain[1] * 1000.0f);
            EXPECT_LT(r.worst[0], 0.005f);
            if (mode != 2) {
                EXPECT_LT(r.worst[1], 0.005f);
                EXPECT_GT(std::max(r.worst_plain[0], r.worst_plain[1]), 0.05f);
                EXPECT_EQ(r.decided, r.second);
            }
        }
    }
}

TEST(BodyFrame, PairLockIsDecidedOnceUnderRealFramePacing) {
    // Vblank-paced frames (60 and 30 fps, 5% missed vblanks, +-0.3 ms timing noise), the session's
    // frame-time EMA, the camera 3, 5 or 8 ms after the pose copy, AER and stereo_mode 0. The body
    // decides a second eye's lock at its pose copy and the camera follows that decision, so no
    // second eye is ever placed for one eye and rendered from another: none more than 20 mm off.
    // With the camera deciding on its own clock (its tick, not the copy) second eyes whose
    // interval falls between the lock window and the window plus the gap get the hands 150 mm off.
    for (const int mode : {1, 0}) {
        for (const double nominal : {1000.0 / 60.0, 1000.0 / 30.0}) {
            for (const double gap : {3.0, 5.0, 8.0}) {
                for (const bool late_w : {false, true}) {
                    std::mt19937 rng(23);
                    std::uniform_real_distribution<double> u01(0.0, 1.0);
                    std::vector<double> frame, copy_gap;
                    for (int k = 0; k < 3000; ++k) {
                        frame.push_back(nominal + (u01(rng) < 0.05 ? 1000.0 / 60.0 : 0.0) +
                                        0.6 * (u01(rng) - 0.5));
                        copy_gap.push_back(gap + 0.6 * (u01(rng) - 0.5));
                    }
                    PairRun run{};
                    run.mode = mode;
                    run.late_w = late_w;
                    run.frames = 3000;
                    run.skip = 60;
                    run.frame_ms = [&frame](int k) { return frame[static_cast<size_t>(k)]; };
                    run.gap_ms = [&copy_gap](int k) { return copy_gap[static_cast<size_t>(k)]; };
                    const PairStats r = SimulatePairs(run);
                    EXPECT_EQ(r.over_20mm, 0) << "mode " << mode << " gap " << gap;
                    EXPECT_EQ(r.decided, r.second);
                    if (nominal < 20.0 && gap == 8.0 && !late_w) {
                        run.decide = false;
                        const PairStats own = SimulatePairs(run);
                        std::printf("  mode %d, 60 fps, gap 8 ms: second eyes over 20 mm %d of %d "
                                    "(worst %.1f mm); the camera on its own clock: %d (worst %.1f "
                                    "mm)\n",
                                    mode, r.over_20mm, r.second, r.worst[1] * 1000.0f,
                                    own.over_20mm, own.worst[1] * 1000.0f);
                        EXPECT_GT(own.over_20mm, 0);
                    }
                }
            }
        }
    }
}

TEST(BodyFrame, EyeTrackerAfterALongSecondEye) {
    // A hitch of 150 ms between the two eyes of a pair (past PairFirstMaxNs). The frame after a
    // second eye this tracker locked (the old camera always locks) is placed for the pair's first
    // eye, not for that frozen second eye (v * 150 ms off); after a second eye it did not lock
    // (it moved with the body), its own eye still holds.
    constexpr std::int64_t Ms = 1'000'000;
    const B::ChrFrame w0 = FrameFor(0.4f, {5.0f, 0.0f, 3.0f});
    const B::ChrFrame w1 = FrameFor(0.4f, {5.6f, 0.0f, 3.0f}); // 4 m/s for 150 ms
    for (const bool locked : {true, false}) {
        B::EyeTracker eyes{};
        B::Anchor first = AnchorFor(w0, {0.0f, 1.6f, -0.1f}, {0.0f, 1.6f, 0.0f}, 0.4f);
        first.steady_ns = 1000 * Ms;
        first.paired = true;
        first.pair_first = true;
        first.lock_ns = locked ? std::numeric_limits<std::int64_t>::max() : 25 * Ms;
        eyes.NoteCopy(w0, 990 * Ms);
        // The second eye's pose copy, 150 ms after the first eye: decided once.
        const B::EyePlacement second = eyes.Place(&first, w1, 1150 * Ms);
        ASSERT_TRUE(second.decided);
        EXPECT_EQ(second.lock.locked, locked);
        EXPECT_EQ(second.lock.first_ns, first.steady_ns);
        EXPECT_EQ(second.lock.decided_ns, 1150 * Ms);
        // Its anchor: locked at the first eye's world position, or moved with the body.
        B::Anchor anchor = first;
        anchor.steady_ns = 1155 * Ms;
        anchor.pair_first = false;
        anchor.chr_t = w1.t;
        if (locked) {
            anchor.eye_rel = first.eye_world - w1.t;
        } else {
            anchor.eye_world = first.eye_world + (w1.t - w0.t);
        }
        const B::EyePlacement next = eyes.Place(&anchor, w1, 1170 * Ms);
        EXPECT_FALSE(next.decided);
        if (locked) {
            EXPECT_TRUE(next.source == B::EyeSource::PairFirst);
            EXPECT_LT(Distance(next.eye_rel, first.eye_rel), 1e-6f);
        } else {
            EXPECT_TRUE(next.source == B::EyeSource::Anchor);
            EXPECT_LT(Distance(next.eye_rel, anchor.eye_rel), 1e-6f);
        }
    }
}

TEST(BodyFrame, DriftFollowsTheCopyInterval) {
    // W moved after the pose copy (the drift prediction in use) and one hitch (25-120 ms) before a
    // pair-first frame: the drift measured over the hitch is scaled to the next copy interval, so
    // the next locked second eye keeps its hand (unscaled: speed x (hitch - 16.7 ms), 37-465 mm).
    for (const double hitch : {25.0, 33.3, 50.0, 120.0}) {
        PairRun run{};
        run.late_w = true;
        run.frames = 200;
        run.skip = 40;
        run.frame_ms = [hitch](int k) { return k == 102 ? hitch : 1000.0 / 60.0; };
        const PairStats r = SimulatePairs(run);
        EXPECT_LT(r.worst[1], 0.005f) << "hitch " << hitch;
        EXPECT_EQ(r.over_20mm, 0);
    }
}

TEST(BodyFrame, DriftCoversStallsBetweenCopyAndCamera) {
    // W steps once per game frame and moves after the pose copy; the camera comes 0.5-12 ms after
    // the copy, 5% of the time one or two vblanks later (a stall: the next copy only after that
    // camera), with 5% missed vblanks and +-0.3 ms noise, AER and stereo_mode 0. The copy after a
    // stall comes up to 3x the last copy interval later while its first eye's anchor is recent
    // (the eye locks): the drift scaled by that ratio (clamped 0..4) keeps every locked second
    // eye's hand where it is (clamped 0..2: 79 mm off at 4.5 m/s).
    for (const int mode : {1, 0}) {
        std::mt19937 rng(101 + static_cast<unsigned>(mode));
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        std::vector<double> frame, gap;
        for (int k = 0; k < 8000; ++k) {
            frame.push_back(1000.0 / 60.0 + (u01(rng) < 0.05 ? 1000.0 / 60.0 : 0.0) +
                            (u01(rng) - 0.5) * 0.6);
            double g = 0.5 + u01(rng) * 11.5;
            const double stall = u01(rng);
            g += stall < 0.025 ? 1000.0 / 60.0 : stall < 0.05 ? 2000.0 / 60.0 : 0.0;
            gap.push_back(g);
        }
        PairRun run{};
        run.mode = mode;
        run.late_w = true;
        run.w_per_frame = true;
        run.frames = 8000;
        run.skip = 60;
        run.frame_ms = [&frame](int k) { return frame[static_cast<size_t>(k)]; };
        run.gap_ms = [&gap](int k) { return gap[static_cast<size_t>(k)]; };
        const PairStats r = SimulatePairs(run);
        std::printf("  mode %d, stalls between copy and camera: second eye at most %.2f mm off (%d "
                    "second eyes)\n",
                    mode, r.worst[1] * 1000.0f, r.second);
        EXPECT_LT(r.worst[1], 0.005f) << "mode " << mode;
        EXPECT_EQ(r.over_20mm, 0);
        EXPECT_EQ(r.decided, r.second);
    }
}

TEST(BodyFrame, DefaultGripBindPose) {
    // The controller pose that holds the weapon bone at its bind rotation: blade forward (within
    // 25 degrees), grip top in the upper half. [U] The exact axes are checked in the headset.
    for (const int side : {C::Left, C::Right}) {
        const int bone = side == C::Right ? C::R_Weapon : C::L_Weapon;
        const Quat weapon = ToQuat(C::Test::TestSkeleton().model[static_cast<size_t>(bone)].q);
        const Quat grip = B::DefaultGrip(side, 0.0f);
        EXPECT_NEAR(B::Dot(grip, grip), 1.0f, 1e-5f);
        const Quat controller = B::Normalize(weapon * B::Conjugate(grip));
        const Vec3 blade = B::Rotate(controller, {0.0f, 0.0f, 1.0f});
        const Vec3 up = B::Rotate(controller, {0.0f, 1.0f, 0.0f});
        EXPECT_LT(B::Angle(blade, {0.0f, 0.0f, -1.0f}), 25.0f * kDeg);
        EXPECT_GT(up.y, 0.3f);
        // The weapon's blade (-Y) is the controller blade, its +X the controller's up.
        EXPECT_LT(Distance(B::Rotate(grip, {0.0f, -1.0f, 0.0f}), {0.0f, 0.0f, 1.0f}), 1e-5f);
        EXPECT_LT(Distance(B::Rotate(grip, {1.0f, 0.0f, 0.0f}), {0.0f, 1.0f, 0.0f}), 1e-5f);
    }
    // blade_pitch_deg tilts the blade toward grip +Y (game frame +Y); the trims.
    const Vec3 pitched = B::Rotate(B::DefaultGrip(C::Right, 20.0f), {0.0f, -1.0f, 0.0f});
    EXPECT_LT(Distance(pitched, {0.0f, std::sin(20.0f * kDeg), std::cos(20.0f * kDeg)}), 1e-5f);
    const Vec3 up_trim =
        B::Rotate(B::DefaultGrip(C::Right, 0.0f, 0.0f, 10.0f), {0.0f, -1.0f, 0.0f});
    EXPECT_NEAR(up_trim.y, std::sin(10.0f * kDeg), 1e-5f);
    const Vec3 right_yaw = B::Rotate(B::DefaultGrip(C::Right, 0.0f, 10.0f), {0.0f, -1.0f, 0.0f});
    const Vec3 left_yaw = B::Rotate(B::DefaultGrip(C::Left, 0.0f, 10.0f), {0.0f, -1.0f, 0.0f});
    EXPECT_GT(right_yaw.x, 0.1f); // outward: the right hand's right
    EXPECT_NEAR(left_yaw.x, -right_yaw.x, 1e-5f);
}

TEST(BodyFrame, HandHold) {
    constexpr std::int64_t Ms = 1'000'000;
    B::HandHold hold{};
    B::TrackedHand out{};
    const B::TrackedHand good = HandAt({0.1f, 1.2f, -0.3f}, Quat{});
    EXPECT_EQ(hold.Update(B::TrackedHand{}, 0, 300 * Ms, out), B::HandUse::Lost);
    EXPECT_EQ(hold.Update(good, 10 * Ms, 300 * Ms, out), B::HandUse::Tracked);
    B::TrackedHand inferred = good;
    inferred.position_tracked = false;
    inferred.p = {9.0f, 9.0f, 9.0f}; // an inferred position is never used
    inferred.q = B::AxisAngle({1.0f, 0.0f, 0.0f}, 0.5f);
    EXPECT_EQ(hold.Update(inferred, 200 * Ms, 300 * Ms, out), B::HandUse::Held);
    EXPECT_LT(Distance(out.p, good.p), 1e-7f);
    EXPECT_NEAR(B::AngleBetween(out.q, inferred.q), 0.0f, 1e-5f); // the IMU orientation
    EXPECT_EQ(hold.Update(inferred, 310 * Ms, 300 * Ms, out), B::HandUse::Held);
    EXPECT_EQ(hold.Update(inferred, 311 * Ms, 300 * Ms, out), B::HandUse::Lost);
    EXPECT_EQ(hold.Update(good, 400 * Ms, 300 * Ms, out), B::HandUse::Tracked);
}

TEST(BodyFrame, HandHoldRejectsGlitchesAndEasesTheRegain) {
    constexpr std::int64_t Ms = 1'000'000;
    constexpr std::int64_t Frame = 16'666'667;
    constexpr std::int64_t Hold = 300 * Ms;
    B::HandHold hold{};
    B::TrackedHand out{};
    const Vec3 p0{0.2f, 1.2f, -0.4f};
    // A sample with the runtime's linear velocity `v`.
    const auto moving = [](Vec3 p, Vec3 v) {
        B::TrackedHand h = HandAt(p, Quat{});
        h.velocity_valid = true;
        h.v = v;
        return h;
    };
    std::int64_t t = 1000 * Ms;
    // A fast swing (12 m/s) is never rejected (the sample time is the check's time base); then the
    // hand stops.
    Vec3 p = p0;
    for (int f = 0; f < 10; ++f, t += Frame) {
        p = p0 + Vec3{12.0f * static_cast<float>(f) / 60.0f, 0.0f, 0.0f};
        EXPECT_EQ(hold.Update(moving(p, {12.0f, 0.0f, 0.0f}), t, Hold, out, t),
                  B::HandUse::Tracked);
        EXPECT_FALSE(hold.rejected);
        EXPECT_LT(Distance(out.p, p), 1e-6f);
    }
    for (int f = 0; f < 3; ++f, t += Frame) {
        EXPECT_EQ(hold.Update(moving(p, {}), t, Hold, out, t), B::HandUse::Tracked);
    }
    // A one-frame 1 m jump while the runtime's velocity says the hand is still: held over, its
    // orientation too; the next good sample goes on (a few mm from the hold, eased).
    B::TrackedHand glitch = moving(p + Vec3{0.0f, 1.0f, 0.0f}, {});
    glitch.q = B::AxisAngle({1, 0, 0}, 1.0f);
    EXPECT_EQ(hold.Update(glitch, t, Hold, out, t), B::HandUse::Held);
    EXPECT_TRUE(hold.rejected);
    EXPECT_LT(Distance(out.p, p), 1e-6f);
    EXPECT_NEAR(B::AngleBetween(out.q, Quat{}), 0.0f, 1e-5f);
    t += Frame;
    const Vec3 next = p + Vec3{0.005f, 0.0f, 0.0f};
    EXPECT_EQ(hold.Update(moving(next, {}), t, Hold, out, t), B::HandUse::Tracked);
    EXPECT_LT(Distance(out.p, next), 0.005f);
    t += Frame;
    // The same sample time again (a stereo pair's second eye reusing its sample) is no motion.
    EXPECT_EQ(hold.Update(moving(next, {}), t, Hold, out, t - Frame), B::HandUse::Tracked);
    // A lasting 2 m jump, the velocity still saying still: held while it is too far for the time
    // since the last accepted sample (one frame: 33 ms after it the tolerance is 1.53 m, 50 ms
    // after it 2.38 m), then eased in over three updates; never lost.
    const Vec3 far = next + Vec3{2.0f, 0.0f, 0.0f};
    int held = 0;
    Vec3 held_p{};
    std::vector<Vec3> shown;
    for (int f = 0; f < 12; ++f) {
        t += Frame;
        const B::HandUse use = hold.Update(moving(far, {}), t, Hold, out, t);
        EXPECT_TRUE(use != B::HandUse::Lost);
        if (use == B::HandUse::Held) {
            ++held;
            held_p = out.p;
            EXPECT_LT(Distance(out.p, next), 0.005f);
        } else {
            shown.push_back(out.p);
        }
    }
    EXPECT_EQ(held, 1);
    ASSERT_GE(shown.size(), size_t{3});
    EXPECT_LT(Distance(shown[0], B::Lerp(held_p, far, 1.0f / 3.0f)), 1e-5f);
    EXPECT_LT(Distance(shown[1], B::Lerp(held_p, far, 2.0f / 3.0f)), 1e-5f);
    EXPECT_LT(Distance(shown[2], far), 1e-6f);
    // A short loss (0.1 s) while the hand moves 0.25 m: the regained hand eases from the held pose
    // over three updates (1/3, 2/3, all of the way), its rotation too.
    const Vec3 a = far;
    const Vec3 b = far + Vec3{0.0f, 0.0f, -0.25f};
    const Quat qb = B::AxisAngle({0.0f, 1.0f, 0.0f}, 0.6f);
    B::TrackedHand lost = HandAt(b, qb);
    lost.position_tracked = false;
    lost.orientation_valid = false;
    for (int f = 0; f < 6; ++f) {
        t += Frame;
        EXPECT_EQ(hold.Update(lost, t, Hold, out, t), B::HandUse::Held);
        EXPECT_LT(Distance(out.p, a), 1e-6f);
    }
    for (int f = 1; f <= 4; ++f) {
        t += Frame;
        EXPECT_EQ(hold.Update(HandAt(b, qb), t, Hold, out, t), B::HandUse::Tracked);
        const float k = std::min(static_cast<float>(f), 3.0f) / 3.0f;
        EXPECT_LT(Distance(out.p, B::Lerp(a, b, k)), 1e-5f);
        EXPECT_NEAR(B::AngleBetween(out.q, qb), 0.6f * (1.0f - k), 1e-3f);
    }
    // A loss longer than the hold: lost; the first sample after it is never rejected.
    for (int f = 0; f < 20; ++f) {
        t += Frame;
        hold.Update(lost, t, Hold, out, t);
    }
    EXPECT_EQ(hold.Update(lost, t + Frame, Hold, out, t + Frame), B::HandUse::Lost);
    t += 2 * Frame;
    B::TrackedHand back = moving(b + Vec3{2.0f, 0.0f, 0.0f}, {});
    back.q = qb;
    EXPECT_EQ(hold.Update(back, t, Hold, out, t), B::HandUse::Tracked);
    EXPECT_FALSE(hold.rejected);
    // Without the runtime's velocities nothing tells its extrapolation from a glitch: a 2 m jump
    // is taken at once.
    B::HandHold bare{};
    EXPECT_EQ(bare.Update(HandAt(p0, Quat{}), t, Hold, out, t), B::HandUse::Tracked);
    t += Frame;
    EXPECT_EQ(bare.Update(HandAt(p0 + Vec3{2.0f, 0.0f, 0.0f}, Quat{}), t, Hold, out, t),
              B::HandUse::Tracked);
    EXPECT_FALSE(bare.rejected);
}

namespace {

// A controller path (tracking space, metres), its velocity (the runtime's linear velocity) and
// acceleration.
struct HandPath {
    std::function<Vec3(double)> p;
    Vec3 V(double t) const {
        const double h = 1e-5;
        return (p(t + h) - p(t - h)) * static_cast<float>(1.0 / (2.0 * h));
    }
    Vec3 A(double t) const {
        const double h = 5e-4;
        return (p(t + h) - p(t) * 2.0f + p(t - h)) * static_cast<float>(1.0 / (h * h));
    }
};

// A minimum-jerk swing over `distance` metres along `dir` peaking at `speed` m/s, from `t0`.
HandPath LinearSwing(Vec3 from, Vec3 dir, double distance, double speed, double t0) {
    const double length = 1.875 * distance / speed; // minimum jerk: peak 1.875 x the mean speed
    return {[=](double t) {
        const double s = P::MinJerk(std::clamp((t - t0) / length, 0.0, 1.0));
        return from + dir * static_cast<float>(distance * s);
    }};
}

// A minimum-jerk arc of `radius` about `centre` in the plane (u, w), `angle` radians, peaking at
// `speed` m/s along the arc.
HandPath ArcSwing(Vec3 centre, Vec3 u, Vec3 w, double radius, double angle, double speed,
                  double t0) {
    const double length = 1.875 * angle * radius / speed;
    return {[=](double t) {
        const double a = angle * P::MinJerk(std::clamp((t - t0) / length, 0.0, 1.0)) - 0.5 * angle;
        return centre + u * static_cast<float>(radius * std::cos(a)) +
               w * static_cast<float>(radius * std::sin(a));
    }};
}

// A swing along `dir` that speeds up from rest to `speed` over 0.45 m (peak 1.7 x speed^2 m/s^2)
// and stops within `stop_s` (the velocity falling as (1 - u)^2: at 10 ms from 25 m/s an impact,
// 5000 m/s^2), from `t0`; then it rests.
HandPath StopSwing(Vec3 from, Vec3 dir, double speed, double stop_s, double t0) {
    const double ramp = 0.9 / speed;
    return {[=](double t) {
        const double s = t - t0;
        double x = 0.0;
        if (s >= ramp + stop_s) {
            x = speed * ramp * 0.5 + speed * stop_s / 3.0;
        } else if (s >= ramp) {
            const double u = 1.0 - (s - ramp) / stop_s;
            x = speed * ramp * 0.5 + speed * stop_s * (1.0 - u * u * u) / 3.0;
        } else if (s > 0.0) {
            const double u = s / ramp;
            x = speed * ramp * (u * u * u - 0.5 * u * u * u * u);
        }
        return from + dir * static_cast<float>(x);
    }};
}

// A waggle of `amplitude` metres at `hz` about `centre`: along `u`, or round a circle in the plane
// of `u` and `w`; eased in over 60 ms from rest at `t0`.
HandPath Waggle(Vec3 centre, Vec3 u, Vec3 w, double amplitude, double hz, bool circle, double t0) {
    return {[=](double t) {
        const double s = std::max(t - t0, 0.0);
        const double ease = s >= 0.06 ? 1.0 : P::MinJerk(s / 0.06);
        const double phase = 2.0 * 3.14159265358979 * hz * s;
        const double a = std::sin(phase), b = circle ? 1.0 - std::cos(phase) : 0.0;
        return centre + (u * static_cast<float>(a) + w * static_cast<float>(b)) *
                            static_cast<float>(amplitude * ease);
    }};
}

// The runtime's sample of `path` at `t` (tracked, with its linear velocity).
B::TrackedHand SampleOf(const HandPath& path, double t) {
    B::TrackedHand h = HandAt(path.p(t), Quat{});
    h.velocity_valid = true;
    h.v = path.V(t);
    return h;
}

// How the runtime predicts a grip located at `s` for `s + horizon`.
enum class Predictor { Exact, ConstantVelocity, ConstantAcceleration };

// That prediction, with the velocity the runtime reports with it (the extrapolation's own).
B::TrackedHand PredictedSample(const HandPath& path, Predictor predictor, double s,
                               double horizon) {
    const auto h = static_cast<float>(horizon);
    switch (predictor) {
    case Predictor::Exact:
        return SampleOf(path, s + horizon);
    case Predictor::ConstantVelocity: {
        B::TrackedHand sample = SampleOf(path, s);
        sample.p = sample.p + sample.v * h;
        return sample;
    }
    case Predictor::ConstantAcceleration: {
        B::TrackedHand sample = SampleOf(path, s);
        const Vec3 a = path.A(s);
        sample.p = sample.p + sample.v * h + a * (0.5f * h * h);
        sample.v = sample.v + a * h;
        return sample;
    }
    }
    return SampleOf(path, s);
}

constexpr double XrPeriod = 1.0 / 120.0; // the runtime's frames
constexpr double GameFrame = 1.0 / 60.0;

// The newest XR frame time at game time `t`.
double XrFrameAt(double t, double period = XrPeriod) {
    return std::floor(t / period + 1e-9) * period;
}

std::int64_t Ns(double seconds) {
    return static_cast<std::int64_t>(std::llround(seconds * 1e9));
}

} // namespace

TEST(BodyFrame, HandHoldNeverHoldsRealSwings) {
    // Melee swings as fast as VR players make them (16-20 m/s peaks are normal; up to 25 m/s
    // here): a 0.9 m straight minimum-jerk swing, 0.6 and 0.4 m radius arcs through 150 degrees,
    // the runtime at 120 Hz with its own velocities, the game at 60 Hz in mono / DIBR (a new
    // sample every frame) and in stereo pairs (the second eye reuses its first eye's sample), at
    // 40 phases against the frames: never held, the weapon always on the controller.
    const B::HeadPose head{true, {0.0f, 1.6f, 0.0f}, Quat{}};
    const Vec3 start{-0.2f, 1.6f, -0.35f};
    const Vec3 down = B::Normalize(Vec3{1.0f, -1.0f, -0.3f});
    int frames = 0, held = 0;
    float worst = 0.0f;
    for (const double speed : {16.0, 20.0, 25.0}) {
        const std::array<HandPath, 3> paths{LinearSwing(start, down, 0.9, speed, 0.2),
                                            ArcSwing({0.2f, 1.4f, -0.1f}, {0.0f, 0.0f, -1.0f},
                                                     {0.0f, 1.0f, 0.0f}, 0.6, 2.6, speed, 0.2),
                                            ArcSwing({0.2f, 1.2f, -0.2f}, {0.0f, 0.0f, -1.0f},
                                                     {1.0f, 0.0f, 0.0f}, 0.4, 2.6, speed, 0.2)};
        for (const HandPath& path : paths) {
            for (const bool pairs : {false, true}) {
                for (int phase = 0; phase < 40; ++phase) {
                    B::HandHold hold{};
                    double sample_t = 0.0;
                    for (int f = 0; f < 36; ++f) {
                        const double t = static_cast<double>(phase) / 40.0 * GameFrame +
                                         static_cast<double>(f) * GameFrame;
                        if (!pairs || f % 2 == 0) {
                            sample_t = XrFrameAt(t);
                        }
                        const B::TrackedHand in = SampleOf(path, sample_t);
                        B::TrackedHand out{};
                        const B::HandUse use =
                            hold.Update(in, Ns(t), 300'000'000, out, Ns(sample_t), head);
                        ++frames;
                        held += use != B::HandUse::Tracked ? 1 : 0;
                        worst = std::max(worst, Distance(out.p, in.p));
                    }
                }
            }
        }
    }
    std::printf("  swings: %d frames, %d held, weapon at most %.2f mm off the controller\n", frames,
                held, worst * 1000.0f);
    EXPECT_EQ(held, 0);
    EXPECT_LT(worst, 1e-5f);
}

namespace {

// What HandHoldNeverHoldsExtrapolatedSwings measures over its runs.
struct ExtrapolatedRuns {
    long frames{0};
    long held{0};
    float worst{0.0f};   // m: the shown hand against the runtime's sample
    float closest{0.0f}; // the largest share of the glitch tolerance a new sample used
};

// One run: the game at `fps` (5% missed vblanks; in stereo pairs a new sample every second frame)
// against the runtime at `xr_hz`, which locates each sample `horizon` before its target (display)
// time, or up to one of its frames earlier when `vary` (at most 60 ms), and predicts it with
// `predictor`.
void RunExtrapolated(const HandPath& path, Predictor predictor, double horizon, double fps,
                     double xr_hz, bool pairs, bool vary, std::mt19937& rng, ExtrapolatedRuns& r) {
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    const B::HeadPose head{true, {0.0f, 1.6f, 0.0f}, Quat{}};
    const double xr = 1.0 / xr_hz;
    B::HandHold hold{};
    B::TrackedHand in{}, prev{};
    double target = 0.0, prev_target = 0.0;
    long located = -1;
    double t = 0.1 + u01(rng) / fps;
    for (int f = 0; t < 0.75; ++f) {
        const auto j = static_cast<long>(std::floor(t / xr));
        if ((!pairs || f % 2 == 0) && j != located) {
            located = j;
            const double ahead = std::min(horizon + (vary ? u01(rng) * xr : 0.0), 0.060);
            prev = in;
            prev_target = target;
            target = static_cast<double>(j) * xr + horizon;
            in = PredictedSample(path, predictor, target - ahead, ahead);
            if (prev.velocity_valid) {
                const auto dt = static_cast<float>(target - prev_target);
                const Vec3 off = in.p - prev.p - (prev.v + in.v) * (0.5f * dt);
                r.closest =
                    std::max(r.closest, B::Length(off) / B::HandHold::Tolerance(dt, prev.v, in.v));
            }
        }
        B::TrackedHand out{};
        const B::HandUse use = hold.Update(in, Ns(t), 300'000'000, out, Ns(target), head);
        ++r.frames;
        r.held += use != B::HandUse::Tracked ? 1 : 0;
        r.worst = std::max(r.worst, Distance(out.p, in.p));
        t += (u01(rng) < 0.05 ? 2.0 : 1.0) / fps;
    }
}

} // namespace

TEST(BodyFrame, HandHoldNeverHoldsExtrapolatedSwings) {
    // The runtime locates the grips for the display time + pose_prediction_ms, so it extrapolates
    // them: at the end of a fast swing its prediction overshoots and pulls back, a jump its
    // reported velocities do not explain. Swings of 8-25 m/s (0.9 m straight minimum jerk, a
    // 0.6 m radius arc through 150 degrees, a stop within 10, 25 or 40 ms), waggles (8 Hz 0.25 m,
    // 12 Hz 0.15 m, a 10 Hz 0.1 m circle), each sample predicted 0-60 ms ahead (that horizon, or
    // up to one runtime frame more, varying per sample, at most 60 ms) exactly, at constant
    // velocity or at constant acceleration (the reported velocity is the prediction's own); the
    // game at 60, 45 and 30 fps with 5% missed vblanks, mono or in stereo pairs; the runtime at
    // 72, 90 and 120 Hz: never held, the weapon always on the controller.
    const Vec3 start{-0.2f, 1.6f, -0.35f};
    const Vec3 rest{0.25f, 1.1f, -0.35f};
    std::vector<HandPath> paths;
    for (const double speed : {8.0, 12.0, 16.0, 20.0, 25.0}) {
        paths.push_back(
            LinearSwing(start, B::Normalize(Vec3{1.0f, -1.0f, -0.3f}), 0.9, speed, 0.2));
        paths.push_back(ArcSwing({0.2f, 1.4f, -0.1f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, 0.6,
                                 2.6, speed, 0.2));
        for (const double stop_ms : {10.0, 25.0, 40.0}) {
            paths.push_back(StopSwing(start, B::Normalize(Vec3{1.0f, -0.6f, -0.5f}), speed,
                                      stop_ms * 1e-3, 0.2));
        }
    }
    paths.push_back(Waggle(rest, {1.0f, 0.0f, 0.0f}, {}, 0.25, 8.0, false, 0.2));
    paths.push_back(Waggle(rest, B::Normalize(Vec3{1.0f, 0.3f, 0.0f}), {}, 0.15, 12.0, false, 0.2));
    paths.push_back(Waggle(rest, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.1, 10.0, true, 0.2));
    std::mt19937 rng(41);
    ExtrapolatedRuns r{};
    for (const Predictor predictor :
         {Predictor::Exact, Predictor::ConstantVelocity, Predictor::ConstantAcceleration}) {
        for (const double horizon : {0.0, 0.020, 0.035, 0.050, 0.060}) {
            for (const double fps : {60.0, 45.0, 30.0}) {
                for (const double xr_hz : {72.0, 90.0, 120.0}) {
                    for (const HandPath& path : paths) {
                        for (const int variant : {0, 1, 2, 3}) { // pairs, varying horizon
                            RunExtrapolated(path, predictor, horizon, fps, xr_hz,
                                            (variant & 1) != 0, (variant & 2) != 0, rng, r);
                        }
                    }
                }
            }
        }
    }
    std::printf("  extrapolated swings: %ld frames, %ld held, weapon at most %.2f mm off the "
                "controller; the closest sample used %.0f%% of the tolerance\n",
                r.frames, r.held, r.worst * 1000.0f, r.closest * 100.0f);
    EXPECT_EQ(r.held, 0);
    EXPECT_LT(r.worst, 1e-5f);
}

TEST(BodyFrame, HandHoldHoldsSpikesButNotRecentres) {
    const B::HeadPose head{true, {0.0f, 1.6f, 0.0f}, Quat{}};
    const Vec3 rest{0.25f, 1.1f, -0.35f};
    const HandPath still{[rest](double) { return rest; }};
    const HandPath swing =
        LinearSwing({-0.2f, 1.6f, -0.35f}, B::Normalize(Vec3{1.0f, -1.0f, -0.3f}), 0.9, 20.0, 0.2);
    // One runtime sample jumps and the next one is back: held over (the last pose shown, the jump
    // never), the next sample tracked again. The hand still, the runtime's velocity saying so:
    // 1 m in a sample 11-17 ms after the last one (60 fps mono, the runtime at 120, 90 or 72 Hz),
    // 1.6 m 33 ms after it (stereo pairs). Mid-swing (18 m/s at 0.233 s): 2.5 m. A shorter jump
    // while the hand moves fast is shown: it cannot be told from an extrapolated swing.
    struct Spike {
        const HandPath* path;
        double xr_hz;
        bool pairs; // stereo pairs: a new sample every second frame
        int frame;  // the spike's game frame
        float size; // m
    };
    for (const Spike& spike :
         {Spike{&still, 120.0, false, 16, 1.0f}, Spike{&still, 90.0, false, 17, 1.0f},
          Spike{&still, 72.0, false, 16, 1.0f}, Spike{&still, 120.0, true, 16, 1.6f},
          Spike{&swing, 120.0, false, 14, 2.5f}}) {
        for (const Vec3 dir : {Vec3{0.0f, 1.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f},
                               B::Normalize(Vec3{1.0f, -1.0f, -0.3f})}) {
            B::HandHold hold{};
            B::TrackedHand out{};
            const int step = spike.pairs ? 2 : 1;
            Vec3 before{};
            double last_sample = 0.0, spike_dt = 0.0;
            bool spike_held = false, after_tracked = false;
            for (int f = 0; f <= spike.frame + 2 * step; f += step) {
                const double t = static_cast<double>(f) * GameFrame;
                const double sample_t = XrFrameAt(t, 1.0 / spike.xr_hz);
                B::TrackedHand in = SampleOf(*spike.path, sample_t);
                if (f == spike.frame) {
                    in.p = in.p + dir * spike.size;
                    spike_dt = sample_t - last_sample;
                }
                last_sample = sample_t;
                const B::HandUse use = hold.Update(in, Ns(t), 300'000'000, out, Ns(sample_t), head);
                if (f == spike.frame - step) {
                    before = out.p;
                }
                if (f == spike.frame) {
                    spike_held =
                        use == B::HandUse::Held && hold.rejected && Distance(out.p, before) < 1e-6f;
                }
                if (f == spike.frame + step) {
                    after_tracked = use == B::HandUse::Tracked;
                }
            }
            EXPECT_TRUE(spike_held)
                << spike.size << " m, " << spike.xr_hz << " Hz" << (spike.pairs ? " pairs" : "")
                << ", " << spike_dt * 1000.0 << " ms after the last sample";
            EXPECT_TRUE(after_tracked);
        }
    }
    // A runtime recentre: the head and both hands move into a new tracking space together (turned
    // 120 degrees and moved 0.7 m), the hand still or mid-swing: never held, the weapon on the
    // controller at once (no regain ease).
    for (const HandPath* path : {&still, &swing}) {
        for (const float yaw : {120.0f * kDeg, -60.0f * kDeg}) {
            const Quat r = B::AxisAngle({0.0f, 1.0f, 0.0f}, yaw);
            const Vec3 move{0.5f, 0.0f, -0.5f};
            B::HandHold hold{};
            B::TrackedHand out{};
            int held = 0;
            float worst = 0.0f;
            for (int f = 0; f < 30; ++f) {
                const double t = static_cast<double>(f) * GameFrame;
                const double sample_t = XrFrameAt(t);
                B::TrackedHand in = SampleOf(*path, sample_t);
                B::HeadPose h = head;
                if (f >= 16) {
                    in.p = B::Rotate(r, in.p) + move;
                    in.q = r;
                    in.v = B::Rotate(r, in.v);
                    h.p = B::Rotate(r, h.p) + move;
                    h.q = r;
                }
                held +=
                    hold.Update(in, Ns(t), 300'000'000, out, Ns(sample_t), h) != B::HandUse::Tracked
                        ? 1
                        : 0;
                worst = std::max(worst, Distance(out.p, in.p));
            }
            EXPECT_EQ(held, 0);
            EXPECT_LT(worst, 1e-5f);
        }
    }
}

TEST(BodyFrame, HandHoldRegainStepsOncePerPair) {
    // A hand regained in stereo pairs (pair sync: the second eye reuses its first eye's sample):
    // both eyes of a pair show the same pose, the ease steps once per pair (2/3, 1/3, 0).
    constexpr std::int64_t Frame = 16'666'667;
    B::HandHold hold{};
    B::TrackedHand out{};
    const Vec3 a{0.2f, 1.2f, -0.4f};
    const Vec3 b{0.2f, 1.2f, -0.6f};
    std::int64_t t = 1'000'000'000;
    EXPECT_EQ(hold.Update(HandAt(a, Quat{}), t, 300'000'000, out, t), B::HandUse::Tracked);
    B::TrackedHand lost = HandAt(b, Quat{});
    lost.position_tracked = false;
    for (int f = 0; f < 4; ++f) {
        t += Frame;
        EXPECT_EQ(hold.Update(lost, t, 300'000'000, out, t), B::HandUse::Held);
    }
    std::int64_t sample = 0;
    for (int f = 0; f < 8; ++f) {
        t += Frame;
        if (f % 2 == 0) {
            sample = t; // the pair's first eye takes a new sample, the second eye reuses it
        }
        EXPECT_EQ(hold.Update(HandAt(b, Quat{}), t, 300'000'000, out, sample), B::HandUse::Tracked);
        const float k = static_cast<float>(std::max(0, 2 - f / 2)) / 3.0f;
        EXPECT_LT(Distance(out.p, B::Lerp(b, a, k)), 1e-5f) << "frame " << f;
    }
}

TEST(BodyPresets, Scripted) {
    for (const P::Preset& preset : P::Presets) {
        for (const int side : {0, 1}) {
            for (const double t : {0.0, 0.5, 1.0, 1.1, 1.3, 1.5, 2.0, 2.99}) {
                const P::HandPose pose = P::Evaluate(preset, side, t);
                const Quat q = ToQuat(pose.q);
                EXPECT_TRUE(B::IsFinite(ToVec(pose.p)) && B::IsFinite(q));
                EXPECT_NEAR(B::Dot(q, q), 1.0f, 1e-5f);
            }
        }
    }
    const auto blade = [](std::string_view name, int side, double t) {
        const P::HandPose pose = P::Evaluate(*P::Find(name), side, t);
        return B::Rotate(ToQuat(pose.q), {0.0f, 0.0f, -1.0f});
    };
    EXPECT_GT(blade("guard", 1, 0.0).y, 0.6f);         // pitch +45: up
    EXPECT_LT(blade("rest", 1, 0.0).y, -0.3f);         // pitch -20: down
    EXPECT_LT(blade("cross", 1, 0.0).x, -0.6f);        // yaw +45: left
    EXPECT_GT(blade("over_shoulder", 1, 0.0).z, 0.7f); // pitch 150: back over the shoulder
    // The roll sweep rolls about the blade only.
    EXPECT_LT(Distance(blade("roll_sweep", 1, 0.2), blade("roll_sweep", 1, 2.8)), 1e-5f);
    // The swing: fast (min-jerk peak above the 2.5 m/s plan figure), from start to end.
    float peak = 0.0f;
    const P::Preset& swing = *P::Find("swing");
    for (double t = 0.9; t < 1.4; t += 0.001) {
        const Vec3 a = ToVec(P::Evaluate(swing, 1, t).p);
        const Vec3 b = ToVec(P::Evaluate(swing, 1, t + 0.001).p);
        peak = std::max(peak, Distance(a, b) / 0.001f);
    }
    EXPECT_GT(peak, 2.5f);
    EXPECT_LT(Distance(ToVec(P::Evaluate(swing, 1, 2.0).p),
                       {P::SwingEnd.x, P::SwingEnd.y, P::SwingEnd.z}),
              1e-6f);
    const P::Preset& lost = *P::Find("lost");
    EXPECT_TRUE(P::Evaluate(lost, 1, 1.0).tracked);
    EXPECT_FALSE(P::Evaluate(lost, 1, 1.5).tracked);
    EXPECT_TRUE(P::Evaluate(lost, 0, 1.5).tracked);
    EXPECT_TRUE(P::Evaluate(lost, 1, 2.0).tracked);
    // Timelines.
    EXPECT_EQ(P::ParseTimeline("1").size(), P::Presets.size());
    EXPECT_EQ(P::ParseTimeline("poses").size(), P::Presets.size());
    std::string unknown;
    const auto list = P::ParseTimeline("rest,bogus,cross", &unknown);
    ASSERT_EQ(list.size(), size_t{2});
    EXPECT_TRUE(list[1]->name == "cross");
    EXPECT_TRUE(unknown == "bogus");
    const P::TimelinePoint point = P::Locate(list, 7.5); // segment 2 = the first again, 1.5 s in
    EXPECT_TRUE(point.preset == list[0]);
    EXPECT_EQ(point.segment, std::uint64_t{2});
    EXPECT_NEAR(point.t, 1.5, 1e-9);
}

TEST(BodySolve, PresetsOnReferencePose) {
    // Every preset through the mapping onto the c0000 reference pose, as the dry run does it: the
    // character faces the tracking forward, the first-person eye 1.6 m up and 0.1 m forward.
    std::array<B::QsTransform, C::BoneCount> reference{};
    for (size_t i = 0; i < reference.size(); ++i) {
        const C::Transform& t = C::Test::TestSkeleton().local[i];
        reference[i] = {ToVec(t.t), ToQuat(t.q), ToVec(t.s)};
    }
    const float yaw = 0.6f;
    const B::ChrFrame frame = FrameFor(yaw, {12.0f, -3.0f, 40.0f});
    const B::Anchor anchor =
        AnchorFor(frame, {0.0f, P::HeadHeight, -0.1f}, {0.0f, P::HeadHeight, 0.0f}, yaw);
    const std::vector<std::string_view> reachable = {"rest",        "still",        "guard",
                                                     "blade_level", "wrist_inject", "lost"};
    for (const P::Preset& preset : P::Presets) {
        std::array<B::QsTransform, C::BoneCount> pose = reference;
        for (const int side : {C::Left, C::Right}) {
            const P::HandPose scripted = P::Evaluate(preset, side, 0.5);
            const B::TrackedHand hand = HandAt(ToVec(scripted.p), ToQuat(scripted.q));
            const B::ModelTarget target =
                B::HandToModel(anchor, frame, hand, B::DefaultGrip(side, 0.0f));
            EXPECT_LT(Distance(B::ModelToTracking(anchor, frame, target.p), hand.p), 1e-4f);
            const B::ArmRig rig = side == C::Right ? B::HunterRightArm() : B::HunterLeftArm();
            B::ArmParams params{};
            params.pole = B::BodyPole(B::BodyFrame{}, rig.side);
            B::ArmState state{};
            const B::ArmResult r = B::SolveArm(pose.data(), C::Parents.data(), C::BoneCount, rig,
                                               {target.p, target.q, 1.0f}, params, state, 0.0f);
            std::printf("  %-13s %s %-8s err %7.1f mm stretch %.3f flex %5.1f reached %d "
                        "detached %d\n",
                        std::string{preset.name}.c_str(), side == C::Right ? "R" : "L",
                        B::ArmStatusName(r.status), r.error * 1000.0f, r.stretch, r.flex / kDeg,
                        r.reached ? 1 : 0, B::Detached(r) ? 1 : 0);
            EXPECT_TRUE(r.status == B::ArmStatus::Solved);
            EXPECT_TRUE(std::isfinite(r.error));
            EXPECT_LE(r.stretch, 1.1001f);
            const bool expect_reached =
                std::find(reachable.begin(), reachable.end(), preset.name) != reachable.end() ||
                side == C::Left; // the left hand rests in every other preset
            if (expect_reached) {
                EXPECT_TRUE(r.reached);
                EXPECT_LT(r.error, 0.001f);
            }
            if (preset.name == "out_of_reach" && side == C::Right) {
                EXPECT_FALSE(r.reached);
                EXPECT_TRUE(B::Detached(r));
                EXPECT_NEAR(r.stretch, 1.1f, 1e-3f);
            }
        }
        for (const B::QsTransform& x : pose) {
            EXPECT_TRUE(B::IsFinite(x));
        }
    }
}

TEST(BodyChannel, SeqLockStress) {
    // A writer at 1 kHz (the XR frame rate is 72-120 Hz) and four readers spinning: every sample
    // returned must be one the writer stored (all fields agree), never a mix of two. 1 s by
    // default; BODY_SEQLOCK_SECONDS=10 for the plan's long run.
    const char* env = std::getenv("BODY_SEQLOCK_SECONDS");
    const double seconds = env != nullptr ? std::max(std::atof(env), 0.1) : 1.0;
    VR::SeqLock<B::HandsSample> lock;
    B::HandsSample probe{};
    EXPECT_FALSE(lock.Load(probe)); // nothing stored yet
    const auto make = [](std::uint64_t n) {
        B::HandsSample s{};
        const float f = static_cast<float>(n);
        s.seq = n;
        s.xr_time = static_cast<std::int64_t>(n) * 3;
        s.steady_ns = static_cast<std::int64_t>(n) * 5;
        s.head_valid = (n & 1) != 0;
        s.head_p = {f, f + 1.0f, f + 2.0f};
        s.head_q = {f, -f, f, -f};
        for (size_t i = 0; i < 2; ++i) {
            s.hand[i].p = {f + 10.0f, f + 11.0f, f + 12.0f};
            s.hand[i].q = {f, f, f, f};
            s.hand[i].v = {-f, -f, -f};
            s.hand[i].w = {f * 2.0f, f * 2.0f, f * 2.0f};
            s.hand[i].grip = f;
            s.hand[i].trigger = -f;
            s.hand[i].position_tracked = (n & 2) != 0;
        }
        return s;
    };
    const auto consistent = [&make](const B::HandsSample& s) {
        const B::HandsSample expected = make(s.seq);
        return std::memcmp(&s.hand, &expected.hand, sizeof(s.hand)) == 0 &&
               s.xr_time == expected.xr_time && s.steady_ns == expected.steady_ns &&
               s.head_valid == expected.head_valid &&
               std::memcmp(&s.head_p, &expected.head_p, sizeof(s.head_p)) == 0 &&
               std::memcmp(&s.head_q, &expected.head_q, sizeof(s.head_q)) == 0;
    };
    std::atomic_bool stop{false};
    std::atomic<std::uint64_t> torn{0}, reads{0}, misses{0}, backwards{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            std::uint64_t last = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                B::HandsSample s{};
                if (!lock.Load(s)) {
                    misses.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                reads.fetch_add(1, std::memory_order_relaxed);
                if (!consistent(s)) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                }
                if (s.seq < last) {
                    backwards.fetch_add(1, std::memory_order_relaxed);
                }
                last = s.seq;
            }
        });
    }
    // Paced by spinning: a 1 ms sleep can take a whole scheduler tick (15.6 ms on Windows).
    const auto start = std::chrono::steady_clock::now();
    const auto end = start + std::chrono::duration<double>(seconds);
    std::uint64_t n = 0;
    for (auto next = start; next < end; next += std::chrono::milliseconds(1)) {
        while (std::chrono::steady_clock::now() < next) {
            std::this_thread::yield();
        }
        lock.Store(make(++n));
    }
    stop.store(true);
    for (auto& t : readers) {
        t.join();
    }
    std::printf("  seqlock: %llu stores, %llu reads, %llu torn, %llu gave up, %llu backwards\n",
                static_cast<unsigned long long>(n), static_cast<unsigned long long>(reads.load()),
                static_cast<unsigned long long>(torn.load()),
                static_cast<unsigned long long>(misses.load()),
                static_cast<unsigned long long>(backwards.load()));
    EXPECT_EQ(torn.load(), std::uint64_t{0});
    EXPECT_EQ(backwards.load(), std::uint64_t{0});
    EXPECT_GT(reads.load(), std::uint64_t{1000});
    EXPECT_EQ(lock.Count(), n);
    ASSERT_TRUE(lock.Load(probe));
    EXPECT_EQ(probe.seq, n);
}

TEST(BodyChannel, ReplayRecord) {
    // VR_BODY_REPLAY=<file written with VR_BODY_RECORD>: every active frame through the mapping
    // and both arms, twice: finite and identical.
    const char* path = std::getenv("VR_BODY_REPLAY");
    if (path == nullptr) {
        return;
    }
    std::FILE* file = std::fopen(path, "rb");
    ASSERT_TRUE(file != nullptr);
    B::RecordHeader header{};
    ASSERT_EQ(std::fread(&header, sizeof(header), 1, file), size_t{1});
    EXPECT_TRUE(header.magic == B::RecordMagic);
    EXPECT_EQ(header.version, B::RecordVersion);
    EXPECT_EQ(header.bones, static_cast<std::uint32_t>(C::BoneCount));
    ASSERT_EQ(header.frame_size, static_cast<std::uint32_t>(sizeof(B::RecordFrame)));
    static B::RecordFrame frame{};
    std::uint64_t frames = 0, active = 0, failures = 0;
    while (std::fread(&frame, sizeof(frame), 1, file) == 1) {
        ++frames;
        B::ChrFrame chr{};
        if ((frame.flags & B::RecordAnchorActive) == 0 || !B::ChrFrameFromRows(frame.w_rows, chr)) {
            continue;
        }
        ++active;
        std::array<std::array<B::QsTransform, C::BoneCount>, 2> runs{};
        for (auto& run : runs) {
            for (size_t i = 0; i < run.size(); ++i) {
                run[i] = B::FromHavok(frame.locals[i]);
            }
            for (const int side : {C::Left, C::Right}) {
                const B::TrackedHand& hand = frame.hands.hand[static_cast<size_t>(side)];
                if (!B::IsTracked(hand)) {
                    continue;
                }
                const B::ModelTarget target =
                    B::HandToModel(frame.anchor, chr, hand, B::DefaultGrip(side, 0.0f));
                const B::ArmRig rig = side == C::Right ? B::HunterRightArm() : B::HunterLeftArm();
                B::ArmParams params{};
                params.pole = B::BodyPole(B::BodyFrame{}, rig.side);
                B::ArmState state{};
                const B::ArmResult r = B::SolveArm(run.data(), C::Parents.data(), C::BoneCount, rig,
                                                   {target.p, target.q, 1.0f}, params, state, 0.0f);
                failures += r.status == B::ArmStatus::Solved ? 0 : 1;
            }
        }
        for (size_t i = 0; i < runs[0].size(); ++i) {
            EXPECT_TRUE(B::IsFinite(runs[0][i]));
            EXPECT_TRUE(std::memcmp(&runs[0][i], &runs[1][i], sizeof(B::QsTransform)) == 0);
        }
    }
    std::fclose(file);
    std::printf("  replay %s: %llu frames, %llu active, %llu arm solves not Solved\n", path,
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(active),
                static_cast<unsigned long long>(failures));
    EXPECT_GT(frames, std::uint64_t{0});
}

TEST(PoseRefresh, HeldAnimationUsesFreshInputWithoutAccumulatingWrites) {
    std::array<float, 3> animation{1, 2, 3}, written{1, 5, 3}, local = written;
    std::bitset<3> edited;
    edited.set(1);
    for (int frame = 0; frame < 120; ++frame) {
        // Other unedited game bones keep updating even while this arm pose is held.
        local[0] = float(frame);
        ASSERT_TRUE(VR::Body::RestoreHeldLocals(local.data(), animation, written, edited));
        EXPECT_FLOAT_EQ(local[0], float(frame));
        EXPECT_FLOAT_EQ(local[1], 2);
        const float live_target = float(frame) * .01f;
        local[1] += live_target;
        written = local;
        EXPECT_FLOAT_EQ(local[1], 2 + live_target);
    }
}
TEST(PoseRefresh, GuestChangesAreNeverOverwrittenOrPartiallyRestored) {
    std::array<float, 3> animation{1, 2, 3}, written{4, 5, 6}, local{4, 9, 6};
    std::bitset<3> edited;
    edited.set();
    EXPECT_FALSE(VR::Body::RestoreHeldLocals(local.data(), animation, written, edited));
    EXPECT_FLOAT_EQ(local[0], 4);
    EXPECT_FLOAT_EQ(local[1], 9);
    EXPECT_FLOAT_EQ(local[2], 6);
    edited.reset();
    EXPECT_FALSE(VR::Body::RestoreHeldLocals(local.data(), animation, written, edited));
}


#include "core/vr/grab_camera.h"

TEST(GrabCamera, OnlyKnownEnemyVictimStates) {
    for (unsigned state = 0; state <= 9; ++state) {
        EXPECT_EQ(VR::GrabCamera::Victim(true, state), state == 2 || state == 4 || state == 6);
        EXPECT_FALSE(VR::GrabCamera::Victim(false, state));
    }
}

TEST(GrabCamera, AnimatedHeadFollowsLiftAndCharacterMotion) {
    VR::Body::ChrFrame frame{};
    frame.valid = true;
    frame.R = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    frame.q = {0, 0, 0, 1};
    frame.t = {10, 3, 20};
    VR::Body::QsTransform head{};
    head.t = {0, 2, 0};
    const auto eye = VR::GrabCamera::Eye(frame, head, {0, 0.1f, -0.1f});
    EXPECT_NEAR(eye.x, 10, 1e-5f);
    EXPECT_NEAR(eye.y, 5.1f, 1e-5f);
    EXPECT_NEAR(eye.z, 19.9f, 1e-5f);
    head.t = {1, 4, -2}; // lifted and carried without moving the physics root
    const auto moved = VR::GrabCamera::Eye(frame, head, {0, 0.1f, -0.1f});
    EXPECT_NEAR(moved.x - eye.x, 1, 1e-5f);
    EXPECT_NEAR(moved.y - eye.y, 2, 1e-5f);
    EXPECT_NEAR(moved.z - eye.z, -2, 1e-5f);
}

TEST(GrabCamera, StaleAndInvalidPosesNeverDriveView) {
    VR::GrabCamera::Sample sample{{1, 2, 3}, 1'000'000'000, true};
    EXPECT_TRUE(VR::GrabCamera::Fresh(sample, sample.ns));
    EXPECT_FALSE(VR::GrabCamera::Fresh(sample, sample.ns - 1));
    EXPECT_FALSE(VR::GrabCamera::Fresh(sample, sample.ns + 151'000'000));
    sample.eye.y = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(VR::GrabCamera::Fresh(sample, sample.ns));
    sample = {};
    EXPECT_FALSE(VR::GrabCamera::Fresh(sample, 0));
}

TEST(GrabCamera, StereoAnchorLocksAcrossGrabEntryAndExit) {
    VR::GrabCamera::PairPosition pair;
    constexpr auto ms = 1'000'000LL;
    EXPECT_FALSE(pair.Step({1, 2, 3}, false, true, 0, true, 25*ms));
    // A grab starts between eyes: the second eye still sees the first eye's base.
    auto p = pair.Step({8, 9, 10}, true, false, 16*ms, true, 25*ms);
    ASSERT_TRUE(p);
    EXPECT_FLOAT_EQ(p->x, 1);
    p = pair.Step({8, 9, 10}, true, true, 32*ms, true, 25*ms);
    ASSERT_TRUE(p);
    EXPECT_FLOAT_EQ(p->x, 8);
    // Control returns between eyes: finish the grab pair before using the normal base.
    p = pair.Step({1, 2, 3}, false, false, 48*ms, true, 25*ms);
    ASSERT_TRUE(p);
    EXPECT_FLOAT_EQ(p->x, 8);
    EXPECT_FALSE(pair.Step({1, 2, 3}, false, true, 64*ms, true, 25*ms));
    pair.Reset();
    p = pair.Step({20, 30, 40}, true, false, 80*ms, true, 25*ms);
    ASSERT_TRUE(p);
    EXPECT_FLOAT_EQ(p->x, 20);
}

TEST(GrabCamera, LongGapAndBodyUnlockDoNotReuseOldStereoAnchor) {
    VR::GrabCamera::PairPosition pair;
    constexpr auto ms = 1'000'000LL;
    pair.Step({1, 2, 3}, true, true, 0, true, 25*ms);
    auto p = pair.Step({8, 9, 10}, true, false, 200*ms, true, 25*ms);
    ASSERT_TRUE(p);
    EXPECT_FLOAT_EQ(p->x, 8);
    VR::Body::PairLock decision{200*ms, 210*ms, false};
    p = pair.Step({10, 11, 12}, true, false, 216*ms, true, 25*ms, &decision);
    ASSERT_TRUE(p);
    EXPECT_FLOAT_EQ(p->x, 10);
    p = pair.Step({12, 13, 14}, true, false, 220*ms, false, 25*ms);
    ASSERT_TRUE(p);
    EXPECT_FLOAT_EQ(p->x, 12);
}

TEST(GrabCamera, ThrowUsesTheViewBeforeTheEntrySnap) {
    using namespace VR;
    GrabCamera::ThrowCamera camera;
    constexpr auto ms = 1'000'000LL;
    const Body::Vec3 before{2, 1.6f, 3};
    camera.Step(false, before, {}, 1, true, 1000 * ms);
    const Body::Vec3 staged = before + Body::Vec3{.6f, 0, 0};
    const auto hold = camera.Step(true, staged, {1, 0, 0}, 1, true, 1016 * ms);
    EXPECT_NEAR(Body::Length(staged + hold - before), 0, 1e-5f);
    // The second eye must reuse that pre-snap anchor.
    EXPECT_FLOAT_EQ(camera.Step(true, {9, 9, 9}, {}, 1, false, 1024 * ms).x, hold.x);
    // Physical head movement remains outside the anchored game-camera base.
    const Body::Vec3 head{.12f, -.04f, .03f};
    EXPECT_NEAR(Body::Length(staged + hold + head - (before + head)), 0, 1e-5f);
}

TEST(GrabCamera, ThrowDoesNotReuseAStaleOrWarpedView) {
    using namespace VR;
    constexpr auto ms = 1'000'000LL;
    for (auto gap : {16 * ms, 500 * ms}) {
        GrabCamera::ThrowCamera camera;
        camera.Step(false, {}, {}, 1, true, 1000 * ms);
        const Body::Vec3 base{gap == 16 * ms ? 4.f : .6f, 0, 0};
        EXPECT_FLOAT_EQ(Body::Length(camera.Step(true, base, {1, 0, 0}, 1, true,
                                                 1000 * ms + gap)), 0);
    }
}

TEST(GrabCamera, LiveVisceralKeepsTheViewThroughTwentySecondsWithoutNewPoses) {
    using namespace VR;
    constexpr auto ms = 1'000'000LL;
    // Both no pose at entry and a pose that expires while held must preserve the lock.
    for (bool have_pose : {false, true}) {
        GrabCamera::ThrowCamera camera;
        camera.Step(false, {}, {}, 1, true, 1000 * ms);
        GrabCamera::Sample pose{};
        if (have_pose)
            pose = {{}, 1010 * ms, true, true};
        Combat::VisceralHold live{};
        live.active = true;
        for (int i = 1; i <= 2000; ++i) {
            const auto now = (1000 + i * 10) * ms;
            live.ns = now;
            live.held = i >= 10;
            const auto sample = GrabCamera::SelectSample(pose, live, true, now);
            ASSERT_TRUE(sample.valid && sample.own_throw);
            const Body::Vec3 base{.6f + .1f * i / 2000.f, 0, 0};
            const auto offset = camera.Step(sample.own_throw, base, {1, 0, 0}, 1, true, now);
            ASSERT_NEAR((base + offset).x, 0, 1e-5f);
        }
        live.released = true;
        live.active = false; // only the end of the native throw starts the glide
        live.ns += 10 * ms;
        EXPECT_FALSE(GrabCamera::SelectSample(pose, live, true, live.ns).valid);
        const auto offset = camera.Step(false, {.7f, 0, 0}, {}, 1, true, live.ns);
        EXPECT_GT(.7f + offset.x, 0);
        EXPECT_LT(.7f + offset.x, .02f); // release still glides instead of snapping
    }
}

TEST(GrabCamera, LiveThrowCannotReviveStaleCancelledOrDisabledState) {
    using namespace VR;
    constexpr auto ms = 1'000'000LL;
    Combat::VisceralHold live{};
    live.ns = 1000 * ms;
    live.active = true;
    EXPECT_TRUE(GrabCamera::SelectSample({}, live, true, 1250 * ms).own_throw);
    EXPECT_FALSE(GrabCamera::SelectSample({}, live, true, 1251 * ms).valid);
    EXPECT_FALSE(GrabCamera::SelectSample({}, live, true, 999 * ms).valid);
    EXPECT_FALSE(GrabCamera::SelectSample({}, live, false, 1010 * ms).valid);
    live.released = true;
    EXPECT_TRUE(GrabCamera::SelectSample({}, live, true, 1010 * ms).own_throw);
    live.camera_cancelled = true;
    EXPECT_FALSE(GrabCamera::SelectSample({}, live, true, 1010 * ms).valid);
    live.camera_cancelled = false;
    live.active = false;
    EXPECT_FALSE(GrabCamera::SelectSample({}, live, true, 1010 * ms).valid);
    // A real incoming grab keeps its animated eye, even with a prior live throw receipt.
    live.active = true;
    const GrabCamera::Sample grabbed{{1, 2, 3}, 1000 * ms, true};
    const auto sample = GrabCamera::SelectSample(grabbed, live, true, 1010 * ms);
    EXPECT_TRUE(sample.valid);
    EXPECT_FALSE(sample.own_throw);
    EXPECT_FLOAT_EQ(sample.eye.y, 2);
}

TEST(GrabCamera, PullReleaseKeepsTheCameraThroughStaleAndFreshExtractionPoses) {
    using namespace VR;
    constexpr auto ms = 1'000'000LL;
    for (auto gap : {16 * ms, 100 * ms}) {
        GrabCamera::ThrowCamera camera;
        camera.Step(false, {}, {}, 1, true, 990 * ms);
        GrabCamera::Sample pose{{}, 1000 * ms, true, true};
        Combat::VisceralHold live{};
        live.active = live.held = true;
        live.ns = 1000 * ms;
        auto selected = GrabCamera::SelectSample(pose, live, true, live.ns);
        camera.Step(selected.own_throw, {.6f, 0, 0}, {1, 0, 0}, 1, true, live.ns);
        // The pose expires while the stab is held.
        live.ns = 2000 * ms;
        selected = GrabCamera::SelectSample(pose, live, true, live.ns);
        camera.Step(selected.own_throw, {.6f, 0, 0}, {1, 0, 0}, 1, true, live.ns);
        live.held = false;
        live.released = true;
        for (int i = 0; i < 12; ++i) {
            live.ns += gap;
            if (i >= 3)
                pose.ns = live.ns; // extraction starts publishing poses again
            selected = GrabCamera::SelectSample(pose, live, true, live.ns);
            ASSERT_TRUE(selected.own_throw);
            const Body::Vec3 base{.6f + .01f * i, 0, 0};
            const auto hold = camera.Step(selected.own_throw, base, {1, 0, 0}, 1, true, live.ns);
            EXPECT_NEAR((base + hold).x, 0, 1e-5f);
        }
        // The game ends the throw while the last outgoing pose is still fresh.
        live.active = false;
        live.ns += 16 * ms;
        selected = GrabCamera::SelectSample(pose, live, true, live.ns);
        ASSERT_FALSE(selected.own_throw);
        const auto offset = camera.Step(false, {.71f, 0, 0}, {}, 1, true, live.ns);
        EXPECT_GT(.71f + offset.x, 0);
        EXPECT_LT(.71f + offset.x, .02f);
    }
}

TEST(GrabCamera, CancelledThrowCannotRelockFromAnOutgoingPose) {
    using namespace VR;
    constexpr auto ms = 1'000'000LL;
    Combat::VisceralHold live{};
    live.active = live.released = live.camera_cancelled = true;
    for (int i = 0; i < 20; ++i) {
        live.ns = (1000 + 16 * i) * ms;
        // Even a slightly newer animation pose cannot undo a game-side interruption.
        GrabCamera::Sample pose{{}, live.ns + ms, true, true};
        EXPECT_FALSE(GrabCamera::SelectSample(pose, live, true, pose.ns).valid);
        // Actual incoming grabs continue to use their animated eye.
        pose.own_throw = false;
        pose.eye = {1, 2, 3};
        const auto grabbed = GrabCamera::SelectSample(pose, live, true, pose.ns);
        EXPECT_TRUE(grabbed.valid);
        EXPECT_FALSE(grabbed.own_throw);
        EXPECT_FLOAT_EQ(grabbed.eye.y, 2);
    }
    live.camera_cancelled = live.active = false;
    live.ns += 16 * ms;
    GrabCamera::Sample ended{{}, live.ns - ms, true, true};
    EXPECT_FALSE(GrabCamera::SelectSample(ended, live, true, live.ns).valid);
    // A genuinely newer pose may announce a new throw before its game receipt.
    ended.ns = live.ns + ms;
    EXPECT_TRUE(GrabCamera::SelectSample(ended, live, true, ended.ns).own_throw);
    live.active = true;
    live.released = false;
    live.ns += 2 * ms;
    EXPECT_TRUE(GrabCamera::SelectSample({}, live, true, live.ns).own_throw);
}

TEST(GrabCamera, ThrowHoldsYourPlaceThenGlidesBack) {
    // The game moves the hunter 50 cm toward the victim during the throw: the view stays put,
    // then glides onto him over GlideS when the throw ends.
    using F = VR::GrabCamera::ThrowCamera;
    F f;
    constexpr auto ms = 1'000'000LL;
    constexpr auto frame = 8 * ms + ms / 3;
    std::int64_t t = 1'000 * ms;
    const VR::Body::Vec3 start{2, 1.6f, 3};
    VR::Body::Vec3 base = start;
    for (int i = 0; i <= 40; ++i) {
        base = start + VR::Body::Vec3{0.5f * std::min(1.0f, i / 20.0f), 0, 0};
        const auto view = base + f.Step(true, base, {1, 0, 0}, 1.0f, true, t);
        EXPECT_NEAR(VR::Body::Length(view - start), 0.0f, 1e-5f);
        // The second eye of a pair: the first eye's offset, whatever it is given.
        EXPECT_FLOAT_EQ(f.Step(true, {9, 9, 9}, {}, 1.0f, false, t + 4 * ms).x, f.Held().x);
        t += frame;
    }
    EXPECT_TRUE(f.Running());
    // The throw ends: no snap, never back, onto him over GlideS.
    float last = 0.0f;
    int frames = 0;
    for (; f.Running() && frames < 100; ++frames) {
        const float x = (base + f.Step(false, base, {}, 1.0f, true, t)).x - start.x;
        EXPECT_GE(x, last - 1e-6f);
        EXPECT_LT(x - last, 0.05f); // under 5 cm a frame
        last = x;
        t += frame;
    }
    EXPECT_NEAR(last, 0.5f, 1e-5f);
    EXPECT_NEAR(frames * 8.333f, F::GlideS * 1000.0f, 20.0f);
    EXPECT_FALSE(f.TakeCut());
}

TEST(GrabCamera, ALongWayBackGlidesLonger) {
    // 1.5 m under the held view: the glide takes 0.75 s (2 m/s on average), not 0.3.
    using F = VR::GrabCamera::ThrowCamera;
    F f;
    constexpr auto ms = 1'000'000LL;
    std::int64_t t = 1'000 * ms;
    f.Step(true, {0, 0, 0}, {1, 0, 0}, 1.0f, true, t);
    const VR::Body::Vec3 base{1.5f, 0, 0};
    f.Step(true, base, {1, 0, 0}, 1.0f, true, t += 10 * ms);
    int frames = 0;
    float last = 0.0f;
    for (; f.Running() && frames < 200; ++frames) {
        const float x = (base + f.Step(false, base, {}, 1.0f, true, t += 10 * ms)).x;
        EXPECT_LT(x - last, 0.04f); // under 4 m/s at the fastest
        last = x;
    }
    EXPECT_NEAR(f.GlideTime(), 0.75f, 1e-5f);
    EXPECT_NEAR(frames * 10.0f, 750.0f, 15.0f);
}

TEST(GrabCamera, ThrowDuringTheGlideStartsFromTheView) {
    using F = VR::GrabCamera::ThrowCamera;
    F f;
    constexpr auto ms = 1'000'000LL;
    std::int64_t t = 1'000 * ms;
    f.Step(true, {0, 0, 0}, {1, 0, 0}, 1.0f, true, t);
    f.Step(true, {0.4f, 0, 0}, {1, 0, 0}, 1.0f, true, t += 10 * ms);
    const VR::Body::Vec3 base{0.4f, 0, 0};
    const float before = (base + f.Step(false, base, {}, 1.0f, true, t += 50 * ms)).x;
    EXPECT_GT(before, 0.0f); // gliding
    EXPECT_LT(before, 0.4f);
    const float after = (base + f.Step(true, base, {1, 0, 0}, 1.0f, true, t += 10 * ms)).x;
    EXPECT_NEAR(after, before, 1e-5f); // held where the view was
}

TEST(GrabCamera, ThrowLetsGoOfAWarpAndBadBases) {
    using F = VR::GrabCamera::ThrowCamera;
    F f;
    constexpr auto ms = 1'000'000LL;
    f.Step(true, {0, 0, 0}, {1, 0, 0}, 1.0f, true, 1000 * ms);
    // A load or a warp (past MaxM): the view cuts to him, once, and holds again from there.
    EXPECT_FLOAT_EQ(VR::Body::Length(f.Step(true, {4, 0, 0}, {1, 0, 0}, 1.0f, true, 1010 * ms)),
                    0.0f);
    EXPECT_TRUE(f.TakeCut());
    EXPECT_FALSE(f.TakeCut());
    EXPECT_NEAR(f.Step(true, {4.2f, 0, 0}, {1, 0, 0}, 1.0f, true, 1020 * ms).x, -0.2f, 1e-5f);
    // Twice the world scale: 4 game units are 2 m.
    F g;
    g.Step(true, {0, 0, 0}, {1, 0, 0}, 2.0f, true, 1000 * ms);
    EXPECT_FLOAT_EQ(g.Step(true, {4, 0, 0}, {1, 0, 0}, 2.0f, true, 1010 * ms).x, -4.0f);
    // No finite base: nothing held.
    F h;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(h.Step(true, {nan, 0, 0}, {1, 0, 0}, 1.0f, true, 1000 * ms).x, 0.0f);
    EXPECT_FALSE(h.Running());
    h.Reset();
    EXPECT_FALSE(h.Running());
}

TEST(GrabCamera, ThrowKeepsTheCameraOutOfTheVictim) {
    // The victim's root 1 m ahead (+X).
    using F = VR::GrabCamera::ThrowCamera;
    F f;
    constexpr auto ms = 1'000'000LL;
    constexpr auto frame = 8 * ms + ms / 3;
    std::int64_t t = 1'000 * ms;
    const VR::Body::Vec3 victim{1, 0, 0};
    const VR::Body::Vec3 staged{0.4f, 1.6f, 0};
    VR::Body::Vec3 push{};
    auto settle = [&](VR::Body::Vec3 camera) {
        for (int i = 0; i < 60; ++i) {
            t += frame;
            f.Step(true, {}, {1, 0, 0}, 1.0f, true, t);
            const auto next = f.KeepOut(camera, staged, true, victim, 1.0f, true);
            // Never a step: at most PushMps.
            EXPECT_LE(VR::Body::Length(next - push), F::PushMps * frame * 1e-9f + 1e-5f);
            push = next;
        }
        return push;
    };
    f.Step(true, {}, {1, 0, 0}, 1.0f, true, t);
    // Your head leaned to 10 cm from its root: pushed back out to KeepOutM (horizontal only).
    auto p = settle({0.9f, 1.6f, 0});
    EXPECT_NEAR(p.x, -(F::KeepOutM - 0.1f), 1e-4f);
    EXPECT_FLOAT_EQ(p.y, 0.0f);
    // The second eye of a pair: the first eye's push.
    EXPECT_FLOAT_EQ(f.KeepOut({}, {}, false, {}, 1.0f, false).x, p.x);
    // Off to the side: pushed out sideways.
    p = settle({1.0f, 1.6f, 0.2f});
    EXPECT_NEAR(p.z, F::KeepOutM - 0.2f, 1e-4f);
    EXPECT_NEAR(p.x, 0.0f, 1e-4f);
    // Further out than KeepOutM: nothing.
    EXPECT_NEAR(VR::Body::Length(settle({0.4f, 1.6f, 0})), 0.0f, 1e-6f);
    // Right over its root: pushed back against the hunter's facing.
    p = settle({1.0f, 1.6f, 0});
    EXPECT_NEAR(p.x, -F::KeepOutM, 1e-4f);
    // The throw ends: the push eases out with the glide, then the victim is forgotten.
    float last = p.x;
    for (int i = 0; i < 60; ++i) {
        t += frame;
        f.Step(false, {}, {}, 1.0f, true, t);
        const float x = f.KeepOut({1.0f, 0, 0}, {}, false, {}, 1.0f, true).x;
        EXPECT_GE(x, last - 1e-6f);
        last = x;
    }
    EXPECT_FLOAT_EQ(last, 0.0f);
    f.Step(true, {}, {1, 0, 0}, 1.0f, true, t + 10 * ms);
    EXPECT_FLOAT_EQ(f.KeepOut({0.9f, 0, 0}, {}, false, {}, 1.0f, true).x, 0.0f);
}

TEST(GrabCamera, ThrowIgnoresAFarVictim) {
    VR::GrabCamera::ThrowCamera f;
    constexpr auto ms = 1'000'000LL;
    f.Step(true, {}, {1, 0, 0}, 1.0f, true, 1000 * ms);
    f.Step(true, {}, {1, 0, 0}, 1.0f, true, 1100 * ms);
    // Not the victim (3 m past his eye) or not finite: no push.
    EXPECT_FLOAT_EQ(f.KeepOut({3.9f, 0, 0}, {0, 0, 0}, true, {4, 0, 0}, 1.0f, true).x, 0.0f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(f.KeepOut({0.1f, 0, 0}, {0, 0, 0}, true, {nan, 0, 0}, 1.0f, true).x, 0.0f);
    // No throw running: no push either.
    VR::GrabCamera::ThrowCamera idle;
    EXPECT_FLOAT_EQ(idle.KeepOut({0.9f, 0, 0}, {0.6f, 0, 0}, true, {1, 0, 0}, 1.0f, true).x, 0.0f);
}

TEST(GrabCamera, HeldPoseRestoresAnimatedEyeBeforeSampling) {
    using T = VR::Body::QsTransform;
    std::array<T, 1> animated{};
    animated[0].t = {0, 3, 0};
    auto written = animated;
    written[0].t = {0, 1, 0}; // previous VR body alignment, not the animation
    auto locals = written;
    std::bitset<1> edited; edited.set();
    ASSERT_TRUE(VR::Body::RestoreHeldLocals(locals.data(), animated, written, edited));
    VR::Body::ChrFrame frame{};
    frame.R = {1,0,0,0,1,0,0,0,1};
    frame.t = {4,5,6};
    EXPECT_FLOAT_EQ(VR::GrabCamera::Eye(frame, locals[0], {}).y, 8);
}
