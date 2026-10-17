// SPDX-License-Identifier: GPL-2.0-or-later

// The physical whip (core/vr/whip_chain.h): the chain holds the game's idle line at rest, every link
// keeps its (stretched) length, the payout reaches the game's own reach in its time and reels back,
// a scripted forehand (the mock's rswing_fore) lashes out to the light row's reach with no fold, a
// teleporting hand never breaks it, and the bone rotations follow the links without twist flips.
// The swing (core/vr/whip_spin.h): the payout follows the swing's speed, holds, reels in; each
// revolution of a spin is a window paid from the combo budget; an overhead spin keeps the chain
// out and circling; the cane's backward idle line is turned to the blade side. The idle lines are
// synthetic (our own straight lines of the whips' bone counts and link lengths); the reader of the
// game's own idle clips (core/vr/whip_idle.h) is tested on a packfile written here and, in the
// game-data tier, on the user's dump.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#ifdef BBVR_GAME_DATA_TESTS
#include <fstream>
#include <iterator>
#include <miniz.h>
#include "core/vr/tpf_patch.h"
#endif

#include "core/vr/melee_window.h"
#include "core/vr/whip_chain.h"
#include "core/vr/whip_idle.h"
#include "core/vr/whip_input.h"
#include "core/vr/whip_moves.h"
#include "core/vr/whip_spin.h"
#include "core/vr/whip_settings.h"
#include "core/vr/seqlock.h"
#include "tests/whip_motion_fixture.h"

namespace {

namespace W = VR::Whip;
namespace B = VR::Body;

constexpr int N = 26;

// The Beast Cutter's idle chain (a999, part frame, m): Bone_Chain_1 at (0.031, -0.214, 0), 25
// links of 3.36 cm along a line 20 degrees off the handle (scratchpad/whip/whipanim.py).
std::array<B::Vec3, N> IdleChain() {
    std::array<B::Vec3, N> p{};
    const B::Vec3 root{0.031f, -0.214f, 0.0f};
    const B::Vec3 dir = B::Normalize(B::Vec3{0.381f, -0.883f, 0.0f});
    for (int i = 0; i < N; ++i) {
        p[i] = root + dir * (0.0336f * static_cast<float>(i));
    }
    return p;
}

float LinkError(const W::Chain& c, float s, const std::array<B::Vec3, N>& rest) {
    float worst = 0.0f;
    for (int i = 1; i < c.Count(); ++i) {
        const float want = B::Length(rest[i] - rest[i - 1]) * s;
        worst = std::max(worst, std::abs(B::Length(c.Point(i) - c.Point(i - 1)) - want));
    }
    return worst;
}

// The mock's rswing_fore: the hand on a 0.55 m sphere round the right shoulder with the blade (the
// part's -Y) out along the radius, from 75 deg right to 45 deg left, minimum jerk to `speed` m/s.
struct Stroke {
    float speed{6.0f};
    float t0{0.3f};
    float T{0.0f};
    B::Quat q0{}, q1{};
    Stroke(float yaw0, float pitch0, float yaw1, float pitch1, float v) : speed(v) {
        q0 = Grip(yaw0, pitch0);
        q1 = Grip(yaw1, pitch1);
        const float path = 2.0f * std::acos(std::min(std::abs(B::Dot(q0, q1)), 1.0f)) * 0.55f;
        T = 1.875f * path / speed;
    }
    static B::Quat Grip(float yaw, float pitch) {
        return B::AxisAngle({0.0f, 1.0f, 0.0f}, yaw * B::Deg) *
               B::AxisAngle({1.0f, 0.0f, 0.0f}, pitch * B::Deg);
    }
    W::Frame At(float t) const {
        const float u = std::clamp((t - t0) / T, 0.0f, 1.0f);
        const float k = u * u * u * (10.0f - 15.0f * u + 6.0f * u * u);
        const B::Quat g = B::Slerp(q0, q1, k);
        const B::Vec3 blade = B::Rotate(g, {0.0f, 0.0f, -1.0f});
        // The part frame: X = the grip's X, Y = the grip's +Z (the blade is -Y), Z = -grip Y.
        const B::Quat part = g * B::AxisAngle({1.0f, 0.0f, 0.0f}, 90.0f * B::Deg);
        return {B::Vec3{0.18f, 1.38f, 0.05f} + blade * 0.55f, part};
    }
};

TEST(WhipChain, PartFrameOfTheStrokeHasTheBladeAlongMinusY) {
    const Stroke s(-75.0f, 5.0f, 45.0f, -10.0f, 6.0f);
    const W::Frame f = s.At(0.5f);
    const B::Vec3 blade_part = B::Rotate(f.q, {0.0f, -1.0f, 0.0f});
    const B::Quat g = B::Slerp(s.q0, s.q1, 0.0f);
    (void)g;
    const B::Vec3 out = B::Normalize(f.t - B::Vec3{0.18f, 1.38f, 0.05f});
    EXPECT_GT(B::Dot(blade_part, out), 0.999f);
}

TEST(WhipChain, HoldsTheIdleLineAtRest) {
    const auto rest = IdleChain();
    W::Chain c;
    ASSERT_TRUE(c.Init(rest.data(), N));
    const W::Frame f{{0.5f, 1.2f, -0.3f}, B::AxisAngle({0.0f, 1.0f, 0.0f}, 0.4f)};
    c.Reset(f);
    const W::ChainConfig cfg{};
    float drift = 0.0f; // the tip's motion per frame over the last second
    for (int i = 0; i < 240; ++i) { // 2 s at 120 Hz
        const B::Vec3 before = c.Point(N - 1);
        c.Step(f, f, 1.0f / 120.0f, 1.0f, cfg);
        if (i >= 120) {
            drift = std::max(drift, B::Length(c.Point(N - 1) - before));
        }
    }
    float worst = 0.0f;
    for (int i = 0; i < N; ++i) {
        worst = std::max(worst, B::Length(c.Point(i) - W::Apply(f, rest[i])));
    }
    EXPECT_LT(worst, 0.06f) << "the tip sags under gravity at most a few cm (no flop)";
    EXPECT_LT(LinkError(c, 1.0f, rest), 1e-4f);
    EXPECT_LT(drift, 0.0005f) << "still at rest (no jitter you could see)";
}

TEST(WhipChain, PaysOutToTheReachAndReelsBack) {
    const auto rest = IdleChain();
    W::Chain c;
    ASSERT_TRUE(c.Init(rest.data(), N));
    const W::Frame f{};
    c.Reset(f);
    W::ChainConfig cfg{};
    const float s_max = W::ExtensionForReach(3.70f, 1.17f, 0.84f);
    EXPECT_NEAR(s_max, 4.01f, 0.02f) << "the light R1's 3.7 m from the clip";
    float t = 0.0f;
    while (t < cfg.t_out - 1e-4f) {
        c.Step(f, f, 1.0f / 120.0f, s_max, cfg);
        t += 1.0f / 120.0f;
    }
    EXPECT_NEAR(c.Extension(), s_max, 0.05f) << "out within t_out";
    EXPECT_NEAR(c.Length(), 0.84f * s_max, 0.03f);
    EXPECT_LT(LinkError(c, c.Extension(), rest), 1e-3f);
    for (int i = 0; i < 24; ++i) {
        c.Step(f, f, 1.0f / 120.0f, 1.0f, cfg);
    }
    EXPECT_NEAR(c.Extension(), 1.0f, 1e-3f) << "back within t_in";
    EXPECT_NEAR(c.Length(), 0.84f, 0.01f);
    EXPECT_FALSE(c.Out() > 0.0f);
}

TEST(WhipChain, AForehandLashesOutWithoutFolding) {
    const auto rest = IdleChain();
    W::Chain c;
    ASSERT_TRUE(c.Init(rest.data(), N));
    const Stroke s(-75.0f, 5.0f, 45.0f, -10.0f, 6.0f);
    c.Reset(s.At(0.0f));
    const W::ChainConfig cfg{};
    const float s_max = W::ExtensionForReach(3.70f, 1.17f, 0.84f);
    const float dt = 1.0f / 30.0f; // the game's frames (the chain substeps)
    float tip_peak = 0.0f;
    float reach = 0.0f;
    float bend = 0.0f;
    float front_reach = 0.0f; // how far in front of the player the tip got (-Z)
    bool open = false;
    float closed_at = -1.0f;
    W::Frame prev = s.At(0.0f);
    B::Vec3 hand_prev = prev.t;
    for (float t = dt; t < 1.6f; t += dt) {
        const W::Frame f = s.At(t);
        const float hand_v = B::Length(f.t - hand_prev) / dt;
        hand_prev = f.t;
        // The presenter's window: open from 2 m/s, closed under 1.4 m/s; the lash holds 0.12 s.
        if (!open && closed_at < 0.0f && hand_v >= 2.0f) {
            open = true;
        } else if (open && hand_v < 1.4f) {
            open = false;
            closed_at = t;
        }
        const bool lash = open || (closed_at >= 0.0f && t - closed_at < 0.12f);
        ASSERT_GT(c.Step(prev, f, dt, lash ? s_max : 1.0f, cfg), 0);
        prev = f;
        tip_peak = std::max(tip_peak, c.TipSpeed());
        reach = std::max(reach, B::Length(c.Point(N - 1) - f.t));
        bend = std::max(bend, c.MaxBend());
        front_reach = std::max(front_reach, -c.Point(N - 1).z);
        ASSERT_LT(LinkError(c, c.Extension(), rest), 2e-3f) << "t=" << t;
    }
    EXPECT_GT(tip_peak, 25.0f) << "the tip lashes (the game's clip: 30-44 m/s)";
    EXPECT_GT(reach, 3.2f) << "out to the light row's reach";
    EXPECT_LT(reach, 3.8f);
    EXPECT_GT(front_reach, 3.0f) << "the lash sweeps across the front";
    EXPECT_LE(bend, cfg.max_bend + 0.01f) << "no fold at the game's frame rate";
    EXPECT_NEAR(c.Extension(), 1.0f, 1e-3f) << "reeled back in after the stroke";
}

TEST(WhipChain, ATeleportingHandNeverBreaksIt) {
    const auto rest = IdleChain();
    W::Chain c;
    ASSERT_TRUE(c.Init(rest.data(), N));
    W::Frame a{};
    c.Reset(a);
    const W::ChainConfig cfg{};
    unsigned seed = 12345u;
    const auto rnd = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f - 0.5f;
    };
    for (int i = 0; i < 400; ++i) {
        const W::Frame b{{rnd() * 2.0f, 1.0f + rnd(), rnd() * 2.0f},
                         B::Normalize(B::Quat{rnd(), rnd(), rnd(), rnd() + 0.6f})};
        c.Step(a, b, 1.0f / 30.0f, i % 50 < 20 ? 6.8f : 1.0f, cfg);
        a = b;
        for (int k = 0; k < N; ++k) {
            ASSERT_TRUE(B::IsFinite(c.Point(k)));
        }
        ASSERT_LT(LinkError(c, c.Extension(), rest), 2e-3f);
        ASSERT_LT(B::Length(c.Point(0) - W::Apply(b, rest[0])), 1e-4f) << "the root on the handle";
        ASSERT_LE(c.MaxBend(), cfg.max_bend + 0.01f) << "never folded";
    }
    // A long gap (a load) starts it again at rest.
    EXPECT_EQ(c.Step(a, a, 0.5f, 1.0f, cfg), 0);
    EXPECT_NEAR(c.Extension(), 1.0f, 1e-6f);
}

TEST(WhipChain, BoneRotationsFollowTheLinksWithoutFlips) {
    const auto rest = IdleChain();
    // Idle rotations: +X along the idle line, a 90-degree roll so the twist is visible.
    std::array<B::Quat, N> idle{};
    const B::Quat base = B::FromTo({1.0f, 0.0f, 0.0f}, rest[1] - rest[0]);
    for (auto& q : idle) {
        q = B::Normalize(base * B::AxisAngle({1.0f, 0.0f, 0.0f}, 0.5f * B::Pi));
    }
    std::array<B::Quat, N> out{};
    // Straight: the idle rotations back.
    W::ChainRotations(rest.data(), idle.data(), N, out.data());
    for (int i = 0; i < N; ++i) {
        EXPECT_LT(B::AngleBetween(out[i], idle[i]), 1e-3f) << i;
    }
    // A hook bending back past 180 degrees from the idle line (the whip trailing behind the hand):
    // every bone's +X along its link, neighbours differ by no more than their link's turn.
    std::array<B::Vec3, N> p{};
    p[0] = rest[0];
    const B::Vec3 d0 = B::Normalize(rest[1] - rest[0]);
    const B::Vec3 axis{0.0f, 0.0f, 1.0f};
    for (int i = 1; i < N; ++i) {
        const float turn = 200.0f * B::Deg * static_cast<float>(i) / static_cast<float>(N - 1);
        p[i] = p[i - 1] + B::RotateAbout(d0, axis, turn) * 0.0336f;
    }
    W::ChainRotations(p.data(), idle.data(), N, out.data());
    for (int i = 0; i + 1 < N; ++i) {
        const B::Vec3 x = B::Rotate(out[i], {1.0f, 0.0f, 0.0f});
        EXPECT_GT(B::Dot(x, B::Normalize(p[i + 1] - p[i])), 0.9999f) << i;
    }
    for (int i = 1; i < N; ++i) {
        const float link_turn = B::Angle(p[std::min(i + 1, N - 1)] - p[i], p[i] - p[i - 1]);
        EXPECT_LT(B::AngleBetween(out[i - 1], out[i]), link_turn + 0.02f) << i;
    }
}

TEST(WhipChain, LocalsRebuildThePointsWithSlightlyOffUnitClipQuaternions) {
    // The idle clip as the game samples it: the chain bones' local scale (Bone_Chain_1 x0.632 on X,
    // the last one x1.582), quaternions 0.2% off unit (decompression), a bent chain to write.
    const auto rest = IdleChain();
    std::array<B::QsTransform, N> clip{};
    for (int k = 0; k < N; ++k) {
        clip[k].t = k == 0 ? B::Vec3{} : B::Vec3{0.0336f, 0.0f, 0.0f};
        clip[k].q = B::Quat{0.0f, 0.0f, 0.0f, 1.002f};
        clip[k].s = k == 0 ? B::Vec3{0.632f, 1.0f, 1.0f}
                    : k == N - 1 ? B::Vec3{1.582f, 1.0f, 1.0f}
                                 : B::Vec3{1.0f, 1.0f, 1.0f};
    }
    B::QsTransform parent{};
    parent.t = rest[0];
    const B::Quat base = B::FromTo({1.0f, 0.0f, 0.0f}, rest[1] - rest[0]);
    parent.q = B::Quat{base.x * 1.002f, base.y * 1.002f, base.z * 1.002f, base.w * 1.002f};
    std::array<B::Vec3, N> p{};
    p[0] = rest[0];
    for (int i = 1; i < N; ++i) {
        const float turn = 150.0f * B::Deg * static_cast<float>(i) / static_cast<float>(N - 1);
        p[i] = p[i - 1] + B::RotateAbout(B::Normalize(rest[1] - rest[0]), {0.0f, 0.0f, 1.0f}, turn) *
                              (0.0336f * 4.0f); // stretched x4
    }
    std::array<B::Quat, N> idle{};
    for (auto& q : idle) {
        q = B::Normalize(base);
    }
    std::array<B::Quat, N> q{};
    W::ChainRotations(p.data(), idle.data(), N, q.data());
    std::array<B::QsTransform, N> out{};
    const B::QsTransform end =
        W::ChainLocals(parent, p.data(), q.data(), clip.data(), N, 1.0f, out.data());
    EXPECT_LT(B::Length(end.t - p[N - 1]), 1e-4f) << "the locals put the last bone on its point";
    B::QsTransform m = parent;
    m.q = B::Normalize(m.q);
    for (int k = 0; k < N; ++k) {
        EXPECT_NEAR(std::sqrt(B::Dot(out[k].q, out[k].q)), 1.0f, 1e-5f) << k;
        if (k > 0) {
            EXPECT_NEAR(B::Length(out[k].t), 0.0336f * 4.0f, 1e-4f) << k;
        }
        EXPECT_FLOAT_EQ(out[k].s.x, clip[k].s.x) << "the clip's scale kept";
        m = B::Compose(m, out[k]);
        EXPECT_LT(B::Length(m.t - p[k]), 1e-4f) << k;
    }
    // Blended halfway: finite, unit, between the clip's and the chain's.
    W::ChainLocals(parent, p.data(), q.data(), clip.data(), N, 0.5f, out.data());
    for (int k = 0; k < N; ++k) {
        EXPECT_TRUE(B::IsFinite(out[k]));
        EXPECT_NEAR(std::sqrt(B::Dot(out[k].q, out[k].q)), 1.0f, 1e-5f) << k;
    }
}

TEST(WhipMoves, TheBeastCuttersFormBRowsOnTheChain) {
    const W::WhipMoves* bc = W::FindWhip(2400);
    ASSERT_NE(bc, nullptr);
    EXPECT_TRUE(bc->tested);
    EXPECT_EQ(bc->rows[0].judge, 400); // the first R1
    EXPECT_EQ(bc->rows[1].judge, 500); // R2
    EXPECT_EQ(bc->rows[2].judge, 510); // the charged R2
    for (std::size_t k = 0; k < 3; ++k) {
        const auto& r = bc->rows[k];
        EXPECT_EQ(r.dmy1, 12100) << k; // the tip
        EXPECT_EQ(r.dmy2, 12130) << k; // the middle blade
        EXPECT_EQ(r.Group(), 12) << k; // part 2: form B
        EXPECT_GT(r.mv, 90) << k;
        EXPECT_GT(bc->reach[k], 3.5f) << k;
    }
    EXPECT_LT(bc->reach[0], bc->reach[1]);
    EXPECT_LT(bc->reach[1], bc->reach[2]);
    // Into the generated weapon table's form A, untouched.
    const VR::Melee::WeaponMoves* base = VR::Melee::FindMoves(2400);
    ASSERT_NE(base, nullptr);
    EXPECT_FALSE(base->rows[3].Valid()) << "melee_moves.h leaves the whip out";
    const VR::Melee::WeaponMoves m = W::WithWhipRows(*base, *bc);
    for (std::size_t k = 0; k < 3; ++k) {
        EXPECT_EQ(m.rows[k].judge, base->rows[k].judge);
        EXPECT_EQ(m.rows[3 + k].judge, bc->rows[k].judge);
    }
    EXPECT_FALSE(W::FindWhip(700) != nullptr) << "the Saw Cleaver is no whip";
    const W::WhipMoves* cane = W::FindWhip(2200);
    ASSERT_NE(cane, nullptr);
    EXPECT_TRUE(cane->tested) << "both whips are physical with whip=physical";
}

