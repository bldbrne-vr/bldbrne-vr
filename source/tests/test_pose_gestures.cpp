// SPDX-License-Identifier: GPL-2.0-or-later

// Physical gestures (core/vr/pose_gestures.h): the settings, the learned list's decoding, the pose
// classifier in the head's frame (Make Contact, Rally, Point / Wave, Bow; the Brain's zone only
// knows Make Contact), the recogniser (settle, stillness, the grace for tracking blips, the hold
// time, one serial per hold), the gates and the game side's decision (the Brain's hold timer and
// region, the Doll's 0.5 s and cooldown, nothing for an unlearned gesture).

#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "core/vr/pose_gestures.h"

namespace {

namespace P = VR::PoseGesture;

constexpr float Dt = 1.0f / 120.0f; // Quest 3 over Link: one frame of the presenter

// A standing player (the mock's body): eyes at 1.6 m looking down -Z.
P::Frame Base() {
    P::Frame f{};
    f.head_ok = true;
    f.head = {0.0f, 1.6f, 0.0f};
    f.forward = {0.0f, 0.0f, -1.0f};
    f.left_ok = f.right_ok = true;
    f.left = {-0.20f, 1.00f, -0.20f}; // the mock's rest
    f.right = {0.20f, 1.00f, -0.20f};
    return f;
}

P::Frame Pitched(P::Frame f, float deg) {
    const float r = deg * 3.14159265f / 180.0f;
    f.forward = {0.0f, std::sin(r), -std::cos(r)};
    return f;
}

P::Frame MakeContact() {
    P::Frame f = Base();
    f.left = {-0.30f, 2.05f, -0.20f};
    f.right = {0.25f, 2.05f, -0.20f};
    return f;
}

P::Frame Rally() {
    P::Frame f = Base();
    f.right = {0.25f, 2.05f, -0.20f}; // the mock's "high"
    return f;
}

P::Frame Point() {
    P::Frame f = Base();
    f.right = {0.17f, 1.38f, -0.50f}; // the mock's "reach_fwd"
    return f;
}

P::Frame Bow() {
    return Pitched(Base(), -40.0f);
}

constexpr std::uint32_t All = P::KindBit(P::Kind::MakeContact) | P::KindBit(P::Kind::Rally) |
                              P::KindBit(P::Kind::Point) | P::KindBit(P::Kind::Bow);

// The kit save's (and the user's) seven: Point Forward, Hunter's Salutation, Conviction, Joy, Wave,
// Hunter's Rally, Sit Down, as GestureGameData holds them (run g3).
constexpr std::uint32_t KitEntries[30] = {
    0x00000003, 0x00010005, 0x00020007, 0x00030009, 0x0004000a, 0x0005000c, 0x0006000e,
    0x00070010, 0x00080013, 0x00090014, 0x000a0016, 0x000b0018, 0x000c001b, 0x000d001c,
    0x000e001e, 0x000f0020, 0x00100022, 0x00110024, 0x00120026, 0x00130028, 0x0014002a,
    0x0015002d, 0x0016002e, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    0xFFFFFFFF, 0xFFFFFFFF,
};

// A reason for the checks (never a null pointer into a string).
const char* Str(const char* s) {
    return s != nullptr ? s : "(none)";
}

// Runs the recogniser over `seconds` of one frame.
const P::Output& Hold(P::Recognizer& r, const P::Frame& f, float seconds,
                      P::Zone zone = P::Zone::Doll, std::uint32_t available = All) {
    const int frames = static_cast<int>(std::lround(seconds / Dt));
    for (int i = 0; i < frames; ++i) {
        r.Update(f, P::Config{}, zone, available, Dt);
    }
    return r.Last();
}

} // namespace

