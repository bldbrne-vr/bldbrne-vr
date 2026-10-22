// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "core/vr/body_channel.h"
#include "core/vr/bloodborne_layout.h"
#include "core/vr/bloodborne_tuning.h"
#include "core/vr/bloodborne_visceral_pull.h"
#include "core/vr/hook_util.h"
#include "core/vr/bloodborne_visceral_stage.h"
#include "core/vr/melee_settings.h"
#include "core/vr/visceral_channel.h"
#include "core/vr/visceral_pull.h"
#include "core/vr/visceral_camera.h"
#include "core/vr/anim_classes.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

namespace P = VR::Melee::Pull;
namespace Cb = VR::Combat;
using Hook::Plausible;
using Hook::Read;
using namespace Layout;

constexpr s64 Ms = 1'000'000;
constexpr u64 WorldChrManRva = 0x553E878;  // (the eboot must reach it: Bloodborne 1.09)
constexpr u64 ChrInsVtableRva = 0x538E2D0; // the characters a target may be (as the melee's IsChr)
constexpr u64 EnemyInsVtableRva = 0x538EBC0;
// The partner's speed holds only while the main player's updates keep refreshing it (a load or a
// quit mid-hold must never leave a reused address slowed).
constexpr s64 PartnerMaxAgeNs = 250 * Ms;
// The target must have been hit this recently (the parry shot, the charged swing's back stagger).
constexpr s64 TargetMaxAgeNs = 10'000 * Ms;

u8* g_base = nullptr;
u64 g_module_size = 0;
bool g_enabled = false;
int g_log = 0;

// The held throw's target, compared by the frame-time detour's entry (a plain qword read).
std::atomic<u8*> g_partner{nullptr};
static_assert(sizeof(std::atomic<u8*>) == sizeof(u8*) && std::atomic<u8*>::is_always_lock_free);
std::atomic<float> g_partner_rate{1.0f};
std::atomic<s64> g_partner_ns{0}; // the main player's update that last set it
// Freehand's catch holds the world still: while this is non-zero the detour's entry sends every
// character through the hook, and every one but the hunter and his target runs at g_world_rate
// (VisceralPullPartnerRate; fresh only with g_partner_ns, as the partner's).
std::atomic<u64> g_world{0};
static_assert(sizeof(std::atomic<u64>) == sizeof(u64) && std::atomic<u64>::is_always_lock_free);
std::atomic<float> g_world_rate{1.0f};

GrabCamera::VisceralCameraLifetime g_camera_hold;
s32 g_camera_hp = 0;
P::Hold g_hold; // the update thread only (the main player's frame time)
s32 g_hold_hp = 0;
std::mutex g_learned_mutex;
P::Learned g_learned;

// One coherent update for the damage hook and body; never combine fields from two throws.
struct ThrowSample {
    u8* player{nullptr};
    u8* target{nullptr};
    u32 serial{0};
    bool active{false};
    bool recover{false};
    bool failed{false};
    bool camera_active{false};
    bool camera_cancelled{false};
    s32 clip{-1};
    float clip_time{0.0f};
    s64 ns{0};
};
ThrowSample g_current{}; // main update only
SeqLock<ThrowSample> g_throw_sample;
struct RipSample {
    u32 serial{0};
    s32 clip{-1};
    float clip_time{0.0f};
};
SeqLock<RipSample> g_rip;
u32 g_damage_seen{0}; // protected by g_learned_mutex

s64 NowNs() {
    return VR::Body::SteadyNowNs();
}

// `chr`'s ThrowState (0: none; odd: it throws; even: it is thrown); -1 when unreadable.
// `check_vtable`: the module must be the player's SprjChrThrowModule.
int ThrowStateOf(const u8* chr, bool check_vtable) {
    const u8* const bag = Read<const u8*>(chr + OffsetChrModules);
    if (!Plausible(bag)) {
        return -1;
    }
    const u8* const module = Read<const u8*>(bag + OffsetThrowModule);
    if (!Plausible(module) ||
        (check_vtable && Read<const u8*>(module) != g_base + ThrowVtableRva)) {
        return -1;
    }
    const u8* const node = Read<const u8*>(module + OffsetThrowNode);
    return Plausible(node) ? Read<u8>(node + OffsetThrowState) : 0;
}