TEST(WhipMoves, TheThreadedCanesFormBRowsOnTheChain) {
    const W::WhipMoves* cane = W::FindWhip(2200);
    ASSERT_NE(cane, nullptr);
    EXPECT_EQ(cane->rows[0].judge, 400);
    EXPECT_EQ(cane->rows[1].judge, 500);
    EXPECT_EQ(cane->rows[2].judge, 510);
    for (std::size_t k = 0; k < 3; ++k) {
        EXPECT_EQ(cane->rows[k].Group(), 12) << k;
        EXPECT_EQ(cane->rows[k].dmy1, 12100) << k;
        EXPECT_GT(cane->reach[k], 2.5f) << k;
    }
    EXPECT_NEAR(cane->idle_tip, 0.892f, 0.01f) << "a999: the cane's tip 0.89 m from the grip";
    const VR::Melee::WeaponMoves* base = VR::Melee::FindMoves(2200);
    ASSERT_NE(base, nullptr);
    const VR::Melee::WeaponMoves m = W::WithWhipRows(*base, *cane);
    EXPECT_EQ(m.rows[3].whoosh, 770) << "the R1's whoosh";
    EXPECT_EQ(m.rows[4].whoosh, 770) << "an R2 with no whoosh of its own takes the R1's, never 611";
    EXPECT_EQ(m.rows[5].whoosh, 5001);
    EXPECT_EQ(m.rows[0].judge, 0) << "form A untouched";
}

TEST(WhipMoves, BothWhipsSwingAtTheirOwnPace) {
    // The game's R1 / R2 cancel in the whips' own clips (a141 / a127 040000, 041000, 041100).
    EXPECT_FLOAT_EQ(W::WhipRecover(2400, 0), 1.233f);
    EXPECT_FLOAT_EQ(W::WhipRecover(2400, 1), 2.067f);
    EXPECT_FLOAT_EQ(W::WhipRecover(2400, 2), 2.100f);
    EXPECT_FLOAT_EQ(W::WhipRecover(2200, 0), 0.767f);
    EXPECT_FLOAT_EQ(W::WhipRecover(2200, 1), 1.333f);
    EXPECT_FLOAT_EQ(W::WhipRecover(2200, 2), 1.267f);
    EXPECT_FLOAT_EQ(W::WhipRecover(700, 0), 0.0f) << "a blade keeps melee_moves.h's Paces";
    EXPECT_FLOAT_EQ(W::WhipRecover(2400, 3), 0.0f);
    for (const W::WhipMoves& w : W::Whips) {
        for (std::size_t k = 0; k < 3; ++k) {
            EXPECT_GE(w.recover[k], 0.45f) << w.name << " " << k;
            EXPECT_LE(w.recover[k], 2.2f) << w.name << " " << k;
        }
        EXPECT_LE(w.recover[0], w.recover[1]) << w.name << ": a lash recovers before an R2";
    }
}

TEST(WhipMoves, AFlickIntoTheWhipHitsWithTheChain) {
    for (const std::int32_t v : {2400, 2200}) {
        const VR::Melee::MoveRow* tr = W::WhipTransformRow(v);
        ASSERT_NE(tr, nullptr) << v;
        EXPECT_EQ(tr->judge, 390) << v << ": the game's trick attack A->B";
        EXPECT_EQ(tr->dmy1, 12100) << v << ": on the chain's tip";
        EXPECT_EQ(tr->Group(), 12) << v;
    }
    EXPECT_EQ(W::WhipTransformRow(700), nullptr) << "the Saw Cleaver is no whip";
}

TEST(WhipDrive, IdleDrivesATransformClipOnlyOnceASwingLashes) {
    W::DriveState s{};
    EXPECT_TRUE(W::StepDrive(s, true, W::IdleClipId, false)) << "idle: always the simulation's";
    EXPECT_FALSE(W::StepDrive(s, false, W::IdleClipId, true)) << "not ours (whip=canned, form A)";
    // A transform's clip (the weapon-only transform: a000_019520, the whip uncoiling).
    EXPECT_FALSE(W::StepDrive(s, true, 19520, false)) << "a squeeze alone: the clip's uncoil";
    EXPECT_TRUE(W::StepDrive(s, true, 19520, true)) << "a flick: the swing lashes the chain";
    EXPECT_TRUE(W::StepDrive(s, true, 19520, false)) << "... and keeps it for the rest of the clip";
    EXPECT_TRUE(W::StepDrive(s, true, W::IdleClipId, false));
    EXPECT_FALSE(s.taken);
    EXPECT_FALSE(W::StepDrive(s, true, 19520, false)) << "the next transform starts with the clip";
    EXPECT_TRUE(W::StepDrive(s, true, 19150, true)) << "the cane's A->B (a000_019150)";
    EXPECT_FALSE(W::StepDrive(s, false, 19150, true)) << "the form not locked yet: not ours";
    EXPECT_FALSE(s.taken);
    // Any other clip the game plays on the part (a canned attack, a visceral) keeps its pose.
    EXPECT_FALSE(W::StepDrive(s, true, 141040000, true));
    EXPECT_FALSE(W::StepDrive(s, true, 141043300, true));
    EXPECT_FALSE(W::StepDrive(s, true, -1, true));
    EXPECT_TRUE(W::IsTransformClip(19530));
    EXPECT_FALSE(W::IsTransformClip(18999));
    EXPECT_FALSE(W::IsTransformClip(W::IdleClipId));
}

TEST(WhipChain, EachMovePaysOutToItsOwnReach) {
    const W::WhipMoves* bc = W::FindWhip(2400);
    const W::WhipMoves* cane = W::FindWhip(2200);
    ASSERT_NE(bc, nullptr);
    ASSERT_NE(cane, nullptr);
    const float light = W::PayoutFor(bc->reach, 0, 1.0f, bc->idle_tip, 0.84f);
    const float heavy = W::PayoutFor(bc->reach, 1, 1.0f, bc->idle_tip, 0.84f);
    const float charged = W::PayoutFor(bc->reach, 2, 1.0f, bc->idle_tip, 0.84f);
    EXPECT_NEAR(light, 4.01f, 0.02f);
    EXPECT_NEAR(heavy, 6.11f, 0.02f);
    EXPECT_NEAR(charged, 6.96f, 0.02f);
    EXPECT_NEAR(W::PayoutFor(bc->reach, 7, 1.0f, bc->idle_tip, 0.84f), charged, 1e-6f);
    EXPECT_LT(W::PayoutFor(bc->reach, 0, 0.5f, bc->idle_tip, 0.84f), light) << "whip_reach_scale";
    EXPECT_GT(W::PayoutFor(cane->reach, 0, 1.0f, cane->idle_tip, 0.719f), 4.5f);
}


// ---- Idle lines of our own ------------------------------------------------------------------------

// A straight idle line in the part's XY plane: `n` bones `link` m apart from `root`, at `deg`
// degrees from the part's +X (the blade runs -Y out of the hand), each bone's +X along its link (a
// rotation about Z), as the games' idle clips hold their chains.
W::WhipRest SyntheticLine(std::int32_t variation, int bones, int n, float link, B::Vec3 root,
                          float deg) {
    W::WhipRest r;
    r.variation = variation;
    r.bones = bones;
    r.n = n;
    const float a = deg * B::Deg;
    const B::Vec3 dir{std::cos(a), std::sin(a), 0.0f};
    for (int k = 0; k < n; ++k) {
        const B::Vec3 p = root + dir * (link * static_cast<float>(k));
        r.t[static_cast<std::size_t>(k)] = {p.x, p.y, p.z};
        r.q[static_cast<std::size_t>(k)] = {0.0f, 0.0f, std::sin(0.5f * a), std::cos(0.5f * a)};
    }
    r.links = link * static_cast<float>(n - 1);
    return r;
}

// Cane-like: 24 wire bones, 3.1 cm apart, back along the forearm (+Y) from just behind the grip.
W::WhipRest SyntheticCane() {
    return SyntheticLine(2200, 53, 24, 0.031f, B::Vec3{-0.04f, 0.09f, 0.0f}, 114.0f);
}

// Beast-Cutter-like: 26 chain bones, 3.35 cm apart, out of the front of the hand (-Y).
W::WhipRest SyntheticBeastCutter() {
    return SyntheticLine(2400, 42, 26, 0.0335f, B::Vec3{0.03f, -0.21f, 0.0f}, -62.0f);
}

TEST(WhipRest, IdleLinesAreValidChains) {
    for (const W::WhipRest& r : {SyntheticCane(), SyntheticBeastCutter()}) {
        std::array<B::Vec3, W::MaxPoints> p{};
        float links = 0.0f;
        for (int k = 0; k < r.n; ++k) {
            p[k] = {r.t[k][0], r.t[k][1], r.t[k][2]};
            const B::Quat q{r.q[k][0], r.q[k][1], r.q[k][2], r.q[k][3]};
            EXPECT_NEAR(std::sqrt(B::Dot(q, q)), 1.0f, 1e-4f);
            if (k > 0) {
                links += B::Length(p[k] - p[k - 1]);
            }
        }
        EXPECT_NEAR(links, r.links, 1e-3f);
        W::Chain c;
        EXPECT_TRUE(c.Init(p.data(), r.n)) << r.variation;
    }
    // The whips the idle lines are read for: the two whips of the moves table.
    for (const auto& part : W::WhipParts) {
        EXPECT_NE(W::FindWhip(part.variation), nullptr) << part.model;
    }
}

// ---- The idle-clip reader (core/vr/whip_idle.h) on a packfile of our own ---------------------

using u8 = std::uint8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;

// An hk_2014-style packfile: "__classnames__", "__types__", "__data__"; local fixups (pointer at
// -> target) and virtual fixups (object -> its class name), as the reader walks them.
struct PackWriter {
    std::vector<u8> data;
    std::vector<std::pair<s32, s32>> local;
    std::vector<std::pair<s32, std::string>> objects;

    s32 Put(const void* bytes, std::size_t n, std::size_t align = 16) {
        while (data.size() % align != 0) {
            data.push_back(0);
        }
        const auto at = static_cast<s32>(data.size());
        const auto* b = static_cast<const u8*>(bytes);
        data.insert(data.end(), b, b + n);
        return at;
    }
    s32 Zeros(std::size_t n, std::size_t align = 16) {
        const std::vector<u8> z(n, 0);
        return Put(z.data(), n, align);
    }
    template <typename T>
    void Set(s32 at, T v) {
        std::memcpy(&data[static_cast<std::size_t>(at)], &v, sizeof(v));
    }
    void Array(s32 field, s32 target, s32 count) {
        local.emplace_back(field, target);
        Set(field + 8, count);
    }
    std::vector<u8> Finish() const {
        std::string names;
        std::vector<s32> name_at;
        for (const auto& o : objects) {
            name_at.push_back(static_cast<s32>(names.size()));
            names += o.second;
            names.push_back('\0');
        }
        std::vector<u8> out(0x100, 0);
        const auto put32 = [&out](std::size_t at, s32 v) { std::memcpy(&out[at], &v, 4); };
        put32(0x0C, 11); // version (hk_2014: 0x40-byte section headers)
        put32(0x14, 3);  // sections
        const auto pad = [&out] {
            while (out.size() % 16 != 0) {
                out.push_back(0);
            }
        };
        const auto section = [&](int i, const char* name, s32 start, s32 local_at, s32 global_at,
                                 s32 virt_at, s32 end) {
            const std::size_t h = 0x40 + 0x40 * static_cast<std::size_t>(i);
            std::memcpy(&out[h], name, std::strlen(name));
            put32(h + 20, start);
            put32(h + 24, local_at);
            put32(h + 28, global_at);
            put32(h + 32, virt_at);
            put32(h + 36, end);
            put32(h + 40, end);
            put32(h + 44, end);
        };
        const auto classes = static_cast<s32>(out.size());
        out.insert(out.end(), names.begin(), names.end());
        pad();
        section(0, "__classnames__", classes, 0, 0, 0, static_cast<s32>(names.size()));
        section(1, "__types__", static_cast<s32>(out.size()), 0, 0, 0, 0);
        const auto start = static_cast<s32>(out.size());
        out.insert(out.end(), data.begin(), data.end());
        pad();
        const auto local_at = static_cast<s32>(out.size()) - start;
        for (const auto& [src, dst] : local) {
            out.resize(out.size() + 8);
            put32(out.size() - 8, src);
            put32(out.size() - 4, dst);
        }
        const auto virt_at = static_cast<s32>(out.size()) - start;
        for (std::size_t i = 0; i < objects.size(); ++i) {
            out.resize(out.size() + 12);
            put32(out.size() - 12, objects[i].first);
            put32(out.size() - 8, 0);
            put32(out.size() - 4, name_at[i]);
        }
        const auto end = static_cast<s32>(out.size()) - start;
        section(2, "__data__", start, local_at, virt_at, virt_at, end);
        return out;
    }
};

// Our synthetic whip: Master, B_blade_root, a leaf on the blade root (not the chain), then `n`
// chain bones each `link` m along its parent's +X, a tip blade on the last.
constexpr int SynthChain = 10;
constexpr float SynthLink = 0.05f;

std::vector<std::string> SynthNames() {
    std::vector<std::string> names{"Master", "B_blade_root", "B_handle"};
    for (int k = 0; k < SynthChain; ++k) {
        names.push_back("Bone_Chain_" + std::to_string(k + 1));
    }
    names.push_back("B_blade_tip");
    return names;
}

std::vector<s16> SynthParents() {
    std::vector<s16> p{-1, 0, 1};
    for (int k = 0; k < SynthChain; ++k) {
        p.push_back(static_cast<s16>(k == 0 ? 1 : 2 + k));
    }
    p.push_back(static_cast<s16>(2 + SynthChain));
    return p;
}

std::vector<u8> SynthSkeleton() {
    const auto names = SynthNames();
    const auto parents = SynthParents();
    const auto n = static_cast<s32>(names.size());
    PackWriter w;
    const s32 skel = w.Zeros(0x50);
    w.objects.emplace_back(skel, "hkaSkeleton");
    const s32 par = w.Put(parents.data(), parents.size() * sizeof(s16));
    const s32 bones = w.Zeros(16 * names.size());
    std::vector<float> pose;
    for (s32 i = 0; i < n; ++i) {
        // Reference pose: Master 5 cm up, the blade root 10 cm down the handle, every other bone
        // `SynthLink` along +X (the reader keeps a bone's reference pose when no track drives it).
        const float tx = i >= 3 ? SynthLink : 0.0f;
        const float ty = i == 0 ? 0.05f : i == 1 ? -0.1f : 0.0f;
        const float t[12] = {tx, ty, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f};
        pose.insert(pose.end(), t, t + 12);
    }
    const s32 ref = w.Put(pose.data(), pose.size() * sizeof(float));
    for (s32 i = 0; i < n; ++i) {
        const auto& name = names[static_cast<std::size_t>(i)];
        const s32 at = w.Put(name.c_str(), name.size() + 1, 2);
        w.local.emplace_back(bones + 16 * i, at);
    }
    w.Array(skel + 0x18, par, n);
    w.Array(skel + 0x28, bones, n);
    w.Array(skel + 0x38, ref, n);
    return w.Finish();
}

// The idle clip: one block of tracks for every bone but Master (bound by the animation binding).
// The blade root turned `deg` about Z (a static float rotation); the chain bones' positions static
// (the first one an 8-bit spline whose first control point is the link), their rotations identity
// (THREECOMP48, static).
std::vector<u8> SynthIdleClip(float deg) {
    const auto n = static_cast<s32>(SynthNames().size());
    const s32 tracks = n - 1;
    std::vector<u8> block;
    const auto pad = [&block](std::size_t a) {
        while (block.size() % a != 0) {
            block.push_back(0);
        }
    };
    const auto f32 = [&block](float v) {
        u8 b[4];
        std::memcpy(b, &v, 4);
        block.insert(block.end(), b, b + 4);
    };
    const auto u16v = [&block](std::uint16_t v) {
        block.push_back(static_cast<u8>(v & 0xFF));
        block.push_back(static_cast<u8>(v >> 8));
    };
    for (s32 ti = 0; ti < tracks; ++ti) {
        const s32 bone = ti + 1;
        const bool spline = bone == 3;
        // qtypes: positions 8-bit (0), rotations float (5) for the blade root, THREECOMP48 (2)
        // otherwise; ptypes static xyz or a spline on x; rtypes static; no scale.
        block.push_back(static_cast<u8>((bone == 1 ? 5 : 2) << 2));
        block.push_back(spline ? 0x10 : 0x07);
        block.push_back(0x0F);
        block.push_back(0x00);
    }
    pad(4);
    const float a = deg * 3.14159265f / 180.0f;
    for (s32 ti = 0; ti < tracks; ++ti) {
        const s32 bone = ti + 1;
        if (bone == 3) {
            u16v(1);          // n: two control points
            block.push_back(1); // degree 1
            for (u8 k : {0, 0, 1, 1}) {
                block.push_back(k);
            }
            pad(4);
            f32(SynthLink);         // x: min
            f32(SynthLink + 0.255f); // x: max
            block.push_back(0);     // control point 0: min
            block.push_back(255);   // control point 1: max
            pad(4);
        } else {
            const float tx = bone >= 3 ? SynthLink : 0.0f;
            const float ty = bone == 1 ? -0.1f : 0.0f;
            f32(tx);
            f32(ty);
            f32(0.0f);
            pad(4);
        }
        if (bone == 1) {
            pad(4);
            f32(0.0f);
            f32(0.0f);
            f32(std::sin(0.5f * a));
            f32(std::cos(0.5f * a));
        } else {
            pad(2);
            u16v(0xBFFF); // identity: x, y, z at the middle, w in the 4th place
            u16v(0xBFFF);
            u16v(0x3FFF);
        }
        pad(4);
        pad(4); // no scale
    }
    PackWriter w;
    const s32 anim = w.Zeros(0xB0);
    w.objects.emplace_back(anim, "hkaSplineCompressedAnimation");
    const s32 binding = w.Zeros(0x40);
    w.objects.emplace_back(binding, "hkaAnimationBinding");
    w.Set(anim + 0x14, tracks);
    w.Set(anim + 0x3C, s32{1}); // blocks
    const s32 zero = 0;
    const s32 offsets = w.Put(&zero, 4);
    w.Array(anim + 0x58, offsets, 1);
    const s32 bytes = w.Put(block.data(), block.size());
    w.Array(anim + 0x98, bytes, static_cast<s32>(block.size()));
    std::vector<s16> map;
    for (s32 ti = 0; ti < tracks; ++ti) {
        map.push_back(static_cast<s16>(ti + 1));
    }
    const s32 m = w.Put(map.data(), map.size() * sizeof(s16));
    w.Array(binding + 0x20, m, tracks);
    return w.Finish();
}

