// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/vr/bloodborne_layout.h"
#include "core/vr/bloodborne_stagger.h"
#include "core/vr/bloodborne_visceral_pull.h"
#include "core/vr/hook_util.h"
#include "core/vr/stagger_speed.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

namespace S = VR::StaggerSpeed;
using Hook::Plausible;
using Hook::Read;
using namespace Layout;

// ChrIns::Update (0x18BD6A0): after its time module's update (0x1A38430) it loads the frame time
// ([[chr+0x3B0]+0x90]+0x18), keeps it at rbp-0x7C and stores it at chr+0xE0; r13 = the ChrIns.
// The detour replaces the store and the next instruction (17 bytes, position-independent).
constexpr u64 SiteRva = 0x18BD716;
constexpr size_t SiteLen = 17;
constexpr std::array<u8, 17> SiteSig = {
    0xC4, 0xC1, 0x7A, 0x11, 0x85, 0xE0, 0x00, 0x00, 0x00, // vmovss [r13+0xE0], xmm0
    0x41, 0x80, 0xA5, 0xE1, 0x01, 0x00, 0x00, 0xF9,       // and byte [r13+0x1E1], 0xF9
};
// The instructions before it (proof): the time module's frame time into xmm0 and rbp-0x7C.
constexpr u64 LoadRva = 0x18BD6FE;
constexpr std::array<u8, 24> LoadSig = {
    0x49, 0x8B, 0x85, 0xB0, 0x03, 0x00, 0x00, // mov rax, [r13+0x3B0]
    0x48, 0x8B, 0x80, 0x90, 0x00, 0x00, 0x00, // mov rax, [rax+0x90]
    0xC5, 0xFA, 0x10, 0x40, 0x18,             // vmovss xmm0, [rax+0x18]
    0xC5, 0xFA, 0x11, 0x45, 0x84,             // vmovss [rbp-0x7C], xmm0
};
// The host path replays this first: the scaled frame time into the function's own copy too.
constexpr std::array<u8, 22> HostReplay = {
    0xC5, 0xFA, 0x11, 0x45, 0x84,                         // vmovss [rbp-0x7C], xmm0
    0xC4, 0xC1, 0x7A, 0x11, 0x85, 0xE0, 0x00, 0x00, 0x00, // vmovss [r13+0xE0], xmm0
    0x41, 0x80, 0xA5, 0xE1, 0x01, 0x00, 0x00, 0xF9,       // and byte [r13+0x1E1], 0xF9
};
// ChrIns::IsMainPlayer: its lea r14, [rip+disp] proves the WorldChrMan global (as the i-frames).
constexpr u64 IsMainPlayerRva = 0x18C2D00;
constexpr std::array<u8, 24> IsMainPlayerSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xFB, 0x4C, 0x8D,
    0x35, 0x67, 0xBB, 0xC7, 0x03, 0x49, 0x8B, 0x06, 0x48, 0x85, 0xC0, 0x75,
};
constexpr size_t IsMainPlayerLeaDisp = 13;
constexpr size_t IsMainPlayerLeaEnd = 17;
constexpr u64 WorldChrManRva = 0x553E878;
constexpr u32 OffsetMainPlayer = 0x60;

u8* g_base = nullptr;
std::unique_ptr<Xbyak::CodeGenerator> g_code;
bool g_installed = false;
bool g_log = false;
S::Shortener g_shortener; // the hook runs on the game's update thread only

// Counters for the log.
u64 g_updates = 0;
u64 g_sped_updates = 0;
u64 g_staggers = 0;
u64 g_ended_gained = 0;
u64 g_ended_window = 0;
u64 g_ended_clip = 0;
s64 g_stats_ns = 0;
s64 g_start_ns = 0; // the running speed-up's start (its log line)

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool ProvesWorldChrMan(const u8* module) {
    if (std::memcmp(module + IsMainPlayerRva, IsMainPlayerSig.data(), IsMainPlayerSig.size()) !=
        0) {
        return false;
    }
    s32 displacement{};
    std::memcpy(&displacement, module + IsMainPlayerRva + IsMainPlayerLeaDisp,
                sizeof(displacement));
    return IsMainPlayerRva + IsMainPlayerLeaEnd + static_cast<s64>(displacement) == WorldChrManRva;
}

// jmp qword [rip+0]; dq target
void EmitAbsoluteJump(Xbyak::CodeGenerator& c, u64 target) {
    c.db(0xFF);
    c.db(0x25);
    c.dd(0);
    c.dq(target);
}

