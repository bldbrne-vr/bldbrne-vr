// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/vr/bloodborne_enemy_bars.h"
#include "core/vr/hook_util.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

// The HUD tag's update (0x1B59E20: rdi the tag, rsi its data block, xmm0 dt; every caller is the
// HUD's update 0x1BF8819.., which keeps the HUD in r15: its 8 EnemyTag%d tags at +0x68..+0xA0, then
// the 7 RemotePCTag%d ones at +0xA8..+0xD8 and the 3 boss bars of BossTop/Fade/BossList/Item_%d_0
// at +0xE0..+0xF0: 18 direct calls 0x1BF8831..0x1BF8A4B, no other caller, no vtable entry; only
// the 8 EnemyTag pointers are hidden, so the boss bars pass untouched). It ends with the tag's
// visibility:
//   1B5A379  49 8D 7E 58                lea rdi, [r14+0x58]          ; the tag's root
//   1B5A37D  41 83 BE 4C 06 00 00 00    cmp dword [r14+0x64C], 0     ; its HP
//   1B5A385  7E 0D                      jle 1B5A394
//   1B5A387  41 80 BE 38 06 00 00 00    cmp byte [r14+0x638], 0      ; shown
//   1B5A38F  0F 95 C0                   setne al
//   1B5A392  EB 02                      jmp 1B5A396
//   1B5A394  31 C0                      xor eax, eax
//   1B5A396  0F B6 F0                   movzx esi, al                ; <- the detour (15 bytes)
//   1B5A399  E8 72 24 F7 FF             call 1ACC810                 ; SetVisible(root, al)
//   1B5A39E  48 8B 05 3B 98 88 03       mov rax, [rip+0x388983B]     ; the stack guard's address
//   1B5A3A5  48 8B 00                   mov rax, [rax]               ; <- back here
// No branch lands inside the 15 patched bytes. The tag's prologue pushes the caller's r15 (the HUD)
// right after rbp: [rbp-8].
constexpr u64 SiteRva = 0x1B5A396;
constexpr size_t SiteLen = 15;
constexpr u64 CheckRva = 0x1B5A379;
constexpr std::array<u8, 47> CheckSig = {
    0x49, 0x8D, 0x7E, 0x58, 0x41, 0x83, 0xBE, 0x4C, 0x06, 0x00, 0x00, 0x00, 0x7E, 0x0D, 0x41, 0x80,
    0xBE, 0x38, 0x06, 0x00, 0x00, 0x00, 0x0F, 0x95, 0xC0, 0xEB, 0x02, 0x31, 0xC0, 0x0F, 0xB6, 0xF0,
    0xE8, 0x72, 0x24, 0xF7, 0xFF, 0x48, 0x8B, 0x05, 0x3B, 0x98, 0x88, 0x03, 0x48, 0x8B, 0x00,
};
constexpr u64 TagUpdateRva = 0x1B59E20;
// push rbp; mov rbp, rsp; push r15; push r14: the caller's r15 at [rbp-8].
constexpr std::array<u8, 8> TagPrologueSig = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56};
// The HUD's first EnemyTag update: mov rdi, [r15+0x68]; test rdi, rdi; je; lea rsi, [r14+0x668];
// vmovss xmm0, [rbp-0x6C8]; call 0x1B59E20 (the other 7 enemy tags' updates follow, +0x70..+0xA0).
constexpr u64 HudCallRva = 0x1BF8819;
constexpr std::array<u8, 29> HudCallSig = {
    0x49, 0x8B, 0x7F, 0x68, 0x48, 0x85, 0xFF, 0x74, 0x14, 0x49, 0x8D, 0xB6, 0x68, 0x06, 0x00,
    0x00, 0xC5, 0xFA, 0x10, 0x85, 0x38, 0xF9, 0xFF, 0xFF, 0xE8, 0xEA, 0x15, 0xF6, 0xFF,
};
constexpr u32 HudEnemyTags = 0x68; // 8 tag pointers
constexpr int EnemyTagCount = 8;
// The 3 boss bars (BossTop/Fade/BossList/Item_%d_0), updated by the same tag update.
constexpr u32 HudBossTags = 0xE0;
constexpr int BossTagCount = 3;
constexpr u64 SetVisibleRva = 0x1ACC810;
constexpr u64 GuardSlotRva = 0x53E3BE0; // what 1B5A39E loads
constexpr u64 BackRva = 0x1B5A3A5;

