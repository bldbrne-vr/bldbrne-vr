// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Bounded best-effort pool: the renderer never waits for speculative capacity.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
namespace ScratchDraw {
class SpeculativePool {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    bool closing{};
    std::vector<std::thread> threads;
public:
    std::atomic<size_t> failures{};
    explicit SpeculativePool(size_t count, void (*initialize)()) {
        try {
            for (size_t i = 0; i < count; ++i) threads.emplace_back([this, initialize] {
                if (initialize) initialize();
                for (;;) {
                    std::function<void()> job;
                    {
                        std::unique_lock lock(mutex);
                        wake.wait(lock, [&] { return closing || !jobs.empty(); });
                        if (closing) return;
                        job = std::move(jobs.front()); jobs.pop_front();
                    }
                    try { job(); } catch (...) { ++failures; }
                }
            });
        } catch (...) { Stop(); throw; }
    }
    void Stop() {
        { std::lock_guard lock(mutex); closing = true; jobs.clear(); }
        wake.notify_all();
        for (auto& thread : threads) if (thread.joinable()) thread.join();
    }
    ~SpeculativePool() { Stop(); }
    bool TrySubmit(std::function<void()> job) {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock || closing || jobs.size() >= 64) return false;
        jobs.push_back(std::move(job));
        lock.unlock(); wake.notify_one(); return true;
    }
};
}
