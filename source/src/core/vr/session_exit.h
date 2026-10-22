// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <cstdint>

// VR: what closes the emulator from the headset's side. The tester's rule (2026-10-09, after the
// game closed when they took the headset off): "ONLY WHEN WE KILL IT! Like either kill it in meta
// link by hitting 'quit app' or we close it on windows." So only two things close it:
//  - EXITING: the runtime's explicit end (the OpenXR spec's "the application should end"; the Meta
//    runtime's Quit of 2026-10-09, usertest_0041 13443-13447: STOPPING, IDLE, EXITING): the
//    presenter pushes the window's quit (CheckSessionExit) and the emulator closes the normal way
//    (the play time, the shader cache, the OpenXR teardown, TerminateProcess:
//    core/vr/exit_sequence.h);
//  - the window's close: the player closing it in Windows, or Meta Link closing it after a Quit that
//    sent no EXITING (the 2026-10-08 runtime: STOPPING, IDLE, then the window closed 10-15 s later,
//    usertest_2880 / usertest_3200fix / usertest_hold10's last lines; once it never came and the
//    window had to be closed by hand, usertest_tilt_live) - emulator.cpp EndAfterClose.
// Nothing else does:
//  - a session stop (STOPPING, IDLE), with or without a controller in use: the runtime stops the
//    session when the headset is taken off, sleeps, its proximity sensor is uncovered or the Link
//    connection restarts, and a Quit without EXITING looks exactly the same. The game keeps
//    running on the desktop (its own camera: "not first person" in the log) and a later READY
//    begins the session again (swapchains, DIBR, late warp, input and the body resume). Until
//    2026-10-09 a stop with a controller in use was taken as a Quit and closed the emulator 3 s
//    into IDLE (vr.ini xr_exit_on_stop_s, now retired): usertest_1116_headsetoff.txt 15944-15980,
//    the headset taken off while holding the controllers (no profile change before the stop),
//    closed the game the player was playing;
//  - LOSS_PENDING (the session being lost: a Link cable or connection drop, the runtime gone): VR
//    goes off, the game keeps running on the desktop until its window is closed.
// The headset's presence (XR_EXT_user_presence) is not offered by the Quest runtime over Link
// (usertest_1116's extension list), and closing never depends on it.
// Pure logic (no OpenXR types) for tests/test_session_exit.cpp.
namespace VR {

enum class XrState { Other, Ready, Running, Stopping, Idle, Exiting, LossPending };

class SessionExitGate {
public:
    // A session state change from the runtime at `now_ns` (steady clock).
    void OnState(XrState state, std::int64_t now_ns) {
        switch (state) {
        case XrState::Stopping:
            if (running_) {
                stopped_since_ = now_ns; // the headset off / asleep, a Link restart, a Quit
                ++stops_;
            }
            running_ = false;
            break;
        case XrState::Ready:
        case XrState::Running:
            running_ = true;
            stopped_since_ = 0;
            break;
        case XrState::Exiting:
            exiting_ = true;
            break;
        case XrState::LossPending:
            lost_ = true; // a Link drop, not a kill
            break;
        case XrState::Idle:
        case XrState::Other:
            break;
        }
    }

    // Whether the emulator should close now: only after EXITING (the window's own close is the
    // window's business).
    [[nodiscard]] bool ShouldExit() const {
        return exiting_;
    }

    // When the runtime stopped the running session, while it has not come back (READY) yet: the
    // game runs on the desktop. 0 when not stopped.
    [[nodiscard]] std::int64_t StoppedSinceNs() const {
        return stopped_since_;
    }

    // Stops of a running session so far (each one resumed or not).
    [[nodiscard]] int Stops() const {
        return stops_;
    }

    // The runtime said the session is being lost (LOSS_PENDING): it never closes.
    [[nodiscard]] bool Lost() const {
        return lost_;
    }

private:
    bool running_{false};           // READY or later since the last stop
    std::int64_t stopped_since_{0}; // the running session stopped at (0: not stopped)
    int stops_{0};
    bool exiting_{false}; // EXITING
    bool lost_{false};    // LOSS_PENDING (never closes)
};

} // namespace VR