std::unique_ptr<Xbyak::CodeGenerator> g_code;
bool g_installed = false;
// Enemy tag updates whose bar the game showed and the stub hid (the stub's plain increments:
// counts for the log, a lost increment does not matter).
alignas(8) u64 g_hidden = 0;
// VR_ENEMY_BARS_LOG: every enemy tag update, and those the game shows.
alignas(8) u64 g_calls = 0;
alignas(8) u64 g_shown = 0;
// The HUD the stub last saw (the caller's r15): its 8 EnemyTags, for the scale hook.
alignas(8) u64 g_hud = 0;
bool g_diag = false;
u64 g_logged_hidden = 0;
bool g_hiding_logged = false;
std::chrono::steady_clock::time_point g_last_hide{};

u64 Load(u64& counter) {
    return std::atomic_ref<u64>(counter).load(std::memory_order_relaxed);
}

// ---- The boss bar near the top (ui_boss_hp_bar = 1, OnBossTag) ----
// The flat game draws the boss bars at the stage's bottom (y ~940 of 1080). Shown, each boss tag's
// clip is set BossRaisePx higher, from the y it had when first seen (the game lays the list out
// once; set from that original every update, so it never drifts). Through the clip's Scaleform
// value as OnSetPosition below: its object interface's GetDisplayInfo (+0xE0) and SetDisplayInfo
// (+0xE8), Scaleform 4's DisplayInfo (X +0x00, Y +0x08, XScale +0x18, VarsSet +0xD4, V_y = 0x02).
// The read is checked first (a finite y on the stage and a scale of a real clip); anything else
// leaves the bar where the game puts it, logged once.
constexpr double BossRaisePx = 780.0;
struct BossClip {
    u64 tag{0};
    double y{0.0};
};
std::array<BossClip, BossTagCount> g_boss_clips{};
std::atomic<u64> g_boss_moved{0};
bool g_boss_refused_logged{false};
bool g_boss_moved_logged{false};
u64 g_boss_hidden_logged{0};
alignas(8) u64 g_boss_hidden = 0;

// ---- The enemy tags' scale (SetEnemyTagScale) ----
// The game places a tag with 0x1ACB8C0 (element, x, y: HUD stage px), which takes the element's
// Scaleform value and sets its position through the value's object interface (GetDisplayInfo /
// SetDisplayInfo, Scaleform 4's DisplayInfo: XScale at +0x18, YScale at +0x20, VarsSet at +0xD4,
// V_xscale | V_yscale = 0x18). A stub at its entry runs OnSetPosition first: for the HUD's
// EnemyTags only, the same value gets the scale for where it goes. The site is verified by a hash of
// its bytes (none are kept here); the prologue it replays is copied from the running game.
constexpr u64 PositionFnRva = 0x1ACB8C0;
constexpr u32 PositionPrologue = 16;
constexpr u64 PositionPrologueHash = 0x592354d3585e3ac0ull; // FNV-1a 64

std::unique_ptr<Xbyak::CodeGenerator> g_scale_code;
std::array<u8, PositionPrologue> g_position_prologue{};
std::atomic<bool> g_scale_on{false};
std::atomic<float> g_scale_half_w{0.0f};
std::atomic<float> g_scale_half_h{0.0f};
std::atomic<u64> g_scaled{0};

u64 Fnv1a(const u8* data, size_t size) {
    u64 h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; ++i) {
        h = (h ^ data[i]) * 0x100000001B3ull;
    }
    return h;
}

using GetValueFn = void(PS4_SYSV_ABI*)(u8* out, u8* self);
using DisplayInfoFn = void(PS4_SYSV_ABI*)(u8* iface, u64 data, u8* info);
using ReleaseFn = void(PS4_SYSV_ABI*)(u8* iface, u8* value, u64 data);