bool IsChr(const u8* chr) {
    if (!Plausible(chr)) {
        return false;
    }
    const u8* const vt = Read<const u8*>(chr);
    return vt == g_base + ChrInsVtableRva || vt == g_base + EnemyInsVtableRva;
}

int HpOf(const u8* chr) {
    const u8* const bag = Read<const u8*>(chr + OffsetChrModules);
    const u8* const data = Plausible(bag) ? Read<const u8*>(bag + OffsetDataModule) : nullptr;
    return Plausible(data) ? Read<s32>(data + OffsetHp) : 0;
}

P::Config ConfigOf(const VR::Melee::MeleeSettings& ms) {
    P::Config c{};
    c.enabled = ms.visceral_pull;
    c.freehand = ms.visceral_freehand;
    c.rate = ms.visceral_pull_speed;
    c.lead_s = ms.visceral_pull_lead_s;
    c.at_s = ms.visceral_pull_at_s;
    c.timeout_s = ms.visceral_pull_timeout_s;
    c.catch_s = ms.visceral_pull_catch_s;
    c.entry_rate = ms.visceral_pull_entry_speed;
    c.rip_rate = ms.visceral_pull_rip_speed;
    c.recover_s = ms.visceral_pull_recover_s;
    c.impact_s = ms.visceral_pull_impact_ms * 0.001f;
    return c;
}

std::string ClipName(s32 clip) {
    return clip >= 0 ? fmt::format("a{:03}_{:06}", clip / 1'000'000, clip % 1'000'000)
                     : std::string{"?"};
}

} // Anonymous namespace

void InitVisceralPull(VAddr base, u64 size) {
    g_base = reinterpret_cast<u8*>(base);
    g_module_size = size;
    g_log = std::getenv("VR_VISCERAL_PULL_LOG") != nullptr ? 1 : 0;
    const auto& ms = VR::Melee::GetMeleeSettings();
    g_enabled = ms.visceral_pull && size > WorldChrManRva + 8;
    {
        std::scoped_lock lock{g_learned_mutex};
        g_learned.Parse(ms.visceral_pull_learned);
    }
    if (!ms.visceral_pull) {
        LOG_INFO(Core, "VR visceral pull: off (visceral_pull=0): viscerals as cinematic_visceral "
                       "says, no hold");
        return;
    }
    LOG_INFO(Core,
             "VR visceral pull: freehand={} (tracked arms throughout; the world holds "
             "until your pull, at most {:.0f} s, full damage, when on)",
             ms.visceral_freehand, P::FreehandTimeoutS);
    LOG_INFO(
        Core,
        "VR visceral pull: on: throws in first person, the rip waits for your pull (x{:.2f} "
        "speed, {:.2f} s before it, at most {:.1f} s (legacy); your pull drives it to the rip; "
        "its beat "
        "{:.0f} ms); {} learned rip time{}{}",
        ms.visceral_pull_speed, ms.visceral_pull_lead_s, ms.visceral_pull_timeout_s,
        ms.visceral_pull_impact_ms, g_learned.Count(), g_learned.Count() == 1 ? "" : "s",
        ms.visceral_pull_at_s > 0.0f
            ? fmt::format(" (visceral_pull_at_s={:.2f}: a fixed hold point)", ms.visceral_pull_at_s)
            : std::string{});
}

bool VisceralPullWanted() {
    return g_enabled;
}

void ConfirmVisceralPull(bool frame_time_hook, bool damage_hook) {
    if (g_enabled) {
        const char* missing = !frame_time_hook ? "the frame-time detour"
                              : !damage_hook   ? "the damage detour (rip timing and mistimed pulls)"
                                               : nullptr;
        if (missing != nullptr) {
            g_enabled = false;
            LOG_WARNING(Core,
                        "VR visceral pull: OFF: {} is not installed; viscerals as "
                        "cinematic_visceral says",
                        missing);
        }
    }
    if (g_enabled) InstallVisceralStage(reinterpret_cast<VAddr>(g_base), g_module_size);
    VR::SetVisceralFirstPerson(g_enabled);
}

