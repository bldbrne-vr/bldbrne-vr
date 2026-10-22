// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <array>
#include <cstring>
#include <fmt/format.h>

#include "core/vr/hook_util.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace VR::Hook {

bool MakeWritable(u8* address, size_t size) {
#ifdef _WIN32
    DWORD old{};
    return VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old) != 0;
#else
    const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    const uintptr_t start = reinterpret_cast<uintptr_t>(address) & ~(page - 1);
    const uintptr_t end = (reinterpret_cast<uintptr_t>(address) + size + page - 1) & ~(page - 1);
    return mprotect(reinterpret_cast<void*>(start), end - start,
                    PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
#endif
}

void FlushCode(u8* address, size_t size) {
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), address, size);
#else
    (void)address;
    (void)size;
#endif
}

void* EmitHostCallStub(Xbyak::CodeGenerator& c, void* fn, const u8* original, size_t length,
                       u64 return_address, const Xbyak::Reg64& argument,
                       const Xbyak::Reg64* third) {
    using namespace Xbyak::util;
    c.align(16);
    void* entry = const_cast<u8*>(c.getCurr());

    c.lea(rsp, ptr[rsp - 128]); // keep the guest red zone intact
    c.pushf();
    for (const auto& reg : {rax, rcx, rdx, rbx, rsi, rdi, r8, r9, r10, r11, rbp}) {
        c.push(reg);
    }
    c.mov(rbp, rsp);
    c.and_(rsp, -32);
    c.sub(rsp, 32 * 16);
    for (int i = 0; i < 16; ++i) {
        c.vmovdqu(ptr[rsp + 32 * i], Xbyak::Ymm(i));
    }
    if (third != nullptr) {
        c.mov(rdx, *third);
    }
    c.mov(rdi, argument);
    c.mov(rsi, rsp);
    c.mov(rax, reinterpret_cast<u64>(fn));
    c.call(rax);
    for (int i = 0; i < 16; ++i) {
        c.vmovdqu(Xbyak::Ymm(i), ptr[rsp + 32 * i]);
    }
    c.mov(rsp, rbp);
    for (const auto& reg : {rbp, r11, r10, r9, r8, rdi, rsi, rbx, rdx, rcx, rax}) {
        c.pop(reg);
    }
    c.popf();
    c.lea(rsp, ptr[rsp + 128]);

    for (size_t i = 0; i < length; ++i) {
        c.db(original[i]);
    }
    // jmp qword [rip+0]; dq return_address
    c.db(0xFF);
    c.db(0x25);
    c.dd(0);
    c.dq(return_address);
    return entry;
}

void* EmitHostCallStubWith(Xbyak::CodeGenerator& c, void* fn, const u8* original, size_t length,
                           u64 return_address,
                           const std::function<void(Xbyak::CodeGenerator&)>& load_args) {
    using namespace Xbyak::util;
    c.align(16);
    void* entry = const_cast<u8*>(c.getCurr());

    c.lea(rsp, ptr[rsp - 128]); // keep the guest red zone intact
    c.pushf();
    for (const auto& reg : {rax, rcx, rdx, rbx, rsi, rdi, r8, r9, r10, r11, rbp}) {
        c.push(reg);
    }
    load_args(c); // guest rbp, r12-r15 and the argument registers are still live here
    c.mov(rbp, rsp);
    c.and_(rsp, -32);
    c.sub(rsp, 32 * 16);
    for (int i = 0; i < 16; ++i) {
        c.vmovdqu(ptr[rsp + 32 * i], Xbyak::Ymm(i));
    }
    c.mov(rax, reinterpret_cast<u64>(fn));
    c.call(rax);
    for (int i = 0; i < 16; ++i) {
        c.vmovdqu(Xbyak::Ymm(i), ptr[rsp + 32 * i]);
    }
    c.mov(rsp, rbp);
    for (const auto& reg : {rbp, r11, r10, r9, r8, rdi, rsi, rbx, rdx, rcx, rax}) {
        c.pop(reg);
    }
    c.popf();
    c.lea(rsp, ptr[rsp + 128]);

    for (size_t i = 0; i < length; ++i) {
        c.db(original[i]);
    }
    // jmp qword [rip+0]; dq return_address
    c.db(0xFF);
    c.db(0x25);
    c.dd(0);
    c.dq(return_address);
    return entry;
}

void WriteJump(u8* site, size_t length, void* target) {
    std::array<u8, 32> bytes{};
    std::memset(bytes.data(), 0x90, bytes.size());
    bytes[0] = 0xFF;
    bytes[1] = 0x25;
    std::memset(bytes.data() + 2, 0, 4); // jmp qword [rip+0]
    const u64 address = reinterpret_cast<u64>(target);
    std::memcpy(bytes.data() + 6, &address, sizeof(address));
    std::memcpy(site, bytes.data(), length);
    FlushCode(site, length);
}

std::string HexBytes(const u8* data, size_t length) {
    std::string text;
    text.reserve(length * 3);
    for (size_t i = 0; i < length; ++i) {
        text += fmt::format("{}{:02X}", i == 0 ? "" : " ", data[i]);
    }
    return text;
}

} // namespace VR::Hook
