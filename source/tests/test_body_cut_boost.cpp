// SPDX-License-Identifier: GPL-2.0-or-later

// vr.ini body_cutaway_boost (core/vr/body_cut_boost.h): a hit, stagger or kept dodge fades the
// hunter's near body out more strongly (level 2: 11 .. 35 cm, the inside never drawn), the camera
// behind or below his eye halfway (level 1); each level held 0.4 s; never weaker than the normal
// cutaway; only three cut values.

#include <cmath>
#include <cstdint>
#include <set>
#include <tuple>

#include <gtest/gtest.h>

#include "core/vr/body_cut_boost.h"
#include "core/vr/body_settings.h"

namespace {

using VR::Body::BoostedRadii;
using VR::Body::CutBoost;
using VR::Body::CutBoostInput;
using VR::Body::CutBoostParams;
using VR::Body::CutBoostWhy;
using VR::Body::CutRadii;

constexpr std::int64_t Ms = 1'000'000;

CutBoostInput Calm() {
    CutBoostInput in{};
    in.fresh = true;
    in.cam_ahead_m = 0.0f;
    in.cam_above_m = -0.02f; // standing: the camera 2 cm under his eye (the median)
    return in;
}

TEST(BodyCutBoost, CalmPlayKeepsTheNormalCutaway) {
    CutBoost boost;
    const CutBoostParams p{};
    for (std::int64_t t = 1; t < 5000 * Ms; t += 11 * Ms) {
        CutBoostInput in = Calm();
        // walking: the camera 3-20 cm above his eye, a little ahead or behind
        in.cam_above_m = 0.03f + 0.17f * static_cast<float>((t / Ms) % 100) / 100.0f;
        in.cam_ahead_m = 0.035f * std::sin(static_cast<float>(t / Ms) * 0.01f);
        ASSERT_EQ(boost.Update(in, t, p), 0u) << t / Ms << " ms";
    }
    EXPECT_EQ(boost.Why(), CutBoostWhy::None);
}

TEST(BodyCutBoost, AHitBoostsFullyAndHolds) {
    CutBoost boost;
    const CutBoostParams p{};
    EXPECT_EQ(boost.Update(Calm(), 100 * Ms, p), 0u);
    CutBoostInput hit = Calm();
    hit.hit = true;
    EXPECT_EQ(boost.Update(hit, 116 * Ms, p), 2u);
    EXPECT_EQ(boost.Why(), CutBoostWhy::Hit);
    EXPECT_STREQ(CutBoost::WhyText(boost.Why()), "a hit, stagger or dodge");
    EXPECT_EQ(boost.Update(hit, 600 * Ms, p), 2u); // the clip goes on
    // The clip ends at 600 ms: held 0.4 s more.
    EXPECT_EQ(boost.Update(Calm(), 616 * Ms, p), 2u);
    EXPECT_EQ(boost.Update(Calm(), 999 * Ms, p), 2u);
    EXPECT_EQ(boost.Update(Calm(), 1001 * Ms, p), 0u);
}

TEST(BodyCutBoost, TheCameraBehindOrBelowHisEyeBoostsHalfway) {
    CutBoost boost;
    const CutBoostParams p{};
    CutBoostInput back = Calm();
    back.cam_ahead_m = -0.039f; // just inside the threshold
    EXPECT_EQ(boost.Update(back, 10 * Ms, p), 0u);
    back.cam_ahead_m = -0.05f; // a head 5 cm behind his eye (he leans back at most 5 degrees)
    EXPECT_EQ(boost.Update(back, 20 * Ms, p), 1u);
    EXPECT_EQ(boost.Why(), CutBoostWhy::Behind);
    EXPECT_EQ(boost.Update(Calm(), 419 * Ms, p), 1u);
    EXPECT_EQ(boost.Update(Calm(), 421 * Ms, p), 0u);
    CutBoostInput low = Calm();
    low.cam_above_m = -0.06f; // a small head drop inside the 8 cm deadband
    EXPECT_EQ(boost.Update(low, 1000 * Ms, p), 1u);
    EXPECT_EQ(boost.Why(), CutBoostWhy::Below);
    // A hit on top of it: the full level, back to half after its hold while the head stays low.
    CutBoostInput both = low;
    both.hit = true;
    EXPECT_EQ(boost.Update(both, 1100 * Ms, p), 2u);
    EXPECT_EQ(boost.Update(low, 1200 * Ms, p), 2u);
    EXPECT_EQ(boost.Update(low, 1501 * Ms, p), 1u);
    EXPECT_EQ(boost.Update(Calm(), 1900 * Ms, p), 1u);
    EXPECT_EQ(boost.Update(Calm(), 1902 * Ms, p), 0u);
}

TEST(BodyCutBoost, AStaleStatusTriggersNothing) {
    // Loading, third person, the body off: no new boost; an earlier one runs out.
    CutBoost boost;
    const CutBoostParams p{};
    CutBoostInput stale{};
    stale.hit = true;
    stale.cam_ahead_m = -0.3f;
    stale.cam_above_m = -0.3f;
    EXPECT_EQ(boost.Update(stale, 10 * Ms, p), 0u);
    CutBoostInput hit = Calm();
    hit.hit = true;
    EXPECT_EQ(boost.Update(hit, 20 * Ms, p), 2u);
    EXPECT_EQ(boost.Update(stale, 300 * Ms, p), 2u);
    EXPECT_EQ(boost.Update(stale, 421 * Ms, p), 0u);
}

TEST(BodyCutBoost, ZeroHoldFollowsTheCauseOnly) {
    CutBoost boost;
    CutBoostParams p{};
    p.hold_s = 0.0f;
    CutBoostInput hit = Calm();
    hit.hit = true;
    EXPECT_EQ(boost.Update(hit, 10 * Ms, p), 2u);
    EXPECT_EQ(boost.Update(Calm(), 11 * Ms, p), 0u);
}

TEST(BodyCutBoost, LadderMountBoostsForThreeSecondsFromTheActualStart) {
    CutBoost boost;
    const CutBoostParams p{};
    CutBoostInput in{}; // ladder animations can release body IK: no fresh body status needed
    in.ladder_start_ns = 100 * Ms;
    EXPECT_EQ(boost.Update(in, 100 * Ms, p), 2u);
    EXPECT_EQ(boost.Why(), CutBoostWhy::Ladder);
    EXPECT_STREQ(CutBoost::WhyText(boost.Why()), "starting a ladder climb");
    EXPECT_EQ(boost.Update(in, 3099 * Ms, p), 2u);
    EXPECT_EQ(boost.Update(in, 3100 * Ms, p), 0u);
    EXPECT_EQ(boost.Update(in, 6000 * Ms, p), 0u); // more rungs do not extend the window

    CutBoost late_renderer;
    EXPECT_EQ(late_renderer.Update(in, 3000 * Ms, p), 2u);
    EXPECT_EQ(late_renderer.Update(in, 3100 * Ms, p), 0u); // not three seconds from first draw
    EXPECT_EQ(late_renderer.Update(in, 99 * Ms, p), 0u); // future timestamp is not a mount
}

TEST(BodyCutBoost, DismountEndsLadderFadeImmediately) {
    CutBoost boost;
    CutBoostParams p{};
    p.recovery_s = 0.2f; // no ladder hold or recovery after getting off
    CutBoostInput in = Calm();
    in.ladder_start_ns = 100 * Ms;
    EXPECT_EQ(boost.Update(in, 100 * Ms, p), 2u);
    in.ladder_start_ns = 0;
    EXPECT_EQ(boost.Update(in, 200 * Ms, p), 0u);
    EXPECT_EQ(boost.Why(), CutBoostWhy::None);
}

TEST(BodyCutBoost, FlatScreenCancelsThisClimbButNotTheNextOne) {
    CutBoost boost;
    const CutBoostParams p{};
    CutBoostInput in = Calm();
    in.ladder_start_ns = 100 * Ms;
    EXPECT_EQ(boost.Update(in, 100 * Ms, p), 2u);
    boost.CancelLadder(in.ladder_start_ns);
    boost.CancelLadder(0); // an older timestamp cannot forget the cancellation
    EXPECT_EQ(boost.Update(in, 200 * Ms, p), 0u); // even a quick return to VR stays clear
    EXPECT_EQ(boost.Update(in, 300 * Ms, p), 0u);
    in.ladder_start_ns = 500 * Ms;
    EXPECT_EQ(boost.Update(in, 500 * Ms, p), 2u);
    EXPECT_EQ(boost.Update(in, 3500 * Ms, p), 0u);

    CutBoost first_seen_on_screen;
    first_seen_on_screen.CancelLadder(in.ladder_start_ns);
    EXPECT_EQ(first_seen_on_screen.Update(in, 600 * Ms, p), 0u);
}

TEST(BodyCutBoost, FlatScreenCancellationDoesNotNeedAFreshLadderSample) {
    CutBoost boost;
    const CutBoostParams p{};
    CutBoostInput in = Calm();
    in.ladder_start_ns = 100 * Ms;
    EXPECT_EQ(boost.Update(in, 200 * Ms, p), 2u);
    // The flat frame knows its own time even if the player update is stale or unreadable.
    boost.CancelLadder(500 * Ms);
    EXPECT_EQ(boost.Update(in, 600 * Ms, p), 0u);

    // A mount first published after that frame still started while the screen was active.
    in.ladder_start_ns = 450 * Ms;
    EXPECT_EQ(boost.Update(in, 600 * Ms, p), 0u);
    boost.CancelLadder(400 * Ms); // an older observation cannot undo cancellation
    EXPECT_EQ(boost.Update(in, 610 * Ms, p), 0u);
    in.ladder_start_ns = 700 * Ms;
    EXPECT_EQ(boost.Update(in, 700 * Ms, p), 2u); // a genuinely later mount can fade again
}

TEST(BodyCutBoost, LadderAndHitProtectionDoNotShortenEachOther) {
    CutBoost boost;
    const CutBoostParams p{};
    CutBoostInput in = Calm();
    in.ladder_start_ns = 100 * Ms;
    in.hit = true;
    EXPECT_EQ(boost.Update(in, 100 * Ms, p), 2u);
    in.hit = false;
    EXPECT_EQ(boost.Update(in, 600 * Ms, p), 2u); // hit hold expired, ladder still boosted
    in.hit = true;
    EXPECT_EQ(boost.Update(in, 3000 * Ms, p), 2u);
    in.hit = false;
    EXPECT_EQ(boost.Update(in, 3100 * Ms, p), 2u); // ladder expired, hit still held
    EXPECT_EQ(boost.Why(), CutBoostWhy::Hit);
    EXPECT_EQ(boost.Update(in, 3401 * Ms, p), 0u);

    in.ladder_start_ns = 4000 * Ms;
    in.hit = true;
    EXPECT_EQ(boost.Update(in, 4000 * Ms, p), 2u);
    boost.CancelLadder(in.ladder_start_ns);
    in.ladder_start_ns = 0;
    in.hit = false;
    EXPECT_EQ(boost.Update(in, 4100 * Ms, p), 2u); // cancel/dismount keeps ordinary protection
    EXPECT_EQ(boost.Why(), CutBoostWhy::Hit);
    EXPECT_EQ(boost.Update(in, 4401 * Ms, p), 0u);
}

TEST(BodyCutBoost, RadiiNeverWeakerAndOnlyThree) {
    const VR::Body::BodySettings d{};
    const CutRadii base{d.body_cutaway_near_m, d.body_cutaway_far_m, d.body_cutaway_inside};
    EXPECT_TRUE(d.body_cutaway_boost);
    EXPECT_FLOAT_EQ(d.body_cutaway_boost_near_m, 0.11f);
    EXPECT_FLOAT_EQ(d.body_cutaway_boost_far_m, 0.35f);
    const CutRadii l0 = BoostedRadii(base, d.body_cutaway_boost_near_m,
                                     d.body_cutaway_boost_far_m, 0);
    const CutRadii l1 = BoostedRadii(base, d.body_cutaway_boost_near_m,
                                     d.body_cutaway_boost_far_m, 1);
    const CutRadii l2 = BoostedRadii(base, d.body_cutaway_boost_near_m,
                                     d.body_cutaway_boost_far_m, 2);
    EXPECT_FLOAT_EQ(l0.near_m, 0.03f);
    EXPECT_FLOAT_EQ(l0.far_m, 0.22f);
    EXPECT_TRUE(l0.inside);
    EXPECT_FLOAT_EQ(l1.near_m, 0.07f);
    EXPECT_FLOAT_EQ(l1.far_m, 0.285f);
    EXPECT_FALSE(l1.inside);
    EXPECT_FLOAT_EQ(l2.near_m, 0.11f);
    EXPECT_FLOAT_EQ(l2.far_m, 0.35f);
    EXPECT_FALSE(l2.inside);
    // Level 2 is past the game camera's 10 cm near clip: no partly kept ring cut open there.
    EXPECT_GT(l2.near_m, 0.10f);
    // How much of the body is kept (the shader: smoothstep(near, far, depth)) at the near clip and
    // at a shoulder 15 / 22 cm away: normal 31% / 69% / 100%, level 2 0% / 7% / 44%.
    const auto keep = [](const CutRadii& r, float d) {
        const float t = std::clamp((d - r.near_m) / (r.far_m - r.near_m), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    EXPECT_NEAR(keep(l0, 0.10f), 0.307f, 0.01f);
    EXPECT_NEAR(keep(l0, 0.15f), 0.69f, 0.01f);
    EXPECT_NEAR(keep(l2, 0.10f), 0.0f, 1e-6f);
    EXPECT_NEAR(keep(l2, 0.15f), 0.07f, 0.01f);
    EXPECT_NEAR(keep(l2, 0.22f), 0.44f, 0.01f);
    // A user's stronger normal fade is never made weaker by the boost.
    const CutRadii strong{0.15f, 0.40f, true};
    const CutRadii s2 = BoostedRadii(strong, 0.11f, 0.35f, 2);
    EXPECT_FLOAT_EQ(s2.near_m, 0.15f);
    EXPECT_FLOAT_EQ(s2.far_m, 0.40f);
    const CutRadii s1 = BoostedRadii(strong, 0.11f, 0.35f, 1);
    EXPECT_GE(s1.near_m, strong.near_m);
    EXPECT_GE(s1.far_m, strong.far_m);
    // Exactly three distinct cuts.
    std::set<std::tuple<float, float, bool>> cuts;
    for (std::uint32_t level = 0; level < 3; ++level) {
        const CutRadii r = BoostedRadii(base, 0.11f, 0.35f, level);
        cuts.insert({r.near_m, r.far_m, r.inside});
    }
    EXPECT_EQ(cuts.size(), 3u);
}

} // namespace

TEST(CutBoostComfort, LateralProtectionAndStagedReturnUseClockTime) {
    VR::Body::CutBoost boost;
    VR::Body::CutBoostParams p;
    p.side_m = .055f;
    p.recovery_s = .2f;
    VR::Body::CutBoostInput in;
    in.fresh = true;
    in.cam_side_m = .07f;
    EXPECT_EQ(boost.Update(in, 1'000'000'000, p), 1u);
    in.hit = true;
    EXPECT_EQ(boost.Update(in, 1'100'000'000, p), 2u);
    in.hit = false;
    in.cam_side_m = 0;
    EXPECT_EQ(boost.Update(in, 1'499'000'000, p), 2u);
    EXPECT_EQ(boost.Update(in, 1'501'000'000, p), 1u);
    EXPECT_EQ(boost.Update(in, 1'701'000'000, p), 0u);
}
TEST(CutBoostComfort, DisabledLateralPreservesLegacyBehaviour) {
    VR::Body::CutBoost boost;
    VR::Body::CutBoostParams p;
    VR::Body::CutBoostInput in;
    in.fresh = true;
    in.cam_side_m = .2f;
    EXPECT_EQ(boost.Update(in, 1'000'000'000, p), 0u);
}