u8* const* VisceralPullPartnerSlot() {
    return reinterpret_cast<u8* const*>(&g_partner);
}

const u64* VisceralPullWorldSlot() {
    return reinterpret_cast<const u64*>(&g_world);
}

float VisceralPullMain(u8* player, s32 clip, float clip_time, float dt, bool control) {
    if (!g_enabled || player == nullptr) {
        return 1.0f;
    }
    const auto& ms = VR::Melee::GetMeleeSettings();
    const P::Config cfg = ConfigOf(ms);
    const s64 now = NowNs();
    if (g_current.player != player) {
        g_camera_hold.Reset();
        g_camera_hp = 0;
    }
    if (g_current.player != player || now - g_current.ns > PartnerMaxAgeNs) {
        g_hold.Cancel();
        g_current = {};
        g_partner.store(nullptr, std::memory_order_release);
        g_world.store(0, std::memory_order_release);
    }
    const int state = ThrowStateOf(player, true);
    const bool throwing = ms.visceral_pull && state > 0 && (state & 1) != 0;
    if (throwing && !g_current.active && !g_camera_hold.Active()) {
        g_current.target = nullptr;
    }

    // Latch the victim once: a later projectile hitting someone else must not change partners.
    u8* target = throwing || g_camera_hold.Active() ? g_current.target : nullptr;
    if (throwing && target == nullptr) {
        s64 hit_ns = 0;
        u8* const last = LastPlayerHitTarget(hit_ns);
        if (last != player && now - hit_ns < TargetMaxAgeNs && IsChr(last)) {
            const int ts = ThrowStateOf(last, false);
            if (ts > 0 && (ts & 1) == 0) {
                target = last;
            }
        }
    }
    const int ts = target != nullptr && IsChr(target) ? ThrowStateOf(target, false) : -1;
    const bool partner_ok = ts > 0 && (ts & 1) == 0;
    const int hp = HpOf(player);
    const bool held = g_hold.State() == P::Phase::Held;
    const char* abort = VR::IsGameMenuOpen()     ? "a menu"
                        : hp <= 0                ? "the hunter died"
                        : held && hp < g_hold_hp ? "the hunter was hit"
                        : !partner_ok            ? "its target is not in the throw"
                                                 : nullptr;
    Cb::VisceralPulled pulled{};
    RipSample rip{};
    const bool ripped = g_rip.Load(rip) && g_current.active && rip.serial == g_current.serial;
    P::In in{};
    in.now = now;
    in.throwing = throwing && partner_ok;
    in.clip = clip;
    in.clip_time = clip_time;
    in.dt = std::isfinite(dt) && dt > 0.0f ? dt : 1.0f / 30.0f;
    // The presenter's pull for this throw (from the catch on).
    const bool pull_seen = g_current.active && Cb::VisceralPulledChannel().Load(pulled) &&
                           pulled.serial == g_current.serial;
    in.progress = pull_seen ? pulled.progress : 0.0f;
    in.pulled = pull_seen && pulled.pulled && pulled.ns <= now;
    in.pulled_ns = pulled.ns;
    in.ripped = ripped;
    in.abort = abort;
    float at = 0.0f;
    {
        std::scoped_lock lock{g_learned_mutex};
        at = g_learned.At(clip);
    }
    const P::Out out = g_hold.Step(in, at, cfg);
    const float rate = partner_ok ? out.rate : 1.0f;
    // (The world's rate is set before its flag; the target keeps its slot while the world holds,
    // so it never takes the world's rate.)
    const float world = partner_ok ? out.world : 1.0f;
    // Outside a throw an open visceral's staggered target takes the slot instead: its stagger
    // runs at StaggerStretchRate (about 0.40 s longer). (It closes as the hunter's throw begins.)
    Cb::VisceralOpen open{};
    u8* const staggered = !throwing && Cb::VisceralOpenChannel().Load(open) && open.open &&
                                  now - open.ns < PartnerMaxAgeNs && open.target != player &&
                                  IsChr(static_cast<const u8*>(open.target))
                              ? static_cast<u8*>(const_cast<void*>(open.target))
                              : nullptr;
    g_partner_rate.store(staggered != nullptr ? P::StaggerStretchRate : rate,
                         std::memory_order_relaxed);
    g_world_rate.store(world, std::memory_order_relaxed);
    g_partner_ns.store(now, std::memory_order_release);
    g_partner.store(throwing && partner_ok && (rate != 1.0f || world != 1.0f) ? target : staggered,
                    std::memory_order_release);
    g_world.store(throwing && world != 1.0f ? 1 : 0, std::memory_order_release);
    if (out.held_now) {
        g_hold_hp = hp;
        LOG_INFO(Core,
                 "VR visceral pull: #{} CATCH {} at {:.3f} s: the heartbeat; the pull drives it{}",
                 g_hold.Serial(), ClipName(clip), clip_time,
                 world != 1.0f ? "; the world holds still" : "");
    }
    if (out.released_now || (g_hold.Failed() && !g_current.failed)) {
        const double held_ms =
            g_hold.HeldNs() != 0 ? static_cast<double>(now - g_hold.HeldNs()) * 1e-6 : 0.0;
        LOG_INFO(Core,
                 "VR visceral pull: #{} released ({}) after {:.0f} ms, the pull at {:.0f}% ({:.3f} "
                 "s); extraction x{:.2f}, damage x{:.2f}",
                 g_hold.Serial(), out.why, held_ms, in.progress * 100.0f, clip_time, cfg.rip_rate,
                 g_hold.Failed() ? P::MistimedDamageScale : 1.0f);
    }
    if (out.impact_now) {
        LOG_INFO(Core, "VR visceral pull: #{} the rip's beat: x{:.2f} for {:.0f} ms",
                 g_hold.Serial(), P::ImpactRate, ms.visceral_pull_impact_ms);
    }
    if (g_log && throwing != g_current.active) {
        LOG_INFO(Core, "VR visceral pull: throw {} ({})", throwing ? "began" : "ended",
                 ClipName(clip));
    }
    // The pull's partner/clip checks control animation pacing, not camera ownership.
    // Hold the camera through extraction even if the victim exits its throw or dies.
    const auto kind = Body::ClassifyAnim(clip);
    const bool cancel_action = control && !GrabCamera::IsVisceralAnimation(clip) &&
        (kind == Body::AnimClass::Dodge || kind == Body::AnimClass::AttackRight ||
         kind == Body::AnimClass::AttackBoth || kind == Body::AnimClass::AttackLeft ||
         kind == Body::AnimClass::ItemUse || kind == Body::AnimClass::Transform ||
         kind == Body::AnimClass::Event || kind == Body::AnimClass::Damage ||
         kind == Body::AnimClass::Death);
    const char* camera_abort = VR::IsGameMenuOpen() ? "menu"
        : hp <= 0 ? "hunter died"
        : g_camera_hold.Active() && g_camera_hp > 0 && hp < g_camera_hp ? "hunter took damage"
        : nullptr;
    const bool was_camera = g_camera_hold.Active();
    g_current.camera_active = g_camera_hold.Step(ms.visceral_pull && ms.visceral_freehand,
                                                state, clip, clip_time, cancel_action,
                                                camera_abort, now);
    g_current.camera_cancelled = g_camera_hold.Cancelled();
    UpdateVisceralStage(player, target, g_current.camera_active,
                        camera_abort != nullptr || g_current.camera_cancelled, now);
    g_camera_hp = hp;
    if (was_camera != g_current.camera_active) {
        LOG_INFO(Core, "VR visceral camera: {} ({}) hunter state={} victim state={} "
                       "clip={} time={:.3f}s pull released={}",
                 g_current.camera_active ? "hold" : "release", g_camera_hold.Reason(),
                 state, ts, ClipName(clip), clip_time, g_hold.State() == P::Phase::Released);
    }
    g_current.player = player;
    g_current.target = target;
    g_current.serial = g_hold.Serial();
    g_current.active = in.throwing;
    g_current.failed = g_hold.Failed();
    g_current.clip = clip;
    g_current.clip_time = clip_time;
    g_current.ns = now;
    g_current.recover = throwing && abort == nullptr &&
                        P::RecoveryReady(ripped, clip, rip.clip, clip_time, rip.clip_time, cfg);
    g_throw_sample.Store(g_current);
    Cb::VisceralHold h{};
    h.serial = g_current.serial;
    h.active = in.throwing;
    h.failed = g_current.failed;
    h.released = g_hold.State() == P::Phase::Released;
    h.held = g_hold.State() == P::Phase::Held;
    h.camera_valid = true;
    h.camera_active = g_current.camera_active;
    h.camera_cancelled = g_current.camera_cancelled;
    h.held_ns = g_hold.HeldNs();
    h.why = out.released_now ? out.why : "";
    h.ns = now;
    Cb::VisceralHoldChannel().Store(h);
    return rate;
}

