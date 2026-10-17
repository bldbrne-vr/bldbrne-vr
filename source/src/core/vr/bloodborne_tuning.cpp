// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/vr/bloodborne_melee.h"
#include "core/vr/bloodborne_ragdoll.h"
#include "core/vr/bloodborne_tuning.h"
#include "core/vr/bloodborne_visceral_pull.h"
#include "core/vr/visceral_pull.h"
#include "core/vr/body_channel.h"
#include "core/vr/hit_direction.h"
#include "core/vr/hook_util.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

using Hook::Plausible;
using Hook::Read;

// SpEffectMgr::GetMaxHpRateProduct(mgr rdi, bool sel sil) -> xmm0 (max HP = base x product).
constexpr u64 MaxHpRateRva = 0x17E46E0;
constexpr std::array<u8, 31> MaxHpRateSig = {
    0x55,                                           // push rbp
    0x48, 0x89, 0xE5,                               // mov rbp, rsp
    0x53,                                           // push rbx
    0x50,                                           // push rax
    0x48, 0x8B, 0x57, 0x08,                         // mov rdx, [rdi+8]
    0xC5, 0xFA, 0x10, 0x05, 0x86, 0x2C, 0x14, 0x03, // vmovss xmm0, [rip+0x3142C86] (1.0f)
    0xC5, 0xF8, 0x28, 0xE0,                         // vmovaps xmm4, xmm0  <- resume
    0x48, 0x85, 0xD2,                               // test rdx, rdx
    0x0F, 0x84, 0xB5, 0x00, 0x00, 0x00,             // jz
};
constexpr size_t MaxHpRatePatchLen = 18;  // through the vmovss
constexpr size_t MaxHpRateReplayLen = 10; // the position-independent part, replayed verbatim
constexpr u64 OneConstRva = 0x4927378;    // the f32 1.0 the replaced vmovss loads

// Chr damage function 0x1A2A770: the final float HP damage in xmm1 right before it is truncated
// into res+0x22C. The signature starts 20 bytes before the site.
constexpr u64 DamageFinalRva = 0x1A2BC9F;
constexpr u64 DamageFinalResumeRva = 0x1A2BCB1;
constexpr size_t DamageFinalLead = 20;
constexpr size_t DamageFinalLen = 18;
constexpr std::array<u8, 38> DamageFinalSig = {
    0xE8, 0x10, 0xFD, 0xB4, 0xFF,                   // call 0x157B9A0 (target factor)
    0x48, 0x89, 0xD9,                               // mov rcx, rbx
    0xC5, 0xFA, 0x10, 0x8D, 0xC0, 0xFC, 0xFF, 0xFF, // vmovss xmm1, [rbp-0x340]
    0xC5, 0xF2, 0x59, 0xC8,                         // vmulss xmm1, xmm1, xmm0
    0xC5, 0xFA, 0x2C, 0xC1,                         // vcvttss2si eax, xmm1  <- site
    0x89, 0x81, 0x2C, 0x02, 0x00, 0x00,             // mov [rcx+0x22C], eax
    0xC5, 0xFA, 0x10, 0x85, 0x30, 0xFD, 0xFF, 0xFF, // vmovss xmm0, [rbp-0x2D0]
};
// Proofs that [rbp-0x318] is the attacking ChrIns at the site.
constexpr u64 CalcDamageRva = 0x1A2A770;
constexpr std::array<u8, 42> CalcDamageSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x81, 0xEC, 0x28, 0x04, 0x00, 0x00, 0x44, 0x89, 0x85, 0xD0, 0xFC, 0xFF, 0xFF, 0x89,
    0x8D, 0xE0, 0xFC, 0xFF, 0xFF, 0x48, 0x89, 0xD1, 0x49, 0x89, 0xF5, 0x49, 0x89, 0xFE,
};
constexpr u64 AttackerStoreRva = 0x1A2AA92; // mov [rbp-0x318], r13
constexpr std::array<u8, 7> AttackerStoreSig = {0x4C, 0x89, 0xAD, 0xE8, 0xFC, 0xFF, 0xFF};
constexpr u64 AttackerUseRva = 0x1A2BB82; // mov rdx, [rbp-0x318]
constexpr std::array<u8, 7> AttackerUseSig = {0x48, 0x8B, 0x95, 0xE8, 0xFC, 0xFF, 0xFF};
constexpr u64 JumpInRva = 0x1A2BC63; // vmovss xmm1, [rbp-0x340]; jmp short -> the site
constexpr std::array<u8, 10> JumpInSig = {0xC5, 0xFA, 0x10, 0x8D, 0xC0, 0xFC, 0xFF, 0xFF, 0xEB, 0x32};
constexpr s32 AttackerSlot = -0x318;

