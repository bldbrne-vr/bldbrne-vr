// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The game's GPU memory, as bbport's guest hooks see it (bbport gpu/shim/bbport_guest_hooks.cpp,
// the same sites of the same eboot):
//
// - Its allocators hand a range out again: whatever the GPU wrote there is dead. The GPU side
//   forgets it (BufferCache::NoteFreshRange), else FAST_GPU_PAGE_KEEP kept it over the new data.
//   The broken street (2026-10-26): lamp trips between Central Yharnam and the Sickroom reused a
//   model's memory for GPU output in one place, then loaded the model again into it in the other.
//   Its bytes equalled the guest's old ones, so to the keep "the CPU changed nothing" and the dead
//   output stayed: 31 vertices hundreds of km away, the ground smeared to the horizon.
// - Its resource loaders copy model and texture data into GPU memory: the GPU side hears of the
//   copy first, as of a file read (MemoryManager::InvalidateMemory), so the copy faults on none of
//   its pages (one call instead of a write fault per 4 KB page). Small copies fault as before.
//
// Every site is compared byte for byte with the expected code first: another version of the game
// is left as it is. FAST_GPU_FRESH=0 / FAST_LOADER_COPY=0 turn either part off (read once).

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/memory.h"
#include "core/vr/bloodborne_gpu_memory.h"
#include "core/vr/hook_util.h"
#include "core/vr/near_code.h"

