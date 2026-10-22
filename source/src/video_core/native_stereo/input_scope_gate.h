// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Scratch experiment: serial accessors need no TLS until a scope has existed.
#pragma once
#include <atomic>
#include <cstdlib>
namespace ScratchDraw {
inline const bool serial_input_fastpath = [] {
  const char *value = std::getenv("VR_SERIAL_INPUT_FASTPATH");
  return !value || value[0] != '0';
}();
// One cache line per gate: Stage A and Stage B both read these on every scope lookup.
class alignas(64) ScopeUseGate {
  std::atomic<bool> used{false};

public:
  bool CanSkip() const noexcept {
    return serial_input_fastpath && !used.load(std::memory_order_relaxed);
  }
  void MarkUsed() noexcept {
    // Never reset. Each scope marks before publishing its own TLS state.
    // A thread with an active scope must observe its own preceding store;
    // other threads still read only their own TLS. No cross-thread payload
    // is published by this atomic, so relaxed ordering suffices. Load first:
    // an unconditional store per scope keeps stealing the cache line from the
    // other stage's thread.
    if (!used.load(std::memory_order_relaxed))
      used.store(true, std::memory_order_relaxed);
  }
};
} // namespace ScratchDraw