// ChrIns::IsMainPlayer: its lea r14, [rip+disp] proves the WorldChrMan global.
constexpr u64 IsMainPlayerRva = 0x18C2D00;
constexpr std::array<u8, 24> IsMainPlayerSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xFB, 0x4C, 0x8D,
    0x35, 0x67, 0xBB, 0xC7, 0x03, 0x49, 0x8B, 0x06, 0x48, 0x85, 0xC0, 0x75,
};
constexpr size_t IsMainPlayerLeaDisp = 13;
constexpr size_t IsMainPlayerLeaEnd = 17;
constexpr u64 WorldChrManRva = 0x553E878;

constexpr u32 OffsetMainPlayer = 0x60;  // WorldChrMan -> main PlayerIns*
constexpr u32 OffsetSpEffects = 0x1C8;  // PlayerIns -> SpEffect manager*
constexpr u32 OffsetChrModules = 0x3B0; // PlayerIns -> modules, +0x20 = ChrDataModule
constexpr u32 OffsetDataModule = 0x20;
constexpr u32 OffsetPlayerGameData = 0x3C0;
constexpr u32 OffsetHp = 0xF8;
constexpr u32 OffsetMaxHp = 0xFC;
constexpr u32 OffsetBaseMaxHp = 0x100;
constexpr u32 OffsetTrackMaxHp = 0x104;
constexpr u32 OffsetPgdMaxHp = 0x18;
constexpr u32 OffsetPgdBaseMaxHp = 0x1C;
constexpr u32 OffsetPgdVitality = 0x40;

// Damage result (rcx at the site).
constexpr u32 ResHand = 0x44; // -1 right weapon, -2 left weapon
constexpr u32 ResAttackType = 0x34;
constexpr u32 ResBehaviorId = 0x38;
constexpr u32 ResAtkParam = 0x3C; // (+0x40 is the isPc word)
constexpr u32 ResBullet = 0x14C;
constexpr u32 ResWeapon = 0x15C;
constexpr u32 ResTarget = 0x1E8;
constexpr s32 HandLeft = -2;

using MaxHpRateFn = float(PS4_SYSV_ABI*)(void* manager, bool sel);

u8* g_base = nullptr;
std::unique_ptr<Xbyak::CodeGenerator> g_code;

// HP bonus (the stub reads g_hp_mult through its address).
float g_hp_mult = 1.0f;
std::atomic<u64> g_hp_calls{0};
MaxHpRateFn g_max_hp_rate = nullptr; // trampoline into the original
bool g_hp_installed = false;
bool g_hp_log = false;
bool g_hp_autotest = false;

// Off-hand damage.
float g_offhand_mult = 1.0f;
bool g_offhand_installed = false;
int g_offhand_log = 0;
bool g_offhand_dry = false;
std::atomic<u64> g_hits_player{0};
std::atomic<u64> g_boosted{0};
std::atomic<u64> g_skipped_hand{0};
std::atomic<u64> g_skipped_target{0};
std::atomic<u64> g_suppressed{0};
std::atomic<u8*> g_last_target{nullptr}; // the last character the main player hit (tests)
std::atomic<s64> g_last_target_ns{0};
// Damage calculations that hit the main player (the hit vignette: core/vr/bloodborne_heal.cpp).
std::atomic<u32> g_taken{0};
std::atomic<s64> g_taken_ns{0};
// The main player's bullets that hit another character with damage (the presenter's hit ticks):
// his left-hand gun's (hand -2 with a behaviour id) and his thrown items' (no behaviour id).
std::atomic<u32> g_gun_hits{0};
std::atomic<u32> g_thrown_hits{0};

using Clock = std::chrono::steady_clock;

struct HitCheck {
    u8* target{};
    s32 hp_before{};
    s32 damage{};
    u32 updates{};
};
std::mutex g_hit_mutex;
HitCheck g_hit_check{};
Clock::time_point g_rate_window{};
u32 g_rate_count = 0;

u8* MainPlayer() {
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    if (!Plausible(world)) {
        return nullptr;
    }
    u8* const player = Read<u8*>(world + OffsetMainPlayer);
    return Plausible(player) ? player : nullptr;
}

u8* DataModule(u8* chr) {
    if (!Plausible(chr)) {
        return nullptr;
    }
    u8* const modules = Read<u8*>(chr + OffsetChrModules);
    if (!Plausible(modules)) {
        return nullptr;
    }
    u8* const data = Read<u8*>(modules + OffsetDataModule);
    return Plausible(data) ? data : nullptr;
}