TEST(WhipRest, ReadsAnIdleClipsFirstFrame) {
    namespace I = W::Idle;
    std::string why;
    const auto skel = SynthSkeleton();
    const auto bones = I::ParseSkeleton(skel, why);
    ASSERT_TRUE(bones.has_value()) << why;
    ASSERT_EQ(bones->size(), SynthNames().size());
    EXPECT_EQ((*bones)[1].name, "B_blade_root");
    EXPECT_EQ((*bones)[4].parent, 3);
    const auto clip = SynthIdleClip(114.0f);
    const auto frame = I::ParseFrame0(clip, why);
    ASSERT_TRUE(frame.has_value()) << why;
    ASSERT_EQ(frame->tracks.size(), SynthNames().size() - 1);
    EXPECT_EQ(frame->track_to_bone[0], 1);
    EXPECT_NEAR(frame->tracks[2].t[0], SynthLink, 1e-7); // the spline's first control point
    const auto line = I::BuildIdle(2200, *bones, *frame, why);
    ASSERT_TRUE(line.has_value()) << why;
    EXPECT_EQ(line->variation, 2200);
    EXPECT_EQ(line->bones, static_cast<int>(SynthNames().size()));
    EXPECT_EQ(line->n, SynthChain); // B_handle (no children) is not the chain
    EXPECT_NEAR(line->links, SynthLink * (SynthChain - 1), 1e-6f);
    // Master keeps its reference pose (5 cm up, no track); the blade root 10 cm down from it,
    // turned 114 degrees; the chain along the turned +X.
    const float a = 114.0f * B::Deg;
    for (int k = 0; k < line->n; ++k) {
        const float d = SynthLink * static_cast<float>(k + 1);
        EXPECT_NEAR(line->t[k][0], d * std::cos(a), 1e-5f) << k;
        EXPECT_NEAR(line->t[k][1], 0.05f - 0.1f + d * std::sin(a), 1e-5f) << k;
        EXPECT_NEAR(line->t[k][2], 0.0f, 1e-6f) << k;
        EXPECT_NEAR(line->q[k][2], std::sin(0.5f * a), 1e-5f) << k;
        EXPECT_NEAR(line->q[k][3], std::cos(0.5f * a), 1e-5f) << k;
    }
    // Turned to the blade (it runs back along +Y), like the cane's.
    std::array<B::Vec3, W::MaxPoints> p{};
    std::array<B::Quat, W::MaxPoints> q{};
    for (int k = 0; k < line->n; ++k) {
        p[k] = {line->t[k][0], line->t[k][1], line->t[k][2]};
        q[k] = {line->q[k][0], line->q[k][1], line->q[k][2], line->q[k][3]};
    }
    EXPECT_TRUE(W::RestToBlade(p.data(), q.data(), line->n));
    // What it refuses: no skeleton, no animation, no B_blade_root, a cut file.
    EXPECT_FALSE(I::ParseSkeleton(clip, why).has_value());
    EXPECT_FALSE(I::ParseFrame0(skel, why).has_value());
    auto renamed = *bones;
    renamed[1].name = "B_blade";
    EXPECT_FALSE(I::BuildIdle(2200, renamed, *frame, why).has_value());
    std::vector<u8> cut(clip.begin(), clip.begin() + 0x80);
    EXPECT_FALSE(I::ParseFrame0(cut, why).has_value());
}

// The user's own whips (game-data tier, skipped without the dump): the idle lines read from the
// parts' anibnds have the hook's chains (bone counts), the link lengths and tip distance the hook
// measured live (rounded measurements, run wf_bc1), the cane's backward and the Beast Cutter's
// forward.
TEST(WhipRest, ReadsTheGamesIdleLines) {
#ifdef BBVR_GAME_DATA_TESTS
    const char* env = std::getenv("BBVR_GAME_DIR");
    const std::string dir = env != nullptr ? env : "./ROM/CUSA03173/dvdroot_ps4";
    for (const auto& part : W::WhipParts) {
        std::ifstream in(dir + "/parts/" + part.model + ".partsbnd.dcx", std::ios::binary);
        if (!in) {
            std::printf("  (skipped: %s not found)\n", part.model);
            return;
        }
        const std::vector<u8> file((std::istreambuf_iterator<char>(in)), {});
        const auto dcx = VR::TpfPatch::ParseDcx(file);
        ASSERT_TRUE(dcx.has_value());
        std::vector<u8> bnd(dcx->unpacked);
        mz_ulong length = dcx->unpacked;
        ASSERT_EQ(mz_uncompress(bnd.data(), &length, file.data() + dcx->stream, dcx->packed), MZ_OK);
        const auto anibnd =
            VR::Body::C0000::Rig::Bnd4Find(bnd, std::string(part.model) + "_2.anibnd");
        ASSERT_TRUE(anibnd.has_value());
        std::string why;
        const auto r = W::Idle::FromAnibnd(part.variation, *anibnd, why);
        ASSERT_TRUE(r.has_value()) << why;
        std::array<B::Vec3, W::MaxPoints> p{};
        std::array<B::Quat, W::MaxPoints> q{};
        for (int k = 0; k < r->n; ++k) {
            p[k] = {r->t[k][0], r->t[k][1], r->t[k][2]};
            q[k] = {r->q[k][0], r->q[k][1], r->q[k][2], r->q[k][3]};
        }
        W::Chain c;
        EXPECT_TRUE(c.Init(p.data(), r->n)) << part.model;
        const float tip = B::Length(p[r->n - 1]);
        if (part.variation == 2400) {
            EXPECT_EQ(r->n, 26);
            EXPECT_EQ(r->bones, 42);
            EXPECT_NEAR(r->links, 0.837f, 0.002f);
            EXPECT_NEAR(tip, 1.036f, 0.002f);
            EXPECT_FALSE(W::RestToBlade(p.data(), q.data(), r->n));
        } else {
            EXPECT_EQ(r->n, 24);
            EXPECT_EQ(r->bones, 53);
            EXPECT_NEAR(r->links, 0.719f, 0.002f);
            EXPECT_TRUE(W::RestToBlade(p.data(), q.data(), r->n));
        }
        std::printf("  %s: %d chain bones of %d, links %.3f m, the last %.3f m from the grip\n",
                    part.model, r->n, r->bones, r->links, tip);
    }
#else
    std::printf("  (build with BBVR_GAME_DATA_TESTS to read the dump's whips)\n");
#endif
}

TEST(WhipChain, TheCanesLashReachesItsOwnReach) {
    // A cane-like idle line (SyntheticCane: 24 bones back along the forearm), turned into the
    // stroke's part frame (its blade along -Y, as the Beast Cutter's: the test's stroke swings the
    // part's -Y).
    const W::WhipRest cane_rest = SyntheticCane();
    const W::WhipRest* r = &cane_rest;
    const int NC = r->n;
    std::array<B::Vec3, W::MaxPoints> rest{};
    for (int k = 0; k < NC; ++k) {
        rest[k] = {-r->t[k][0], -r->t[k][1], r->t[k][2]}; // 180 degrees about Z
    }
    W::Chain c;
    ASSERT_TRUE(c.Init(rest.data(), NC));
    const W::WhipMoves* cane = W::FindWhip(2200);
    ASSERT_NE(cane, nullptr);
    const float s_max = W::PayoutFor(cane->reach, 0, 1.0f, cane->idle_tip, r->links);
    const Stroke s(-75.0f, 5.0f, 45.0f, -10.0f, 6.0f);
    c.Reset(s.At(0.0f));
    const W::ChainConfig cfg{};
    const float dt = 1.0f / 30.0f;
    W::Frame prev = s.At(0.0f);
    float reach = 0.0f;
    float bend = 0.0f;
    for (float t = dt; t < 1.4f; t += dt) {
        const W::Frame f = s.At(t);
        const bool lash = t > 0.35f && t < 0.9f;
        ASSERT_GT(c.Step(prev, f, dt, lash ? s_max : 1.0f, cfg), 0);
        prev = f;
        reach = std::max(reach, B::Length(c.Point(NC - 1) - f.t));
        bend = std::max(bend, c.MaxBend());
    }
    // The chain's end: the cane's reach less its tip blade (0.07 m past the last wire bone at idle).
    EXPECT_GT(reach, cane->reach[0] - 0.45f);
    EXPECT_LT(reach, cane->reach[0] + 0.05f);
    EXPECT_LE(bend, cfg.max_bend + 0.01f);
    EXPECT_NEAR(c.Extension(), 1.0f, 1e-3f);
}

TEST(WhipChain, RejectsBadChains) {
    W::Chain c;
    const std::array<B::Vec3, 3> dup{B::Vec3{}, B::Vec3{}, B::Vec3{0.0f, -1.0f, 0.0f}};
    EXPECT_FALSE(c.Init(dup.data(), 3));
    const auto rest = IdleChain();
    EXPECT_FALSE(c.Init(rest.data(), 2));
    EXPECT_TRUE(c.Init(rest.data(), N));
    EXPECT_FLOAT_EQ(W::ExtensionForReach(1.0f, 1.17f, 0.84f), 1.0f);
}

// ---- The swing's speed pays the chain out (core/vr/whip_spin.h) ---------------------------------

TEST(WhipPayout, FollowsTheSpeedHoldsThenReelsIn) {
    W::Payout p;
    const W::PayoutConfig c{};
    const float cap = 4.0f;
    const float dt = 1.0f / 60.0f;
    EXPECT_FLOAT_EQ(p.Step(0.3f, cap, dt, c), 1.0f) << "a still hand: the idle stick";
    EXPECT_FLOAT_EQ(p.Step(c.v_start, cap, dt, c), 1.0f) << "not yet";
    const float mid = p.Step(0.5f * (c.v_start + c.v_full), cap, dt, c);
    EXPECT_GT(mid, 1.5f) << "a moderate swing pays it partly out";
    EXPECT_LT(mid, 3.5f);
    EXPECT_FLOAT_EQ(p.Step(c.v_full + 1.0f, cap, dt, c), cap) << "a fast one fully";
    // Kept up (a spin a little slower than full): it stays out.
    for (int i = 0; i < 120; ++i) {
        EXPECT_FLOAT_EQ(p.Step(c.v_keep + 0.2f, cap, dt, c), cap);
    }
    // Slow: out for the hold, then in.
    float t = 0.0f;
    while (p.Step(0.2f, cap, dt, c) > 1.0f) {
        t += dt;
        ASSERT_LT(t, 2.0f);
    }
    EXPECT_NEAR(t, c.hold_s, 2.0f * dt) << "reels in after the hold";
    EXPECT_TRUE(p.Reeled());
    EXPECT_FALSE(p.Out());
    // A short dip (the bottom of a stroke) does not reel it in.
    p.Step(9.0f, cap, dt, c);
    for (int i = 0; i < 10; ++i) {
        p.Step(0.5f, cap, dt, c);
    }
    EXPECT_FLOAT_EQ(p.Step(3.0f, cap, dt, c), cap);
}

TEST(CaneRecall, ShorterPauseKeepsTheOriginalReturnDuration) {
    for (const int hz : {30, 60, 90}) {
        SCOPED_TRACE(hz);
        const float dt = 1.0f / hz;
        const auto rest = IdleChain();
        const auto cfg = W::ConfigFor(2200);
        W::PayoutConfig pc;
        pc.hold_s = W::HoldFor(2200, pc.hold_s);
        const float cap = W::ReachExtension(2200, 4.0f, .892f, .84f);
        const W::Frame hand{{0, 2, 0}, B::FromTo(rest[N-1]-rest[0], {0, 0, -1})};
        std::array<B::Vec3, N> points;
        for (int i = 0; i < N; ++i)
            points[i] = W::Apply(hand, rest[0] + (rest[i]-rest[0]) * cap);
        W::Chain chain;
        ASSERT_TRUE(chain.Init(rest.data(), N));
        chain.ResetTo(points.data(), cap);
        W::Payout payout;
        payout.Step(6, cap, dt, pc);
        float starts = -1, closed = -1;
        for (int frame = 1; frame <= hz; ++frame) {
            const float target = payout.Step(0, cap, dt, pc);
            if (target == 1 && starts < 0) starts = (frame - 1) * dt;
            chain.Step(hand, hand, dt, target, cfg);
            if (chain.Extension() <= 1.0001f && closed < 0) closed = frame * dt;
        }
        ASSERT_GE(starts, 0);
        ASSERT_GT(closed, starts);
        EXPECT_NEAR(starts, .42f, dt + 1e-5f);
        EXPECT_NEAR(closed - starts, .25f, dt + 1e-5f);
        std::printf("  cane return %dHz: starts %.3f s, closes %.3f s, duration %.3f s\n",
                    hz, starts, closed, closed-starts);
    }
    EXPECT_NEAR(W::HoldFor(2400, .6f), 1.4f, 1e-6f); // Cutter waits 1.4 s before auto-recall
}
TEST(CaneRecall, RepeatedDirectionChangesResetThePause) {
    for (const int hz : {30, 60, 90}) {
        SCOPED_TRACE(hz);
        const float dt = 1.0f / hz;
        W::PayoutConfig pc;
        pc.hold_s = W::HoldFor(2200, pc.hold_s);
        W::Payout payout;
        payout.Step(6, 4.432f, dt, pc);
        // Six seconds of changing directions: each reversal has 0.30 s of low speed.
        // The slow intervals must not add together and pull the whip in during the next arc.
        for (int reversal = 0; reversal < 20; ++reversal) {
            for (int frame = 0; frame < hz * 3 / 10; ++frame) {
                EXPECT_GT(payout.Step(.2f, 4.432f, dt, pc), 4);
                EXPECT_FALSE(payout.Reeled());
            }
            EXPECT_GT(payout.Step(6, 4.432f, dt, pc), 4);
            EXPECT_FLOAT_EQ(payout.Slow(), 0);
        }
        // A real stop then recalls within half a second.
        for (int frame = 0; frame < hz / 2; ++frame)
            payout.Step(0, 4.432f, dt, pc);
        EXPECT_FLOAT_EQ(payout.Target(), 1);
    }
}
TEST(WhipPayout, AHeavyWindowsReachEndsBackAtTheLights) {
    W::Payout p;
    const W::PayoutConfig c{};
    EXPECT_FLOAT_EQ(p.Step(8.0f, 6.1f, 0.02f, c), 6.1f) << "a heavy swing: the heavy reach";
    EXPECT_FLOAT_EQ(p.Step(8.0f, 4.0f, 0.02f, c), 4.0f) << "its window over: the light's";
    EXPECT_FLOAT_EQ(p.Step(8.0f, 0.5f, 0.02f, c), 1.0f) << "never below the stick";
    EXPECT_FLOAT_EQ(p.Step(std::nanf(""), 4.0f, 0.02f, c), 1.0f);
}

// A hand velocity going round a circle at `rev_s` revolutions per second.
std::array<float, 3> CircleVelocity(float t, float rev_s, float r, B::Vec3 axis_hint = {}) {
    const float w = 2.0f * 3.14159265f * rev_s;
    const float a = w * t;
    if (axis_hint.x != 0.0f) { // a vertical circle (an underarm / overarm loop)
        return {0.0f, -r * w * std::sin(a), r * w * std::cos(a)};
    }
    return {-r * w * std::sin(a), 0.0f, r * w * std::cos(a)};
}

TEST(WhipSpin, CountsEachRevolution) {
    W::SpinCounter s;
    const W::SpinConfig c{};
    const float dt = 1.0f / 90.0f;
    int revs = 0;
    float first = -1.0f;
    for (float t = 0.0f; t < 2.0f; t += dt) {
        const auto v = CircleVelocity(t, 1.75f, 0.25f);
        if (s.Step(v[0], v[1], v[2], dt, c)) {
            ++revs;
            if (first < 0.0f) {
                first = t;
            }
        }
    }
    EXPECT_EQ(revs, 3) << "1.75 rev/s for 2 s";
    EXPECT_NEAR(first, 1.0f / 1.75f, 0.03f);
    // The other way round, and in a vertical plane: the same.
    W::SpinCounter v;
    revs = 0;
    for (float t = 0.0f; t < 2.2f; t += dt) { // 3.3 revolutions
        const auto u = CircleVelocity(-t, 1.5f, 0.3f, {1.0f, 0.0f, 0.0f});
        revs += v.Step(-u[0], -u[1], -u[2], dt, c) ? 1 : 0;
    }
    EXPECT_EQ(revs, 3);
}

TEST(WhipSpin, AFigureEightOrAWaggleIsNoRevolution) {
    const W::SpinConfig c{};
    const float dt = 1.0f / 90.0f;
    // A figure eight: one loop each way.
    W::SpinCounter s;
    int revs = 0;
    for (float t = 0.0f; t < 3.0f; t += dt) {
        const float a = 2.0f * 3.14159265f * 0.7f * t;
        // x = sin(a), z = sin(2a) / 2: the velocity turns one way round one lobe, back round the
        // other.
        const float w = 2.0f * 3.14159265f * 0.7f;
        revs += s.Step(0.5f * w * std::cos(a), 0.0f, 0.5f * w * std::cos(2.0f * a), dt, c) ? 1 : 0;
    }
    EXPECT_EQ(revs, 0);
    // Forehand, backhand, forehand ... (the hand stops between them).
    W::SpinCounter b;
    revs = 0;
    for (float t = 0.0f; t < 3.0f; t += dt) {
        const float v = 3.0f * std::sin(2.0f * 3.14159265f * 1.2f * t);
        revs += b.Step(v, 0.1f * std::abs(v), 0.0f, dt, c) ? 1 : 0;
    }
    EXPECT_EQ(revs, 0);
    // A slow circle (the hand under min_speed): nothing.
    W::SpinCounter slow;
    revs = 0;
    for (float t = 0.0f; t < 3.0f; t += dt) {
        const auto v = CircleVelocity(t, 0.5f, 0.3f);
        revs += slow.Step(v[0], v[1], v[2], dt, c) ? 1 : 0;
    }
    EXPECT_EQ(revs, 0);
}

