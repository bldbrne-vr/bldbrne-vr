// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdint>

#include "core/vr/hook_util.h"
#include "core/vr/hook_wrap.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace VR::Hook {

namespace {

// The data segment holding vtables is normally writable already (relocation wrote it); only an
// unexpected protection is changed.
bool IsWritable(const u8* address) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) {
        return false;
    }
    constexpr DWORD writable =
        PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (info.Protect & writable) != 0 && (info.Protect & PAGE_GUARD) == 0;
#else
    (void)address;
    return false;
#endif
}

} // Anonymous namespace

void* EmitPostCallWrapper(Xbyak::CodeGenerator& c, u64 original, void* post) {
    using namespace Xbyak::util;
    c.align(16);
    void* entry = const_cast<u8*>(c.getCurr());
    // Entry rsp = 8 (mod 16): after three pushes it is 16-byte aligned for both calls.
    c.push(rbp);
    c.mov(rbp, rsp);
    c.push(rbx);
    c.push(r12);
    c.mov(rbx, rdi);
    c.mov(rax, original);
    c.call(rax);
    c.mov(r12, rax);
    c.mov(rdi, rbx);
    c.mov(rax, reinterpret_cast<u64>(post));
    c.call(rax);
    c.mov(rax, r12);
    c.pop(r12);
    c.pop(rbx);
    c.pop(rbp);
    c.ret();
    return entry;
}

bool SwapVtableSlot(u8* slot, u64 expected, u64 replacement, u64& found) {
    found = 0;
    if (slot == nullptr || (reinterpret_cast<uintptr_t>(slot) & 7) != 0) {
        return false;
    }
    auto& word = *reinterpret_cast<u64*>(slot);
    found = std::atomic_ref<u64>(word).load(std::memory_order_acquire);
    if (found != expected) {
        return false;
    }
    if (!IsWritable(slot) && !MakeWritable(slot, sizeof(u64))) {
        return false;
    }
    std::atomic_ref<u64>(word).store(replacement, std::memory_order_release);
    return true;
}

} // namespace VR::Hook