s32 ChrHp(u8* chr) {
    u8* const data = DataModule(chr);
    return data != nullptr ? Read<s32>(data + OffsetHp) : -1;
}

// Rate limit for the per-hit lines: 20 per second.
bool HitLineAllowed() {
    const auto now = Clock::now();
    if (now - g_rate_window >= std::chrono::seconds(1)) {
        g_rate_window = now;
        g_rate_count = 0;
    }
    if (g_rate_count >= 20) {
        g_suppressed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ++g_rate_count;
    return true;
}

// Directional hit feedback (core/vr/hit_direction.h): where a hit on the hunter came from. The
// damage result's hit position (+0x180, as the map hit's), the characters' physics positions
// (module bag +0x68, owner +0x08, position +0x1E0) and the view's heading of the latest
// first-person frame (the body anchor).
constexpr u32 ResHitPosition = 0x180;
constexpr u32 OffsetPhysicsModule = 0x68;
constexpr u32 OffsetModuleOwner = 0x08;
constexpr u32 OffsetPhysicsPosition = 0x1E0;
constexpr s64 HitDirViewMaxAgeNs = 500'000'000;
std::mutex g_dir_mutex; // one writer of HitDir::Channel at a time
u32 g_dir_serial = 0;
Clock::time_point g_dir_window{};
u32 g_dir_lines = 0;

bool ChrPosition(u8* chr, HitDir::V3& out) {
    if (!Plausible(chr)) {
        return false;
    }
    u8* const mods = Read<u8*>(chr + OffsetChrModules);
    u8* const physics = Plausible(mods) ? Read<u8*>(mods + OffsetPhysicsModule) : nullptr;
    if (!Plausible(physics) || Read<u8*>(physics + OffsetModuleOwner) != chr) {
        return false;
    }
    out = {Read<float>(physics + OffsetPhysicsPosition),
           Read<float>(physics + OffsetPhysicsPosition + 4),
           Read<float>(physics + OffsetPhysicsPosition + 8)};
    return HitDir::Finite(out);
}

void NoteHitDirection(u8* res, u8* player, u8* attacker) {
    HitDir::Sample s{};
    s.center_ok = ChrPosition(player, s.center);
    s.hit = {Read<float>(res + ResHitPosition), Read<float>(res + ResHitPosition + 4),
             Read<float>(res + ResHitPosition + 8)};
    s.hit_ok = true;
    s.attacker_ok = attacker != player && ChrPosition(attacker, s.attacker);
    const HitDir::Resolved r = HitDir::Resolve(s);
    const s64 now = VR::Body::SteadyNowNs();
    VR::Body::Anchor anchor{};
    const bool view_ok = VR::Body::LatestAnchor(anchor) && anchor.valid &&
                         now - anchor.steady_ns <= HitDirViewMaxAgeNs;
    HitDir::Event e{};
    e.steady_ns = now;
    e.source = static_cast<u8>(r.source);
    e.view_ok = view_ok && r.source != HitDir::Source::None;
    e.world_yaw = r.world_yaw;
    e.view_yaw = view_ok ? anchor.view_yaw : 0.0f;
    e.azimuth = e.view_ok ? HitDir::Azimuth(r.world_yaw, anchor.view_yaw) : 0.0f;
    e.distance = r.distance;
    std::scoped_lock lock{g_dir_mutex};
    e.serial = ++g_dir_serial;
    HitDir::Channel().Store(e);
    const auto clock = Clock::now();
    if (clock - g_dir_window >= std::chrono::seconds(1)) {
        g_dir_window = clock;
        g_dir_lines = 0;
    }
    if (++g_dir_lines > 10) {
        return;
    }
    // Both candidates (the hit position and the attacker) with their side, for the logs.
    const auto candidate = [&](bool ok, const HitDir::V3& p, HitDir::Source source, float min_h,
                               float max_h, float max_dy) -> std::string {
        if (!ok || !s.center_ok) {
            return "-";
        }
        const HitDir::Resolved c = HitDir::FromPoint(s.center, p, source, min_h, max_h, max_dy);
        const std::string at = fmt::format("({:.2f},{:.2f},{:.2f})", p.x, p.y, p.z);
        if (c.source == HitDir::Source::None) {
            return at + " not used";
        }
        return view_ok ? fmt::format("{} {:.2f} away, az {:+.0f} deg", at, c.distance,
                                     HitDir::Azimuth(c.world_yaw, anchor.view_yaw) / HitDir::Deg)
                       : fmt::format("{} {:.2f} away, yaw {:.0f} deg", at, c.distance,
                                     c.world_yaw / HitDir::Deg);
    };
    LOG_INFO(Core,
             "VR hit dir: #{} from {}{} | hit point {} | attacker {} | hunter ({:.2f},{:.2f},{:.2f}) "
             "view yaw {}",
             e.serial, HitDir::SourceName(r.source),
             e.view_ok ? fmt::format(" ({}, az {:+.0f} deg)", HitDir::SideName(e.azimuth),
                                     e.azimuth / HitDir::Deg)
                       : std::string{r.source != HitDir::Source::None ? " (no view)" : ""},
             candidate(true, s.hit, HitDir::Source::HitPoint, HitDir::HitPointMinOffset,
                       HitDir::HitPointMaxOffset, HitDir::HitPointMaxHeight),
             candidate(s.attacker_ok, s.attacker, HitDir::Source::Attacker,
                       HitDir::AttackerMinDistance, HitDir::AttackerMaxDistance, 1e9f),
             s.center.x, s.center.y, s.center.z,
             view_ok ? fmt::format("{:.0f} deg", anchor.view_yaw / HitDir::Deg)
                     : std::string{"unknown"});
}

void PS4_SYSV_ABI OffhandDamageHook(u8* res, u8* saved_ymm, u8* attacker) {
    u8* const player = MainPlayer();
    if (player == nullptr) {
        return;
    }
    float* const damage = reinterpret_cast<float*>(saved_ymm + 32); // xmm1, low lane
    u8* const target = Read<u8*>(res + ResTarget);
    if (target == player && std::isfinite(*damage) && *damage > 0.0f) {
        // A hit on the hunter (an enemy's attack, a bullet, an explosion, his own Molotov): the
        // HP drop the player update sees next is a hit (the hit vignette), not a drain. Where it
        // came from first (directional hit feedback), so it is there when the hit is counted.
        if (const auto& vs = VR::GetSettings();
            vs.hit_vignette_directional || vs.hit_haptics_directional) {
            NoteHitDirection(res, player, attacker);
        }
        g_taken_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             Clock::now().time_since_epoch())
                             .count(),
                         std::memory_order_relaxed);
        g_taken.fetch_add(1, std::memory_order_release);
    }
    if (attacker != player) {
        if (g_offhand_log >= 2 && target == player) {
            std::scoped_lock lock{g_hit_mutex};
            if (HitLineAllowed()) {
                LOG_INFO(Core, "VR dmg: TAKEN atk={} hand={} dmg {:.2f} (untouched)",
                         Read<s32>(res + ResAtkParam), Read<s32>(res + ResHand), *damage);
            }
        }
        return;
    }
    g_hits_player.fetch_add(1, std::memory_order_relaxed);
    if (target != player && std::isfinite(*damage) && *damage > 0.0f &&
        Read<s32>(res + ResBullet) != -1) {
        // A bullet of his hitting a character: his gun's (the left weapon's behaviour) or a
        // thrown item's (a knife, a Molotov, a pebble: hand -1, no behaviour id).
        const s32 bid = Read<s32>(res + ResBehaviorId);
        if (Read<s32>(res + ResHand) == HandLeft && bid != -1) {
            g_gun_hits.fetch_add(1, std::memory_order_release);
        } else if (bid == -1) {
            g_thrown_hits.fetch_add(1, std::memory_order_release);
        }
    }
    // The interactive visceral (core/vr/bloodborne_visceral_pull.h): a throw's first damage
    // teaches its rip time.
    if (target != player && std::isfinite(*damage) && *damage > 0.0f) {
        if (VisceralPullOnDamage(player, target, Read<s32>(res + ResBullet))) {
            *damage *= VR::Melee::Pull::MistimedDamageScale;
        }
    }
    g_last_target.store(target, std::memory_order_relaxed);
    g_last_target_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               Clock::now().time_since_epoch())
                               .count(),
                           std::memory_order_relaxed);
    // A physical swing's hit (core/vr/bloodborne_melee.h): scaled by the blade's speed there.
    const float raw = *damage;
    const bool swing = MeleeDamage(res, target, damage);
    // melee_kill_ragdoll (core/vr/bloodborne_ragdoll.h): a lethal right-hand weapon attack arms
    // its target's instant ragdoll.
    *damage = RagdollOnPlayerHit(res, player, *damage);
    const float before = *damage;
    const s32 hand = Read<s32>(res + ResHand);
    const bool left = hand == HandLeft;
    const bool other = target != player;
    const bool boost = left && other && std::isfinite(before) && before > 0.0f;
    float after = before;
    if (boost) {
        after = std::min(before * g_offhand_mult, 2.0e9f);
        if (!g_offhand_dry) {
            *damage = after;
        }
        g_boosted.fetch_add(1, std::memory_order_relaxed);
    } else if (!left) {
        g_skipped_hand.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_skipped_target.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_offhand_log == 0) {
        return;
    }
    std::scoped_lock lock{g_hit_mutex};
    const s32 target_hp = ChrHp(target);
    const s32 applied = static_cast<s32>(g_offhand_dry ? before : after);
    if (boost) {
        g_hit_check = {target, target_hp, applied, 0};
    }
    if (!HitLineAllowed()) {
        return;
    }
    LOG_INFO(Core,
             "VR dmg: HIT #{} type={} atk={} bid={} hand={} bullet={} wep={} tgt={} tgtHp={} "
             "dmg {:.2f}->{:.2f} int {}->{} x{:.3f} {}",
             g_hits_player.load(std::memory_order_relaxed), Read<s32>(res + ResAttackType),
             Read<s32>(res + ResAtkParam), Read<s32>(res + ResBehaviorId), hand,
             Read<s32>(res + ResBullet), Read<s32>(res + ResWeapon), fmt::ptr(target), target_hp,
             raw, after, static_cast<s32>(raw), static_cast<s32>(after),
             boost ? g_offhand_mult : (raw > 0.0f ? after / raw : 1.0f),
             boost   ? (g_offhand_dry ? "BOOST(dry)" : "BOOST")
             : swing ? "SWING"
             : !left ? "skip(hand)"
                     : "skip(target)");
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

