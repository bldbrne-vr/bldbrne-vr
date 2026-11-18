// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-FileCopyrightText: 2026 bbport contributors (deadinside28/bloodborne_pc)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Adapted from bbport 0fc2921, gpu/games/bloodborne/hooks_jobs.cpp. The game's
// six-slot render-job pool leaks a slot on each failed job. Laurence's opening
// cinematic can exhaust it at high FPS, leaving all its workers waiting forever.
// Repair the error epilogue and the two cancellation/reset sites, preserving the
// guest's registers, flags and red zone. No worker/queue replacement or FPS change.

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
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

void RenderJobsEbootLoaded(VAddr base, u64 size) {
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
