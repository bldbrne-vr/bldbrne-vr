// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include "core/vr/hook_util.h"

namespace VR::Haptics::ObjectBreak::Native {

inline constexpr u64 SiteRva = 0x18B850C;
inline constexpr std::array<u8, 20> SiteSig = {
    0x49, 0x8B, 0x4E, 0x58,                         // mov rcx,[r14+0x58]
    0xC5, 0xFA, 0x10, 0x81, 0x98, 0x00, 0x00, 0x00, // vmovss xmm0,[rcx+0x98]
    0xC5, 0xFA, 0x10, 0x89, 0xA8, 0x00, 0x00, 0x00, // vmovss xmm1,[rcx+0xA8]
};

// At the verified native call site eax is the destruction result, r14 the attacker, rbx
// the hit result. The observer gets only those two pointers, on this call's stack lifetime.
// Both paths replay the same instructions and preserve the native return value and flags.
inline void* Emit(Xbyak::CodeGenerator& c, void* observer, u64 resume) {
    using namespace Xbyak::util;
    auto* const entry = const_cast<u8*>(c.getCurr());
    Xbyak::Label unchanged, observe;
    c.pushf();
    c.test(al, al);
    c.jz(unchanged);
    c.popf();
    c.jmp(observe);
    c.L(unchanged);
    c.popf();
    for (const u8 b : SiteSig) {
        c.db(b);
    }
    c.db(0xFF);
    c.db(0x25);
    c.dd(0);
    c.dq(resume);
    c.L(observe);
    Hook::EmitHostCallStubWith(c, observer, SiteSig.data(), SiteSig.size(), resume,
                              [](Xbyak::CodeGenerator& stub) {
                                  stub.mov(rdi, rbx);
                                  stub.mov(rsi, r14);
                              });
    return entry;
}

} // namespace VR::Haptics::ObjectBreak::Native
