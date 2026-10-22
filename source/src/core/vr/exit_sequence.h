// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

// VR: the process's end after a close (emulator.cpp Emulator::EndAfterClose; vr.ini
// xr_exit_teardown_ms, xr_exit_terminate).
//
// The tester's 2026-10-09 log (Quit in the Meta Link menu): the session went STOPPING, IDLE,
// EXITING; the emulator closed the normal way (the play time and the shader cache written, then
// std::quick_exit) - and the process never ended until it was killed (which worked at once).
// quick_exit runs its handlers, then ExitProcess: the loader lock, every other thread killed (the
// process now terminating), then DLL_PROCESS_DETACH for every loaded DLL, the OpenXR runtime's
// client DLL included, with its session and instance never destroyed (quick_exit runs no
// destructors). A runtime DLL whose detach never returns hangs it there for good (the mock's
// MOCKXR_HANG_DETACH: such a process cannot even be killed, it is already terminating), so the
// user's, which could, most likely stuck just before: in a handler or waiting for the loader lock.
// So a close now:
//  1. lets a game save that is being written finish (WaitWhile, a time limit);
//  2. ends OpenXR in order - the frame thread stopped, the GPU idle, the session and the instance
//     destroyed (the runtime told the application left; its DLL let go by the loader) - on its own
//     thread with a time limit (RunBounded: never blocking the exit);
//  3. runs quick_exit's handlers as before (the save backup thread, the ImGui ini, the log) and, on
//     Windows, ends the process with TerminateProcess at their end: no DLL detach to hang it, the
//     exit code kept (a kill, which ended it at once, does the same); a Watchdog ends it anyway
//     if the handlers have not finished in time.
// A headset taken off or asleep still never closes the emulator (core/vr/session_exit.h).
// Pure (std only) for tests/test_exit_sequence.cpp.
namespace VR::Exit {

using Clock = std::chrono::steady_clock;

inline double MsSince(Clock::time_point from) {
    return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

struct StepResult {
    bool done{false}; // it finished within its time limit
    double ms{0.0};   // how long it ran (or waited, when it did not finish)
};

// Runs `step` on its own thread and waits for it at most `budget`. A step still running then is
// left behind (its thread detached: the process ends without it), so it must only touch what
// outlives the process's end. A step that throws counts as done. `budget` <= 0: not run.
inline StepResult RunBounded(std::function<void()> step, std::chrono::milliseconds budget) {
    if (budget.count() <= 0 || !step) {
        return {};
    }
    struct State {
        std::mutex m;
        std::condition_variable cv;
        bool done{false};
    };
    auto state = std::make_shared<State>();
    const auto start = Clock::now();
    std::thread([state, step = std::move(step)] {
        try {
            step();
        } catch (...) {
        }
        std::scoped_lock lk{state->m};
        state->done = true;
        state->cv.notify_all();
    }).detach();
    std::unique_lock lk{state->m};
    const bool done = state->cv.wait_for(lk, budget, [&] { return state->done; });
    return {done, MsSince(start)};
}

// Waits while `busy()` (polled every `poll`), at most `budget`. done = it was not busy any more.
inline StepResult WaitWhile(const std::function<bool()>& busy, std::chrono::milliseconds budget,
                            std::chrono::milliseconds poll = std::chrono::milliseconds{5}) {
    const auto start = Clock::now();
    const auto deadline = start + budget;
    while (busy()) {
        if (Clock::now() >= deadline) {
            return {false, MsSince(start)};
        }
        std::this_thread::sleep_for(poll);
    }
    return {true, MsSince(start)};
}

// Calls `fire` once, on its own thread, unless Disarm() comes within `budget` of Arm(). Re-arming
// disarms the previous one.
class Watchdog {
public:
    Watchdog() = default;
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    ~Watchdog() {
        Disarm();
    }

    void Arm(std::chrono::milliseconds budget, std::function<void()> fire) {
        Disarm();
        auto state = std::make_shared<State>();
        state_ = state;
        std::thread([state, budget, fire = std::move(fire)] {
            std::unique_lock lk{state->m};
            if (state->cv.wait_for(lk, budget, [&] { return state->disarmed; })) {
                return;
            }
            lk.unlock();
            if (fire) {
                fire();
            }
        }).detach();
    }

    void Disarm() {
        if (auto state = std::exchange(state_, nullptr)) {
            std::scoped_lock lk{state->m};
            state->disarmed = true;
            state->cv.notify_all();
        }
    }

private:
    struct State {
        std::mutex m;
        std::condition_variable cv;
        bool disarmed{false};
    };
    std::shared_ptr<State> state_;
};

// The OpenXR teardown the presenter registers while it runs (renderer_vulkan/openxr_presenter.cpp
// TeardownForExit); none without a headset. Run under the hook's own lock, so a presenter going
// away waits for a teardown in progress.
struct TeardownHook {
    std::mutex m;
    std::function<void()> fn;
};
inline TeardownHook& Hook() {
    static TeardownHook hook;
    return hook;
}
inline void SetTeardown(std::function<void()> fn) {
    std::scoped_lock lk{Hook().m};
    Hook().fn = std::move(fn);
}
inline void ClearTeardown() {
    SetTeardown({});
}
inline bool HasTeardown() {
    std::scoped_lock lk{Hook().m};
    return static_cast<bool>(Hook().fn);
}
// Runs the registered teardown (once: it is taken) within `budget`. done=false, ms=0: none.
inline StepResult RunTeardown(std::chrono::milliseconds budget) {
    if (!HasTeardown()) {
        return {};
    }
    return RunBounded(
        [] {
            std::scoped_lock lk{Hook().m};
            if (auto fn = std::exchange(Hook().fn, nullptr)) {
                fn();
            }
        },
        budget);
}

// When the headset's exit asked the window to close (OpenXR CheckSessionExit pushes the quit),
// steady ns; 0 = it did not (the window's own close). For the exit's log line.
inline std::atomic<std::int64_t>& QuitRequestedNs() {
    static std::atomic<std::int64_t> ns{0};
    return ns;
}
inline std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

} // namespace VR::Exit
