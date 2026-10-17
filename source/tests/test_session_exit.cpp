// SPDX-License-Identifier: GPL-2.0-or-later

// core/vr/session_exit.h: only the runtime's EXITING closes the emulator from the headset's side
// (and the window's own close, outside this gate). A session stop (STOPPING, IDLE) - the headset
// taken off or asleep, with or without the controllers in use, a Link restart, a Quit without
// EXITING - and LOSS_PENDING never do: the game keeps running and a later READY resumes VR. The
// sequences are the user's own logs.

#include <cstdint>

#include <gtest/gtest.h>

#include "core/vr/session_exit.h"

namespace {

using VR::SessionExitGate;
using VR::XrState;

constexpr std::int64_t Ms = 1'000'000;
constexpr std::int64_t Hour = 3'600'000 * Ms;

// READY .. FOCUSED at `t` (the presenter maps SYNCHRONIZED / VISIBLE / FOCUSED to Running).
void Start(SessionExitGate& gate, std::int64_t t) {
    gate.OnState(XrState::Ready, t);
    gate.OnState(XrState::Running, t + 1 * Ms); // SYNCHRONIZED
    gate.OnState(XrState::Running, t + 2 * Ms); // VISIBLE
    gate.OnState(XrState::Running, t + 3 * Ms); // FOCUSED
}

// FOCUSED -> VISIBLE -> SYNCHRONIZED -> STOPPING -> IDLE in one poll at `t` (every stop in the
// user's logs).
void Stop(SessionExitGate& gate, std::int64_t t) {
    gate.OnState(XrState::Running, t); // VISIBLE
    gate.OnState(XrState::Running, t); // SYNCHRONIZED
    gate.OnState(XrState::Stopping, t);
    gate.OnState(XrState::Idle, t);
}

TEST(SessionExit, HeadsetTakenOffHoldingTheControllersKeepsRunning) {
    // usertest_1116_headsetoff.txt 15944-15980: playing with both controllers bound (no profile
    // change before the stop), the headset taken off: VISIBLE, SYNCHRONIZED, STOPPING, IDLE. The
    // old gate (xr_exit_on_stop_s=3) closed the game 3 s into the IDLE; it must keep running.
    SessionExitGate gate;
    gate.OnState(XrState::Idle, 0); // the session is created IDLE
    Start(gate, 100 * Ms);
    const std::int64_t t0 = 30 * 60'000 * Ms; // half an hour of play
    Stop(gate, t0);
    EXPECT_EQ(gate.StoppedSinceNs(), t0);
    EXPECT_EQ(gate.Stops(), 1);
    for (std::int64_t t = t0; t < t0 + 60'000 * Ms; t += 10 * Ms) { // the 10 ms IDLE loop
        ASSERT_FALSE(gate.ShouldExit()) << (t - t0) / Ms << " ms into the IDLE";
    }
    EXPECT_FALSE(gate.ShouldExit()); // an hour with the headset on the desk: still running
    EXPECT_FALSE(gate.Lost());
    // The headset back on: READY, the session begun again.
    Start(gate, t0 + Hour);
    EXPECT_EQ(gate.StoppedSinceNs(), 0);
    EXPECT_FALSE(gate.ShouldExit());
}

TEST(SessionExit, AHeadsetThatSleepsKeepsRunningAndResumes) {
    // usertest_hold10.txt 25183-25604: both hands' profiles went to none, ~1.1 s later VISIBLE /
    // SYNCHRONIZED / STOPPING / IDLE in one poll, 50.5 s IDLE, READY .. FOCUSED, a second stop
    // 0.3 s long, then 15 more minutes of play.
    SessionExitGate gate;
    Start(gate, 0);
    const std::int64_t t0 = 1084 * Ms;
    Stop(gate, t0);
    for (std::int64_t t = t0; t < t0 + 50'531 * Ms; t += 10 * Ms) {
        ASSERT_FALSE(gate.ShouldExit()) << (t - t0) / Ms << " ms into the IDLE";
    }
    const std::int64_t t1 = t0 + 50'531 * Ms;
    Start(gate, t1);
    EXPECT_EQ(gate.StoppedSinceNs(), 0);
    Stop(gate, t1 + 400 * Ms); // the second, short stop
    EXPECT_EQ(gate.Stops(), 2);
    EXPECT_FALSE(gate.ShouldExit());
    Start(gate, t1 + 700 * Ms);
    EXPECT_FALSE(gate.ShouldExit());
    // hold10 47217-47220, the session's end: a Quit in the menu of the 2026-10-08 runtime (STOPPING,
    // IDLE, no EXITING). It looks exactly like a headset taken off, so the gate keeps the game
    // running; Meta Link closed the window (the window's close ends the emulator, not this gate).
    Stop(gate, t1 + 900'000 * Ms);
    EXPECT_FALSE(gate.ShouldExit());
    EXPECT_NE(gate.StoppedSinceNs(), 0);
}

TEST(SessionExit, QuitWithExitingClosesAtOnce) {
    // usertest_0041.txt 13422-13447, Quit in the Meta Link menu (2026-10-09): VISIBLE,
    // SYNCHRONIZED, STOPPING, IDLE, EXITING.
    SessionExitGate gate;
    Start(gate, 0);
    Stop(gate, 600'000 * Ms);
    EXPECT_FALSE(gate.ShouldExit()); // the IDLE alone: not yet
    gate.OnState(XrState::Exiting, 600'001 * Ms);
    EXPECT_TRUE(gate.ShouldExit());
    // EXITING without a stop first (any runtime's explicit end) closes too.
    SessionExitGate direct;
    Start(direct, 0);
    direct.OnState(XrState::Exiting, 5 * Ms);
    EXPECT_TRUE(direct.ShouldExit());
    EXPECT_FALSE(direct.Lost());
}

TEST(SessionExit, ALinkHiccupKeepsRunning) {
    // usertest_sharp 7326-7344: a stop with the controllers bound, READY again within 0.98 s.
    SessionExitGate gate;
    Start(gate, 0);
    Stop(gate, 10'000 * Ms);
    EXPECT_FALSE(gate.ShouldExit());
    Start(gate, 10'980 * Ms);
    EXPECT_EQ(gate.StoppedSinceNs(), 0);
    EXPECT_EQ(gate.Stops(), 1);
    EXPECT_FALSE(gate.ShouldExit());
    // An IDLE without a stop first (never seen; an odd runtime) is not a stop either.
    gate.OnState(XrState::Idle, 20'000 * Ms);
    EXPECT_EQ(gate.Stops(), 1);
    EXPECT_FALSE(gate.ShouldExit());
}

TEST(SessionExit, LossNeverCloses) {
    // The user, 2026-10-09: "if I just take off the headset, do not exit the game. Only if I kill
    // it": a Link cable or connection drop (LOSS_PENDING) is not a kill.
    SessionExitGate drop;
    Start(drop, 0);
    drop.OnState(XrState::LossPending, 5 * Ms);
    EXPECT_TRUE(drop.Lost());
    EXPECT_FALSE(drop.ShouldExit());
    // Lost while stopped (the headset off, then the cable pulled).
    SessionExitGate asleep;
    Start(asleep, 0);
    Stop(asleep, 10 * Ms);
    asleep.OnState(XrState::LossPending, 20 * Ms);
    EXPECT_TRUE(asleep.Lost());
    EXPECT_FALSE(asleep.ShouldExit());
    // EXITING after a loss (the runtime's explicit end) still closes.
    drop.OnState(XrState::Exiting, 70'000 * Ms);
    EXPECT_TRUE(drop.ShouldExit());
}

TEST(SessionExit, TheStartUpIdleIsNotAStop) {
    // A session is created IDLE and waits for READY (the headset not on yet): not a stop (the
    // presenter's 12 s watchdog still says how to start Link), never a close.
    SessionExitGate gate;
    gate.OnState(XrState::Idle, 0);
    EXPECT_EQ(gate.StoppedSinceNs(), 0);
    EXPECT_EQ(gate.Stops(), 0);
    EXPECT_FALSE(gate.ShouldExit());
    // A STOPPING that never ran (not seen; defensive) is not a stop of a running session.
    gate.OnState(XrState::Stopping, 10 * Ms);
    EXPECT_EQ(gate.Stops(), 0);
    Start(gate, 61'000 * Ms);
    EXPECT_FALSE(gate.ShouldExit());
}

TEST(SessionExit, ManyStopsNeverClose) {
    // An evening of taking the headset off and on (and the dashboard's own stops): never closed.
    SessionExitGate gate;
    std::int64_t t = 0;
    for (int i = 0; i < 100; ++i) {
        Start(gate, t);
        t += 60'000 * Ms;
        Stop(gate, t);
        t += (i % 3 == 0 ? 500 : 120'000) * Ms; // a hiccup or two minutes off
        ASSERT_FALSE(gate.ShouldExit()) << "stop " << i;
    }
    EXPECT_EQ(gate.Stops(), 100);
}

} // namespace
