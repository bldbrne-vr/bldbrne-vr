// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Opt-in diagnostic only. Sequence numbers order observations, not the memory
// operations between them; begin/end pairs expose overlapping writes without
// introducing a mutex around guest memory or changing production ordering.
namespace FlipOrderTrace {
// Read once at start-up (a plain load per event, no function-local static's guard).
inline const bool g_enabled = std::getenv("VR_FLIP_ORDER_TRACE") != nullptr;
inline bool Enabled() {
    return g_enabled;
}
inline std::atomic<uintptr_t> labels{};
inline std::atomic<uint64_t> sequence{};
inline void Event(const char* event, uintptr_t address, uint64_t value, uint32_t bytes = 0,
                  uint64_t detail = 0) {
    if (!Enabled())
        return;
    const auto seq = sequence.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(
        stderr, "FLIP_TRACE seq=%llu event=%s address=%llx value=%llu bytes=%u detail=%llu\n",
        static_cast<unsigned long long>(seq), event, static_cast<unsigned long long>(address),
        static_cast<unsigned long long>(value), bytes, static_cast<unsigned long long>(detail));
}
inline void SetLabels(uintptr_t address) {
    if (!Enabled())
        return;
    const auto previous = labels.exchange(address, std::memory_order_relaxed);
    if (previous != address)
        Event("labels", address, 16, 8);
}
inline void Write(const char* event, uintptr_t address, uint64_t value, uint32_t bytes,
                  uint64_t detail = 0) {
    if (!Enabled())
        return;
    const auto base = labels.load(std::memory_order_relaxed);
    if (base && bytes && address < base + 16 * sizeof(uint64_t) &&
        (address >= base || base - address < bytes))
        Event(event, address, value, bytes, detail);
}
} // namespace FlipOrderTrace