bool VisceralPullFreehand() {
    const auto& ms = VR::Melee::GetMeleeSettings();
    return g_enabled && ms.visceral_pull && ms.visceral_freehand;
}

bool VisceralPullRecoverArms(const u8* player) {
    ThrowSample s{};
    return g_enabled && VR::Melee::GetMeleeSettings().visceral_pull && g_throw_sample.Load(s) &&
           s.player == player && s.active && s.recover && NowNs() - s.ns < PartnerMaxAgeNs;
}

u8* VisceralPullVictim(const u8* player) {
    ThrowSample s{};
    return g_enabled && g_throw_sample.Load(s) && s.player == player &&
                   (s.active || s.camera_active) && !s.camera_cancelled &&
                   NowNs() - s.ns < PartnerMaxAgeNs && IsChr(s.target)
               ? s.target
               : nullptr;
}

float VisceralPullPartnerRate(const u8* chr) {
    if (chr == nullptr ||
        NowNs() - g_partner_ns.load(std::memory_order_acquire) > PartnerMaxAgeNs) {
        return 1.0f;
    }
    if (chr == g_partner.load(std::memory_order_acquire)) {
        return g_partner_rate.load(std::memory_order_relaxed);
    }
    return g_world.load(std::memory_order_acquire) != 0
               ? g_world_rate.load(std::memory_order_relaxed)
               : 1.0f;
}

