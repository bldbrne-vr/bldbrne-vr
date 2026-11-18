// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <thread>
#include <vector>

namespace VideoCore::StartupCompile {
// A short rest after every BatchSize jobs, and at most Workers() at once: room for the rest of the
// PC while it prepares (2026-11-10: "do the 10ms pause every 64 jobs, and add 4 workers";
// 32 jobs and 2 workers before). A CPU with fewer than 6 threads keeps 2, one with 2 or fewer 1.
inline constexpr std::size_t BatchSize = 64;
inline constexpr auto BatchPause = std::chrono::milliseconds{10};
inline unsigned Workers() {
    const unsigned hw = std::thread::hardware_concurrency();
    return hw >= 6 ? 4 : hw > 2 ? 2 : 1;
}

// Startup only. The caller stays available to pump the UI; at most Workers() jobs run concurrently.
// All workers finish before return or exception propagation; none may outlive caller-owned data.
template <typename Work, typename Poll>
void RunBatch(std::size_t count, Work&& work, Poll&& poll) {
    if (count == 0)
        return;
    std::atomic<std::size_t> next{0}, done{0};
    std::vector<std::future<void>> workers;
    std::vector<std::jthread> threads;
    for (unsigned w = 0; w < Workers() && w < count; ++w) {
        std::packaged_task<void()> task([&] {
            for (auto i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
                work(i);
                done.fetch_add(1);
            }
        });
        workers.push_back(task.get_future());
        threads.emplace_back(std::move(task));
    }
    for (;;) {
        bool ready = true;
        for (auto& worker : workers)
            ready &= worker.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready;
        poll(done.load());
        if (ready)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    threads.clear(); // Join, including destruction of thread-local shader pools.
    for (auto& worker : workers)
        worker.get();
}

// The desktop startup display uses a software renderer, never another Vulkan workload.
// Calls are confined to the startup thread; non-main-thread/headless starts simply have no UI.
struct DisplayState;
class Display {
public:
    Display();
    ~Display();
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

private:
    DisplayState* impl{};
};
void Begin(const char* stage, std::size_t total);
void Update(std::size_t done);
inline void Pause() {
    std::this_thread::sleep_for(BatchPause);
}
template <typename Work>
void RunBatches(std::size_t count, Work&& work) {
    for (std::size_t first = 0; first < count; first += BatchSize) {
        const auto size = std::min(BatchSize, count - first);
        RunBatch(
            size, [&](std::size_t i) { work(first + i); },
            [&](std::size_t done) { Update(first + done); });
        if (first + size < count)
            Pause();
    }
}
} // namespace VideoCore::StartupCompile