bool InstallHpBonus(u8* module, u64 size, int pct, bool wcm_ok) {
    if (pct <= 0) {
        LOG_INFO(Core, "VR hp: player_hp_bonus_pct=0, vanilla max HP (not installed)");
        return false;
    }
    u8* const site = module + MaxHpRateRva;
    const bool sig_ok = size > OneConstRva + sizeof(u32) &&
                        std::memcmp(site, MaxHpRateSig.data(), MaxHpRateSig.size()) == 0;
    const bool one_ok = sig_ok && Read<u32>(module + OneConstRva) == 0x3F800000u;
    if (!sig_ok || !one_ok || !wcm_ok) {
        LOG_WARNING(Core, "VR hp: not installed (sig {}, one {}, WorldChrMan {}); live bytes: {}",
                    sig_ok ? "ok" : "MISMATCH", one_ok ? "ok" : "MISMATCH",
                    wcm_ok ? "ok" : "MISMATCH",
                    size > MaxHpRateRva + MaxHpRateSig.size()
                        ? Hook::HexBytes(site, MaxHpRateSig.size())
                        : std::string("out of range"));
        return false;
    }
    g_hp_mult = 1.0f + static_cast<float>(pct) / 100.0f;

    using namespace Xbyak::util;
    auto& c = *g_code;
    // Trampoline: the replaced prologue, the RIP-relative load re-emitted, back to the original.
    c.align(16);
    auto* const tramp = const_cast<u8*>(c.getCurr());
    Xbyak::Label one;
    for (size_t i = 0; i < MaxHpRateReplayLen; ++i) {
        c.db(MaxHpRateSig[i]);
    }
    c.vmovss(xmm0, ptr[rip + one]);
    EmitAbsoluteJump(c, reinterpret_cast<u64>(site + MaxHpRatePatchLen));
    c.L(one);
    c.dd(0x3F800000u);

    // Entry: call the original, then scale the product for the main player's manager only.
    c.align(16);
    auto* const entry = const_cast<u8*>(c.getCurr());
    Xbyak::Label done;
    c.push(rbx); // rsp is 16-byte aligned at the call below
    c.mov(rbx, rdi);
    c.mov(rax, reinterpret_cast<u64>(tramp));
    c.call(rax);
    c.mov(rax, reinterpret_cast<u64>(module + WorldChrManRva));
    c.mov(rax, ptr[rax]);
    c.test(rax, rax);
    c.jz(done);
    c.mov(rax, ptr[rax + OffsetMainPlayer]);
    c.test(rax, rax);
    c.jz(done);
    c.cmp(rbx, ptr[rax + OffsetSpEffects]);
    c.jne(done);
    c.mov(rax, reinterpret_cast<u64>(&g_hp_mult));
    c.vmulss(xmm0, xmm0, ptr[rax]);
    c.mov(rax, reinterpret_cast<u64>(&g_hp_calls));
    c.lock();
    c.inc(qword[rax]);
    c.L(done);
    c.pop(rbx);
    c.ret();
    c.ready();

    if (!Hook::MakeWritable(site, MaxHpRatePatchLen)) {
        LOG_ERROR(Core, "VR hp: could not make 0x17E46E0 writable; not installed");
        return false;
    }
    g_max_hp_rate = reinterpret_cast<MaxHpRateFn>(tramp);
    Hook::WriteJump(site, MaxHpRatePatchLen, entry);
    LOG_INFO(Core,
             "VR hp: installed at {:#x} pct={} mult={:.3f} (sig ok, one ok, WorldChrMan ok)",
             MaxHpRateRva, pct, g_hp_mult);
    return true;
}

