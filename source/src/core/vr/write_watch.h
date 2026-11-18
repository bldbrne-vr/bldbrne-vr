// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>
#include "common/types.h"

// CPU data breakpoints on guest memory (Windows; a no-op elsewhere): every write to a watched
// address on any thread of the process traps, and the trap records which instruction wrote it,
// the return addresses inside the game's code on that thread's stack and the values left there.
// A diagnostic for finding the game code behind a value (VR_VISCERAL_WATCH: the visceral's enemy
// and the hunter's world position, the game's follow-camera pivot). Three slots (DR0 .. DR2);
// arming and disarming suspend each thread in turn for a moment.
namespace VR::WriteWatch {

struct Hit {
    s64 ns{0};
    u32 thread{0};
    int slot{-1};
    u64 rip{0};                 // the instruction after the write
    std::array<float, 3> value{}; // the watched float and the ones its stride and twice it on
    std::array<u64, 8> stack{}; // return addresses inside the module, innermost first
    u32 stack_count{0};
};

constexpr int Slots = 3;
// Watches 4 bytes at each non-null address for writes. `module`/`size`: the code range return
// addresses are taken from. False when it could not arm (or not on Windows).
bool Arm(const std::array<const void*, Slots>& addresses, const std::array<u64, Slots>& strides,
         const void* module, u64 size);
void Disarm();
[[nodiscard]] bool Armed();
// The hits since Arm, oldest first (at most MaxHits; `dropped` counts the rest).
std::vector<Hit> TakeHits(u32& dropped);

} // namespace VR::WriteWatch
