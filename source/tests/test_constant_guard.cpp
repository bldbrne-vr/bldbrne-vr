// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_CPU_GUARD (scratchpad/fast-renderer/constant_write_guard.h, used by
// Rasterizer::PrepareConstants; src/video_core/texture_cache/download_log.h, used by
// TextureCache::DownloadImageMemory and Rasterizer::CheckGuardConstants):
// - PendingWriteGuard refuses exactly the bytes a write still queued (B has not passed its
//   ticket), a signal not yet written, or a queued unknown writer covers, against a brute-force
//   model, over random writes of every size class, signals and completion orders; with
//   Match::Pages exactly the bytes whose 4 KiB pages such a write touches (signals by bytes);
// - each of its byte-matched refusals is also one of the old guard's (ConstantWriteGuard, merged
//   and table modes) given the same calls, so FAST_CPU_GUARD only ever accepts more there;
// - the old guard's Describe names the group that refused (statistics);
// - DownloadLog reports every download begun after a snapshot, and a snapshot taken while one
//   was writing as unknown; DownloadLogReader answers as DownloadLog::Since does for
//   non-decreasing snapshots, taking the log's lock once per download.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <random>
#include <span>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "scratchpad/fast-renderer/constant_write_guard.h"
#include "video_core/texture_cache/download_log.h"