TEST(WhipSpin, ARestartWaitsAWholeRevolution) {
    W::SpinCounter s;
    const W::SpinConfig c{};
    const float dt = 1.0f / 90.0f;
    float t = 0.0f;
    for (; t < 0.4f; t += dt) {
        const auto v = CircleVelocity(t, 1.75f, 0.25f);
        ASSERT_FALSE(s.Step(v[0], v[1], v[2], dt, c));
    }
    s.Restart(); // a stroke's own window opened here
    float next = -1.0f;
    for (; t < 2.0f && next < 0.0f; t += dt) {
        const auto v = CircleVelocity(t, 1.75f, 0.25f);
        if (s.Step(v[0], v[1], v[2], dt, c)) {
            next = t;
        }
    }
    EXPECT_NEAR(next, 0.4f + 1.0f / 1.75f, 0.03f);
}

TEST(WhipSpin, TheChainsTipSpeedScalesItsHits) {
    EXPECT_NEAR(W::BladeEquivalentTip(8.0f), 3.5f, 1e-4f) << "a slow lash: the floor";
    EXPECT_NEAR(W::BladeEquivalentTip(30.0f), 9.0f, 1e-4f) << "a full lash: full speed";
    EXPECT_GT(W::BladeEquivalentTip(44.0f), 9.0f);
    EXPECT_FLOAT_EQ(W::BladeEquivalentTip(0.0f), 1.5f);
    EXPECT_FLOAT_EQ(W::BladeEquivalentTip(-50.0f), 0.0f);
    EXPECT_GT(W::HurtTipSpeed, 3.0f);
}

TEST(WhipSpin, EachRevolutionIsASwingPaidFromTheBudget) {
    // 2 rev/s for 10 s, nothing but the spin (no stroke of the classifier): each revolution opens a
    // committed light window; two charges, each back after the whip's light recovery - 20 ms.
    namespace M = VR::Melee;
    M::WindowTracker tracker;
    M::TrackerConfig tc{};
    W::SpinCounter spin;
    const W::SpinConfig sc{};
    const float recover = W::WhipRecover(2400, 0);
    ASSERT_GT(recover, 1.0f);
    const float dt = 1.0f / 90.0f;
    int paid = 0, unpaid = 0, telegraphs = 0;
    std::uint32_t last = 0;
    M::TelegraphTracker tg;
    const M::TelegraphConfig gc{};
    for (float t = 0.0f; t < 10.0f; t += dt) {
        const auto v = CircleVelocity(t, 2.0f, 0.25f);
        M::TrackerInput in{};
        in.dt = dt;
        in.swings = true;
        in.hand_v = {v[0], v[1], v[2]};
        in.hand_speed = std::hypot(v[0], v[2]);
        in.tip_speed = 12.0f;
        in.recover = {recover, W::WhipRecover(2400, 1), W::WhipRecover(2400, 2)};
        M::TrackerOutput out = tracker.Step(in, tc);
        bool rev = spin.Step(v[0], v[1], v[2], dt, sc);
        if (out.opened) {
            spin.Restart();
        } else if (rev) {
            out = tracker.OpenRevolution(in, tc);
            EXPECT_TRUE(out.opened);
        }
        const M::Window& w = tracker.Current();
        if (out.opened) {
            EXPECT_TRUE(w.committed);
            EXPECT_EQ(w.kind, M::MoveKind::Light);
            EXPECT_NE(w.serial, last);
            last = w.serial;
            (w.unpaid ? unpaid : paid) += 1;
        }
        M::TelegraphInput gi{};
        gi.dt = dt;
        gi.swings = true;
        gi.window = &w;
        gi.opened = out.opened;
        gi.spin = out.opened && rev;
        telegraphs += tg.Step(gi, gc).fire ? 1 : 0;
    }
    EXPECT_EQ(paid + unpaid, 19) << "a window per revolution (the first after one revolution)";
    const float refill = M::BudgetRefill(recover, tc.budget);
    EXPECT_LE(paid, 2 + static_cast<int>(10.0f / refill) + 1) << "the whip's own rate";
    EXPECT_GE(paid, static_cast<int>(10.0f / refill));
    EXPECT_GT(unpaid, 5) << "the rest of the revolutions do not hurt";
    EXPECT_GE(telegraphs, 5) << "a spin tells the enemies (at most every 0.8 s)";
    std::printf("  spin budget: %d revolutions paid, %d not (refill %.3f s), %d telegraphs\n",
                paid, unpaid, refill, telegraphs);
}

// An overhead spin (the hand on a 25 cm circle above the head, the blade out along the radius):
// spun up over 0.4 s to `rev_s`, held, stopped over 0.2 s at `stop`, then still.
struct Spin {
    float rev_s{1.75f};
    float r{0.25f};
    float up{0.4f};
    float stop{2.6f};
    float down{0.2f};
    B::Vec3 c{0.1f, 1.95f, -0.1f};
    float Phase(float t) const {
        const float w = 2.0f * 3.14159265f * rev_s;
        if (t <= 0.0f) {
            return 0.0f;
        }
        if (t < up) {
            return 0.5f * w * t * t / up;
        }
        const float a_up = 0.5f * w * up;
        if (t < stop) {
            return a_up + w * (t - up);
        }
        const float a_run = a_up + w * (stop - up);
        const float u = std::min(t - stop, down);
        return a_run + w * (u - 0.5f * u * u / down);
    }
    W::Frame At(float t) const {
        const float a = Phase(t);
        const B::Vec3 radial{std::cos(a), 0.0f, std::sin(a)};
        // The blade (the part's -Y) out along the radius, 10 degrees up.
        const B::Vec3 blade = B::Normalize(radial + B::Vec3{0.0f, 0.18f, 0.0f});
        const B::Vec3 side = B::Normalize(B::Cross({0.0f, 1.0f, 0.0f}, radial));
        const B::Vec3 y = blade * -1.0f;
        const B::Vec3 z = B::Normalize(B::Cross(side, y));
        const B::Vec3 x = B::Cross(y, z);
        return {c + radial * r, B::FromBasis(x, y, z)};
    }
};

TEST(WhipChain, AnOverheadSpinKeepsItOutCirclingThenItReelsIn) {
    const auto rest = IdleChain();
    W::Chain c;
    ASSERT_TRUE(c.Init(rest.data(), N));
    const Spin spin{};
    W::Payout payout;
    const W::PayoutConfig pc{};
    const W::ChainConfig cfg{};
    const float cap = W::ExtensionForReach(3.70f, 1.17f, 0.84f);
    const float dt = 1.0f / 30.0f; // the game's frames
    W::Frame prev = spin.At(0.0f);
    c.Reset(prev);
    float out_min = 100.0f, radius_min = 100.0f, height_max = 0.0f, bend = 0.0f;
    float tip_turn = 0.0f; // the tip's angle round the spin's centre, summed
    float prev_angle = 0.0f;
    bool have_angle = false;
    float reeled_at = -1.0f;
    float tip_speed_min = 1000.0f;
    float hang = 100.0f;
    for (float t = dt; t < 4.0f; t += dt) {
        const W::Frame f = spin.At(t);
        // The presenter's tip speed: the blade's far end 0.8 m out.
        const B::Vec3 tip_now = W::Apply(f, {0.0f, -0.8f, 0.0f});
        const B::Vec3 tip_before = W::Apply(prev, {0.0f, -0.8f, 0.0f});
        const float speed = B::Length(tip_now - tip_before) / dt;
        const float s = payout.Step(speed, cap, dt, pc);
        W::ChainConfig now_cfg = cfg;
        now_cfg.release = W::ReleaseForSpeed(speed, pc.v_keep, cfg); // as the hook does
        now_cfg.side_damp = W::SideDampForSpeed(speed, pc.v_keep, cfg);
        ASSERT_GT(c.Step(prev, f, dt, s, now_cfg), 0);
        prev = f;
        const B::Vec3 tip = c.Point(N - 1);
        if (t > spin.stop + spin.down + 0.45f && t < spin.stop + spin.down + 0.55f) {
            hang = std::min(hang, tip.y - f.t.y); // held still, still out: it hangs
        }
        const B::Vec3 d = tip - spin.c;
        bend = std::max(bend, c.MaxBend());
        if (t > 1.0f && t < spin.stop) {
            // Spinning: out, circling level round the head like a helicopter blade.
            out_min = std::min(out_min, c.Extension());
            radius_min = std::min(radius_min, std::hypot(d.x, d.z));
            height_max = std::max(height_max, std::abs(d.y));
            tip_speed_min = std::min(tip_speed_min, c.TipSpeed());
            const float angle = std::atan2(d.z, d.x);
            if (have_angle) {
                float da = angle - prev_angle;
                da -= 2.0f * 3.14159265f * std::round(da / (2.0f * 3.14159265f));
                tip_turn += da;
            }
            prev_angle = angle;
            have_angle = true;
        }
        if (reeled_at < 0.0f && t > spin.stop && c.Extension() <= 1.001f) {
            reeled_at = t;
        }
    }
    const float hand_turn = spin.Phase(spin.stop) - spin.Phase(1.0f);
    EXPECT_GT(out_min, cap - 0.05f) << "out all through the spin";
    EXPECT_GT(radius_min, 2.6f) << "the chain stands out from the head";
    EXPECT_LT(height_max, 1.5f) << "level, not hanging";
    EXPECT_NEAR(tip_turn, hand_turn, 1.6f) << "the tip circles with the hand (trailing)";
    EXPECT_GT(tip_speed_min, W::HurtTipSpeed * 2.0f) << "fast all round: it hurts all round";
    EXPECT_LE(bend, cfg.max_bend + 0.01f);
    EXPECT_LT(hang, -0.5f) << "stopped, the chain falls (no 3.5 m pole) before the reel-in";
    EXPECT_GT(reeled_at, spin.stop + spin.down + pc.hold_s - 0.1f) << "out for the hold";
    EXPECT_LT(reeled_at, spin.stop + spin.down + pc.hold_s + 0.4f) << "then in";
    EXPECT_NEAR(c.Extension(), 1.0f, 1e-3f);
    std::printf("  spin: out >= x%.2f, tip radius >= %.2f m, |height| <= %.2f m, tip turned %.2f "
                "of the hand's %.2f rad, tip >= %.1f m/s, bend %.0f deg, reeled in %.2f s after "
                "the stop\n",
                out_min, radius_min, height_max, tip_turn, hand_turn, tip_speed_min,
                bend / B::Deg, reeled_at - spin.stop);
}


TEST(WhipRest, TheCanesBackwardIdleLineIsTurnedToTheBlade) {
    // The cane's idle clip holds its chain back along the forearm (+Y of the part); the Beast
    // Cutter's, like every blade, points -Y out of the hand and stays as it is (lines of our own
    // with those directions).
    for (const std::int32_t v : {2200, 2400}) {
        const W::WhipRest line = v == 2200 ? SyntheticCane() : SyntheticBeastCutter();
        const W::WhipRest* r = &line;
        std::array<B::Vec3, W::MaxPoints> p{};
        std::array<B::Quat, W::MaxPoints> q{};
        for (int k = 0; k < r->n; ++k) {
            p[k] = {r->t[k][0], r->t[k][1], r->t[k][2]};
            q[k] = B::Normalize(B::Quat{r->q[k][0], r->q[k][1], r->q[k][2], r->q[k][3]});
        }
        const auto before = p;
        const auto before_q = q;
        const bool turned = W::RestToBlade(p.data(), q.data(), r->n);
        EXPECT_EQ(turned, v == 2200) << v;
        EXPECT_LT(p[r->n - 1].y - p[0].y, 0.0f) << v << ": out of the front of the hand";
        for (int k = 1; k < r->n; ++k) {
            EXPECT_NEAR(B::Length(p[k] - p[k - 1]), B::Length(before[k] - before[k - 1]), 1e-5f);
            // Each bone's +X still runs along its link (the rotations turned with the points).
            if (k + 1 < r->n) {
                const B::Vec3 x0 = B::Rotate(before_q[k], {1.0f, 0.0f, 0.0f});
                const B::Vec3 x1 = B::Rotate(q[k], {1.0f, 0.0f, 0.0f});
                const B::Vec3 l0 = B::Normalize(before[k + 1] - before[k], x0);
                const B::Vec3 l1 = B::Normalize(p[k + 1] - p[k], x1);
                EXPECT_NEAR(B::Dot(x0, l0), B::Dot(x1, l1), 1e-3f);
            }
        }
        EXPECT_NEAR(B::Length(p[0] - before[0]), 0.0f, 1e-6f) << "the root stays on the handle";
    }
}

TEST(WhipChain, TheCaneSpinsOutLikeTheBeastCutter) {
    const W::WhipRest cane_rest = SyntheticCane();
    const W::WhipRest* r = &cane_rest;
    std::array<B::Vec3, W::MaxPoints> rest{};
    std::array<B::Quat, W::MaxPoints> q{};
    for (int k = 0; k < r->n; ++k) {
        rest[k] = {r->t[k][0], r->t[k][1], r->t[k][2]};
        q[k] = B::Quat{r->q[k][0], r->q[k][1], r->q[k][2], r->q[k][3]};
    }
    ASSERT_TRUE(W::RestToBlade(rest.data(), q.data(), r->n));
    W::Chain c;
    ASSERT_TRUE(c.Init(rest.data(), r->n));
    const W::WhipMoves* cane = W::FindWhip(2200);
    const float cap = W::PayoutFor(cane->reach, 0, 1.0f, cane->idle_tip, r->links);
    const Spin spin{};
    W::Payout payout;
    const W::PayoutConfig pc{};
    const W::ChainConfig cfg{};
    const float dt = 1.0f / 30.0f;
    W::Frame prev = spin.At(0.0f);
    c.Reset(prev);
    float radius_min = 100.0f, height_max = 0.0f;
    for (float t = dt; t < spin.stop; t += dt) {
        const W::Frame f = spin.At(t);
        const float speed = B::Length(W::Apply(f, {0.0f, -0.8f, 0.0f}) -
                                      W::Apply(prev, {0.0f, -0.8f, 0.0f})) / dt;
        W::ChainConfig now_cfg = cfg;
        now_cfg.release = W::ReleaseForSpeed(speed, pc.v_keep, cfg);
        now_cfg.side_damp = W::SideDampForSpeed(speed, pc.v_keep, cfg);
        ASSERT_GT(c.Step(prev, f, dt, payout.Step(speed, cap, dt, pc), now_cfg), 0);
        prev = f;
        if (t > 1.0f) {
            const B::Vec3 d = c.Point(r->n - 1) - spin.c;
            radius_min = std::min(radius_min, std::hypot(d.x, d.z));
            height_max = std::max(height_max, std::abs(d.y));
        }
    }
    EXPECT_GT(radius_min, cane->reach[0] - 0.6f) << "out, circling";
    EXPECT_LT(height_max, 1.0f) << "level";
}

// A 30 degree snap turn: the hook carries the chain round with the hunter (Chain::Carry, its last
// frame's hand with it), so the idle stick stays on the hand; left alone it was 0.68 m behind.
TEST(WhipChain, ASnapTurnCarriesTheChainRound) {
    const auto rest = IdleChain();
    const B::Vec3 pivot{0.0f, 0.0f, 0.0f};
    const auto at = [&](float yaw) {
        const B::Quat body = B::AxisAngle({0.0f, 1.0f, 0.0f}, yaw);
        return W::Frame{pivot + B::Rotate(body, B::Vec3{0.25f, 1.2f, -0.45f}),
                        B::Normalize(body * B::AxisAngle({1.0f, 0.0f, 0.0f}, 60.0f * B::Deg))};
    };
    for (const bool carry : {false, true}) {
        W::Chain c;
        ASSERT_TRUE(c.Init(rest.data(), N));
        const W::ChainConfig cfg{};
        W::Frame f = at(0.0f);
        c.Reset(f);
        for (int i = 0; i < 30; ++i) {
            c.Step(f, f, 1.0f / 60.0f, 1.0f, cfg);
        }
        const float turn = 30.0f * B::Deg;
        const W::Frame g = at(turn);
        W::Frame prev = f;
        if (carry) {
            const B::Quat rq = B::AxisAngle({0.0f, 1.0f, 0.0f}, turn);
            c.Carry(rq, pivot, pivot);
            prev = {pivot + B::Rotate(rq, f.t - pivot), B::Normalize(rq * f.q)};
        }
        ASSERT_GT(c.Step(prev, g, 1.0f / 60.0f, 1.0f, cfg), 0);
        float worst = 0.0f;
        for (int k = 0; k < N; ++k) {
            worst = std::max(worst, B::Length(c.Point(k) - W::Apply(g, rest[k])));
        }
        if (carry) {
            EXPECT_LT(worst, 0.06f) << "carried round: on the hand";
            EXPECT_LT(c.TipSpeed(), 1.0f) << "no spring back";
        } else {
            EXPECT_GT(worst, 0.3f) << "left alone it lags behind";
        }
    }
}

// A moderate motion pays the chain out a little (x1.03-1.2): the stick stays a stick. The release
// once followed the share of the payout's own peak, so x1.05 released it all and the stick drooped
// 0.9 m to the floor for the hold, then snapped back up at 40 m/s (the verification run wv_bc2).
TEST(WhipChain, ASmallPayoutKeepsTheStickNoDroop) {
    const auto rest = IdleChain();
    const W::Frame f{{0.2f, 1.25f, -0.4f},
                     B::Normalize(Stroke::Grip(0.0f, 45.0f) *
                                  B::AxisAngle({1.0f, 0.0f, 0.0f}, 90.0f * B::Deg))};
    for (const float s : {1.05f, 1.2f}) {
        W::Chain c;
        ASSERT_TRUE(c.Init(rest.data(), N));
        c.Reset(f);
        W::ChainConfig cfg{};
        cfg.release = cfg.release_slow; // slow: as limp as it gets
        for (int i = 0; i < 36; ++i) { // the 0.6 s hold at 60 Hz
            ASSERT_GT(c.Step(f, f, 1.0f / 60.0f, s, cfg), 0);
        }
        float worst = 0.0f;
        const B::Vec3 root = W::Apply(f, rest[0]);
        for (int k = 0; k < N; ++k) {
            const B::Vec3 line = root + B::Rotate(f.q, rest[k] - rest[0]) * s;
            worst = std::max(worst, B::Length(c.Point(k) - line));
        }
        EXPECT_LT(worst, 0.12f) << "x" << s << ": no droop";
    }
}

