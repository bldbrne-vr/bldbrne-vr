// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <optional>
#include <stdexcept>
#include "common/alignment.h"
#include "core/libraries/kernel/memory.h"

namespace Core {

// Initial physical file layout. Direct pages occupy [0, flexible_offset).
struct MemoryBackingLayout {
    u64 flexible_offset;
    u64 size;

    // A larger guest flexible pool can use space returned by direct memory at startup.
    [[nodiscard]] std::optional<u64> GuestFlexibleOffset(u64 direct, u64 flexible) const {
        if (flexible > size) {
            return std::nullopt;
        }
        const u64 offset = std::min(flexible_offset, Common::AlignDown(size - flexible, 64_KB));
        if (direct > offset) {
            return std::nullopt;
        }
        return offset;
    }

    static MemoryBackingLayout ForConfig(bool compact, bool devkit, bool pro, s32 extra_direct_mib,
                                         s32 extra_flexible_mib) {
        s64 direct = ORBIS_KERNEL_TOTAL_MEM_DEV_PRO;
        if (compact && !devkit) {
            // Pro off rules out Neo even before the title's attributes are available.
            direct = pro ? ORBIS_KERNEL_TOTAL_MEM_PRO : ORBIS_KERNEL_TOTAL_MEM;
        }
        constexpr s64 mib = 1_MB;
        direct += extra_direct_mib * mib;
        const s64 flexible = static_cast<s64>(ORBIS_KERNEL_FLEXIBLE_MEMORY_SIZE) +
                             extra_flexible_mib * mib;
        if (direct <= 0 || flexible <= 0) {
            throw std::invalid_argument(
                "Configured direct and flexible backing sizes must be positive");
        }
        return {static_cast<u64>(direct), static_cast<u64>(direct + flexible)};
    }
};

} // namespace Core
