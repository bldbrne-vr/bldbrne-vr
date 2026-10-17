// SPDX-License-Identifier: GPL-2.0-or-later

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
#include "core/vr/melee_settings.h"
#include "core/vr/visceral_channel.h"
#include "core/vr/visceral_pull.h"
#include "core/vr/interaction_visceral.h"
#include "core/vr/interaction_native_visceral_query.h"
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

u8* g_base = nullptr;
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

// The frame-time detour lives until process exit. Retain one loader/context reference
// for that lifetime; initialize it before detours can call the game-thread hold.
VR::Interactions::Module* g_hold_module{};
bool g_hold_fault{};
VR::Interactions::VisceralRuntime g_hold_runtime;
VR::Interactions::VisceralHold g_hold{g_hold_runtime};
std::mutex g_learned_mutex;
VR::Interactions::VisceralTimes g_learned;
VR::Interactions::Module* g_timing_module{};
bool g_timing_fault{};

// One coherent update for the damage hook and body; never combine fields from two throws.
struct ThrowSample {
    u8* player{nullptr};
    u8* target{nullptr};
    u32 serial{0};
    bool active{false};
    bool recover{false};
    bool failed{false};
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

s64 NowNs() {
    return VR::Body::SteadyNowNs();
}

bool NativeVisceralPolicy(const PiNativeVisceralQueryInput& facts,
                          PiNativeVisceralQueryOutput& decision) {
    return VR::Interactions::NativeVisceralQuery(g_hold_module, facts, decision);
}

void DisableVisceralHold(const char* why) {
    g_hold.Cancel();
    g_enabled=false;
    g_partner.store(nullptr,std::memory_order_release);
    g_world.store(0,std::memory_order_release);
    g_partner_rate.store(1,std::memory_order_relaxed);
    g_world_rate.store(1,std::memory_order_relaxed);
    g_current={};g_throw_sample.Store(g_current);Cb::VisceralHoldChannel().Store({});
    VR::SetVisceralFirstPerson(false);
    LOG_ERROR(Core,"VR visceral hold: {}; released partner/world speed and disabled hold",why);
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
    g_log = std::getenv("VR_VISCERAL_PULL_LOG") != nullptr ? 1 : 0;
    const auto& ms = VR::Melee::GetMeleeSettings();
    g_enabled = ms.visceral_pull && size > WorldChrManRva + 8;
    if(g_enabled && VR::Interactions::DllRequested()) {
        if(!g_hold_module)g_hold_module=new VR::Interactions::Module();
        g_hold_module->EnableFaultBroadcast();
        if(!g_hold_module->Available() && !g_hold_module->LoadBesideExecutable()) {
            g_enabled=false;
            LOG_ERROR(Core,"VR visceral hold: DLL unavailable: {}; interactive hold disabled",
                g_hold_module->Diagnostic());
        } else {
            g_hold_runtime.Bind(*g_hold_module,g_hold_fault);
            LOG_INFO(Core,"VR visceral hold: separate game-thread DLL context initialized");
        }
    }
    {
        std::scoped_lock lock{g_learned_mutex};
        if(g_enabled && VR::Interactions::DllRequested()) {
            if(!g_timing_module)g_timing_module=new VR::Interactions::Module();
            g_timing_module->EnableFaultBroadcast();
            if(!g_timing_module->Available() && !g_timing_module->LoadBesideExecutable()) {
                g_enabled=false;
                LOG_ERROR(Core,"VR visceral timing: DLL unavailable: {}",g_timing_module->Diagnostic());
            } else g_learned.Bind(*g_timing_module,g_timing_fault);
        }
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
    VR::SetVisceralFirstPerson(g_enabled);
}

u8* const* VisceralPullPartnerSlot() {
    return reinterpret_cast<u8* const*>(&g_partner);
}

const u64* VisceralPullWorldSlot() {
    return reinterpret_cast<const u64*>(&g_world);
}

float VisceralPullMain(u8* player, s32 clip, float clip_time, float dt) {
    if (!g_enabled || player == nullptr) {
        return 1.0f;
    }
    if(VR::Interactions::ProcessFaultLatch().Tripped()) {
        DisableVisceralHold("physical interaction module fault");
        return 1;
    }
    const auto& ms = VR::Melee::GetMeleeSettings();
    const P::Config cfg = ConfigOf(ms);
    const s64 now = NowNs();
    PiNativeVisceralQueryInput query{};
    query.operation = PI_NVQ_SCOPE;
    query.now_ns = now;
    query.stamp_ns = g_current.ns;
    query.player_state = ThrowStateOf(player, true);
    query.flags = (g_current.player == player ? PI_NVF_SAME_PLAYER : 0) |
                  (ms.visceral_pull ? PI_NVF_PULL_ENABLED : 0) |
                  (g_current.active ? PI_NVF_ACTIVE : 0) |
                  (g_current.target ? PI_NVF_HAVE_TARGET : 0);
    PiNativeVisceralQueryOutput scope{};
    if (!NativeVisceralPolicy(query, scope)) {
        DisableVisceralHold("DLL native scope fault");
        return 1;
    }
    if (scope.flags & PI_NVO_RESET) {
        g_hold.Cancel();
        g_current = {};
        g_partner.store(nullptr, std::memory_order_release);
        g_world.store(0, std::memory_order_release);
    }
    const bool throwing = (scope.flags & PI_NVO_THROWING) != 0;
    // Only a DLL-approved retained identity can survive a later projectile hit.
    u8* target = (scope.flags & PI_NVO_KEEP_TARGET) ? g_current.target : nullptr;
    if (scope.flags & PI_NVO_QUERY_TARGET) {
        s64 hit_ns = 0;
        u8* const last = LastPlayerHitTarget(hit_ns);
        query = {};
        query.operation = PI_NVQ_CANDIDATE;
        query.now_ns = now; query.stamp_ns = hit_ns;
        query.flags = (last ? PI_NVF_HAVE_TARGET : 0) |
                      (last == player ? PI_NVF_TARGET_IS_PLAYER : 0);
        PiNativeVisceralQueryOutput candidate{};
        if (!NativeVisceralPolicy(query, candidate)) {
            DisableVisceralHold("DLL native target fault");
            return 1;
        }
        if ((candidate.flags & PI_NVO_ALLOW) && IsChr(last)) {
            const int ts = ThrowStateOf(last, false);
            if (ts > 0 && (ts & 1) == 0) {
                target = last;
            }
        }
    }
    const int ts = target != nullptr && IsChr(target) ? ThrowStateOf(target, false) : -1;
    const bool partner_ok = ts > 0 && (ts & 1) == 0;
    const int hp = HpOf(player);
    Cb::VisceralPulled pulled{};
    RipSample rip{};
    const bool have_rip = g_rip.Load(rip);
    P::In in{};
    in.now = now;
    in.clip = clip;
    in.clip_time = clip_time;
    in.dt = std::isfinite(dt) && dt > 0.0f ? dt : 1.0f / 30.0f;
    // The presenter's pull for this throw (from the catch on).
    const bool have_pull = (scope.flags & PI_NVO_QUERY_RECEIPTS) &&
                           Cb::VisceralPulledChannel().Load(pulled);
    query = {};
    query.operation = PI_NVQ_RECEIPTS;
    query.now_ns = now; query.stamp_ns = pulled.ns;
    query.serial = g_current.serial; query.rip_serial = rip.serial;
    query.pull_serial = pulled.serial;
    query.flags = (g_current.active ? PI_NVF_ACTIVE : 0) |
                  (have_rip ? PI_NVF_HAVE_RIP : 0) | (have_pull ? PI_NVF_HAVE_PULL : 0) |
                  (pulled.pulled ? PI_NVF_PULL_COMPLETE : 0);
    PiNativeVisceralQueryOutput receipt{};
    if (!NativeVisceralPolicy(query, receipt)) {
        DisableVisceralHold("DLL native receipt fault");
        return 1;
    }
    in.progress = (receipt.flags & PI_NVO_PULL_SEEN) ? pulled.progress : 0.0f;
    in.pulled = (receipt.flags & PI_NVO_PULLED) != 0;
    in.pulled_ns = pulled.ns;
    in.ripped = (receipt.flags & PI_NVO_RIPPED) != 0;
    float at = 0.0f;
    {
        std::scoped_lock lock{g_learned_mutex};
        at = g_learned.At(clip);
    }
    const PiNativeVisceralHoldFacts native{throwing,partner_ok,VR::IsGameMenuOpen(),hp};
    const P::Out out = g_hold.NativeStep(in,at,cfg,native,rip.clip,rip.clip_time);
    if(g_hold.Faulted()) {
        DisableVisceralHold("DLL hold fault");
        return 1;
    }
    const float rate = out.rate;
    // (The world's rate is set before its flag; the target keeps its slot while the world holds,
    // so it never takes the world's rate.)
    const float world = out.world;
    // Outside a throw an open visceral's staggered target takes the slot instead: its stagger
    // runs at StaggerStretchRate (about 0.40 s longer). (It closes as the hunter's throw begins.)
    Cb::VisceralOpen open{};
    const bool have_open = !throwing && Cb::VisceralOpenChannel().Load(open);
    query = {};
    query.operation = PI_NVQ_OPEN;
    query.now_ns = now; query.stamp_ns = open.ns;
    query.flags = (throwing ? PI_NVF_THROWING : 0) | (have_open ? PI_NVF_HAVE_SAMPLE : 0) |
                  (open.open ? PI_NVF_OPEN : 0) | (open.target ? PI_NVF_HAVE_TARGET : 0) |
                  (open.target == player ? PI_NVF_TARGET_IS_PLAYER : 0);
    PiNativeVisceralQueryOutput candidate{};
    if (!NativeVisceralPolicy(query, candidate)) {
        DisableVisceralHold("DLL native stagger fault");
        return 1;
    }
    u8* const staggered = (candidate.flags & PI_NVO_ALLOW) &&
                         IsChr(static_cast<const u8*>(open.target))
                             ? static_cast<u8*>(const_cast<void*>(open.target)) : nullptr;
    query = {};
    query.operation = PI_NVQ_PUBLISH;
    query.flags = (throwing ? PI_NVF_THROWING : 0) |
                  (partner_ok ? PI_NVF_PARTNER_OK : 0) |
                  (staggered ? PI_NVF_STAGGER_VALID : 0);
    query.partner_rate = rate; query.world_rate = world;
    PiNativeVisceralQueryOutput publication{};
    if (!NativeVisceralPolicy(query, publication)) {
        DisableVisceralHold("DLL native rate-publication fault");
        return 1;
    }
    g_partner_rate.store(publication.rate, std::memory_order_relaxed);
    g_world_rate.store(publication.world, std::memory_order_relaxed);
    g_partner_ns.store(now, std::memory_order_release);
    g_partner.store(publication.actor == PI_NVA_TARGET ? target :
                    publication.actor == PI_NVA_STAGGERED ? staggered : nullptr,
                    std::memory_order_release);
    g_world.store((publication.flags & PI_NVO_WORLD_ACTIVE) ? 1 : 0, std::memory_order_release);
    if (out.held_now) {
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
    g_current.camera_cancelled = g_hold.CameraCancelled();
    g_current.player = player;
    g_current.target = target;
    g_current.serial = g_hold.Serial();
    g_current.active = g_hold.NativeActive();
    g_current.failed = g_hold.Failed();
    g_current.clip = clip;
    g_current.clip_time = clip_time;
    g_current.ns = now;
    g_current.recover = g_hold.Recover();
    g_throw_sample.Store(g_current);
    Cb::VisceralHold h{};
    h.serial = g_current.serial;
    h.active = g_current.active;
    h.failed = g_current.failed;
    h.released = g_hold.State() == P::Phase::Released;
    h.held = g_hold.State() == P::Phase::Held;
    h.camera_cancelled = g_current.camera_cancelled;
    h.held_ns = g_hold.HeldNs();
    h.why = out.released_now ? out.why : "";
    h.ns = now;
    Cb::VisceralHoldChannel().Store(h);
    return rate;
}

bool VisceralPullFreehand() {
    const auto& ms = VR::Melee::GetMeleeSettings();
    PiNativeVisceralQueryInput query{};
    query.operation = PI_NVQ_FREEHAND;
    query.flags = (g_enabled ? PI_NVF_ENABLED : 0) |
                  (ms.visceral_pull ? PI_NVF_PULL_ENABLED : 0) |
                  (ms.visceral_freehand ? PI_NVF_FREEHAND : 0);
    PiNativeVisceralQueryOutput out{};
    return NativeVisceralPolicy(query, out) && (out.flags & PI_NVO_ALLOW);
}

bool VisceralPullRecoverArms(const u8* player) {
    ThrowSample s{};
    const bool have = g_throw_sample.Load(s);
    PiNativeVisceralQueryInput query{};
    query.operation = PI_NVQ_RECOVER;
    query.now_ns = NowNs(); query.stamp_ns = s.ns;
    query.flags = (g_enabled ? PI_NVF_ENABLED : 0) |
                  (VR::Melee::GetMeleeSettings().visceral_pull ? PI_NVF_PULL_ENABLED : 0) |
                  (have ? PI_NVF_HAVE_SAMPLE : 0) | (s.player == player ? PI_NVF_SAME_PLAYER : 0) |
                  (s.active ? PI_NVF_ACTIVE : 0) | (s.recover ? PI_NVF_RECOVER : 0);
    PiNativeVisceralQueryOutput out{};
    return NativeVisceralPolicy(query, out) && (out.flags & PI_NVO_ALLOW);
}

u8* VisceralPullVictim(const u8* player) {
    ThrowSample s{};
    const bool have = g_throw_sample.Load(s);
    PiNativeVisceralQueryInput query{};
    query.operation = PI_NVQ_VICTIM;
    query.now_ns = NowNs(); query.stamp_ns = s.ns;
    query.flags = (g_enabled ? PI_NVF_ENABLED : 0) | (have ? PI_NVF_HAVE_SAMPLE : 0) |
                  (s.player == player ? PI_NVF_SAME_PLAYER : 0) | (s.active ? PI_NVF_ACTIVE : 0);
    PiNativeVisceralQueryOutput out{};
    return NativeVisceralPolicy(query, out) && (out.flags & PI_NVO_ALLOW) ? s.target : nullptr;
}

float VisceralPullPartnerRate(const u8* chr) {
    PiNativeVisceralQueryInput query{};
    query.operation = PI_NVQ_RATE;
    query.now_ns = NowNs();
    query.stamp_ns = g_partner_ns.load(std::memory_order_acquire);
    query.flags = (chr ? PI_NVF_HAVE_ACTOR : 0) |
                  (chr == g_partner.load(std::memory_order_acquire) ? PI_NVF_SAME_TARGET : 0) |
                  (g_world.load(std::memory_order_acquire) ? PI_NVF_WORLD_ACTIVE : 0);
    query.partner_rate = g_partner_rate.load(std::memory_order_relaxed);
    query.world_rate = g_world_rate.load(std::memory_order_relaxed);
    PiNativeVisceralQueryOutput out{};
    return NativeVisceralPolicy(query, out) ? out.rate : 1;
}

bool VisceralPullOnDamage(u8* player, u8* target, s32 bullet) {
    ThrowSample s{};
    const bool have = g_throw_sample.Load(s);
    PiNativeVisceralQueryInput query{};
    query.operation = PI_NVQ_DAMAGE;
    query.now_ns = NowNs(); query.stamp_ns = s.ns; query.bullet = bullet;
    query.flags = (g_enabled ? PI_NVF_ENABLED : 0) | (have ? PI_NVF_HAVE_SAMPLE : 0) |
                  (s.active ? PI_NVF_ACTIVE : 0) | (s.player == player ? PI_NVF_SAME_PLAYER : 0) |
                  (target ? PI_NVF_HAVE_TARGET : 0) | (target == s.target ? PI_NVF_SAME_TARGET : 0) |
                  (target == player ? PI_NVF_TARGET_IS_PLAYER : 0);
    PiNativeVisceralQueryOutput out{};
    if (!NativeVisceralPolicy(query, out) || !(out.flags & PI_NVO_ALLOW) || !IsChr(target))
        return false;
    query = {};
    query.operation = PI_NVQ_DAMAGE_STATE;
    query.player_state = ThrowStateOf(player, true);
    query.target_state = ThrowStateOf(target, false);
    if (!NativeVisceralPolicy(query, out) || !(out.flags & PI_NVO_ALLOW))
        return false;
    bool fresh = false;
    bool reduce_damage = false;
    std::string text;
    {
        std::scoped_lock lock{g_learned_mutex};
        const auto damage = g_learned.NoteDamage(s.serial, s.failed, s.clip, s.clip_time);
        if (g_learned.Faulted()) return false;
        if (!damage.first) return damage.reduce_damage != 0;
        fresh = damage.learned != 0;
        reduce_damage = damage.reduce_damage != 0;
        text = g_learned.Text();
        g_rip.Store({damage.serial, s.clip, s.clip_time});
        Cb::VisceralRipChannel().Store({damage.serial, NowNs(), reduce_damage});
    }
    LOG_INFO(Core, "VR visceral pull: #{} {} rips at {:.3f} s{}", s.serial, ClipName(s.clip),
             s.clip_time, fresh ? " (learned for the next throw; saved in vr.ini)" : "");
    if (fresh) {
        VR::SaveSetting("visceral_pull_learned", text);
    }
    return reduce_damage;
}

} // namespace VR::Bloodborne
