// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace Common {

// Like std::shared_mutex, but reader has priority over writer: a reader waits only for an ACTIVE
// writer, never for a waiting one; a writer waits until no reader and no writer holds the lock.
//
// The state lives in one atomic word (the writer bit plus the reader count), so an uncontended
// lock_shared / unlock_shared is a single atomic read-modify-write each. The inner mutex and the
// condition variable are only used to sleep when the lock is taken (and to wake those sleepers).
// Every operation on `state` and `waiters` is sequentially consistent: a sleeper increments
// `waiters` before it re-reads `state`, and a releaser writes `state` before it reads `waiters`, so
// one of the two always sees the other (no lost wake-up).
class SharedFirstMutex {
public:
    void lock() {
        if (TryAcquireWriter()) {
            return;
        }
        std::unique_lock<std::mutex> lk(mtx);
        waiters.fetch_add(1);
        cv.wait(lk, [this]() { return TryAcquireWriter(); });
        waiters.fetch_sub(1);
    }

    bool try_lock() {
        return TryAcquireWriter();
    }

    template <typename Clock, typename Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
        if (TryAcquireWriter()) {
            return true;
        }
        std::unique_lock<std::mutex> lk(mtx);
        waiters.fetch_add(1);
        const bool acquired = cv.wait_until(lk, abs_time, [this]() { return TryAcquireWriter(); });
        waiters.fetch_sub(1);
        return acquired;
    }

    void unlock() {
        // Only the writer holds the lock (readers cannot enter while the writer bit is set).
        state.store(0);
        WakeWaiters();
    }

    void lock_shared() {
        if (TryAcquireReader()) {
            return;
        }
        std::unique_lock<std::mutex> lk(mtx);
        waiters.fetch_add(1);
        cv.wait(lk, [this]() { return TryAcquireReader(); });
        waiters.fetch_sub(1);
    }

    bool try_lock_shared() {
        return TryAcquireReader();
    }

    template <typename Clock, typename Duration>
    bool try_lock_shared_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
        if (TryAcquireReader()) {
            return true;
        }
        std::unique_lock<std::mutex> lk(mtx);
        waiters.fetch_add(1);
        const bool acquired = cv.wait_until(lk, abs_time, [this]() { return TryAcquireReader(); });
        waiters.fetch_sub(1);
        return acquired;
    }

    void unlock_shared() {
        // The last reader out lets a waiting writer in; readers never wait for readers.
        if (state.fetch_sub(1) == 1) {
            WakeWaiters();
        }
    }

private:
    static constexpr std::uint32_t WriterBit = 1u << 31;

    bool TryAcquireWriter() {
        std::uint32_t expected = 0;
        return state.compare_exchange_strong(expected, WriterBit);
    }

    bool TryAcquireReader() {
        std::uint32_t s = state.load();
        while ((s & WriterBit) == 0) {
            if (state.compare_exchange_weak(s, s + 1)) {
                return true;
            }
        }
        return false;
    }

    void WakeWaiters() {
        if (waiters.load() != 0) {
            // Taking the mutex orders the notify after a sleeper's predicate check (it holds the
            // mutex from its waiters increment until it sleeps).
            std::lock_guard<std::mutex> lk(mtx);
            cv.notify_all();
        }
    }

    std::atomic<std::uint32_t> state{0};
    std::atomic<std::uint32_t> waiters{0};
    std::mutex mtx;
    std::condition_variable cv;
};

} // namespace Common