void PS4_SYSV_ABI OnSetPosition(u8* element, u64 x_arg, u64 y_arg) {
    if (!g_scale_on.load(std::memory_order_relaxed) || element == nullptr) {
        return;
    }
    const u64 hud = Load(g_hud);
    if (hud == 0) {
        return;
    }
    bool enemy = false;
    for (int i = 0; i < EnemyTagCount && !enemy; ++i) {
        enemy = Hook::Read<u64>(reinterpret_cast<const u8*>(hud + HudEnemyTags + 8 * i)) ==
                reinterpret_cast<u64>(element);
    }
    if (!enemy) {
        return;
    }
    // Stage px -> the flat picture's tangents; its squeeze there, undone (the curved panel keeps
    // the flat picture's directions: across 1 / (1 + tx^2), and cos(elevation) on the cylinder).
    const float half_w = g_scale_half_w.load(std::memory_order_relaxed);
    const float half_h = g_scale_half_h.load(std::memory_order_relaxed);
    const float tx = (static_cast<float>(static_cast<s32>(x_arg)) / 960.0f - 1.0f) * half_w;
    const float ty = (1.0f - static_cast<float>(static_cast<s32>(y_arg)) / 540.0f) * half_h;
    const float a = 1.0f + tx * tx;
    const float b = 1.0f + ty * ty / a;
    const double sx = std::clamp(a * std::sqrt(b), 1.0f, 4.0f);
    const double sy = std::clamp(std::sqrt(a) * b, 1.0f, 4.0f);
    // As 0x1ACB8C0: the element's value (its vtable's first entry), then its object interface.
    alignas(16) u8 value[0x40]{};
    const auto get = Hook::Read<GetValueFn>(Hook::Read<u8*>(element));
    get(value, element);
    if ((value[0x20] & 0x8F) == 0) {
        return;
    }
    u8* const iface = Hook::Read<u8*>(value + 0x18);
    const u64 data = Hook::Read<u64>(value + 0x28);
    alignas(16) u8 info[0xE0]{};
    const double xscale = 100.0 * sx;
    const double yscale = 100.0 * sy;
    const u16 vars = 0x18;
    std::memcpy(info + 0x18, &xscale, sizeof(xscale));
    std::memcpy(info + 0x20, &yscale, sizeof(yscale));
    std::memcpy(info + 0xD4, &vars, sizeof(vars));
    const auto set = Hook::Read<DisplayInfoFn>(Hook::Read<u8*>(iface) + 0xE8);
    set(iface, data, info);
    if ((value[0x20] & 0x40) != 0) {
        const auto release = Hook::Read<ReleaseFn>(Hook::Read<u8*>(iface) + 0x18);
        release(iface, value + 8, data);
    }
    g_scaled.fetch_add(1, std::memory_order_relaxed);
}

// The tag update's stub, for a boss tag the game shows while ui_boss_hp_bar is 1 (rdi: the tag).
void PS4_SYSV_ABI OnBossTag(u8* tag) {
    if (tag == nullptr) {
        return;
    }
    alignas(16) u8 value[0x40]{};
    const auto get = Hook::Read<GetValueFn>(Hook::Read<u8*>(tag));
    get(value, tag);
    if ((value[0x20] & 0x8F) == 0) {
        return;
    }
    u8* const iface = Hook::Read<u8*>(value + 0x18);
    const u64 data = Hook::Read<u64>(value + 0x28);
    const auto release = [&] {
        if ((value[0x20] & 0x40) != 0) {
            const auto rel = Hook::Read<ReleaseFn>(Hook::Read<u8*>(iface) + 0x18);
            rel(iface, value + 8, data);
        }
    };
    BossClip* clip = nullptr;
    for (auto& c : g_boss_clips) {
        if (c.tag == reinterpret_cast<u64>(tag)) {
            clip = &c;
            break;
        }
    }
    if (clip == nullptr) {
        alignas(16) u8 info[0xE0]{};
        const auto read = Hook::Read<DisplayInfoFn>(Hook::Read<u8*>(iface) + 0xE0);
        read(iface, data, info);
        double y{}, xscale{};
        std::memcpy(&y, info + 0x08, sizeof(y));
        std::memcpy(&xscale, info + 0x18, sizeof(xscale));
        const bool plausible = std::isfinite(y) && std::abs(y) < 4000.0 &&
                               std::isfinite(xscale) && xscale > 1.0 && xscale < 1000.0;
        if (!plausible) {
            if (!g_boss_refused_logged) {
                g_boss_refused_logged = true;
                LOG_WARNING(Core, "VR ui: boss bar: its clip's position read as y {} scale {}: not "
                                  "a clip's; the bar stays where the game puts it",
                            y, xscale);
            }
            release();
            return;
        }
        for (auto& c : g_boss_clips) {
            if (c.tag == 0) {
                c = {reinterpret_cast<u64>(tag), y};
                clip = &c;
                break;
            }
        }
        if (clip == nullptr) {
            release();
            return;
        }
    }
    alignas(16) u8 info[0xE0]{};
    const double y = clip->y - BossRaisePx;
    const u16 vars = 0x02; // V_y
    std::memcpy(info + 0x08, &y, sizeof(y));
    std::memcpy(info + 0xD4, &vars, sizeof(vars));
    const auto set = Hook::Read<DisplayInfoFn>(Hook::Read<u8*>(iface) + 0xE8);
    set(iface, data, info);
    release();
    if (g_boss_moved.fetch_add(1, std::memory_order_relaxed) == 0 && !g_boss_moved_logged) {
        g_boss_moved_logged = true;
        LOG_INFO(Core, "VR ui: boss bar shown near the top (its clip y {:.0f} -> {:.0f}, "
                       "ui_boss_hp_bar=1)",
                 clip->y, y);
    }
}

