// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "core/vr/summon_bell.h"
#include "core/vr/vr_session.h"
#include "core/vr/hook_util.h"
#include "common/logging/log.h"
#include <array>
#include <chrono>
#include <memory>

namespace VR::Bloodborne {
namespace {
constexpr u64 Site=0x144e7b8, End=0x144e7d9, Native=0x1cd91f0;
constexpr std::array<u8,33> Signature={0xbe,0x10,0,0,0,0xc5,0xf8,0x57,0xc0,0x31,0xc9,0x45,0x31,0xc0,0x45,0x31,0xc9,0x89,0xda,0xe8,0x20,0xaa,0x88,0,0x84,0xc0,0x74,0x05,0x41,0x80,0x4e,0x10,0x01};
using PromptFn=bool(PS4_SYSV_ABI *)(void*,int,int,int,int,int,int,int,float);
PromptFn prompt{};
std::unique_ptr<Xbyak::CodeGenerator> code;
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
    } catch (const Xbyak::Error& e) {
        LOG_ERROR(Core,"VR summon bell: install failed {}",e.what());
    }
}
}