bool VisceralPullOnDamage(u8* player, u8* target, s32 bullet) {
    ThrowSample s{};
    if (!g_enabled || bullet != -1 || !g_throw_sample.Load(s) || !s.active || s.player != player ||
        target == nullptr || target != s.target || target == player ||
        NowNs() - s.ns > PartnerMaxAgeNs || !IsChr(target)) {
        return false;
    }
    const int ps = ThrowStateOf(player, true);
    const int ts = ThrowStateOf(target, false);
    if (ps <= 0 || (ps & 1) == 0 || ts <= 0 || (ts & 1) != 0) {
        return false;
    }
    bool fresh = false;
    std::string text;
    {
        std::scoped_lock lock{g_learned_mutex};
        if (g_damage_seen == s.serial) {
            return s.failed;
        }
        g_damage_seen = s.serial;
        fresh = g_learned.Note(s.clip, s.clip_time);
        text = g_learned.Text();
        g_rip.Store({s.serial, s.clip, s.clip_time});
        Cb::VisceralRipChannel().Store({s.serial, NowNs(), s.failed});
    }
    LOG_INFO(Core, "VR visceral pull: #{} {} rips at {:.3f} s{}", s.serial, ClipName(s.clip),
             s.clip_time, fresh ? " (learned for the next throw; saved in vr.ini)" : "");
    if (fresh) {
        VR::SaveSetting("visceral_pull_learned", text);
    }
    return s.failed;
}

} // namespace VR::Bloodborne
