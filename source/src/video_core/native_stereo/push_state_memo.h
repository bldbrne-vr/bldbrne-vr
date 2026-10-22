// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
namespace ScratchPush {
// Read at the first call, as before (test_push_state_memo.cpp sets the variable at run time), then
// one relaxed load per call: Enabled() runs per draw, and a function-local static's guard is a
// thread-local read per call. -1: not read yet (a race reads the same environment twice).
inline std::atomic<signed char> g_owned_state{-1}, g_enabled_state{-1};
inline bool OwnedEnabled() {
    signed char state = g_owned_state.load(std::memory_order_relaxed);
    if (state < 0) [[unlikely]] {
        state = std::getenv("VR_OWNED_PUSH_STATE") != nullptr;
        g_owned_state.store(state, std::memory_order_relaxed);
    }
    return state != 0;
}
inline bool Enabled() {
    signed char state = g_enabled_state.load(std::memory_order_relaxed);
    if (state < 0) [[unlikely]] {
        state = std::getenv("VR_REUSE_PUSH_STATE") != nullptr || OwnedEnabled();
        g_enabled_state.store(state, std::memory_order_relaxed);
    }
    return state != 0;
}
inline std::atomic<std::uint64_t> generation{1};
// Global invalidation also covers recording that moves between host threads.
inline void Invalidate() { if (Enabled()) generation.fetch_add(1, std::memory_order_relaxed); }
struct Memo {
    using Key = std::array<std::uint64_t, 5>;
    // The largest upload: native stereo graphics with Shader::VrPushData (bytes 0..223).
    static constexpr std::size_t Capacity = 224;
    Key key{};
    std::array<unsigned char, Capacity> bytes{};
    std::uint64_t epoch{};
    std::size_t size{};
    bool valid{};
    bool Prepare(const Key& next, const void* data, std::size_t length) {
        return PrepareImpl<true>(next, data, length);
    }
    // One memo per scheduler; Vulkan already requires serialized command recording.
    bool PrepareOwned(const Key& next, const void* data, std::size_t length) {
        return PrepareImpl<false>(next, data, length);
    }
private:
    template<bool globally_invalidate>
    bool PrepareImpl(const Key& next, const void* data, std::size_t length) {
        const auto current = generation.load(std::memory_order_relaxed);
        if (valid && epoch == current && key == next && size == length &&
            length <= bytes.size() && std::memcmp(bytes.data(), data, length) == 0) return true;
        if constexpr (globally_invalidate) epoch = generation.fetch_add(1, std::memory_order_relaxed) + 1;
        else epoch = current;
        valid = length <= bytes.size();
        if (valid) { key = next; size = length; std::memcpy(bytes.data(), data, length); }
        return false;
    }
};
}