// A forehand that stops (the mock's rswing_fore, the payout from the presenter's swing speed as the
// hook has it): the tip sweeps across the front and whips past the hand's end to the side, then
// settles; it does not carry on round the hunter. Without the sideways drag (the verification run
// wv_bc1) it went on 177 degrees past the front, behind him.
TEST(WhipChain, AStoppedStrokeSettlesBesideTheHunterNotBehindHim) {
    const auto rest = IdleChain();
    const Stroke s(-75.0f, 5.0f, 45.0f, -10.0f, 6.0f);
    const float cap = W::ExtensionForReach(3.70f, 1.17f, 0.84f);
    for (const bool side : {false, true}) {
        W::Chain c;
        ASSERT_TRUE(c.Init(rest.data(), N));
        c.Reset(s.At(0.0f));
        W::Payout payout;
        const W::PayoutConfig pc{};
        W::ChainConfig cfg{};
        if (!side) {
            cfg.side_damp_slow = 0.0f;
        }
        const float dt = 1.0f / 60.0f;
        W::Frame prev = s.At(0.0f);
        float front_reach = 0.0f, past = 0.0f;
        for (float t = dt; t < s.t0 + s.T + 1.0f; t += dt) {
            const W::Frame f = s.At(t);
            const float speed = B::Length(W::Apply(f, {0.0f, -0.8f, 0.0f}) -
                                          W::Apply(prev, {0.0f, -0.8f, 0.0f})) / dt;
            const float target = payout.Step(speed, cap, dt, pc);
            W::ChainConfig now_cfg = cfg;
            now_cfg.release = W::ReleaseForSpeed(speed, pc.v_keep, cfg);
            now_cfg.side_damp = W::SideDampForSpeed(speed, pc.v_keep, cfg);
            ASSERT_GT(c.Step(prev, f, dt, target, now_cfg), 0);
            prev = f;
            const B::Vec3 tip = c.Point(N - 1);
            front_reach = std::max(front_reach, -tip.z);
            if (t > s.t0 + s.T && c.Extension() > 2.0f) {
                // Degrees past the front toward the stroke's end side (the left: -x).
                past = std::max(past, std::atan2(-tip.x, -tip.z) / B::Deg);
            }
        }
        if (!side) {
            EXPECT_GT(past, 120.0f) << "without the drag it carries on round, behind him";
            continue;
        }
        EXPECT_GT(front_reach, 3.0f) << "the lash sweeps across the front";
        EXPECT_GT(past, 30.0f) << "the tip whips past the hand's end";
        EXPECT_LT(past, 100.0f) << "and settles beside the hunter, not behind him";
        std::printf("  a stopped forehand: the tip %.0f deg past the front at most (in front %.2f m)\n",
                    past, front_reach);
    }
}

// Out and slow (the hold before the reel-in), the 3.4 m chain from a hand 1.2 m up hangs: without
// the ground it went 2 m through the floor (the verification run wv_bc1); with the ground at the
// hunter's feet it lies on it, keeping (most of) its length.
TEST(WhipChain, AChainOutAndSlowLiesOnTheGroundNotThroughIt) {
    const auto rest = IdleChain();
    const W::Frame f{{0.0f, 1.2f, 0.0f}, B::AxisAngle({1.0f, 0.0f, 0.0f}, 90.0f * B::Deg)};
    const float s = W::ExtensionForReach(3.70f, 1.17f, 0.84f);
    for (const bool ground : {false, true}) {
        W::Chain c;
        ASSERT_TRUE(c.Init(rest.data(), N));
        c.Reset(f);
        W::ChainConfig cfg{};
        cfg.release = cfg.release_slow; // out and slow: it hangs
        if (ground) {
            cfg.floor_y = 0.03f;
        }
        float lowest = 1e9f;
        for (int i = 0; i < 240; ++i) { // 2 s at 120 Hz
            ASSERT_GT(c.Step(f, f, 1.0f / 120.0f, s, cfg), 0);
            for (int k = 0; k < N; ++k) {
                lowest = std::min(lowest, c.Point(k).y);
            }
        }
        if (!ground) {
            EXPECT_LT(lowest, -1.0f) << "without the ground it hangs through the floor";
            continue;
        }
        EXPECT_GE(lowest, 0.03f - 1e-4f) << "never below the ground";
        EXPECT_LT(c.Point(N - 1).y, 0.03f + 0.05f) << "the tip lies on the ground";
        EXPECT_GT(c.Length(), 0.9f * 0.84f * s) << "the chain keeps its length";
        EXPECT_LT(c.MaxBend(), 1.31f) << "the joint limit holds";
    }
}

// A crack: the chain lashed out by the wrist alone (no window from the swing gate) gets a window of
// its own, once a surge; a lash a committed window covers, its follow-through, a reel-in, a hand
// at rest or the weapon brought back never crack.
TEST(WhipCrack, TheWristAloneCracksOnceASurge) {
    const float dt = 1.0f / 90.0f;
    const W::CrackConfig cc{};
    W::CrackInput base{};
    base.dt = dt;
    base.swings = true;
    base.lash = 3;
    base.asked_out = true;
    base.out = 1.0f;
    base.tip = 24.0f;
    base.swing_speed = 7.6f;
    base.v_drive = 5.5f;
    {
        // The wrist crack (T6a in the mock: 7.6 m/s, the tip 24.6 m/s, no window): cracks at once,
        // then not again while that surge lasts.
        W::CrackGate g;
        EXPECT_TRUE(g.Step(base, cc));
        int more = 0;
        for (int i = 0; i < 60; ++i) {
            more += g.Step(base, cc) ? 1 : 0;
        }
        EXPECT_EQ(more, 0) << "one window a surge";
        // The tip slow for 0.15 s: the next surge of the same lash cracks again.
        W::CrackInput slow = base;
        slow.tip = 3.0f;
        for (int i = 0; i < 15; ++i) {
            EXPECT_FALSE(g.Step(slow, cc));
        }
        EXPECT_TRUE(g.Step(base, cc));
        EXPECT_EQ(g.Cracks(), 2u);
    }
    {
        // A stroke's committed window covers its lash: its follow-through after the close (the
        // tip still fast, the hand still driving) is no crack, however long after the gap.
        W::CrackGate g;
        W::CrackInput open = base;
        open.window_open = true;
        open.window_committed = true;
        for (int i = 0; i < 20; ++i) {
            EXPECT_FALSE(g.Step(open, cc));
        }
        W::CrackInput after = base;
        int cracks = 0;
        for (int i = 0; i < 60; ++i) {
            after.since_close_s = i * dt;
            cracks += g.Step(after, cc) ? 1 : 0;
        }
        EXPECT_EQ(cracks, 0);
        // A new lash (paid out again after a reel-in) is a new surge.
        after.lash = 4;
        EXPECT_TRUE(g.Step(after, cc));
    }
    {
        // An onset window the gate threw away (uncommitted) covers nothing: the lash cracks once
        // the gap after its close has passed.
        W::CrackGate g;
        W::CrackInput onset = base;
        onset.window_open = true;
        EXPECT_FALSE(g.Step(onset, cc));
        W::CrackInput after = base;
        after.since_close_s = 0.1f;
        EXPECT_FALSE(g.Step(after, cc)) << "within the gap";
        after.since_close_s = 0.3f;
        EXPECT_TRUE(g.Step(after, cc));
    }
    {
        // None of these crack.
        auto never = [&](W::CrackInput in, const char* what) {
            W::CrackGate g;
            int cracks = 0;
            for (int i = 0; i < 90; ++i) {
                cracks += g.Step(in, cc) ? 1 : 0;
            }
            EXPECT_EQ(cracks, 0) << what;
        };
        W::CrackInput in = base;
        in.asked_out = false;
        never(in, "the reel-in (the chain pulled back fast)");
        in = base;
        in.swing_speed = 1.0f;
        never(in, "the hand at rest (the chain falling or swinging on)");
        in = base;
        in.out = 0.2f;
        never(in, "a small payout (a moderate motion)");
        in = base;
        in.tip = 4.0f;
        never(in, "a slow tip");
        in = base;
        in.recovery = true;
        never(in, "the weapon brought back");
        in = base;
        in.swings = false;
        never(in, "swings off");
        in = base;
        in.lash = 0;
        never(in, "no lash yet");
        in = base;
        in.swing_speed = 3.3f;
        in.tip = 6.1f;
        never(in, "a drag (the stroke's return in the mock: the swing 3.3, the tip 6.1 m/s)");
        in = base;
        in.tip = 9.0f;
        never(in, "a tip that hurts but does not crack");
    }
    {
        // The drive must be recent: the tip fast 0.1 s after a flick cracks, 0.4 s after (the
        // chain falling on after the hand stopped) does not.
        for (const float after : {0.1f, 0.4f}) {
            W::CrackGate g;
            W::CrackInput drive = base;
            drive.tip = 3.0f;
            for (int i = 0; i < 5; ++i) {
                EXPECT_FALSE(g.Step(drive, cc));
            }
            W::CrackInput still = base;
            still.swing_speed = 1.0f;
            still.tip = 3.0f;
            for (float t = 0.0f; t < after; t += dt) {
                EXPECT_FALSE(g.Step(still, cc));
            }
            still.tip = 20.0f;
            const bool cracks = g.Step(still, cc);
            EXPECT_EQ(cracks, (after < 0.25f)) << after << " s after the drive";
        }
    }
}

TEST(WhipCrack, ACrackIsASwingPaidFromTheBudget) {
    // Wrist cracks every 0.5 s for 6 s, the swing gate counting none: each opens a committed light
    // window ("a whip crack"), paid at the whip's own rate.
    namespace M = VR::Melee;
    M::WindowTracker tracker;
    M::TrackerConfig tc{};
    W::CrackGate gate;
    const W::CrackConfig cc{};
    const float recover = W::WhipRecover(2400, 0);
    const float dt = 1.0f / 90.0f;
    int paid = 0, unpaid = 0;
    float last_close = -10.0f;
    for (float t = 0.0f; t < 6.0f; t += dt) {
        const float phase = std::fmod(t, 0.5f);
        const bool surge = phase < 0.2f; // the tip fast for 0.2 s of every 0.5 s
        M::TrackerInput in{};
        in.dt = dt;
        in.swings = true;
        in.tip_speed = surge ? 7.0f : 1.0f;
        in.blade_rate = surge ? 9.0f : 0.5f;
        in.recover = {recover, W::WhipRecover(2400, 1), W::WhipRecover(2400, 2)};
        M::TrackerOutput out = tracker.Step(in, tc);
        if (out.closed) {
            last_close = t;
        }
        const M::Window& w = tracker.Current();
        W::CrackInput ci{};
        ci.dt = dt;
        ci.swings = true;
        ci.window_open = w.open;
        ci.window_committed = w.committed;
        ci.since_close_s = t - last_close;
        ci.lash = 1;
        ci.asked_out = true;
        ci.out = 1.0f;
        ci.tip = surge ? 20.0f : 2.0f;
        ci.swing_speed = in.tip_speed;
        ci.v_drive = 5.5f;
        if (gate.Step(ci, cc)) {
            out = tracker.OpenRevolution(in, tc, "a whip crack");
            ASSERT_TRUE(out.opened);
            const M::Window& cw = tracker.Current();
            EXPECT_TRUE(cw.committed);
            EXPECT_EQ(cw.kind, M::MoveKind::Light);
            EXPECT_STREQ(cw.why, "a whip crack");
            (cw.unpaid ? unpaid : paid) += 1;
        }
    }
    EXPECT_EQ(paid + unpaid, 12) << "one window a crack";
    const float refill = M::BudgetRefill(recover, tc.budget);
    EXPECT_LE(paid, 2 + static_cast<int>(6.0f / refill) + 1) << "the whip's own rate";
    EXPECT_GE(unpaid, 4);
    std::printf("  cracks: %d paid, %d not (refill %.3f s)\n", paid, unpaid, refill);
}

} // namespace

namespace {
TEST(WhipGaze, GuidesOnlyNearbyOutgoingMotionWithoutAddingSpeed) {
    const W::GazeAssist gaze{true, {}, {0.0f, 0.0f, -1.0f}};
    const B::Vec3 tip{0.1f, 0.0f, -2.0f};
    const auto near = B::RotateAbout(gaze.forward, {0, 1, 0}, 10.0f * B::Deg) * 12.0f;
    const auto out = W::AssistLash(near, tip, {}, gaze, 1.0f / 30.0f);
    EXPECT_LT(B::Angle(out, gaze.forward), B::Angle(near, gaze.forward));
    EXPECT_NEAR(B::Length(out), B::Length(near), 1e-5f);
    EXPECT_LE(B::Angle(out, near), 45.0f * B::Deg / 30.0f + 1e-4f);
    const auto far = B::RotateAbout(gaze.forward, {0, 1, 0}, 35.0f * B::Deg) * 12.0f;
    EXPECT_LT(B::Length(W::AssistLash(far, tip, {}, gaze, 0.033f) - far), 1e-6f);
    EXPECT_LT(B::Length(W::AssistLash(-near, tip, {}, gaze, 0.033f) + near), 1e-6f);
    EXPECT_LT(B::Length(W::AssistLash(near, {2, 0, 0}, {}, gaze, 0.033f) - near), 1e-6f);
    EXPECT_LT(B::Length(W::AssistLash(near, tip, {}, {}, 0.033f) - near), 1e-6f);
}
TEST(WhipGaze, LeavesStoppedSlowAndDegenerateMotionAlone) {
    const W::GazeAssist gaze{true, {}, {0, 0, -1}};
    for (const B::Vec3 v : {B::Vec3{}, B::Vec3{0.1f, 0, -1.0f}}) {
        EXPECT_FLOAT_EQ(B::Length(W::AssistLash(v, {0, 0, -2}, {}, gaze, .033f) - v), 0);
    }
    const B::Vec3 v{1, 0, -10};
    EXPECT_FLOAT_EQ(B::Length(W::AssistLash(v, {0, 0, -2}, {}, {true, {}, {}}, .033f) - v), 0);
}

TEST(BeastCutter, ExtendedChainFallsEvenWhenTheHandlePointsUp) {
    const auto rest = IdleChain();
    for (const float hz : {30.0f, 60.0f, 90.0f}) {
        W::Chain chain;
        ASSERT_TRUE(chain.Init(rest.data(), N));
        W::Frame f{{0, 2, 0}, B::AxisAngle({0, 0, 1}, 150 * B::Deg)};
        std::array<B::Vec3, N> extended{};
        for (int i = 0; i < N; ++i)
            extended[i] = W::Apply(f, rest[0] + (rest[i] - rest[0]) * 4.0f);
        chain.ResetTo(extended.data(), 4.0f);
        const auto cfg = W::ConfigFor(2400);
        for (int step = 0; step < int(2.5f * hz); ++step) {
            ASSERT_GT(chain.Step(f, f, 1 / hz, 4.0f, cfg), 0);
            ASSERT_LT(LinkError(chain, 4.0f, rest), 2e-3f);
            ASSERT_LE(chain.MaxBend(), cfg.max_bend + .01f);
        }
        EXPECT_LT(chain.Point(N - 1).y, chain.Point(0).y - 1.5f);
        EXPECT_LT(chain.TipSpeed(), 4.0f);
    }
}

TEST(BeastCutter, OverheadBothDirectionsStayFlexibleAndFallAfterStopping) {
    const auto rest = IdleChain();
    for (const float direction : {-1.0f, 1.0f}) {
        for (const float hz : {30.0f, 90.0f}) {
            W::Chain chain;
            ASSERT_TRUE(chain.Init(rest.data(), N));
            Spin spin;
            spin.rev_s *= direction;
            W::Frame prev = spin.At(0);
            chain.Reset(prev);
            W::Payout payout;
            W::PayoutConfig pc{};
            const auto cfg = W::ConfigFor(2400);
            const float dt = 1 / hz;
            float peak = 0, hang = 100, bend = 0;
            for (float t = dt; t < 4; t += dt) {
                const auto f = spin.At(t);
                const float speed =
                    B::Length(W::Apply(f, {0, -.8f, 0}) - W::Apply(prev, {0, -.8f, 0})) / dt;
                auto now = cfg;
                now.release = W::ReleaseForSpeed(speed, pc.v_keep, cfg);
                now.side_damp = W::SideDampForSpeed(speed, pc.v_keep, cfg);
                ASSERT_GT(chain.Step(prev, f, dt, payout.Step(speed, 4.01f, dt, pc), now), 0);
                ASSERT_LT(LinkError(chain, chain.Extension(), rest), 2e-3f);
                ASSERT_LE(chain.MaxBend(), cfg.max_bend + .01f);
                if (t > 1 && t < spin.stop) {
                    peak = std::max(peak, chain.TipSpeed());
                }
                if (t > spin.stop && t < 3.35f)
                    bend = std::max(bend, chain.MaxBend());
                if (t > 3.20f && t < 3.35f)
                    hang = std::min(hang, chain.Point(N - 1).y - f.t.y);
                prev = f;
            }
            EXPECT_GT(peak, W::HurtTipSpeed * 2);
            EXPECT_GT(bend, 5 * B::Deg);
            EXPECT_LT(hang, -.5f);
            EXPECT_NEAR(chain.Extension(), 1, .01f);
            std::printf("  cutter spin dir %.0f %.0fHz: peak %.2f, bend %.1f deg, hang %.2f\n",
                        direction, hz, peak, bend / B::Deg, hang);
        }
    }
}
} // namespace

