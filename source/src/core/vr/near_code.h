// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

// Memory within +-2 GB of the game's eboot, for RIP-relative targets: a `lea rax, [rip+disp32]`
// repointed at our own string or at a trampoline (`jmp qword [rip+0]; dq host_function`) must land
// within reach of the instruction. Our other hooks only write 14-byte absolute jumps, which reach
// anywhere; this is for the sites that keep their instruction and only change its displacement.
//
// One 64 KB region, mapped once through the guest memory manager (its own VMA, read / write /
// execute, named "VR near code") at the first free address after the module's cpu_patches
// trampoline area (module + aligned size + 8 MB), so neither the guest's allocations nor
// cpu_patches' own trampolines can ever take it; handed out as a bump allocator, never freed.
namespace VR::Hook {

// `bytes` of zeroed near memory aligned to `align`, or nullptr (logged) when the region could not
// be mapped within reach of [module_base, module_base + module_size) or is full.
u8* NearAlloc(u64 module_base, u64 module_size, std::size_t bytes, std::size_t align = 16);

// The rel32 displacement from the end of an instruction (`next`) to `target`, when it fits.
bool Rel32(u64 next, u64 target, s32& out);

} // namespace VR::Hook
