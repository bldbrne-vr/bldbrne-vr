// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Owned CPU reads used by draw recording. GPU buffer contents retain their
// ordered cache path.
#pragma once
#include "input_scope_gate.h"
#include <cstddef>
#include <cstdint>
#include <span>

namespace ScratchDraw {
struct MemoryRead {
  std::uintptr_t address{};
  std::span<const std::uint32_t> words;
};
inline ScopeUseGate memory_scope_gate;
inline thread_local std::span<const MemoryRead> active_memory;
inline const void *MemorySource(const void *source,
                                std::size_t bytes) noexcept {
  if (!memory_scope_gate.CanSkip()) {
    const auto address = reinterpret_cast<std::uintptr_t>(source);
    for (const auto &read : active_memory) {
      if (address < read.address)
        continue;
      const auto offset = address - read.address;
      if (offset <= read.words.size_bytes() &&
          bytes <= read.words.size_bytes() - offset)
        return reinterpret_cast<const std::byte *>(read.words.data()) + offset;
    }
  }
  return source;
}
class MemoryScope {
  std::span<const MemoryRead> previous;

public:
  explicit MemoryScope(std::span<const MemoryRead> reads)
      : previous(active_memory) {
    memory_scope_gate.MarkUsed();
    active_memory = reads;
  }
  ~MemoryScope() { active_memory = previous; }
  MemoryScope(const MemoryScope &) = delete;
  MemoryScope &operator=(const MemoryScope &) = delete;
};
} // namespace ScratchDraw
