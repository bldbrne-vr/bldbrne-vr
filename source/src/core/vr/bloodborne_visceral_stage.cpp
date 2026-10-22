// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include "common/arch.h"
#include "common/logging/log.h"
#include "core/vr/bloodborne_layout.h"
#include "core/vr/bloodborne_visceral_pull.h"
#include "core/vr/bloodborne_visceral_stage.h"
#include "core/vr/body_channel.h"
#include "core/vr/hook_util.h"
#include "core/vr/visceral_channel.h"
#include "core/vr/visceral_stage.h"
#include "core/vr/vr_session.h"

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
    if (!VisceralStage::Constrain(s.entry, hunter, destination)) return;
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

void InstallVisceralStage(VAddr base, u64 size) {
    if (code) return;
    game = reinterpret_cast<u8*>(base);
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

void UpdateVisceralStage(u8* player, u8* victim, bool owns_camera, bool cancelled, s64 now) {
    // Native button-initiated throws also get a stage, using the last rendered view.
    if (owns_camera) BeginVisceralStage(player);
    std::scoped_lock lock{mutex};
    const bool was = state.life.Active(now);
    if (!was) return;
    if (victim) state.victim = victim;
    state.life.Update(owns_camera, cancelled || player != state.player || !VisceralStageEnabled(), now);
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

void TraceVisceralView(const char* kind, u64 record, const float* expected, const float* actual) {
    if (!TraceOn()) return;
    u64 serial;
    { std::scoped_lock lock{mutex}; serial = state.serial; }
    Trace(kind, record, serial, {expected[0],expected[1],expected[2]},
          {actual[0],actual[1],actual[2]});
}
} // namespace VR::Bloodborne
