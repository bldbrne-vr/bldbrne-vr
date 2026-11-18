// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace VR::Lever {
// Object animation ID is not proof of active playback. Native 1.09's 0x15A5010
// checks the slot handle at object+0x20C in controller+0x290 (stride 0x78),
// including its generation, then slot+0x4C. Only a positively identified inactive
// slot is an additional acceptable case here: missing/stale/unreadable slots fail
// closed even where the native query would report inactive. This says nothing about
// permission to use the object; action state, rest pose and native permission must
// still pass. Read is the adapter's checked scalar memory reader.
template <typename Read>
bool AnimationAllowsRestCheck(std::int32_t animation, std::uint64_t controller,
                              std::uint32_t handle, Read&& read) {
    if (animation == -1)
        return true; // unchanged common path, no additional memory reads
    if (animation < 0 || controller < 0x10000 || controller >= 0x0000800000000000ULL)
        return false;
    std::uint32_t count{};
    std::uint64_t slots{};
    const auto index = handle & 0xFFFFu;
    if (!read(controller, count) || count == 0 || count > 65536 || index >= count ||
        !read(controller + 8, slots) || slots < 0x10000 ||
        slots >= 0x0000800000000000ULL - 65536ULL * 0x78)
        return false;
    const auto slot = slots + static_cast<std::uint64_t>(index) * 0x78;
    std::uint32_t identity{}, active{}, confirmed{};
    return read(slot, identity) && identity == handle &&
           read(slot + 0x4C, active) && active == 0 &&
           read(slot, confirmed) && confirmed == handle;
}
} // namespace VR::Lever