TEST(PoseSettings, DefaultsAndParsing) {
    P::PoseSettings s{};
    EXPECT_TRUE(s.enabled);
    EXPECT_FALSE(s.all);
    EXPECT_NEAR(s.contact_hold_s, 10.0f, 1e-6f);
    EXPECT_TRUE(P::ParsePoseSetting(s, "pose_gestures", "0"));
    EXPECT_FALSE(s.enabled);
    EXPECT_TRUE(P::ParsePoseSetting(s, "pose_gestures_all", "1"));
    EXPECT_TRUE(s.all);
    EXPECT_TRUE(P::ParsePoseSetting(s, "pose_contact_hold_s", "20"));
    EXPECT_NEAR(s.contact_hold_s, 20.0f, 1e-6f);
    EXPECT_TRUE(P::ParsePoseSetting(s, "pose_contact_hold_s", "999"));
    EXPECT_NEAR(s.contact_hold_s, 120.0f, 1e-6f);
    EXPECT_TRUE(P::ParsePoseSetting(s, "pose_contact_hold_s", "0"));
    EXPECT_NEAR(s.contact_hold_s, 1.0f, 1e-6f);
    // `gestures` is the right hand's swings: never ours.
    EXPECT_FALSE(P::ParsePoseSetting(s, "gestures", "0"));
    bool threw = false;
    try {
        P::ParsePoseSetting(s, "pose_contact_hold_s", "abc");
    } catch (const std::exception&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(PoseSettings, DefaultsBlockParsesBack) {
    std::ostringstream out;
    P::WritePoseDefaults(out);
    std::istringstream in{out.str()};
    std::string line;
    P::PoseSettings s{};
    s.enabled = false;
    s.all = true;
    s.contact_hold_s = 3.0f;
    int keys = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto eq = line.find('=');
        ASSERT_NE(eq, std::string::npos);
        EXPECT_TRUE(P::ParsePoseSetting(s, line.substr(0, eq), line.substr(eq + 1)));
        ++keys;
    }
    EXPECT_EQ(keys, 3);
    EXPECT_TRUE(s.enabled);
    EXPECT_FALSE(s.all);
    EXPECT_NEAR(s.contact_hold_s, 10.0f, 1e-6f);
}

TEST(PoseLearned, KitSaveDecodes) {
    const std::uint32_t mask = P::LearnedMask(KitEntries, 30);
    EXPECT_EQ(mask, P::G(1) | P::G(2) | P::G(3) | P::G(4) | P::G(9) | P::G(13) | P::G(22));
    // Rally (Hunter's Rally), Point / Wave and Bow (Hunter's Salutation) count; Make Contact not.
    EXPECT_TRUE(P::Available(P::Kind::Rally, mask, false));
    EXPECT_TRUE(P::Available(P::Kind::Point, mask, false));
    EXPECT_TRUE(P::Available(P::Kind::Bow, mask, false));
    EXPECT_FALSE(P::Available(P::Kind::MakeContact, mask, false));
    EXPECT_TRUE(P::Available(P::Kind::MakeContact, mask, true)); // pose_gestures_all=1
    EXPECT_FALSE(P::Available(P::Kind::None, mask, true));
    EXPECT_EQ(P::AvailableKinds(mask, false), All & ~P::KindBit(P::Kind::MakeContact));
    EXPECT_EQ(P::AvailableKinds(0, true), All);
    EXPECT_EQ(P::AvailableKinds(0, false), 0u);
}

TEST(PoseLearned, MakeContactLearnedAndUnusedEntries) {
    std::uint32_t entries[30];
    for (auto& e : entries) {
        e = 0xFFFFFFFFu;
    }
    entries[20] = (20u << 16) | (21u << 1) | 1u; // Make Contact, learned
    entries[21] = (21u << 16) | (22u << 1);      // Sit Down, not learned
    entries[22] = (22u << 16) | 0xFFFEu | 1u;    // an empty slot (idx bits all set)
    entries[23] = (23u << 16) | (24u << 1) | 1u; // an unused gesture (idx > 23): never
    const std::uint32_t mask = P::LearnedMask(entries, 30);
    EXPECT_EQ(mask, P::G(21));
    EXPECT_TRUE(P::Available(P::Kind::MakeContact, mask, false));
    EXPECT_FALSE(P::Available(P::Kind::Rally, mask, false));
}

TEST(PoseMarkers, Categories) {
    EXPECT_EQ(P::MarkerOf(P::Kind::MakeContact), 161);
    EXPECT_EQ(P::MarkerOf(P::Kind::Rally), 161);
    EXPECT_EQ(P::MarkerOf(P::Kind::Point), 162);
    EXPECT_EQ(P::MarkerOf(P::Kind::Bow), 163);
    EXPECT_EQ(P::MarkerOf(P::Kind::None), -1);
}

TEST(PoseRegion, BrainCylinder) {
    EXPECT_TRUE(P::InBrainRegion(P::BrainCentre));
    EXPECT_TRUE(P::InBrainRegion({P::BrainCentre.x + 9.5f, P::BrainCentre.y + 0.2f, P::BrainCentre.z}));
    EXPECT_FALSE(P::InBrainRegion({P::BrainCentre.x + 10.1f, P::BrainCentre.y, P::BrainCentre.z}));
    EXPECT_FALSE(P::InBrainRegion({P::BrainCentre.x, P::BrainCentre.y + 6.0f, P::BrainCentre.z}));
    EXPECT_FALSE(P::InBrainRegion({P::BrainCentre.x, P::BrainCentre.y - 3.0f, P::BrainCentre.z}));
    // The zone reaches a little further, so a pose can start before the region.
    EXPECT_TRUE(P::NearBrain({P::BrainCentre.x, P::BrainCentre.y, P::BrainCentre.z + 11.5f}));
    EXPECT_FALSE(P::InBrainRegion({P::BrainCentre.x, P::BrainCentre.y, P::BrainCentre.z + 11.5f}));
    EXPECT_FALSE(P::NearBrain({P::BrainCentre.x, P::BrainCentre.y, P::BrainCentre.z + 12.5f}));
    // Elsewhere (the Doll's spot in the Hunter's Dream) is nowhere near.
    EXPECT_FALSE(P::NearBrain({-5.98f, -5.64f, -14.66f}));
}

TEST(PoseClassify, TheFourPoses) {
    const P::Config c{};
    EXPECT_EQ(P::Classify(Base(), c, P::Zone::Doll), P::Kind::None); // arms at rest, level head
    EXPECT_EQ(P::Classify(MakeContact(), c, P::Zone::Doll), P::Kind::MakeContact);
    EXPECT_EQ(P::Classify(Rally(), c, P::Zone::Doll), P::Kind::Rally);
    EXPECT_EQ(P::Classify(Point(), c, P::Zone::Doll), P::Kind::Point);
    EXPECT_EQ(P::Classify(Bow(), c, P::Zone::Doll), P::Kind::Bow);
    // The left hand works the same.
    P::Frame left_rally = Base();
    left_rally.left = {-0.25f, 2.05f, -0.20f};
    EXPECT_EQ(P::Classify(left_rally, c, P::Zone::Doll), P::Kind::Rally);
    P::Frame left_point = Base();
    left_point.left = {-0.17f, 1.38f, -0.50f};
    EXPECT_EQ(P::Classify(left_point, c, P::Zone::Doll), P::Kind::Point);
}

TEST(PoseClassify, HeadFrameFollowsTheYaw) {
    const P::Config c{};
    // Turned 90 degrees to the left (forward -X) and moved: the same pose in the head's frame.
    P::Frame f{};
    f.head_ok = f.left_ok = f.right_ok = true;
    f.head = {2.0f, 1.5f, 3.0f};
    f.forward = {-1.0f, 0.0f, 0.0f};
    f.left = {2.2f, 0.9f, 3.2f};   // low
    f.right = {1.5f, 1.28f, 2.83f}; // 50 cm in front, at the shoulder
    EXPECT_EQ(P::Classify(f, c, P::Zone::Doll), P::Kind::Point);
    const P::Local r = P::ToHead(f, f.right);
    EXPECT_NEAR(r.fwd, 0.5f, 1e-4f);
    EXPECT_NEAR(r.up, -0.22f, 1e-4f);
    // A hand behind the head is no point.
    f.right = {2.5f, 1.28f, 2.83f};
    EXPECT_EQ(P::Classify(f, c, P::Zone::Doll), P::Kind::None);
}

TEST(PoseClassify, Boundaries) {
    const P::Config c{};
    // Make Contact needs the hands apart (both up and together is no pose).
    P::Frame together = MakeContact();
    together.left = {-0.05f, 2.05f, -0.20f};
    together.right = {0.05f, 2.05f, -0.20f};
    EXPECT_EQ(P::Classify(together, c, P::Zone::Doll), P::Kind::None);
    // ... and both over the head: one at the brow is no Make Contact (and the other is not low).
    P::Frame brow = MakeContact();
    brow.left = {-0.30f, 1.65f, -0.20f};
    EXPECT_EQ(P::Classify(brow, c, P::Zone::Doll), P::Kind::None);
    // Rally needs the other hand low: a hand up and one at the chest is nothing.
    P::Frame chest = Rally();
    chest.left = {-0.15f, 1.35f, -0.25f};
    EXPECT_EQ(P::Classify(chest, c, P::Zone::Doll), P::Kind::None);
    // A bow needs the head down 30 degrees (not 20, not at the feet) and both hands low.
    EXPECT_EQ(P::Classify(Pitched(Base(), -20.0f), c, P::Zone::Doll), P::Kind::None);
    EXPECT_EQ(P::Classify(Pitched(Base(), -85.0f), c, P::Zone::Doll), P::Kind::None);
    P::Frame bow_hand = Bow();
    bow_hand.right = {0.10f, 1.35f, -0.30f}; // a hand on the chest
    EXPECT_EQ(P::Classify(bow_hand, c, P::Zone::Doll), P::Kind::None);
    // A point with the hand too close (a guard at the chest) is nothing.
    P::Frame guard = Point();
    guard.right = {0.18f, 1.25f, -0.25f};
    EXPECT_EQ(P::Classify(guard, c, P::Zone::Doll), P::Kind::None);
}

// Slightly loose: a fist a little over the eyes, a point a little lower or closer, a bow with a
// hand at the bottom of the chest. A hand beside the head is still no wave.
TEST(PoseClassify, SlightlyLoose) {
    const P::Config c{};
    P::Frame fist = Base();
    fist.right = {0.20f, 1.68f, -0.15f}; // 8 cm over the eyes
    EXPECT_EQ(P::Classify(fist, c, P::Zone::Doll), P::Kind::Rally);
    P::Frame lower = Base();
    lower.right = {0.17f, 1.27f, -0.45f}; // 33 cm under the eyes
    EXPECT_EQ(P::Classify(lower, c, P::Zone::Doll), P::Kind::Point);
    P::Frame closer = Base();
    closer.right = {0.30f, 1.55f, -0.32f}; // 32 cm in front
    EXPECT_EQ(P::Classify(closer, c, P::Zone::Doll), P::Kind::Point);
    P::Frame bow_hand = Bow();
    bow_hand.right = {0.10f, 1.23f, -0.25f}; // 37 cm under the eyes
    EXPECT_EQ(P::Classify(bow_hand, c, P::Zone::Doll), P::Kind::Bow);
    P::Frame beside = Base();
    beside.right = {0.35f, 1.65f, -0.15f};
    EXPECT_EQ(P::Classify(beside, c, P::Zone::Doll), P::Kind::None);
}

TEST(PoseClassify, ZonesAndTracking) {
    const P::Config c{};
    // At the Brain only Make Contact counts; nowhere else anything.
    EXPECT_EQ(P::Classify(MakeContact(), c, P::Zone::Brain), P::Kind::MakeContact);
    EXPECT_EQ(P::Classify(Rally(), c, P::Zone::Brain), P::Kind::None);
    EXPECT_EQ(P::Classify(Bow(), c, P::Zone::Brain), P::Kind::None);
    EXPECT_EQ(P::Classify(MakeContact(), c, P::Zone::None), P::Kind::None);
    // A lost hand or head is no pose.
    P::Frame lost = MakeContact();
    lost.left_ok = false;
    EXPECT_EQ(P::Classify(lost, c, P::Zone::Doll), P::Kind::None);
    lost = MakeContact();
    lost.head_ok = false;
    EXPECT_EQ(P::Classify(lost, c, P::Zone::Doll), P::Kind::None);
}

TEST(PoseRecognizer, SettlesThenHolds) {
    P::Recognizer r;
    const P::Frame f = Rally();
    const auto v1 = Hold(r, f, 0.35f).pose; // not yet
    EXPECT_EQ(v1, P::Kind::None);
    const P::Output& o = Hold(r, f, 0.1f);
    EXPECT_EQ(o.pose, P::Kind::Rally);
    EXPECT_EQ(o.serial, 1u);
    EXPECT_NEAR(o.held_s, 0.45f, 0.02f);
    const auto v2 = Hold(r, f, 1.0f).held_s;
    EXPECT_NEAR(v2, 1.45f, 0.02f);
    EXPECT_EQ(r.Last().serial, 1u); // one hold, one serial
}

TEST(PoseRecognizer, RecognisedOnceWithTheTick) {
    P::Recognizer r;
    int ticks = 0;
    for (int i = 0; i < 240; ++i) {
        ticks += r.Update(Bow(), P::Config{}, P::Zone::Doll, All, Dt).recognized_now ? 1 : 0;
    }
    EXPECT_EQ(ticks, 1);
}

TEST(PoseRecognizer, StillPosesNeedStillHands) {
    P::Recognizer r;
    P::Frame f = MakeContact();
    f.left_v = {0.6f, 0.0f, 0.0f}; // waving the arms about
    const auto v3 = Hold(r, f, 1.0f).pose;
    EXPECT_EQ(v3, P::Kind::None);
    f.left_v = {};
    const auto v4 = Hold(r, f, 0.35f).pose; // the stillness settles too
    EXPECT_EQ(v4, P::Kind::None);
    const auto v5 = Hold(r, f, 0.1f).pose;
    EXPECT_EQ(v5, P::Kind::MakeContact);
    // Once held, moving a little keeps it.
    f.right_v = {0.8f, 0.0f, 0.0f};
    const auto v6 = Hold(r, f, 1.0f).pose;
    EXPECT_EQ(v6, P::Kind::MakeContact);
}

TEST(PoseRecognizer, AWaveMoves) {
    P::Recognizer r;
    P::Frame f = Point();
    f.right_v = {0.9f, 0.0f, 0.0f}; // side to side
    const auto v7 = Hold(r, f, 0.45f).pose;
    EXPECT_EQ(v7, P::Kind::Point);
}

TEST(PoseRecognizer, GraceForBlipsThenRelease) {
    P::Recognizer r;
    const P::Frame f = MakeContact();
    Hold(r, f, 2.0f, P::Zone::Brain);
    ASSERT_EQ(r.Last().pose, P::Kind::MakeContact);
    // A 0.3 s tracking blip keeps the hold and its time.
    P::Frame blip = f;
    blip.left_ok = false;
    Hold(r, blip, 0.3f, P::Zone::Brain);
    EXPECT_EQ(r.Last().pose, P::Kind::MakeContact);
    Hold(r, f, 1.0f, P::Zone::Brain);
    EXPECT_EQ(r.Last().pose, P::Kind::MakeContact);
    EXPECT_NEAR(r.Last().held_s, 3.3f, 0.03f);
    EXPECT_EQ(r.Last().serial, 1u);
    // Arms down for longer: released, and the time with it.
    bool released = false;
    float released_s = 0.0f;
    for (int i = 0; i < 120; ++i) {
        const P::Output& o = r.Update(Base(), P::Config{}, P::Zone::Brain, All, Dt);
        if (o.released == P::Kind::MakeContact) {
            released = true;
            released_s = o.released_held_s;
        }
    }
    EXPECT_TRUE(released);
    EXPECT_NEAR(released_s, 3.7f, 0.05f);
    EXPECT_EQ(r.Last().pose, P::Kind::None);
    EXPECT_NEAR(r.Last().held_s, 0.0f, 1e-6f);
    // Raised again: a new hold from zero, a new serial.
    Hold(r, f, 0.5f, P::Zone::Brain);
    EXPECT_EQ(r.Last().serial, 2u);
    EXPECT_LT(r.Last().held_s, 0.6f);
}

TEST(PoseRecognizer, LeavingTheZoneReleasesAtOnce) {
    P::Recognizer r;
    Hold(r, Rally(), 1.0f);
    ASSERT_EQ(r.Last().pose, P::Kind::Rally);
    const P::Output& o = r.Update(Rally(), P::Config{}, P::Zone::None, All, Dt);
    EXPECT_EQ(o.released, P::Kind::Rally);
    EXPECT_EQ(o.pose, P::Kind::None);
}

TEST(PoseRecognizer, UnavailablePosesAreNoPose) {
    P::Recognizer r;
    const std::uint32_t kit = P::AvailableKinds(P::LearnedMask(KitEntries, 30), false);
    const auto v8 = Hold(r, MakeContact(), 3.0f, P::Zone::Doll, kit).pose;
    EXPECT_EQ(v8, P::Kind::None);
    const auto v9 = Hold(r, MakeContact(), 60.0f, P::Zone::Brain, kit).pose;
    EXPECT_EQ(v9, P::Kind::None);
    EXPECT_EQ(r.Last().serial, 0u);
}

TEST(PoseRecognizer, ChangingPoseStartsANewHold) {
    P::Recognizer r;
    Hold(r, Rally(), 1.0f);
    ASSERT_EQ(r.Last().serial, 1u);
    Hold(r, Point(), 1.5f); // the grace runs out, then Point settles
    EXPECT_EQ(r.Last().pose, P::Kind::Point);
    EXPECT_EQ(r.Last().serial, 2u);
    EXPECT_LT(r.Last().held_s, 1.2f);
}

TEST(PoseRecognizer, ResetKeepsTheSerialGoing) {
    P::Recognizer r;
    Hold(r, Bow(), 1.0f);
    ASSERT_EQ(r.Last().serial, 1u);
    r.Reset();
    EXPECT_EQ(r.Last().pose, P::Kind::None);
    Hold(r, Bow(), 1.0f);
    EXPECT_EQ(r.Last().serial, 2u); // a hold after a reset is never taken for the old one
}

TEST(PoseGates, Order) {
    P::GateInput g{true, true, true, false, false};
    EXPECT_TRUE(P::GateBlock(g) == nullptr);
    g.free = false;
    EXPECT_STREQ(Str(P::GateBlock(g)), "busy (attacking, rolling, a ladder, an item, ...)");
    g.cinematic = true;
    EXPECT_STREQ(Str(P::GateBlock(g)), "a cinematic");
    g.menu = true;
    EXPECT_STREQ(Str(P::GateBlock(g)), "a menu is open");
    g.alive = false;
    EXPECT_STREQ(Str(P::GateBlock(g)), "dead");
    g.host = false;
    EXPECT_STREQ(Str(P::GateBlock(g)), "not the host");
}

namespace {

P::TriggerInput Doll(P::Kind pose, float held, std::uint32_t serial, double now) {
    P::TriggerInput in{};
    in.zone = P::Zone::Doll;
    in.pose = pose;
    in.held_s = held;
    in.serial = serial;
    in.learned = P::LearnedMask(KitEntries, 30);
    in.now_s = now;
    return in;
}

P::TriggerInput Brain(float held, std::uint32_t serial, bool learned = true) {
    P::TriggerInput in{};
    in.zone = P::Zone::Brain;
    in.in_region = true;
    in.pose = P::Kind::MakeContact;
    in.held_s = held;
    in.serial = serial;
    in.learned = learned ? P::G(21) : P::LearnedMask(KitEntries, 30);
    in.contact_hold_s = 50.0f;
    in.now_s = 100.0;
    return in;
}

} // namespace

TEST(PoseTrigger, DollCategoriesOncePerHoldWithCooldown) {
    P::Trigger t;
    const auto v10 = t.Step(Doll(P::Kind::Bow, 0.4f, 1, 10.0)).count; // under 0.5 s
    EXPECT_EQ(v10, 0);
    P::TriggerOutput o = t.Step(Doll(P::Kind::Bow, 0.5f, 1, 10.1));
    ASSERT_EQ(o.count, 1);
    EXPECT_EQ(o.markers[0], 163);
    // Held on: once per hold.
    const auto v11 = t.Step(Doll(P::Kind::Bow, 3.0f, 1, 12.6)).count;
    EXPECT_EQ(v11, 0);
    // A new pose within 5 s waits; after it, fires.
    o = t.Step(Doll(P::Kind::Point, 0.6f, 2, 13.0));
    EXPECT_EQ(o.count, 0);
    EXPECT_STREQ(Str(o.waiting), "cooldown");
    o = t.Step(Doll(P::Kind::Point, 3.6f, 2, 15.2));
    ASSERT_EQ(o.count, 1);
    EXPECT_EQ(o.markers[0], 162);
    o = t.Step(Doll(P::Kind::Rally, 0.5f, 3, 21.0));
    ASSERT_EQ(o.count, 1);
    EXPECT_EQ(o.markers[0], 161);
}

TEST(PoseTrigger, DollWaitsLongerAfterARally) {
    // Her 7001 (161) runs 5-6 s: a marker inside it would be remembered by her event and answered
    // instead of the next pose. The held pose waits for the end, then fires.
    P::Trigger t;
    P::TriggerOutput o = t.Step(Doll(P::Kind::Rally, 0.5f, 1, 10.0));
    ASSERT_EQ(o.count, 1);
    EXPECT_EQ(o.markers[0], 161);
    o = t.Step(Doll(P::Kind::Point, 3.0f, 2, 15.5));
    EXPECT_EQ(o.count, 0);
    EXPECT_STREQ(Str(o.waiting), "cooldown");
    o = t.Step(Doll(P::Kind::Point, 5.5f, 2, 18.0));
    ASSERT_EQ(o.count, 1);
    EXPECT_EQ(o.markers[0], 162);
    // After a 162 the usual 5 s.
    o = t.Step(Doll(P::Kind::Bow, 0.6f, 3, 23.1));
    ASSERT_EQ(o.count, 1);
    EXPECT_EQ(o.markers[0], 163);
}

TEST(PoseTrigger, UnlearnedNeverFires) {
    P::Trigger t;
    // Make Contact is not in the kit save: never, at the Doll or at the Brain, however long.
    P::TriggerOutput o = t.Step(Doll(P::Kind::MakeContact, 10.0f, 1, 10.0));
    EXPECT_EQ(o.count, 0);
    EXPECT_STREQ(Str(o.waiting), "not learned");
    o = t.Step(Brain(120.0f, 2, false));
    EXPECT_EQ(o.count, 0);
    EXPECT_STREQ(Str(o.waiting), "not learned");
    // pose_gestures_all=1: every gesture counts.
    P::TriggerInput all = Brain(50.0f, 3, false);
    all.all = true;
    o = t.Step(all);
    ASSERT_EQ(o.count, 2);
    EXPECT_EQ(o.markers[0], 164);
    EXPECT_EQ(o.markers[1], 161);
}

TEST(PoseTrigger, BrainHoldTimer) {
    P::Trigger t;
    P::TriggerOutput o = t.Step(Brain(49.9f, 1));
    EXPECT_EQ(o.count, 0);
    EXPECT_STREQ(Str(o.waiting), "holding");
    o = t.Step(Brain(50.0f, 1));
    ASSERT_EQ(o.count, 2);
    EXPECT_EQ(o.markers[0], 164);
    EXPECT_EQ(o.markers[1], 161);
    const auto v12 = t.Step(Brain(80.0f, 1)).count; // once per hold
    EXPECT_EQ(v12, 0);
    // A new hold starts from zero (the recogniser's serial).
    const auto v13 = t.Step(Brain(10.0f, 2)).count;
    EXPECT_EQ(v13, 0);
    const auto v14 = t.Step(Brain(50.5f, 2)).count;
    EXPECT_EQ(v14, 2);
    // A shorter hold time (pose_contact_hold_s).
    P::Trigger t2;
    P::TriggerInput quick = Brain(20.0f, 1);
    quick.contact_hold_s = 20.0f;
    const auto v15 = t2.Step(quick).count;
    EXPECT_EQ(v15, 2);
}

TEST(PoseTrigger, BrainNeedsTheRegionAndMakeContact) {
    P::Trigger t;
    P::TriggerInput out = Brain(60.0f, 1);
    out.in_region = false;
    P::TriggerOutput o = t.Step(out);
    EXPECT_EQ(o.count, 0);
    EXPECT_STREQ(Str(o.waiting), "outside the Brain's region");
    // Walked into the region with the hold on: fires there.
    const auto v16 = t.Step(Brain(61.0f, 1)).count;
    EXPECT_EQ(v16, 2);
    P::Trigger t2;
    P::TriggerInput rally = Brain(60.0f, 1);
    rally.pose = P::Kind::Rally;
    rally.learned = P::LearnedMask(KitEntries, 30);
    const auto v17 = t2.Step(rally).count;
    EXPECT_EQ(v17, 0);
}

TEST(PoseTrigger, GatesStopIt) {
    P::Trigger t;
    P::TriggerInput in = Doll(P::Kind::Bow, 1.0f, 1, 10.0);
    in.block = "a menu is open";
    P::TriggerOutput o = t.Step(in);
    EXPECT_EQ(o.count, 0);
    EXPECT_STREQ(Str(o.waiting), "a menu is open");
    in.block = nullptr;
    const auto v18 = t.Step(in).count;
    EXPECT_EQ(v18, 1);
    P::Trigger t2;
    P::TriggerInput b = Brain(60.0f, 1);
    b.block = "dead";
    const auto v19 = t2.Step(b).count;
    EXPECT_EQ(v19, 0);
    // Outside both zones nothing.
    P::Trigger t3;
    P::TriggerInput none = Doll(P::Kind::Bow, 1.0f, 1, 10.0);
    none.zone = P::Zone::None;
    const auto v20 = t3.Step(none).count;
    EXPECT_EQ(v20, 0);
}

TEST(PoseEndToEnd, DollAndBrainFromFrames) {
    // The presenter's recogniser feeding the game's trigger, frame by frame, as at the Doll with
    // the kit save: bow (163), point (162), rally (161), Make Contact (nothing: not learned).
    const std::uint32_t learned = P::LearnedMask(KitEntries, 30);
    const std::uint32_t avail = P::AvailableKinds(learned, false);
    P::Recognizer r;
    P::Trigger t;
    double now = 0.0;
    std::string fired;
    const auto run = [&](const P::Frame& f, float seconds, P::Zone zone, float hold_s) {
        const int frames = static_cast<int>(std::lround(seconds / Dt));
        for (int i = 0; i < frames; ++i) {
            now += Dt;
            const P::Output& o = r.Update(f, P::Config{}, zone, avail, Dt);
            P::TriggerInput in{};
            in.zone = zone;
            in.in_region = true;
            in.pose = o.pose;
            in.held_s = o.held_s;
            in.serial = o.serial;
            in.learned = learned;
            in.contact_hold_s = hold_s;
            in.now_s = now;
            const P::TriggerOutput out = t.Step(in);
            for (int k = 0; k < out.count; ++k) {
                fired += std::to_string(out.markers[k]) + " ";
            }
        }
    };
    run(Bow(), 3.0f, P::Zone::Doll, 50.0f);
    run(Base(), 3.0f, P::Zone::Doll, 50.0f);
    run(Point(), 3.0f, P::Zone::Doll, 50.0f);
    run(Base(), 3.0f, P::Zone::Doll, 50.0f);
    run(Rally(), 3.0f, P::Zone::Doll, 50.0f);
    run(MakeContact(), 70.0f, P::Zone::Doll, 50.0f);
    run(MakeContact(), 70.0f, P::Zone::Brain, 50.0f);
    EXPECT_EQ(fired, "163 162 161 ");
}
