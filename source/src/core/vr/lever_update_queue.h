// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <span>

namespace VR::Lever {
// 1.09 object scheduler: the entry's low twelve pending bits are consumed into
// active bits by 0x1CB4DD0. Bit 0x100 requests a model refresh; active bit 0x1000
// records membership in the pending-index list. The game clears membership when
// no new pending work remains. Never clear other work or remove entries ourselves.
// Call only on the game thread before this queue is consumed, with validated live
// storage. This adapter does not start an animation or establish grab eligibility.
enum class RefreshResult { Queued, AlreadyQueued, Invalid };

inline RefreshResult RequestModelRefresh(std::uint16_t index, std::uint32_t capacity,
                                         std::span<std::uint16_t> indices, std::uint32_t &count,
                                         std::uint16_t &pending, std::uint16_t &active) {
    constexpr std::uint16_t ModelRefresh = 0x100;
    constexpr std::uint16_t Membership = 0x1000;
    if (capacity == 0 || capacity > 65536 || indices.size() != capacity || index >= capacity ||
        count > capacity) {
        return RefreshResult::Invalid;
    }
    // Validate membership instead of appending a duplicate when flags/list disagree.
    // A failed validation must not change any flags or queue storage.
    unsigned occurrences = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (indices[i] >= capacity) {
            return RefreshResult::Invalid;
        }
        occurrences += indices[i] == index;
    }
    const bool member = (active & Membership) != 0;
    if (occurrences != (member ? 1u : 0u) || (!member && count == capacity)) {
        return RefreshResult::Invalid;
    }
    pending |= ModelRefresh;
    if (member) {
        return RefreshResult::AlreadyQueued;
    }
    indices[count++] = index;
    active |= Membership;
    return RefreshResult::Queued;
}
} // namespace VR::Lever