bool InstallOffhandDamage(u8* module, u64 size, int pct, bool wcm_ok) {
    // Physical melee scales its swings' hits here too (core/vr/bloodborne_melee.h).
    const bool melee = MeleePhysicalWanted();
    // The kill ragdoll classifies every hit here too (core/vr/bloodborne_ragdoll.h).
    const bool ragdoll = RagdollWanted();
    if (pct <= 0 && !melee && !ragdoll) {
        LOG_INFO(Core, "VR dmg: offhand_damage_pct=0, vanilla damage (not installed)");
        return false;
    }
    pct = std::max(pct, 0);
    if (size <= WorldChrManRva + 8) {
        LOG_WARNING(Core, "VR dmg: eboot too small ({:#x}); off-hand bonus not installed", size);
        return false;
    }
    const auto matches = [module](u64 rva, const auto& signature) {
        return std::memcmp(module + rva, signature.data(), signature.size()) == 0;
    };
    const bool sig_ok = matches(DamageFinalRva - DamageFinalLead, DamageFinalSig);
    const bool prologue_ok = matches(CalcDamageRva, CalcDamageSig);
    const bool slot_ok = matches(AttackerStoreRva, AttackerStoreSig) &&
                         matches(AttackerUseRva, AttackerUseSig);
    const bool jump_ok = matches(JumpInRva, JumpInSig);
    const auto verdict = [](bool ok) { return ok ? "ok" : "MISMATCH"; };
    if (!sig_ok || !prologue_ok || !slot_ok || !jump_ok || !wcm_ok) {
        LOG_WARNING(Core,
                    "VR dmg: off-hand bonus not installed (sig {}, prologue {}, attacker slot {}, "
                    "jump-in {}, WorldChrMan {}); live bytes: {}",
                    verdict(sig_ok), verdict(prologue_ok), verdict(slot_ok), verdict(jump_ok),
                    verdict(wcm_ok),
                    Hook::HexBytes(module + DamageFinalRva - DamageFinalLead,
                                   DamageFinalSig.size()));
        return false;
    }
    g_offhand_mult = 1.0f + static_cast<float>(pct) / 100.0f;
    u8* const site = module + DamageFinalRva;

    using namespace Xbyak::util;
    auto& c = *g_code;
    void* const host = Hook::EmitHostCallStub(
        c, reinterpret_cast<void*>(&OffhandDamageHook), DamageFinalSig.data() + DamageFinalLead,
        DamageFinalLen, reinterpret_cast<u64>(module + DamageFinalResumeRva), rcx, &rax);
    // rax is dead at the site (the replayed vcvttss2si writes it): load the attacker into it
    // while rbp is still the guest frame.
    c.align(16);
    auto* const entry = const_cast<u8*>(c.getCurr());
    c.mov(rax, ptr[rbp + AttackerSlot]);
    c.jmp(host, Xbyak::CodeGenerator::T_NEAR);
    c.ready();

    if (!Hook::MakeWritable(site, DamageFinalLen)) {
        LOG_ERROR(Core, "VR dmg: could not make 0x1A2BC9F writable; not installed");
        return false;
    }
    Hook::WriteJump(site, DamageFinalLen, entry);
    LOG_INFO(Core,
             "VR dmg: offhand +{}% installed at {:#x} (sig ok, prologue ok, attacker slot ok, "
             "jump-in ok, WorldChrMan ok) mult={:.3f}{}{}{}",
             pct, DamageFinalRva, g_offhand_mult, melee ? ", physical melee's hits too" : "",
             ragdoll ? ", the kill ragdoll's blows too" : "", g_offhand_dry ? " DRYRUN" : "");
    return true;
}

