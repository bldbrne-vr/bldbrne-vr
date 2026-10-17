// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <xbyak/xbyak.h>

#include "common/types.h"

// Function-boundary hooks through relocated vtable slots (core/vr/hook_util.h has the mid-function
// detours). A vtable slot holds base + RVA only after Linker::RelocateAllImports, so these are
// installed after relocation (VR::Bloodborne::OnEbootRelocated), never from OnEbootLoaded.
namespace VR::Hook {

// Emits a wrapper with the original's signature: it calls `original` with the caller's arguments
// untouched, then `post(first argument)` (a PS4_SYSV_ABI host function) on the same thread, and
// returns the original's rax. rbx, r12 and rbp are kept for the guest caller and rsp is 16-byte
// aligned at both calls. Only for functions without stack arguments (the wrapper's pushes would
// move them).
void* EmitPostCallWrapper(Xbyak::CodeGenerator& c, u64 original, void* post);

// Replaces the 8-byte, 8-byte aligned slot's `expected` value with `replacement` (one release
// store: a guest thread dispatching through the slot meanwhile sees either value). Refuses when the
// slot holds anything else; `found` gets the value read.
bool SwapVtableSlot(u8* slot, u64 expected, u64 replacement, u64& found);

} // namespace VR::Hook
