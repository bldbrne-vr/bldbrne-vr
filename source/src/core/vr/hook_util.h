// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include <functional>
#include <string>
#include <xbyak/xbyak.h>

#include "common/types.h"

// Helpers for patching guest code (native x86-64, mapped at its guest address): a 14-byte
// absolute jump at the hook site leads to an Xbyak stub that calls host code on the guest
// thread, replays the overwritten instructions and jumps back.
namespace VR::Hook {

// Reads a value from guest memory (any alignment).
template <typename T>
T Read(const u8* address) {
    T value;
    std::memcpy(&value, address, sizeof(T));
    return value;
}

// Pointers read from game structures: non-null, inside the user address range and aligned
// (param rows only need to be in range).
inline bool Plausible(const u8* pointer, uintptr_t align = 8) {
    const auto value = reinterpret_cast<uintptr_t>(pointer);
    return value >= 0x10000 && value < 0x0000'8000'0000'0000ULL && (value & (align - 1)) == 0;
}

bool MakeWritable(u8* address, size_t size);

void FlushCode(u8* address, size_t size);

// Emits a stub that saves all volatile guest state, calls `fn(argument, saved_ymm[, third])`
// with the System V ABI, restores state, replays the `length` overwritten instructions from
// `original` (a constant copy, never the live site) and jumps to `return_address`.
void* EmitHostCallStub(Xbyak::CodeGenerator& c, void* fn, const u8* original, size_t length,
                       u64 return_address, const Xbyak::Reg64& argument,
                       const Xbyak::Reg64* third = nullptr);

// Like EmitHostCallStub, but `load_args` emits the moves into rdi, rsi, rdx, rcx (System V) right
// after the saves, while every guest register (rbp, r12-r15 and the argument registers) still
// holds its guest value; `fn` gets no saved_ymm pointer. For read-only hooks (logs).
void* EmitHostCallStubWith(Xbyak::CodeGenerator& c, void* fn, const u8* original, size_t length,
                           u64 return_address,
                           const std::function<void(Xbyak::CodeGenerator&)>& load_args);

// Overwrites `length` (>= 14) bytes at `site` with `jmp qword [rip+0]; dq target`, padded
// with NOPs.
void WriteJump(u8* site, size_t length, void* target);

// "49 8B 45 00 ..." for install logs.
std::string HexBytes(const u8* data, size_t length);

} // namespace VR::Hook
