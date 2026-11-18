// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Overlapping moon / sun shadow cascades (2026-11-11: "Let's do it"; the idea from the
// BBFSS / Lantern Engine notes, our own code). The game sets up its four directional cascades in
// one function (1.09: CascadeRange); each cascade gets a camera slice from its near split to its
// far one, and that slice decides both what casts into the cascade and how its map is fitted. Our
// moon shadows (shader_recompiler/ir/passes/moon_shadow.h) blend into the next cascade over the
// last CascadeBlendShare of a cascade's depth, but the next cascade's map only holds casters from
// its own near split on: something standing in that band cast no shadow there in the next map.
// Here, right before the game stores cascade c's near distance (c = 1..3), it is moved back by
// MoonShadow::CascadeBlendShare of cascade c-1's depth (never nearer than the camera's own near):
// both cascades then hold the band, and the blend sees the same shadows on both sides. Which
// cascade a pixel uses (the resolve's split distances) stays the game's. A guest-code stub, no
// host call: the cascade index in r8d, the shadow context in rdi (its near splits at +0x6dc, a
// float each from cascade 1), the game's camera in rdx (near at +0x54), the near in xmm0.
// Startup only, with the moon shadows (vr.ini moon_shadows); VR_SHADOW_OVERLAP=0|1 overrides.

#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/vr/bloodborne_shadow_cascades.h"
#include "core/vr/hook_util.h"
#include "core/vr/vr_session.h"
#include "shader_recompiler/ir/passes/moon_shadow.h"

namespace VR {

namespace {

// 1.09 image offsets: the cascade range function's entry, and in it the stores of cascade c's
// near and far distances: vmovss [rsi+0x54], xmm0; vminss xmm0, xmm1, [rdx+0x58];
// vmovss [rsi+0x58], xmm0 (15 bytes, replayed after ours).
constexpr u64 CascadeRange = 0xeac540;
constexpr std::array<u8, 18> CascadeRangeEntry{0xc5, 0xf8, 0x28, 0x02, 0xc5, 0xf8, 0x29, 0x06, 0xc5,
                                               0xf8, 0x28, 0x42, 0x10, 0xc5, 0xf8, 0x29, 0x46, 0x10};
constexpr u64 NearStores = 0xeac657;
constexpr std::array<u8, 15> NearStoresBytes{0xc5, 0xfa, 0x11, 0x46, 0x54, 0xc5, 0xf2, 0x5d,
                                             0x42, 0x58, 0xc5, 0xfa, 0x11, 0x46, 0x58};
constexpr u32 SplitsOffset = 0x6dc; // the context's near splits, cascade 1 first
constexpr u32 CameraNearOffset = 0x54;

std::unique_ptr<Xbyak::CodeGenerator> g_code;

bool Wanted() {
    if (const char* env = std::getenv("VR_SHADOW_OVERLAP")) {
        return env[0] != '0';
    }
    return VR::IsEnabled() && VR::GetSettings().moon_shadows;
}

} // namespace

void InstallShadowCascadeOverlap(u64 base, u64 size) {
    if (g_code || base == 0 || size < NearStores + NearStoresBytes.size() || !Wanted()) {
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    if (std::memcmp(module + CascadeRange, CascadeRangeEntry.data(), CascadeRangeEntry.size()) != 0 ||
        std::memcmp(module + NearStores, NearStoresBytes.data(), NearStoresBytes.size()) != 0) {
        LOG_WARNING(Core, "VR shadow cascades: unsupported cascade code (not 1.09); the game's "
                          "cascades kept");
        return;
    }
    const float share = Shader::Optimization::MoonShadow::CascadeBlendShare;
    try {
        auto c = std::make_unique<Xbyak::CodeGenerator>(4096);
        using namespace Xbyak::util;
        Xbyak::Label done, have_previous, share_constant;
        c->lea(rsp, ptr[rsp - 128]); // the guest red zone
        c->pushf();
        c->sub(rsp, 32);
        c->vmovups(ptr[rsp], xmm2);
        c->vmovups(ptr[rsp + 16], xmm3);
        // Cascades 1..3 only, with both structures there.
        c->cmp(r8d, 1);
        c->jb(done, Xbyak::CodeGenerator::T_NEAR);
        c->cmp(r8d, 3);
        c->ja(done, Xbyak::CodeGenerator::T_NEAR);
        c->test(rdi, rdi);
        c->jz(done, Xbyak::CodeGenerator::T_NEAR);
        c->test(rdx, rdx);
        c->jz(done, Xbyak::CodeGenerator::T_NEAR);
        // The previous cascade's near (cascade 0's is 0).
        c->vxorps(xmm2, xmm2, xmm2);
        c->cmp(r8d, 1);
        c->je(have_previous);
        c->vmovss(xmm2, ptr[rdi + r8 * 4 + (SplitsOffset - 8)]);
        c->L(have_previous);
        // near > previous (an unordered compare, a NaN on either side, skips too).
        c->vucomiss(xmm0, xmm2);
        c->jbe(done, Xbyak::CodeGenerator::T_NEAR);
        // The camera's near: a number, not beyond this near.
        c->vmovss(xmm3, ptr[rdx + CameraNearOffset]);
        c->vucomiss(xmm3, xmm3);
        c->jp(done, Xbyak::CodeGenerator::T_NEAR);
        c->vucomiss(xmm3, xmm0);
        c->ja(done, Xbyak::CodeGenerator::T_NEAR);
        // near - share (near - previous), at least the camera's near.
        c->vsubss(xmm2, xmm0, xmm2);
        c->vmulss(xmm2, xmm2, ptr[rip + share_constant]);
        c->vsubss(xmm2, xmm0, xmm2);
        c->vmaxss(xmm0, xmm2, xmm3);
        c->L(done);
        c->vmovups(xmm2, ptr[rsp]);
        c->vmovups(xmm3, ptr[rsp + 16]);
        c->add(rsp, 32);
        c->popf();
        c->lea(rsp, ptr[rsp + 128]);
        // The game's stores, then back after them.
        for (const u8 b : NearStoresBytes) {
            c->db(b);
        }
        c->db(0xff); // jmp qword [rip+0]
        c->db(0x25);
        c->dd(0);
        c->dq(base + NearStores + NearStoresBytes.size());
        c->L(share_constant);
        c->dd(std::bit_cast<u32>(share));
        c->ready();
        if (!Hook::MakeWritable(module + NearStores, NearStoresBytes.size())) {
            LOG_WARNING(Core, "VR shadow cascades: the cascade code is not writable; kept");
            return;
        }
        Hook::FlushCode(const_cast<u8*>(c->getCode()), c->getSize());
        Hook::WriteJump(module + NearStores, NearStoresBytes.size(),
                        const_cast<u8*>(c->getCode()));
        g_code = std::move(c);
        LOG_INFO(Core, "VR shadow cascades: cascades 1..3 start {:.0f}% of the previous cascade's "
                       "depth earlier (verified 1.09 code): the seamless blend has both maps' "
                       "casters",
                 share * 100.0f);
    } catch (const Xbyak::Error& e) {
        LOG_ERROR(Core, "VR shadow cascades: install failed {}", e.what());
    }
}

} // namespace VR
