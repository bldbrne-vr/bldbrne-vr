// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-FileCopyrightText: 2026 bbport contributors (deadinside28/bloodborne_pc)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Adapted from bbport 0fc2921, gpu/games/bloodborne/hooks_jobs.cpp. The game's
// six-slot render-job pool leaks a slot on each failed job. Laurence's opening
// cinematic can exhaust it at high FPS, leaving all its workers waiting forever.
// Repair the error epilogue and the two cancellation/reset sites, preserving the
// guest's registers, flags and red zone. No worker/queue replacement or FPS change.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/memory.h"
#include "core/vr/bloodborne_render_jobs.h"
#include "core/vr/hook_util.h"
#include "core/vr/near_code.h"

namespace VR::Bloodborne {
namespace {
constexpr u64 FailureRva = 0x1068092;
constexpr std::array<u8, 7> FailureBytes{0x48, 0x8b, 0x03, 0x48, 0x3b, 0x45, 0xd0};
constexpr std::array<u64, 2> DropRvas{0x106798a, 0x1067a5c};
constexpr std::array<u8, 11> DropBytes{0x48, 0xc7, 0x83, 0x20, 0x01, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00};
constexpr u64 GiveBackRva = 0x1056990;
constexpr std::array<u8, 13> GiveBackBytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                           0x56, 0x53, 0x50, 0x49, 0x89, 0xff};
constexpr u64 QueueRva = 0x54b8788;
constexpr u64 HeldOffset = 0x120;
using GiveBack = void(PS4_SYSV_ABI*)(u64 queue, u64 item);
GiveBack give_back{};
u8* module{};
std::unique_ptr<Xbyak::CodeGenerator> code;
std::atomic<u64> returned_slots{};

void NoteReturn(const char* reason) {
    const u64 n = returned_slots.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 5 || (n & (n - 1)) == 0) {
        LOG_INFO(Core, "Render jobs: returned a held slot on {} ({} repaired exits)", reason, n);
    }
}

void PS4_SYSV_ABI OnFailure(u8* runner, u64 queue, u32 result) {
    // BBPort's repair: only the failed-job result can leave a slot held here.
    if (result != 1)
        return;
    auto& held = *reinterpret_cast<u64*>(runner + HeldOffset);
    if (!held)
        return;
    give_back(queue, held);
    held = 0;
    NoteReturn("failure");
}

void PS4_SYSV_ABI OnDrop(u8* runner) {
    auto& held = *reinterpret_cast<u64*>(runner + HeldOffset);
    if (!held)
        return;
    const u64 queue = Hook::Read<u64>(module + QueueRva);
    if (!queue)
        return;
    give_back(queue, held);
    held = 0;
    NoteReturn("cancel/reset");
}

struct Detour {
    u8* site{};
    size_t length{};
    s32 displacement{};
};

// The original sites are 7/11 bytes, too short for the usual 14-byte jump. Only
// the tiny relay is near the image; the register-saving host-call stub can be anywhere.
bool Prepare(u64 base, u64 size, u64 rva, const u8* original, size_t length, void* callback,
             const std::function<void(Xbyak::CodeGenerator&)>& args, Detour& out) {
    using namespace Xbyak::util;
    auto& c = *code;
    c.align(16);
    void* const stub = const_cast<u8*>(c.getCurr());
    // Skip the host call and SIMD saves when BBPort's callback would do nothing.
    // Preserve flags and the red zone: the reset sites only overwrite a MOV.
    const bool failure = rva == FailureRva;
    Xbyak::Label plain, repair;
    c.lea(rsp, ptr[rsp - 128]);
    c.pushf();
    if (failure) {
        c.cmp(r15d, 1);
        c.jne(plain);
    }
    c.cmp(qword[(failure ? r14 : rbx) + HeldOffset], 0);
    c.jne(repair);
    c.L(plain);
    c.popf();
    c.lea(rsp, ptr[rsp + 128]);
    for (size_t i = 0; i < length; ++i)
        c.db(original[i]);
    c.db(0xff);
    c.db(0x25);
    c.dd(0);
    c.dq(base + rva + length);
    c.L(repair);
    c.popf();
    c.lea(rsp, ptr[rsp + 128]);
    Hook::EmitHostCallStubWith(c, callback, original, length, base + rva + length, args);
    u8* relay = Hook::NearAlloc(base, size, 14);
    if (!relay || !Hook::Rel32(base + rva + 5, reinterpret_cast<u64>(relay), out.displacement)) {
        return false;
    }
    relay[0] = 0xff;
    relay[1] = 0x25;
    std::memset(relay + 2, 0, 4);
    const auto target = reinterpret_cast<u64>(stub);
    std::memcpy(relay + 6, &target, sizeof(target));
    Hook::FlushCode(relay, 14);
    out.site = reinterpret_cast<u8*>(base + rva);
    out.length = length;
    return Hook::MakeWritable(out.site, out.length);
}
} // namespace

