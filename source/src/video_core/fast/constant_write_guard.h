// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Stage-A journal: queued writers must not be read before B establishes GPU
// tracking.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>
namespace FastRender {
// bbport PendingReads layout (vk_rasterizer.h:605-641): the newest ticket per
// hashed granule, 256 B for ranges up to 4 KiB, 16 KiB up to 256 KiB, 1 MiB
// above. A lookup checks all three levels. Collisions only make a lookup more
// conservative. Nothing is pruned: a slot stops mattering once B passes it.
// Ranges over MaxBytes are not noted here (callers keep them exactly).
class GranuleTickets {
public:
  static constexpr uint32_t Levels = 3;
  static constexpr uint64_t Shifts[Levels] = {8, 14, 20};
  static constexpr uint64_t Limits[Levels - 1] = {4096, 256 * 1024};
  static constexpr uint32_t Bits = 16;
  static constexpr uint64_t MaxBytes = uint64_t{256} << 20;
  static bool Fits(uint64_t address, uint64_t bytes) {
    return bytes && bytes <= MaxBytes && address <= UINT64_MAX - bytes;
  }
  void Note(uint64_t address, uint64_t bytes, uint64_t ticket) {
    if (tables[0].empty())
      for (auto &table : tables)
        table.assign(size_t{1} << Bits, 0);
    const uint32_t level = bytes <= Limits[0] ? 0 : bytes <= Limits[1] ? 1 : 2;
    auto &table = tables[level];
    const uint64_t last = (address + bytes - 1) >> Shifts[level];
    for (uint64_t g = address >> Shifts[level]; g <= last; ++g) {
      uint64_t &slot = table[Slot(g, level)];
      slot = std::max(slot, ticket);
    }
    newest = std::max(newest, ticket);
  }
  uint64_t Newest(uint64_t address, uint64_t bytes) const {
    uint64_t result = 0;
    if (tables[0].empty())
      return result;
    for (uint32_t level = 0; level < Levels; ++level) {
      const uint64_t last = (address + bytes - 1) >> Shifts[level];
      for (uint64_t g = address >> Shifts[level]; g <= last; ++g)
        result = std::max(result, tables[level][Slot(g, level)]);
    }
    return result;
  }
  // The newest ticket ever noted: when B has passed it, no slot can match.
  uint64_t NewestAny() const { return newest; }

private:
  static uint64_t Slot(uint64_t granule, uint32_t level) {
    return ((granule * 4 + level) * 0x9E3779B97F4A7C15ull) >> (64 - Bits);
  }
  std::vector<uint64_t> tables[Levels];
  uint64_t newest{};
};

class ConstantWriteGuard {
public:
  // What a range was added for (statistics only; merged groups keep the union).
  enum Kind : uint32_t { KindWrite = 1, KindImage = 2, KindSignal = 4 };
  struct Range {
    uint64_t begin{}, end{}, ticket{};
    uint32_t kinds{};
  };
  enum class Hit { None, Unknown, Invalid, Range, Table, Big };
  // The group a Hit::Range refusal came from (Describe).
  struct Cause {
    bool permanent{}, stale{}; // stale: a ticketed group B has passed, not pruned yet
    uint32_t kinds{};
  };
  ConstantWriteGuard() = default;
  // Table mode (FAST bit 19): ticketed writes expire as soon as B passes them.
  // Only permanent (UINT64_MAX) ranges stay in the merged list, so a deferred
  // signal or image range can no longer absorb its neighbours forever.
  explicit ConstantWriteGuard(bool table) : table_mode(table) {}
  bool TableMode() const { return table_mode; }
  void Add(uint64_t address, uint64_t bytes, uint64_t ticket, uint32_t kinds = KindWrite) {
    if (!bytes)
      return;
    if (table_mode && ticket != UINT64_MAX) {
      if (address > UINT64_MAX - bytes) {
        Unknown(ticket); // wrapping
        ++unknown_adds;
      } else if (GranuleTickets::Fits(address, bytes)) {
        table.Note(address, bytes, ticket);
      } else {
        // Huge descriptor sizes (e.g. 0xFFFFFFFF-byte written buffers) stay exact:
        // few distinct ranges, each keeping its newest ticket.
        const uint64_t end = address + bytes;
        auto it = std::find_if(big.begin(), big.end(), [&](const Range &r) {
          return r.begin == address && r.end == end;
        });
        if (it != big.end())
          it->ticket = std::max(it->ticket, ticket);
        else
          big.push_back({address, end, ticket});
      }
      return;
    }
    const auto end =
        address > UINT64_MAX - bytes ? UINT64_MAX : address + bytes;
    // Same partition as one merged list of touching ranges: a group that contains a
    // permanent range is permanent (`ranges`), the others stay ticketed. Keeping them
    // apart lets the periodic prune scan only the short ticketed list; the permanent
    // list grows to tens of thousands of entries over a session.
    Range add{address, end, ticket, kinds};
    if (ticket != UINT64_MAX && !Touches(ranges, add)) {
      Merge(ticketed, add);
      return;
    }
    // Permanent, or touching a permanent group: absorb the touching ticketed groups and
    // merge into the permanent list. One pass suffices: ticketed groups never touch each
    // other or a permanent group, so absorbing cannot expose a new neighbour.
    add.ticket = UINT64_MAX;
    auto first = std::lower_bound(ticketed.begin(), ticketed.end(), add.begin,
        [](const Range &r, uint64_t at) { return r.end < at; });
    auto last = first;
    while (last != ticketed.end() && last->begin <= add.end) {
      add.begin = std::min(add.begin, last->begin);
      add.end = std::max(add.end, last->end);
      add.kinds |= last->kinds;
      ++last;
    }
    ticketed.erase(first, last);
    Merge(ranges, add);
  }
  void Unknown(uint64_t ticket) { unknown = std::max(unknown, ticket); }
  template <class Pending>
  Hit Check(uint64_t address, uint64_t bytes, Pending pending) {
    // Periodic pruning amortizes completion queries; keeping completed ranges
    // between prunes is conservative. UINT64_MAX denotes a deferred CPU signal.
    // In table mode the list holds only permanent ranges, so there is nothing to prune.
    if (!table_mode && (++checks & 63) == 0)
      std::erase_if(ticketed, [&](const Range &r) { return !pending(r.ticket); });
    if (unknown && pending(unknown))
      return Hit::Unknown;
    if (!bytes || address > UINT64_MAX - bytes)
      return Hit::Invalid;
    if (Overlaps(ranges, address, bytes) || Overlaps(ticketed, address, bytes))
      return Hit::Range;
    if (table_mode && !big.empty()) {
      std::erase_if(big, [&](const Range &r) { return !pending(r.ticket); });
      for (const auto &r : big)
        if (r.begin < address + bytes && address < r.end)
          return Hit::Big;
    }
    if (table_mode && table.NewestAny() && pending(table.NewestAny())) {
      const uint64_t newest = table.Newest(address, bytes);
      if (newest && pending(newest))
        return Hit::Table;
    }
    return Hit::None;
  }
  template <class Pending>
  bool Intersects(uint64_t address, uint64_t bytes, Pending pending) {
    return Check(address, bytes, pending) != Hit::None;
  }
  // After Check returned Hit::Range for these bytes: the group that refused them (statistics).
  template <class Pending>
  Cause Describe(uint64_t address, uint64_t bytes, Pending pending) const {
    if (const Range *r = Overlapping(ranges, address, bytes))
      return {true, false, r->kinds};
    if (const Range *r = Overlapping(ticketed, address, bytes))
      return {false, !pending(r->ticket), r->kinds};
    return {};
  }
  size_t RangeCount() const { return ranges.size() + ticketed.size(); }
  uint64_t UnknownAdds() const { return unknown_adds; }

private:
  // Sorted, disjoint, non-touching. Merges touching ranges, keeping the newest ticket;
  // a range already covered only refreshes the ticket (no vector shifting).
  static void Merge(std::vector<Range> &list, Range add) {
    auto first = std::lower_bound(list.begin(), list.end(), add.begin,
        [](const Range &r, uint64_t at) { return r.end < at; });
    if (first != list.end() && first->begin <= add.begin && add.end <= first->end) {
      first->ticket = std::max(first->ticket, add.ticket);
      first->kinds |= add.kinds;
      return;
    }
    auto last = first;
    while (last != list.end() && last->begin <= add.end) {
      add.begin = std::min(add.begin, last->begin);
      add.end = std::max(add.end, last->end);
      add.ticket = std::max(add.ticket, last->ticket);
      add.kinds |= last->kinds;
      ++last;
    }
    if (first == last) {
      list.insert(first, add);
    } else {
      *first = add;
      list.erase(first + 1, last);
    }
  }
  static bool Touches(const std::vector<Range> &list, const Range &add) {
    auto it = std::lower_bound(list.begin(), list.end(), add.begin,
        [](const Range &r, uint64_t at) { return r.end < at; });
    return it != list.end() && it->begin <= add.end;
  }
  static bool Overlaps(const std::vector<Range> &list, uint64_t address, uint64_t bytes) {
    return Overlapping(list, address, bytes) != nullptr;
  }
  static const Range *Overlapping(const std::vector<Range> &list, uint64_t address,
                                  uint64_t bytes) {
    auto it = std::lower_bound(list.begin(), list.end(), address,
        [](const Range &r, uint64_t at) { return r.end <= at; });
    return it != list.end() && it->begin < address + bytes ? &*it : nullptr;
  }
  std::vector<Range> ranges, ticketed, big;
  GranuleTickets table;
  uint64_t unknown{}, checks{}, unknown_adds{};
  bool table_mode{};
};

// FAST_CPU_GUARD: bbport's rule for the constant ring (its pending_write_table, vk_rasterizer.cpp
// NotePendingGpuWrite/PendingWriteOverlaps). A constant is refused only while a write that lands
// on its bytes, or with Match::Pages on their 4 KiB pages, is still queued:
// - each ticketed write keeps its own range and ticket (no merging with neighbours, no promotion
//   to permanent), so it stops refusing exactly when B passes it, as bbport's table does;
// - Match::Pages (the constant ring, CE dumps) matches a write by the pages it touches: B's own
//   read takes the cached buffer, with no per-eye pairing, whenever a page of the bytes is
//   GPU-modified (BufferCache::ObtainBuffer, BindBuffers; the tracker's pages are 4 KiB), and a
//   queued write marks its pages when B records it, so a ring copy of bytes on such a page would
//   differ from B's own binding even where the write misses them (and a CE dump's CPU write
//   would come before the marking, not after it as a full drain orders them). Match::Bytes is
//   bbport's byte-exact rule;
// - a deferred guest signal refuses until B has run its task and the recorder has written it
//   (ConstantWriteGuard keeps it for the whole activation); signals are CPU writes of the guest
//   bytes (no page is marked), matched by their bytes either way;
// - image footprints are not noted: GPU writes to images never reach guest memory here, and the
//   downloads that do are caught on B (VideoCore::DownloadLog).
// With Match::Bytes every refusal of this guard is also one of ConstantWriteGuard's, given the
// same calls; Match::Pages also refuses bytes that only share a page with a queued write
// (Hit::Page), which the old guard's byte ranges let through.
// `Progress` (Stage A): Pending(ticket), SignalsEncoded(), SignalWritten(sequence).
class PendingWriteGuard {
public:
  // Page: the first queued write found (Match::Pages) shares a page with the bytes but misses
  // them (statistics: a later one may overlap them; either refuses).
  enum class Hit : uint32_t {
    None, Unknown, Invalid, Write, Page, Big, SignalQueued, SignalUnwritten, Count
  };
  enum class Match : uint8_t { Bytes, Pages };
  // BufferCache's GPU-modified tracking granularity (region_definitions.h TRACKER_PAGE_BITS).
  static constexpr uint64_t PageBytes = 4096;
  // Writes of up to Limits[c] bytes are class c, each list sorted by begin: a write that reaches
  // into a query begins less than Limits[c] before it, which bounds the scan. Bigger ones (huge
  // descriptor sizes) are a short unsorted list.
  static constexpr uint32_t Classes = 3;
  static constexpr uint64_t Limits[Classes] = {4096, 256 * 1024, uint64_t{64} << 20};
  void AddWrite(uint64_t address, uint64_t bytes, uint64_t ticket) {
    if (!bytes || !ticket)
      return;
    // Wrapping: up to the top, as ConstantWriteGuard's merged lists keep it (a big write).
    const bool wraps = address > UINT64_MAX - bytes;
    const Write add{address, wraps ? UINT64_MAX : address + bytes, ticket};
    uint32_t c = 0;
    while (c < Classes && (wraps || bytes > Limits[c]))
      ++c;
    unknown_adds += wraps;
    newest[c] = std::max(newest[c], ticket);
    auto &list = lists[c];
    if (c == Classes) {
      for (auto &w : list)
        if (w.begin == add.begin && w.end == add.end) {
          w.ticket = std::max(w.ticket, ticket);
          return;
        }
      list.push_back(add);
      return;
    }
    auto it = std::lower_bound(list.begin(), list.end(), add.begin,
                               [](const Write &w, uint64_t at) { return w.begin < at; });
    // The same range written again (a buffer every frame): one entry with the newest ticket.
    for (auto same = it; same != list.end() && same->begin == add.begin; ++same)
      if (same->end == add.end) {
        same->ticket = std::max(same->ticket, ticket);
        return;
      }
    list.insert(it, add);
  }
  // A guest signal queued with (or before) `ticket`, written by the recorder after that.
  void AddSignal(uint64_t address, uint64_t bytes, uint64_t ticket) {
    if (!bytes)
      return;
    const uint64_t end = address > UINT64_MAX - bytes ? UINT64_MAX : address + bytes;
    for (auto &s : signals)
      if (s.begin == address && s.end == end) {
        s.ticket = std::max(s.ticket, ticket);
        s.resolved = false; // a newer signal: its sequence is not known yet
        return;
      }
    signals.push_back({address, end, ticket, 0, false});
    signal_lo = std::min(signal_lo, address);
    signal_hi = std::max(signal_hi, end);
  }
  void Unknown(uint64_t ticket) { unknown = std::max(unknown, ticket); }
  template <class Progress>
  Hit Check(uint64_t address, uint64_t bytes, Progress &progress, Match match = Match::Bytes) {
    // Lookups ask each overlapping write whether it is still queued, so pruning only bounds the
    // lists: the signals every 64 checks, the writes once they have doubled since the last prune.
    if ((++checks & 63) == 0)
      Prune(progress);
    if (unknown && progress.Pending(unknown))
      return Hit::Unknown;
    if (!bytes || address > UINT64_MAX - bytes)
      return Hit::Invalid;
    const uint64_t end = address + bytes;
    // Match::Pages: the query widened to whole pages, which a write overlaps exactly when it
    // touches a page of the bytes (the top page stops at UINT64_MAX, as wrapping writes do).
    const bool pages = match == Match::Pages;
    const uint64_t first = pages ? address & ~(PageBytes - 1) : address;
    const uint64_t last = !pages                              ? end
                          : end > UINT64_MAX - (PageBytes - 1) ? UINT64_MAX
                                                               : (end + PageBytes - 1) & ~(PageBytes - 1);
    for (uint32_t c = 0; c <= Classes; ++c) {
      auto &list = lists[c];
      if (list.empty())
        continue;
      if (!progress.Pending(newest[c])) { // O(1): B has passed every write of this class
        list.clear();
        newest[c] = 0;
        continue;
      }
      auto it = list.begin();
      if (c < Classes)
        it = std::lower_bound(list.begin(), list.end(),
                              first > Limits[c] ? first - Limits[c] : 0,
                              [](const Write &w, uint64_t at) { return w.begin < at; });
      for (; it != list.end() && (c == Classes || it->begin < last); ++it)
        if (it->begin < last && first < it->end && progress.Pending(it->ticket))
          return c == Classes                           ? Hit::Big
                 : it->begin < end && address < it->end ? Hit::Write
                                                        : Hit::Page;
    }
    if (address >= signal_hi || signal_lo >= end) // O(1): no label near these bytes
      return Hit::None;
    for (auto &s : signals) {
      if (s.begin >= end || address >= s.end)
        continue;
      if (progress.Pending(s.ticket))
        return Hit::SignalQueued;
      Resolve(s, progress);
      if (!progress.SignalWritten(s.sequence))
        return Hit::SignalUnwritten;
    }
    return Hit::None;
  }
  template <class Progress>
  bool Intersects(uint64_t address, uint64_t bytes, Progress &progress,
                  Match match = Match::Bytes) {
    return Check(address, bytes, progress, match) != Hit::None;
  }
  size_t Count() const {
    size_t count = signals.size();
    for (const auto &list : lists)
      count += list.size();
    return count;
  }
  uint64_t UnknownAdds() const { return unknown_adds; } // wrapping writes

private:
  struct Write {
    uint64_t begin{}, end{}, ticket{};
  };
  struct Signal {
    uint64_t begin{}, end{}, ticket{}, sequence{};
    bool resolved{};
  };
  // Once B has passed the signal's task, every signal encoded so far includes it.
  template <class Progress> static void Resolve(Signal &s, Progress &progress) {
    if (!s.resolved) {
      s.sequence = progress.SignalsEncoded();
      s.resolved = true;
    }
  }
  template <class Progress> void Prune(Progress &progress) {
    size_t writes = 0;
    for (const auto &list : lists)
      writes += list.size();
    if (writes >= prune_writes) {
      writes = 0;
      for (auto &list : lists) {
        std::erase_if(list, [&](const Write &w) { return !progress.Pending(w.ticket); });
        writes += list.size();
      }
      prune_writes = 2 * writes + 64;
    }
    std::erase_if(signals, [&](Signal &s) {
      if (progress.Pending(s.ticket))
        return false;
      Resolve(s, progress);
      return progress.SignalWritten(s.sequence);
    });
    signal_lo = UINT64_MAX;
    signal_hi = 0;
    for (const auto &s : signals) {
      signal_lo = std::min(signal_lo, s.begin);
      signal_hi = std::max(signal_hi, s.end);
    }
    if (unknown && !progress.Pending(unknown))
      unknown = 0;
  }
  std::array<std::vector<Write>, Classes + 1> lists;
  std::array<uint64_t, Classes + 1> newest{};
  std::vector<Signal> signals;
  uint64_t signal_lo{UINT64_MAX}, signal_hi{}; // bounds of `signals`
  size_t prune_writes{64};
  uint64_t unknown{}, checks{}, unknown_adds{};
};
} // namespace FastRender