// The game's state of the update before this one: the TimeAct ring's newest clip and the
// requests it accepts. False when a module does not look right (nothing is sped up).
bool ReadState(const u8* chr, S::In& in) {
    const u8* const bag = Read<const u8*>(chr + OffsetChrModules);
    if (!Plausible(bag)) {
        return false;
    }
    const u8* const ta = Read<const u8*>(bag + OffsetTimeActModule);
    if (!Plausible(ta) || Read<const u8*>(ta) != g_base + TimeActVtableRva) {
        return false;
    }
    const s32 write = Read<s32>(ta + OffsetTimeActWrite);
    if (write < 0 || write >= TimeActRingSize) {
        return false;
    }
    const s32 newest = (write - 1 + TimeActRingSize) % TimeActRingSize;
    const u8* const entry = ta + OffsetTimeActRing + newest * TimeActEntrySize;
    in.anim_id = Read<s32>(entry);
    in.clip_time = Read<float>(entry + 8);
    const u8* const requests = Read<const u8*>(bag + OffsetActionRequestModule);
    if (!Plausible(requests) || Read<const u8*>(requests + OffsetOwner) != chr) {
        return false;
    }
    const u64 allow = Read<u64>(requests + OffsetRequestAllow);
    const bool move = (Read<u8>(requests + OffsetRequestMove) & RequestMoveBit) != 0;
    in.window = (allow & 0xB) != 0 || move; // R1, R2, L2 or a move
    return true;
}

const char* EndName(S::Shortener::End e) {
    switch (e) {
    case S::Shortener::End::Gained:
        return "gained it all";
    case S::Shortener::End::Window:
        return "the game's window opened first";
    case S::Shortener::End::NewClip:
        return "a new clip";
    case S::Shortener::End::None:
        break;
    }
    return "?";
}

void PS4_SYSV_ABI StaggerHook(u8* chr, u8* saved_ymm) {
    float dt{};
    std::memcpy(&dt, saved_ymm, sizeof(dt)); // xmm0: the frame time about to be stored
    const u8* const world = Read<const u8*>(g_base + WorldChrManRva);
    if (!Plausible(world) || Read<u8*>(world + OffsetMainPlayer) != chr) {
        // Not the main player: the interactive visceral's held target (the entry lets only it
        // through besides him) runs at the hold's speed with him; while freehand's catch holds
        // the world still (the entry lets every character through) everyone else at its rate.
        const float partner = VisceralPullPartnerRate(chr);
        if (partner != 1.0f && std::isfinite(dt)) {
            const float scaled = dt * partner;
            std::memcpy(saved_ymm, &scaled, sizeof(scaled));
        }
        return;
    }
    ++g_updates;
    const float shorten_s = std::max(VR::GetSettings().stagger_shorten_ms, 0.0f) * 0.001f;
    S::In in{};
    in.dt = dt;
    in.shorten_s = shorten_s;
    const bool state_ok = ReadState(chr, in);
    // The interactive visceral: his throw's clip and time (read before the request check).
    const float hold = VisceralPullMain(chr, state_ok ? in.anim_id : -1, in.clip_time, dt,
                                        state_ok && in.window);
    if (!state_ok) {
        in.anim_id = -1;
    }
    const s32 clip = g_shortener.Id();
    const float gained = g_shortener.Gained();
    const float rate = g_shortener.Step(in) * hold;
    const s64 now = NowNs();
    if (const auto end = g_shortener.TakeEnded(); end != S::Shortener::End::None) {
        (end == S::Shortener::End::Gained   ? g_ended_gained
         : end == S::Shortener::End::Window ? g_ended_window
                                            : g_ended_clip)++;
        const float g = end == S::Shortener::End::Gained ? g_shortener.Gained() : gained;
        LOG_INFO(Core,
                 "VR stagger: a{:03}_{:06} sped up for {} ms (up to x{:.2f}): {:.0f} ms sooner "
                 "({})",
                 (clip >= 0 ? clip : 0) / 1'000'000, (clip >= 0 ? clip : 0) % 1'000'000,
                 (now - g_start_ns) / 1'000'000, S::MaxRate, g * 1000.0f, EndName(end));
    }
    if (g_shortener.TakeStarted()) {
        ++g_staggers;
        g_start_ns = now;
    }
    if (rate != 1.0f && std::isfinite(dt)) {
        ++g_sped_updates;
        const float scaled = dt * rate;
        std::memcpy(saved_ymm, &scaled, sizeof(scaled)); // stored at chr+0xE0 and rbp-0x7C
    }
    if (g_log && now - g_stats_ns > 10'000'000'000LL) {
        g_stats_ns = now;
        LOG_INFO(Core,
                 "VR stagger: STATS updates={} sped={} staggers={} (gained {}, window first {}, new "
                 "clip {}) shorten={:.0f} ms",
                 g_updates, g_sped_updates, g_staggers, g_ended_gained, g_ended_window,
                 g_ended_clip, shorten_s * 1000.0f);
    }
}

} // Anonymous namespace

