// SPDX-License-Identifier: GPL-2.0-or-later

// Simon's Bowblade's bow (core/vr/bow.h) at 90 Hz: it comes out with an arrow ready in the string
// hand, the next ones from over the shoulder, each nocked at the bow and never dropped (bow_quiver;
// else the nock needs the squeeze at the bow, or just before the hand gets there); a part draw is
// the quick shot, a full one held charges it on either firing route;
// too little puts the arrow back (in the fingers with the quiver); a hand lost keeps the arrow for
// the release; a tick per third of the draw, the wall at full; the hand on the string stays on the
// arrow's line between the string at rest and full draw; the arrow's line is the one from just
// before the release, not the release's flinch.

#include <cmath>

#include <gtest/gtest.h>

#include "core/vr/bow.h"
#include "core/vr/bow_pose.h"
#include "c0000_test_skeleton.h"

namespace {

namespace W = VR::Bow;

constexpr float Dt = 1.0f / 90.0f;
// The bow hand held out in front, the string hand drawing straight back toward the face.
constexpr W::V3 BowAt{-0.10f, 1.35f, -0.55f};

W::Input At(W::V3 string, bool pinch, bool valid = true) {
    W::Input in{};
    in.dt = Dt;
    in.valid = valid;
    in.bow = BowAt;
    in.string = string;
    in.pinch = pinch;
    return in;
}

// The squeeze at the bow nocks (bow_quiver=0): the draw's and the shot's tests.
W::Settings Plain() {
    W::Settings s{};
    s.quiver = false;
    return s;
}

// The string hand `d` metres behind the bow along +z (toward the archer).
W::V3 Drawn(float d) {
    return {BowAt.x, BowAt.y, BowAt.z + d};
}

// Nocks, draws to `to` metres over `secs`, then holds `hold` seconds; returns the last output.
W::Output Draw(W::Bow& bow, const W::Settings& s, float to, float secs, float hold = 0.0f,
               int* detents = nullptr, bool* full = nullptr) {
    W::Output o = bow.Update(At(Drawn(0.08f), true), s);
    EXPECT_EQ(o.event, W::Event::Nock);
    const int n = static_cast<int>(secs / Dt);
    for (int i = 1; i <= n; ++i) {
        o = bow.Update(At(Drawn(0.08f + (to - 0.08f) * i / n), true), s);
        if (detents != nullptr && o.detent) {
            ++*detents;
        }
        if (full != nullptr && o.event == W::Event::Full) {
            *full = true;
        }
    }
    for (int i = 0; i < static_cast<int>(hold / Dt); ++i) {
        o = bow.Update(At(Drawn(to), true), s);
        if (full != nullptr && o.event == W::Event::Full) {
            *full = true; // (bow_direct: charged by the hold)
        }
    }
    return o;
}

TEST(Bow, NockingNeedsTheSqueezeAtTheBow) {
    const W::Settings s = Plain();
    W::Bow bow;
    // Squeezed 35 cm from the bow: nothing; still held when brought in 0.4 s later: no nock
    // either (a new squeeze is needed there).
    EXPECT_EQ(bow.Update(At(Drawn(0.35f), true), s).event, W::Event::None);
    for (int i = 0; i < 36; ++i) {
        EXPECT_EQ(bow.Update(At(Drawn(0.35f), true), s).event, W::Event::None);
    }
    EXPECT_EQ(bow.Update(At(Drawn(0.08f), true), s).event, W::Event::None);
    EXPECT_EQ(bow.State(), W::Phase::Idle);
    bow.Update(At(Drawn(0.08f), false), s);
    EXPECT_EQ(bow.Update(At(Drawn(0.08f), true), s).event, W::Event::Nock);
    EXPECT_EQ(bow.State(), W::Phase::Nocked);
    // A squeeze begun on the way, 0.2 s before the hand reaches the bow: it nocks there.
    W::Bow early;
    early.Update(At(Drawn(0.40f), false), s);
    W::Output o{};
    for (int i = 0; i <= 18 && o.event == W::Event::None; ++i) {
        o = early.Update(At(Drawn(0.40f - 0.32f * static_cast<float>(i) / 18.0f), true), s);
    }
    EXPECT_EQ(o.event, W::Event::Nock);
}

TEST(Bow, APartDrawIsTheQuickShotAlongTheArrow) {
    const W::Settings s = Plain();
    W::Bow bow;
    const W::Output held = Draw(bow, s, 0.40f, 0.4f, 0.2f);
    EXPECT_NEAR(held.draw, (0.40f - s.rest_m) / (s.draw_m - s.rest_m), 1e-3f);
    EXPECT_FALSE(held.charged);
    const W::Output shot = bow.Update(At(Drawn(0.40f), false), s);
    EXPECT_EQ(shot.event, W::Event::Quick);
    EXPECT_FALSE(shot.charged);
    EXPECT_NEAR(shot.aim.z, -1.0f, 1e-4f); // string -> bow: forward
    EXPECT_NEAR(shot.aim_draw, held.draw, 1e-3f);
    EXPECT_EQ(bow.State(), W::Phase::Idle);
}

TEST(Bow, AFullDrawHoldsTheChargeUntilTheRelease) {
    const W::Settings s = Plain();
    W::Bow bow;
    bool full = false;
    const W::Output held = Draw(bow, s, 0.62f, 0.6f, 1.0f, nullptr, &full);
    EXPECT_TRUE(full);
    EXPECT_FLOAT_EQ(held.draw, 1.0f);
    EXPECT_TRUE(held.charged);
    // Easing off a little keeps the charge (it is let go only by the release).
    EXPECT_TRUE(bow.Update(At(Drawn(0.50f), true), s).charged);
    const W::Output shot = bow.Update(At(Drawn(0.50f), false), s);
    EXPECT_EQ(shot.event, W::Event::Charged);
    EXPECT_FALSE(shot.charged);
}

// The draw first reaches full on the release frame: R2 never went to the game, so it is the quick
// shot (R1), never a charged shot with nothing to let go.
TEST(Bow, AReleaseAsTheDrawReachesFullIsTheQuickShot) {
    const W::Settings s = Plain();
    W::Bow bow;
    const W::Output held = Draw(bow, s, 0.52f, 0.4f, 0.1f);
    EXPECT_FALSE(held.charged);
    const W::Output shot = bow.Update(At(Drawn(0.54f), false), s);
    EXPECT_EQ(shot.event, W::Event::Quick);
    EXPECT_FALSE(shot.charged);
}

TEST(Bow, TooLittleDrawPutsTheArrowBack) {
    const W::Settings s = Plain();
    W::Bow bow;
    Draw(bow, s, 0.15f, 0.2f);
    EXPECT_EQ(bow.Update(At(Drawn(0.15f), false), s).event, W::Event::PutBack);
    EXPECT_EQ(bow.State(), W::Phase::Idle);
}

TEST(Bow, ATickPerThirdOfTheDraw) {
    const W::Settings s = Plain();
    W::Bow bow;
    int detents = 0;
    Draw(bow, s, 0.62f, 0.8f, 0.0f, &detents);
    EXPECT_EQ(detents, 3);
}

// bow_direct: the charge is the full draw held charge_s (the triple buzz on Full); released
// before that it is the quick shot. The fallback also waits for a physical release.
TEST(Bow, TheChargeIsTheFullDrawHeld) {
    const W::Settings s = Plain();
    W::Bow quick;
    bool full = false;
    Draw(quick, s, 0.62f, 0.4f, s.charge_s - 0.15f, nullptr, &full);
    EXPECT_FALSE(full);
    EXPECT_EQ(quick.Update(At(Drawn(0.62f), false), s).event, W::Event::Quick);
    W::Bow charged;
    Draw(charged, s, 0.62f, 0.4f, s.charge_s + 0.05f, nullptr, &full);
    EXPECT_TRUE(full);
    EXPECT_EQ(charged.Update(At(Drawn(0.62f), false), s).event, W::Event::Charged);
    // Eased off below full before it charged: the hold starts again.
    W::Bow eased;
    full = false;
    Draw(eased, s, 0.62f, 0.4f, s.charge_s - 0.15f, nullptr, &full);
    eased.Update(At(Drawn(0.45f), true), s);
    for (int i = 0; i < static_cast<int>(0.3f / Dt); ++i) {
        full = eased.Update(At(Drawn(0.62f), true), s).event == W::Event::Full || full;
    }
    EXPECT_FALSE(full);
    W::Settings game = s;
    game.direct = false;
    W::Bow own;
    full = false;
    Draw(own, game, 0.62f, 0.6f, 0.8f, nullptr, &full);
    EXPECT_TRUE(full);
}

TEST(Bow, TheShotKeepsTheLineFromBeforeTheReleasesFlinch) {
    const W::Settings s = Plain();
    W::Bow bow;
    Draw(bow, s, 0.45f, 0.4f, 0.1f);
    // The release frame: the string hand has jerked 15 cm aside already.
    const W::Output shot =
        bow.Update(At({BowAt.x + 0.15f, BowAt.y, BowAt.z + 0.45f}, false), s);
    EXPECT_EQ(shot.event, W::Event::Quick);
    EXPECT_NEAR(shot.aim.x, 0.0f, 1e-3f);
    EXPECT_NEAR(shot.aim.z, -1.0f, 1e-3f);
}

// A hand the cameras lose (the string hand anchored at the cheek) keeps the arrow nocked however
// long: the release shoots along the last line seen, a part draw too.
TEST(Bow, AHandLostKeepsTheArrowForTheRelease) {
    const W::Settings s = Plain();
    W::Bow bow;
    Draw(bow, s, 0.40f, 0.3f);
    for (int i = 0; i < 180; ++i) { // 2 s
        EXPECT_EQ(bow.Update(At(Drawn(0.40f), true, false), s).event, W::Event::None);
    }
    EXPECT_EQ(bow.State(), W::Phase::Nocked);
    const W::Output o = bow.Update(At(Drawn(0.40f), false, false), s);
    EXPECT_EQ(o.event, W::Event::Quick);
    EXPECT_NEAR(o.aim.z, -1.0f, 1e-3f);
    // Charged before the loss: the charged shot at the release, never by itself.
    W::Bow charged;
    Draw(charged, s, 0.62f, 0.4f, s.charge_s + 0.05f);
    for (int i = 0; i < 90; ++i) {
        EXPECT_EQ(charged.Update(At(Drawn(0.62f), true, false), s).event, W::Event::None);
    }
    EXPECT_EQ(charged.Update(At(Drawn(0.62f), false, false), s).event, W::Event::Charged);
    // Too little draw is still put back.
    W::Bow little;
    Draw(little, s, 0.15f, 0.2f);
    little.Update(At(Drawn(0.15f), true, false), s);
    EXPECT_EQ(little.Update(At(Drawn(0.15f), false, false), s).event, W::Event::PutBack);
}

// Both fire routes retain a fully charged arrow through tracking loss.
TEST(Bow, TrackingLossNeverReleasesTheFallbackShot) {
    W::Settings s = Plain();
    s.direct = false;
    W::Bow bow;
    Draw(bow, s, 0.62f, 0.6f, 0.8f);
    for (int i = 0; i < 900; ++i) {
        EXPECT_EQ(bow.Update(At(Drawn(0.62f), true, false), s).event, W::Event::None);
    }
    EXPECT_EQ(bow.State(), W::Phase::Nocked);
    EXPECT_EQ(bow.Update(At(Drawn(0.62f), false, false), s).event, W::Event::Charged);
    EXPECT_EQ(bow.Update(At(Drawn(0.62f), false, false), s).event, W::Event::None);
}

TEST(Bow, LettingDownAChargedArrowCancelsIt) {
    W::Bow bow;
    const W::Settings s = Plain();
    Draw(bow, s, 0.62f, 0.4f, 0.8f);
    const auto down = bow.Update(At(Drawn(0.13f), true), s);
    EXPECT_FALSE(down.charged);
    EXPECT_EQ(bow.Update(At(Drawn(0.13f), false), s).event, W::Event::PutBack);
}

TEST(Bow, ReleaseFlinchCannotCreateDrawEnergy) {
    W::Bow bow;
    const W::Settings s = Plain();
    Draw(bow, s, 0.14f, 0.3f, 0.1f);
    EXPECT_EQ(bow.Update(At(Drawn(0.60f), false), s).event, W::Event::PutBack);
    W::Bow just_nocked;
    just_nocked.Update(At(Drawn(0.08f), true), s);
    EXPECT_EQ(just_nocked.Update(At(Drawn(0.60f), false), s).event, W::Event::PutBack);
}

TEST(Bow, HoldingForTenSecondsNeverShootsOnEitherRoute) {
    for (bool direct : {false, true}) {
        W::Settings s = Plain();
        s.direct = direct;
        W::Bow bow;
        Draw(bow, s, 0.62f, 0.5f);
        for (int i = 0; i < 900; ++i) {
            const auto e = bow.Update(At(Drawn(0.62f), true), s).event;
            EXPECT_TRUE(e == W::Event::None || e == W::Event::Full);
        }
        EXPECT_EQ(bow.Update(At(Drawn(0.62f), false), s).event, W::Event::Charged);
    }
}

TEST(Bow, SmallTremorDoesNotRestartACharge) {
    W::Settings s = Plain();
    W::Bow bow;
    Draw(bow, s, s.rest_m + (s.draw_m - s.rest_m) * 0.86f, 0.3f);
    W::Output o;
    for (int i = 0; i < 90; ++i) {
        const float fraction = i % 2 ? 0.83f : 0.86f;
        o = bow.Update(At(Drawn(s.rest_m + (s.draw_m - s.rest_m) * fraction), true), s);
    }
    EXPECT_TRUE(o.charged);
}

TEST(Bow, DisconnectedTriggerCannotReleaseAnArrow) {
    const auto s = Plain();
    W::Bow bow;
    bow.Update(At(Drawn(0.08f), true), s);
    for (int i = 0; i < 90; ++i) bow.Update(At(Drawn(0.60f), true), s);
    auto missing = At(Drawn(0.60f), false, false);
    missing.pinch_valid = false;
    for (int i = 0; i < 900; ++i) {
        const auto o = bow.Update(missing, s);
        EXPECT_EQ(o.event, W::Event::None);
        EXPECT_EQ(o.phase, W::Phase::Nocked);
        EXPECT_TRUE(o.charged);
    }
    EXPECT_EQ(bow.Update(At(Drawn(0.60f), true), s).event, W::Event::None);
    EXPECT_EQ(bow.Update(At(Drawn(0.60f), false), s).event, W::Event::Charged);
}

TEST(Bow, NativeArrowNockAndShaftFollowTheBowAtEveryRoll) {
    namespace B = VR::Body;
    const B::Vec3 nock{0.2f, 1.5f, 0.1f};
    const B::Vec3 rest{-0.1f, 1.4f, -0.5f};
    const auto line = B::Normalize(rest - nock);
    for (float roll : {0.0f, 0.7f, 1.5f, 3.14f}) {
        const auto q = W::AlongString(B::AxisAngle({0, 0, 1}, roll), line);
        const auto arrow = W::NativeArrow(nock, q);
        EXPECT_NEAR(B::Length(arrow.t + B::Rotate(q, {0, W::NativeNockM, 0}) - nock), 0, 1e-5f);
        EXPECT_NEAR(B::Dot(B::Rotate(q, {0, -1, 0}), line), 1, 1e-5f);
        EXPECT_NEAR(B::Length(B::Cross(arrow.t - rest, line)), 0, 1e-5f);
    }
}

TEST(Bow, NativeBowLimbsKeepTheControllerUpAxisAtEveryRoll) {
    namespace B = VR::Body;
    for (float roll : {0.0f, 0.7f, 1.5f, 3.14f}) {
        const auto grip = B::AxisAngle({0, -1, 0}, roll);
        const auto forward = B::Rotate(grip, {0, -1, 0});
        const auto mesh = W::BowMeshOrientation(grip, forward);
        EXPECT_NEAR(B::Dot(B::Rotate(mesh, {0, 0, 1}), B::Rotate(grip, {1, 0, 0})), 1, 1e-5f);
        EXPECT_NEAR(B::Dot(B::Rotate(mesh, {0, -1, 0}), forward), 1, 1e-5f);
    }
}

TEST(Bow, NativePoseCompensatesAnimatedRootsWithoutMovingHands) {
    namespace B = VR::Body;
    namespace C = B::C0000;
    for (int bi : {0, 1}) {
        for (bool nocked : {false, true}) {
            auto sk = C::Test::TestSkeleton();
            std::array<B::QsTransform, C::BoneCount> pose{};
            for (int i = 0; i < C::BoneCount; ++i) {
                const auto& r = sk.local[i];
                pose[i] = {{r.t[0], r.t[1], r.t[2]}, {r.q[0], r.q[1], r.q[2], r.q[3]}, {1, 1, 1}};
            }
            // Nonzero, rotated body attachment dummies as well as animated part roots.
            sk.right_weapon_base.position = {0.02f, 0.03f, -0.01f};
            sk.left_weapon_base.position = {-0.02f, 0.03f, 0.01f};
            const auto before = pose;
            const auto& bd = bi == 0 ? sk.left_weapon_base : sk.right_weapon_base;
            const auto& ad = bi == 0 ? sk.right_weapon_base : sk.left_weapon_base;
            const auto ba = W::Attachment(bd), aa = W::Attachment(ad);
            const auto grip = B::Compose(*B::ModelOf(pose.data(), C::Parents.data(), C::BoneCount, bd.bone), ba);
            const auto hand = B::Compose(*B::ModelOf(pose.data(), C::Parents.data(), C::BoneCount, ad.bone), aa);
            // On the string the nock is the string hand's grip and the shaft rests beside the bow
            // fist, on the string hand's side; carried, it is pinched toward the hand's top.
            const auto lc = B::ModelOf(pose.data(), C::Parents.data(), C::BoneCount, C::L_Clavicle);
            const auto rc = B::ModelOf(pose.data(), C::Parents.data(), C::BoneCount, C::R_Clavicle);
            ASSERT_TRUE(lc && rc);
            const auto string_side = B::Normalize(bi == C::Left ? rc->t - lc->t : lc->t - rc->t);
            const auto rest = W::ArrowRest(grip.t, B::Rotate(grip.q, {1, 0, 0}), string_side);
            constexpr float pitch = 45.0f * B::Deg;
            // Carried: held round the shaft, its nock CarryBackM behind the fist.
            const auto carried = B::Rotate(hand.q, {-std::sin(pitch), -std::cos(pitch), 0});
            const auto nock = nocked ? hand.t
                                     : hand.t + B::Rotate(hand.q, {W::CarryLiftM, 0, 0}) -
                                           carried * W::CarryBackM;
            const auto up = B::Rotate(grip.q, {1, 0, 0});
            const auto pointing = B::Rotate(grip.q, {-std::sin(pitch), -std::cos(pitch), 0});
            W::NativeGeometry geo{};
            geo.bow_root = {{0.4f, 0.2f, 0.1f}, B::AxisAngle({1, 0, 0}, 1.2f), {1, 1, 1}};
            geo.bow_grip = geo.bow_root.t + B::Rotate(geo.bow_root.q, {0, -0.5f, 0});
            geo.arrow_root = {{-0.1f, 0.05f, 0.2f}, B::AxisAngle({0, 0, 1}, -0.8f), {1, 1, 1}};
            W::BowHold expected_hold{}, hold{};
            const auto expected = W::HeldBow(expected_hold, grip.q, pointing, nocked, rest, nock);
            const auto written = W::PoseNativeArrow(pose.data(), sk, bi, nocked, geo, pitch, hold);
            EXPECT_EQ(written.count(), 2u); // the bow is held the same way on the string or not
            for (int i = 0; i < C::BoneCount; ++i) {
                if (written.test(i)) continue;
                EXPECT_FLOAT_EQ(B::Length(pose[i].t - before[i].t), 0);
                EXPECT_FLOAT_EQ(pose[i].q.x, before[i].q.x);
                EXPECT_FLOAT_EQ(pose[i].q.y, before[i].q.y);
                EXPECT_FLOAT_EQ(pose[i].q.z, before[i].q.z);
                EXPECT_FLOAT_EQ(pose[i].q.w, before[i].q.w);
            }
            const auto arrow = B::Compose(B::Compose(*B::ModelOf(pose.data(), C::Parents.data(), C::BoneCount, ad.bone), aa), geo.arrow_root);
            EXPECT_NEAR(B::Length(arrow.t + B::Rotate(arrow.q, {0, W::NativeNockM * W::ArrowScale, 0}) - nock), 0, 1e-5f);
            EXPECT_NEAR(arrow.s.y, W::ArrowScale, 1e-6f); // shown at 90%, the nock still in the fingers
            const auto bow = B::Compose(*B::ModelOf(pose.data(), C::Parents.data(), C::BoneCount, bd.bone), ba);
            // The bow's grip just off the fist toward the string side, either way: the fist on its
            // outer side, room for the arrow on the other.
            const auto bow_grip = grip.t + W::Across(up, string_side) * W::BowShiftM;
            EXPECT_NEAR(B::Length(bow.t + B::Rotate(bow.q, geo.bow_grip) - bow_grip), 0, 1e-5f);
            // Held as HeldBow holds it, on the string or not.
            EXPECT_LT(B::AngleBetween(B::Normalize(bow.q * geo.bow_root.q), expected), 1e-3f);
            if (!nocked) {
                // Pointing forward: the blade line tilted down about the hand's side axis.
                EXPECT_NEAR(B::Dot(B::Rotate(arrow.q, {0, -1, 0}), carried), 1, 1e-5f);
            }
            if (nocked) {
                // The arrow's line runs through the rest beside the fist, never through the grip
                // (this rig's bind-pose hands meet the line at an angle: at least 1.5 cm off).
                EXPECT_GT(B::Length(B::Cross(grip.t - nock, B::Normalize(rest - nock))), 0.015f);
                const auto line = B::Normalize(rest - nock);
                EXPECT_NEAR(B::Dot(B::Rotate(arrow.q, {0, -1, 0}), line), 1, 1e-5f);
            }
        }
    }
}

// Reproduce transform -> draw -> release -> empty hand -> next arrow. The visual
// grip must survive Idle without keeping the controls in Nocked or Holding.
TEST(Bow, EmptyBowKeepsItsGripAfterQuickAndChargedShots) {
    namespace B = VR::Body;
    namespace C = B::C0000;
    for (bool direct : {false, true}) {
        for (bool charged : {false, true}) {
            for (int bi : {C::Left, C::Right}) {
                SCOPED_TRACE(::testing::Message() << direct << " " << charged << " " << bi);
                auto sk = C::Test::TestSkeleton();
                std::array<B::QsTransform, C::BoneCount> original{};
                for (int i = 0; i < C::BoneCount; ++i) {
                    const auto& r = sk.local[i];
                    original[i] = {{r.t[0], r.t[1], r.t[2]},
                                   {r.q[0], r.q[1], r.q[2], r.q[3]}, {1, 1, 1}};
                }
                const auto& bd = bi == C::Left ? sk.left_weapon_base : sk.right_weapon_base;
                const auto& ad = bi == C::Left ? sk.right_weapon_base : sk.left_weapon_base;
                W::NativeGeometry geo{};
                geo.bow_root.q = B::AxisAngle({1, 0, 0}, 1.2f);
                geo.bow_grip = {0.1f, -0.4f, 0.2f};
                W::BowHold hold{};
                W::Bow bow;
                W::Settings settings{};
                settings.direct = direct;
                bow.Arm(false, true); // transformation: arrow ready, not nocked
                const auto pose = [&] {
                    auto local = original; // each game frame starts from its native pose
                    const bool arrow = bow.State() != W::Phase::Idle;
                    const auto written = W::PoseNativeArrow(
                        local.data(), sk, bi, bow.State() == W::Phase::Nocked,
                        geo, 45.0f * B::Deg, hold, arrow);
                    EXPECT_TRUE(written.test(bd.bone));
                    EXPECT_EQ(written.test(ad.bone), arrow);
                    EXPECT_EQ(written.count(), arrow ? 2u : 1u);
                    for (int i = 0; i < C::BoneCount; ++i) {
                        if (written.test(i)) continue;
                        EXPECT_FLOAT_EQ(B::Length(local[i].t - original[i].t), 0);
                        EXPECT_FLOAT_EQ(local[i].q.x, original[i].q.x);
                        EXPECT_FLOAT_EQ(local[i].q.y, original[i].q.y);
                        EXPECT_FLOAT_EQ(local[i].q.z, original[i].q.z);
                        EXPECT_FLOAT_EQ(local[i].q.w, original[i].q.w);
                    }
                    return local[bd.bone];
                };
                const auto initial = pose();
                // Arrow visibility alone cannot change the transformed bow grip.
                bow.Reset();
                const auto empty = pose();
                EXPECT_NEAR(B::Length(empty.t - initial.t), 0, 1e-5f);
                EXPECT_LT(B::AngleBetween(empty.q, initial.q), 1e-3f);
                bow.Arm(false, true);
                Draw(bow, settings, charged ? 0.62f : 0.40f, 0.5f, charged ? 0.8f : 0.0f);
                const auto drawn = pose();
                const auto shot = bow.Update(At(Drawn(charged ? 0.62f : 0.40f), false), settings);
                EXPECT_EQ(shot.event, charged ? W::Event::Charged : W::Event::Quick);
                EXPECT_EQ(bow.State(), W::Phase::Idle);
                EXPECT_FALSE(shot.charged);
                const auto released = pose();
                EXPECT_NEAR(B::Length(released.t - drawn.t), 0, 1e-5f);
                EXPECT_LT(B::AngleBetween(released.q, drawn.q), 1e-3f);
                // Normal idle controls: no phantom arrow or repeated shot; fetch the next one.
                EXPECT_EQ(bow.Update(At(Drawn(0.08f), true), settings).event, W::Event::Empty);
                EXPECT_EQ(bow.State(), W::Phase::Idle);
                bow.Update(At(Drawn(0.08f), false), settings);
                auto take = At({0.15f, 1.55f, 0.10f}, true);
                take.at_quiver = true;
                EXPECT_EQ(bow.Update(take, settings).event, W::Event::Take);
                EXPECT_EQ(bow.State(), W::Phase::Holding);
                const auto ready = pose();
                EXPECT_NEAR(B::Length(ready.t - released.t), 0, 1e-5f);
                EXPECT_LT(B::AngleBetween(ready.q, released.q), 1e-3f);
            }
        }
    }
}

TEST(Bow, NativeStringCentreLandsOnNockInRotatedPartSkeleton) {
    namespace B = VR::Body;
    std::array<B::QsTransform, 3> pose{};
    const std::int16_t parents[]{-1, 0, 1};
    pose[0] = {{0.5f, 0, -0.2f}, B::AxisAngle({0, 0, 1}, 0.8f), {1, 1, 1}};
    pose[1].t = {0, -0.3f, 0};
    const B::Vec3 nock{-0.2f, 0.4f, 0.3f};
    ASSERT_TRUE(W::MoveString(pose.data(), parents, 3, 2, nock));
    const auto centre = B::ModelOf(pose.data(), parents, 3, 2);
    ASSERT_TRUE(centre);
    EXPECT_NEAR(B::Length(centre->t - nock), 0, 1e-5f);
    EXPECT_FLOAT_EQ(pose[1].t.y, -0.3f);
    EXPECT_FALSE(W::MoveString(pose.data(), parents, 3, 4, nock));
}

TEST(Bow, TrackingOcclusionKeepsNockWithBowAndResetsOnRecenter) {
    namespace B = VR::Body;
    W::NockedTracking hold;
    B::HandsSample sample{};
    sample.bow_hand = 0;
    sample.bow_nocked = true;
    for (auto& hand : sample.hand) {
        hand.position_valid = hand.position_tracked = hand.orientation_valid = true;
    }
    sample.hand[0].p = {0, 1, -0.6f};
    sample.hand[1].p = {0, 1, 0};
    hold.Apply(sample);
    sample.hand[0].p.x = 0.2f;
    sample.hand[1].position_tracked = false;
    sample.hand[1].p = {9, 9, 9};
    hold.Apply(sample);
    EXPECT_TRUE(B::IsTracked(sample.hand[1]));
    EXPECT_NEAR(sample.hand[1].p.x, 0.2f, 1e-5f);
    EXPECT_NEAR(B::Length(sample.hand[1].p - sample.hand[0].p), 0.6f, 1e-5f);
    sample.space_serial++;
    sample.hand[1].position_tracked = false;
    hold.Apply(sample);
    EXPECT_FALSE(B::IsTracked(sample.hand[1]));
}

// A trigger already held when the bow comes up is no squeeze.
TEST(Bow, ATriggerHeldAsTheBowComesUpDoesNotNock) {
    const W::Settings s = Plain();
    W::Bow bow;
    bow.Arm(true);
    EXPECT_EQ(bow.Update(At(Drawn(0.08f), true), s).event, W::Event::None);
    bow.Update(At(Drawn(0.08f), false), s);
    EXPECT_EQ(bow.Update(At(Drawn(0.08f), true), s).event, W::Event::Nock);
    // The same with an arrow ready in the hand (bow_quiver).
    const W::Settings q{};
    W::Bow ready;
    ready.Arm(true, true);
    for (int i = 0; i < 30; ++i) {
        EXPECT_EQ(ready.Update(At(Drawn(0.08f), true), q).event, W::Event::None);
    }
    EXPECT_EQ(ready.State(), W::Phase::Holding);
    ready.Update(At(Drawn(0.08f), false), q);
    EXPECT_EQ(ready.Update(At(Drawn(0.08f), true), q).event, W::Event::Nock);
}

// bow_quiver (the default): an arrow taken from over the shoulder and carried by its nock to the
// bow nocks there and shoots as before; a squeeze at the bow with none in hand is a soft no (once);
// let go on the way, it stays in the fingers and a squeeze at the bow nocks it.
TEST(Bow, AnArrowFromTheQuiverNocksAtTheBow) {
    const W::Settings s{};
    ASSERT_TRUE(s.quiver);
    W::Bow bow;
    EXPECT_EQ(bow.Update(At(Drawn(0.08f), true), s).event, W::Event::Empty);
    EXPECT_EQ(bow.Update(At(Drawn(0.08f), true), s).event, W::Event::None);
    EXPECT_EQ(bow.State(), W::Phase::Idle);
    bow.Update(At(Drawn(0.08f), false), s);
    const W::V3 shoulder{0.15f, 1.55f, 0.10f};
    W::Input take = At(shoulder, true);
    take.at_quiver = true;
    EXPECT_EQ(bow.Update(take, s).event, W::Event::Take);
    EXPECT_EQ(bow.State(), W::Phase::Holding);
    const W::V3 to = Drawn(0.08f);
    W::Output o{};
    for (int i = 1; i <= 36 && o.event == W::Event::None; ++i) {
        const float k = static_cast<float>(i) / 36.0f;
        const W::V3 at{shoulder.x + (to.x - shoulder.x) * k, shoulder.y + (to.y - shoulder.y) * k,
                       shoulder.z + (to.z - shoulder.z) * k};
        o = bow.Update(At(at, true), s);
    }
    EXPECT_EQ(o.event, W::Event::Nock);
    EXPECT_EQ(bow.State(), W::Phase::Nocked);
    for (int i = 1; i <= 27; ++i) {
        bow.Update(At(Drawn(0.08f + 0.32f * static_cast<float>(i) / 27.0f), true), s);
    }
    EXPECT_EQ(bow.Update(At(Drawn(0.40f), false), s).event, W::Event::Quick);
    EXPECT_EQ(bow.State(), W::Phase::Idle);
    // Taken, then let go on the way: still in the fingers; at the bow without a squeeze nothing,
    // with one nocked.
    W::Bow keep;
    keep.Update(At(shoulder, false), s);
    EXPECT_EQ(keep.Update(take, s).event, W::Event::Take);
    EXPECT_EQ(keep.Update(At({0.0f, 1.45f, -0.20f}, false), s).event, W::Event::None);
    EXPECT_EQ(keep.State(), W::Phase::Holding);
    EXPECT_EQ(keep.Update(At(Drawn(0.08f), false), s).event, W::Event::None);
    EXPECT_EQ(keep.State(), W::Phase::Holding);
    EXPECT_EQ(keep.Update(At(Drawn(0.08f), true), s).event, W::Event::Nock);
    // A squeeze held from before the shoulder (more than NockGraceS): no arrow.
    W::Bow late;
    late.Update(At(shoulder, false), s);
    for (int i = 0; i < 36; ++i) {
        late.Update(At({0.15f, 1.20f, -0.10f}, true), s);
    }
    EXPECT_EQ(late.Update(take, s).event, W::Event::None);
    EXPECT_EQ(late.State(), W::Phase::Idle);
}

// The bow comes out with an arrow in the string hand (bow_quiver): nocked at the bow and shot, the
// hand is empty (a squeeze at the bow: the soft no) until the next arrow from the quiver; an arrow
// put back by too little a draw stays in the fingers.
TEST(Bow, TheBowComesOutWithAnArrowReady) {
    const W::Settings s{};
    W::Bow bow;
    bow.Arm(false, true);
    EXPECT_EQ(bow.State(), W::Phase::Holding);
    bow.Update(At({0.10f, 1.30f, -0.10f}, false), s); // carried, the trigger let go
    EXPECT_EQ(bow.State(), W::Phase::Holding);
    EXPECT_EQ(bow.Update(At(Drawn(0.08f), true), s).event, W::Event::Nock);
    EXPECT_EQ(bow.Update(At(Drawn(0.10f), false), s).event, W::Event::PutBack); // too little
    EXPECT_EQ(bow.State(), W::Phase::Holding); // ... back in the fingers
    EXPECT_EQ(bow.Update(At(Drawn(0.08f), true), s).event, W::Event::Nock);
    for (int i = 1; i <= 27; ++i) {
        bow.Update(At(Drawn(0.08f + 0.32f * static_cast<float>(i) / 27.0f), true), s);
    }
    EXPECT_EQ(bow.Update(At(Drawn(0.40f), false), s).event, W::Event::Quick);
    EXPECT_EQ(bow.State(), W::Phase::Idle); // shot: the hand empty
    EXPECT_EQ(bow.Update(At(Drawn(0.08f), true), s).event, W::Event::Empty);
    bow.Update(At({0.15f, 1.55f, 0.10f}, false), s);
    W::Input take = At({0.15f, 1.55f, 0.10f}, true);
    take.at_quiver = true;
    EXPECT_EQ(bow.Update(take, s).event, W::Event::Take);
    // A hand lost on a short draw and let go: put back, still in the fingers.
    W::Bow lost;
    lost.Arm(false, true);
    lost.Update(At(Drawn(0.08f), true), s);
    lost.Update(At(Drawn(0.10f), true, false), s);
    EXPECT_EQ(lost.Update(At(Drawn(0.10f), false, false), s).event, W::Event::PutBack);
    EXPECT_EQ(lost.State(), W::Phase::Holding);
    // Without the quiver the bow comes out empty (the arrow comes at the bow) and an arrow put
    // back is gone.
    const W::Settings plain = Plain();
    W::Bow none;
    none.Arm(false, plain.quiver);
    EXPECT_EQ(none.State(), W::Phase::Idle);
    EXPECT_EQ(none.Update(At(Drawn(0.08f), true), plain).event, W::Event::Nock);
    EXPECT_EQ(none.Update(At(Drawn(0.10f), false), plain).event, W::Event::PutBack);
    EXPECT_EQ(none.State(), W::Phase::Idle);
}

// The wall: full draw reached once per arrow (the third's tick that frame too); the hand on the
// string stays on the arrow's line, never past full draw nor in front of the string at rest.
TEST(Bow, TheWallAndTheHandOnTheString) {
    const W::Settings s = Plain();
    W::Bow bow;
    W::Output o = bow.Update(At(Drawn(0.08f), true), s);
    ASSERT_EQ(o.event, W::Event::Nock);
    int walls = 0;
    for (int i = 1; i <= 45; ++i) { // to 0.75 m: past full draw (0.60)
        o = bow.Update(At(Drawn(0.08f + 0.67f * static_cast<float>(i) / 45.0f), true), s);
        walls += o.wall ? 1 : 0;
        if (o.wall) {
            EXPECT_TRUE(o.detent);
            EXPECT_FLOAT_EQ(o.draw, 1.0f);
        }
    }
    for (int i = 0; i < 20; ++i) { // eased back below full, then full again: no second wall
        o = bow.Update(At(Drawn(i < 10 ? 0.50f : 0.70f), true), s);
        walls += o.wall ? 1 : 0;
    }
    EXPECT_EQ(walls, 1);
    const float rest = bow.RestM(s);
    EXPECT_NEAR(rest, 0.12f, 1e-6f); // nocked at 8 cm: the string at rest_m
    const auto dist = [](W::V3 a, W::V3 b) { return W::Length(W::Sub(a, b)); };
    const W::V3 past = W::NockPoint(BowAt, Drawn(0.75f), rest, s.draw_m);
    EXPECT_NEAR(dist(past, BowAt), s.draw_m, 1e-5f); // the controller past full draw
    EXPECT_NEAR(past.x, BowAt.x, 1e-6f);              // ... the hand on the arrow's line
    EXPECT_NEAR(past.y, BowAt.y, 1e-6f);
    const W::V3 mid = W::NockPoint(BowAt, Drawn(0.35f), rest, s.draw_m);
    EXPECT_NEAR(dist(mid, Drawn(0.35f)), 0.0f, 1e-6f); // within the draw: the controller itself
    const W::V3 in = W::NockPoint(BowAt, Drawn(0.05f), rest, s.draw_m);
    EXPECT_NEAR(dist(in, BowAt), rest, 1e-5f); // pushed in: the string at rest
    const W::V3 side{BowAt.x + 0.5f, BowAt.y, BowAt.z + 0.5f}; // off to the side, too far
    const W::V3 off = W::NockPoint(BowAt, side, rest, s.draw_m);
    EXPECT_NEAR(dist(off, BowAt), s.draw_m, 1e-5f);
    EXPECT_NEAR((off.x - BowAt.x) / (off.z - BowAt.z), 1.0f, 1e-5f); // along the hands' line
    // Nocked as far out as the draw (a short bow_draw_cm, a long bow_nock_cm): the hand still
    // goes to where the draw is full (5 cm past the rest), not short of it.
    const W::V3 deep = W::NockPoint(BowAt, Drawn(0.60f), 0.30f, 0.30f);
    EXPECT_NEAR(dist(deep, BowAt), 0.35f, 1e-5f);
}

// The string rests where the arrow was nocked: nocked at bow_nock_cm and let go right there, it
// goes back (no weak shot); drawn on from there, it shoots as before.
TEST(Bow, ReleasedWhereItWasNockedTheArrowGoesBack) {
    const W::Settings s = Plain();
    W::Bow bow;
    bow.Update(At(Drawn(0.199f), false), s);
    EXPECT_EQ(bow.Update(At(Drawn(0.199f), true), s).event, W::Event::Nock);
    EXPECT_EQ(bow.Update(At(Drawn(0.199f), false), s).event, W::Event::PutBack);
    W::Bow on;
    on.Update(At(Drawn(0.199f), false), s);
    on.Update(At(Drawn(0.199f), true), s);
    for (int i = 1; i <= 27; ++i) {
        on.Update(At(Drawn(0.199f + 0.201f * static_cast<float>(i) / 27.0f), true), s);
    }
    EXPECT_EQ(on.Update(At(Drawn(0.40f), false), s).event, W::Event::Quick);
}

// The bow is the hand's: it turns with the wrist, held as when shooting. Nocking never turns it,
// drawing eases it onto the arrow, and the grip of the full draw is kept after the release.
TEST(Bow, TheBowTurnsWithTheWristAndNeverFlipsAtTheNockOrTheRelease) {
    namespace B = VR::Body;
    W::BowHold hold;
    const B::Vec3 rest{-0.15f, 1.45f, -0.55f};
    const auto pointing = [](B::Quat g) { return B::Rotate(g, {-0.7071f, -0.7071f, 0}); };
    // Idle: rigid to the fist (a turned wrist turns the bow by the same rotation).
    const B::Quat g0 = B::AxisAngle({0, 1, 0}, 0.3f);
    const B::Quat turn = B::AxisAngle(B::Normalize(B::Vec3{1, 0.4f, 0.2f}), 0.9f);
    const B::Quat idle0 = W::HeldBow(hold, g0, pointing(g0), false, rest, {});
    const B::Quat idle1 = W::HeldBow(hold, turn * g0, pointing(turn * g0), false, rest, {});
    EXPECT_LT(B::AngleBetween(idle1, B::Normalize(turn * idle0)), 1e-3f);
    // Nocked with the string hand beside the bow (any line): exactly the idle hold.
    for (B::Vec3 at : {B::Vec3{0.05f, 1.35f, -0.45f}, B::Vec3{-0.3f, 1.6f, -0.5f}}) {
        EXPECT_LT(B::AngleBetween(W::HeldBow(hold, g0, pointing(g0), true, rest, at), idle0), 1e-4f);
    }
    // Drawn to the cheek: along the arrow, smoothly all the way.
    const B::Vec3 cheek{0.08f, 1.52f, 0.0f};
    B::Quat last = idle0;
    for (int i = 0; i <= 60; ++i) {
        const B::Vec3 at = rest + B::Normalize(cheek - rest) * (0.2f + 0.35f * i / 60.0f);
        const B::Quat now = W::HeldBow(hold, g0, pointing(g0), true, rest, at);
        EXPECT_LT(B::AngleBetween(now, last), 8.0f * B::Deg); // gradual: no flip
        last = now;
    }
    const B::Quat drawn = W::HeldBow(hold, g0, pointing(g0), true, rest, cheek);
    EXPECT_NEAR(B::Dot(B::Rotate(drawn, {0, -1, 0}), B::Normalize(rest - cheek)), 1, 1e-5f);
    ASSERT_TRUE(hold.learned);
    // Released: the full draw's grip stays, and from then on turns with the wrist.
    EXPECT_LT(B::AngleBetween(W::HeldBow(hold, g0, pointing(g0), false, rest, {}), drawn), 1e-4f);
    EXPECT_LT(B::AngleBetween(W::HeldBow(hold, turn * g0, pointing(turn * g0), false, rest, {}),
                              B::Normalize(turn * drawn)), 1e-3f);
}

// The string of a braced bow sits about 20 cm off the grip: a squeeze there, or a little behind
// it, nocks; well beyond the bow it does not (bow_nock_cm 27).
TEST(Bow, TheNockZoneReachesRoundTheString) {
    const W::Settings s = Plain();
    for (float d : {0.18f, 0.22f, 0.26f}) {
        W::Bow bow;
        bow.Update(At(Drawn(d), false), s);
        EXPECT_EQ(bow.Update(At(Drawn(d), true), s).event, W::Event::Nock) << d;
    }
    W::Bow far;
    far.Update(At(Drawn(0.30f), false), s);
    EXPECT_NE(far.Update(At(Drawn(0.30f), true), s).event, W::Event::Nock);
}

// The arrow rests beside the bow fist: a little up the limb and across to the string hand's side
// (whatever part of that direction runs along the limb is ignored); none known: just above.
TEST(Bow, TheArrowRestIsBesideTheBowFistOnTheStringSide) {
    namespace B = VR::Body;
    const B::Vec3 grip{0.1f, 1.2f, -0.4f};
    for (float roll : {0.0f, 0.8f, 2.0f}) {
        const B::Vec3 up = B::Rotate(B::AxisAngle({0, 0, 1}, roll), {0, 1, 0});
        const B::Vec3 right = B::Normalize(B::Vec3{1, 0.3f, 0});
        const B::Vec3 rest = W::ArrowRest(grip, up, right);
        EXPECT_NEAR(B::Dot(rest - grip, up), W::ArrowRestUpM, 1e-5f);
        const B::Vec3 across = (rest - grip) - up * W::ArrowRestUpM;
        EXPECT_NEAR(B::Length(across), W::ArrowRestSideM, 1e-5f);
        EXPECT_GT(B::Dot(across, right), 0.0f);
    }
    const B::Vec3 plain = W::ArrowRest(grip, {0, 1, 0}, {});
    EXPECT_NEAR(B::Length(plain - grip - B::Vec3{0, W::ArrowRestUpM, 0}), 0, 1e-6f);
}

// The quiver is over a shoulder, behind the ears: a cheek or ear anchor at full draw is not it.
TEST(Bow, TheQuiverIsBehindTheEars) {
    EXPECT_TRUE(W::AtQuiver(-0.20f, 0.05f, 0.15f));   // over the right shoulder
    EXPECT_TRUE(W::AtQuiver(-0.15f, -0.20f, -0.25f)); // ... or the left, lower
    EXPECT_TRUE(W::AtQuiver(-0.08f, 0.0f, 0.20f));    // just behind the ear (nearer, 2026-10-13)
    EXPECT_TRUE(W::AtQuiver(-0.20f, -0.38f, 0.47f));  // roomier: lower and further out
    EXPECT_TRUE(W::AtQuiver(-0.20f, -0.41f, -0.51f)); // ... 2 cm more (2026-10-14)
    EXPECT_FALSE(W::AtQuiver(-0.05f, -0.10f, 0.10f)); // the cheek anchor at full draw
    EXPECT_FALSE(W::AtQuiver(-0.20f, -0.45f, 0.15f)); // behind the back, too low
    EXPECT_FALSE(W::AtQuiver(-0.20f, 0.05f, 0.60f));  // out to the side
}

TEST(Bow, SettingsParse) {
    W::Settings& s = W::GetBowSettings();
    const W::Settings saved = s;
    EXPECT_TRUE(W::ParseBowSetting("bow_draw_cm", "70"));
    EXPECT_NEAR(s.draw_m, 0.70f, 1e-5f);
    EXPECT_TRUE(W::ParseBowSetting("bow_draw_cm", "500"));
    EXPECT_NEAR(s.draw_m, 0.90f, 1e-5f); // clamped
    EXPECT_TRUE(W::ParseBowSetting("bow", "0"));
    EXPECT_FALSE(s.enabled);
    EXPECT_TRUE(W::ParseBowSetting("bow_direct", "0"));
    EXPECT_FALSE(s.direct);
    EXPECT_TRUE(W::ParseBowSetting("bow_twang", "5130"));
    EXPECT_EQ(s.twang, 5130);
    EXPECT_TRUE(W::ParseBowSetting("bow_charge_ms", "800"));
    EXPECT_NEAR(s.charge_s, 0.8f, 1e-5f);
    EXPECT_TRUE(W::ParseBowSetting("bow_quiver", "0"));
    EXPECT_FALSE(s.quiver);
    EXPECT_FALSE(W::ParseBowSetting("bowl", "1"));
    s = saved;
}

} // namespace
