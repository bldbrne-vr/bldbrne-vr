// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace VR::DeferredTool {
inline constexpr std::int64_t DelayNs = 150'000'000; // the squish, then the tentacles (the tester's 2026-11-04 "150ms or so")
inline constexpr std::int64_t MaxAgeNs = 400'000'000;
enum class Decision { Wait, Cancel, Cast };
// An identity only: never dereference an address retained from an earlier game update.
struct Ticket {
    std::int64_t started{};
    std::uintptr_t player{};
    std::uint32_t handle{};
    [[nodiscard]] Decision Check(std::int64_t now, std::uintptr_t current,
                                 std::uint32_t current_handle, bool eligible) const {
        if (!eligible || started <= 0 || now < started || player == 0 ||
            current != player || current_handle != handle || now - started > MaxAgeNs)
            return Decision::Cancel;
        return now - started < DelayNs ? Decision::Wait : Decision::Cast;
    }
};
} // namespace VR::DeferredTool