// --- Logs (main player's update thread) ---

struct HpState {
    u8* player{};
    u32 first_updates{};
    s32 base{}, max{}, hp{}, track{};
    float mult{};
    s32 vitality{};
    Clock::time_point last_state{};
    // Autotest.
    int autotest_phase{}; // 0 wait for full HP, 1 waiting 15 s, 2 off, 3 on again, 4 done
    Clock::time_point autotest_at{};
    u32 autotest_lines{};
};
HpState g_hp{};
Clock::time_point g_last_stats{};

s32 ExpectedMaxHp(s32 base, float product) {
    const float scaled = product * g_hp_mult;
    const float value = static_cast<float>(base) * scaled;
    return std::max(1, static_cast<s32>(value));
}

void LogHp(u8* player, const char* tag) {
    u8* const data = DataModule(player);
    u8* const pgd = Read<u8*>(player + OffsetPlayerGameData);
    u8* const manager = Read<u8*>(player + OffsetSpEffects);
    if (data == nullptr || !Plausible(manager)) {
        return;
    }
    const s32 base = Read<s32>(data + OffsetBaseMaxHp);
    const s32 max = Read<s32>(data + OffsetMaxHp);
    const float p0 = g_max_hp_rate(manager, false);
    const float p1 = g_max_hp_rate(manager, true);
    const s32 expect = ExpectedMaxHp(base, p0);
    LOG_INFO(Core,
             "VR hp: {} player={} base={} max={} hp={} track={} P0={:.4f} P1={:.4f} mult={:.3f} "
             "expect={} {} pgdBase={} pgdMax={} vit={} boosted_calls={}",
             tag, fmt::ptr(player), base, max, Read<s32>(data + OffsetHp),
             Read<s32>(data + OffsetTrackMaxHp), p0, p1, g_hp_mult, expect,
             expect == max ? "ok" : "MISMATCH",
             Plausible(pgd) ? Read<s32>(pgd + OffsetPgdBaseMaxHp) : -1,
             Plausible(pgd) ? Read<s32>(pgd + OffsetPgdMaxHp) : -1,
             Plausible(pgd) ? Read<s32>(pgd + OffsetPgdVitality) : -1,
             g_hp_calls.load(std::memory_order_relaxed));
}