namespace {
// The render resource cache (adapted from bbport 0fc2921, gpu/games/bloodborne/hooks_jobs.cpp
// ResourceCacheBacking). The cache the render jobs build their draws' tables in has its own
// 160 MiB range heap, apart from the graphics heaps. Laurence's opening cinematic fills it:
// allocations of 20-40 KiB fail, each failed job leaves its draws without their resource tables
// (null pointers in the user data), and the picture loses everything near (2026-11-18: 8192+
// failed jobs within seconds, the same at 30 and 60 fps and at any render size; the emulator read
// through the null tables and ended, the "crash on triggering the Laurence fight"). The game
// allocates its 160 MiB as always; right after, its result and the size the range heap is set up
// with are replaced by 512 MiB of direct memory of our own (bbport: 320 MiB still failed), so
// nothing is taken from what the game's areas load into. Without that much direct memory free the
// game's own cache stays. VR_RENDER_RESOURCE_CACHE_MB=160: the game's cache as it is.
constexpr u64 CacheRva = 0x216b21d;
// mov rcx, rbx; mov [rcx + 0x10], rax: right after the allocator's Allocate(size, 16) returns.
constexpr std::array<u8, 7> CacheBytes{0x48, 0x89, 0xd9, 0x48, 0x89, 0x41, 0x10};

u64 PS4_SYSV_ABI OnCacheBacking(u64 block, u8* record) {
    constexpr u32 native_bytes = 160u << 20;
    u32 asked{};
    std::memcpy(&asked, record + 0x54, sizeof(asked));
    if (asked != native_bytes || block == 0) {
        return block;
    }
    static const u32 bytes = [] {
        const char* env = std::getenv("VR_RENDER_RESOURCE_CACHE_MB");
        const long mb = env != nullptr ? std::strtol(env, nullptr, 10) : 512;
        return static_cast<u32>(std::clamp(mb, 160l, 512l)) << 20;
    }();
    if (bytes == native_bytes) {
        return block;
    }
    auto* const memory = Core::Memory::Instance();
    constexpr u64 alignment = 64ull << 10;
    const PAddr phys = memory->Allocate(0, memory->GetTotalDirectSize(), bytes, alignment, 0);
    if (phys == static_cast<PAddr>(-1)) {
        LOG_WARNING(Core, "Render resource cache: no direct memory for {} MiB: the game's 160 MiB",
                    bytes >> 20);
        return block;
    }
    void* address = nullptr;
    const s32 result = memory->MapMemory(
        &address, 0, bytes, Core::MemoryProt::CpuReadWrite | Core::MemoryProt::GpuReadWrite,
        Core::MemoryMapFlags::NoFlags, Core::VMAType::Direct, "render resource cache", false, phys,
        alignment);
    if (result != 0 || address == nullptr) {
        memory->Free(phys, bytes, false);
        LOG_WARNING(Core, "Render resource cache: {} MiB not mapped (error {:#x}): the game's 160 MiB",
                    bytes >> 20, static_cast<u32>(result));
        return block;
    }
    // (The creation record can be a stack object.)
    std::memcpy(record + 0x54, &bytes, sizeof(bytes));
    LOG_INFO(Core, "Render resource cache: {} MiB of our direct memory at {} (the game's 160 MiB "
                   "stays allocated)", bytes >> 20, fmt::ptr(address));
    return reinterpret_cast<u64>(address);
}

// The site follows a call's return: the volatile registers and the flags are dead, the stack is
// aligned and no red zone is in use. The stub hands the block (rax) and the creation record (r14)
// over, takes the block to use back in rax, runs the two instructions the jump replaced and
// returns behind them.
std::unique_ptr<Xbyak::CodeGenerator> cache_code;
void InstallResourceCache(VAddr base, u64 size) {
    using namespace Xbyak::util;
    if (cache_code || size < CacheRva + CacheBytes.size()) {
        return;
    }
    auto* const site = reinterpret_cast<u8*>(base + CacheRva);
    if (std::memcmp(site, CacheBytes.data(), CacheBytes.size()) != 0) {
        LOG_WARNING(Core, "Render resource cache: 1.09 signature differs; the game's 160 MiB");
        return;
    }
    cache_code = std::make_unique<Xbyak::CodeGenerator>(4096);
    auto& c = *cache_code;
    s32 displacement{};
    u8* relay{};
    try {
        void* const stub = const_cast<u8*>(c.getCurr());
        c.mov(rdi, rax);
        c.mov(rsi, r14);
        c.mov(rax, reinterpret_cast<u64>(&OnCacheBacking));
        c.call(rax);
        for (const u8 b : CacheBytes) {
            c.db(b);
        }
        c.db(0xff);
        c.db(0x25);
        c.dd(0);
        c.dq(base + CacheRva + CacheBytes.size());
        c.ready();
        relay = Hook::NearAlloc(base, size, 14);
        if (relay == nullptr ||
            !Hook::Rel32(base + CacheRva + 5, reinterpret_cast<u64>(relay), displacement)) {
            LOG_WARNING(Core, "Render resource cache: no relay near the code; the game's 160 MiB");
            return;
        }
        relay[0] = 0xff;
        relay[1] = 0x25;
        std::memset(relay + 2, 0, 4);
        const auto target = reinterpret_cast<u64>(stub);
        std::memcpy(relay + 6, &target, sizeof(target));
        Hook::FlushCode(relay, 14);
    } catch (const Xbyak::Error& e) {
        LOG_ERROR(Core, "Render resource cache: stub not emitted: {}", e.what());
        return;
    }
    if (!Hook::MakeWritable(site, CacheBytes.size())) {
        return;
    }
    site[0] = 0xe9;
    std::memcpy(site + 1, &displacement, sizeof(displacement));
    std::memset(site + 5, 0x90, CacheBytes.size() - 5);
    Hook::FlushCode(site, CacheBytes.size());
    LOG_INFO(Core, "Render resource cache: its backing is replaced when the game creates it");
}
} // namespace

