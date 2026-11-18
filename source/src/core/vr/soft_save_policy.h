// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace VR::SoftSave {
// Pure save barrier: a fresh character AND system write must finish after this request.
// No open handle is allowed, including readers; copying during a load is not a soft save.
struct Barrier {
    std::uint64_t character{}, system{};
    bool Ready(std::uint64_t current_character, std::uint64_t current_system, bool handles_open,
               bool manager_busy, std::int64_t quiet_ms) const {
        return current_character > character && current_system > system && !handles_open &&
               !manager_busy && quiet_ms >= 1000;
    }
};
} // namespace VR::SoftSave