namespace {
TEST(BeastCutter, BackwardsTugTransfersTensionBeforeAnyReelIn) {
    const auto rest = IdleChain();
    const auto cfg = W::ConfigFor(2400);
    const W::Frame start{{0, 2, 0}, B::FromTo(B::Normalize(rest[N - 1] - rest[0]), {0, 0, -1})};
    std::array<B::Vec3, N> extended{};
    for (int i = 0; i < N; ++i)
        extended[i] = W::Apply(start, rest[0] + (rest[i] - rest[0]) * 4);
    W::Chain held, pulled;
    ASSERT_TRUE(held.Init(rest.data(), N));
    ASSERT_TRUE(pulled.Init(rest.data(), N));
    held.ResetTo(extended.data(), 4);
    pulled.ResetTo(extended.data(), 4);
    W::Frame prev = start;
    for (int k = 1; k <= 12; ++k) {
        W::Frame f = start;
        f.t.z += .6f * k / 12;
        held.Step(start, start, 1 / 120.f, 4, cfg);
        pulled.Step(prev, f, 1 / 120.f, 4, cfg);
        prev = f;
        ASSERT_LT(LinkError(pulled, 4, rest), 2e-3f);
    }
    EXPECT_GT(pulled.Point(N - 1).z - held.Point(N - 1).z, .4f);
    EXPECT_GT(pulled.Velocity(N - 1).z - held.Velocity(N - 1).z, 3.f);
    EXPECT_FLOAT_EQ(pulled.Extension(), 4.f) << "return is tension, not timed shortening";
}
} // namespace

namespace {
TEST(BeastCutter, MorePayoutCannotRestartTheStraighteningMotor) {
    const auto rest = IdleChain();
    W::Chain chain;
    ASSERT_TRUE(chain.Init(rest.data(), N));
    // Handle points up, but the already extended chain lies horizontally. With no gravity or
    // hand motion there is no source of vertical momentum when its length increases.
    const W::Frame f{{0, 2, 0}, B::FromTo(rest[N-1]-rest[0], {0, 1, 0})};
    const auto root = W::Apply(f, rest[0]);
    std::array<B::Vec3, N> points{};
    for (int i = 0; i < N; ++i) points[i] = root + B::Vec3{.0336f * i * 2.5f, 0, 0};
    chain.ResetTo(points.data(), 2.5f);
    auto cfg = W::ConfigFor(2400);
    cfg.gravity = 0;
    cfg.handle_links = 0; // isolate payout from the separately tested handle attachment torque
    for (int i = 0; i < 30; ++i) {
        chain.Step(f, f, 1 / 120.0f, 4.01f, cfg);
        EXPECT_NEAR(chain.Point(N-1).y, root.y, 1e-4f);
        ASSERT_LT(LinkError(chain, chain.Extension(), rest), 2e-3f);
    }
}
}

namespace {
// Cover direction, game frame rate, light/charged reach and floor contact independently.
class BeastCutterSlice : public ::testing::TestWithParam<std::tuple<int, int, bool, bool>> {};
TEST_P(BeastCutterSlice, FollowsTheStrokeKeepsReachAndReturns) {
    const auto [kind, hz, charged, floor] = GetParam();
    const auto rest = IdleChain();
    const float cap = W::ReachExtension(2400, W::ExtensionForReach(
        charged ? 6.18f : 3.70f, 1.17f, .84f), 1.17f, .84f);
    const float dt = 1.0f / hz;
    auto cfg = W::ConfigFor(2400);
    if (floor) cfg.floor_y = .05f;
    W::Chain chain;
    ASSERT_TRUE(chain.Init(rest.data(), N));
    auto prev = W::TestMotion::Slice(0, kind);
    chain.Reset(prev);
    W::Payout payout;
    const W::PayoutConfig pc{};
    float reach = 0, peak_speed = 0;
    for (int frame = 1; frame <= 3 * hz; ++frame) {
        const auto f = W::TestMotion::Slice(frame * dt, kind);
        const float speed = B::Length(W::Apply(f, {0, -.8f, 0}) -
                                      W::Apply(prev, {0, -.8f, 0})) / dt;
        auto now = cfg;
        now.release = W::ReleaseForSpeed(speed, pc.v_keep, cfg);
        now.side_damp = W::SideDampForSpeed(speed, pc.v_keep, cfg);
        ASSERT_GT(chain.Step(prev, f, dt, payout.Step(speed, cap, dt, pc), now), 0);
        EXPECT_LT(B::Length(chain.Point(0) - W::Apply(f, rest[0])), 1e-4f);
        for (int i = 0; i < N; ++i) {
            ASSERT_TRUE(B::IsFinite(chain.Point(i)));
            ASSERT_TRUE(B::IsFinite(chain.Velocity(i)));
            if (floor) EXPECT_GE(chain.Point(i).y, cfg.floor_y - 1e-4f);
        }
        if (!floor) {
            ASSERT_LT(LinkError(chain, chain.Extension(), rest), 2e-3f);
            ASSERT_LE(chain.MaxBend(), cfg.max_bend + .01f);
        }
        EXPECT_LE(B::Length(chain.Point(N-1) - chain.Point(0)), .84f * cap + .03f);
        reach = std::max(reach, -chain.Point(N-1).z);
        if (frame * dt < 1.1f) peak_speed = std::max(peak_speed, chain.TipSpeed());
        prev = f;
    }
    EXPECT_GT(reach, 2.5f) << "each cut crosses useful space in front of the hunter";
    EXPECT_GT(peak_speed, W::HurtTipSpeed * 2);
    EXPECT_NEAR(chain.Extension(), 1.0f, .01f);
}
INSTANTIATE_TEST_SUITE_P(DirectionsAndFrameRates, BeastCutterSlice,
    ::testing::Combine(::testing::Range(0, 4), ::testing::Values(30, 60, 90),
                       ::testing::Bool(), ::testing::Bool()));
} // namespace

namespace {
TEST(BeastCutter, ChangingDirectionDuringPayoutBendsTheThrowIntoASwipe) {
    const auto rest = IdleChain();
    for (const float hz : {30.0f, 60.0f, 90.0f}) {
        std::array<W::Chain, 2> chains;
        std::array<W::Payout, 2> payouts;
        std::array<W::Frame, 2> previous;
        const W::PayoutConfig pc{};
        const auto cfg = W::ConfigFor(2400);
        const float dt = 1.0f / hz, cap = W::ReachExtension(2400, 4.01f, 1.171f, .84f);
        for (int mode = 0; mode < 2; ++mode) {
            ASSERT_TRUE(chains[mode].Init(rest.data(), N));
            previous[mode] = W::TestMotion::Throw(0);
            chains[mode].Reset(previous[mode]);
        }
        bool turned_while_extending = false;
        float side_separation = 0;
        for (int frame = 1; frame <= int(.95f * hz); ++frame) {
            const float t = frame * dt;
            for (int mode = 0; mode < 2; ++mode) {
                const auto f = W::TestMotion::Throw(t, false, mode == 1);
                const float speed = B::Length(W::Apply(f, {0, -.8f, 0}) -
                    W::Apply(previous[mode], {0, -.8f, 0})) / dt;
                auto now = cfg;
                now.release = W::ReleaseForSpeed(speed, pc.v_keep, cfg);
                now.side_damp = W::SideDampForSpeed(speed, pc.v_keep, cfg);
                const float target = payouts[mode].Step(speed, cap, dt, pc);
                if (mode == 1 && t > .43f && t < .60f &&
                    chains[mode].Extension() > 1.1f &&
                    target > chains[mode].Extension() + .05f) {
                    turned_while_extending = true;
                }
                ASSERT_GT(chains[mode].Step(previous[mode], f, dt, target, now), 0);
                ASSERT_LT(LinkError(chains[mode], chains[mode].Extension(), rest), 2e-3f);
                ASSERT_LE(chains[mode].MaxBend(), cfg.max_bend + .01f);
                previous[mode] = f;
            }
            if (t <= .43f) {
                EXPECT_LT(B::Length(chains[0].Point(N-1)-chains[1].Point(N-1)), 1e-5f);
            }
            if (t > .6f) {
                side_separation = std::max(side_separation,
                    chains[0].Point(N-1).x - chains[1].Point(N-1).x);
            }
        }
        EXPECT_TRUE(turned_while_extending);
        EXPECT_GT(side_separation, .65f) << "the already released front follows the new swipe";
        std::printf("  mid-payout swipe %.0fHz: lateral tip change %.2f m\n", hz, side_separation);
    }
}
} // namespace