void InstallTagScale(u8* module, u64 size) {
    if (g_scale_code || size <= PositionFnRva + PositionPrologue) {
        return;
    }
    u8* const site = module + PositionFnRva;
    if (Fnv1a(site, PositionPrologue) != PositionPrologueHash) {
        LOG_WARNING(Core, "VR ui: enemy tag scale not installed (the tag position function at {:#x} "
                          "does not match): the tags keep the game's size",
                    PositionFnRva);
        return;
    }
    std::memcpy(g_position_prologue.data(), site, PositionPrologue);
    if (!Hook::MakeWritable(site, PositionPrologue)) {
        LOG_ERROR(Core, "VR ui: enemy tag scale: could not make {:#x} writable; not installed",
                  PositionFnRva);
        return;
    }
    try {
        g_scale_code = std::make_unique<Xbyak::CodeGenerator>(1024);
        void* const stub = Hook::EmitHostCallStubWith(
            *g_scale_code, reinterpret_cast<void*>(&OnSetPosition), g_position_prologue.data(),
            PositionPrologue, reinterpret_cast<u64>(site + PositionPrologue),
            [](Xbyak::CodeGenerator&) {}); // rdi element, esi x, edx y: already in place
        g_scale_code->ready();
        Hook::WriteJump(site, PositionPrologue, stub);
    } catch (const Xbyak::Error& error) {
        LOG_ERROR(Core, "VR ui: enemy tag scale: code emission failed ({}); not installed",
                  error.what());
        g_scale_code.reset();
        return;
    }
    LOG_INFO(Core, "VR ui: enemy tag scale installed at {:#x} (the tags keep their size and shape "
                   "across the curved rest panel)",
             PositionFnRva);
}

} // Anonymous namespace

void SetEnemyTagScale(bool on, float half_w, float half_h) {
    g_scale_half_w.store(half_w, std::memory_order_relaxed);
    g_scale_half_h.store(half_h, std::memory_order_relaxed);
    const bool was = g_scale_on.exchange(on, std::memory_order_relaxed);
    if (was != on) {
        LOG_INFO(Core, "VR ui: enemy tag scale {} (half width {:.3f}, half height {:.3f}; {} tag "
                       "placements scaled so far)",
                 on ? "on" : "off", half_w, half_h, g_scaled.load(std::memory_order_relaxed));
    }
}

