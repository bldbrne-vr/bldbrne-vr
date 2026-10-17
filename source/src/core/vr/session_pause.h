// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace VR {

class SessionPausePolicy {
public:
    bool Update(bool focused, bool available) {
        if (!available) {
            was_focused = false;
            return false;
        }
        was_focused |= focused;
        return was_focused && !focused;
    }

private:
    bool was_focused{false};
};

class SessionPauseGate {
public:
    template <typename Clock>
    void Request(bool pause, Clock now) {
        std::lock_guard lock{mutex};
        requested.store(pause);
        if (pause && !Paused()) {
            ++sequence;
            pause_start.store(now());
            ++sequence;
        }
        if (!pause) {
            if (Paused()) {
                ++sequence;
                elapsed.fetch_add(now() - pause_start.load());
                pause_start.store(0);
                ++sequence;
            }
            changed.notify_all();
        }
    }

    bool Waiting() const {
        return waiting.load() != 0;
    }

    bool Requested() const {
        return requested.load();
    }

    bool Paused() const {
        return pause_start.load() != 0;
    }

    void Wait() {
        if (!Requested()) {
            return;
        }
        std::unique_lock lock{mutex};
        if (!Requested()) {
            return;
        }
        ++waiting;
        changed.wait(lock, [&] { return !Requested(); });
        --waiting;
    }

    template <typename Clock>
    std::uint64_t ReadClock(Clock now) const {
        for (;;) {
            const auto before = sequence.load();
            if (before & 1) {
                continue;
            }
            const auto start = pause_start.load();
            const auto offset = elapsed.load();
            const auto tick = start != 0 ? start : now();
            if (sequence.load() == before) {
                return tick - offset;
            }
        }
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic_bool requested{false};
    std::atomic<unsigned> waiting{0};
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint64_t> pause_start{0};
    std::atomic<std::uint64_t> elapsed{0};
};

inline SessionPauseGate& HeadsetPause() {
    static SessionPauseGate gate;
    return gate;
}

}