namespace {
TEST(WhipInput, UsesTheWrittenPoseAndAttachmentAxes) {
    B::ChrFrame character{};
    ASSERT_TRUE(B::ChrFrameFromRows({0,0,1, 3, 0,1,0, 2, -1,0,0, 1}, character));
    const B::QsTransform bone{{.2f,.4f,-.1f}, B::AxisAngle({0,0,1}, .7f), {1,1,1}};
    const B::Vec3 offset{0,-.12f,0}, forward{0,0,1}, up{0,1,0};
    W::Frame attachment{};
    ASSERT_TRUE(W::AttachmentFrame(character, bone, offset, forward, up, attachment));
    EXPECT_LT(B::Length(attachment.t - B::ToWorld(character,
                         bone.t + B::Rotate(bone.q, offset))), 1e-6f);
    EXPECT_LT(B::Length(B::Rotate(attachment.q, forward) -
                         B::RotateToWorld(character, B::Rotate(bone.q, forward))), 1e-5f);
    // A simulation point converted to the part and then placed by THIS pose lands at itself.
    const B::Vec3 tip{1.1f,1.3f,-2.2f};
    EXPECT_LT(B::Length(W::Apply(attachment, W::ApplyInverse(attachment, tip)) - tip), 1e-5f);
    EXPECT_FALSE(W::AttachmentFrame({}, bone, offset, forward, up, attachment));
}
TEST(WhipInput, RepeatedPartEvaluationsDoNotAdvancePhysicsTwice) {
    W::PoseClock clock;
    EXPECT_FLOAT_EQ(clock.Step(1'000'000'000), 0.f);
    EXPECT_NEAR(clock.Step(1'011'111'111), 1.f/90, 1e-7f);
    EXPECT_FLOAT_EQ(clock.Step(1'011'111'111), 0.f);
    EXPECT_FLOAT_EQ(clock.Step(1'000'000'000), 0.f);
    EXPECT_NEAR(clock.Step(1'022'222'222), 1.f/90, 1e-7f);
}
class WhipHandling : public ::testing::TestWithParam<std::tuple<int,int>> {};
TEST_P(WhipHandling, GentleMotionFollowsTheHandAndSettlesWithoutWiggling) {
    const auto [variation,hz] = GetParam();
    const auto rest = IdleChain();
    auto cfg = W::ConfigFor(variation);
    W::Chain chain;
    ASSERT_TRUE(chain.Init(rest.data(),N));
    W::Frame prev{{0,1.5f,0}, B::FromTo(B::Normalize(rest[N-1]-rest[0]),{0,0,-1})};
    chain.Reset(prev);
    float lag=0, stopped_speed=0;
    for(int k=1;k<=8*hz;++k) {
        auto f=prev;
        const float t=static_cast<float>(k)/hz;
        f.t.x=.08f*std::sin(std::min(t,4.f)*B::Pi/2);
        f.q=B::AxisAngle({0,1,0}, .08f*std::sin(std::min(t,4.f)*B::Pi/2))*
            B::FromTo(B::Normalize(rest[N-1]-rest[0]),{0,0,-1});
        chain.Step(prev,f,1.f/hz,1,cfg);
        lag=std::max(lag,B::Length(chain.Point(N-1)-W::Apply(f,rest[N-1])));
        if(t>5) stopped_speed=std::max(stopped_speed,chain.TipSpeed());
        ASSERT_LT(LinkError(chain,1,rest),2e-3f);
        prev=f;
    }
    EXPECT_LT(lag,.015f);
    EXPECT_LT(stopped_speed,.01f);
}
TEST_P(WhipHandling, SmallWristCirclesKeepTheFrontCirclingInsteadOfHanging) {
    const auto [variation,hz] = GetParam();
    const auto rest=IdleChain();
    const auto pose=[&](float t) {
        const float angle=2*B::Pi*.8f*t;
        const B::Vec3 radial{std::cos(angle),0,std::sin(angle)};
        return W::Frame{B::Vec3{0,2,0}+radial*.02f,
                       B::FromTo(B::Normalize(rest[N-1]-rest[0]),radial)};
    };
    W::Chain chain;ASSERT_TRUE(chain.Init(rest.data(),N));
    auto prev=pose(0);chain.Reset(prev);
    W::Payout payout;const W::PayoutConfig pc{};
    const auto cfg=W::ConfigFor(variation);
    float min_height=100, mean_radius=0;int samples=0;
    for(int k=1;k<=5*hz;++k) {
        const auto f=pose(static_cast<float>(k)/hz);
        const float speed=B::Length(W::Apply(f,{0,-.8f,0})-W::Apply(prev,{0,-.8f,0}))*hz;
        auto now=cfg;now.release=W::ReleaseForSpeed(speed,pc.v_keep,cfg);
        now.side_damp=W::SideDampForSpeed(speed,pc.v_keep,cfg);
        chain.Step(prev,f,1.f/hz,payout.Step(speed,4.01f,1.f/hz,pc),now);
        if(k>2*hz) {const auto tip=chain.Point(N-1);min_height=std::min(min_height,tip.y-2);
            mean_radius+=std::hypot(tip.x,tip.z);++samples;}
        ASSERT_LT(LinkError(chain,chain.Extension(),rest),2e-3f);
        ASSERT_LE(chain.MaxBend(),cfg.max_bend+.01f);
        prev=f;
    }
    EXPECT_GT(mean_radius/samples,2.f);
    EXPECT_GT(min_height,-1.15f);
    std::printf("  wrist circle %d %dHz: minimum height %.2f, mean radius %.2f\n",
                variation,hz,min_height,mean_radius/samples);
}
INSTANTIATE_TEST_SUITE_P(BothWhips,WhipHandling,
    ::testing::Combine(::testing::Values(2400,2200),::testing::Values(30,60,90)));
} // namespace

namespace {
TEST(BeastCutter, InternalDampingRemovesBounceWithoutRemovingNetMomentum) {
    const auto rest = IdleChain();
    auto damped = W::ConfigFor(2400), free = damped;
    free.link_velocity_damp = 0;
    const W::Frame f{{0, 3, 0}, B::FromTo(rest[N-1]-rest[0], {0, 0, -1})};
    std::array<B::Vec3, N> points{};
    for (int i = 0; i < N; ++i) {
        points[i] = W::Apply(f, rest[0] + (rest[i]-rest[0])*4);
        points[i].x += .25f * std::sin(i*.2f);
    }
    W::Chain a, b;
    ASSERT_TRUE(a.Init(rest.data(),N)); ASSERT_TRUE(b.Init(rest.data(),N));
    a.ResetTo(points.data(),4); b.ResetTo(points.data(),4);
    // One substep: both chains undergo identical forces/constraints before the damping impulse.
    a.Step(f,f,damped.substep,4,free); b.Step(f,f,damped.substep,4,damped);
    B::Vec3 momentum_before{}, momentum_after{};
    float energy_before=0, energy_after=0;
    for(int i=damped.handle_links+1;i<N;++i) {
        const float u=static_cast<float>(i)/(N-1);
        const float mass=VR::Whip::ChainPointMass(u, damped);
        momentum_before += a.Velocity(i)*mass;
        momentum_after += b.Velocity(i)*mass;
        energy_before += B::LengthSq(a.Velocity(i))*mass;
        energy_after += B::LengthSq(b.Velocity(i))*mass;
        EXPECT_LT(B::Length(a.Point(i)-b.Point(i)),1e-6f);
    }
    EXPECT_LT(B::Length(momentum_before-momentum_after),1e-3f);
    EXPECT_GT(energy_before,1.f);
    EXPECT_LT(energy_after,energy_before);
}
TEST(BeastCutter, ChainCanBendImmediatelyBeyondTheHandle) {
    const auto rest=IdleChain();
    const auto cfg=W::ConfigFor(2400);
    const W::Frame f{{0,4,0},B::FromTo(rest[N-1]-rest[0],{0,0,-1})};
    W::Chain chain;ASSERT_TRUE(chain.Init(rest.data(),N));
    std::array<B::Vec3,N> points{};
    for(int i=0;i<N;++i)points[i]=W::Apply(f,rest[0]+(rest[i]-rest[0])*4);
    chain.ResetTo(points.data(),4);
    for(int i=0;i<60;++i)chain.Step(f,f,1.f/120,4,cfg);
    EXPECT_LT(B::Length(chain.Point(1)-points[1]),1e-4f);
    EXPECT_GT(B::Length(chain.Point(3)-points[3]),.01f);
}
} // namespace

namespace {
TEST(BeastRecall, SmallPullReturnsAndSamePullCannotRelaunch) {
    W::PullRecall recall; W::Payout payout; W::PayoutConfig c;
    payout.Step(6,4,1.f/90,c);
    for(int i=0;i<5;++i) recall.Step(-.7f,.7f,1.4f,4,1.f/90,c);
    ASSERT_TRUE(recall.Returning());
    payout.Recall(); EXPECT_TRUE(payout.Reeled()); EXPECT_FLOAT_EQ(payout.Target(),1);
    EXPECT_TRUE(recall.Step(-.8f,.8f,4,3,1.f/90,c));
    EXPECT_FALSE(recall.Step(.2f,2.5f,6,2,1.f/90,c));
    EXPECT_GT(payout.Step(6,4,1.f/90,c),3.9f);
}
TEST(BeastRecall, FirmerPullRecallsWithoutImmediatelyRelaunching) {
    for (const int hz : {30, 60, 90}) {
        for (const float speed : {0.7f, 1.8f, 2.15f}) {
            SCOPED_TRACE(::testing::Message() << hz << " Hz, " << speed << " m/s pull");
            W::PullRecall recall;
            W::Payout payout;
            const W::PayoutConfig c;
            const float dt = 1.0f / hz;
            payout.Step(6, 4.4608f, dt, c);
            // A little off-axis and with wrist rotation: a firm pull used to be rejected
            // by the 1.6 m/s hand limit or 5.5 m/s tip limit, then treated as a new throw.
            const float tip_speed = speed < 1 ? 2.0f : 6.0f;
            for (int i = 0; i < hz / 10; ++i) {
                const bool returning = recall.Step(-speed * 0.62f, speed, tip_speed, 4, dt, c);
                payout.Step(tip_speed, 4.4608f, dt, c, returning);
            }
            ASSERT_TRUE(recall.Returning());
            EXPECT_FLOAT_EQ(payout.Target(), 1);
            // The peak of that same inward flick stays latched through a modest overshoot.
            for (int i = 0; i < hz / 5; ++i) {
                const bool returning = recall.Step(-2.0f, 2.4f, 6.5f, 3, dt, c);
                EXPECT_TRUE(returning);
                EXPECT_FLOAT_EQ(payout.Step(6.5f, 4.4608f, dt, c, returning), 1);
            }
            // A fresh outward stroke can still interrupt the return without waiting.
            EXPECT_FALSE(recall.Step(1.5f, 2.0f, 6, 2, dt, c));
            EXPECT_GT(payout.Step(6, 4.4608f, dt, c, recall.Returning()), 4);
        }
    }
}
TEST(BeastRecall, SingleFrameFlicksDoNotAccumulateIntoARecall) {
    const W::PayoutConfig c;
    for (const int hz : {30, 60, 90}) {
        W::PullRecall recall;
        const float dt = 1.0f / hz;
        for (int i = 0; i < 10; ++i) {
            EXPECT_FALSE(recall.Step(-1.8f, 2.0f, 6, 4, dt, c));
            EXPECT_FALSE(recall.Step(0, 0, 0, 4, dt, c));
        }
    }
}
TEST(BeastRecall, SidewaysAndUpwardSlicesKeepTheirLength) {
    W::PullRecall recall; W::PayoutConfig c;
    for(int i=0;i<90;++i) {
        EXPECT_FALSE(recall.Step(-.2f,3,6,4,1.f/90,c)); // sideways swing
        EXPECT_FALSE(recall.Step(-3,3,6,4,1.f/90,c)); // powerful upward lift from floor
    }
    for(int i=0;i<5;++i) recall.Step(-.7f,.7f,1.4f,4,1.f/90,c);
    ASSERT_TRUE(recall.Returning());
    EXPECT_FALSE(recall.Step(-3,3,6,3,1.f/90,c)); // upward stroke can interrupt recall
}
TEST(BeastRecall, NoiseAndPoseGapsDoNotRecall) {
    W::PullRecall recall; W::PayoutConfig c;
    for(int i=0;i<100;++i) {
        EXPECT_FALSE(recall.Step(-.2f,.2f,.2f,4,1.f/90,c));
        EXPECT_FALSE(recall.Step(-3,3,3,4,.12f,c));
    }
    EXPECT_FALSE(recall.Step(-.7f,.7f,1.4f,4,1.f/90,c));
    EXPECT_FALSE(recall.Step(.7f,.7f,1.4f,4,1.f/90,c));
}
TEST(WhipReach, ExtraReachPreservesClosedLength) {
    EXPECT_FLOAT_EQ(W::ReachExtension(2400,1,1.171f,.84f),1);
    EXPECT_FLOAT_EQ(W::ReachExtension(2200,1,.892f,.84f),1);
    EXPECT_NEAR(W::ReachExtension(2400,4,1.171f,.84f),4.8491879f,1e-6f);
    EXPECT_NEAR(W::ReachExtension(2200,4,.892f,.84f),4.432f,1e-6f);
}
TEST(CaneTransform, ClipAndPhysicsUseSameBasisThroughoutBlend) {
    const B::QsTransform parent{{.1f,.2f,0},B::AxisAngle({0,1,0},.3f),{1,1,1}};
    std::array<B::Vec3,3> raw{{{0,.2f,0},{0,.4f,0},{0,.6f,0}}}, p=raw;
    std::array<B::Quat,3> q;
    q.fill(B::FromTo({1,0,0},{0,1,0}));
    std::array<B::QsTransform,3> clip{},out{};
    auto prev=parent;
    for(int i=0;i<3;++i) {
        const B::QsTransform model{raw[i],q[i],{1,1,1}};
        clip[i]=B::LocalFromModel(prev,model);prev=model;
    }
    ASSERT_TRUE(W::RestToBlade(p.data(),q.data(),3));
    const auto child1=clip[1];
    clip[0]=W::ClipRootToBlade(parent,clip[0]);
    EXPECT_LT(B::Length(clip[1].t-child1.t),1e-7f);
    for(int i=0;i<=20;++i) {
        const auto end=W::ChainLocals(parent,p.data(),q.data(),clip.data(),3,i/20.f,out.data());
        EXPECT_LT(B::Length(end.t-p[2]),1e-5f); // no 180-degree sweep or collapsed midpoint
        EXPECT_GT(std::abs(B::Dot(end.q,q[2])),.99999f);
    }
}
TEST(BeastGround, DropSettlesAndAnUpwardStrokeLiftsTheChain) {
    const auto rest=IdleChain();auto cfg=W::ConfigFor(2400);cfg.floor_y=0;
    W::Frame f{{0,1.25f,0},B::FromTo(rest[N-1]-rest[0],{0,0,-1})};
    std::array<B::Vec3,N> p{};
    for(int i=0;i<N;++i)p[i]=W::Apply(f,rest[0]+(rest[i]-rest[0])*4.36f);
    W::Chain chain;ASSERT_TRUE(chain.Init(rest.data(),N));chain.ResetTo(p.data(),4.36f);
    float settled_speed=0,settled_y=0;
    for(int i=0;i<360;++i) {
        chain.Step(f,f,1.f/90,4.36f,cfg);
        if(i>270) {settled_speed=std::max(settled_speed,chain.TipSpeed());
            settled_y=std::max(settled_y,chain.Point(N-1).y);}
        for(int k=2;k<N;++k)ASSERT_GE(chain.Point(k).y,-1e-5f);
    }
    EXPECT_LT(settled_y,.02f);EXPECT_LT(settled_speed,.1f);
    int first_near=-1,first_tip=-1;float tip_peak=0,tip_speed=0;
    auto prev=f;
    for(int i=1;i<=90;++i) {
        const float u=std::min(i/27.f,1.f);
        auto now=f;now.t.y+=1.0f*u;
        now.q=B::AxisAngle({1,0,0},2.0f*u)*f.q;
        chain.Step(prev,now,1.f/90,4.36f,cfg);prev=now;
        if(chain.Point(12).y>.3f && first_near<0)first_near=i;
        if(chain.Point(N-1).y>.3f && first_tip<0)first_tip=i;
        tip_peak=std::max(tip_peak,chain.Point(N-1).y);
        tip_speed=std::max(tip_speed,chain.TipSpeed());
    }
    std::printf("  ground settle speed %.3f y %.3f; lift near %d tip %d peak %.3f speed %.3f\n",
                settled_speed,settled_y,first_near,first_tip,tip_peak,tip_speed);
    EXPECT_GE(first_near,0);EXPECT_GT(first_tip,first_near);
    EXPECT_GT(tip_peak,1.f);EXPECT_GT(tip_speed,3.f);
}
} // namespace

namespace {
TEST(BeastCompact, TriggerOverridesEverySwingUntilReleased) {
    W::Payout payout;const W::PayoutConfig pc{};
    ASSERT_GT(payout.Step(6,4.36f,1.f/90,pc),4);
    EXPECT_FLOAT_EQ(payout.Step(30,7,1.f/90,pc,true),1);
    EXPECT_TRUE(payout.Reeled());
    for(int i=0;i<180;++i) {
        EXPECT_FLOAT_EQ(payout.Step(30,7,1.f/90,pc,true),1);
        EXPECT_FALSE(payout.Out());
    }
    EXPECT_FLOAT_EQ(payout.Step(0,7,1.f/90,pc),1); // release alone is not a throw
    EXPECT_GT(payout.Step(6,4.36f,1.f/90,pc),4); // renewed swing can extend
    EXPECT_FALSE(W::CompactCutter(2200,true,true,true));
    EXPECT_FALSE(W::CompactCutter(2400,false,true,true));
    EXPECT_FALSE(W::CompactCutter(2400,true,false,true));
    EXPECT_TRUE(W::CompactCutter(2400,true,true,true));
}
TEST(BeastCompact, InputPublicationKeepsTheTriggerAndSpeedTogether) {
    VR::SeqLock<W::SwingInput> channel;W::SwingInput sample;
    EXPECT_FALSE(channel.Load(sample));
    channel.Store({1'000'000'000,15,true});ASSERT_TRUE(channel.Load(sample));
    auto fresh=W::FreshSwingInput(sample,1'050'000'000,150'000'000);
    EXPECT_TRUE(fresh.compact);EXPECT_FLOAT_EQ(fresh.speed,15);
    EXPECT_EQ(W::FreshSwingInput(sample,1'200'000'000,150'000'000).ns,0);
    EXPECT_EQ(W::FreshSwingInput(sample,900'000'000,150'000'000).ns,0);
    channel.Store({1'010'000'000,0,false});ASSERT_TRUE(channel.Load(sample));
    EXPECT_FALSE(sample.compact);EXPECT_FLOAT_EQ(sample.speed,0);
}
TEST(BeastCompact, ReelsInThenFollowsFastMotionRigidly) {
    const auto rest=IdleChain();auto cfg=W::ConfigFor(2400);
    cfg.lock_closed=true;cfg.t_in=.18f;
    const auto f=W::TestMotion::Slice(0,0);
    std::array<B::Vec3,N> points;
    for(int i=0;i<N;++i)points[i]=W::Apply(f,rest[0]+(rest[i]-rest[0])*4.36f);
    W::Chain chain;ASSERT_TRUE(chain.Init(rest.data(),N));chain.ResetTo(points.data(),4.36f);
    for(int i=0;i<18;++i)chain.Step(f,f,1.f/90,1,cfg);
    ASSERT_NEAR(chain.Extension(),1,1e-4f);
    auto previous=f;
    for(int i=1;i<=90;++i) {
        const auto now=W::TestMotion::Slice(i/90.f,0);
        chain.Step(previous,now,1.f/90,1,cfg);
        for(int k=0;k<N;++k)EXPECT_LT(B::Length(chain.Point(k)-W::Apply(now,rest[k])),1e-4f);
        previous=now;
    }
}
class BeastResponse : public ::testing::TestWithParam<int> {};
TEST_P(BeastResponse, SideToSideLagIsReducedWithoutChangingWeight) {
    const int hz=GetParam();const auto rest=IdleChain();
    auto cfg=W::ConfigFor(2400),old=cfg;old.swing_drive=0;
    const auto pose=[&](float t) {
        const float a=(-65+130*W::TestMotion::Ease((t-.3f)/.45f)-
                       130*W::TestMotion::Ease((t-.95f)/.45f))*B::Deg;
        const B::Vec3 blade{std::sin(a),0,-std::cos(a)};
        return W::Frame{B::Vec3{0,2,0}+blade*.15f,B::FromTo(rest[N-1]-rest[0],blade)};
    };
    std::array<W::Chain,2> chains;auto previous=pose(0);
    std::array<B::Vec3,N> points;
    for(int i=0;i<N;++i)points[i]=W::Apply(previous,rest[0]+(rest[i]-rest[0])*4.36f);
    for(auto& chain:chains){ASSERT_TRUE(chain.Init(rest.data(),N));chain.ResetTo(points.data(),4.36f);}
    std::array<std::array<float,2>,2> crossings{{{{-1,-1}},{{-1,-1}}}};
    for(int i=1;i<=2*hz;++i) {
        const float t=float(i)/hz;const auto f=pose(t);
        for(int k=0;k<2;++k) {
            chains[k].Step(previous,f,1.f/hz,4.36f,k?cfg:old);
            ASSERT_LT(LinkError(chains[k],chains[k].Extension(),rest),2e-3f);
            ASSERT_LE(chains[k].MaxBend(),cfg.max_bend+.01f);
            const float tip=chains[k].Point(N-1).x;
            if(t>.525f && tip>0 && crossings[k][0]<0)crossings[k][0]=t;
            if(t>1.175f && tip<0 && crossings[k][1]<0)crossings[k][1]=t;
        }
        previous=f;
    }
    for(int stroke=0;stroke<2;++stroke) {
        const float hand_cross=stroke==0?.525f:1.175f;
        // If the old chain never reaches the other side, its delay exceeds the recording
        // window. Compare against that lower bound, not the -1 "no crossing" sentinel.
        const bool missed=crossings[0][stroke]<0;
        const float before=(missed?2.0f:crossings[0][stroke])-hand_cross;
        const float after=crossings[1][stroke]-hand_cross;
        std::printf("  reversal %d %dHz: hand-to-tip lag %s%.3f -> %.3f s\n",
                    stroke,hz,missed?">":"",before,after);
        ASSERT_GT(crossings[1][stroke],0);
        EXPECT_LT(after,before-.05f);EXPECT_LT(after,.35f);
    }
}
TEST_P(BeastResponse, ForwardThrowTravelsBeyondTheFeet) {
    const int hz=GetParam();const auto rest=IdleChain();
    auto cfg=W::ConfigFor(2400);cfg.floor_y=.03f;
    W::Chain chain;ASSERT_TRUE(chain.Init(rest.data(),N));
    auto previous=W::TestMotion::Throw(0);chain.Reset(previous);
    W::Payout payout;W::PayoutConfig pc;pc.hold_s=W::HoldFor(2400,pc.hold_s);
    float reach=0;
    for(int i=1;i<=hz;++i) {
        const auto f=W::TestMotion::Throw(float(i)/hz);
        const float speed=B::Length(W::Apply(f,{0,-.8f,0})-W::Apply(previous,{0,-.8f,0}))*hz;
        chain.Step(previous,f,1.f/hz,payout.Step(speed,W::ReachExtension(2400,4.01f,1.171f,.84f),1.f/hz,pc),cfg);
        reach=std::max(reach,-chain.Point(N-1).z);previous=f;
    }
    std::printf("  forward throw %dHz: reach %.3f m\n",hz,reach);
    EXPECT_GT(reach,2.5f);
}
INSTANTIATE_TEST_SUITE_P(FrameRates,BeastResponse,::testing::Values(30,60,90));
TEST(BeastResponse, PassiveWeightAndCaneDynamicsAreUnaffectedByDrive) {
    const auto rest=IdleChain();auto cfg=W::ConfigFor(2400),old=cfg;old.swing_drive=0;
    const W::Frame f{{0,3,0},B::FromTo(rest[N-1]-rest[0],{0,0,-1})};
    std::array<B::Vec3,N> p;for(int i=0;i<N;++i)p[i]=W::Apply(f,rest[0]+(rest[i]-rest[0])*4.36f);
    W::Chain a,b;ASSERT_TRUE(a.Init(rest.data(),N));ASSERT_TRUE(b.Init(rest.data(),N));
    a.ResetTo(p.data(),4.36f);b.ResetTo(p.data(),4.36f);
    for(int i=0;i<180;++i){a.Step(f,f,1.f/90,4.36f,cfg);b.Step(f,f,1.f/90,4.36f,old);}
    for(int i=0;i<N;++i)EXPECT_LT(B::Length(a.Point(i)-b.Point(i)),1e-6f);
    EXPECT_FLOAT_EQ(W::ConfigFor(2200).swing_drive,0);
}
} // namespace

// The recall clink (core/vr/whip_audio.h) waits for the blade: when the length closes the free
// links still swing in, and they settle within 2 cm well before the clink's 0.15 s timeout. Held
// closed (the trigger), the blade is its closed shape at the length's closure.
TEST(BeastCutter, ClosedDeviationSettlesAfterTheLengthCloses) {
    const auto rest = IdleChain();
    const W::Frame f{{0.2f, 1.4f, -0.3f}, B::AxisAngle({1, 0, 0}, 90.0f * B::Deg)};
    for (const float t_in : {0.40f, 0.24f}) for (const bool lock : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "t_in " << t_in << (lock ? " locked" : ""));
        W::Chain c;
        ASSERT_TRUE(c.Init(rest.data(), N));
        c.Reset(f);
        EXPECT_LT(c.ClosedDeviation(f), 1e-6f);
        auto cfg = W::ConfigFor(2400);
        cfg.t_in = t_in;
        cfg.lock_closed = lock;
        const float dt = 1.0f / 90.0f;
        for (int i = 0; i < 81; ++i) c.Step(f, f, dt, 4.0f, cfg); // out and drooping
        EXPECT_GT(c.ClosedDeviation(f), 0.5f);
        float closed = -1.0f, settled = -1.0f;
        for (int i = 0; i < 90 && settled < 0.0f; ++i) {
            c.Step(f, f, dt, 1.0f, cfg);
            const float t = (i + 1) * dt, d = c.ClosedDeviation(f);
            if (closed < 0.0f && c.Extension() <= 1.001f) {
                closed = t;
                if (lock) EXPECT_LT(d, 1e-4f);
                else EXPECT_GT(d, 0.02f) << "the links still swing when the length closes";
            }
            if (closed >= 0.0f && d <= 0.02f) settled = t;
        }
        std::printf("  t_in %.2f%s: length closed at %.3f s, blade within 2 cm at %.3f s\n", t_in,
                    lock ? " locked" : "", closed, settled);
        ASSERT_GE(closed, 0.0f);
        ASSERT_GE(settled, closed);
        EXPECT_LT(settled - closed, 0.15f) << "settles before the clink's timeout";
    }
}

TEST(WhipReach, CutterAddsEightPercentToFullReachAcrossMoveCaps) {
    for(float links : {.78f, .84f, .90f}) for(float base : {3.70f,5.46f,6.18f}) {
        const float idle = 1.171f;
        const float previous_reach = idle + (base-idle)*1.1536f;
        const float cap = W::ReachExtension(2400,W::ExtensionForReach(base,idle,links),idle,links);
        EXPECT_NEAR(idle+(cap-1)*links,previous_reach*1.08f,2e-6f);
        EXPECT_FLOAT_EQ(W::ReachExtension(2400,1,idle,links),1);
    }
}

namespace {
constexpr int CanePoints = 24;

std::array<B::Vec3, CanePoints> CaneLaunchRest() {
    const auto idle = SyntheticCane();
    std::array<B::Vec3, CanePoints> rest{};
    for (int i = 0; i < CanePoints; ++i)
        rest[i] = {-idle.t[i][0], -idle.t[i][1], idle.t[i][2]};
    return rest;
}

W::Frame CaneLaunchFrame(B::Vec3 movement, float t) {
    return {{0.2f + movement.x * t, 3.0f + movement.y * t, movement.z * t},
            B::FromTo({0, -1, 0}, {0, 0, -1})};
}

void ExpectSameCane(const W::Chain& a, const W::Chain& b) {
    EXPECT_FLOAT_EQ(a.Extension(), b.Extension());
    for (int i = 0; i < CanePoints; ++i) {
        EXPECT_EQ(B::LengthSq(a.Point(i) - b.Point(i)), 0.0f);
        EXPECT_EQ(B::LengthSq(a.Velocity(i) - b.Velocity(i)), 0.0f);
    }
}

TEST(CaneLaunch, ClosedLaunchFollowsStrokeWithSmallBoost) {
    const auto rest = CaneLaunchRest();
    const auto cfg = W::ConfigFor(2200);
    auto baseline = cfg;
    baseline.launch_drive = 0;
    for (int hz : {30, 60, 90})
        for (B::Vec3 direction :
             {B::Vec3{1, 0, 0}, B::Vec3{-1, 0, 0}, B::Vec3{0, 1, 0}, B::Vec3{0, -1, 0}}) {
            SCOPED_TRACE(::testing::Message()
                         << hz << " Hz direction " << direction.x << "," << direction.y);
            std::array<W::Chain, 2> chains;
            auto previous = CaneLaunchFrame(direction * 3.0f, 0);
            for (auto& chain : chains) {
                ASSERT_TRUE(chain.Init(rest.data(), CanePoints));
                chain.Reset(previous);
            }
            for (int i = 1; i <= hz / 6; ++i) {
                const auto f = CaneLaunchFrame(direction * 3.0f, float(i) / hz);
                for (int k = 0; k < 2; ++k) {
                    chains[k].Step(previous, f, 1.0f / hz, 4.5f, k ? cfg : baseline);
                    ASSERT_LE(chains[k].MaxBend(), cfg.max_bend + .01f);
                    for (int j = 1; j < CanePoints; ++j)
                        ASSERT_NEAR(B::Length(chains[k].Point(j) - chains[k].Point(j - 1)),
                                    B::Length(rest[j] - rest[j - 1]) * chains[k].Extension(),
                                    .002f);
                }
                previous = f;
            }
            const float help = B::Dot(
                chains[1].Point(CanePoints - 1) - chains[0].Point(CanePoints - 1), direction);
            std::printf("  cane closed launch %dHz (%g,%g): tip follows stroke by +%.4f m\n", hz,
                        direction.x, direction.y, help);
            EXPECT_GT(help, .005f);
            EXPECT_LT(help, .30f) << "a small launch assist";
            EXPECT_FLOAT_EQ(chains[0].Extension(), chains[1].Extension());
        }
}

TEST(CaneLaunch, ExpiredBoostDoesNotRestartFromExtraPayoutOrPartialRecall) {
    const auto rest = CaneLaunchRest();
    auto cfg = W::ConfigFor(2200), baseline = cfg;
    baseline.launch_drive = 0;
    for (int hz : {30, 60, 90}) {
        W::Chain boosted;
        ASSERT_TRUE(boosted.Init(rest.data(), CanePoints));
        auto previous = CaneLaunchFrame({3, 0, 0}, 0);
        boosted.Reset(previous);
        for (int i = 1; i <= hz / 3; ++i) {
            const auto f = CaneLaunchFrame({3, 0, 0}, float(i) / hz);
            boosted.Step(previous, f, 1.0f / hz, 4.5f, cfg);
            previous = f;
        }
        // Compare from identical physical state after the launch window, retaining its momentum.
        W::Chain unassisted = boosted;
        const auto origin = previous;
        for (int i = 1; i <= hz * 2; ++i) {
            auto f = origin;
            f.t.x += 3.0f * float(i) / hz;
            const float target = i % 9 == 0 ? 1.0f : i % 2 ? 5.0f : 3.0f;
            boosted.Step(previous, f, 1.0f / hz, target, cfg);
            unassisted.Step(previous, f, 1.0f / hz, target, baseline);
            ASSERT_GT(boosted.Extension(), 1.1f);
            ExpectSameCane(boosted, unassisted);
            previous = f;
        }
    }
}

TEST(CaneLaunch, FullRecallEnablesNextLaunchAndRestIsUnchanged) {
    const auto rest = CaneLaunchRest();
    auto cfg = W::ConfigFor(2200), baseline = cfg;
    baseline.launch_drive = 0;
    EXPECT_FLOAT_EQ(W::ConfigFor(2400).launch_drive, 0);
    for (int hz : {30, 60, 90}) {
        const auto start = CaneLaunchFrame({}, 0);
        std::array<B::Vec3, CanePoints> points{};
        for (int i = 0; i < CanePoints; ++i)
            points[i] = W::Apply(start, rest[0] + (rest[i] - rest[0]) * 4.5f);
        std::array<W::Chain, 2> chains;
        for (auto& chain : chains) {
            ASSERT_TRUE(chain.Init(rest.data(), CanePoints));
            chain.ResetTo(points.data(), 4.5f);
        }
        for (int i = 0; i < hz; ++i) {
            chains[0].Step(start, start, 1.0f / hz, 1.0f, baseline);
            chains[1].Step(start, start, 1.0f / hz, 1.0f, cfg);
            ExpectSameCane(chains[0], chains[1]);
        }
        ASSERT_FLOAT_EQ(chains[1].Extension(), 1.0f);
        auto previous = start;
        for (int i = 1; i <= hz / 6; ++i) {
            const auto f = CaneLaunchFrame({3, 0, 0}, float(i) / hz);
            chains[0].Step(previous, f, 1.0f / hz, 4.5f, baseline);
            chains[1].Step(previous, f, 1.0f / hz, 4.5f, cfg);
            previous = f;
        }
        EXPECT_GT(chains[1].Point(CanePoints - 1).x - chains[0].Point(CanePoints - 1).x, .005f);
    }
}

TEST(CaneLaunch, NearlyClosedAndAnimationHandoffsDoNotGetAnotherBoost) {
    const auto rest = CaneLaunchRest();
    auto cfg = W::ConfigFor(2200), baseline = cfg;
    baseline.launch_drive = 0;
    for (float extension : {1.00005f, 1.01f, 2.0f, 4.5f}) {
        SCOPED_TRACE(extension);
        auto previous = CaneLaunchFrame({3, 0, 0}, 0);
        W::Chain chain;
        ASSERT_TRUE(chain.Init(rest.data(), CanePoints));
        chain.Reset(previous);
        // Begin a launch, then replace it with an animation pose while the boost is pending.
        const auto f = CaneLaunchFrame({3, 0, 0}, .05f);
        chain.Step(previous, f, .05f, 4.5f, cfg);
        previous = f;
        std::array<B::Vec3, CanePoints> points{};
        for (int i = 0; i < CanePoints; ++i)
            points[i] = W::Apply(f, rest[0] + (rest[i] - rest[0]) * extension);
        chain.ResetTo(points.data(), extension);
        W::Chain unassisted = chain;
        for (int i = 1; i <= 30; ++i) {
            const auto next = CaneLaunchFrame({3, 0, 0}, .05f + float(i) / 90);
            chain.Step(previous, next, 1.0f / 90, 4.5f, cfg);
            unassisted.Step(previous, next, 1.0f / 90, 4.5f, baseline);
            ExpectSameCane(chain, unassisted);
            previous = next;
        }
    }
}

TEST(CaneLaunch, RecallImmediatelyCancelsPendingBoost) {
    const auto rest = CaneLaunchRest();
    auto cfg = W::ConfigFor(2200), baseline = cfg;
    baseline.launch_drive = 0;
    W::Chain chain;
    ASSERT_TRUE(chain.Init(rest.data(), CanePoints));
    auto previous = CaneLaunchFrame({3, 0, 0}, 0);
    chain.Reset(previous);
    for (int i = 1; i <= 6; ++i) {
        const auto f = CaneLaunchFrame({3, 0, 0}, float(i) / 90);
        chain.Step(previous, f, 1.0f / 90, 4.5f, cfg);
        previous = f;
    }
    W::Chain unassisted = chain;
    // Cancel in mid-launch, then immediately swing again before reaching closed length.
    for (int i = 7; i <= 60; ++i) {
        const auto f = CaneLaunchFrame({3, 0, 0}, float(i) / 90);
        const float target = i == 7 ? 1.0f : 4.5f;
        chain.Step(previous, f, 1.0f / 90, target, cfg);
        unassisted.Step(previous, f, 1.0f / 90, target, baseline);
        ASSERT_GT(chain.Extension(), 1.0f);
        ExpectSameCane(chain, unassisted);
        previous = f;
    }
}

TEST(CaneLaunch, BoostLasts320MillisecondsThenExpires) {
    const auto rest = CaneLaunchRest();
    auto cfg = W::ConfigFor(2200), baseline = cfg;
    baseline.launch_drive = 0;
    W::Chain chain;
    ASSERT_TRUE(chain.Init(rest.data(), CanePoints));
    auto previous = CaneLaunchFrame({3, 0, 0}, 0);
    chain.Reset(previous);
    constexpr float dt = .01f;
    for (int i = 1; i <= 24; ++i) {
        const auto f = CaneLaunchFrame({3, 0, 0}, i * dt);
        chain.Step(previous, f, dt, 4.5f, cfg);
        previous = f;
    }
    W::Chain unassisted = chain;
    const auto during = CaneLaunchFrame({3, 0, 0}, .25f);
    chain.Step(previous, during, dt, 4.5f, cfg);
    unassisted.Step(previous, during, dt, 4.5f, baseline);
    EXPECT_GT(B::Length(chain.Point(CanePoints - 1) - unassisted.Point(CanePoints - 1)), 1e-5f)
        << "still helping after the original 200 ms window";
    previous = during;
    for (int i = 26; i <= 30; ++i) {
        const auto f = CaneLaunchFrame({3, 0, 0}, i * dt);
        chain.Step(previous, f, dt, 4.5f, cfg);
        previous = f;
    }
    // The tail of the requested window still helps; a 250/300 ms window must fail.
    unassisted = chain;
    const auto tail = CaneLaunchFrame({3, 0, 0}, .31f);
    chain.Step(previous, tail, dt, 4.5f, cfg);
    unassisted.Step(previous, tail, dt, 4.5f, baseline);
    EXPECT_GT(B::Length(chain.Point(CanePoints - 1) - unassisted.Point(CanePoints - 1)), 1e-6f);
    const auto end = CaneLaunchFrame({3, 0, 0}, .32f);
    chain.Step(tail, end, dt, 4.5f, cfg);
    unassisted = chain;
    const auto after = CaneLaunchFrame({3, 0, 0}, .33f);
    chain.Step(end, after, dt, 4.5f, cfg);
    unassisted.Step(end, after, dt, 4.5f, baseline);
    ExpectSameCane(chain, unassisted);
}
} // namespace

namespace {
TEST(CaneFollowup, RepeatedStrokesStayDirectedAndTipLashesPastTheStoppedHand) {
    for (int hz : {30, 60, 90, 120})
        for (int plane = 0; plane < 4; ++plane) {
            std::array<B::Vec3, 24> rest{};
            auto axis = B::Normalize(B::Vec3{.407f, -.914f, 0});
            for (int j = 0; j < 24; ++j)
                rest[j] = B::Vec3{.04f, -.09f, 0} + axis * (.031f * j);
            auto pose = [&](float t) {
                float a = (-55 + 110 * W::TestMotion::Ease((t - .3f) / .4f) -
                           110 * W::TestMotion::Ease((t - .85f) / .4f) +
                           110 * W::TestMotion::Ease((t - 1.4f) / .4f)) *
                          B::Deg;
                if (plane % 2)
                    a = -a;
                B::Vec3 blade = plane < 2 ? B::Vec3{std::sin(a), 0, -std::cos(a)}
                                          : B::Vec3{0, std::sin(a), -std::cos(a)};
                return W::Frame{B::Vec3{0, 2.0f, 0} + blade * .3f, B::FromTo(axis, blade)};
            };
            auto cfg = W::ConfigFor(2200), old = cfg;
            old.launch_drive = 0;
            cfg.floor_y = old.floor_y = 0;
            std::array<W::Chain, 2> chains;
            auto previous = pose(0);
            for (auto& c : chains) {
                ASSERT_TRUE(c.Init(rest.data(), 24));
                c.Reset(previous);
            }
            W::Payout payout;
            W::PayoutConfig pc;
            pc.hold_s = W::HoldFor(2200, .6f);
            float cross[2][3] = {{-1, -1, -1}, {-1, -1, -1}}, peak[2][3] = {}, after[2][3] = {};
            for (int i = 1; i <= hz * 3; ++i) {
                float t = float(i) / hz;
                auto f = pose(t);
                float speed = B::Length(W::Apply(f, rest[0] + axis * .7f) -
                                        W::Apply(previous, rest[0] + axis * .7f)) *
                              hz;
                float target = payout.Step(speed, 5.2f, 1.f / hz, pc);
                for (int k = 0; k < 2; ++k) {
                    auto& c = chains[k];
                    c.Step(previous, f, 1.f / hz, target, (k || t < .8f) ? cfg : old);
                    ASSERT_TRUE(B::IsFinite(c.Point(23)));
                    ASSERT_LT(c.MaxSpeed(), 80.f);
                    ASSERT_LE(c.MaxBend(), cfg.max_bend + .01f);
                    for (int j = 1; j < 24; ++j) {
                        ASSERT_GE(c.Point(j).y, -.001f);
                        if (c.Point(j).y > .01f && c.Point(j - 1).y > .01f)
                            ASSERT_NEAR(B::Length(c.Point(j) - c.Point(j - 1)),
                                        .031f * c.Extension(), .002f);
                    }
                    for (int stroke = 0; stroke < 3; ++stroke) {
                        float mid = .5f + .55f * stroke, end = .7f + .55f * stroke;
                        float d = (stroke == 1 ? -1.f : 1.f) * (plane % 2 ? -1.f : 1.f);
                        float p = plane < 2 ? c.Point(23).x : c.Point(23).y - f.t.y;
                        if (t > mid && t < end + .55f && p * d > 0 && cross[k][stroke] < 0)
                            cross[k][stroke] = t - mid;
                        if (t >= mid - .2f && t < end + .2f)
                            peak[k][stroke] = std::max(peak[k][stroke], c.TipSpeed());
                        if (t >= end && t < end + .14f)
                            after[k][stroke] = std::max(after[k][stroke], c.TipSpeed());
                    }
                }
                previous = f;
            }
            SCOPED_TRACE(::testing::Message() << hz << " Hz plane " << plane);
            EXPECT_FLOAT_EQ(cross[0][0], cross[1][0]); // the first throw stays intact
            for (int stroke = 1; stroke < 3; ++stroke) {
                EXPECT_GT(cross[1][stroke], .04f); // some real lag: it is not a rigid sword
                EXPECT_LT(cross[1][stroke], .24f);
                EXPECT_LE(cross[1][stroke], cross[0][stroke] + 1.f / hz);
                EXPECT_GT(after[1][stroke], W::HurtTipSpeed); // hand stopped; tip still lashes
            }
            EXPECT_LT(cross[1][2], cross[0][2] - .015f);
            for (int s = 0; s < 3; ++s)
                std::printf(
                    "%dHz plane%d swing%d lag %.3f -> %.3f peak %.1f -> %.1f after %.1f -> %.1f\n",
                    hz, plane, s + 1, cross[0][s], cross[1][s], peak[0][s], peak[1][s], after[0][s],
                    after[1][s]);
        }
}

} // namespace

namespace {
TEST(CaneFollowup, PauseAndReversalRearmButCirclesAndTremorDoNot) {
    for (int hz : {240, 270, 360}) {
        const float dt = 1.f / hz;
        W::LashDrive drive;
        drive.Step({3, 0, 0}, dt, true, true);
        for (int i = 0; i < hz; ++i)
            drive.Step({3, 0, 0}, dt, true, false);
        EXPECT_EQ(drive.Weight(.5f), 0);
        for (int i = 1; i <= hz * 2; ++i) {
            float a = float(i) / hz * 6;
            drive.Step({3 * std::cos(a), 0, 3 * std::sin(a)}, dt, true, false);
            EXPECT_EQ(drive.Weight(.5f), 0); // no extra kick once per circle
        }
        for (int i = 0; i < hz / 5; ++i)
            drive.Step({}, dt, true, false);
        for (int i = 0; i < hz / 10; ++i)
            drive.Step({-3, 0, 0}, dt, true, false);
        EXPECT_GT(drive.Weight(.5f), .5f);
        EXPECT_EQ(drive.Weight(1), 0); // no artificial force at the very tip
        for (int i = 0; i < hz; ++i)
            drive.Step({-3, 0, 0}, dt, true, false);
        for (int i = 0; i < hz / 10; ++i)
            drive.Step({3, 0, 0}, dt, true, false);
        EXPECT_GT(drive.Weight(.5f), .5f); // backhand without any reel-in
        for (int i = 0; i < hz; ++i)
            drive.Step({3, 0, 0}, dt, true, false);
        for (int i = 0; i < hz; ++i) {
            drive.Step({i % 2 ? .4f : -.4f, 0, 0}, dt, true, false);
            EXPECT_EQ(drive.Weight(.5f), 0);
        }
    }
}
TEST(CaneFollowup, RecallAndResetCancelThePulseAndCarryPreservesItsDirection) {
    W::LashDrive drive;
    for (int i = 0; i < 20; ++i)
        drive.Step({}, .005f, true, false);
    for (int i = 0; i < 20; ++i)
        drive.Step({3, 0, 0}, .005f, true, false);
    ASSERT_GT(drive.Weight(.5f), 0);
    drive.Step({3, 0, 0}, .005f, false, false);
    EXPECT_EQ(drive.Weight(.5f), 0);
    drive.Step({3, 0, 0}, .005f, true, true);
    for (int i = 0; i < 100; ++i)
        drive.Step({3, 0, 0}, .005f, true, false);
    drive.Carry(B::AxisAngle({0, 1, 0}, B::Pi));
    for (int i = 0; i < 20; ++i)
        drive.Step({-3, 0, 0}, .005f, true, false);
    EXPECT_EQ(drive.Weight(.5f), 0); // snap turn is not a backhand
    drive.Reset();
    EXPECT_EQ(drive.Weight(.5f), 0);
}
} // namespace