void RenderJobsEbootLoaded(VAddr base, u64 size) {
    if (const char* cache = std::getenv("VR_RENDER_RESOURCE_CACHE_MB");
        cache == nullptr || std::strtol(cache, nullptr, 10) != 160) {
        InstallResourceCache(base, size);
    }
    const char* enabled = std::getenv("VR_RENDER_JOB_FIX");
    if (code || (enabled && enabled[0] == '0'))
        return;
    if (size < QueueRva + sizeof(u64))
        return;
    auto* image = reinterpret_cast<u8*>(base);
    // Preflight the complete group before changing any guest instruction. A different
    // executable or another patch leaves all three sites alone.
    if (std::memcmp(image + FailureRva, FailureBytes.data(), FailureBytes.size()) != 0 ||
        std::memcmp(image + DropRvas[0], DropBytes.data(), DropBytes.size()) != 0 ||
        std::memcmp(image + DropRvas[1], DropBytes.data(), DropBytes.size()) != 0 ||
        std::memcmp(image + GiveBackRva, GiveBackBytes.data(), GiveBackBytes.size()) != 0) {
        LOG_WARNING(Core, "Render jobs: 1.09 signatures differ; slot repair not installed");
        return;
    }
    code = std::make_unique<Xbyak::CodeGenerator>(8192);
    std::array<Detour, 3> detours{};
    using namespace Xbyak::util;
    try {
        if (!Prepare(
                base, size, FailureRva, FailureBytes.data(), FailureBytes.size(),
                reinterpret_cast<void*>(&OnFailure),
                [](Xbyak::CodeGenerator& c) {
                    c.mov(rdi, r14);
                    c.mov(rsi, r12); // our save helper keeps the guest's r12 intact
                    c.mov(edx, r15d);
                },
                detours[0]))
            return;
        for (size_t i = 0; i < DropRvas.size(); ++i) {
            if (!Prepare(
                    base, size, DropRvas[i], DropBytes.data(), DropBytes.size(),
                    reinterpret_cast<void*>(&OnDrop),
                    [](Xbyak::CodeGenerator& c) { c.mov(rdi, rbx); }, detours[i + 1]))
                return;
        }
    } catch (const Xbyak::Error& e) {
        LOG_ERROR(Core, "Render jobs: stubs not emitted; slot repair not installed: {}", e.what());
        return;
    }
    module = image;
    give_back = reinterpret_cast<GiveBack>(base + GiveBackRva);
    for (const auto& d : detours) {
        d.site[0] = 0xe9;
        std::memcpy(d.site + 1, &d.displacement, sizeof(d.displacement));
        std::memset(d.site + 5, 0x90, d.length - 5);
        Hook::FlushCode(d.site, d.length);
    }
    LOG_INFO(Core, "Render jobs: failed/cancelled jobs return their slots (3 verified hooks)");
}
} // namespace VR::Bloodborne