namespace {

using FastRender::ConstantWriteGuard;
using FastRender::PendingWriteGuard;
using Hit = PendingWriteGuard::Hit;

// B's progress as the guard sees it: tickets up to `passed` are done; signals up to `encoded`
// exist and up to `written` have been written. Asked live, like Rasterizer::GuardProgress.
struct Progress {
    std::uint64_t passed = 0, encoded = 0, written = 0;
    bool Pending(std::uint64_t ticket) const {
        return ticket && ticket > passed;
    }
    std::uint64_t SignalsEncoded() const {
        return encoded;
    }
    bool SignalWritten(std::uint64_t sequence) const {
        return written >= sequence;
    }
};

// The rule itself, by brute force. A signal's sequence is the one B gives it when it runs the
// signal's task; the guard learns it later (the signals encoded when it first sees the task
// passed), so it may wait for a few more: `lower` refuses exactly, `upper` as late as possible.
struct Model {
    struct Write {
        std::uint64_t begin, end, ticket;
    };
    struct Signal {
        std::uint64_t begin, end, ticket, sequence; // the sequence B gives it when it runs
    };
    std::vector<Write> writes;
    std::vector<Signal> signals;
    std::uint64_t unknown = 0;
    // `pages`: a write refuses when it touches a 4 KiB page of the bytes.
    bool Refuses(std::uint64_t address, std::uint64_t bytes, const Progress& p, bool upper,
                 bool pages = false) const {
        if (unknown && p.Pending(unknown))
            return true;
        const std::uint64_t end = address + bytes;
        const auto page = [](std::uint64_t at) { return at >> 12; };
        for (const auto& w : writes)
            if ((pages ? page(w.begin) <= page(end - 1) && page(address) <= page(w.end - 1)
                       : w.begin < end && address < w.end) &&
                p.Pending(w.ticket))
                return true;
        for (const auto& s : signals)
            if (s.begin < end && address < s.end &&
                (p.Pending(s.ticket) || p.written < (upper ? p.encoded : s.sequence)))
                return true;
        return false;
    }
};

std::uint64_t RandomSize(std::mt19937_64& rng) {
    switch (rng() % 8) {
    case 0:
    case 1:
    case 2:
        return 1 + rng() % 4096; // class 0
    case 3:
    case 4:
        return 4097 + rng() % (256 * 1024 - 4096); // class 1
    case 5:
    case 6:
        return 256 * 1024 + 1 + rng() % (8u << 20); // class 2
    default:
        return (std::uint64_t{64} << 20) + 1 + rng() % (std::uint64_t{3} << 30); // big
    }
}

TEST(ConstantGuard, PendingWritesAreExact) {
    std::uint64_t page_only = 0; // refused by page only, all seeds
    for (std::uint32_t seed = 1; seed <= 6; ++seed) {
        std::mt19937_64 rng(seed);
        PendingWriteGuard guard;
        Model model;
        Progress p;
        std::uint64_t next = 1;
        std::uint64_t refusals = 0, checks = 0;
        const std::uint64_t base = 0x2'0000'0000ull;
        const std::uint64_t span = std::uint64_t{1} << (seed % 2 ? 26 : 34);
        for (int i = 0; i < 60000; ++i) {
            const auto op = rng() % 32;
            if (op < 6) {
                const std::uint64_t bytes = RandomSize(rng);
                const std::uint64_t address = base + (rng() % span) / 16 * 16;
                const std::uint64_t ticket = next++;
                guard.AddWrite(address, bytes, ticket);
                model.writes.push_back({address, address + bytes, ticket});
                // The same buffer again later (dedupe keeps the newest ticket).
                if (rng() % 4 == 0) {
                    const std::uint64_t again = next++;
                    guard.AddWrite(address, bytes, again);
                    model.writes.push_back({address, address + bytes, again});
                }
            } else if (op == 6) {
                const std::uint64_t address = base + (rng() % span) / 8 * 8;
                const std::uint64_t bytes = rng() % 2 ? 4 : 8;
                const std::uint64_t ticket = next++;
                guard.AddSignal(address, bytes, ticket);
                model.signals.push_back({address, address + bytes, ticket, ~std::uint64_t{0}});
            } else if (op == 7 && rng() % 16 == 0) {
                const std::uint64_t ticket = next++;
                guard.Unknown(ticket);
                model.unknown = std::max(model.unknown, ticket);
            } else if (op < 12) {
                // B advances; each signal task it passes is encoded then.
                p.passed = std::min(next - 1, p.passed + 1 + rng() % 6);
                for (auto& s : model.signals)
                    if (s.sequence == ~std::uint64_t{0} && !p.Pending(s.ticket))
                        s.sequence = ++p.encoded;
            } else if (op < 14) {
                p.written = std::min(p.encoded, p.written + 1 + rng() % 3); // the recorder
            } else {
                const std::uint64_t bytes = 1 + rng() % 16384;
                const std::uint64_t address = base + (rng() % span) / 4 * 4;
                const bool pages = rng() % 2 == 0;
                const Hit hit = guard.Check(address, bytes, p,
                                            pages ? PendingWriteGuard::Match::Pages
                                                  : PendingWriteGuard::Match::Bytes);
                const bool got = hit != Hit::None;
                ASSERT_TRUE(got || !model.Refuses(address, bytes, p, false, pages))
                    << "missed: seed " << seed << " step " << i << " address " << address
                    << " bytes " << bytes << " pages " << pages;
                ASSERT_TRUE(!got || model.Refuses(address, bytes, p, true, pages))
                    << "refused: seed " << seed << " step " << i << " address " << address
                    << " bytes " << bytes << " pages " << pages;
                ASSERT_TRUE(hit != Hit::Page || pages) << "seed " << seed << " step " << i;
                refusals += got;
                page_only += got && !model.Refuses(address, bytes, p, true);
                ++checks;
            }
        }
        EXPECT_GT(refusals, 0u);
        EXPECT_LT(refusals, checks);
    }
    EXPECT_GT(page_only, 0u); // page matching refused more
}

TEST(ConstantGuard, PagesMatchWhatTheirWritesMark) {
    PendingWriteGuard guard;
    Progress p;
    constexpr auto pages = PendingWriteGuard::Match::Pages;
    guard.AddWrite(0x1000, 0x100, 1); // a storage buffer written in page 0x1000
    // A 272-byte lighting constant later in that page: the bytes miss the write, the page not.
    EXPECT_EQ(guard.Check(0x1800, 272, p), Hit::None);
    EXPECT_EQ(guard.Check(0x1800, 272, p, pages), Hit::Page);
    EXPECT_EQ(guard.Check(0x1080, 16, p, pages), Hit::Write);
    EXPECT_EQ(guard.Check(0x2000, 16, p, pages), Hit::None); // the next page
    EXPECT_EQ(guard.Check(0xFF0, 16, p, pages), Hit::None);  // the page before
    EXPECT_EQ(guard.Check(0xFF0, 17, p, pages), Hit::Write); // reaching into it
    EXPECT_EQ(guard.Check(0x10F0, 16, p, pages), Hit::Write); // its last bytes
    EXPECT_EQ(guard.Check(0x1100, 16, p, pages), Hit::Page);  // right after it
    guard.AddWrite(0x5FFF, 2, 2); // straddling pages 0x5000 and 0x6000
    EXPECT_EQ(guard.Check(0x5000, 4, p, pages), Hit::Page);
    EXPECT_EQ(guard.Check(0x6FFC, 4, p, pages), Hit::Page);
    EXPECT_EQ(guard.Check(0x7000, 4, p, pages), Hit::None);
    // A class 1 write found from a query in the page below its begin.
    guard.AddWrite(0x40100, 0x10000, 3);
    EXPECT_EQ(guard.Check(0x40000, 8, p, pages), Hit::Page);
    EXPECT_EQ(guard.Check(0x50FF8, 8, p, pages), Hit::Page);
    EXPECT_EQ(guard.Check(0x51000, 8, p, pages), Hit::None);
    // Signals stay byte-exact.
    guard.AddSignal(0x9000, 8, 4);
    EXPECT_EQ(guard.Check(0x9010, 4, p, pages), Hit::None);
    EXPECT_EQ(guard.Check(0x9004, 4, p, pages), Hit::SignalQueued);
    // The top page stops at the top of the address space.
    guard.AddWrite(~std::uint64_t{0} - 0x20, 0x10, 5);
    EXPECT_EQ(guard.Check(~std::uint64_t{0} - 0x200, 0x10, p, pages), Hit::Page);
    EXPECT_EQ(guard.Check(~std::uint64_t{0} - 0x10, 0x10, p, pages), Hit::Page);
    p.passed = 5;
    p.encoded = p.written = 5;
    EXPECT_EQ(guard.Check(0x1800, 272, p, pages), Hit::None); // passed: B's own page test now
}

TEST(ConstantGuard, SignalsWaitForTheRecorder) {
    PendingWriteGuard guard;
    Progress p;
    guard.AddSignal(0x1000, 8, 5);
    EXPECT_EQ(guard.Check(0x1004, 4, p), Hit::SignalQueued);
    EXPECT_EQ(guard.Check(0x1008, 4, p), Hit::None); // the next bytes
    p.passed = 5;
    p.encoded = 3; // B ran the fence task: it is signal 3
    EXPECT_EQ(guard.Check(0x1000, 4, p), Hit::SignalUnwritten);
    p.encoded = 9; // later signals do not move the one already resolved
    p.written = 3;
    EXPECT_EQ(guard.Check(0x1000, 4, p), Hit::None);
    // The same label again: a new signal, resolved anew.
    guard.AddSignal(0x1000, 8, 7);
    EXPECT_EQ(guard.Check(0x1000, 4, p), Hit::SignalQueued);
    p.passed = 7;
    EXPECT_EQ(guard.Check(0x1000, 4, p), Hit::SignalUnwritten);
    p.written = 9;
    EXPECT_EQ(guard.Check(0x1000, 4, p), Hit::None);
}

TEST(ConstantGuard, WritesExpireAndBigOnesStayExact) {
    PendingWriteGuard guard;
    Progress p;
    guard.AddWrite(0x10000, 64, 1);
    guard.AddWrite(0x10040, 64, 3); // touching: still two writes
    EXPECT_EQ(guard.Check(0x10000, 16, p), Hit::Write);
    p.passed = 1;
    EXPECT_EQ(guard.Check(0x10000, 64, p), Hit::None); // the old guard merged it with 3
    EXPECT_EQ(guard.Check(0x1003F, 2, p), Hit::Write);
    guard.AddWrite(0x8'0000'0000ull, 0xFFFF'FFFFull, 4); // a 4 GiB descriptor
    EXPECT_EQ(guard.Check(0x8'8000'0000ull, 4, p), Hit::Big);
    EXPECT_EQ(guard.Check(0x7'FFFF'FFF0ull, 16, p), Hit::None);
    guard.AddWrite(~std::uint64_t{0} - 8, 64, 5); // wrapping: up to the top
    EXPECT_EQ(guard.Check(~std::uint64_t{0} - 4, 4, p), Hit::Big);
    EXPECT_EQ(guard.UnknownAdds(), 1u);
    guard.Unknown(6);
    EXPECT_EQ(guard.Check(0x100, 4, p), Hit::Unknown);
    p.passed = 6;
    EXPECT_EQ(guard.Check(0x100, 4, p), Hit::None);
    EXPECT_EQ(guard.Check(0x8'8000'0000ull, 4, p), Hit::None);
    for (int i = 0; i < 128; ++i) // pruned: nothing left once B passed everything
        guard.Check(0x100, 4, p);
    EXPECT_EQ(guard.Count(), 0u);
}

// Every refusal of the new rule is one of the old guard's, fed as the rasterizer feeds it
// (signals and image footprints as permanent ranges, the footprints not given to the new rule).
TEST(ConstantGuard, NewRefusalsAreOldRefusals) {
    for (const bool table : {false, true}) {
        std::mt19937_64 rng(table ? 77 : 78);
        ConstantWriteGuard old_guard{table};
        PendingWriteGuard guard;
        Progress p;
        std::uint64_t next = 1, freed = 0, both = 0;
        const auto pending = [&](std::uint64_t t) { return p.Pending(t); };
        const std::uint64_t base = 0x4'0000'0000ull;
        for (int i = 0; i < 200000; ++i) {
            const auto op = rng() % 16;
            const std::uint64_t address = base + (rng() % (1u << 24)) / 16 * 16;
            if (op < 4) {
                const std::uint64_t bytes = op == 3 ? RandomSize(rng) : 1 + rng() % 4096;
                const std::uint64_t ticket = next++;
                old_guard.Add(address, bytes, ticket);
                guard.AddWrite(address, bytes, ticket);
            } else if (op == 4 && rng() % 4 == 0) {
                const std::uint64_t bytes = 1 + rng() % (1u << 20); // a render target
                old_guard.Add(address, bytes, next, ConstantWriteGuard::KindWrite);
                old_guard.Add(address, bytes, UINT64_MAX, ConstantWriteGuard::KindImage);
                ++next;
            } else if (op == 5 && rng() % 8 == 0) {
                old_guard.Add(address, 8, UINT64_MAX, ConstantWriteGuard::KindSignal);
                guard.AddSignal(address, 8, next - 1);
            } else if (op == 6 && rng() % 64 == 0) {
                old_guard.Unknown(next);
                guard.Unknown(next);
                ++next;
            } else if (op < 10) {
                p.passed = std::min(next - 1, p.passed + rng() % 5);
                p.encoded = p.written = p.passed; // signals written as soon as run, here
            } else {
                const std::uint64_t bytes = 1 + rng() % 16384;
                const bool old_refuses = old_guard.Check(address, bytes, pending) !=
                                         ConstantWriteGuard::Hit::None;
                const bool new_refuses = guard.Check(address, bytes, p) != Hit::None;
                ASSERT_TRUE(old_refuses || !new_refuses) << "table " << table << " step " << i;
                freed += old_refuses && !new_refuses;
                both += old_refuses && new_refuses;
            }
        }
        EXPECT_GT(freed, 0u);
        EXPECT_GT(both, 0u);
    }
}

TEST(ConstantGuard, DescribeNamesTheRefusingGroup) {
    ConstantWriteGuard guard;
    std::uint64_t passed = 0;
    const auto pending = [&](std::uint64_t t) { return t > passed; };
    guard.Add(0x1000, 0x100, 1);                                         // ticketed
    guard.Add(0x8000, 0x100, UINT64_MAX, ConstantWriteGuard::KindImage); // a footprint
    guard.Add(0x8100, 0x40, 2);                       // touches it: becomes permanent
    guard.Add(0x20000, 8, UINT64_MAX, ConstantWriteGuard::KindSignal); // a label
    using OldHit = ConstantWriteGuard::Hit;
    ASSERT_EQ(guard.Check(0x1010, 4, pending), OldHit::Range);
    auto cause = guard.Describe(0x1010, 4, pending);
    EXPECT_FALSE(cause.permanent);
    EXPECT_FALSE(cause.stale);
    passed = 1;
    cause = guard.Describe(0x1010, 4, pending);
    EXPECT_TRUE(cause.stale);
    cause = guard.Describe(0x8120, 4, pending);
    EXPECT_TRUE(cause.permanent);
    EXPECT_EQ(cause.kinds, ConstantWriteGuard::KindImage | ConstantWriteGuard::KindWrite);
    cause = guard.Describe(0x20000, 8, pending);
    EXPECT_TRUE(cause.permanent);
    EXPECT_EQ(cause.kinds, std::uint32_t{ConstantWriteGuard::KindSignal});
}

TEST(ConstantGuard, DownloadLogReportsLaterDownloads) {
    VideoCore::DownloadLog log;
    const auto quiet = log.Snapshot();
    EXPECT_TRUE(log.Unchanged(quiet));
    log.Begin(0x1000, 0x100);
    const auto busy = log.Snapshot(); // a write in progress: nothing is known
    EXPECT_FALSE(log.Unchanged(busy));
    EXPECT_FALSE(log.Since(busy, [](std::uint64_t, std::uint64_t) {}));
    log.End();
    const auto after = log.Snapshot();
    EXPECT_TRUE(log.Unchanged(after));
    EXPECT_FALSE(log.Unchanged(quiet));
    std::vector<std::pair<std::uint64_t, std::uint64_t>> seen;
    ASSERT_TRUE(log.Since(quiet, [&](std::uint64_t a, std::uint64_t b) { seen.emplace_back(a, b); }));
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].first, 0x1000u);
    EXPECT_EQ(seen[0].second, 0x100u);
    for (std::uint64_t i = 0; i < VideoCore::DownloadLog::Capacity; ++i) {
        log.Begin(0x2000 + i, 1);
        log.End();
    }
    EXPECT_FALSE(log.Since(quiet, [](std::uint64_t, std::uint64_t) {})); // lost track
    seen.clear();
    EXPECT_TRUE(log.Since(after, [&](std::uint64_t a, std::uint64_t b) { seen.emplace_back(a, b); }));
    EXPECT_EQ(seen.size(), VideoCore::DownloadLog::Capacity);
}

