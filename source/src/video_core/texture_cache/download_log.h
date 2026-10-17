// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace VideoCore {

// FAST_CPU_GUARD: the guest ranges image downloads write (TextureCache::DownloadImageMemory, on
// whichever thread writes them), numbered in the order they begin. They are the only writes of
// image data to guest memory, so the constant ring's guard no longer keeps every render target's
// footprint for the whole activation (FastRender::PendingWriteGuard). Instead Stage A takes a
// Snapshot before it copies a packet's constants, and B, before binding them, asks which ranges
// a download may have written since (Since): those constants it binds the ordinary way, reading
// the guest bytes itself as it always did. Downloads are rare (memory pressure, or the
// readbackLinearImages setting), so the common test is one load (Unchanged).
class DownloadLog {
public:
    static constexpr std::uint64_t Capacity = 64;
    // Set in a snapshot taken while a download was writing: nothing is known about the bytes.
    static constexpr std::uint64_t Busy = std::uint64_t{1} << 63;

    // Around the guest-memory write of one download.
    void Begin(std::uint64_t address, std::uint64_t bytes) {
        std::scoped_lock lock{mutex};
        const std::uint64_t n = begun.load(std::memory_order_relaxed);
        ranges[n % Capacity] = {address, bytes};
        begun.store(n + 1, std::memory_order_seq_cst);
    }
    void End() {
        ended.fetch_add(1, std::memory_order_seq_cst);
    }
    // Every download counted in the result had finished writing when it was taken (ended is read
    // first: if as many had ended as had begun, none was writing), unless Busy is set.
    std::uint64_t Snapshot() const {
        const std::uint64_t done = ended.load(std::memory_order_seq_cst);
        const std::uint64_t started = begun.load(std::memory_order_seq_cst);
        return started == done ? started : started | Busy;
    }
    // No download began since `snapshot`, and none was writing when it was taken.
    bool Unchanged(std::uint64_t snapshot) const {
        return begun.load(std::memory_order_seq_cst) == snapshot;
    }
    // Calls f(address, bytes) for each download begun since `snapshot`. False when that is not
    // known (a Busy snapshot, or more downloads than the log holds): treat every byte as written.
    template <class F>
    bool Since(std::uint64_t snapshot, F&& f) const {
        if (snapshot & Busy) {
            return false;
        }
        std::scoped_lock lock{mutex};
        const std::uint64_t now = begun.load(std::memory_order_relaxed);
        if (now - snapshot > Capacity) {
            return false;
        }
        for (std::uint64_t i = snapshot; i < now; ++i) {
            f(ranges[i % Capacity].first, ranges[i % Capacity].second);
        }
        return true;
    }
    // Calls f(number, address, bytes) for each download the log still holds from number `from`
    // on, in order, and returns how many have begun (DownloadLogReader).
    template <class F>
    std::uint64_t Read(std::uint64_t from, F&& f) const {
        std::scoped_lock lock{mutex};
        const std::uint64_t now = begun.load(std::memory_order_relaxed);
        for (std::uint64_t i = std::max(from, now > Capacity ? now - Capacity : 0); i < now; ++i) {
            f(i, ranges[i % Capacity].first, ranges[i % Capacity].second);
        }
        return now;
    }

private:
    mutable std::mutex mutex;
    std::array<std::pair<std::uint64_t, std::uint64_t>, Capacity> ranges{};
    std::atomic<std::uint64_t> begun{0}, ended{0};
};

// Since for one reader whose snapshots never decrease (Stage B, which takes packets in the order
// Stage A snapshot them): the log's ranges are copied once per download and kept from the oldest
// snapshot still to come, so the packets queued behind a download (up to a frame of them) take
// the log's lock once in all, not once each.
class DownloadLogReader {
public:
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    // The ranges of the downloads begun since `snapshot` (valid until the next call), or false
    // when that is not known (DownloadLog::Since).
    bool Since(const DownloadLog& log, std::uint64_t snapshot, std::span<const Range>& out) {
        out = {};
        if (snapshot & DownloadLog::Busy) {
            return false;
        }
        if (!log.Unchanged(end)) {
            end = log.Read(end, [&](std::uint64_t number, std::uint64_t address, std::uint64_t bytes) {
                if (number != first + held.size()) { // the log moved on past ones never read
                    held.clear();
                    first = number;
                }
                held.emplace_back(address, bytes);
            });
        }
        // Every snapshot from here on is at least this one: what precedes it is never asked again.
        if (snapshot > first) {
            const std::uint64_t drop = std::min<std::uint64_t>(snapshot - first, held.size());
            held.erase(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(drop));
            first += drop;
        }
        if (snapshot < first) {
            return false; // lost track
        }
        out = held;
        return true;
    }

private:
    std::vector<Range> held; // downloads first, first + 1, ... (up to end)
    std::uint64_t first{}, end{};
};

inline DownloadLog g_download_log;

} // namespace VideoCore