void InstallEnemyBars(VAddr base, u64 size) {
    if (g_code) {
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    const auto matches = [&](u64 rva, const auto& sig) {
        return size > rva + sig.size() && std::memcmp(module + rva, sig.data(), sig.size()) == 0;
    };
    const bool site_ok = matches(CheckRva, CheckSig);
    const bool tag_ok = matches(TagUpdateRva, TagPrologueSig);
    const bool hud_ok = matches(HudCallRva, HudCallSig);
    if (!site_ok || !tag_ok || !hud_ok) {
        LOG_WARNING(Core,
                    "VR ui: enemy HP bars switch not installed (tag visibility {}, tag update {}, "
                    "the HUD's enemy tags {}; live bytes at {:#x}: {}): the bars show as the game "
                    "shows them",
                    site_ok ? "ok" : "MISMATCH", tag_ok ? "ok" : "MISMATCH",
                    hud_ok ? "ok" : "MISMATCH", CheckRva,
                    size > CheckRva + CheckSig.size()
                        ? Hook::HexBytes(module + CheckRva, CheckSig.size())
                        : std::string("out of range"));
        return;
    }
    u8* const site = module + SiteRva;
    g_diag = std::getenv("VR_ENEMY_BARS_LOG") != nullptr;
    // The live setting, read by the stub at every tag update (one byte of the settings that live
    // as long as the process; the VR page writes it on another thread).
    const bool* const shown = &VR::GetSettings().ui_enemy_hp_bars;
    const bool* const boss_shown = &VR::GetSettings().ui_boss_hp_bar;
    static_assert(sizeof(bool) == 1);
    void* entry = nullptr;
    try {
        using namespace Xbyak::util;
        g_code = std::make_unique<Xbyak::CodeGenerator>(512);
        auto& c = *g_code;
        Xbyak::Label keep, enemy, boss, boss_on, set_visible, guard_slot, back;
        entry = const_cast<u8*>(c.getCurr());
        // al: the game's visibility for this tag, rdi: its root, r14: the tag. rcx is free here
        // (the game's call below clobbers it anyway).
        c.mov(rcx, qword[rbp - 8]); // the caller's r15: the HUD
        c.push(rax); // al: the game's visibility
        c.mov(rax, reinterpret_cast<u64>(&g_hud));
        c.mov(qword[rax], rcx); // for the tags' scale (OnSetPosition)
        c.pop(rax);
        for (int i = 0; i < EnemyTagCount; ++i) {
            c.cmp(r14, qword[rcx + HudEnemyTags + 8 * i]);
            c.je(enemy, Xbyak::CodeGenerator::T_NEAR);
        }
        for (int i = 0; i < BossTagCount; ++i) {
            c.cmp(r14, qword[rcx + HudBossTags + 8 * i]);
            c.je(boss, Xbyak::CodeGenerator::T_NEAR);
        }
        // A RemotePCTag: as the game shows it.
        c.jmp(keep, Xbyak::CodeGenerator::T_NEAR);
        // A boss bar: hidden (ui_boss_hp_bar 0), else shown as the game shows it, moved up
        // (OnBossTag). The call keeps every register the code after the stub uses (al the
        // visibility, rdi the root, r14 the tag); 8 pushes keep the game's 16-byte alignment here
        // (a call follows in its own code).
        c.L(boss);
        c.mov(rcx, reinterpret_cast<u64>(boss_shown));
        c.cmp(byte[rcx], 0);
        c.jne(boss_on, Xbyak::CodeGenerator::T_NEAR);
        c.test(al, al);
        c.jz(keep, Xbyak::CodeGenerator::T_NEAR);
        c.xor_(eax, eax);
        c.mov(rcx, reinterpret_cast<u64>(&g_boss_hidden));
        c.inc(qword[rcx]);
        c.jmp(keep, Xbyak::CodeGenerator::T_NEAR);
        c.L(boss_on);
        c.test(al, al);
        c.jz(keep, Xbyak::CodeGenerator::T_NEAR);
        c.push(rax);
        c.push(rdi);
        c.push(rsi);
        c.push(rdx);
        c.push(r8);
        c.push(r9);
        c.push(r10);
        c.push(r11);
        c.mov(rdi, r14);
        c.mov(rax, reinterpret_cast<u64>(&OnBossTag));
        c.call(rax);
        c.pop(r11);
        c.pop(r10);
        c.pop(r9);
        c.pop(r8);
        c.pop(rdx);
        c.pop(rsi);
        c.pop(rdi);
        c.pop(rax);
        c.jmp(keep, Xbyak::CodeGenerator::T_NEAR);
        c.L(enemy);
        if (g_diag) {
            Xbyak::Label not_shown;
            c.mov(rcx, reinterpret_cast<u64>(&g_calls));
            c.inc(qword[rcx]);
            c.test(al, al);
            c.jz(not_shown);
            c.mov(rcx, reinterpret_cast<u64>(&g_shown));
            c.inc(qword[rcx]);
            c.L(not_shown);
        }
        c.mov(rcx, reinterpret_cast<u64>(shown));
        c.cmp(byte[rcx], 0);
        c.jne(keep); // on: the game's own decision
        c.test(al, al);
        c.jz(keep); // not shown anyway
        c.xor_(eax, eax); // off: hidden as a tag without an enemy
        c.mov(rcx, reinterpret_cast<u64>(&g_hidden));
        c.inc(qword[rcx]);
        c.L(keep);
        // The replayed instructions: the call and the rip-relative load through absolute addresses.
        c.movzx(esi, al);
        c.call(ptr[rip + set_visible]);
        c.mov(rax, ptr[rip + guard_slot]);
        c.mov(rax, qword[rax]);
        c.jmp(ptr[rip + back]);
        c.align(8);
        c.L(set_visible);
        c.dq(reinterpret_cast<u64>(module + SetVisibleRva));
        c.L(guard_slot);
        c.dq(reinterpret_cast<u64>(module + GuardSlotRva));
        c.L(back);
        c.dq(reinterpret_cast<u64>(module + BackRva));
        c.ready();
    } catch (const Xbyak::Error& error) {
        LOG_ERROR(Core, "VR ui: enemy HP bars switch: code emission failed ({}); not installed",
                  error.what());
        g_code.reset();
        return;
    }
    if (!Hook::MakeWritable(site, SiteLen)) {
        LOG_ERROR(Core, "VR ui: enemy HP bars switch: could not make {:#x} writable; not installed",
                  SiteRva);
        return;
    }
    Hook::WriteJump(site, SiteLen, entry);
    g_installed = true;
    InstallTagScale(module, size);
    LOG_INFO(Core,
             "VR ui: enemy HP bars switch installed at {:#x} (the HUD's EnemyTag visibility, sig "
             "ok): ui_enemy_hp_bars={} now ({}), live from the VR page{}",
             SiteRva, *shown ? 1 : 0,
             *shown ? "the bars show as the game shows them"
                    : "every enemy's bar and damage number hidden",
             g_diag ? ", VR_ENEMY_BARS_LOG" : "");
}

void EnemyBarsUpdate() {
    if (!g_installed) {
        return;
    }
    const u64 hidden = Load(g_hidden);
    const auto now = std::chrono::steady_clock::now();
    if (g_diag) {
        static auto last = now;
        if (now - last >= std::chrono::seconds(1)) {
            last = now;
            LOG_INFO(Core, "VR ui: enemy HP bars (VR_ENEMY_BARS_LOG): enemy tag updates {}, shown "
                           "{}, hidden {}, ui_enemy_hp_bars={}",
                     Load(g_calls), Load(g_shown), hidden,
                     VR::GetSettings().ui_enemy_hp_bars ? 1 : 0);
        }
    }
    const u64 boss_hidden = Load(g_boss_hidden);
    if (boss_hidden != g_boss_hidden_logged) {
        if (g_boss_hidden_logged == 0) {
            LOG_INFO(Core, "VR ui: boss bar hidden (ui_boss_hp_bar=0)");
        }
        g_boss_hidden_logged = boss_hidden;
    }
    if (hidden != g_logged_hidden) {
        if (!g_hiding_logged) {
            LOG_INFO(Core, "VR ui: enemy HP bars: hiding the bars the game shows "
                           "(ui_enemy_hp_bars=0)");
            g_hiding_logged = true;
        }
        g_logged_hidden = hidden;
        g_last_hide = now;
    } else if (g_hiding_logged && now - g_last_hide > std::chrono::seconds(2)) {
        LOG_INFO(Core, "VR ui: enemy HP bars: none hidden for 2 s ({} tag updates hidden; "
                       "ui_enemy_hp_bars={})",
                 hidden, VR::GetSettings().ui_enemy_hp_bars ? 1 : 0);
        g_hiding_logged = false;
    }
}

} // namespace VR::Bloodborne
