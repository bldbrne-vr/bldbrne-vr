// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Allocation-free execution of a validated SRT walker plan, inside a narrow read guard.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace ScratchDraw {
struct SrtReadStep {
    enum class Kind { Push, Pop, Copy } kind;
    std::uint32_t source_byte{}, target_word{};
};
// Plan construction validates nesting <=64, destination bounds, and root user-data
// bounds. These are host-owned instructions, never guest-provided opcodes.
inline void ExecuteSrtReadPlan(std::span<const SrtReadStep> plan,
                               const std::uint32_t* user, std::uint32_t* flat) {
    std::array<std::uintptr_t, 64> stack{};
    std::size_t depth = 0;
    auto base = reinterpret_cast<std::uintptr_t>(user);
    for (const auto& step : plan) {
        switch (step.kind) {
        case SrtReadStep::Kind::Push:
            stack[depth++] = base;
            std::memcpy(&base, reinterpret_cast<const void*>(base + step.source_byte), sizeof(base));
            base &= 0xFFFFFFFFFFFFULL;
            break;
        case SrtReadStep::Kind::Pop:
            base = stack[--depth];
            break;
        case SrtReadStep::Kind::Copy:
            std::memcpy(flat + step.target_word,
                        reinterpret_cast<const void*>(base + step.source_byte), sizeof(*flat));
            break;
        }
    }
}
} // namespace ScratchDraw
