// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Opt-in B3 host uploads: workers only touch resolved, protected byte spans.
#pragma once
#include <chrono>
#include "video_core/fast/host_copy_queue.h"
#include "common/thread.h"
#include "video_core/fast_render.h"
namespace FastRender {
inline std::atomic<HostCopyQueue*> active_host_copies{};
inline std::mutex host_copy_stats_mutex;
inline HostCopyQueue& HostCopies() {
    struct Owner {
        HostCopyQueue queue{HostCopyQueue::CopyBytes, [](unsigned index) {
                                Common::SetCurrentThreadName(index ? "fast:Copy1" : "fast:Copy0");
                                RegisterThread(index ? Role::Copy1 : Role::Copy0);
                            }};
        Owner() {
            active_host_copies.store(&queue, std::memory_order_release);
        }
        ~Owner() {
            std::scoped_lock lock{host_copy_stats_mutex};
            queue.WaitAll();
            active_host_copies.store(nullptr, std::memory_order_release);
        }
    };
    static Owner owner;
    return owner.queue;
}
enum class CopyWait : unsigned { Protection, Mapping, Submit, Count };
inline std::array<std::atomic<u64>, 3> copy_wait_calls{}, copy_wait_ns{};
inline void WaitHostCopies(CopyWait reason = CopyWait::Protection, u64 address = 0, u64 bytes = 0) {
    auto* queue = active_host_copies.load(std::memory_order_acquire);
    if (!queue || !queue->Pending())
        return;
    const auto ticket = bytes ? queue->SourcePrefix(address, bytes) : queue->Flush();
    if (queue->Completed() >= ticket)
        return;
    const auto begin = std::chrono::steady_clock::now();
    queue->Wait(ticket);
    copy_wait_calls[unsigned(reason)].fetch_add(1, std::memory_order_relaxed);
    copy_wait_ns[unsigned(reason)].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now() - begin)
                                                 .count(),
                                             std::memory_order_relaxed);
}
inline u64 FlushHostCopies() {
    if (auto* queue = active_host_copies.load(std::memory_order_acquire))
        return queue->Pending() ? queue->Flush() : queue->Completed();
    return 0;
}
inline void WaitHostCopies(u64 ticket) {
    if (!ticket || HostCopies().Completed() >= ticket)
        return;
    const auto begin = std::chrono::steady_clock::now();
    HostCopies().Wait(ticket);
    copy_wait_calls[unsigned(CopyWait::Submit)].fetch_add(1, std::memory_order_relaxed);
    copy_wait_ns[unsigned(CopyWait::Submit)].fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             begin)
            .count(),
        std::memory_order_relaxed);
}
} // namespace FastRender
