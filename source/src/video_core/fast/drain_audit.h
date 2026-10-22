// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// B1b diagnostic only: no scheduling decisions depend on this journal.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

namespace FastRender::DrainAudit {
using U64 = std::uint64_t;
struct Range {
  U64 address{}, bytes{};
  bool Overlaps(Range other) const {
    return bytes && other.bytes &&
           (address <= other.address ? other.address - address < bytes
                                     : address - other.address < other.bytes);
  }
};
enum class Kind {
  Unknown,
  Eop,
  Eos,
  Release,
  WriteData,
  Dma,
  Compute,
  Graphics,
  CpuFault,
  Ce
};
inline const char *Name(Kind kind) {
  constexpr const char *names[]{"unknown",    "eop", "eos",     "release",
                                "write_data", "dma", "compute", "graphics",
                                "cpu_fault",  "ce"};
  return names[static_cast<unsigned>(kind)];
}
struct Writer {
  Range range{};
  Kind kind{Kind::Unknown};
  U64 ticket{}, value{};
  bool literal{};
};
struct Pending {
  U64 ticket{};
  std::vector<Range> ranges;
  bool unknown{};
};
struct Overlap {
  unsigned packets{}, matching{}, unknown{};
};
class Journal {
  std::mutex mutex;
  std::array<Writer, 8192> writes{};
  U64 next{};
  std::deque<Pending> reads;

public:
  void Write(Writer writer) {
    if (!writer.range.bytes)
      return;
    std::scoped_lock lock{mutex};
    writes[next++ % writes.size()] = writer;
  }
  Writer Last(Range range) {
    std::scoped_lock lock{mutex};
    const auto count = std::min<U64>(next, writes.size());
    for (U64 i = 0; i < count; ++i) {
      auto writer = writes[(next - i - 1) % writes.size()];
      if (!writer.range.Overlaps(range))
        continue;
      // A literal is usable only when this record covers the whole queried
      // word.
      const U64 offset = range.address - writer.range.address;
      writer.literal &=
          range.address >= writer.range.address && range.bytes == 4 &&
          (offset == 0 ||
           ((writer.kind == Kind::Eop || writer.kind == Kind::Release) &&
            offset == 4)) &&
          offset + 4 <= writer.range.bytes;
      if (writer.literal)
        writer.value >>= offset * 8;
      return writer;
    }
    return {};
  }
  template <class IsPending>
  void Reads(U64 ticket, std::span<const Range> ranges, bool unknown,
             IsPending pending) {
    if (!ticket)
      return;
    std::scoped_lock lock{mutex};
    while (!reads.empty() && !pending(reads.front().ticket))
      reads.pop_front();
    reads.push_back({ticket, {ranges.begin(), ranges.end()}, unknown});
  }
  template <class IsPending> Overlap Probe(Range range, IsPending pending) {
    std::scoped_lock lock{mutex};
    while (!reads.empty() && !pending(reads.front().ticket))
      reads.pop_front();
    Overlap result;
    for (const auto &read : reads) {
      if (!pending(read.ticket))
        continue;
      ++result.packets;
      result.unknown += read.unknown;
      result.matching +=
          std::any_of(read.ranges.begin(), read.ranges.end(),
                      [&](Range source) { return source.Overlaps(range); });
    }
    return result;
  }
};
inline bool Enabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("FAST_DRAIN_AUDIT");
    return value && value[0] == '1';
  }();
  return enabled;
}
inline Journal &Get() {
  // Diagnostic process lifetime; callbacks can outlive renderer shutdown.
  static auto *journal = new Journal;
  return *journal;
}
} // namespace FastRender::DrainAudit
