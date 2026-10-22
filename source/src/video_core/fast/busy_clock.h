// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// FAST_CPU_IDLE_CLOCK: stage B's busy time from the ends of its busy stretches
// instead of two timestamps around every packet's handler (bbport times only
// its busy cycles too). A window closes when B goes idle and every 64 packets,
// so the FAST stats see the totals move within a few hundred microseconds.
#pragma once
#include <cstdint>

namespace FastRender {
struct BusyClock {
  static constexpr std::uint32_t kWindowPackets = 64;
  struct Window {
    std::uint64_t ticks;   // since the window opened: the handlers plus the
                           // ring's per-packet bookkeeping between them
    std::uint32_t packets; // handled in it
    std::uint64_t timed;   // FAST_CPU_IDLE_CLOCK_VERIFY: the handlers' own
                           // ticks, which `ticks` must cover
  };
  std::uint64_t since{};   // when the open window opened
  std::uint32_t packets{}; // handled in the open window
  std::uint64_t timed{};   // verify: the handlers' own ticks in it
  // B found a packet after being idle.
  void Open(std::uint64_t now) {
    since = now;
    packets = 0;
    timed = 0;
  }
  // One packet handled; true when the window is full and should be closed.
  bool Packet() { return ++packets == kWindowPackets; }
  // Closes the open window at `now` (B goes idle, or the window is full) and
  // opens the next one there.
  Window Close(std::uint64_t now) {
    const Window window{now - since, packets, timed};
    Open(now);
    return window;
  }
};
} // namespace FastRender