void HpUpdate(u8* player) {
    u8* const data = DataModule(player);
    if (data == nullptr) {
        return;
    }
    const auto now = Clock::now();
    if (player != g_hp.player) {
        g_hp = {};
        g_hp.player = player;
        g_hp.last_state = now;
    }
    const s32 base = Read<s32>(data + OffsetBaseMaxHp);
    const s32 max = Read<s32>(data + OffsetMaxHp);
    const s32 hp = Read<s32>(data + OffsetHp);
    const s32 track = Read<s32>(data + OffsetTrackMaxHp);
    u8* const pgd = Read<u8*>(player + OffsetPlayerGameData);
    const s32 vitality = Plausible(pgd) ? Read<s32>(pgd + OffsetPgdVitality) : -1;

    if (g_hp_log) {
        if (g_hp.first_updates < 5) {
            ++g_hp.first_updates;
            LogHp(player, g_hp.first_updates == 1 ? "PLAYER new" : "PLAYER");
        } else if (base != g_hp.base || max != g_hp.max || g_hp_mult != g_hp.mult ||
                   now - g_hp.last_state >= std::chrono::seconds(10)) {
            if (base != g_hp.base && vitality == g_hp.vitality) {
                LOG_WARNING(Core, "VR hp: WARN base {}->{} changed without a Vitality change",
                            g_hp.base, base);
            }
            LogHp(player, "STATE");
            g_hp.last_state = now;
        }
        if (hp < g_hp.hp && max > 0 && g_hp.first_updates >= 5) {
            const s32 lost = g_hp.hp - hp;
            LOG_INFO(Core, "VR hp: HIT hp {}->{} (-{} = {:.1f}% of max {})", g_hp.hp, hp, lost,
                     100.0f * static_cast<float>(lost) / static_cast<float>(max), max);
        }
    }

    if (g_hp_autotest) {
        switch (g_hp.autotest_phase) {
        case 0:
            if (max > 0 && hp == max) {
                g_hp.autotest_phase = 1;
                g_hp.autotest_at = now + std::chrono::seconds(15);
                LOG_INFO(Core, "VR hp: AUTOTEST full HP; turning the bonus off in 15 s");
            }
            break;
        case 1:
        case 2:
            if (now >= g_hp.autotest_at) {
                const bool off = g_hp.autotest_phase == 1;
                g_hp_mult = off ? 1.0f
                                : 1.0f + static_cast<float>(
                                             VR::GetSettings().player_hp_bonus_pct) /
                                             100.0f;
                g_hp.autotest_phase = off ? 2 : 3;
                g_hp.autotest_at = now + std::chrono::seconds(5);
                g_hp.autotest_lines = 0;
                LOG_INFO(Core, "VR hp: AUTOTEST bonus {} (mult {:.3f})", off ? "OFF" : "ON",
                         g_hp_mult);
            } else if (g_hp.autotest_phase == 2 && g_hp.autotest_lines < 5) {
                ++g_hp.autotest_lines;
                LogHp(player, "AUTOTEST off");
            }
            break;
        case 3:
            if (g_hp.autotest_lines < 5) {
                ++g_hp.autotest_lines;
                LogHp(player, "AUTOTEST on");
            } else {
                g_hp.autotest_phase = 4;
                LOG_INFO(Core, "VR hp: AUTOTEST done");
            }
            break;
        default:
            break;
        }
    }

    g_hp.base = base;
    g_hp.max = max;
    g_hp.hp = hp;
    g_hp.track = track;
    g_hp.mult = g_hp_mult;
    g_hp.vitality = vitality;
}

