// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/vr/bloodborne_lights.h"
#include "core/vr/hook_util.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

// The two light-set builders (found 2026-10-20 by watching the CPU copy of the light-set table:
// eboot 0x22E6FE0 builds the table, one set per call of the first). Each takes the camera's world
// matrix (rows right, up, forward, position; [render context + 0x3F8] at every call site) and
// only uses it to turn a flagged light's direction:
//   0x22DE8B0 (rdi the set, rsi its source, rdx the matrix): lights 1-3, flags at source
//             +0x20C..+0x20E; called twice from 0x22E6FE0, nowhere else.
//   0x22DEEB0 (rcx the matrix): the other set kind, flag at source +0x24C; called once (0x2252018).
// The hooks replace the matrix argument before the prologue copies it (mov r15, rdx / rcx).
struct Builder {
    const char* name;
    u64 rva;
    size_t length; // the replayed prologue: whole instructions, >= 14
    std::array<u8, 18> prologue;
    bool matrix_in_rcx; // else rdx
};
constexpr std::array<Builder, 2> Builders{{
    // push rbp; mov rbp, rsp; push r15; push r14; push rbx; sub rsp, 0x28; mov r15, rdx
    {"set", 0x22DE8B0, 16,
     {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x49, 0x89,
      0xD7},
     false},
    // push rbp; mov rbp, rsp; push r15; push r14; push r12; push rbx; sub rsp, 0x170
    {"set2", 0x22DEEB0, 18,
     {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC, 0x70,
      0x01, 0x00, 0x00},
     true},
}};

std::unique_ptr<Xbyak::CodeGenerator> g_code;
bool g_installed = false;
std::atomic<u64> g_turned{0};
// VR_LIGHT_YAW_SIGN=-1: the head's yaw turned back the other way (a check of the convention).
float g_yaw_sign = 1.0f;

// The matrix a builder gets: the camera's, levelled and turned back by the head's yaw (the
// newest camera update's; the builders run while the game prepares that update's frame). The
// position row stays. One per guest thread, reused at every call.
alignas(16) thread_local float t_matrix[16];

const float* PS4_SYSV_ABI BodyLightMatrix(const float* m) {
    float yaw = 0.0f;
    if (!VR::GetSettings().body_fill_lights || !VR::NewestHeadYaw(yaw)) {
        return m; // menus, cinematics, the virtual screen, or off: the game's camera
    }
    const float* const right = m;
    const float* const up = m + 4;
    const float* const fwd = m + 8;
    float fx = fwd[0], fz = fwd[2];
    const float flat = std::sqrt(fx * fx + fz * fz);
    if (!(flat > 0.05f)) {
        return m; // looking straight up or down: no heading to level
    }
    fx /= flat;
    fz /= flat;
    // The levelled right: across the heading, on the camera's right's side.
    float rx = fz, rz = -fx;
    if (rx * right[0] + rz * right[2] < 0.0f) {
        rx = -rx;
        rz = -rz;
    }
    const float a = g_yaw_sign * yaw;
    const float c = std::cos(a), s = std::sin(a);
    const auto turn = [c, s](float& x, float& z) {
        const float nx = x * c + z * s;
        z = -x * s + z * c;
        x = nx;
    };
    turn(rx, rz);
    turn(fx, fz);
    float* const t = t_matrix;
    t[0] = rx, t[1] = 0.0f, t[2] = rz, t[3] = right[3];
    t[4] = 0.0f, t[5] = up[1] >= 0.0f ? 1.0f : -1.0f, t[6] = 0.0f, t[7] = up[3];
    t[8] = fx, t[9] = 0.0f, t[10] = fz, t[11] = fwd[3];
    std::memcpy(t + 12, m + 12, 4 * sizeof(float));
    g_turned.fetch_add(1, std::memory_order_relaxed);
    return t;
}

} // namespace

void InstallLights(VAddr base, u64 size) {
    if (g_installed) {
        return;
    }
    if (const char* sign = std::getenv("VR_LIGHT_YAW_SIGN"); sign != nullptr && sign[0] == '-') {
        g_yaw_sign = -1.0f;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    for (const Builder& b : Builders) {
        if (b.rva + b.length > size ||
            std::memcmp(module + b.rva, b.prologue.data(), b.length) != 0) {
            LOG_WARNING(Core,
                        "VR lights: the light-set builder '{}' at {:#x} differs from retail 1.09 "
                        "({}); camera-attached lights keep following the head",
                        b.name, b.rva,
                        b.rva + b.length > size ? std::string("out of range")
                                                : VR::Hook::HexBytes(module + b.rva, b.length));
            return;
        }
    }
    std::array<void*, Builders.size()> entries{};
    try {
        using namespace Xbyak::util;
        g_code = std::make_unique<Xbyak::CodeGenerator>(512);
        auto& c = *g_code;
        for (size_t i = 0; i < Builders.size(); ++i) {
            const Builder& b = Builders[i];
            c.align(16);
            entries[i] = const_cast<u8*>(c.getCurr());
            // At the builder's entry: rsp = 8 mod 16, nothing of the callee live yet but its
            // argument registers (pointers only: no vector argument to keep).
            c.lea(rsp, ptr[rsp - 128]);
            for (const auto& reg : {rdi, rsi, rdx, rcx, r8, r9, r10, r11, rax}) {
                c.push(reg); // 9 pushes: 16-byte aligned for the call
            }
            c.mov(rdi, b.matrix_in_rcx ? rcx : rdx);
            c.mov(rax, reinterpret_cast<u64>(&BodyLightMatrix));
            c.call(rax);
            c.mov(qword[rsp + (b.matrix_in_rcx ? 40 : 48)], rax); // the saved rcx / rdx
            for (const auto& reg : {rax, r11, r10, r9, r8, rcx, rdx, rsi, rdi}) {
                c.pop(reg);
            }
            c.lea(rsp, ptr[rsp + 128]);
            for (size_t k = 0; k < b.length; ++k) {
                c.db(b.prologue[k]);
            }
            c.db(0xFF); // jmp qword [rip+0]; dq back
            c.db(0x25);
            c.dd(0);
            c.dq(base + b.rva + b.length);
        }
        c.ready();
    } catch (const Xbyak::Error& e) {
        LOG_ERROR(Core, "VR lights: stub generation failed: {}", e.what());
        return;
    }
    for (size_t i = 0; i < Builders.size(); ++i) {
        u8* const site = module + Builders[i].rva;
        if (!VR::Hook::MakeWritable(site, Builders[i].length)) {
            LOG_ERROR(Core, "VR lights: cannot write the builder at {:#x}", Builders[i].rva);
            return;
        }
    }
    for (size_t i = 0; i < Builders.size(); ++i) {
        u8* const site = module + Builders[i].rva;
        VR::Hook::WriteJump(site, Builders[i].length, entries[i]);
        VR::Hook::FlushCode(site, Builders[i].length);
    }
    g_installed = true;
    LOG_INFO(Core,
             "VR lights: the light-set builders at {:#x} and {:#x} hooked; camera-attached fill "
             "lights follow {} (body_fill_lights={}, live; yaw sign {:+.0f})",
             Builders[0].rva, Builders[1].rva,
             VR::GetSettings().body_fill_lights ? "the hunter's heading" : "your head, as the game",
             VR::GetSettings().body_fill_lights ? 1 : 0, g_yaw_sign);
}

} // namespace VR::Bloodborne
