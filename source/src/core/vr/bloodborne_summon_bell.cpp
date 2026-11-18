// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "core/vr/summon_bell.h"
#include "core/vr/vr_session.h"
#include "core/vr/hook_util.h"
#include "core/vr/near_code.h"
#include "common/logging/log.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <vector>

namespace VR::Bloodborne {
namespace {
constexpr u64 Site=0x144e7b8, End=0x144e7d9, Native=0x1cd91f0;
constexpr std::array<u8,33> Signature={0xbe,0x10,0,0,0,0xc5,0xf8,0x57,0xc0,0x31,0xc9,0x45,0x31,0xc0,0x45,0x31,0xc9,0x89,0xda,0xe8,0x20,0xaa,0x88,0,0x84,0xc0,0x74,0x05,0x41,0x80,0x4e,0x10,0x01};
using PromptFn=bool(PS4_SYSV_ABI *)(void*,int,int,int,int,int,int,int,float);
PromptFn prompt{};
std::unique_ptr<Xbyak::CodeGenerator> code;
// The summon event's ringing is EMEVD ForceAnimationPlayback (10000, 100111). Its two 1.09
// handlers (an object or a character by entity id) write the character's event-animation request
// inline, then return: mov edx,[rbp-0x30] (the clip); mov [rax+0x20],edx (rax: its
// SprjChrEventModule); mov rax,[rcx+0x20] (rcx: the character); mov [rax+4],edx (the sticky copy).
// SprjChrEventModule::SetEventAnim (0x1A59190) has the same body, but nothing in 1.09 calls it
// (no call, jump, vtable entry or address reference): the first hook there never ran (the
// 2026-10-30 headset log: ringing accepted, the game's own ringing played).
constexpr std::array<u64,2> PlayAnimRvas={0x13CBFA6,0x13CC15A};
constexpr std::array<u8,13> PlayAnimWrite={0x8B,0x55,0xD0, 0x89,0x50,0x20, 0x48,0x8B,0x41,0x20,
                                           0x89,0x50,0x04};
constexpr u64 WorldChrManRva=0x553E878; // +0x60: the main player
u8* ring_module{};
std::unique_ptr<Xbyak::CodeGenerator> ring_code;
struct RingSite { u8* site; s32 displacement; };
std::vector<RingSite> ring_sites;
bool PS4_SYSV_ABI SkipRing(u8* chr, int anim) {
    static std::atomic<bool> seen{false};
    if (!seen.exchange(true)) // proof that the handlers run through the hook
        LOG_INFO(Core,"VR summon bell: first ForceAnimationPlayback through the hook: clip {}",anim);
    if (anim<0 || anim%1000000!=100111) return false;
    u8* const world=Hook::Read<u8*>(ring_module+WorldChrManRva);
    if (world==nullptr || Hook::Read<u8*>(world+0x60)!=chr) return false;
    const double now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool skip=VR::IsFirstPerson() && SummonBell::SkipRingAnimation(now,anim);
    static int logged=0;
    if (logged<8) {
        ++logged;
        LOG_INFO(Core,"VR summon bell: the game's ringing (clip {}) {}",anim,
                 skip ? "skipped" : "played (no physical ring just before)");
    }
    return skip;
}
// Saves the guest state, asks SkipRing(rcx, the clip) and on true goes straight to the handler's
// epilogue (as its not-ready path does: nothing written); else the original writes. edx carries
// the answer back (the writes load it again). The 13 bytes take a rel32 jump to a near relay.
bool EmitRingSkip(u64 base, u64 size, u64 rva) {
    using namespace Xbyak::util;
    auto& c=*ring_code;
    c.align(16);
    const u8* entry=c.getCurr();
    c.lea(rsp,ptr[rsp-128]); // the guest red zone
    c.pushf();
    for (const auto& reg : {rax,rcx,rdx,rbx,rsi,rdi,r8,r9,r10,r11,rbp}) c.push(reg);
    c.mov(esi,dword[rbp-0x30]); // the clip (the guest's frame, before rbp is ours)
    c.mov(rdi,rcx);             // the character
    c.mov(rbp,rsp);
    c.and_(rsp,-32);
    c.sub(rsp,32*16);
    for (int i=0;i<16;++i) c.vmovdqu(ptr[rsp+32*i],Xbyak::Ymm(i));
    c.mov(rax,reinterpret_cast<u64>(&SkipRing));
    c.call(rax);
    for (int i=0;i<16;++i) c.vmovdqu(Xbyak::Ymm(i),ptr[rsp+32*i]);
    c.mov(rsp,rbp);
    c.mov(ptr[rsp+8*8],rax); // the saved rdx slot carries the answer
    for (const auto& reg : {rbp,r11,r10,r9,r8,rdi,rsi,rbx,rdx,rcx,rax}) c.pop(reg);
    c.popf();
    c.lea(rsp,ptr[rsp+128]);
    Xbyak::Label skip;
    c.test(dl,dl);
    c.jnz(skip,Xbyak::CodeGenerator::T_NEAR);
    for (const u8 b : PlayAnimWrite) c.db(b);
    c.L(skip);
    c.db(0xff); // jmp qword [rip+0]: back after the writes
    c.db(0x25);
    c.dd(0);
    c.dq(base+rva+PlayAnimWrite.size());
    u8* relay=Hook::NearAlloc(base,size,14);
    s32 displacement{};
    if (!relay || !Hook::Rel32(base+rva+5,reinterpret_cast<u64>(relay),displacement)) return false;
    relay[0]=0xff;
    relay[1]=0x25;
    std::memset(relay+2,0,4);
    const auto target=reinterpret_cast<u64>(entry);
    std::memcpy(relay+6,&target,sizeof(target));
    Hook::FlushCode(relay,14);
    u8* site=reinterpret_cast<u8*>(base+rva);
    if (!Hook::MakeWritable(site,PlayAnimWrite.size())) return false;
    ring_sites.push_back({site,displacement});
    return true;
}
void InstallRingSkip(u64 base, u64 size) {
    auto* module=reinterpret_cast<u8*>(base);
    for (const u64 rva : PlayAnimRvas) {
        if (rva+PlayAnimWrite.size()>size ||
            std::memcmp(module+rva,PlayAnimWrite.data(),PlayAnimWrite.size())!=0) {
            LOG_WARNING(Core,"VR summon bell: unsupported ForceAnimationPlayback code at {:#x}; the "
                             "game's ringing kept",rva);
            return;
        }
    }
    ring_module=module;
    ring_code=std::make_unique<Xbyak::CodeGenerator>(4096);
    for (const u64 rva : PlayAnimRvas) {
        if (!EmitRingSkip(base,size,rva)) {
            LOG_WARNING(Core,"VR summon bell: no near relay for {:#x}; the game's ringing kept",rva);
            ring_sites.clear();
            return;
        }
    }
    ring_code->ready();
    Hook::FlushCode(const_cast<u8*>(ring_code->getCode()),ring_code->getSize());
    for (const auto& s : ring_sites) {
        s.site[0]=0xe9;
        std::memcpy(s.site+1,&s.displacement,sizeof(s.displacement));
        std::memset(s.site+5,0x90,PlayAnimWrite.size()-5);
        Hook::FlushCode(s.site,PlayAnimWrite.size());
    }
    LOG_INFO(Core,"VR summon bell: the game's ringing after a physical ring is skipped ({} "
                  "ForceAnimationPlayback sites)",ring_sites.size());
}
void PS4_SYSV_ABI Check(void* manager, u8* condition) {
    const int action=Hook::Read<int>(condition+0x28);
    const int character=Hook::Read<int>(condition+0x2c);
    const bool accepted=prompt(manager,16,action,0,0,0,0,0,0.f);
    bool pass=accepted;
    if (SummonBell::Supported(action,character)) {
        const double now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        std::scoped_lock lock{SummonBell::mutex};
        const bool allowed=SummonBell::enabled.load() && SummonBell::input_allowed &&
            now>=SummonBell::input_time && now-SummonBell::input_time<.25 &&
            VR::IsFirstPerson() && VR::IsImmersive();
        const bool was=SummonBell::state.pending;
        pass=SummonBell::state.Offer(reinterpret_cast<std::uintptr_t>(condition),action,character,
                                    accepted,allowed,now);
        if (!was && SummonBell::state.pending)
            LOG_INFO(Core,"VR summon bell: drawn action={} character={}",action,character);
        if (was && pass)
            LOG_INFO(Core,"VR summon bell: ringing accepted action={} character={}",action,character);
    }
    if (pass) condition[0x10] |= 1;
}
}
void InstallSummonBell(u64 base,u64 size) {
    if (code || !base || size<Native+17) return;
    auto* module=reinterpret_cast<u8*>(base);
    if (std::memcmp(module+Site,Signature.data(),Signature.size())!=0) {
        LOG_WARNING(Core,"VR summon bell: unsupported action-condition code; original interaction kept");
        return;
    }
    try {
        auto c=std::make_unique<Xbyak::CodeGenerator>(4096);
        prompt=reinterpret_cast<PromptFn>(base+Native);
        void* entry=Hook::EmitHostCallStubWith(*c,reinterpret_cast<void*>(&Check),nullptr,0,
            base+End,[](auto& a){ a.mov(a.rsi,a.r14); });
        c->ready();
        if (!Hook::MakeWritable(module+Site,Signature.size())) return;
        Hook::FlushCode(const_cast<u8*>(c->getCode()),c->getSize());
        Hook::WriteJump(module+Site,24,entry);
        code=std::move(c);
        SummonBell::installed.store(true);
        LOG_INFO(Core,"VR summon bell: verified 1.09 action-condition hook installed");
        InstallRingSkip(base,size);
    } catch (const Xbyak::Error& e) {
        LOG_ERROR(Core,"VR summon bell: install failed {}",e.what());
    }
}
}
