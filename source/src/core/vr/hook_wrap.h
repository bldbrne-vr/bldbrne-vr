// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

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
