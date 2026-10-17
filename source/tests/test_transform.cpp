// SPDX-License-Identifier: GPL-2.0-or-later

// The weapon-only transform (core/vr/transform.h, core/vr/transform_clips.h): the clip table's
// consistency, which clip a squeeze plays (or why L1 stays), the clock (speed, the skipped still
// lead-in), the cues (each sound and one-shot effect exactly once whatever the frame steps, those
// of a skipped lead-in at the start, held effects only inside their window) and the squeeze latch
// (wait, start, refuse after the buffer, a new squeeze replacing one still waiting).

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <utility>

#include <gtest/gtest.h>

#include "core/vr/transform.h"

namespace {

namespace T = VR::Transform;

const T::Clip* Find(int family, bool to_b) {
    for (const T::Clip& c : T::Clips) {
        if (c.family == family && c.to_b == to_b) {
            return &c;
        }
    }
    return nullptr;
}

// ---- The table ----------------------------------------------------------------------------------

TEST(TransformClips, NativeRowsAreConsistent) {
    for (const T::Clip& c : T::Clips) {
        if (c.support == T::Support::Vanilla) {
            EXPECT_NE(c.why, nullptr) << c.family;
            continue;
        }
        EXPECT_TRUE(c.why == nullptr) << c.family;
        EXPECT_GT(c.length, 0.3f) << c.family;
        EXPECT_GT(c.t_style, 0.0f) << c.family;
        EXPECT_LT(c.t_style, c.length) << c.family;
        EXPECT_GE(c.clip, 19000) << c.family;
        EXPECT_LE(c.clip, 19999) << c.family;
        EXPECT_GE(c.still, 0.0f) << c.family;
        EXPECT_LE(c.still, c.t_style + 1e-4f) << c.family;
        if (c.to_b) {
            EXPECT_TRUE(T::IsFormB(c.style)) << c.family << " A->B sets " << c.style;
        } else {
            EXPECT_EQ(c.style, T::StyleFormA) << c.family;
        }
        EXPECT_LE(c.cue_count, c.cues.size());
        for (std::size_t i = 0; i < c.cue_count; ++i) {
            const T::Cue& q = c.cues[i];
            EXPECT_GE(q.t0, 0.0f) << c.family;
            EXPECT_LE(q.t0, q.t1) << c.family;
            EXPECT_LE(q.t0, c.length + 1e-3f) << c.family;
            EXPECT_GT(q.a, 0) << c.family;
        }
    }
}

TEST(TransformClips, OneRowPerFamilyAndDirection) {
    std::set<std::pair<int, bool>> seen;
    for (const T::Clip& c : T::Clips) {
        EXPECT_TRUE(seen.insert({c.family, c.to_b}).second) << c.family << " " << c.to_b;
    }
}

TEST(TransformClips, TheKitWeaponsAreNativeBothWays) {
    // Saw Cleaver 70, Hunter Axe 50, Threaded Cane 220 (the research's verified clip data).
    struct Want {
        int family;
        bool to_b;
        int clip;
        float length;
        int style;
        float t_style;
    };
    const Want wants[] = {
        {70, true, 19000, 1.167f, 5, 0.467f},  {70, false, 19010, 1.167f, 1, 0.467f},
        {50, true, 19040, 1.233f, 3, 0.600f},  {50, false, 19250, 1.400f, 1, 0.567f},
        {220, true, 19150, 1.167f, 5, 0.267f}, {220, false, 19300, 1.333f, 1, 0.500f},
    };
    for (const Want& w : wants) {
        const T::Clip* c = Find(w.family, w.to_b);
        ASSERT_NE(c, nullptr) << w.family;
        EXPECT_NE(c->support, T::Support::Vanilla) << w.family;
        EXPECT_TRUE(c->weapon_anim) << w.family;
        EXPECT_EQ(c->clip, w.clip);
        EXPECT_NEAR(c->length, w.length, 1e-3f);
        EXPECT_EQ(c->style, w.style);
        EXPECT_NEAR(c->t_style, w.t_style, 1e-3f);
    }
    // The cleaver's mechanical sounds (c602 / c600 / c601) and its effect on the folded blade.
    const T::Clip* cleaver = Find(70, true);
    ASSERT_NE(cleaver, nullptr);
    int sounds = 0;
    int effects = 0;
    for (std::size_t i = 0; i < cleaver->cue_count; ++i) {
        (cleaver->cues[i].kind == T::CueKind::Sound ? sounds : effects) += 1;
    }
    EXPECT_EQ(sounds, 4);
    EXPECT_EQ(effects, 1);
    EXPECT_EQ(cleaver->cues[4].a, 7110);
    EXPECT_EQ(cleaver->cues[4].b, 10110);
    EXPECT_EQ(cleaver->cues[4].c, -1);
}

TEST(TransformClips, GameplayClipsKeepL1) {
    // Chikage (SpEffect 204), Bloodletter (6160 / 6170), Kos-type bullets, fists: never native.
    for (const int family : {20, 290, 400, 410, 10, 380}) {
        for (const bool to_b : {true, false}) {
            const T::Clip* c = Find(family, to_b);
            if (c != nullptr) {
                EXPECT_EQ(c->support, T::Support::Vanilla) << family;
            }
        }
    }
    // The Boom Hammer has no B->A clip.
    const T::Clip* boom = Find(280, false);
    ASSERT_NE(boom, nullptr);
    EXPECT_EQ(boom->support, T::Support::Vanilla);
}

// ---- Which clip --------------------------------------------------------------------------------

TEST(TransformChoose, FormDecidesTheDirection) {
    const T::Choice a = T::Choose(70, T::StyleFormA, false);
    ASSERT_NE(a.clip, nullptr);
    EXPECT_EQ(a.clip->clip, 19000);
    const T::Choice b = T::Choose(70, T::StyleFormB, false);
    ASSERT_NE(b.clip, nullptr);
    EXPECT_EQ(b.clip->clip, 19010);
    const T::Choice axe_b = T::Choose(50, T::StyleFormBTwoHanded, false);
    ASSERT_NE(axe_b.clip, nullptr);
    EXPECT_EQ(axe_b.clip->clip, 19250);
    const T::Choice cane_a = T::Choose(220, T::StyleFormA, false);
    ASSERT_NE(cane_a.clip, nullptr);
    EXPECT_EQ(cane_a.clip->clip, 19150);
}

TEST(TransformChoose, WhyL1Stays) {
    EXPECT_EQ(T::Choose(-1, 1, true).clip, nullptr);
    EXPECT_EQ(T::Choose(140, 1, true).clip, nullptr); // the Hunter Pistol: not a trick weapon
    EXPECT_STREQ(T::Choose(140, 1, true).why, "not a trick weapon");
    EXPECT_EQ(T::Choose(70, T::StyleLeftTwoHanded, true).clip, nullptr);
    EXPECT_EQ(T::Choose(70, 0, true).clip, nullptr);
    EXPECT_EQ(T::Choose(70, 6, true).clip, nullptr);
    EXPECT_EQ(T::Choose(20, 1, true).clip, nullptr); // Chikage
    EXPECT_NE(T::Choose(20, 1, true).why, nullptr);
    EXPECT_EQ(T::Choose(280, 5, true).clip, nullptr); // Boom Hammer B->A
}

TEST(TransformChoose, UntestedRowsFollowTheSetting) {
    // (Every native row is tested today; a row added untested must follow transform_untested.)
    for (const T::Clip& c : T::Clips) {
        if (c.support != T::Support::Untested) {
            continue;
        }
        // From form A, or from the form B its A->B row sets.
        const T::Clip* ab = c.to_b ? &c : Find(c.family, true);
        ASSERT_NE(ab, nullptr);
        const std::int32_t from = c.to_b ? T::StyleFormA : ab->style;
        EXPECT_EQ(T::Choose(c.family, from, true).clip, &c) << c.family;
        EXPECT_EQ(T::Choose(c.family, from, false).clip, nullptr) << c.family;
    }
}

// ---- The clock ---------------------------------------------------------------------------------

TEST(TransformClock, SpeedSkipAndClamp) {
    const T::Clip& c = *Find(70, true);
    const T::Timing vanilla{1.0f, 0.0f};
    EXPECT_FLOAT_EQ(T::ClipTime(0.0f, c, vanilla), 0.0f);
    EXPECT_NEAR(T::ClipTime(0.5f, c, vanilla), 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(T::ClipTime(5.0f, c, vanilla), c.length);
    EXPECT_FLOAT_EQ(T::ClipTime(-1.0f, c, vanilla), 0.0f);
    EXPECT_NEAR(T::SecondsToSwitch(c, vanilla), 0.467f, 1e-4f);
    const T::Timing quick{1.5f, 0.2f};
    EXPECT_NEAR(T::ClipTime(0.0f, c, quick), 0.2f, 1e-6f);
    EXPECT_NEAR(T::ClipTime(0.1f, c, quick), 0.35f, 1e-5f);
    EXPECT_NEAR(T::SecondsToSwitch(c, quick), (0.467f - 0.2f) / 1.5f, 1e-4f);
    EXPECT_NEAR(T::SecondsToEnd(c, quick), (1.167f - 0.2f) / 1.5f, 1e-4f);
    // The skip never passes the switch (0.1 s of clip before it stays).
    const T::Timing too_far{1.0f, 0.4f};
    const T::Clip& cane = *Find(220, true); // switch at 0.267
    EXPECT_NEAR(T::SkipFor(cane, too_far), 0.167f, 1e-4f);
    EXPECT_LT(T::ClipTime(0.0f, cane, too_far), cane.t_style);
}

TEST(TransformClock, AutoSkipsTheMeasuredStillLeadIn) {
    const T::Timing automatic{}; // transform_skip_s=auto
    // The cleaver's blade starts moving at 0.20 s, the axe's at 0.47 s (switch 0.60), the cane's
    // form A has no clip (still to its switch at 0.267: capped 0.1 s before it).
    EXPECT_NEAR(T::SkipFor(*Find(70, true), automatic), 0.20f, 1e-4f);
    EXPECT_NEAR(T::SkipFor(*Find(50, true), automatic), 0.47f, 1e-4f);
    EXPECT_NEAR(T::SkipFor(*Find(220, true), automatic), 0.167f, 1e-4f);
    for (const T::Clip& c : T::Clips) {
        if (c.support == T::Support::Vanilla) {
            continue;
        }
        // The switch never earlier than 0.1 s after the squeeze, never later than the game's.
        EXPECT_LE(T::SecondsToSwitch(c, automatic), c.t_style + 1e-4f) << c.family;
        EXPECT_GE(T::SecondsToSwitch(c, automatic), std::min(0.1f, c.t_style) - 1e-4f) << c.family;
        EXPECT_LE(T::SecondsToSwitch(c, automatic), 0.9f) << c.family;
    }
}

// ---- Cues
// ----------------------------------------------------------------------------------------

// Steps a run through the whole clip with frames of `dt` seconds: how often each cue fired, and
// the times at which held effects were asked for.
struct CueRun {
    std::array<int, 8> fired{};
    std::array<int, 8> held{};
    std::array<float, 8> first_t{};
};

CueRun RunCues(const T::Clip& c, const T::Timing& tm, float dt) {
    CueRun out{};
    out.first_t.fill(-1.0f);
    const float start = T::ClipTime(0.0f, c, tm);
    std::uint32_t fired = 0;
    for (float e = 0.0f; e < T::SecondsToEnd(c, tm) + 0.1f; e += dt) {
        const float t = T::ClipTime(e, c, tm);
        const T::CueStep s = T::StepCues(c, start, t, fired);
        for (std::size_t i = 0; i < c.cue_count; ++i) {
            if ((s.fire >> i) & 1u) {
                ++out.fired[i];
                if (out.first_t[i] < 0.0f) {
                    out.first_t[i] = t;
                }
            }
            if ((s.held >> i) & 1u) {
                ++out.held[i];
                EXPECT_GE(t, c.cues[i].t0);
                EXPECT_LT(t, c.cues[i].t1);
            }
        }
    }
    return out;
}

TEST(TransformCues, EachOneShotOnceAtAnyFrameRate) {
    for (const T::Clip& c : T::Clips) {
        if (c.support == T::Support::Vanilla) {
            continue;
        }
        for (const float dt : {1.0f / 120.0f, 1.0f / 60.0f, 1.0f / 30.0f, 0.1f, 0.25f}) {
            const CueRun r = RunCues(c, {1.0f, 0.0f}, dt);
            for (std::size_t i = 0; i < c.cue_count; ++i) {
                if (T::IsHeld(c.cues[i])) {
                    continue;
                }
                EXPECT_EQ(r.fired[i], 1) << c.family << " cue " << i << " dt " << dt;
                // Never early, at most one frame late.
                EXPECT_GE(r.first_t[i], c.cues[i].t0 - 1e-5f);
                EXPECT_LE(r.first_t[i], c.cues[i].t0 + dt + 1e-4f);
            }
        }
    }
}

TEST(TransformCues, SkippedLeadInPlaysAtTheStart) {
    const T::Clip& c = *Find(70, true); // sounds at 0.233 0.300 0.333 0.367, effect at 0.433
    const CueRun r = RunCues(c, {1.0f, 0.31f}, 1.0f / 60.0f);
    for (std::size_t i = 0; i < c.cue_count; ++i) {
        EXPECT_EQ(r.fired[i], 1) << i;
    }
    EXPECT_NEAR(r.first_t[0], 0.31f, 1e-4f); // c4800 at 0.233: at the start
    EXPECT_NEAR(r.first_t[1], 0.31f, 1e-4f); // c602 at 0.300: at the start
    EXPECT_GE(r.first_t[2], 0.333f);         // c600 on time
    // The axe's unlatch sound (c740 at 0.40) is inside its still lead-in: it plays at the squeeze.
    const T::Clip& axe = *Find(50, true);
    const CueRun a = RunCues(axe, {}, 1.0f / 60.0f);
    EXPECT_EQ(a.fired[0], 1);
    EXPECT_NEAR(a.first_t[0], 0.47f, 1e-4f);
}

TEST(TransformCues, HeldEffectsOnlyInTheirWindow) {
    const T::Clip& c = *Find(220, true); // the cane's 7109 on slot 0, 0.500-0.833
    std::size_t held = c.cue_count;
    for (std::size_t i = 0; i < c.cue_count; ++i) {
        if (T::IsHeld(c.cues[i])) {
            held = i;
        }
    }
    ASSERT_LT(held, c.cue_count);
    const CueRun r = RunCues(c, {1.0f, 0.0f}, 1.0f / 60.0f);
    EXPECT_NEAR(r.held[held], (0.833f - 0.5f) * 60.0f, 2.0f);
    EXPECT_EQ(r.fired[held], 0);
}

// ---- The squeeze -------------------------------------------------------------------------------

constexpr std::int64_t Ms = 1'000'000;

TEST(TransformPull, StartsAtOnceWhenFree) {
    T::PullLatch l{};
    T::PullInput in{true, 1, 1000 * Ms, 1001 * Ms, 400 * Ms, true};
    EXPECT_TRUE(T::StepPull(l, in) == T::PullStep::Start);
    EXPECT_TRUE(T::StepPull(l, in) == T::PullStep::Idle); // the same squeeze never twice
}

TEST(TransformPull, WaitsThenStartsOrIsRefused) {
    T::PullLatch l{};
    T::PullInput in{true, 1, 1000 * Ms, 1000 * Ms, 400 * Ms, false};
    EXPECT_TRUE(T::StepPull(l, in) == T::PullStep::Wait);
    in.now = 1300 * Ms;
    EXPECT_TRUE(T::StepPull(l, in) == T::PullStep::Wait);
    in.can_start = true; // the dodge ended
    EXPECT_TRUE(T::StepPull(l, in) == T::PullStep::Start);

    T::PullLatch l2{};
    T::PullInput in2{true, 7, 1000 * Ms, 1000 * Ms, 400 * Ms, false};
    EXPECT_TRUE(T::StepPull(l2, in2) == T::PullStep::Wait);
    in2.now = 1401 * Ms;
    EXPECT_TRUE(T::StepPull(l2, in2) == T::PullStep::Deny);
    in2.can_start = true;
    EXPECT_TRUE(T::StepPull(l2, in2) == T::PullStep::Idle); // refused for good
}

TEST(TransformPull, StaleAndReplaced) {
    T::PullLatch l{};
    // First seen long after it was made (the game was loading): refused, not started late.
    T::PullInput in{true, 3, 1000 * Ms, 2000 * Ms, 400 * Ms, true};
    EXPECT_TRUE(T::StepPull(l, in) == T::PullStep::Deny);
    // A second squeeze while the first waits replaces it.
    T::PullLatch w{};
    T::PullInput a{true, 4, 3000 * Ms, 3000 * Ms, 400 * Ms, false};
    EXPECT_TRUE(T::StepPull(w, a) == T::PullStep::Wait);
    T::PullInput b{true, 5, 3200 * Ms, 3200 * Ms, 400 * Ms, false};
    EXPECT_TRUE(T::StepPull(w, b) == T::PullStep::Wait);
    EXPECT_EQ(w.pending, 5u);
    b.now = 3500 * Ms; // 500 ms after the first, 300 after the second: still waiting
    EXPECT_TRUE(T::StepPull(w, b) == T::PullStep::Wait);
    b.can_start = true;
    EXPECT_TRUE(T::StepPull(w, b) == T::PullStep::Start);
}

TEST(TransformPull, InterruptOnlyAfterTheLock) {
    const T::Clip& c = *Find(70, true);
    EXPECT_FALSE(T::Interruptible(c, 0.3f, false));
    EXPECT_FALSE(T::Interruptible(c, 0.5f, false)); // past the time, the switch not done yet
    EXPECT_FALSE(T::Interruptible(c, 0.5f, true));  // switched, not settled
    EXPECT_TRUE(T::Interruptible(c, 0.6f, true));
}

// ---- The flick ---------------------------------------------------------------------------------

TEST(TransformFlick, ThePlainClockIsTheOldOne) {
    // A plain squeeze's run clock gives the same clip times as before the flick existed.
    for (const T::Clip& c : T::Clips) {
        if (c.support == T::Support::Vanilla) {
            continue;
        }
        for (const T::Timing tm : {T::Timing{}, T::Timing{1.0f, 0.0f}, T::Timing{1.5f, 0.2f}}) {
            const T::Clock k = T::NormalClock(c, tm);
            for (const float e : {0.0f, 0.05f, 0.2f, 0.5f, 1.0f, 3.0f}) {
                EXPECT_NEAR(T::ClockTime(k, e, c), T::ClipTime(e, c, tm), 1e-5f) << c.family;
            }
            EXPECT_NEAR(T::ClockSecondsTo(k, c.t_style, c), T::SecondsToSwitch(c, tm), 1e-5f);
            EXPECT_NEAR(T::ClockSecondsTo(k, c.length, c), T::SecondsToEnd(c, tm), 1e-5f);
        }
    }
}

TEST(TransformFlick, StartsAtRestAndSwitchesWithinTheStroke) {
    const T::FlickTiming f{}; // x2.5 at least, the switch within 0.1 s
    for (const T::Clip& c : T::Clips) {
        if (c.support == T::Support::Vanilla) {
            continue;
        }
        // The flick start: never inside the still lead-in, never past the switch.
        EXPECT_GE(c.flick, c.still - 1e-4f) << c.family;
        EXPECT_LE(c.flick, c.t_style + 1e-4f) << c.family;
        const float start = T::FlickStart(c);
        EXPECT_GE(start, c.still - 1e-4f) << c.family;
        EXPECT_LE(start, c.t_style + 1e-4f) << c.family;
        const T::Clock k = T::FlickClock(c, f);
        EXPECT_NEAR(k.base_t, start, 1e-6f);
        EXPECT_GE(k.speed, f.speed - 1e-5f) << c.family;
        EXPECT_LE(k.speed, T::MaxClipSpeed) << c.family;
        // The switch within the lead, unless the clip would have to run faster than x4.
        const float to_switch = T::ClockSecondsTo(k, c.t_style, c);
        const float bound = std::max(f.lead_s, (c.t_style - start) / T::MaxClipSpeed);
        EXPECT_LE(to_switch, bound + 1e-4f) << c.family << " " << c.to_b;
        // Never later than a plain squeeze's switch.
        EXPECT_LE(to_switch, T::SecondsToSwitch(c, {}) + 1e-4f) << c.family << " " << c.to_b;
        // The rest of the clip (the lock settling) at the same pace: it ends.
        EXPECT_NEAR(T::ClockTime(k, 10.0f, c), c.length, 1e-6f);
    }
    // The kit's weapons (measured flick starts): the Saw Cleaver's A->B from 0.23 at x2.5 (the
    // blade swings out from its rest pose: switch after 95 ms), its B->A skips the unlatch
    // wiggle (0.44: 11 ms); the Hunter Axe's A->B from 0.483 (47 ms), its B->A moves from the
    // first frame (x4, the most: 0.14 s); the Threaded Cane's A->B switches at once, its B->A at
    // x4 (0.125 s).
    struct Want {
        int family;
        bool to_b;
        float start;
        float speed;
        float to_switch;
    };
    const Want wants[] = {
        {70, true, 0.230f, 2.5f, 0.0948f},  {70, false, 0.440f, 2.5f, 0.0108f},
        {50, true, 0.483f, 2.5f, 0.0468f},  {50, false, 0.0f, 4.0f, 0.1418f},
        {220, true, 0.267f, 2.5f, 0.0f},    {220, false, 0.0f, 4.0f, 0.125f},
    };
    for (const Want& w : wants) {
        const T::Clip& c = *Find(w.family, w.to_b);
        const T::Clock k = T::FlickClock(c, f);
        EXPECT_NEAR(k.base_t, w.start, 1e-3f) << w.family << " " << w.to_b;
        EXPECT_NEAR(k.speed, w.speed, 1e-2f) << w.family << " " << w.to_b;
        EXPECT_NEAR(T::ClockSecondsTo(k, c.t_style, c), w.to_switch, 1e-3f)
            << w.family << " " << w.to_b;
    }
}

// A hand transform (the presenter's StartNativeTransform with `hand`) runs the flick's clock: the
// form switches as the hands finish the gesture. A plain squeeze's would come 0.06 .. 0.10 s
// later, the Kirkhammer's draw 0.27 s (with the retract's default x1.75).
TEST(TransformFlick, HandTransformsSwitchAsTheHandsFinish) {
    const T::FlickTiming f{};
    T::Timing plain{};
    plain.retract = 1.75f;
    for (const int family : {40, 270, 80, 81, 260}) { // the dual blades, the sheathed swords
        for (const bool to_b : {true, false}) {
            const T::Clip* c = Find(family, to_b);
            ASSERT_NE(c, nullptr) << family << " " << to_b;
            EXPECT_NE(c->support, T::Support::Vanilla) << family;
            const float hand = T::ClockSecondsTo(T::FlickClock(*c, f), c->t_style, *c);
            const float squeeze = T::ClockSecondsTo(T::NormalClock(*c, plain), c->t_style, *c);
            EXPECT_LE(hand, 0.12f) << family << " " << to_b;
            EXPECT_LE(hand, squeeze + 1e-4f) << family << " " << to_b;
        }
    }
    const T::Clip& draw = *Find(80, false); // the Kirkhammer's: it moves from its first frame
    EXPECT_NEAR(T::ClockSecondsTo(T::NormalClock(draw, plain), draw.t_style, draw), 0.267f, 1e-3f);
    EXPECT_NEAR(T::ClockSecondsTo(T::FlickClock(draw, f), draw.t_style, draw), 0.117f, 1e-3f);
}

TEST(TransformFlick, ASwingJoiningARunSpeedsItUpNeverDown) {
    const T::FlickTiming f{};
    const T::Clip& c = *Find(70, true); // switch at 0.467
    // A plain squeeze's run at t=0.25 (x1): the rest at x2.5 at least, the switch within 0.1 s.
    const T::Clock k = T::FlickFrom(c, 0.25f, 1.0f, f);
    EXPECT_NEAR(k.base_t, 0.25f, 1e-6f);
    EXPECT_GE(k.speed, 2.5f);
    EXPECT_LE(T::ClockSecondsTo(k, c.t_style, c), 0.1f + 1e-4f);
    // Already faster than the flick's pace: kept.
    EXPECT_NEAR(T::FlickFrom(c, 0.25f, 3.0f, f).speed, 3.0f, 1e-6f);
    // Joined at the very start of a clip whose switch is far: as fast as allowed.
    const T::Clip& burial = *Find(51, false); // switch at 0.867
    EXPECT_NEAR(T::FlickFrom(burial, 0.0f, 1.0f, f).speed, T::MaxClipSpeed, 1e-6f);
    // Clamped inside the clip.
    EXPECT_NEAR(T::FlickFrom(c, 5.0f, 1.0f, f).base_t, c.length, 1e-6f);
    EXPECT_NEAR(T::FlickFrom(c, -1.0f, 1.0f, f).base_t, 0.0f, 1e-6f);
}

// ---- The retract's quick start (transform_retract_speed) -------------------------------------

TEST(TransformRetract, OneIsTheGamesTimingAndTheExtendNeverChanges) {
    for (const T::Clip& c : T::Clips) {
        if (c.support == T::Support::Vanilla) {
            continue;
        }
        // retract 1: exactly the plain clock; an A->B clip: the plain clock whatever the setting.
        for (const float retract : {1.0f, 1.75f, 3.0f}) {
            const T::Timing tm{1.0f, -1.0f, retract};
            if (retract > 1.0f && !c.to_b) {
                continue;
            }
            const T::Clock k = T::NormalClock(c, tm);
            EXPECT_EQ(T::ClockLeadSeconds(k), 0.0f) << c.family << " " << c.to_b;
            for (const float e : {0.0f, 0.1f, 0.3f, 0.6f, 2.0f}) {
                EXPECT_NEAR(T::ClockTime(k, e, c), T::ClipTime(e, c, tm), 1e-5f) << c.family;
            }
            EXPECT_NEAR(T::ClockSecondsTo(k, c.t_style, c), T::SecondsToSwitch(c, tm), 1e-5f);
        }
    }
}

TEST(TransformRetract, TheSawCleaversRetractSwitchesAsSoonAsItsExtend) {
    const T::Clip& extend = *Find(70, true);   // a000_19000: from its still 0.20, switch at 0.467
    const T::Clip& retract = *Find(70, false); // a000_19010: from 0, switch at 0.467
    const T::Timing tm{1.0f, -1.0f, 1.75f};
    const T::Clock ke = T::NormalClock(extend, tm);
    const T::Clock kr = T::NormalClock(retract, tm);
    // The bug report's numbers: the extend switched after 0.267 s, the retract after 0.467 s.
    EXPECT_NEAR(T::ClockSecondsTo(ke, extend.t_style, extend), 0.267f, 1e-3f);
    EXPECT_NEAR(T::ClockSecondsTo(T::NormalClock(retract, {}), retract.t_style, retract), 0.467f,
                1e-3f);
    // x1.75 up to the switch: 0.467 / 1.75 = 0.267 s, as the extend; nothing skipped.
    EXPECT_NEAR(kr.base_t, 0.0f, 1e-6f);
    EXPECT_NEAR(kr.lead_speed, 1.75f, 1e-6f);
    EXPECT_NEAR(kr.speed, 1.0f, 1e-6f);
    const float to_switch = T::ClockSecondsTo(kr, retract.t_style, retract);
    EXPECT_NEAR(to_switch, 0.2669f, 1e-3f);
    EXPECT_NEAR(to_switch, T::ClockSecondsTo(ke, extend.t_style, extend), 5e-3f);
    // Then the game's pace: the lock settling (0.467 .. 1.167) takes its 0.7 s.
    EXPECT_NEAR(T::ClockSecondsTo(kr, retract.length, retract), to_switch + 0.7f, 1e-3f);
    EXPECT_NEAR(T::ClockSpeedAt(kr, 0.1f), 1.75f, 1e-6f);
    EXPECT_NEAR(T::ClockSpeedAt(kr, 0.5f), 1.0f, 1e-6f);
    // Continuous at the switch, monotonic, the clip's start and end reached.
    EXPECT_NEAR(T::ClockTime(kr, 0.0f, retract), 0.0f, 1e-6f);
    EXPECT_NEAR(T::ClockTime(kr, to_switch, retract), retract.t_style, 1e-4f);
    EXPECT_NEAR(T::ClockTime(kr, to_switch + 0.1f, retract), retract.t_style + 0.1f, 1e-4f);
    EXPECT_NEAR(T::ClockTime(kr, 10.0f, retract), retract.length, 1e-6f);
    float last = -1.0f;
    for (float e = 0.0f; e < 1.2f; e += 1.0f / 90.0f) {
        const float t = T::ClockTime(kr, e, retract);
        EXPECT_GE(t, last);
        last = t;
        // The inverse agrees with the clock.
        if (t < retract.length - 1e-3f) {
            EXPECT_NEAR(T::ClockSecondsTo(kr, t, retract), e, 1e-4f);
        }
    }
    // Its sounds and the effect fire once each, in the clip's order, just sooner: 4800 at
    // 0.233 / 1.75 = 0.133 s (was 0.233 s), the effect at 0.433 / 1.75 = 0.247 s (was 0.433 s).
    std::uint32_t fired = 0;
    std::array<float, 8> when{};
    when.fill(-1.0f);
    for (float e = 0.0f; e < 1.2f; e += 1.0f / 90.0f) {
        const float t = T::ClockTime(kr, e, retract);
        const T::CueStep s = T::StepCues(retract, kr.base_t, t, fired);
        for (std::size_t i = 0; i < retract.cue_count; ++i) {
            if ((s.fire >> i) & 1u) {
                EXPECT_LT(when[i], 0.0f) << i; // once
                when[i] = e;
            }
        }
    }
    for (std::size_t i = 0; i < retract.cue_count; ++i) {
        EXPECT_GE(when[i], 0.0f) << i;
        EXPECT_NEAR(when[i], retract.cues[i].t0 / 1.75f, 1.0f / 90.0f + 1e-4f) << i;
        if (i > 0) {
            EXPECT_GE(when[i], when[i - 1]) << i;
        }
    }
}

TEST(TransformRetract, EveryRetractStartsFasterNoneSkipsAnything) {
    const T::Timing plain{};
    const T::Timing quick{1.0f, -1.0f, 1.75f};
    for (const T::Clip& c : T::Clips) {
        if (c.support == T::Support::Vanilla || c.to_b) {
            continue;
        }
        const T::Clock k0 = T::NormalClock(c, plain);
        const T::Clock k = T::NormalClock(c, quick);
        EXPECT_NEAR(k.base_t, k0.base_t, 1e-6f) << c.family; // the same start (nothing skipped)
        const float before = T::ClockSecondsTo(k0, c.t_style, c);
        const float after = T::ClockSecondsTo(k, c.t_style, c);
        EXPECT_NEAR(after, before / 1.75f, 1e-4f) << c.family;
        // After the switch: the game's pace.
        EXPECT_NEAR(T::ClockSecondsTo(k, c.length, c) - after,
                    T::ClockSecondsTo(k0, c.length, c) - before, 1e-4f)
            << c.family;
        // A faster transform_speed is never slowed by the lead; x4 at most.
        const T::Clock kf = T::NormalClock(c, {3.0f, -1.0f, 3.0f});
        EXPECT_LE(T::ClockSpeedAt(kf, k.base_t), T::MaxClipSpeed);
        EXPECT_GE(T::ClockSpeedAt(kf, k.base_t), 3.0f - 1e-6f);
    }
}

TEST(TransformRetract, ASwingJoiningTheQuickStartNeverSlowsIt) {
    const T::FlickTiming f{};
    const T::Clip& c = *Find(70, false);
    const T::Clock k = T::NormalClock(c, {1.0f, -1.0f, 1.75f});
    const float t = T::ClockTime(k, 0.1f, c); // 0.175: still in the quick start
    EXPECT_NEAR(t, 0.175f, 1e-4f);
    const T::Clock j = T::FlickFrom(c, t, T::ClockSpeedAt(k, t), f);
    EXPECT_GE(j.speed, 2.5f - 1e-6f);
    EXPECT_EQ(T::ClockLeadSeconds(j), 0.0f);
    EXPECT_LE(T::ClockSecondsTo(j, c.t_style, c), 0.1f + 1e-4f);
}

TEST(TransformFlick, FlickCuesEachOnceTheLeadInAtTheStart) {
    // A flick skips the clip's lead-in: its sounds (the unlatch clicks) play together at the
    // start, every one-shot exactly once, the held effects only inside their window.
    const T::FlickTiming f{};
    for (const T::Clip& c : T::Clips) {
        if (c.support == T::Support::Vanilla) {
            continue;
        }
        const T::Clock k = T::FlickClock(c, f);
        std::uint32_t fired = 0;
        std::array<int, 8> count{};
        for (float e = 0.0f; e < T::ClockSecondsTo(k, c.length, c) + 0.1f; e += 1.0f / 60.0f) {
            const float t = T::ClockTime(k, e, c);
            const T::CueStep s = T::StepCues(c, k.base_t, t, fired);
            for (std::size_t i = 0; i < c.cue_count; ++i) {
                if ((s.fire >> i) & 1u) {
                    ++count[i];
                    if (c.cues[i].t0 <= k.base_t) {
                        EXPECT_NEAR(e, 0.0f, 1e-6f) << c.family; // the lead-in's: at once
                    }
                }
                if ((s.held >> i) & 1u) {
                    EXPECT_GE(t, c.cues[i].t0);
                    EXPECT_LT(t, c.cues[i].t1);
                }
            }
        }
        for (std::size_t i = 0; i < c.cue_count; ++i) {
            if (!T::IsHeld(c.cues[i])) {
                EXPECT_EQ(count[i], 1) << c.family << " cue " << i;
            }
        }
    }
}

} // namespace

namespace {
TEST(TransformGripHold, BothChainWeaponsWait80MillisecondsAndFireOncePerSqueeze) {
    for (int family : {220, 240})
        for (int hz : {30, 60, 72, 90, 120}) {
            ASSERT_TRUE(T::GripHoldRequired(family));
            T::GripHold hold;
            for (int form = 0; form < 2; ++form) {
                EXPECT_FALSE(hold.Step(false, family, true, 1.f / hz));
                EXPECT_FALSE(hold.Step(true, family, true, 1.f / hz));
                int fired = 0;
                for (int i = 1; i <= hz * 2; ++i) {
                    const bool fire = hold.Step(true, family, true, 1.f / hz);
                    if (i < int(std::ceil(.08f * hz)))
                        EXPECT_FALSE(fire);
                    if (fire)
                        EXPECT_NEAR(float(i) / hz, .08f, 1.f / hz);
                    fired += fire;
                }
                EXPECT_EQ(fired, 1);
                EXPECT_TRUE(hold.Accepted());
            }
        }
    for (int family : {-1, 200, 210, 230, 250})
        EXPECT_FALSE(T::GripHoldRequired(family));
}
TEST(TransformGripHold, TapsCannotAccumulateAndReleaseNeverFires) {
    T::GripHold hold;
    for (int tap = 0; tap < 5; ++tap) {
        for (int i = 0; i < 8; ++i)
            EXPECT_FALSE(hold.Step(true, 220, true, .01f));
        EXPECT_FALSE(hold.Step(false, 220, true, .01f));
        EXPECT_FALSE(hold.Accepted());
    }
}
TEST(TransformGripHold, BlockTrackingGapOrWeaponChangeSpendsTheSqueeze) {
    for (int mode = 0; mode < 5; ++mode) {
        T::GripHold hold;
        for (int i = 0; i < 7; ++i)
            EXPECT_FALSE(hold.Step(true, 220, true, .01f));
        if (mode == 0)
            EXPECT_FALSE(hold.Step(true, 220, false, .01f));
        if (mode == 1)
            EXPECT_FALSE(hold.Step(true, 240, true, .01f));
        if (mode == 2)
            EXPECT_FALSE(hold.Step(true, 220, true, .2f));
        if (mode == 3)
            EXPECT_FALSE(hold.Step(true, 220, true, NAN));
        if (mode == 4)
            hold.Reset(); // menu or lost context while the grip remains down
        for (int i = 0; i < 100; ++i)
            EXPECT_FALSE(hold.Step(true, 220, true, .01f));
        EXPECT_FALSE(hold.Accepted());
        hold.Step(false, 220, true, .01f);
        hold.Step(true, 220, true, .01f);
        for (int i = 0; i < 7; ++i)
            EXPECT_FALSE(hold.Step(true, 220, true, .01f));
        EXPECT_TRUE(hold.Step(true, 220, true, .01f));
    }
}
} // namespace

namespace {
TEST(TransformGripHold, AFrameStallCancelsANearlyCompleteHoldUntilRelease) {
    T::GripHold hold;
    hold.Step(true, 220, true, .01f);
    for (int i = 0; i < 7; ++i)
        ASSERT_FALSE(hold.Step(true, 220, true, .01f));
    // The presenter passes the original sample time, before its animation dt clamp.
    EXPECT_FALSE(hold.Step(true, 220, true, .5f));
    EXPECT_FALSE(hold.Accepted());
    for (int i = 0; i < 100; ++i)
        EXPECT_FALSE(hold.Step(true, 220, true, .01f));
    hold.Step(false, 220, true, .01f);
    hold.Step(true, 220, true, .01f);
    for (int i = 0; i < 7; ++i)
        EXPECT_FALSE(hold.Step(true, 220, true, .01f));
    EXPECT_TRUE(hold.Step(true, 220, true, .01f));
}

TEST(TransformGripHold, SwitchingToABladeCannotBypassThePendingTransformHold) {
    for (int family : {220, 240}) {
        T::GripHold hold;
        ASSERT_TRUE(hold.Required(family));
        hold.Step(true, family, true, .01f);
        for (int i = 0; i < 7; ++i)
            ASSERT_FALSE(hold.Step(true, family, true, .01f));
        // Both left- and right-grip L1 routes consult Required before Step.
        ASSERT_TRUE(hold.Required(700));
        EXPECT_FALSE(hold.Step(true, 700, true, .01f));
        EXPECT_FALSE(hold.Accepted());
        EXPECT_TRUE(hold.Required(700));
        hold.Step(false, 700, false, .01f);
        EXPECT_FALSE(hold.Required(700));
        // A fresh blade squeeze retains its immediate transform.
        hold.Step(true, 700, false, .01f);
        EXPECT_FALSE(hold.Required(700));
    }
}
} // namespace
