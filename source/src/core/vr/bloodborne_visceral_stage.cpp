// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include "common/arch.h"
#include "common/logging/log.h"
#include "core/vr/bloodborne_evasion.h"
#include "core/vr/bloodborne_layout.h"
#include "core/vr/bloodborne_melee.h"
#include "core/vr/bloodborne_visceral_pull.h"
#include "core/vr/bloodborne_visceral_stage.h"
#include "core/vr/body_channel.h"
#include "core/vr/hook_util.h"
#include "core/vr/visceral_channel.h"
#include "core/vr/visceral_stage.h"
#include "core/vr/vr_session.h"
#include "core/vr/write_watch.h"

namespace VR::Bloodborne {
namespace {
using Hook::Read;
using Hook::Plausible;
namespace L = Layout;
constexpr u64 MovementRva = 0x1A61260;
// ChrThrowModule's pending destination is consumed here by native character movement.
// The following branch rejects !module[0x51]; later code reads module+0x30 and
// adds (destination - physics_position) / dt to physics module+0x2C0.
constexpr std::array<u8, 15> MovementSig{
    0x55,0x48,0x89,0xE5,0x53,0x50,0x48,0x89,0xFB,0x31,0xC0,0x80,0x7B,0x51,0x00};
u8* game{};
u64 game_size{};
std::unique_ptr<Xbyak::CodeGenerator> code;
std::mutex mutex;
struct State {
    VisceralStage::Entry entry{};
    VisceralStage::Lifetime life{};
    u8* player{};
    u8* victim{};
    u64 serial{};
    bool confirmed{};
    unsigned hunter_writes{}, victim_writes{};
    // The drawn hold (Hold): how far the victim is drawn back (world units, only grows), along
    // `away`, at most `cap` (the map behind it); `base_ns` its first update.
    float push{}, cap{};
    Body::Vec3 away{}, last_root{};
    s64 base_ns{};
    bool stopping{};
    bool dumped[2]{};
};
State state;

u8* Module(const u8* chr, u32 offset) {
    if (!Plausible(chr)) return nullptr;
    const auto bag = Read<u8*>(chr + L::OffsetChrModules);
    return Plausible(bag) ? Read<u8*>(bag + offset) : nullptr;
}

bool PhysicsRoot(const u8* chr, Body::Vec3& root) {
    const auto p = Module(chr, 0x68);
    if (!Plausible(p) || Read<const u8*>(p + L::OffsetOwner) != chr) return false;
    root = Read<Body::Vec3>(p + 0x1E0);
    return Body::IsFinite(root);
}

bool TraceOn() { static const bool on = std::getenv("VR_VISCERAL_STAGE_TRACE") != nullptr; return on; }
void Trace(const char* kind, u64 record, u64 serial, Body::Vec3 a, Body::Vec3 b) {
    if (!TraceOn()) return;
    static std::mutex trace_mutex;
    std::scoped_lock lock{trace_mutex};
    static FILE* file = std::fopen("visceral_stage.csv", "w");
    if (!file) return;
    std::fprintf(file, "%lld,%s,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                 static_cast<long long>(Body::SteadyNowNs()), kind,
                 static_cast<unsigned long long>(record), static_cast<unsigned long long>(serial),
                 a.x,a.y,a.z,b.x,b.y,b.z);
    std::fflush(file);
}

// The size-aware hold (VisceralStage::BodyPush): the victim goes back from where the game puts it,
// straight away from the entry's eyes, only as far as its own body (its live capsules) needs to
// keep KeepM x world scale from them, never more than MaxPushM; then only as far as the map lets
// it (the game's own sphere cast from its middle along the push, MarginM short of what it hits),
// so the game's alignment is never sent into a wall. False: leave the destination alone (a
// sentinel, no body read, the destination at the eyes).
constexpr float KeepM = 0.30f;
constexpr float MaxPushM = 1.5f;
constexpr float CastLiftM = 1.0f, CastRadiusM = 0.3f, MarginM = 0.05f;
bool VictimDestination(const VisceralStage::Entry& e, const u8* victim, Body::Vec3& destination) {
    if (!VisceralStage::Valid(e) || !Body::IsFinite(destination) ||
        Body::LengthSq(destination - e.physics_root) > 36 * e.scale * e.scale)
        return false;
    // VR_VISCERAL_STAGE_KEEP_M=<m> (tests): the clearance instead of KeepM.
    static const float keep_m = [] {
        const char* env = std::getenv("VR_VISCERAL_STAGE_KEEP_M");
        const float m = env && *env ? std::strtof(env, nullptr) : 0.0f;
        return std::isfinite(m) && m > 0.0f && m < 2.0f ? m : KeepM;
    }();
    Body::Vec3 root{};
    if (!PhysicsRoot(victim, root)) return false;
    std::array<GrabCamera::CollisionCapsule, 128> body{};
    const size_t count = VictimCapsules(victim, root, e.scale, body);
    Body::Vec3 away{destination.x - e.eye.x, 0.0f, destination.z - e.eye.z};
    const float gap = Body::Length(away);
    if (count == 0 || gap < 1e-3f) return false;
    away = away * (1.0f / gap);
    float push = VisceralStage::BodyPush(e.eye, away, std::span{body.data(), count},
                                         destination - root, keep_m * e.scale, MaxPushM * e.scale);
    if (push > 0.0f) {
        const float from[3]{destination.x, destination.y + CastLiftM * e.scale, destination.z};
        const float to[3]{from[0] + away.x * push, from[1], from[2] + away.z * push};
        float hit[3]{}, normal[3]{}, fraction = 1.0f;
        if (CastMapSphere(from, to, CastRadiusM * e.scale, hit, normal, fraction))
            push = std::max(0.0f, push * fraction - MarginM * e.scale);
    }
    destination.x += away.x * push;
    destination.z += away.z * push;
    return true;
}

void PS4_SYSV_ABI MovementHook(u8* module, u8*) {
    if (!VisceralStageEnabled() || !Plausible(module) || !Read<u8>(module + 0x51)) return;
    const auto chr = Read<u8*>(module + L::OffsetOwner);
    State s;
    { std::scoped_lock lock{mutex}; s = state; }
    const s64 now = Body::SteadyNowNs();
    if (!s.life.Active(now) || (chr != s.player && chr != s.victim)) return;
    // Never adjust a reused module, unrelated pair or incoming enemy grab.
    if (Module(chr, L::OffsetThrowModule) != module) return;
    const auto node = Read<u8*>(module + L::OffsetThrowNode);
    if (!Plausible(node) || Read<u8*>(node + L::OffsetOwner) != chr) return;
    const int native = Read<u8>(node + L::OffsetThrowState);
    const bool hunter = chr == s.player;
    if (native <= 0 || ((native & 1) != 0) != hunter) return;
    const u8* other = hunter ? s.victim : s.player;
    if (!Plausible(other) || Read<u32>(node + 0x24) != Read<u32>(other + 0x8)) return;
    auto destination = Read<Body::Vec3>(module + 0x30);
    const auto original = destination;
    if (hunter) {
        if (!VisceralStage::Constrain(s.entry, true, destination)) return;
    } else if (!VictimDestination(s.entry, chr, destination)) {
        return;
    }
    // Changing the pair's alignment destination can make native throw setup abort.
    // Keep the consumer observational while validating the independent camera anchor.
    static const bool constrain = std::getenv("VR_VISCERAL_STAGE_CONSTRAIN") != nullptr;
    if (!constrain) {
        Trace(hunter ? "hunter_observed" : "victim_observed", 0, s.serial, original, destination);
        return;
    }
    std::memcpy(module + 0x30, &destination, sizeof(destination));
    { std::scoped_lock lock{mutex};
      if (state.serial == s.serial) ++(hunter ? state.hunter_writes : state.victim_writes); }
    Trace(hunter ? "hunter_destination" : "victim_destination", 0, s.serial, original, destination);
}
} // namespace

bool VisceralStageEnabled() {
    return code && VisceralPullFreehand() && VR::IsFirstPerson() && !VR::CinematicActive();
}

void InstallHold();

void InstallVisceralStage(VAddr base, u64 size) {
    if (code) return;
    game = reinterpret_cast<u8*>(base);
    game_size = size;
    if (size < MovementRva + MovementSig.size() ||
        std::memcmp(game + MovementRva, MovementSig.data(), MovementSig.size()) != 0 ||
        !Hook::MakeWritable(game + MovementRva, MovementSig.size())) {
        LOG_ERROR(Core, "VR visceral stage: native movement signature mismatch; stage disabled");
        return;
    }
    auto stub = std::make_unique<Xbyak::CodeGenerator>(2048);
    auto target = Hook::EmitHostCallStub(*stub, reinterpret_cast<void*>(&MovementHook),
        MovementSig.data(), MovementSig.size(),
        reinterpret_cast<u64>(game + MovementRva + MovementSig.size()), Xbyak::util::rdi);
    Hook::WriteJump(game + MovementRva, MovementSig.size(), target);
    code = std::move(stub);
    InstallHold();
    LOG_INFO(Core, "VR visceral stage: native pair destination hook installed; entry view owns camera");
}

u64 BeginVisceralStage(u8* player) {
    if (!VisceralStageEnabled() || VR::IsGameMenuOpen()) return 0;
    const s64 now = Body::SteadyNowNs();
    Body::Anchor anchor{};
    if (!Body::LatestAnchor(anchor) || !anchor.valid || now < anchor.steady_ns ||
        now - anchor.steady_ns > 150'000'000) return 0;
    VisceralStage::Entry entry;
    entry.base = anchor.eye_world - anchor.head_offset;
    entry.eye = anchor.eye_world;
    entry.root = anchor.chr_t;
    entry.scale = anchor.world_scale;
    entry.forward = {std::sin(anchor.view_yaw), 0, std::cos(anchor.view_yaw)};
    if (!PhysicsRoot(player, entry.physics_root) || !VisceralStage::Valid(entry)) return 0;
    Combat::VisceralOpen open{};
    const bool have_victim = Combat::VisceralOpenChannel().Load(open) && open.open &&
        now >= open.ns && now - open.ns < 250'000'000;
    std::scoped_lock lock{mutex};
    if (state.life.Active(now)) return 0;
    const u64 serial = state.serial + 1;
    state = {};
    state.serial = serial;
    state.entry = entry;
    state.player = player;
    state.victim = have_victim ? static_cast<u8*>(const_cast<void*>(open.target)) : nullptr;
    state.life.Begin(now);
    Trace("begin", anchor.record, serial, entry.base, entry.physics_root);
    LOG_INFO(Core, "VR visceral stage #{}: armed BEFORE native throw, camera record {}", serial, anchor.record);
    return serial;
}

void ConfirmVisceralStage(u64 token, bool accepted) {
    if (!token) return;
    std::scoped_lock lock{mutex};
    if (state.serial != token) return;
    state.confirmed = accepted;
    if (!accepted) state.life.Reject();
    Trace(accepted ? "accepted" : "rejected", 0, token, state.entry.base, state.entry.root);
}

// The drawn hold. During a throw the game draws the victim where its physics body is (0x15132E0
// builds ChrCtrl W from the physics position and copies it to the render instance at
// [ctrl+0xF0]+0x60), and the physics step rewrites that position every update (0x1E389ED), so
// neither a destination nor a position write holds it back. Right after that build (the call
// site 0x1515223, on the victim's own update thread) the victim's W, its inverse and the render
// copy are moved by the published offset: the whole drawn body, its hit capsules with it; its
// physics body and the throw's pairing stay the game's own.
constexpr u64 WorldBuiltRva = 0x1515223;
// mov byte [rbx+0x210], 0 (the "rebuild W" flag); mov rax, [r14+8]; mov rcx, [rax+0x3B0]
constexpr std::array<u8, 18> WorldBuiltSig{0xC6, 0x83, 0x10, 0x02, 0x00, 0x00, 0x00, 0x49, 0x8B,
                                           0x46, 0x08, 0x48, 0x8B, 0x88, 0xB0, 0x03, 0x00, 0x00};
std::unique_ptr<Xbyak::CodeGenerator> hold_code;
std::atomic<u8*> hold_ctrl{nullptr};  // the victim's ChrCtrl while it is held
std::atomic<u8*> hold_owner{nullptr}; // ... and the victim itself (the ctrl's owner then)
std::atomic<u64> hold_offset{0};      // world x, z (two floats)

void PublishHold(u8* ctrl, u8* owner, float dx, float dz) {
    const float d[2]{dx, dz};
    u64 packed{};
    std::memcpy(&packed, d, sizeof(packed));
    hold_offset.store(packed, std::memory_order_relaxed);
    hold_owner.store(owner, std::memory_order_relaxed);
    hold_ctrl.store(ctrl, std::memory_order_release);
}

void DropHold() {
    hold_ctrl.store(nullptr, std::memory_order_release);
    hold_owner.store(nullptr, std::memory_order_relaxed);
    hold_offset.store(0, std::memory_order_relaxed);
}

void PS4_SYSV_ABI OnWorldBuilt(u8* ctrl) {
    if (ctrl != hold_ctrl.load(std::memory_order_acquire)) return;
    // The same character still: a freed victim's ChrCtrl address reused by another is not held.
    if (Read<u8*>(ctrl + L::OffsetOwner) != hold_owner.load(std::memory_order_relaxed)) return;
    const u64 packed = hold_offset.load(std::memory_order_relaxed);
    float d[2];
    std::memcpy(d, &packed, sizeof(d));
    if (!std::isfinite(d[0]) || !std::isfinite(d[1]) || (d[0] == 0.0f && d[1] == 0.0f)) return;
    // W: three float4 rows {R row i, t_i}; its inverse the same at +0xC0 (t' = t + d, so the
    // inverse's translation takes -R^-1 d); the render copy keeps t as its last column (+0x90).
    auto* const w = reinterpret_cast<float*>(ctrl + 0x90);
    w[3] += d[0];
    w[11] += d[1];
    auto* const inv = reinterpret_cast<float*>(ctrl + 0xC0);
    for (int i = 0; i < 3; ++i) inv[4 * i + 3] -= inv[4 * i] * d[0] + inv[4 * i + 2] * d[1];
    u8* const instance = Read<u8*>(ctrl + 0xF0);
    if (Plausible(instance, 16)) {
        auto* const t = reinterpret_cast<float*>(instance + 0x90);
        t[0] += d[0];
        t[2] += d[1];
    }
}

// Gated stub: one compare per W build for every character; the host call only for the victim's.
void* EmitHoldStub(Xbyak::CodeGenerator& c, u8* site) {
    using namespace Xbyak::util;
    c.align(16);
    void* const entry = const_cast<u8*>(c.getCurr());
    Xbyak::Label skip;
    c.lea(rsp, ptr[rsp - 128]); // keep the guest red zone intact
    c.pushf();
    c.push(rax);
    c.mov(rax, reinterpret_cast<u64>(&hold_ctrl));
    c.mov(rax, ptr[rax]);
    c.cmp(rax, r14);
    c.pop(rax); // pop keeps the flags
    c.jne(skip, Xbyak::CodeGenerator::T_NEAR);
    for (const auto& reg : {rax, rcx, rdx, rbx, rsi, rdi, r8, r9, r10, r11, rbp}) c.push(reg);
    c.mov(rdi, r14);
    c.mov(rbp, rsp);
    c.and_(rsp, -32);
    c.sub(rsp, 32 * 16);
    for (int i = 0; i < 16; ++i) c.vmovdqu(ptr[rsp + 32 * i], Xbyak::Ymm(i));
    c.mov(rax, reinterpret_cast<u64>(&OnWorldBuilt));
    c.call(rax);
    for (int i = 0; i < 16; ++i) c.vmovdqu(Xbyak::Ymm(i), ptr[rsp + 32 * i]);
    c.mov(rsp, rbp);
    for (const auto& reg : {rbp, r11, r10, r9, r8, rdi, rsi, rbx, rdx, rcx, rax}) c.pop(reg);
    c.L(skip);
    c.popf();
    c.lea(rsp, ptr[rsp + 128]);
    for (const u8 byte : WorldBuiltSig) c.db(byte);
    c.db(0xFF); // jmp qword [rip+0]; dq resume
    c.db(0x25);
    c.dd(0);
    c.dq(reinterpret_cast<u64>(site + WorldBuiltSig.size()));
    c.ready();
    return entry;
}

void InstallHold() {
    u8* const site = game + WorldBuiltRva;
    if (game_size < WorldBuiltRva + WorldBuiltSig.size() ||
        std::memcmp(site, WorldBuiltSig.data(), WorldBuiltSig.size()) != 0 ||
        !Hook::MakeWritable(site, WorldBuiltSig.size())) {
        LOG_ERROR(Core, "VR visceral stage: drawn-hold site mismatch; the victim is not held");
        return;
    }
    auto stub = std::make_unique<Xbyak::CodeGenerator>(1024);
    void* const entry = EmitHoldStub(*stub, site);
    Hook::WriteJump(site, WorldBuiltSig.size(), entry);
    hold_code = std::move(stub);
}

// VR_VISCERAL_STAGE_HOLD: 1 size-aware (the default), 0 none; test: a fixed HoldTestM from 1.4 s
// into the stage, the mock's eyes dumped just before and after; shift: ShiftTest.
int HoldMode() {
    static const int mode = [] {
        const char* v = std::getenv("VR_VISCERAL_STAGE_HOLD");
        if (!v || !*v) return 1;
        const std::string_view mode{v};
        return mode == "test" ? 2 : mode == "shift" ? 3 : (v[0] == '1' ? 1 : 0);
    }();
    return mode;
}
constexpr float HoldTestM = 0.6f;
// The nearest any part of the victim may come to the eyes (m x world scale; 0.40 until the user's
// 2026-10-28 headset run: 48 cm); VR_VISCERAL_STAGE_KEEP_M=<m> (tests) instead.
constexpr float HoldKeepDefaultM = 0.48f;
float HoldKeepM() {
    static const float keep_m = [] {
        const char* env = std::getenv("VR_VISCERAL_STAGE_KEEP_M");
        const float m = env && *env ? std::strtof(env, nullptr) : 0.0f;
        return std::isfinite(m) && m > 0.0f && m < 2.0f ? m : HoldKeepDefaultM;
    }();
    return keep_m;
}

void DumpEyes(const char* name) {
    const char* dir = std::getenv("MOCKXR_DUMP_DIR");
    if (!dir) return;
    if (FILE* f = std::fopen((std::string(dir) + "\\dump_now").c_str(), "w")) {
        std::fprintf(f, "%s\n", name);
        std::fclose(f);
        LOG_INFO(Core, "VR visceral stage: eyes dumped as {}", name);
    }
}

// The released hold eases out after the stage while the victim is thrown clear.
struct Release {
    u8* victim{};
    u8* ctrl{};
    float push{};
    Body::Vec3 away{};
    s64 last_ns{};
};
Release release;
constexpr float ReleaseMps = 1.0f;

// `victim` is still the live character whose ChrCtrl is `ctrl` (its ctrl and physics module both
// name it their owner): what every write after the stage checks first, so a victim freed or its
// memory reused (a death, a warp, an area load right after the throw) is never written.
bool StillVictim(u8* victim, u8* ctrl) {
    if (!Plausible(victim) || !Plausible(ctrl)) return false;
    if (Read<u8*>(victim + L::OffsetChrCtrl) != ctrl || Read<u8*>(ctrl + L::OffsetOwner) != victim)
        return false;
    u8* const physics = Module(victim, 0x68);
    return Plausible(physics) && Read<u8*>(physics + L::OffsetOwner) == victim;
}

// Rebuilds its W at its next update (the game's own "moved" flag), checked first.
void ForceRebuild(u8* victim) {
    u8* const physics = Plausible(victim) ? Module(victim, 0x68) : nullptr;
    if (Plausible(physics) && Read<u8*>(physics + L::OffsetOwner) == victim) physics[0x210] = 1;
}

void Hold(State& s, s64 now) {
    const int mode = HoldMode();
    if (mode == 0 && !TraceOn()) return;
    Body::Anchor anchor{};
    Body::Vec3 root{};
    if (!PhysicsRoot(s.victim, root) || !Body::LatestAnchor(anchor) || !anchor.valid) return;
    u8* const ctrl = Read<u8*>(s.victim + L::OffsetChrCtrl);
    if (!Plausible(ctrl) || Read<const u8*>(ctrl + L::OffsetOwner) != s.victim) return;
    std::array<GrabCamera::CollisionCapsule, 128> capsules{};
    const size_t count = VictimCapsules(s.victim, root, s.entry.scale, capsules);
    const auto body = std::span{capsules.data(), count};
    const float gap = GrabCamera::NearestGap(anchor.eye_world, body);
    if (!s.stopping) {
        Body::Vec3 away{root.x - s.entry.eye.x, 0.0f, root.z - s.entry.eye.z};
        if (Body::Length(away) < 1e-3f) return;
        s.away = away * (1.0f / Body::Length(away));
        s.base_ns = now;
        s.stopping = true;
        // The map behind it: never further than the game's own sphere cast from its middle gets.
        s.cap = MaxPushM * s.entry.scale;
        const float from[3]{root.x, root.y + CastLiftM * s.entry.scale, root.z};
        const float to[3]{from[0] + s.away.x * s.cap, from[1], from[2] + s.away.z * s.cap};
        float hit[3]{}, normal[3]{}, fraction = 1.0f;
        if (CastMapSphere(from, to, CastRadiusM * s.entry.scale, hit, normal, fraction))
            s.cap = std::max(0.0f, s.cap * fraction - MarginM * s.entry.scale);
    }
    const float t = static_cast<float>(now - s.base_ns) * 1e-9f;
    // The game's own placement (the pairing's snap) moves the root before the capsules follow:
    // that update's body is stale, so it is not measured.
    const bool settled = Body::Length(root - s.last_root) < 0.05f * s.entry.scale;
    s.last_root = root;
    if (mode == 1 && count > 0 && settled) {
        // The capsules follow the drawn body, so they already carry the hold: measured where the
        // game alone would have them, then held back by what is missing (only ever more, so the
        // victim never comes back toward you during the throw), at most to the map behind it.
        const float need = VisceralStage::BodyPush(anchor.eye_world, s.away, body,
                                                   s.away * -s.push, HoldKeepM() * s.entry.scale,
                                                   MaxPushM * s.entry.scale);
        s.push = std::min(s.cap, std::max(s.push, need));
    } else if (mode == 2) {
        if (t >= 1.30f && !s.dumped[0]) { s.dumped[0] = true; DumpEyes("hold_before"); }
        if (t >= 1.40f) s.push = HoldTestM * s.entry.scale;
        if (t >= 1.55f && !s.dumped[1]) { s.dumped[1] = true; DumpEyes("hold_after"); }
    }
    if (mode != 0) {
        PublishHold(ctrl, s.victim, s.away.x * s.push, s.away.z * s.push);
        ForceRebuild(s.victim);
        release = {s.victim, ctrl, s.push, s.away, now};
    }
    if (TraceOn()) {
        const Body::Vec3 w{Read<float>(ctrl + 0x90 + 12), Read<float>(ctrl + 0x90 + 28),
                           Read<float>(ctrl + 0x90 + 44)};
        Trace("victim_gap", anchor.record, s.serial, root,
              {std::isfinite(gap) ? gap : -99.0f, s.push, static_cast<float>(count)});
        Trace("victim_w", anchor.record, s.serial, w, {t, s.cap, 0.0f});
    }
}

// VR_VISCERAL_STAGE_HOLD=shift (tests, outside any visceral): every 10 s from 40 s, the visceral
// test's target within 3 m drawn ShiftM to the side of the eyes for 0.3 s, the mock's eyes dumped
// before, during and after (the same W build as during a throw: does it reach the drawn body?).
constexpr float ShiftM = 0.6f;
void ShiftTest(s64 now) {
    static s64 first = 0, next = 0;
    static int phase = 0, round = 0;
    static u8* chr = nullptr;
    if (!first) first = next = now + 40'000'000'000LL;
    if (round >= 3 || now < next) return;
    Body::Anchor anchor{};
    Body::Vec3 root{};
    if (phase == 0) {
        chr = VisceralTestTarget();
        if (!PhysicsRoot(chr, root) || !Body::LatestAnchor(anchor) || !anchor.valid ||
            std::hypot(root.x - anchor.eye_world.x, root.z - anchor.eye_world.z) > 3.0f) {
            next = now + 500'000'000;
            return;
        }
        u8* const ctrl = Read<u8*>(chr + L::OffsetChrCtrl);
        if (!Plausible(ctrl)) return;
        DumpEyes(fmt::format("shift{}_before", round).c_str());
        const float dx = root.x - anchor.eye_world.x, dz = root.z - anchor.eye_world.z;
        const float n = std::hypot(dx, dz);
        PublishHold(ctrl, chr, -dz / n * ShiftM, dx / n * ShiftM);
        ForceRebuild(chr);
        LOG_INFO(Core, "VR visceral stage: shift test {}: the target drawn {:.1f} m to the side at "
                       "{:.2f} m", round, ShiftM, n);
        phase = 1;
        next = now + 300'000'000;
    } else if (phase == 1) {
        ForceRebuild(chr);
        DumpEyes(fmt::format("shift{}_during", round).c_str());
        phase = 2;
        next = now + 100'000'000;
    } else if (phase == 2) {
        DropHold();
        ForceRebuild(chr);
        phase = 3;
        next = now + 300'000'000;
    } else {
        DumpEyes(fmt::format("shift{}_after", round).c_str());
        phase = 0;
        ++round;
        next = now + 10'000'000'000LL;
    }
}

// After the stage: the hold eases out at ReleaseMps (it never comes back toward you faster than
// the throw sends the victim away), then the victim is the game's alone again. Re-checked every
// update: a victim that is no longer the same character, a menu, a load or the stage turned off
// (first person gone) drops the hold at once, with nothing written to the victim.
void ReleaseTick(s64 now) {
    if (!release.ctrl) return;
    if (!StillVictim(release.victim, release.ctrl) || !VisceralStageEnabled() ||
        VR::IsGameMenuOpen()) {
        DropHold();
        release = {};
        return;
    }
    const float dt = std::clamp(static_cast<float>(now - release.last_ns) * 1e-9f, 0.0f, 0.1f);
    release.last_ns = now;
    release.push = std::max(0.0f, release.push - ReleaseMps * dt);
    if (release.push <= 0.0f) {
        DropHold();
        ForceRebuild(release.victim); // once more without the hold
        release = {};
        return;
    }
    PublishHold(release.ctrl, release.victim, release.away.x * release.push,
                release.away.z * release.push);
    ForceRebuild(release.victim);
}

// VR_VISCERAL_STAGE_WATCH=1 (finding where the game places the victim during the throw): from the
// first update that knows the victim until the stage ends, every write to the victim's drawn world
// translation (ChrCtrl W, x), its physics position (x) and the hunter's W (x) traps
// (core/vr/write_watch.h); the end logs each writer, how often, its largest step and its stack.
struct Watch {
    u64 serial{};
    s64 armed_ns{};
};
Watch watch;

bool WatchOn() { static const bool on = std::getenv("VR_VISCERAL_STAGE_WATCH") != nullptr; return on; }

std::string CodeAt(u64 address) {
    const u64 base = reinterpret_cast<u64>(game);
    return address > base && address < base + game_size ? fmt::format("{:#x}", address - base)
                                                         : fmt::format("host {:#x}", address);
}

void ReportWatch(s64 now) {
    u32 dropped = 0;
    const auto hits = VR::WriteWatch::TakeHits(dropped);
    const char* who[3] = {"victim W", "victim physics", "hunter W"};
    struct Writer {
        int slot; u64 rip; u32 n; s64 first, last; float step; const VR::WriteWatch::Hit* sample;
    };
    std::vector<Writer> writers;
    std::array<const VR::WriteWatch::Hit*, 3> previous{};
    for (const auto& h : hits) {
        float step = 0.0f;
        if (const auto* p = previous[h.slot])
            step = std::hypot(h.value[0] - p->value[0], h.value[2] - p->value[2]);
        previous[h.slot] = &h;
        Writer* w = nullptr;
        for (auto& c : writers) if (c.slot == h.slot && c.rip == h.rip) w = &c;
        if (!w) { writers.push_back({h.slot, h.rip, 0, h.ns, h.ns, 0.0f, &h}); w = &writers.back(); }
        ++w->n; w->last = h.ns; w->step = std::max(w->step, step);
    }
    LOG_INFO(Core, "VR visceral watch #{}: {} writes in {} ms ({} dropped)", watch.serial,
             hits.size(), (now - watch.armed_ns) / 1'000'000, dropped);
    for (const auto& w : writers) {
        std::string stack;
        for (u32 k = 0; k < w.sample->stack_count; ++k)
            stack += (k ? " " : "") + CodeAt(w.sample->stack[k]);
        LOG_INFO(Core, "VR visceral watch: {} written {} times by {} (thread {}, +{} .. +{} ms), "
                       "largest step {:.1f} cm, first ({:.3f}, {:.3f}, {:.3f}); stack {}",
                 who[w.slot], w.n, CodeAt(w.rip), w.sample->thread,
                 (w.first - watch.armed_ns) / 1'000'000, (w.last - watch.armed_ns) / 1'000'000,
                 w.step * 100.0f, w.sample->value[0], w.sample->value[1], w.sample->value[2],
                 stack.empty() ? std::string{"-"} : stack);
    }
}

void WatchTick(const State& s, bool active, s64 now) {
    if (!WatchOn()) return;
    if (VR::WriteWatch::Armed()) {
        if (!active || s.serial != watch.serial || now - watch.armed_ns > 6'000'000'000LL) {
            VR::WriteWatch::Disarm();
            ReportWatch(now);
        }
        return;
    }
    if (!active || s.serial == watch.serial || !Plausible(s.victim)) return;
    const auto world_x = [](const u8* chr) -> const void* {
        const u8* const ctrl = Plausible(chr) ? Read<u8*>(chr + L::OffsetChrCtrl) : nullptr;
        return Plausible(ctrl) && Read<const u8*>(ctrl + L::OffsetOwner) == chr ? ctrl + 0x9C
                                                                                 : nullptr;
    };
    const u8* const physics = Module(s.victim, 0x68);
    const void* const victim_w = world_x(s.victim);
    if (!victim_w) return;
    watch = {s.serial, now};
    const bool armed = VR::WriteWatch::Arm(
        {victim_w, Plausible(physics) ? physics + 0x1E0 : nullptr, world_x(s.player)}, {16, 4, 16},
        game, game_size);
    LOG_INFO(Core, "VR visceral watch #{}: {}", s.serial, armed ? "armed" : "could not arm");
}

void UpdateVisceralStage(u8* player, u8* victim, bool owns_camera, bool cancelled, s64 now) {
    // Native button-initiated throws also get a stage, using the last rendered view.
    if (owns_camera) BeginVisceralStage(player);
    std::scoped_lock lock{mutex};
    const bool was = state.life.Active(now);
    if (!was) {
        if (HoldMode() == 3) ShiftTest(now);
        ReleaseTick(now);
        return;
    }
    if (victim) state.victim = victim;
    Hold(state, now);
    WatchTick(state, true, now);
    state.life.Update(owns_camera, cancelled || player != state.player || !VisceralStageEnabled(), now);
    if (!state.life.Active(now)) WatchTick(state, false, now);
    if (!state.life.Active(now)) {
        LOG_INFO(Core, "VR visceral stage #{}: ended; native destination writes hunter={} victim={}",
                 state.serial, state.hunter_writes, state.victim_writes);
        Trace("end", 0, state.serial, state.entry.base, state.entry.root);
    }
}

bool VisceralStagePlacement(s64 now, float scale, GrabCamera::ThrowPlacement& out) {
    if (!VisceralStageEnabled()) return false;
    std::scoped_lock lock{mutex};
    if (!state.life.Active(now) || scale != state.entry.scale) return false;
    out = {state.entry.base, state.entry.root, {}, now, 1, scale, true, true};
    return true;
}

void TraceVisceralCamera(u64 record, const float* position, const float* tracked_offset) {
    if (!TraceOn()) return;
    State s;
    { std::scoped_lock lock{mutex}; s = state; }
    const Body::Vec3 actual{position[0],position[1],position[2]};
    const Body::Vec3 tracked{tracked_offset[0],tracked_offset[1],tracked_offset[2]};
    Trace("camera", record, s.serial, s.life.Active(Body::SteadyNowNs()) ? s.entry.base + tracked : actual, actual);
    Trace("tracked", record, s.serial, tracked, {});
}

void TraceVisceralDraw(u64 record, const float* expected, const float* actual, const float* scene) {
    if (!TraceOn()) return;
    State s;
    { std::scoped_lock lock{mutex}; s = state; }
    // The first scene buffer can belong to an auxiliary view. Record each distinct
    // view/projection (bounded to 8), using only thread-local deduplication state.
    static thread_local u64 last_record{};
    static thread_local std::array<std::array<float,28>,8> seen{};
    static thread_local unsigned count{};
    if (last_record != record) { last_record=record; count=0; }
    std::array<float,28> key{};
    std::memcpy(key.data(),scene+180,12*sizeof(float));
    std::memcpy(key.data()+12,scene+52,16*sizeof(float));
    for (unsigned i=0;i<count;++i)
        if (std::memcmp(seen[i].data(),key.data(),sizeof(key))==0) return;
    if (count==seen.size()) return;
    const unsigned index=count;
    seen[count++]=key;
    const auto kind = [index](const char* base) {
        return index==0 ? std::string(base) : fmt::format("{}_{}",base,index);
    };
    Trace(kind("draw").c_str(), record, s.serial, {expected[0],expected[1],expected[2]},
          {actual[0],actual[1],actual[2]});
    const float* inverse_view=scene+180;
    for (int axis=0; axis<3; ++axis) {
        const char* kinds[]{"draw_right", "draw_up", "draw_forward"};
        const Body::Vec3 row{inverse_view[axis],inverse_view[4+axis],inverse_view[8+axis]};
        Trace(kind(kinds[axis]).c_str(), record, s.serial, row, row);
    }
    Trace(kind("draw_projection").c_str(), record, s.serial,
          {scene[52],scene[57],scene[62]}, {scene[54],scene[58],scene[63]});

}

bool VisceralTraceOn() {
    return TraceOn();
}

void TraceVisceralView(const char* kind, u64 record, const float* expected, const float* actual) {
    if (!TraceOn()) return;
    u64 serial;
    { std::scoped_lock lock{mutex}; serial = state.serial; }
    Trace(kind, record, serial, {expected[0],expected[1],expected[2]},
          {actual[0],actual[1],actual[2]});
}
} // namespace VR::Bloodborne
