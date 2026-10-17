// SPDX-License-Identifier: GPL-2.0-or-later

// A gap in the camera updates (src/core/vr/camera_gap.h): a hitch, where the game camera stayed
// where the last update left it, resumes the view (the heading and the 6DoF origin kept, so a
// duck through a hitch stays a duck); a load or teleport (the follow camera's pivot moved more than
// JumpM) or a turned game camera (more than TurnRad) is a takeover, and so is the very first
// update. The duck with a hitch, step by step, as OnCameraTick measures it.

#include <cmath>
#include <limits>
#include <gtest/gtest.h>
#include "core/vr/camera_gap.h"

namespace {

namespace G = VR::CameraGap;

constexpr float kDeg = 3.14159265358979323846f / 180.0f;

G::Last LastAt(float yaw, float x, float y, float z) {
    G::Last last{};
    last.valid = true;
    last.yaw = yaw;
    last.anchor_valid = true;
    last.anchor[0] = x;
    last.anchor[1] = y;
    last.anchor[2] = z;
    return last;
}

} // namespace

TEST(CameraGap, TheFirstUpdateTakesOver) {
    const G::Last none{};
    const float anchor[3] = {0.0f, 0.0f, 0.0f};
    EXPECT_EQ(G::Decide(none, 0.0f, anchor, 1.0f).verdict, G::Verdict::First);
    EXPECT_EQ(G::Decide(none, 0.0f, nullptr, 1.0f).verdict, G::Verdict::First);
    EXPECT_FALSE(G::Decide(none, 0.0f, anchor, 1.0f).Resume());
}

TEST(CameraGap, AHitchResumes) {
    // The game did not run: the camera's yaw is what the last update wrote, the hunter where he
    // was (a frame's walk at most).
    const G::Last last = LastAt(0.7f, 10.0f, 2.0f, -30.0f);
    const float same[3] = {10.0f, 2.0f, -30.0f};
    G::Decision d = G::Decide(last, 0.7f, same, 1.0f);
    EXPECT_EQ(d.verdict, G::Verdict::Resume);
    EXPECT_TRUE(d.Resume());
    EXPECT_NEAR(d.moved_m, 0.0f, 1e-6f);
    EXPECT_NEAR(d.turned_rad, 0.0f, 1e-6f);
    // A step and a few degrees of the game's own camera follow: still a hitch.
    const float stepped[3] = {10.4f, 2.0f, -30.3f};
    d = G::Decide(last, 0.7f + 8.0f * kDeg, stepped, 1.0f);
    EXPECT_TRUE(d.Resume());
    EXPECT_NEAR(d.moved_m, 0.5f, 1e-4f);
    EXPECT_NEAR(d.turned_rad / kDeg, 8.0f, 1e-3f);
    // Across the +-180 degree seam.
    const G::Last seam = LastAt(179.0f * kDeg, 0.0f, 0.0f, 0.0f);
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    d = G::Decide(seam, -179.0f * kDeg, origin, 1.0f);
    EXPECT_TRUE(d.Resume());
    EXPECT_NEAR(d.turned_rad / kDeg, 2.0f, 1e-3f);
    // No anchor (the camera hook without its follow camera): the yaw decides alone.
    EXPECT_TRUE(G::Decide(last, 0.7f, nullptr, 1.0f).Resume());
    EXPECT_LT(G::Decide(last, 0.7f, nullptr, 1.0f).moved_m, 0.0f);
    G::Last no_anchor = last;
    no_anchor.anchor_valid = false;
    EXPECT_TRUE(G::Decide(no_anchor, 0.7f, same, 1.0f).Resume());
}