namespace VR::Bloodborne {
namespace {

// The range allocator's epilogue (allocator 0x26aa070: rax a node; [node + 8] the start,
// [[node + 0x18] + 8] the end): add rsp, 0x18; pop rbx; pop r12; pop r13; pop r14; pop r15;
// pop rbp; ret. Reached by the allocator's own branches only, at its first instruction.
constexpr u64 AllocReturnRva = 0x26aa255;
constexpr std::array<u8, 15> AllocReturnBytes = {0x48, 0x83, 0xC4, 0x18, 0x5B, 0x41, 0x5C, 0x41,
                                                 0x5D, 0x41, 0x5E, 0x41, 0x5F, 0x5D, 0xC3};

// The other GPU heap's three allocation call sites, just after the call (rax the address,
// [rbp - 0x50] the size): mov rbx, rax; test rbx, rbx; je rel32 <miss>; mov rax, [rbp - 0x50].
// The je is replayed as an absolute jump to its target.
constexpr std::array<u64, 3> HeapReturnRvas = {0x11e6629, 0x11e6736, 0x11e6846};
constexpr std::array<u8, 6> HeapReturnHead = {0x48, 0x89, 0xC3, 0x48, 0x85, 0xDB};
constexpr std::array<u8, 4> HeapReturnTail = {0x48, 0x8B, 0x45, 0xB0};

// `call memcpy` (0x261a1b0) in the resource loaders 0x216d420 and 0x2171d60.
constexpr u64 MemcpyRva = 0x261a1b0;
constexpr std::array<u64, 2> LoaderCallRvas = {0x216d5ea, 0x2171fea};
constexpr u64 LoaderCopyMin = 8 * 1024; // smaller: as before (at most a fault or two)

std::atomic<u64> fresh_ranges{0}, fresh_bytes{0}, loader_copies{0}, loader_bytes{0};
std::unique_ptr<Xbyak::CodeGenerator> code;

void NoteFresh(u64 address, u64 size) {
    if (address == 0 || size == 0 || size >= (u64{1} << 32)) {
        return;
    }
    const u64 n = fresh_ranges.fetch_add(1, std::memory_order_relaxed) + 1;
    const u64 bytes = fresh_bytes.fetch_add(size, std::memory_order_relaxed) + size;
    Core::Memory::Instance()->NoteFreshGpuRange(address, size);
    if ((n & 4095) == 0) {
        LOG_INFO(Core, "GPU memory: {} ranges handed out again so far ({} MiB)", n, bytes >> 20);
    }
}

void PS4_SYSV_ABI OnRangeAllocated(u64 node) {
    if (!Hook::Plausible(reinterpret_cast<const u8*>(node))) {
        return;
    }
    const u64 start = Hook::Read<u64>(reinterpret_cast<const u8*>(node + 8));
    const u64 next = Hook::Read<u64>(reinterpret_cast<const u8*>(node + 0x18));
    const u64 end = Hook::Plausible(reinterpret_cast<const u8*>(next))
                        ? Hook::Read<u64>(reinterpret_cast<const u8*>(next + 8))
                        : start;
    if (end > start) {
        NoteFresh(start, end - start);
    }
}

void PS4_SYSV_ABI OnHeapAllocated(u64 address, u64 size) {
    NoteFresh(address, size);
}

void* PS4_SYSV_ABI LoaderCopy(void* dst, const void* src, std::size_t size) {
    if (size >= LoaderCopyMin) {
        const u64 n = loader_copies.fetch_add(1, std::memory_order_relaxed) + 1;
        const u64 bytes = loader_bytes.fetch_add(size, std::memory_order_relaxed) + size;
        if ((n & 1023) == 0) {
            LOG_INFO(Core, "GPU memory: {} loader copies announced so far ({} MiB)", n,
                     bytes >> 20);
        }
        // Its pages lift their protection and take the copy as the emulator's own write: every
        // byte uploads, none faults. A GPU upload between this and the copy re-protects them;
        // the rest of the copy then faults as before.
        Core::Memory::Instance()->InvalidateMemory(reinterpret_cast<VAddr>(dst), size);
    }
    return std::memcpy(dst, src, size);
}

bool Env(const char* name) {
    const char* value = std::getenv(name);
    return !(value && value[0] == '0');
}

void InstallFresh(u8* base) {
    using namespace Xbyak::util;
    u8* const alloc = base + AllocReturnRva;
    if (std::memcmp(alloc, AllocReturnBytes.data(), AllocReturnBytes.size()) != 0) {
        LOG_WARNING(Core, "GPU memory: allocator epilogue differs ({}); not hooked",
                    Hook::HexBytes(alloc, AllocReturnBytes.size()));
        return;
    }
    for (const u64 rva : HeapReturnRvas) {
        const u8* site = base + rva;
        if (std::memcmp(site, HeapReturnHead.data(), HeapReturnHead.size()) != 0 ||
            site[6] != 0x0F || site[7] != 0x84 ||
            std::memcmp(site + 12, HeapReturnTail.data(), HeapReturnTail.size()) != 0) {
            LOG_WARNING(Core, "GPU memory: heap call site {:#x} differs ({}); not hooked", rva,
                        Hook::HexBytes(site, 16));
            return;
        }
    }
    // The allocator: the node in rax; the epilogue (ret included) replayed, so the jump back is
    // never reached.
    void* const alloc_stub = Hook::EmitHostCallStubWith(
        *code, reinterpret_cast<void*>(&OnRangeAllocated), AllocReturnBytes.data(),
        AllocReturnBytes.size(), reinterpret_cast<u64>(alloc + AllocReturnBytes.size()),
        [](Xbyak::CodeGenerator& c) { c.mov(rdi, rax); });
    std::array<void*, HeapReturnRvas.size()> heap_stubs{};
    for (size_t i = 0; i < HeapReturnRvas.size(); ++i) {
        const u8* site = base + HeapReturnRvas[i];
        s32 rel{};
        std::memcpy(&rel, site + 8, sizeof(rel));
        const u64 miss = reinterpret_cast<u64>(site + 12) + static_cast<s64>(rel);
        // mov rbx, rax; test rbx, rbx; jnz +14; jmp qword [rip+0]; dq miss; mov rax, [rbp-0x50]
        std::array<u8, 6 + 2 + 14 + 4> replay{};
        std::memcpy(replay.data(), HeapReturnHead.data(), HeapReturnHead.size());
        replay[6] = 0x75;
        replay[7] = 14;
        replay[8] = 0xFF;
        replay[9] = 0x25;
        std::memcpy(replay.data() + 14, &miss, sizeof(miss));
        std::memcpy(replay.data() + 22, HeapReturnTail.data(), HeapReturnTail.size());
        // The replay bytes are copied into the stub as it is emitted.
        heap_stubs[i] = Hook::EmitHostCallStubWith(
            *code, reinterpret_cast<void*>(&OnHeapAllocated), replay.data(), replay.size(),
            reinterpret_cast<u64>(site + 16), [](Xbyak::CodeGenerator& c) {
                c.mov(rdi, rax);
                c.mov(rsi, qword[rbp - 0x50]);
            });
    }
    if (!Hook::MakeWritable(alloc, AllocReturnBytes.size())) {
        LOG_WARNING(Core, "GPU memory: allocator not writable; not hooked");
        return;
    }
    for (const u64 rva : HeapReturnRvas) {
        if (!Hook::MakeWritable(base + rva, 16)) {
            LOG_WARNING(Core, "GPU memory: heap call site not writable; not hooked");
            return;
        }
    }
    Hook::WriteJump(alloc, AllocReturnBytes.size(), alloc_stub);
    for (size_t i = 0; i < HeapReturnRvas.size(); ++i) {
        Hook::WriteJump(base + HeapReturnRvas[i], 16, heap_stubs[i]);
    }
    LOG_INFO(Core, "GPU memory: ranges the game hands out again forget their old GPU data "
                   "(range allocator + {} heap sites)",
             HeapReturnRvas.size());
}

void InstallLoaderCopies(u8* base, u64 size) {
    for (const u64 rva : LoaderCallRvas) {
        s32 rel{};
        std::memcpy(&rel, base + rva + 1, sizeof(rel));
        if (base[rva] != 0xE8 || rva + 5 + static_cast<s64>(rel) != MemcpyRva) {
            LOG_WARNING(Core, "GPU memory: loader copy site {:#x} differs ({}); not hooked", rva,
                        Hook::HexBytes(base + rva, 5));
            return;
        }
    }
    // jmp qword [rip+0]; dq LoaderCopy: the calls' new target, within rel32 reach.
    u8* const trampoline = Hook::NearAlloc(reinterpret_cast<u64>(base), size, 14, 16);
    if (trampoline == nullptr) {
        LOG_WARNING(Core, "GPU memory: no near memory; loader copies not hooked");
        return;
    }
    const u64 target = reinterpret_cast<u64>(&LoaderCopy);
    trampoline[0] = 0xFF;
    trampoline[1] = 0x25;
    std::memset(trampoline + 2, 0, 4);
    std::memcpy(trampoline + 6, &target, sizeof(target));
    Hook::FlushCode(trampoline, 14);
    std::array<s32, LoaderCallRvas.size()> rels{};
    for (size_t i = 0; i < LoaderCallRvas.size(); ++i) {
        const u64 next = reinterpret_cast<u64>(base + LoaderCallRvas[i] + 5);
        if (!Hook::Rel32(next, reinterpret_cast<u64>(trampoline), rels[i]) ||
            !Hook::MakeWritable(base + LoaderCallRvas[i], 5)) {
            LOG_WARNING(Core, "GPU memory: loader copy site out of reach; not hooked");
            return;
        }
    }
    for (size_t i = 0; i < LoaderCallRvas.size(); ++i) {
        // The 4 bytes of the call's displacement, one aligned-enough store each.
        std::memcpy(base + LoaderCallRvas[i] + 1, &rels[i], sizeof(s32));
        Hook::FlushCode(base + LoaderCallRvas[i], 5);
    }
    LOG_INFO(Core, "GPU memory: resource loader copies of {} KB or more announce their writes "
                   "(no page faults)",
             LoaderCopyMin / 1024);
}

} // namespace

void GpuMemoryEbootLoaded(VAddr address, u64 size) {
    u8* const base = reinterpret_cast<u8*>(address);
    if (size <= AllocReturnRva + 16 || size <= MemcpyRva) {
        return;
    }
    if (!code) {
        code = std::make_unique<Xbyak::CodeGenerator>(4096);
    }
    try {
        if (Env("FAST_GPU_FRESH")) {
            InstallFresh(base);
        }
    } catch (const Xbyak::Error& e) {
        LOG_ERROR(Core, "GPU memory: allocator hooks not installed: {}", e.what());
    }
    if (Env("FAST_LOADER_COPY")) {
        InstallLoaderCopies(base, size);
    }
}

} // namespace VR::Bloodborne
