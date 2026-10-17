// SPDX-License-Identifier: GPL-2.0-or-later

// common/shared_first_mutex.h: the reader-priority shared mutex of the memory manager (the stream
// buffer uploads), the guest pthread rwlocks and the rasterizer's mapped ranges. Its fast path is one
// atomic word (writer bit + reader count); these tests pin the semantics of the old mutex + condvar
// version: readers wait only for an active writer, writers wait for no reader and no writer, the
// timed waits time out and succeed, and nothing is lost or deadlocks under contention.

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "common/shared_first_mutex.h"

namespace {

using Common::SharedFirstMutex;
using namespace std::chrono_literals;

TEST(SharedFirstMutex, ReadersShareWritersExclude) {
    SharedFirstMutex m;
    m.lock_shared();
    EXPECT_TRUE(m.try_lock_shared()); // two readers
    EXPECT_FALSE(m.try_lock());       // no writer while readers hold it
    m.unlock_shared();
    EXPECT_FALSE(m.try_lock()); // one reader left
    m.unlock_shared();
    EXPECT_TRUE(m.try_lock()); // free: a writer gets it
    EXPECT_FALSE(m.try_lock());
    EXPECT_FALSE(m.try_lock_shared()); // no reader while the writer holds it
    m.unlock();
    EXPECT_TRUE(m.try_lock_shared());
    m.unlock_shared();
}

TEST(SharedFirstMutex, ReaderIsNotBlockedByAWaitingWriter) {
    SharedFirstMutex m;
    m.lock_shared();
    std::atomic<bool> writer_in{false};
    std::thread writer([&] {
        m.lock(); // waits for the reader
        writer_in = true;
        m.unlock();
    });
    std::this_thread::sleep_for(50ms); // the writer is now waiting
    EXPECT_FALSE(writer_in.load());
    // Reader priority: a second reader still gets in while the writer waits.
    EXPECT_TRUE(m.try_lock_shared());
    m.unlock_shared();
    EXPECT_FALSE(writer_in.load());
    m.unlock_shared(); // the last reader out lets the writer in
    writer.join();
    EXPECT_TRUE(writer_in.load());
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

TEST(SharedFirstMutex, WaitingReaderWakesWhenTheWriterLeaves) {
    SharedFirstMutex m;
    m.lock();
    std::atomic<int> readers_in{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            m.lock_shared();
            ++readers_in;
            m.unlock_shared();
        });
    }
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(readers_in.load(), 0);
    m.unlock();
    for (auto& t : readers) {
        t.join();
    }
    EXPECT_EQ(readers_in.load(), 4);
}

TEST(SharedFirstMutex, TimedWaitsTimeOutAndSucceed) {
    SharedFirstMutex m;
    m.lock();
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(m.try_lock_shared_until(t0 + 30ms));
    EXPECT_FALSE(m.try_lock_until(std::chrono::steady_clock::now() + 30ms));
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 55ms);
    m.unlock();

    m.lock_shared();
    EXPECT_FALSE(m.try_lock_until(std::chrono::steady_clock::now() + 20ms));
    EXPECT_TRUE(m.try_lock_shared_until(std::chrono::steady_clock::now() + 20ms));
    m.unlock_shared();
    m.unlock_shared();

    // A timed writer that is released in time gets the lock.
    m.lock_shared();
    std::atomic<bool> got{false};
    std::thread w([&] {
        got = m.try_lock_until(std::chrono::steady_clock::now() + 2s);
        if (got) {
            m.unlock();
        }
    });
    std::this_thread::sleep_for(30ms);
    m.unlock_shared();
    w.join();
    EXPECT_TRUE(got.load());

    // A timed reader released in time gets it too.
    m.lock();
    got = false;
    std::thread r([&] {
        got = m.try_lock_shared_until(std::chrono::steady_clock::now() + 2s);
        if (got) {
            m.unlock_shared();
        }
    });
    std::this_thread::sleep_for(30ms);
    m.unlock();
    r.join();
    EXPECT_TRUE(got.load());
}

// Readers check that a writer's two-word update is never seen half done; writers count their
// sections. Mixed blocking, try and timed calls; std::shared_lock / std::unique_lock as the callers
// use it.
TEST(SharedFirstMutex, StressReadersAndWriters) {
    SharedFirstMutex m;
    long long a = 0, b = 0; // guarded: writers keep a == b
    std::atomic<long long> torn{0}, reads{0}, writes{0}, overlap{0};
    std::atomic<int> active_writers{0}, active_readers{0};
    constexpr int Readers = 6, Writers = 3, ReadIters = 60000, WriteIters = 6000;
    std::vector<std::thread> threads;
    for (int t = 0; t < Readers; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < ReadIters; ++i) {
                const int mode = (i + t) % 3;
                bool locked = true;
                if (mode == 0) {
                    m.lock_shared();
                } else if (mode == 1) {
                    locked = m.try_lock_shared();
                } else {
                    locked = m.try_lock_shared_until(std::chrono::steady_clock::now() + 1ms);
                }
                if (!locked) {
                    continue;
                }
                ++active_readers;
                if (active_writers.load() != 0) {
                    ++overlap;
                }
                if (a != b) {
                    ++torn;
                }
                ++reads;
                --active_readers;
                m.unlock_shared();
            }
        });
    }
    for (int t = 0; t < Writers; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < WriteIters; ++i) {
                const int mode = (i + t) % 3;
                bool locked = true;
                if (mode == 0) {
                    m.lock();
                } else if (mode == 1) {
                    locked = m.try_lock();
                } else {
                    locked = m.try_lock_until(std::chrono::steady_clock::now() + 1ms);
                }
                if (!locked) {
                    continue;
                }
                if (active_writers.fetch_add(1) != 0 || active_readers.load() != 0) {
                    ++overlap;
                }
                ++a;
                std::this_thread::yield();
                ++b;
                ++writes;
                active_writers.fetch_sub(1);
                m.unlock();
            }
        });
    }
    // RAII wrappers, as memory.cpp and the rasterizer use it.
    threads.emplace_back([&] {
        for (int i = 0; i < 20000; ++i) {
            if (i % 4 == 0) {
                std::unique_lock lk{m};
                if (active_writers.fetch_add(1) != 0 || active_readers.load() != 0) {
                    ++overlap;
                }
                ++a;
                ++b;
                ++writes;
                active_writers.fetch_sub(1);
            } else {
                std::shared_lock lk{m};
                if (a != b) {
                    ++torn;
                }
                ++reads;
            }
        }
    });
    for (auto& th : threads) {
        th.join();
    }
    EXPECT_EQ(torn.load(), 0);
    EXPECT_EQ(overlap.load(), 0);
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, writes.load());
    EXPECT_GT(reads.load(), 0);
    EXPECT_GT(writes.load(), 0);
    // Everything was released: a writer gets it at once.
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

} // namespace
