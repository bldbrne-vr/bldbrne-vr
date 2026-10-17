// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <bitset>
#include <cstdint>
#include <gtest/gtest.h>

#include "core/vr/body_respawn.h"
#include "core/vr/pose_refresh.h"

namespace {

using VR::Body::NewPlayer;
using VR::Body::RespawnWatch;

constexpr std::int64_t Ms = 1'000'000;
constexpr std::int64_t Frame = 11 * Ms; // ~90 Hz pose copies

int g_players[3]{};
const void* const A = &g_players[0];
const void* const B = &g_players[1];

// Copies of player `p` for `count` frames; returns the first non-None.
NewPlayer Copies(RespawnWatch& w, const void* p, std::int64_t& now, int count) {
    NewPlayer seen = NewPlayer::None;
    for (int i = 0; i < count; ++i) {
        now += Frame;
        const NewPlayer why = w.NoteCopy(p, now);
        if (seen == NewPlayer::None) {
            seen = why;
        }
    }
    return seen;
}

TEST(BodyRespawn, TheFirstPlayerIsNotNew) {
    RespawnWatch w;
    std::int64_t now = 1000 * Ms;
    EXPECT_EQ(w.NoteCopy(A, now), NewPlayer::None);
    EXPECT_FALSE(w.Settling());
    EXPECT_EQ(Copies(w, A, now, 600), NewPlayer::None);
    EXPECT_FALSE(w.Settle(false, now).settling);
}

TEST(BodyRespawn, AnotherPlayerObjectIsNew) {
    RespawnWatch w;
    std::int64_t now = 1000 * Ms;
    Copies(w, A, now, 10);
    now += Frame;
    EXPECT_EQ(w.NoteCopy(B, now), NewPlayer::Object);
    EXPECT_TRUE(w.Settling());
    // Only once: the next copies of B are the same player.
    EXPECT_EQ(Copies(w, B, now, 100), NewPlayer::None);
}

TEST(BodyRespawn, NormalPlayHitchesAreNotALoad) {
    RespawnWatch w;
    std::int64_t now = 1000 * Ms;
    Copies(w, A, now, 10);
    now += 2500 * Ms; // a long shader-compile hitch
    EXPECT_EQ(w.NoteCopy(A, now), NewPlayer::None);
    EXPECT_FALSE(w.Settling());
}

TEST(BodyRespawn, TheSameObjectAfterALoadIsNew) {
    RespawnWatch w;
    std::int64_t now = 1000 * Ms;
    Copies(w, A, now, 10);
    now += RespawnWatch::LoadGapNs;
    EXPECT_EQ(w.NoteCopy(A, now), NewPlayer::Load);
    EXPECT_TRUE(w.Settling());
}

TEST(BodyRespawn, RevivedAfterHpZeroAtTheSameAddress) {
    RespawnWatch w;
    std::int64_t now = 1000 * Ms;
    Copies(w, A, now, 10);
    EXPECT_EQ(w.NoteHp(true, 1200, now), NewPlayer::None);
    EXPECT_EQ(w.NoteHp(true, 0, now), NewPlayer::None); // the death
    EXPECT_FALSE(w.Settling());
    EXPECT_EQ(w.NoteHp(false, 0, now), NewPlayer::None); // no read: nothing changes
    EXPECT_EQ(w.NoteHp(true, -5, now), NewPlayer::None);
    Copies(w, A, now, 100); // the death's animation, the reload's frames (no long gap)
    EXPECT_EQ(w.NoteHp(true, 1200, now), NewPlayer::Revived);
    EXPECT_TRUE(w.Settling());
    EXPECT_EQ(w.NoteHp(true, 1200, now), NewPlayer::None);
    EXPECT_EQ(w.NoteHp(true, 600, now), NewPlayer::None); // a hit is not a revival
}

TEST(BodyRespawn, ANewObjectForgetsTheDeath) {
    RespawnWatch w;
    std::int64_t now = 1000 * Ms;
    Copies(w, A, now, 10);
    EXPECT_EQ(w.NoteHp(true, 0, now), NewPlayer::None);
    now += Frame;
    EXPECT_EQ(w.NoteCopy(B, now), NewPlayer::Object);
    // The new object's HP is not a second respawn.
    EXPECT_EQ(w.NoteHp(true, 1200, now), NewPlayer::None);
}

TEST(BodyRespawn, SettleLastsAtLeastTheMinimum) {
    RespawnWatch w;
    std::int64_t now = 1000 * Ms;
    Copies(w, A, now, 10);
    now += Frame;
    const std::int64_t start = now;
    w.NoteCopy(B, now);
    for (; now - start < RespawnWatch::SettleMinNs; now += Frame) {
        EXPECT_TRUE(w.Settle(false, now).settling) << (now - start) / Ms;
    }
    const auto end = w.Settle(false, now);
    EXPECT_FALSE(end.settling);
    EXPECT_TRUE(end.resumed);
    EXPECT_GE(end.after_ns, RespawnWatch::SettleMinNs);
    EXPECT_LT(end.after_ns, RespawnWatch::SettleMinNs + Frame);
    // Over: nothing more.
    const auto later = w.Settle(true, now + Frame);
    EXPECT_FALSE(later.settling);
    EXPECT_FALSE(later.resumed);
}

TEST(BodyRespawn, SettleWaitsOutTheEventStateUpToTheCap) {
    RespawnWatch w;
    std::int64_t now = 1000 * Ms;
    Copies(w, A, now, 10);
    EXPECT_EQ(w.NoteHp(true, 0, now), NewPlayer::None);
    now += Frame;
    const std::int64_t start = now;
    EXPECT_EQ(w.NoteHp(true, 1200, now), NewPlayer::Revived);
    // The get-up (event state) for 0.8 s: past the minimum, still settling.
    for (; now - start < 800 * Ms; now += Frame) {
        EXPECT_TRUE(w.Settle(true, now).settling);
    }
    const auto end = w.Settle(false, now);
    EXPECT_TRUE(end.resumed);
    EXPECT_GE(end.after_ns, 800 * Ms);

    // An event state that never ends: the cap.
    now += Frame;
    w.NoteCopy(B, now);
    const std::int64_t again = now;
    VR::Body::SettleStep last{};
    for (; !last.resumed; now += Frame) {
        last = w.Settle(true, now);
        ASSERT_LE(now - again, RespawnWatch::SettleMaxNs + Frame);
    }
    EXPECT_GE(last.after_ns, RespawnWatch::SettleMaxNs);
}

TEST(BodyRespawn, AnotherRespawnRestartsTheSettle) {
    RespawnWatch w;
    std::int64_t now = 1000 * Ms;
    Copies(w, A, now, 10);
    now += Frame;
    w.NoteCopy(B, now);
    now += 400 * Ms;
    EXPECT_TRUE(w.Settle(false, now).settling);
    w.NoteCopy(A, now); // yet another player object
    now += 400 * Ms;
    EXPECT_TRUE(w.Settle(false, now).settling); // 0.4 s into the new settle
}

TEST(BodyRespawn, AClockStepBackNeverStallsTheWrites) {
    RespawnWatch w;
    std::int64_t now = 1000 * Ms;
    Copies(w, A, now, 10);
    now += Frame;
    w.NoteCopy(B, now);
    EXPECT_TRUE(w.Settle(true, now - 5 * Ms).resumed);
}

TEST(BodyRespawn, NamesForTheLog) {
    EXPECT_STREQ(VR::Body::NewPlayerName(NewPlayer::Object), "another player object");
    EXPECT_STREQ(VR::Body::NewPlayerName(NewPlayer::Load), "a load");
    EXPECT_STREQ(VR::Body::NewPlayerName(NewPlayer::Revived), "revived after HP 0");
}

// RestoreGame: only the pose we wrote, its flags, a running module and a synced pose.
TEST(StaleRestore, EveryCheckIsRequired) {
    VR::Body::StaleRestoreFacts ok{true, true, true, VR::Body::PoseSyncBoth};
    EXPECT_TRUE(VR::Body::StaleRestoreAllowed(ok));
    auto f = ok;
    f.same_pose = false;
    EXPECT_FALSE(VR::Body::StaleRestoreAllowed(f));
    f = ok;
    f.flags_ok = false;
    EXPECT_FALSE(VR::Body::StaleRestoreAllowed(f));
    f = ok;
    f.module_running = false; // a player still being built
    EXPECT_FALSE(VR::Body::StaleRestoreAllowed(f));
    for (const std::uint16_t sync : {0x0000, 0x0001, 0x0100}) {
        f = ok;
        f.sync = sync; // not synchronized since (0x0100: setPoseLocalSpace's state)
        EXPECT_FALSE(VR::Body::StaleRestoreAllowed(f)) << sync;
    }
}

// A rebuilt player at the same addresses: its locals are not our write, so nothing of the last
// player's animation goes into them.
TEST(StaleRestore, ANewPlayersLocalsAreNeverOverwritten) {
    std::array<float, 4> animated{1, 2, 3, 4}, written{5, 6, 7, 8};
    std::bitset<4> edited;
    edited.set(1);
    edited.set(2);
    std::array<float, 4> rebuilt{0, 0, 0, 0};
    EXPECT_FALSE(VR::Body::RestoreHeldLocals(rebuilt.data(), animated, written, edited));
    EXPECT_EQ(rebuilt, (std::array<float, 4>{0, 0, 0, 0}));
    // Still exactly our write: the game's own locals of the edited bones come back.
    std::array<float, 4> ours = written;
    ours[0] = 9; // an unedited bone the game moved
    EXPECT_TRUE(VR::Body::RestoreHeldLocals(ours.data(), animated, written, edited));
    EXPECT_EQ(ours, (std::array<float, 4>{9, 2, 3, 8}));
}

} // namespace
