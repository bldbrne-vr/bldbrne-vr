// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <thread>

// One persistent helper; synchronous completion preserves caller lifetime.
// Only the bytes of an already-validated mapped region are passed to this worker.
namespace ScratchCopy {
class Worker {
    std::mutex caller, state;
    std::condition_variable ready, finished;
    void* destination{};
    const void* source{};
    std::size_t count{};
    bool pending{}, stopping{};
    std::thread thread;
public:
    Worker() : thread([this] {
        std::unique_lock lock(state);
        for (;;) {
            ready.wait(lock, [this] { return pending || stopping; });
            if (stopping) return;
            auto* dst = destination;
            const auto* src = source;
            const auto size = count;
            lock.unlock();
            std::memcpy(dst, src, size);
            lock.lock();
            pending = false;
            finished.notify_one();
        }
    }) {}
    ~Worker() {
        { std::lock_guard lock(state); stopping = true; }
        ready.notify_one();
        thread.join();
    }
    void Copy(void* dst, const void* src, std::size_t bytes) {
        if (bytes < 1024 * 1024) { std::memcpy(dst, src, bytes); return; }
        // Other callers fall back rather than queue behind the helper.
        std::unique_lock use(caller, std::try_to_lock);
        if (!use.owns_lock()) { std::memcpy(dst, src, bytes); return; }
        const auto half = (bytes / 2) & ~std::size_t(63);
        {
            std::lock_guard lock(state);
            destination = static_cast<char*>(dst) + half;
            source = static_cast<const char*>(src) + half;
            count = bytes - half;
            pending = true;
        }
        ready.notify_one();
        std::memcpy(dst, src, half);
        std::unique_lock lock(state);
        finished.wait(lock, [this] { return !pending; });
    }
};
inline void Copy(void* dst, const void* src, std::size_t bytes) {
    if (bytes < 1024 * 1024) { std::memcpy(dst, src, bytes); return; }
    static Worker worker;
    worker.Copy(dst, src, bytes);
}
}
