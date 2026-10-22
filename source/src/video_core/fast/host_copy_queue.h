// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// B3 host-copy engine. Integration must protect source mappings and publish
// clean/write-protected source ranges before enqueueing. GPU submits wait a
// prefix.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
namespace FastRender {
class HostCopyQueue {
public:
  using Ticket = std::uint64_t;
  struct Copy {
    void *destination;
    const void *source;
    std::size_t bytes;
  };
  using Execute = void (*)(const Copy &);
  using Start = void (*)(unsigned);
  static constexpr std::size_t BatchBytes = 512 * 1024;
  static void CopyBytes(const Copy &copy) {
    if (copy.source)
      std::memcpy(copy.destination, copy.source, copy.bytes);
    else
      std::memset(copy.destination, 0, copy.bytes);
  }
  explicit HostCopyQueue(Execute execute = CopyBytes, Start start = nullptr)
      : execute(execute), start(start) {
    for (unsigned i = 0; i < threads.size(); ++i)
      threads[i] = std::jthread([this, i] { Run(i); });
  }
  ~HostCopyQueue() {
    WaitAll();
    {
      std::scoped_lock lock{mutex};
      stopping = true;
    }
    ready.notify_all();
    for (auto &thread : threads)
      thread.join();
  }
  void Enqueue(void *destination, const void *source, std::size_t bytes) {
    auto *dst = static_cast<std::byte *>(destination);
    auto *src = static_cast<const std::byte *>(source);
    std::unique_lock lock{mutex};
    while (bytes) {
      space.wait(lock, [&] { return batches.size() < 64; });
      const auto amount = std::min(bytes, BatchBytes - pending_bytes);
      pending.push_back({dst, src, amount});
      if (src) {
        const auto address = reinterpret_cast<std::uintptr_t>(src);
        pending_begin = std::min(pending_begin, address);
        pending_end = std::max(pending_end, address + amount);
      }
      pending_bytes += amount;
      queued_bytes.fetch_add(amount, std::memory_order_relaxed);
      dirty.store(true, std::memory_order_release);
      dst += amount;
      if (src)
        src += amount;
      bytes -= amount;
      if (pending_bytes == BatchBytes || pending.size() == 128)
        Publish();
    }
  }
  // Returns an immutable completion prefix. Waiting it cannot accidentally
  // include work queued by the producer after the associated GPU submission.
  Ticket Flush() {
    std::scoped_lock lock{mutex};
    Publish();
    return issued;
  }
  // Conservative bounding interval per batch. Unrelated writes must not
  // publish a partial batch or wait for uploads from a different source range.
  Ticket SourcePrefix(std::uintptr_t address, std::size_t bytes) {
    std::scoped_lock lock{mutex};
    PruneSources();
    const auto end = address + bytes;
    const auto overlaps = [&](auto begin, auto last) {
      return begin < end && address < last;
    };
    Ticket target = 0;
    for (const auto &range : sources)
      if (overlaps(range.begin, range.end))
        target = range.ticket;
    if (pending_begin != ~std::uintptr_t{0} &&
        overlaps(pending_begin, pending_end)) {
      Publish();
      target = issued;
    }
    return target;
  }
  void Wait(Ticket target) {
    if (completed.load(std::memory_order_acquire) >= target)
      return;
    std::unique_lock lock{mutex};
    done.wait(lock, [&] {
      return completed.load(std::memory_order_acquire) >= target;
    });
  }
  void WaitAll() {
    if (!dirty.load(std::memory_order_acquire))
      return;
    Wait(Flush());
  }
  bool Pending() const { return dirty.load(std::memory_order_acquire); }
  Ticket Completed() const { return completed.load(std::memory_order_acquire); }
  std::atomic<std::uint64_t> queued_bytes{}, copied_bytes{},
      completed_batches{};

private:
  struct Batch {
    Ticket ticket;
    std::vector<Copy> copies;
  };
  struct SourceRange {
    Ticket ticket;
    std::uintptr_t begin, end;
  };
  void PruneSources() {
    const auto prefix = completed.load(std::memory_order_acquire);
    while (!sources.empty() && sources.front().ticket <= prefix)
      sources.pop_front();
  }
  void Publish() {
    if (pending.empty())
      return;
    PruneSources();
    ++issued;
    if (pending_begin != ~std::uintptr_t{0})
      sources.push_back({issued, pending_begin, pending_end});
    pending_begin = ~std::uintptr_t{0};
    pending_end = 0;
    batches.push_back({issued, std::move(pending)});
    pending = {};
    pending_bytes = 0;
    ready.notify_one();
  }
  void Run(unsigned index) {
    if (start)
      start(index);
    for (;;) {
      Batch batch;
      {
        std::unique_lock lock{mutex};
        ready.wait(lock, [&] { return stopping || !batches.empty(); });
        if (batches.empty()) {
          assert(stopping);
          return;
        }
        batch = std::move(batches.front());
        batches.pop_front();
        space.notify_all();
      }
      std::uint64_t bytes = 0;
      for (const auto &copy : batch.copies) {
        execute(copy);
        bytes += copy.bytes;
      }
      copied_bytes.fetch_add(bytes, std::memory_order_relaxed);
      completed_batches.fetch_add(1, std::memory_order_relaxed);
      {
        std::scoped_lock lock{mutex};
        finished.insert(batch.ticket);
        auto prefix = completed.load(std::memory_order_relaxed);
        while (finished.erase(prefix + 1))
          ++prefix;
        completed.store(prefix, std::memory_order_release);
        if (prefix == issued && pending.empty())
          dirty.store(false, std::memory_order_release);
      }
      done.notify_all();
    }
  }
  Execute execute;
  Start start;
  std::mutex mutex;
  std::condition_variable ready, space, done;
  std::deque<Batch> batches;
  std::deque<SourceRange> sources;
  std::uintptr_t pending_begin{~std::uintptr_t{0}}, pending_end{};
  std::vector<Copy> pending;
  std::set<Ticket> finished;
  Ticket issued{};
  std::atomic<Ticket> completed{};
  std::atomic<bool> dirty{};
  std::size_t pending_bytes{};
  bool stopping{};
  std::array<std::jthread, 2> threads;
};
} // namespace FastRender
