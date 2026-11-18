// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdint>

namespace Common {

/**
 * SpinLock class
 * a lock similar to mutex that spins briefly instead of calling the supervisor, then parks the
 * waiter (WaitOnAddress / futex) so a descheduled holder does not leave every waiter spinning
 * through its time slice. Should be used on short sequences of code.
 */
class SpinLock {
public:
    SpinLock() = default;

    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    SpinLock(SpinLock&&) = delete;
    SpinLock& operator=(SpinLock&&) = delete;

    void lock();
    void unlock();
    [[nodiscard]] bool try_lock();

private:
    void LockSlow();

    // 0 free, 1 held, 2 held with a parked waiter (unlock wakes one)
    std::atomic<std::uint32_t> state{0};
};

} // namespace Common
