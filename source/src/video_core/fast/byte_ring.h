// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Adapted from bbport's vk_draw_pipe.h: SPSC byte packets, wrap marker and
// release publication.
#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>

namespace FastRender {
// How the consumer wakes the threads sleeping in Drain, Wait or Space.
enum class RingWake : std::uint8_t {
  // FAST_CPU_LAZY_WAKE=0: after every packet while anyone sleeps. Each sleeper
  // wakes, finds its target not consumed yet and sleeps again, and the consumer
  // pays a kernel call per packet (3.4% of stage B, sampled 2026-10-14).
  EveryPacket,
  // Default: once, when the lowest target a sleeper registered is consumed.
  Target,
  // FAST_CPU_LAZY_WAKE_VERIFY=1: Target's decisions, which each sleeper checks
  // once its target is consumed, plus EveryPacket's wakes, which keep the
  // sleepers going if a decision is wrong.
  Verify,
};
inline RingWake RingWakeFromEnvironment() {
  const char *on = std::getenv("FAST_CPU_LAZY_WAKE");
  if (on && on[0] == '0')
    return RingWake::EveryPacket;
  const char *verify = std::getenv("FAST_CPU_LAZY_WAKE_VERIFY");
  return verify && verify[0] == '1' ? RingWake::Verify : RingWake::Target;
}
// Read once at start-up; each ring copies it when it is made.
inline const RingWake g_ring_wake = RingWakeFromEnvironment();

// FAST_CPU_LAZY_WAKE protocol (Target and Verify; every operation is seq_cst):
//   sleeper (Drain/Wait/Space, any thread, after its spin):
//     loop { [woken: spin until consumed >= target, 4096 pauses at most;
//             done if it is]
//            e = wake_epoch; wake_at = min(wake_at, target);
//            if (consumed >= target) break; wake_epoch.wait(e); }
//   consumer, after each packet:
//     consumed = tail;
//     if (wake_at <= tail) { wake_at = none; ++wake_epoch; notify_all; }
//   and the same reset and wake early, when it takes the packet that will
//   reach wake_at (Peek): the sleeper's wake-up (about 7 us after a long
//   sleep on a 5800X3D, against 2 us when woken every packet) then overlaps
//   that packet's handler, and the sleeper spins through the rest of it.
// No lost wakeup: a sleeper sleeps only after its min (R) and a load of
// consumed (L) that saw less than its target. Let S be the consumer's first
// store of consumed >= target: L reads an older value, so R < L < S < the
// consumer's next wake_at load (W) in the single total order. W returns R's
// value or a lower one (sleepers only lower wake_at), so the consumer wakes
// everyone, unless a consumer reset to none came between R and W, and every
// reset (early ones too) wakes everyone right after it. Either way wake_epoch
// changes after R, hence after the sleeper read e, and its wait(e) returns
// (or never blocks). Only the consumer raises wake_at; a woken sleeper whose
// target is not consumed yet registers again after its spin. A sleeper that
// finds its target consumed right after registering leaves the target
// behind, which costs one spurious wake on the next packet (no kernel call
// when nobody sleeps). tests/test_ring_wake.cpp runs it.
template <std::size_t Capacity = 16u << 20> class ByteRing {
  static_assert(Capacity >= 256 && Capacity % 64 == 0);
#ifdef BBVR_NO_LOGS
  static constexpr bool kCounts = false;
#else
  static constexpr bool kCounts = true;
#endif
  static constexpr std::uint64_t kNone = UINT64_MAX;
  struct Header {
    std::uint32_t size, payload;
    std::uint64_t reserved{}; // Keep the payload 16-byte aligned.
  };
  static_assert(sizeof(Header) == 16);
  struct alignas(64) Storage {
    std::byte bytes[Capacity];
  };
  std::unique_ptr<Storage> storage = std::make_unique<Storage>(); // read-only after construction
  const RingWake wake_mode;                                        // read-only after construction
  // Producer (A) and consumer (B) fields on separate cache lines.
  alignas(64) std::uint64_t head{}; // producer only
  std::uint32_t pending{};
  alignas(64) std::atomic<std::uint64_t> published{0};
  alignas(64) std::atomic<std::uint64_t> consumed{0};
  std::uint64_t tail{}; // consumer only, same line as consumed
  mutable std::atomic<std::uint32_t> drain_waiters{0};
  // FAST_CPU_LAZY_WAKE: the lowest target a sleeper registered (kNone: none)
  // and the generation sleepers wait on. Sleepers write them only on their way
  // to sleep, so the consumer's per-packet load stays on its own line.
  mutable std::atomic<std::uint64_t> wake_at{kNone};
  mutable std::atomic<std::uint32_t> wake_epoch{0};
  std::uint64_t wakes{}, would_wake{}; // consumer counts (kCounts)
  // Sleepers' counts, and FAST_CPU_LAZY_WAKE_VERIFY's state.
  alignas(64) mutable std::atomic<std::uint64_t> sleeps{0};
  mutable std::atomic<std::uint64_t> post_wake_spin_ns{0}; // kCounts
  std::atomic<std::uint64_t> lazy_wakes{0}; // consumer: Target's wakes
  std::atomic<std::uint64_t> decided{0};    // consumer: tail once decided on
  mutable std::atomic<std::uint64_t> verified{0}, mismatches{0};
  mutable std::atomic<std::uint64_t> lost_target{0}, lost_decided{0};
  bool drop_target_wakes{}; // tests only: Verify's check must find these
  bool WaitConsumed(std::uint64_t target) const {
    for (unsigned spin = 0; spin < 4096; ++spin) {
      if (Reached(target))
        return false;
      _mm_pause();
    }
    for (unsigned yield = 0; yield < 8; ++yield) {
      if (Reached(target))
        return false;
      std::this_thread::yield();
    }
    drain_waiters.fetch_add(1, std::memory_order_seq_cst);
    bool lost = false;
    if (wake_mode == RingWake::EveryPacket) {
      auto seen = consumed.load(std::memory_order_seq_cst);
      while (seen < target) {
        consumed.wait(seen, std::memory_order_relaxed);
        seen = consumed.load(std::memory_order_seq_cst);
      }
    } else {
      lost = SleepUntil(target);
    }
    drain_waiters.fetch_sub(1, std::memory_order_seq_cst);
    return lost;
  }
  // The sleeper's side of the protocol above; true when Verify finds that the
  // consumer reached and decided on `target` without a targeted wake.
  bool SleepUntil(std::uint64_t target) const {
    bool slept = false;
    std::uint64_t lazy_seen = 0; // Verify: lazy_wakes before the last sleep's min
    for (;;) {
      if (slept) {
        // Woken at the target, or early, while the consumer runs the packet
        // that reaches it: spin through that packet rather than sleep again.
        // Leaving here registers nothing (no target left behind). The spin is
        // busy time on the sleeper's thread (post_wake_spin_ns).
        [[maybe_unused]] std::chrono::steady_clock::time_point spin_start{};
        if constexpr (kCounts)
          spin_start = std::chrono::steady_clock::now();
        for (unsigned spin = 0; spin < 4096 && !Reached(target); ++spin)
          _mm_pause();
        if constexpr (kCounts)
          post_wake_spin_ns.fetch_add(
              static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - spin_start)
                      .count()),
              std::memory_order_relaxed);
        if (Reached(target))
          break;
      }
      const auto epoch = wake_epoch.load(std::memory_order_seq_cst);
      const auto lazy = wake_mode == RingWake::Verify
                            ? lazy_wakes.load(std::memory_order_seq_cst)
                            : 0;
      auto lowest = wake_at.load(std::memory_order_seq_cst);
      while (target < lowest &&
             !wake_at.compare_exchange_weak(lowest, target,
                                            std::memory_order_seq_cst)) {
      }
      if (consumed.load(std::memory_order_seq_cst) >= target)
        break;
      if (kCounts && !slept)
        sleeps.fetch_add(1, std::memory_order_relaxed);
      slept = true;
      lazy_seen = lazy;
      wake_epoch.wait(epoch, std::memory_order_seq_cst);
    }
    return slept && wake_mode == RingWake::Verify &&
           LostWake(target, lazy_seen);
  }
  // Verify, after the last sleep: the consumer decides right after each store
  // of consumed, so wait for its decision on `target`. A targeted wake after
  // the last sleep's min (lazy_wakes moved) is what the protocol promises;
  // EveryPacket's wakes are what woke this sleeper either way. A wake made for
  // another sleeper also counts: it reset wake_at, so this one had to and did
  // register again, or found its target consumed.
  bool LostWake(std::uint64_t target, std::uint64_t lazy_seen) const {
    for (unsigned spin = 0; decided.load(std::memory_order_seq_cst) < target;
         ++spin) {
      if (spin < 4096)
        _mm_pause();
      else
        std::this_thread::yield();
    }
    verified.fetch_add(1, std::memory_order_relaxed);
    if (lazy_wakes.load(std::memory_order_seq_cst) != lazy_seen)
      return false;
    mismatches.fetch_add(1, std::memory_order_relaxed);
    lost_target.store(target, std::memory_order_relaxed);
    lost_decided.store(decided.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
    return true;
  }
  void PublishConsumed() {
    // SC publication/recheck closes the race between announcing sleep and
    // the consumer deciding whether a notification is needed.
    consumed.store(tail, std::memory_order_seq_cst);
    if (wake_mode == RingWake::EveryPacket) {
      if (drain_waiters.load(std::memory_order_seq_cst))
        consumed.notify_all();
      return;
    }
    if constexpr (kCounts)
      would_wake += drain_waiters.load(std::memory_order_relaxed) != 0;
    const bool reached = wake_at.load(std::memory_order_seq_cst) <= tail;
    if (wake_mode == RingWake::Verify) {
      VerifyPublish(reached);
      return;
    }
    if (reached)
      WakeTargets();
  }
  // A registered target is (or, from Peek, is about to be) consumed: drop the
  // registrations and wake every sleeper; the others register again.
  void WakeTargets() {
    wake_at.store(kNone, std::memory_order_seq_cst);
    if (wake_mode == RingWake::Verify && !drop_target_wakes)
      lazy_wakes.store(lazy_wakes.load(std::memory_order_relaxed) + 1,
                       std::memory_order_seq_cst);
    WakeSleepers();
  }
  void WakeSleepers() {
    if constexpr (kCounts)
      ++wakes;
    wake_epoch.fetch_add(1, std::memory_order_seq_cst);
    wake_epoch.notify_all();
  }
  // Verify: Target's decisions are recorded (WakeTargets), EveryPacket's rule
  // wakes the sleepers on the other packets.
  void VerifyPublish(bool reached) {
    if (reached)
      WakeTargets();
    else if (drain_waiters.load(std::memory_order_seq_cst))
      WakeSleepers();
    decided.store(tail, std::memory_order_seq_cst);
  }
  void Space(std::uint64_t at, std::size_t bytes) {
    if (at + bytes > Capacity)
      WaitConsumed(at + bytes - Capacity);
  }

public:
  ByteRing() : ByteRing(g_ring_wake) {}
  explicit ByteRing(RingWake mode) : wake_mode{mode} {}
  std::byte *Begin(std::uint32_t payload) {
    if (!payload || payload > Capacity / 4 - sizeof(Header))
      throw std::length_error("draw packet exceeds ring limit");
    const auto size = std::uint32_t((payload + sizeof(Header) + 63) & ~63u);
    const auto offset = head % Capacity;
    if (offset + size > Capacity) {
      Space(head, Capacity - offset);
      const Header wrap{};
      std::memcpy(storage->bytes + offset, &wrap, sizeof(wrap));
      head += Capacity - offset;
      // Publish wrap together with the payload, so sleeping consumers cannot
      // miss a wake.
    }
    Space(head, size);
    const Header header{size, payload};
    std::memcpy(storage->bytes + head % Capacity, &header, sizeof(header));
    pending = size;
    return storage->bytes + head % Capacity + sizeof(Header);
  }
  void Commit() {
    head += pending;
    published.store(head, std::memory_order_seq_cst);
  }
  std::span<const std::byte> Peek() {
    while (tail != published.load(std::memory_order_acquire)) {
      Header header;
      std::memcpy(&header, storage->bytes + tail % Capacity, sizeof(header));
      if (header.size) {
        // FAST_CPU_LAZY_WAKE's early wake (a plain load: PublishConsumed's
        // check is the guarantee).
        if (wake_mode != RingWake::EveryPacket &&
            wake_at.load(std::memory_order_relaxed) <= tail + header.size)
          WakeTargets();
        return {storage->bytes + tail % Capacity + sizeof(Header),
                header.payload};
      }
      tail += Capacity - tail % Capacity;
      PublishConsumed();
    }
    return {};
  }
  void Release() {
    Header header;
    std::memcpy(&header, storage->bytes + tail % Capacity, sizeof(header));
    tail += header.size;
    PublishConsumed();
  }
  std::uint64_t Published() const {
    return published.load(std::memory_order_seq_cst);
  }
  bool Reached(std::uint64_t target) const {
    return consumed.load(std::memory_order_acquire) >= target;
  }
  // Drain and Wait return true when FAST_CPU_LAZY_WAKE_VERIFY found a lost
  // wake (the sleeper was woken by EveryPacket's rule instead).
  bool Drain() const { return WaitConsumed(Published()); }
  // Partial drain: wait only until the consumer has passed `target`.
  bool Wait(std::uint64_t target) const { return WaitConsumed(target); }

  RingWake WakeMode() const { return wake_mode; }
  // FAST_CPU_LAZY_WAKE's counts, compiled out with BBVR_NO_LOGS (Verify's
  // stay). wakes and would_wake are the consumer's own: read them there.
  struct WakeCounts {
    std::uint64_t sleeps;     // waits that slept
    std::uint64_t wakes;      // the consumer's wakes (Verify: with
                              // EveryPacket's)
    std::uint64_t would_wake; // packets consumed while someone slept: the
                              // wakes EveryPacket makes
    std::uint64_t verified;   // Verify: sleeps whose target was checked
    std::uint64_t mismatches; // Verify: ... and found without a targeted wake
    // The sleepers' spins after a wake (busy time on their threads, mostly
    // Stage A's: its busy figures rise with it while fps may improve).
    std::uint64_t post_wake_spin_ns;
  };
  WakeCounts Counts() const {
    return {sleeps.load(std::memory_order_relaxed), wakes, would_wake,
            verified.load(std::memory_order_relaxed),
            mismatches.load(std::memory_order_relaxed),
            post_wake_spin_ns.load(std::memory_order_relaxed)};
  }
  // Verify, from any thread: the target of the latest lost wake, the consumed
  // position the consumer had decided on when its sleeper checked, and the
  // checks so far.
  struct LostWakeInfo {
    std::uint64_t target, decided, mismatches, verified;
  };
  LostWakeInfo LastLostWake() const {
    return {lost_target.load(std::memory_order_relaxed),
            lost_decided.load(std::memory_order_relaxed),
            mismatches.load(std::memory_order_relaxed),
            verified.load(std::memory_order_relaxed)};
  }
  // Tests only (Verify): the consumer records no targeted wake, which the
  // sleepers' check must report; its wakes still happen.
  void DropTargetWakesForTest() { drop_target_wakes = true; }
};
} // namespace FastRender