void InstallStaggerSpeed(VAddr base, u64 size) {
    if (g_code || g_installed) {
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    g_base = module;
    g_log = std::getenv("VR_STAGGER_LOG") != nullptr;
    const float shorten = VR::GetSettings().stagger_shorten_ms;
    const bool pull = VisceralPullWanted(); // the interactive visceral rides on the same detour
    if (!(shorten > 0.0f) && !pull) {
        LOG_INFO(Core, "VR stagger: stagger_shorten_ms=0, the game's staggers (not installed)");
        return;
    }
    if (size <= WorldChrManRva + 8) {
        LOG_WARNING(Core, "VR stagger: eboot too small ({:#x}); not installed", size);
        return;
    }
    const auto matches = [module](u64 rva, const auto& signature) {
        return std::memcmp(module + rva, signature.data(), signature.size()) == 0;
    };
    const bool site_ok = matches(SiteRva, SiteSig);
    const bool load_ok = matches(LoadRva, LoadSig);
    const bool wcm_ok = ProvesWorldChrMan(module);
    if (!site_ok || !load_ok || !wcm_ok) {
        LOG_WARNING(Core,
                    "VR stagger: not installed (site {}, frame time load {}, WorldChrMan {}); live "
                    "bytes at {:#x}: {}",
                    site_ok ? "ok" : "MISMATCH", load_ok ? "ok" : "MISMATCH",
                    wcm_ok ? "ok" : "MISMATCH", SiteRva,
                    Hook::HexBytes(module + SiteRva, SiteSig.size()));
        return;
    }
    u8* const site = module + SiteRva;
    const u64 resume = reinterpret_cast<u64>(site + SiteLen);
    try {
        g_code = std::make_unique<Xbyak::CodeGenerator>(1024);
        using namespace Xbyak::util;
        auto& c = *g_code;
        void* const host =
            Hook::EmitHostCallStub(c, reinterpret_cast<void*>(&StaggerHook), HostReplay.data(),
                                   HostReplay.size(), resume, r13);
        // Entry: only the main player goes through the host call (rax is dead here: the next
        // use loads it; the flags are rewritten by the replayed `and`), and the interactive
        // visceral's held target while a hold runs (its slot is null otherwise), and every
        // character while freehand's catch holds the world still (its qword is 0 otherwise);
        // everyone else replays the two instructions and goes on.
        c.align(16);
        auto* const entry = const_cast<u8*>(c.getCurr());
        Xbyak::Label skip, main_player, to_host;
        c.mov(rax, reinterpret_cast<u64>(VisceralPullWorldSlot()));
        c.cmp(qword[rax], 0);
        c.jne(to_host, Xbyak::CodeGenerator::T_NEAR);
        c.mov(rax, reinterpret_cast<u64>(VisceralPullPartnerSlot()));
        c.mov(rax, ptr[rax]);
        c.test(rax, rax);
        c.jz(main_player);
        c.cmp(r13, rax);
        c.jne(main_player);
        c.jmp(host, Xbyak::CodeGenerator::T_NEAR);
        c.L(main_player);
        c.mov(rax, reinterpret_cast<u64>(module + WorldChrManRva));
        c.mov(rax, ptr[rax]);
        c.test(rax, rax);
        c.jz(skip);
        c.cmp(r13, ptr[rax + OffsetMainPlayer]);
        c.jne(skip);
        c.jmp(host, Xbyak::CodeGenerator::T_NEAR);
        c.L(skip);
        for (size_t i = 0; i < SiteLen; ++i) {
            c.db(SiteSig[i]);
        }
        EmitAbsoluteJump(c, resume);
        c.L(to_host);
        c.jmp(host, Xbyak::CodeGenerator::T_NEAR);
        c.ready();
        if (!Hook::MakeWritable(site, SiteLen)) {
            LOG_ERROR(Core, "VR stagger: could not make {:#x} writable; not installed", SiteRva);
            return;
        }
        Hook::WriteJump(site, SiteLen, entry);
        g_installed = true;
    } catch (const Xbyak::Error& error) {
        LOG_ERROR(Core, "VR stagger: code emission failed ({}); not installed", error.what());
        return;
    }
    LOG_INFO(Core,
             "VR stagger: installed at {:#x} (ChrIns::Update frame time): hit staggers "
             "(a000_020000-020399) end {:.0f} ms sooner (played up to x{:.2f} from their start){}",
             SiteRva, shorten, S::MaxRate,
             pull ? "; the interactive visceral's hold (visceral_pull; freehand: the world's too)"
                  : "");
}

bool IsStaggerSpeedInstalled() {
    return g_installed;
}

} // namespace VR::Bloodborne