TEST(CameraGap, ALoadOrTeleportTakesOver) {
    const G::Last last = LastAt(0.7f, 10.0f, 2.0f, -30.0f);
    // A lamp, a death, an area change: the hunter is elsewhere.
    const float far[3] = {140.0f, -12.0f, 55.0f};
    EXPECT_EQ(G::Decide(last, 0.7f, far, 1.0f).verdict, G::Verdict::Jumped);
    // Just over and just under the threshold (metres: game units over world_scale).
    const float over[3] = {10.0f + G::JumpM + 0.01f, 2.0f, -30.0f};
    const float under[3] = {10.0f + G::JumpM - 0.01f, 2.0f, -30.0f};
    EXPECT_EQ(G::Decide(last, 0.7f, over, 1.0f).verdict, G::Verdict::Jumped);
    EXPECT_EQ(G::Decide(last, 0.7f, under, 1.0f).verdict, G::Verdict::Resume);
    // world_scale 2: 2 game units a metre, so the same move is half as far.
    EXPECT_EQ(G::Decide(last, 0.7f, over, 2.0f).verdict, G::Verdict::Resume);
    // A non-finite anchor is never taken for a hitch.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float bad[3] = {nan, 2.0f, -30.0f};
    EXPECT_EQ(G::Decide(last, 0.7f, bad, 1.0f).verdict, G::Verdict::Jumped);
    // The game turned its camera (a cutscene's end that places it behind the hunter).
    const float same[3] = {10.0f, 2.0f, -30.0f};
    const G::Decision d = G::Decide(last, 0.7f + 40.0f * kDeg, same, 1.0f);
    EXPECT_EQ(d.verdict, G::Verdict::Turned);
    EXPECT_NEAR(d.turned_rad / kDeg, 40.0f, 1e-3f);
    EXPECT_EQ(G::Decide(last, 0.7f + G::TurnRad + 0.01f, same, 1.0f).verdict,
              G::Verdict::Turned);
    EXPECT_EQ(G::Decide(last, 0.7f + G::TurnRad - 0.01f, same, 1.0f).verdict,
              G::Verdict::Resume);
}

// The playtest's case, as OnCameraTick measures it: recentered standing (origin y 1.60), the head
// ducks 0.5 m, a 0.2 s hitch, the head stands up. The camera's vertical offset is the head's from
// the origin: before (every gap took over) the origin went to the ducked head and standing put
// the view 0.5 m above the hunter; now it stays where the recenter put it.
TEST(CameraGap, ADuckThroughAHitchStaysADuck) {
    struct Bridge {
        float origin_y = 1.60f;
        G::Last last{};
        bool resume_on = true;
        void Tick(float head_y, bool after_gap, float game_yaw, const float* anchor) {
            if (after_gap) {
                const G::Decision d = G::Decide(last, game_yaw, anchor, 1.0f);
                if (!(resume_on && d.Resume())) {
                    origin_y = head_y; // the takeover: the origin from the head now
                }
            }
            last.valid = true;
            last.yaw = game_yaw;
            last.anchor_valid = anchor != nullptr;
            if (anchor != nullptr) {
                last.anchor[0] = anchor[0];
                last.anchor[1] = anchor[1];
                last.anchor[2] = anchor[2];
            }
        }
    };
    const float hunter[3] = {3.0f, 0.0f, -4.0f};
    for (const bool resume_on : {false, true}) {
        Bridge b{};
        b.resume_on = resume_on;
        b.Tick(1.60f, true, 0.3f, hunter); // the first update
        b.origin_y = 1.60f;                // the recenter, standing
        b.Tick(1.10f, false, 0.3f, hunter); // ducked 0.5 m
        EXPECT_NEAR(1.10f - b.origin_y, -0.50f, 1e-6f);
        b.Tick(1.10f, true, 0.3f, hunter); // the first update after a 0.2 s hitch, still ducked
        b.Tick(1.60f, false, 0.3f, hunter); // standing up
        const float offset = 1.60f - b.origin_y;
        if (resume_on) {
            EXPECT_NEAR(offset, 0.0f, 1e-6f) << "the view is back at the hunter's eyes";
        } else {
            EXPECT_NEAR(offset, 0.50f, 1e-6f) << "camera_gap_resume=0: the old 0.5 m float";
        }
    }
}