void OffhandUpdate() {
    {
        std::scoped_lock lock{g_hit_mutex};
        if (g_hit_check.target != nullptr && ++g_hit_check.updates <= 5) {
            const s32 hp = ChrHp(g_hit_check.target);
            const s32 expected = std::max(0, g_hit_check.hp_before - g_hit_check.damage);
            if (hp == expected || g_hit_check.updates == 5) {
                LOG_INFO(Core, "VR dmg: VERIFY tgt={} hp {}->{} expected {} {}",
                         fmt::ptr(g_hit_check.target), g_hit_check.hp_before, hp, expected,
                         hp == expected ? "ok" : "MISMATCH (or other hits)");
                g_hit_check = {};
            }
        }
    }
    const auto now = Clock::now();
    if (now - g_last_stats >= std::chrono::seconds(10)) {
        g_last_stats = now;
        LOG_INFO(Core,
                 "VR dmg: STATS hits_player={} boosted={} skipped_hand={} skipped_target={} "
                 "suppressed={}",
                 g_hits_player.load(std::memory_order_relaxed),
                 g_boosted.load(std::memory_order_relaxed),
                 g_skipped_hand.load(std::memory_order_relaxed),
                 g_skipped_target.load(std::memory_order_relaxed),
                 g_suppressed.load(std::memory_order_relaxed));
    }
}

} // Anonymous namespace

void InstallTuning(VAddr base, u64 size) {
    if (g_code) {
        return;
    }
    const auto& settings = VR::GetSettings();
    auto* const module = reinterpret_cast<u8*>(base);
    const bool wcm_ok = size > WorldChrManRva + 8 && ProvesWorldChrMan(module);
    g_base = module;
    g_hp_log = std::getenv("VR_HP_LOG") != nullptr;
    g_hp_autotest = std::getenv("VR_HP_AUTOTEST") != nullptr;
    if (const char* level = std::getenv("VR_OFFHAND_LOG")) {
        g_offhand_log = std::clamp(std::atoi(level), 0, 2);
        if (g_offhand_log == 0) {
            g_offhand_log = 1;
        }
    }
    g_offhand_dry = std::getenv("VR_OFFHAND_DRYRUN") != nullptr;
    try {
        g_code = std::make_unique<Xbyak::CodeGenerator>(2048);
        g_hp_installed = InstallHpBonus(module, size, settings.player_hp_bonus_pct, wcm_ok);
        g_offhand_installed =
            InstallOffhandDamage(module, size, settings.offhand_damage_pct, wcm_ok);
    } catch (const Xbyak::Error& error) {
        LOG_ERROR(Core, "VR tuning: code emission failed ({}); nothing installed",
                  error.what());
    }
}

void TuningUpdate(u8* player) {
    if (player == nullptr) {
        return;
    }
    if (g_hp_installed && (g_hp_log || g_hp_autotest)) {
        HpUpdate(player);
    }
    if (g_offhand_installed && g_offhand_log > 0) {
        OffhandUpdate();
    }
}

bool IsHpBonusInstalled() {
    return g_hp_installed;
}

bool IsOffhandDamageInstalled() {
    return g_offhand_installed;
}

u8* LastPlayerHitTarget(s64& steady_ns) {
    steady_ns = g_last_target_ns.load(std::memory_order_relaxed);
    return g_last_target.load(std::memory_order_relaxed);
}

u32 PlayerDamageEvents(s64& last_steady_ns) {
    const u32 count = g_taken.load(std::memory_order_acquire);
    last_steady_ns = g_taken_ns.load(std::memory_order_relaxed);
    return count;
}

RangedHits PlayerRangedHits() {
    return {g_gun_hits.load(std::memory_order_acquire),
            g_thrown_hits.load(std::memory_order_acquire)};
}

} // namespace VR::Bloodborne