// DownloadLogReader against every download made: for snapshots that only grow (with Busy ones and
// runs of downloads longer than the log among them) it reports exactly the ranges begun since,
// whenever DownloadLog::Since can (and sometimes more often: it keeps what it copied before the
// log moved on), and never a wrong list.
TEST(ConstantGuard, DownloadLogReaderMatchesSince) {
    for (std::uint32_t seed = 1; seed <= 4; ++seed) {
        std::mt19937_64 rng(seed);
        VideoCore::DownloadLog log;
        VideoCore::DownloadLogReader reader;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> all; // every download, in order
        std::vector<std::uint64_t> snapshots; // queued packets' snapshots, oldest first
        std::uint64_t asked = 0, unknown = 0, ranges = 0, kept = 0;
        for (int i = 0; i < 20000; ++i) {
            const auto op = rng() % 16;
            if (op < 2) {
                const auto run = rng() % 8 == 0 ? 1 + rng() % 100 : 1;
                for (std::uint64_t n = 0; n < run; ++n) {
                    all.emplace_back(0x10000 + (rng() % 4096) * 64, 1 + rng() % 0x4000);
                    log.Begin(all.back().first, all.back().second);
                    if (rng() % 64 == 0) // a snapshot while one is writing
                        snapshots.push_back(log.Snapshot());
                    log.End();
                }
            } else if (op < 8) {
                snapshots.push_back(log.Snapshot()); // Stage A, a packet's constants
            } else if (!snapshots.empty()) {
                // Stage B takes the oldest; snapshots never decrease along the queue.
                const auto snapshot = snapshots.front();
                snapshots.erase(snapshots.begin());
                const bool since = log.Since(snapshot, [](std::uint64_t, std::uint64_t) {});
                std::span<const VideoCore::DownloadLogReader::Range> got;
                const bool known = reader.Since(log, snapshot, got);
                ASSERT_TRUE(known || !since) << "seed " << seed << " step " << i;
                ASSERT_TRUE(known || (snapshot & VideoCore::DownloadLog::Busy) ||
                            all.size() - snapshot > VideoCore::DownloadLog::Capacity)
                    << "seed " << seed << " step " << i;
                if (known) {
                    ASSERT_FALSE(snapshot & VideoCore::DownloadLog::Busy);
                    ASSERT_TRUE(std::ranges::equal(
                        got, std::span{all}.subspan(static_cast<std::size_t>(snapshot))))
                        << "seed " << seed << " step " << i;
                }
                ++asked;
                unknown += !known;
                kept += known && !since;
                ranges += got.size();
            }
        }
        EXPECT_GT(asked, 0u);
        EXPECT_GT(unknown, 0u);
        EXPECT_GT(ranges, 0u);
        EXPECT_GT(kept, 0u) << "seed " << seed;
    }
}

// Downloads on two threads while a reader snapshots: while a snapshot without Busy still holds
// (Unchanged), no download is between its Begin and End.
TEST(ConstantGuard, DownloadLogSnapshotsAreQuiet) {
    VideoCore::DownloadLog log;
    std::atomic<std::uint32_t> writing{0};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> violations{0};
    const auto writer = [&] {
        for (int i = 0; i < 20000 && !stop; ++i) {
            log.Begin(0x1000, 16);
            writing.fetch_add(1, std::memory_order_seq_cst);
            writing.fetch_sub(1, std::memory_order_seq_cst);
            log.End();
        }
    };
    std::thread a(writer), b(writer);
    for (int i = 0; i < 200000; ++i) {
        const auto snapshot = log.Snapshot();
        if (!(snapshot & VideoCore::DownloadLog::Busy) && log.Unchanged(snapshot) &&
            writing.load(std::memory_order_seq_cst) != 0 && log.Unchanged(snapshot))
            violations.fetch_add(1);
    }
    stop = true;
    a.join();
    b.join();
    EXPECT_EQ(violations.load(), 0u);
}

} // namespace
