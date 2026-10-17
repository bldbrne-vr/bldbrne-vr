// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include "common/logging/log.h"
#include "core/vr/bloodborne_levers.h"
#include "core/vr/body_frame.h"
// Before hook_util.h: its xbyak brings <windows.h>, whose `near` macro would eat members here.
#include "core/vr/lever_acquisition.h"
#include "core/vr/interaction_fault.h"
#include "core/vr/interaction_lever.h"
#include "core/vr/interaction_native_lever_prompt.h"
#include "core/vr/interaction_native_lever_rig.h"
#include "core/vr/hook_util.h"
#include "core/vr/lever_channel.h"
#include "core/vr/lever_models.h"
#include "core/vr/lever_pose_edit.h"
#include "core/vr/lever_settings.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace VR::Bloodborne {
namespace {

// Static 1.09 evidence: 13ADCC0 selects the first geometrically valid action; 13A4A30
// checks its permissions only after the normal interact button. 13AE600 receives the
// original event. Selection alone is NOT an eligible VR grab.
constexpr u64 SelectRva = 0x13AE171;
constexpr std::array<u8, 15> SelectBytes = {
    0x41, 0x89, 0x85, 0xA8, 0x00, 0x00, 0x00,       // mov [r13+A8], eax
    0x41, 0x80, 0xBD, 0x9C, 0x00, 0x00, 0x00, 0x00, // cmp byte [r13+9C], 0
};
// EMEVD ActionButton condition: unlike ObjAct selection this also offers prompts
// for disabled/already-used levers. r12 is the resolved target object (null for actors).
constexpr u64 ScriptPromptRva = 0x144E7A9;
constexpr std::array<u8, 15> ScriptPromptBytes = {
    0xC7, 0x44, 0x24, 0x08, 0, 0, 0, 0, 0xC7, 0x04, 0x24, 0, 0, 0, 0,
};
constexpr u64 PermissionRva = 0x13AE19A;
constexpr std::array<u8, 14> PermissionBytes = {
    0x48, 0x8B, 0x4D, 0x90,                         // mov rcx, [rbp-70]
    0x48, 0x8B, 0x8C, 0x19, 0x88, 0x00, 0x00, 0x00, // mov rcx, [rcx+rbx+88]
    0x84, 0xC0,                                     // test al, al
};
constexpr u64 EventRva = 0x13AE600;
constexpr std::array<u8, 20> EventBytes = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
    0x41, 0x54, 0x53, 0x48, 0x81, 0xEC, 0xA8, 0x01, 0x00, 0x00,
};
// The object pose job has completed here. The original call synchronizes model-space bones
// before rendering/collision consumers. Its relative call must be relocated explicitly.
constexpr u64 PoseRva = 0x15A434B;
constexpr std::array<u8, 19> PoseBytes = {
    0x48, 0x83, 0xC3, 0x38, 0x48, 0x89, 0xDF, 0xE8, 0xC9, 0x0B,
    0x12, 0xFF, 0x49, 0x8B, 0xBE, 0xA8, 0x02, 0x00, 0x00,
};
// Before the scheduler consumes pending indices, including when no prompt is selected.
constexpr u64 UpdateRva = 0x1CB4DD0;
constexpr std::array<u8, 17> UpdateBytes = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41,
    0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x38,
};
constexpr u64 SyncRva = 0x6C4F20;
constexpr std::array<u8, 16> SyncBytes = {
    0x80, 0x7F, 0x38, 0x00, 0x0F, 0x85, 0x83, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x07, 0x44, 0x8B, 0x40,
};
// Read directly by the x86-64 entry gate; only this opt-in diagnostic starts active.
static_assert(std::atomic<u8>::is_always_lock_free && sizeof(std::atomic<u8>) == 1);
std::atomic<u8> g_update_probe_active{0};
bool g_runtime_enabled{};
bool g_motion_test{};
bool g_diagnostics{};
constexpr u64 AcceptRva = 0x13AE370;
constexpr std::array<u8, 17> AcceptBytes = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41,
    0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x38,
};
constexpr u64 CheckRva = 0x13A4A30;
constexpr std::array<u8, 17> CheckBytes = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41,
    0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x48,
};
// The game's "may the player interact now" query: a bitmask of open menus, dialogs, talk and
// event states (zero when free). 13ADCC0 calls it every frame: any bit skips selection (13AE034),
// and these bits refuse the interact button (13ADDA0). Neither 13A4A30, 13AE370 nor 13AE600
// asks it. Past its static initialization (guard byte 5562888) it only reads menu state.
constexpr u64 BlockedRva = 0x176B7E0;
constexpr std::array<u8, 16> BlockedBytes = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
    0x53, 0x48, 0x83, 0xEC, 0x48, 0x4C, 0x8B, 0x3D,
};
constexpr u64 BlockedInitRva = 0x5562888;  // its static-initialization guard (nonzero: done)
constexpr u64 BlockedStateRva = 0x5562878; // the menu state it reads
constexpr u32 SelectBlocked = 0xFFFFFFFF;  // 13AE034: no action is selected
constexpr u32 AcceptBlocked = 0xFFFFFF37;  // 13ADDA0: the interact button is refused
// FD4 dependency invalidation, not the unrelated cloth publication at 1CA37B0.
constexpr u64 DirtyRva = 0x1D3C500;
constexpr std::array<u8, 16> DirtyBytes = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
    0x53, 0x50, 0x49, 0x89, 0xFF, 0x4C, 0x8D, 0x35,
};
constexpr u64 MatrixNodeVtableRva = 0x53A12B0;
std::atomic<u64> g_observed_object{};
std::atomic<s64> g_observe_until{};
s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
constexpr u64 ObjectManRva = 0x553B168;
constexpr u64 ObjActManRva = 0x553B0F0;   // SprjWorldObjActMan: +0x20 blocks, +0x28 array (0xD8)
constexpr u64 WorldChrManRva = 0x553E878; // +0x60: the main player
// 13ADCC0 checks its actions against [[553E880]+0x108] when set, else WorldChrMan+0x60
// (13ADD23..13ADD62 and 13AE021..13AE030): the actor the selector passes as r15.
constexpr u64 SelectionActorRva = 0x553E880;
u8 *g_base{};
std::unique_ptr<Xbyak::CodeGenerator> g_code;

// Diagnostic-only reads must fail closed, including during map unloading. Plausible alone
// does not establish that a pointer is mapped. Windows is the current investigation host.
bool Copy(u64 address, void *out, size_t size) {
    if (address < 0x10000 || address >= 0x0000800000000000ULL ||
        size > 0x0000800000000000ULL - address) {
        return false;
    }
#ifdef _WIN32
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address), out,
                             size, &copied) &&
           copied == size;
#else
    return false;
#endif
}
template <typename T> bool ReadAt(u64 address, T &out) { return Copy(address, &out, sizeof(out)); }
template <typename T, size_t N> T Field(const std::array<u8, N> &bytes, size_t offset) {
    T out{};
    if (offset <= N && sizeof(T) <= N - offset) {
        std::memcpy(&out, bytes.data() + offset, sizeof(out));
    }
    return out;
}

// The ObjActParam rows of every measured lever model (core/vr/lever_models.h): o240001 in
// m24_00 / 01 / 02, o270003 in the Forbidden Woods, o250010 and o253010 in Cainhurst, o243005
// (row 100) in Central Yharnam, o342202 in the Hunter's Nightmare, o352000 / o352001 in the
// Research Hall (and its capstan o353010, row 3500110), o220005 in Hemwick, o260041 and o260040 in
// Mensis, o280001 in Yahar'gul, o321110 in Byrgenwerth and o360010 in the Fishing Hamlet (no
// Chalice Dungeon row). Keep the
// original parameter/event identity: each row keeps its own game action, the rig only moves the
// handle. A listed row is only a candidate: the prompt is replaced per instance, when that
// object's rig binds (core/vr/lever_prompt.h).
constexpr const auto &PhysicalLeverActions = VR::Lever::Actions;
bool PhysicalLeverAction(s32 id) {
    return VR::Lever::SupportedAction(id);
}

bool OperateAnimation(s32 anim) {
    // This is a diagnostic filter, NOT a lever classification: cannons and revival share IDs.
    return anim == 101150 || anim == 101151 || anim == 101200 || anim == 101230 || anim == 101240;
}

bool Object(u32 handle, u64 &address, std::array<u8, 0x2E0> &object) {
    if (handle == std::numeric_limits<u32>::max()) {
        return false;
    }
    u64 manager{}, blocks{}, objects{};
    s32 block_count{}, object_count{};
    const u32 block = (handle >> 20) & 0xFF;
    const u32 index = handle & 0xFFFFF;
    if (!ReadAt(reinterpret_cast<u64>(g_base) + ObjectManRva, manager) ||
        !ReadAt(manager + 0x20, block_count) || block_count <= 0 || block_count > 256 ||
        block >= static_cast<u32>(block_count) || !ReadAt(manager + 0x28, blocks) ||
        !Hook::Plausible(reinterpret_cast<u8 *>(blocks))) {
        return false;
    }
    const u64 entry = blocks + block * 0x1F8;
    if (!ReadAt(entry + 0xC8, object_count) || object_count <= 0 || object_count > 0x100000 ||
        index >= static_cast<u32>(object_count) || !ReadAt(entry + 0xD0, objects) ||
        !Hook::Plausible(reinterpret_cast<u8 *>(objects))) {
        return false;
    }
    address = objects + static_cast<u64>(index) * 0x2E0;
    return Copy(address, object.data(), object.size());
}

// Game-thread adapter. Kept behind the development switch until rendered interaction
// and original gameplay completion have been validated together.
namespace LiveLever {
namespace L = VR::Lever;
s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
bool Writable(u64 address, size_t size) {
#ifdef _WIN32
    if (!size || address < 0x10000 || address >= 0x0000800000000000ULL ||
        size > 0x0000800000000000ULL - address)
        return false;
    const u64 end = address + size;
    while (address < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const auto protection = info.Protect & 0xFF;
        if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
            return false;
        const u64 next = reinterpret_cast<u64>(info.BaseAddress) + info.RegionSize;
        if (next <= address)
            return false;
        address = next;
    }
    return true;
#else
    return false;
#endif
}
struct Pose {
    u64 object{}, controller{}, node{}, skeleton{}, model{}, locals{}, models{}, flags{}, entry{};
    u64 matrix_node{}, mapping{};
    u32 handle{}; // the object's handle (a parked capstan's record names it)
    u32 count{}; // Havok bones: 4 (o240001's skeleton, o243005, ...) or 3 (o250010, o353010, ...)
    // The matrix provider's FLVER -> Havok table: one entry per FLVER bone (o260040: 2, o321110:
    // 4 over 3 Havok bones; a negative entry draws from no Havok bone).
    u32 map_count{};
    std::array<s16, L::MaxMeshBones> mesh_map{};
    std::array<float, 12> frame{};
    L::PoseBuffers Buffers() const {
        return {{reinterpret_cast<Body::HkQsTransform *>(locals), count},
                {reinterpret_cast<u32 *>(flags), count},
                *reinterpret_cast<u16 *>(node + 0x70)};
    }
};
struct Offer {
    u64 row{}, actor{}, block{}, param{};
    u32 handle{}, actor_handle{};
    s32 state{};
    s32 action{}; // its ObjActParam row id (a parked capstan's record keeps it)
    u64 generation{};
    s64 ns{};
    Pose pose{};
    L::BoneBinding binding{};
    L::Rig rig{};
    const L::Model *model{};
    float angle{};  // where its handle stands when offered: 0 at rest, a parked capstan's angle
    bool parked{};  // a parked capstan (`parked` below): the hold resumes its bone
};
Offer offer;
L::BasicPoseEdit<VR::Interactions::NativeLeverPoseGeometry> edit;
// The capstan the hand let go before its stop (lever_models.h Parked): written by Update and
// Unpark, read by Bind and Classify, all under the busy guard. Never set by motion-only rehearsal
// (its levers return to rest), whose Bind never reads it.
L::Parked parked;
static_assert(uint32_t(L::CommandKind::Hold) == PI_NLC_HOLD &&
              uint32_t(L::CommandKind::Release) == PI_NLC_RELEASE &&
              uint32_t(L::CommandKind::Complete) == PI_NLC_COMPLETE &&
              uint32_t(L::CommandKind::Settle) == PI_NLC_SETTLE);
u64 generation{}, attempted_completion{}, reported_completion{};
float logged_hold_angle{};
float applied_angle{};
VR::Interactions::LeverSettle settling;
VR::Interactions::Module* settle_module{};
bool settle_fault{};
PiNativeLeverGeometryResult ModelGeometry(const L::Model* model,float sweep = 0.f) {
    PiNativeLeverGeometryFacts p{};
    p.model_index = PhysicalInteractions::NativeLeverRigPackets::ModelIndex(model);
    p.binding.sweep = sweep;p.motion_test = g_motion_test;
    PiNativeLeverGeometryResult out{};
    VR::Interactions::NativeLeverGeometry(settle_module,PI_NLR_MODEL,p,out);
    return out;
}
VR::Interactions::NativeLeverPromptRuntime prompt_policy;
// These queries run before the busy guard and touch only copied facts and the
// immutable API table. They never share the stateful runtime's sequence fields.
bool NeedsPromptClassify(L::PromptVerdict& verdict, u64 row, u32 handle,
                         u64 fallback, u64 serial) {
    if (VR::Interactions::ProcessFaultLatch().Tripped()) {
        verdict.bound = 0; // Selection-thread cleanup restores the native prompt.
        return false;
    }
    PiNativeLeverQueryInput p{};
    p.operation = PI_NLQ_NEEDS_CLASSIFY;
    p.same_row = verdict.row == row;
    p.same_handle = verdict.handle == handle;
    p.recheck = verdict.recheck;
    p.fallback = fallback;p.serial = serial;
    p.previous_fallback = verdict.fallback;p.previous_serial = verdict.serial;
    PiNativeLeverQueryOutput out{};
    if (!VR::Interactions::NativeLeverQuery(settle_module,p,out)) {
        verdict.bound = 0;
        return false;
    }
    return out.value;
}
bool PromptReady(s64 stamp, s64 now) {
    if (VR::Interactions::ProcessFaultLatch().Tripped()) return false;
    PiNativeLeverQueryInput p{};
    p.operation = PI_NLQ_READY;p.stamp_ns = stamp;p.now_ns = now;
    PiNativeLeverQueryOutput out{};
    return VR::Interactions::NativeLeverQuery(settle_module,p,out) && out.value;
}
struct Dispatch {
    u64 block{};
    s32 index{-1};
    u32 actor{};
    bool active{}, seen{};
};
thread_local Dispatch dispatch;
// Only the synchronous event created by our own original acceptance call may be changed.
// B, remote receipt, other actors/rows, and later events keep their original record.
void RewriteEvent(u64 block, u64 record, u32 remote) {
    if (!dispatch.active || dispatch.seen || remote || block != dispatch.block)
        return;
    std::array<s32, 8> event{};
    if (!Copy(record, event.data(), sizeof(event)) || event[1] != dispatch.index ||
        static_cast<u32>(event[3]) != dispatch.actor || !Writable(record, sizeof(event)))
        return;
    event[4] = -1; // no hunter event animation (first person and hand control stay active)
    event[6] = -1; // the physical pull already placed the lever; do not replay its clip
    event[7] = 2;  // original event's non-alignment path; mode 0 DOES align to its dummy
    std::memcpy(reinterpret_cast<void *>(record), event.data(), sizeof(event));
    dispatch.seen = true;
}
void ReportCompletion(const L::Command &command, bool accepted) {
    reported_completion = command.serial;
    L::Channel().RecordCompletion(
        {NowNs(), command.serial, command.id, command.generation, command.hand, accepted});
}
// The game's own interaction gate (BlockedRva) with `mask`: true while a menu, dialog, talk or
// event blocks interaction, and (fail closed) before the game has ever asked it.
bool InteractionBlocked(u32 mask) {
    u8 initialized{};
    u64 state{};
    if (!ReadAt(reinterpret_cast<u64>(g_base) + BlockedInitRva, initialized) || !initialized ||
        !ReadAt(reinterpret_cast<u64>(g_base) + BlockedStateRva, state) ||
        !Hook::Plausible(reinterpret_cast<u8 *>(state)))
        return true;
    using Query = u32(PS4_SYSV_ABI *)();
    return (reinterpret_cast<Query>(g_base + BlockedRva)() & mask) != 0;
}
bool Complete(const L::Command &command) {
    if(VR::Interactions::ProcessFaultLatch().Tripped())return false;
    attempted_completion = command.serial; // never retry an irreversible dispatch
    if (g_motion_test) {
        ReportCompletion(command, false); // motion rehearsal never dispatches a game event
        return false; // next scheduler tick restores the original pose
    }
    // The game's button path refuses the interact press on these bits (13ADDA0); so does the
    // hand's. A pull finished while a dialog or event opened returns to rest without an event.
    if (InteractionBlocked(AcceptBlocked)) {
        ReportCompletion(command, false);
        LOG_INFO(Core, "VR lever development: complete handle={:#x} not dispatched: the game "
                       "blocks interaction now",
                 command.id);
        return false;
    }
    u64 rows{};
    u32 actor_handle{};
    if (!ReadAt(offer.block + 0x90, rows) || offer.row < rows ||
        !ReadAt(offer.actor + 8, actor_handle) || actor_handle != offer.actor_handle)
        return false;
    const auto index = static_cast<s32>((offer.row - rows) / 0x140);
    struct Scope {
        ~Scope() { dispatch = {}; }
    } scope;
    dispatch = {offer.block, index, actor_handle, true, false};
    using Accept = bool(PS4_SYSV_ABI *)(u64, s32, u32);
    const bool accepted =
        reinterpret_cast<Accept>(g_base + AcceptRva)(offer.block, index, actor_handle);
    const bool matched = dispatch.seen;
    ReportCompletion(command, accepted && matched);
    LOG_INFO(
        Core,
        "VR lever development: complete handle={:#x} generation={} event_matched={} accepted={}",
        command.id, command.generation, matched, accepted);
    return accepted && matched;
}

s64 offer_request{};
std::atomic_flag busy = ATOMIC_FLAG_INIT;
struct Guard {
    bool acquired{!busy.test_and_set(std::memory_order_acquire)};
    ~Guard() {
        if (acquired)
            busy.clear(std::memory_order_release);
    }
};

bool ReadPoseStorage(const std::array<u8, 0x2E0> &object, Pose &pose) {
    if (Field<s32>(object, 0x208) != -1)
        return false;
    pose.controller = Field<u64>(object, 0x290);
    pose.model = Field<u64>(object, 0x270);
    pose.entry = Field<u64>(object, 0x10);
    std::array<u8, 0x40> hka{};
    if (!ReadAt(pose.controller + 0x20, pose.node) ||
        !Copy(pose.node + 0x38, hka.data(), hka.size()))
        return false;
    // Object initialization 15A1C43 connects this existing matrix provider to the
    // pose node; 1D34460/1D34710 read its mapped model-space bones for the mesh.
    pose.matrix_node = Field<u64>(object, 0x2A0);
    pose.mapping = Field<u64>(object, 0x298);
    // The lever models have 3 or 4 Havok bones (core/vr/lever_models.h); which mapping is a
    // lever's own is checked with its names and rest pose in Bind.
    const s32 count = Field<s32>(hka, 0x10);
    if (count < 3 || count > static_cast<s32>(L::MaxBones) || Field<s32>(hka, 0x20) != count ||
        Field<s32>(hka, 0x30) != count)
        return false;
    pose.count = static_cast<u32>(count);
    std::array<u8, 0x80> matrix{};
    u64 map_data{};
    pose.map_count = 0;
    pose.mesh_map = {};
    // The table is read FLVER -> Havok (1D34460: a FLVER bone index below its count selects the
    // Havok bone it draws from), so its count is the mesh's bone count, not the skeleton's; Bind
    // compares the whole table with the model's.
    if (!Copy(pose.matrix_node, matrix.data(), matrix.size()) ||
        Field<u64>(matrix, 0) != reinterpret_cast<u64>(g_base) + MatrixNodeVtableRva ||
        Field<u64>(matrix, 0x60) != pose.node + 0x18 ||
        Field<u64>(matrix, 0x78) != pose.mapping ||
        !(Field<u8>(matrix, 0xC) & 1) || (pose.matrix_node & 7) ||
        !Writable(pose.matrix_node + 0x10, sizeof(u32)) ||
        !ReadAt(pose.mapping, pose.map_count) || pose.map_count == 0 ||
        pose.map_count > L::MaxMeshBones || !ReadAt(pose.mapping + 8, map_data) ||
        !Copy(map_data, pose.mesh_map.data(), pose.map_count * sizeof(s16)))
        return false;
    pose.skeleton = Field<u64>(hka, 0);
    pose.locals = Field<u64>(hka, 8);
    pose.models = Field<u64>(hka, 0x18);
    pose.flags = Field<u64>(hka, 0x28);
    if ((pose.locals & 15) || (pose.models & 15) || (pose.flags & 3) || (pose.node & 7) ||
        !Writable(pose.locals, pose.count * sizeof(Body::HkQsTransform)) ||
        !Writable(pose.models, pose.count * sizeof(Body::HkQsTransform)) ||
        !Writable(pose.flags, pose.count * sizeof(u32)) || !Writable(pose.node + 0x70, 2))
        return false;
    const auto sync = Field<u16>(hka, 0x38);
    if (sync != 0x0100 && sync != 0x0101)
        return false;
    std::memcpy(pose.frame.data(), object.data() + 0x240, sizeof(pose.frame));
    return true;
}
bool ReadPose(u32 handle, Pose &pose) {
    std::array<u8, 0x2E0> object{};
    pose.handle = handle;
    return Object(handle, pose.object, object) && ReadPoseStorage(object, pose);
}
// A skeleton with a measured model's bones that matches no model for its row (its mesh table, its
// root, its lever's rest or scale differ; or the lever is not at rest, used or mid-clip): logged
// once per model and row a session, with what the game holds, so the first run in the game shows
// whether the matching rules (the FLVER -> Havok table, the bone's own scale) hold there. Bounded:
// one line per listed (model, row) pair, from any thread.
std::array<std::atomic<bool>, L::Models.size() * 3> g_unmatched_logged{};
void LogUnmatched(const L::Skeleton &skeleton, const Pose &pose,
                  const std::array<Body::QsTransform, L::MaxBones> &locals, s32 action) {
    for (size_t m = 0; m < L::Models.size(); ++m) {
        const auto &model = L::Models[m];
        if (!L::SameBones(model, skeleton))
            continue;
        for (size_t a = 0; a < model.actions.size(); ++a) {
            if (model.actions[a] == 0 || model.actions[a] != action ||
                g_unmatched_logged[m * 3 + a].exchange(true, std::memory_order_relaxed))
                continue;
            const auto &lever = locals[model.lever];
            // Every ancestor of the lever bone (the root alone for a lever stick; o353010's root
            // and column), as the game holds it.
            std::string ancestors;
            const auto chain = L::Chain(model);
            for (size_t i = 0; i < chain.n; ++i) {
                const auto &b = locals[chain.idx[i]];
                ancestors += fmt::format("{}{} t=({:.4f},{:.4f},{:.4f}) q=({:.4f},{:.4f},{:.4f},"
                                         "{:.4f}) s=({:.4f},{:.4f},{:.4f})",
                                         i ? "; " : "", model.bones[chain.idx[i]], b.t.x, b.t.y,
                                         b.t.z, b.q.x, b.q.y, b.q.z, b.q.w, b.s.x, b.s.y, b.s.z);
            }
            LOG_INFO(Core,
                     "VR lever: {} bones on row {} do not bind (logged once): mesh table {} "
                     "[{},{},{},{}] (the model's {} [{},{},{},{}]); {}; lever t=({:.4f},"
                     "{:.4f},{:.4f}) q=({:.5f},{:.5f},{:.5f},{:.5f}) s=({:.5f},{:.5f},{:.5f}) (rest "
                     "s=({:.5f},{:.5f},{:.5f}))",
                     model.name, action, pose.map_count, pose.mesh_map[0], pose.mesh_map[1],
                     pose.mesh_map[2], pose.mesh_map[3], model.mesh_count, model.mesh_map[0],
                     model.mesh_map[1], model.mesh_map[2], model.mesh_map[3], ancestors,
                     lever.t.x, lever.t.y, lever.t.z, lever.q.x, lever.q.y, lever.q.z,
                     lever.q.w, lever.s.x, lever.s.y, lever.s.z, model.rest_s.x, model.rest_s.y,
                     model.rest_s.z);
        }
    }
}
// The measured model this object is (core/vr/lever_models.h: names, parents, mesh mapping and
// rest pose all match), bound at its live rest; or the capstan parked here (StillParked: the bar
// as the hand left it), bound at its true rest with its angle; false for anything else, or a
// lever mid-clip. `angle`: where its handle stands (0 at rest). Callers hold the busy guard,
// except motion-only rehearsal's MotionRequest (it never parks a lever, and never reads the record).
bool Bind(const Pose &pose, L::BoneBinding &binding, L::Rig &rig, const L::Model **model = nullptr,
          s32 action = 0, float *angle = nullptr, bool *is_parked = nullptr) {
    u64 bones{}, parents{};
    s32 count{}, parent_count{};
    L::Skeleton skeleton{};
    skeleton.count = pose.count;
    skeleton.mesh_count = pose.map_count;
    skeleton.mesh_map = pose.mesh_map;
    if (pose.count > L::MaxBones || !ReadAt(pose.skeleton + 0x28, bones) ||
        !ReadAt(pose.skeleton + 0x30, count) || count != static_cast<s32>(pose.count) ||
        !ReadAt(pose.skeleton + 0x18, parents) || !ReadAt(pose.skeleton + 0x20, parent_count) ||
        parent_count != count || !Copy(parents, skeleton.parents.data(), pose.count * sizeof(s16)))
        return false;
    for (u32 i = 0; i < pose.count; ++i) {
        u64 name{};
        if (!ReadAt(bones + i * 16, name) ||
            !Copy(name & ~u64{1}, skeleton.names[i].data(), skeleton.names[i].size()))
            return false;
    }
    const auto local = pose.Buffers().locals;
    PiNativeLeverBindFacts facts{};
    facts.count = pose.count;facts.mesh_count = pose.map_count;facts.motion_test = g_motion_test;
    facts.action = action;facts.parked_model = -1;
    std::memcpy(facts.names,skeleton.names.data(),sizeof(facts.names));
    std::memcpy(facts.parents,skeleton.parents.data(),sizeof(facts.parents));
    std::memcpy(facts.mesh_map,skeleton.mesh_map.data(),sizeof(facts.mesh_map));
    std::copy(pose.frame.begin(),pose.frame.end(),facts.rows);
    std::memcpy(facts.locals,local.data(),pose.count * sizeof(Body::HkQsTransform));
    // Motion-only rehearsal never reads the parked record (it runs outside the busy guard).
    if (!g_motion_test) {
        facts.parked_model = PhysicalInteractions::NativeLeverRigPackets::ModelIndex(parked.model);
        facts.parked_object_present = parked.object != 0;
        facts.same_object = parked.object == pose.object;
        facts.same_entry = parked.entry == pose.entry;
        facts.same_handle = parked.handle == pose.handle;
        facts.parked_angle = parked.angle;
        std::memcpy(facts.original,&parked.original,sizeof(facts.original));
        std::memcpy(facts.last,&parked.last,sizeof(facts.last));
    }
    PiNativeLeverRigInput packet{};packet.operation = PI_NLR_BIND;packet.data.bind = facts;
    PiNativeLeverRigOutput reply{};
    if (!VR::Interactions::NativeLeverRig(settle_module,packet,reply)) return false;
    const auto& out = reply.data.bind;
    if (!out.valid) {
        if (out.reason == PI_NLR_UNMATCHED) {
            std::array<Body::QsTransform,L::MaxBones> locals{};
            for (u32 i = 0; i < pose.count; ++i) locals[i] = Body::FromHavok(local[i]);
            LogUnmatched(skeleton,pose,locals,action);
        }
        return false;
    }
    binding = PhysicalInteractions::NativeLeverRigPackets::Unpack(out.binding);
    rig = PhysicalInteractions::NativeLeverRigPackets::Unpack(out.rig);
    if (model) *model = &L::Models[out.model_index];
    if (angle) *angle = out.angle;
    if (is_parked) *is_parked = out.is_parked != 0;
    return true;
}
bool SamePose(const Pose &a, const Pose &b) {
    if (a.object != b.object || a.controller != b.controller || a.node != b.node ||
        a.skeleton != b.skeleton || a.model != b.model || a.locals != b.locals ||
        a.models != b.models || a.flags != b.flags || a.entry != b.entry ||
        a.matrix_node != b.matrix_node || a.mapping != b.mapping || a.handle != b.handle ||
        a.count != b.count || a.map_count != b.map_count || a.mesh_map != b.mesh_map)
        return false;
    for (size_t i = 0; i < a.frame.size(); ++i)
        if (!std::isfinite(b.frame[i]) || std::abs(a.frame[i] - b.frame[i]) > 0.0001f)
            return false;
    return true;
}
bool Row(u64 row, u64 block, u64 &param, u32 &handle, s32 &state, s32 *id = nullptr) {
    std::array<u8, 0x140> action{};
    std::array<u8, 0x30> values{};
    u64 rows{};
    s32 count{};
    if (!ReadAt(block + 0x90, rows) || !ReadAt(block + 0x98, count) || count <= 0 ||
        count > 65536 || row < rows || (row - rows) % 0x140 ||
        (row - rows) / 0x140 >= static_cast<u32>(count) ||
        !Copy(row, action.data(), action.size()) || !PhysicalLeverAction(Field<s32>(action, 0x80)))
        return false;
    param = Field<u64>(action, 0x88);
    handle = Field<u32>(action, 0x58);
    state = Field<s32>(action, 0x5C);
    if (id)
        *id = Field<s32>(action, 0x80);
    return Copy(param, values.data(), values.size()) && OperateAnimation(Field<s32>(values, 0x0C));
}
bool Permitted(u64 row, u64 actor) {
    u64 controller{}, vtable{}, getter{}, actor_vtable{};
    if (!ReadAt(row + 0x90, controller) || !ReadAt(controller, vtable) ||
        !ReadAt(vtable + 0x58, getter) || !Hook::Plausible(reinterpret_cast<u8 *>(getter)) ||
        !ReadAt(actor, actor_vtable) || !Hook::Plausible(reinterpret_cast<u8 *>(actor_vtable)))
        return false;
    using Check = bool(PS4_SYSV_ABI *)(u64, u64);
    return reinterpret_cast<Check>(g_base + CheckRva)(row, actor);
}
void PublishOffer(s64 now) {
    offer.ns = now;
    L::Channel().PublishOffer(
        {now, {true, offer.handle, offer.generation, 0, 0, offer.rig, offer.angle}, offer_request});
}
void ClearOffer() {
    L::Channel().PublishOffer({});
    L::Channel().held_object.store(0, std::memory_order_release);
    offer = {};
}

// Per-instance prompt replacement (core/vr/lever_prompt.h), host side of the native gate: the
// demotions live in the DLL; copied equality facts and native slot handles use the busy guard.
// The armed lever: the bound lever the game selected last. A squeeze may reach for it while the
// player stays near it (the XR side's proximity rule, lever_prompt.h ArmMargin), after the hunter
// has left the game's prompt zone (the handle lies outside it); served from the scheduler hook.
// It is disarmed when its area or actor changes (cleared), and not armed while it is used,
// running, animating or physical levers are off (kept, re-checked while the player is near).
struct RecentLever {
    u64 row{}, block{}, actor{}, actor_vtable{}, blocks{}, rows{};
    u32 handle{}, actor_handle{};
    s32 block_count{}, row_count{}, state{};
    s64 ns{};
    L::Rig rig{};
    bool armed{};
    float angle{}; // where its handle stands (a parked capstan's bar)
};
RecentLever recent;
// The action-block array and this block's rows as the game has them now; the actor identity.
bool LiveIdentity(u64 row, u64 block, u64 actor, RecentLever &out) {
    u64 manager{};
    out = {};
    if (!ReadAt(reinterpret_cast<u64>(g_base) + ObjActManRva, manager) ||
        !ReadAt(manager + 0x20, out.block_count) || out.block_count <= 0 ||
        out.block_count > 256 || !ReadAt(manager + 0x28, out.blocks) || block < out.blocks ||
        (block - out.blocks) % 0xD8 ||
        (block - out.blocks) / 0xD8 >= static_cast<u64>(out.block_count) ||
        !ReadAt(block + 0x90, out.rows) || !ReadAt(block + 0x98, out.row_count) ||
        out.row_count <= 0 || out.row_count > 65536 || row < out.rows ||
        (row - out.rows) % 0x140 || (row - out.rows) / 0x140 >= static_cast<u64>(out.row_count) ||
        !ReadAt(actor, out.actor_vtable) || !Hook::Plausible(reinterpret_cast<u8 *>(out.actor_vtable)) ||
        !ReadAt(actor + 8, out.actor_handle) || out.actor_handle == 0xFFFFFFFF ||
        !ReadAt(row + 0x58, out.handle) || !ReadAt(row + 0x5C, out.state))
        return false;
    out.row = row;
    out.block = block;
    out.actor = actor;
    return true;
}
bool Selectable(s32 state) { return state != 3 && state != 4; } // 13AE09A skips [Run] and [End]

// A refused physical attempt brings this lever's normal prompt and button back for a while.
// Callers hold the guard. Any thread: the verdict itself is only ever written on the selection
// thread, so the change reaches it through the prompt serial the native gate compares.
void Demote(u64 row, u32 handle, s64 until) {
    auto &channel = L::Channel();
    prompt_policy.Demote(row, handle, until);
    channel.PromptChanged(); // next selection pass reclassifies (and keeps the prompt)
}

// Native gate's host classifier: called on a different selected row/object, a B fallback, a
// prompt change or a due recheck. Hides the prompt only when this exact instance's rig binds, a
// hand can pull it now and the game's tables hold it (or while it is held).
void Classify(u64 row, u64 actor, u64 block) {
    auto &channel = L::Channel();
    auto &verdict = channel.verdict;
    u32 handle = 0xFFFFFFFF;
    ReadAt(row + 0x58, handle);
    const auto fallback = channel.prompt_fallback.load(std::memory_order_acquire);
    const auto serial = channel.prompt_serial.load(std::memory_order_acquire);
    if (!NeedsPromptClassify(verdict, row, handle, fallback, serial))
        return;
    Guard guard;
    if (!guard.acquired) {
        if (verdict.row == row && verdict.handle == handle)
            verdict.recheck = 1; // the scheduler owns the lever state now: ask again next pass
        return;
    }
    const auto ready = channel.hand_pull_ready.load(std::memory_order_acquire);
    const auto now = NowNs();
    const bool held = (edit.Owned() || channel.work.load(std::memory_order_acquire)) &&
                      offer.row == row && offer.handle == handle;
    u64 param{};
    u32 object{};
    s32 state{};
    Pose pose{};
    L::BoneBinding binding{};
    L::Rig rig{};
    RecentLever seen{};
    const L::Model *model = nullptr;
    const char *why = nullptr;
    s32 action{};
    float angle{};
    ReadAt(row + 0x80, action);
    if (!L::Enabled())
        why = "physical levers off";
    else if (!Row(row, block, param, object, state, &action))
        why = "not a listed lever row";
    else if (!ReadPose(object, pose))
        why = "object pose unavailable or animating";
    // (A held lever is off its rest by design: its verdict stays bound without a bind, and no
    // "does not bind" line is logged for a hand's own pull.)
    else if (!held && !Bind(pose, binding, rig, &model, action, &angle))
        why = "no rig measured for this model and row, or not at rest";
    else if (!PromptReady(ready, now))
        why = "no hand can pull it now: virtual screen, third person, or body or hands untracked";
    else if (!LiveIdentity(row, block, actor, seen))
        why = "not found in the game's action tables, so it cannot be armed";
    const bool binds = why == nullptr;
    PiNativeLeverPromptInput prompt{};
    prompt.operation = PI_NLP_DECIDE;prompt.now_ns = now;prompt.handle = handle;
    prompt.enabled = L::Enabled();prompt.binds = binds;prompt.held = held;
    prompt.fallback = fallback;prompt.serial = serial;prompt.previous_fallback = verdict.fallback;
    prompt.ready_present = ready != 0;prompt.motion_test = g_motion_test;
    prompt.parked_match = parked.model && parked.handle == handle;
    const auto next = prompt_policy.Call(prompt,row);
    if (prompt_policy.Faulted()) { verdict.bound = 0;return; }
    // The capstan the hand parked, whose hand pull is over for now: its prompt given back (B, a
    // refused pull), a display mode no hand pulls in (the virtual screen, third person: the XR
    // side cleared its word, not merely late; it asks itself as it clears it) or physical levers
    // off. The game holds it unturned and its own button plays its clip from rest: its scheduler
    // puts the bar back at rest first (Unpark), so flat play sees it as the game does. A word
    // merely late (a tracking gap, the headset off) keeps it parked for the hand to carry on.
    if (next.request_unpark)
        channel.RequestUnpark(now);
    if (next.bound && !held) {
        // binds: LiveIdentity filled `seen`. A hidden prompt always has an armed lever behind it.
        seen.ns = now;
        seen.rig = rig;
        seen.armed = true;
        seen.angle = angle;
        if (recent.row != seen.row || recent.handle != seen.handle || !recent.armed)
            LOG_INFO(Core, "VR lever: handle={:#x} armed: grabbable while you stay within "
                           "{:.1f} m of its hinge",
                     seen.handle, L::ArmMargin + Body::Length(rig.rest));
        recent = seen;
        // Where its handle is: arming, the reach tick and log (refreshed with each recheck).
        channel.PublishNearby({now, object, rig, true, angle});
    }
    if (next.bound != verdict.bound || row != verdict.row || handle != verdict.handle) {
        // Once per change of the selected lever or its verdict, never per frame.
        if (next.bound)
            LOG_INFO(Core, "VR lever: handle={:#x} row {} ({}): pulled by hand, prompt hidden",
                     handle, action, held ? "held" : model ? model->name : "?");
        else
            LOG_INFO(Core, "VR lever: handle={:#x} row {}: normal prompt kept ({})", handle, action,
                     why ? why
                     : next.demoted
                         ? "B or a refused pull: for now"
                         : "off");
    }
    verdict.handle = handle;
    verdict.recheck = static_cast<uint16_t>(next.recheck);
    verdict.bound = static_cast<uint8_t>(next.bound);
    verdict.fallback = fallback;
    verdict.serial = serial;
    verdict.row = row;
}

void Selected(u64 row, u64 actor, u64 block, const char *path = "selected") {
    if (!g_runtime_enabled)
        return;
    Guard guard;
    if (!guard.acquired || edit.Owned() || L::Channel().work.load(std::memory_order_acquire))
        return;
    const auto request = L::Channel().acquire.exchange(0, std::memory_order_acq_rel);
    if (!request)
        return;
    const auto now = NowNs();
    PiNativeLeverServiceFacts service{};
    service.kind = PI_NLS_SELECTED;
    service.requested = 1;
    service.request_ns = request;
    service.now_ns = now;
    PiNativeLeverServiceResult intent{};
    if (!VR::Interactions::NativeLeverService(settle_module, service, intent) ||
        !intent.allowed)
      return;
    offer_request = request;
    Offer next{};
    next.row = row;
    next.actor = actor;
    next.block = block;
    s32 action{};
    u32 instance = 0xFFFFFFFF;
    ReadAt(row + 0x58, instance);
    PiNativeLeverOfferFacts facts{};
    facts.now_ns = now;
    facts.phase = PI_NLO_BEGIN;
    facts.motion_test = g_motion_test;
    facts.have_instance = instance != 0xFFFFFFFF;
    PiNativeLeverOfferResult resolution{};
    bool resolved = false;
    // At most six read-only native queries; no unbounded retry or guest wait.
    for (int step = 0; step < 8; ++step) {
        if (!VR::Interactions::NativeLeverOffer(settle_module, facts, resolution))
            break;
        if (resolution.reason || resolution.accepted) {
            resolved = true;
            break;
        }
        facts.phase = resolution.next;
        switch (resolution.next) {
        case PI_NLO_DEMOTION:
            facts.success = !prompt_policy.Demoted(row, instance, now);
            if (prompt_policy.Faulted()) break;
            continue;
        case PI_NLO_ROW:
            facts.success = Row(row, block, next.param, next.handle, next.state, &action);
            facts.state = next.state;
            continue;
        case PI_NLO_POSE:
            facts.success = ReadPose(next.handle, next.pose);
            continue;
        case PI_NLO_RIG:
            facts.success = Bind(next.pose, next.binding, next.rig, &next.model, action,
                                 &next.angle, &next.parked);
            continue;
        case PI_NLO_PERMISSION:
            facts.success = Permitted(row, actor);
            continue;
        case PI_NLO_ACTOR:
            facts.success = ReadAt(actor + 8, next.actor_handle) &&
                            next.actor_handle != 0xFFFFFFFF;
            facts.same_offer = offer.row == row && offer.actor == actor &&
                offer.param == next.param && offer.handle == next.handle &&
                offer.actor_handle == next.actor_handle && offer.state == next.state &&
                SamePose(offer.pose, next.pose);
            continue;
        default: break;
        }
        break;
    }
    if (!resolved) {
        resolution = {};
        resolution.reason = PI_NLO_MODULE;
        if (settle_module) settle_module->ReportFault();
    }
    static constexpr const char* Rejected[] = {
        nullptr,
        "its normal prompt and button are back for now (B or a refused pull)",
        "unsupported or stale action row", "action already running or used",
        "object pose unavailable or animating", "unsupported rig or non-rest pose",
        "game permission denied", "actor unavailable", "interaction module unavailable"
    };
    const char* rejected = Rejected[resolution.reason];
    next.action = action;
    if (rejected) {
        // One message per requested squeeze, never an idle scan or frame log.
        LOG_INFO(Core, "VR lever development: request={} ({}) rejected: {} handle={:#x} state={}",
                 request, path, rejected, instance, next.state);
        if (resolution.demote)
            Demote(row, instance, resolution.until_ns);
        ClearOffer();
        L::Channel().PublishOffer({now, {}, request});
        return;
    }
    next.generation = resolution.reuse_generation ? offer.generation : ++generation;
    offer = next;
    PublishOffer(now);
    // One line per squeeze: the XR side's matching "resolution" line says whether the hand
    // reached this grip.
    const auto grip = L::HandleAt(offer.rig, offer.angle);
    LOG_INFO(Core,
             "VR lever development: offer request={} ({}) handle={:#x} generation={} model={} "
             "pivot=({:.3f},{:.3f},{:.3f}) grip=({:.3f},{:.3f},{:.3f}){}",
             request, path, offer.handle, offer.generation, offer.model ? offer.model->name : "?",
             offer.rig.pivot.x, offer.rig.pivot.y, offer.rig.pivot.z, grip.x, grip.y, grip.z,
             offer.parked ? fmt::format(" parked at {:.1f} degrees", offer.angle / Body::Deg)
                          : std::string{});
}

RecentLever ArmedLever() {
    Guard guard; // Classify (selection thread) writes it under the same guard
    return guard.acquired ? recent : RecentLever{};
}
// The armed lever's own action block, rows and object, and the actor, as the game has them now:
// unchanged since selection. Its block must still be a slot of the game's block array, holding
// the same rows; other blocks streaming in or out (the array's count) do not matter.
bool StillInPlace(const RecentLever &seen) {
    RecentLever live{};
    return LiveIdentity(seen.row, seen.block, seen.actor, live) && live.rows == seen.rows &&
           live.row_count == seen.row_count && live.handle == seen.handle &&
           live.actor_vtable == seen.actor_vtable && live.actor_handle == seen.actor_handle;
}
// A squeeze for the armed lever the scheduler will not serve gets its answer at once (not
// eligible), so the XR side returns it to ordinary controls instead of waiting it out.
void Unserved(s64 request, const char *why) {
    Guard guard;
    auto expected = request;
    if (!guard.acquired ||
        !L::Channel().acquire.compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
        return; // busy (the next tick decides) or already answered
    L::Channel().PublishOffer({NowNs(), {}, request});
    LOG_INFO(Core, "VR lever development: request={} (armed) not served: {}", request, why);
}
// Nearby has one writer at a time: every publication happens under the busy guard.
void DisarmLocked(const RecentLever &seen, const char *why) { // the caller holds the guard
    if (recent.row != seen.row || recent.ns != seen.ns)
        return; // reselected meanwhile: that publication stands
    recent = {};
    L::Channel().PublishNearby({});
    LOG_INFO(Core, "VR lever: handle={:#x} disarmed: {}", seen.handle, why);
}
void Disarm(const RecentLever &seen, const char *why) {
    Guard guard;
    if (guard.acquired) // busy: the next check decides
        DisarmLocked(seen, why);
}

// Scheduler-hook service of a squeeze for the armed lever when the game does not have it selected
// in this frame: the hand reaches the handle from beside or behind the housing, outside the
// game's prompt zone (the XR side asks only while a squeezing hand reaches for it). Identity is
// re-read from the game's own tables (the same action block, rows and actor as when it was
// selected) and the game's interaction gate is asked as its selector does every frame (no menu,
// dialog, talk or event), before the ordinary resolver re-checks the row, state, a demotion, the
// rig at rest and the game's permission.
void ServiceArmed() {
    auto &channel = L::Channel();
    const auto request = channel.acquire.load(std::memory_order_acquire);
    if (!request)
      return;
    PiNativeLeverServiceFacts service{};
    service.kind = PI_NLS_ARMED_GATE;
    service.requested = 1;
    service.request_ns = request;
    service.motion_test = g_motion_test;
    service.enabled = L::Enabled();
    PiNativeLeverServiceResult intent{};
    if (!VR::Interactions::NativeLeverService(settle_module, service, intent) ||
        !intent.allowed)
      return;
    const auto seen = ArmedLever();
    service.kind = PI_NLS_ARMED_READY;
    service.have_row = seen.row != 0;
    service.now_ns = NowNs();
    if (!VR::Interactions::NativeLeverService(settle_module, service, intent) ||
        !intent.allowed)
      return;
    if (!StillInPlace(seen)) {
        Disarm(seen, "its area or the player changed");
        Unserved(request, "its area or the player changed");
        return;
    }
    // The selector selects nothing while any of these bits is set (13AE034). Not a refusal of the
    // lever: no demotion, the squeeze just keeps its ordinary use.
    if (InteractionBlocked(SelectBlocked)) {
        Unserved(request, "the game blocks interaction now (a menu, dialog, talk or event)");
        return;
    }
    Selected(seen.row, seen.actor, seen.block, "armed");
}

// The actor 13ADCC0 runs its selection and permission checks for (its r15 at 13AE171/13AE195):
// [[553E880]+0x108] when set, else the main player (WorldChrMan +0x60); 0 when neither reads.
u64 SelectionActor() {
    u64 slot{}, actor{}, world{};
    if (ReadAt(reinterpret_cast<u64>(g_base) + SelectionActorRva, slot) && slot &&
        ReadAt(slot + 0x108, actor) && actor)
        return actor;
    if (ReadAt(reinterpret_cast<u64>(g_base) + WorldChrManRva, world) && world &&
        ReadAt(world + 0x60, actor))
        return actor;
    return 0;
}

// Proximity arming (lever_prompt.h ScanNs). The XR side asks a few times a second, only while a
// hand can pull and no armed lever is near, with where the head and hands are (game units). The
// game's prompt zone stands the hunter ~1.9 m in front of the hinge, where the handle is out of
// reach; a lever must not need the game to have selected it before a hand can take it. Every
// listed lever row in the game's own action tables whose object lies near is classified as the
// selection's classifier does: a listed row with an Operate Device param, unused, its normal
// prompt not given back for now (B or a refused pull), its object's pose readable and not
// animating, and a measured rig that binds at rest for this model and row. The nearest such lever
// the head or a hand is within its arm radius of becomes the armed lever, as a selected one does.
// No permission call and no write: a squeeze for it still goes through ServiceArmed (identity,
// the game's interaction gate, then the resolver's row, state, rig and the game's permission).
// A bound lever near but not yet within reach gets a note at most every ScanNoteNs: how far the
// head or a hand is from its hinge (a log then says why a lever did not arm).
constexpr u32 ScanMaxRows = 8192;       // a bound on one scan's work, whatever the tables say
constexpr s64 ScanNoteNs = 2'000'000'000;
s64 scan_noted_ns{};
void Scan() {
    auto &channel = L::Channel();
    const auto request = channel.scan.exchange(0, std::memory_order_acq_rel);
    if (!request) return; // No transport request to service.
    const auto now = NowNs();
    PiNativeLeverGeometryFacts geometry{};
    geometry.request_ns = request;geometry.now_ns = now;
    geometry.enabled = L::Enabled();geometry.motion_test = g_motion_test;
    PiNativeLeverGeometryResult decision{};
    if (!VR::Interactions::NativeLeverGeometry(settle_module,PI_NLR_SCAN_GATE,geometry,decision) ||
        !decision.allowed) return;
    L::ScanRequest where{};
    geometry.requested = channel.RequestedScan(where);geometry.where_ns = where.ns;
    geometry.ready_ns = channel.hand_pull_ready.load(std::memory_order_acquire);
    geometry.scale = where.scale;
    geometry.head[0] = where.head.x;geometry.head[1] = where.head.y;geometry.head[2] = where.head.z;
    for (int h = 0; h < 2; ++h) {
        geometry.tracked[h] = where.tracked[h];
        geometry.hands[h][0] = where.hands[h].x;geometry.hands[h][1] = where.hands[h].y;
        geometry.hands[h][2] = where.hands[h].z;
    }
    if (!VR::Interactions::NativeLeverGeometry(settle_module,PI_NLR_SCAN_BEGIN,geometry,decision) ||
        !decision.allowed) return;
    Guard guard;
    if (!guard.acquired || edit.Owned() || channel.work.load(std::memory_order_acquire))
        return; // busy, or a hold or its cleanup owns the lever state: the next scan decides
    const u64 actor = SelectionActor();
    u64 manager{}, blocks{};
    s32 block_count{};
    if (!actor || !ReadAt(reinterpret_cast<u64>(g_base) + ObjActManRva, manager) ||
        !ReadAt(manager + 0x20, block_count) || block_count <= 0 || block_count > 256 ||
        !ReadAt(manager + 0x28, blocks))
        return;
    struct Found {
        u64 row{}, block{};
        u32 handle{};
        s32 action{};
        float distance{std::numeric_limits<float>::infinity()};
        L::Rig rig{}; // game units
        const L::Model *model{};
        float angle{}; // where its handle stands (a parked capstan's bar)
        float radius{}; // approved diagnostic snapshot, metres
    } best, nearest; // the nearest within its arm radius; the nearest at all (the note)
    u32 inspected = 0;
    constexpr s32 Chunk = 32; // rows read per copy: a block's table in a few reads
    std::array<u8, Chunk * 0x140> chunk{};
    for (s32 b = 0; b < block_count && inspected < ScanMaxRows; ++b) {
        const u64 block = blocks + static_cast<u64>(b) * 0xD8;
        u64 rows{};
        s32 row_count{};
        if (!ReadAt(block + 0x90, rows) || !ReadAt(block + 0x98, row_count) || row_count <= 0 ||
            row_count > 65536)
            continue;
        for (s32 first = 0; first < row_count && inspected < ScanMaxRows; first += Chunk) {
            const s32 n = std::min(Chunk, row_count - first);
            inspected += static_cast<u32>(n);
            if (!Copy(rows + static_cast<u64>(first) * 0x140, chunk.data(),
                      static_cast<size_t>(n) * 0x140))
                break;
            for (s32 i = 0; i < n; ++i) {
                if (!PhysicalLeverAction(
                        Field<s32>(chunk, static_cast<size_t>(i) * 0x140 + 0x80)))
                    continue;
                const u64 row = rows + static_cast<u64>(first + i) * 0x140;
                u64 param{};
                u32 handle{};
                s32 state{}, action{};
                if (!Row(row, block, param, handle, state, &action) || !Selectable(state) ||
                    prompt_policy.Demoted(row, handle, now))
                    continue;
                Pose pose{};
                std::array<u8, 0x2E0> object{};
                if (!Object(handle, pose.object, object))
                    continue;
                // Far objects are skipped before their skeleton is read.
                geometry.object[0] = Field<float>(object,0x24C);
                geometry.object[1] = Field<float>(object,0x25C);
                geometry.object[2] = Field<float>(object,0x26C);
                if (!VR::Interactions::NativeLeverGeometry(settle_module,PI_NLR_SCAN_OBJECT,geometry,decision)) return;
                if (!decision.allowed) continue;
                L::BoneBinding binding{};
                L::Rig rig{};
                const L::Model *model = nullptr;
                float angle{};
                pose.handle = handle;
                if (!ReadPoseStorage(object, pose) ||
                    !Bind(pose, binding, rig, &model, action, &angle))
                    continue;
                geometry.rig = PhysicalInteractions::NativeLeverRigPackets::Pack(rig);
                geometry.best_valid = best.row != 0;geometry.nearest_valid = nearest.row != 0;
                geometry.best_distance = geometry.best_valid ? best.distance : 0.f;
                geometry.nearest_distance = geometry.nearest_valid ? nearest.distance : 0.f;
                if (!VR::Interactions::NativeLeverGeometry(settle_module,PI_NLR_SCAN_CANDIDATE,geometry,decision)) return;
                if (decision.replace_best)
                    best = {row,block,handle,action,decision.reach,rig,model,angle,decision.radius};
                if (decision.replace_nearest)
                    nearest = {row,block,handle,action,decision.distance,rig,model,angle,decision.radius};
            }
        }
    }
    if (!best.row && nearest.row && (scan_noted_ns == 0 || now - scan_noted_ns >= ScanNoteNs)) {
        scan_noted_ns = now;
        LOG_INFO(Core,
                 "VR lever scan: handle={:#x} row {} ({}) is the nearest hand-pulled lever, {:.2f} m "
                 "from the head or a hand (arms within {:.2f}) | head game ({:.2f},{:.2f},{:.2f})",
                 nearest.handle, nearest.action, nearest.model ? nearest.model->name : "?",
                 nearest.distance, nearest.radius, where.head.x,
                 where.head.y, where.head.z);
    }
    RecentLever seen{};
    if (!best.row || !LiveIdentity(best.row, best.block, actor, seen))
        return;
    seen.ns = now;
    seen.rig = best.rig;
    seen.armed = true;
    seen.angle = best.angle;
    if (recent.row != seen.row || recent.handle != seen.handle || !recent.armed)
        LOG_INFO(Core,
                 "VR lever: handle={:#x} row {} ({}): armed by proximity, {:.2f} m from its "
                 "hinge, without the game selecting it: grabbable while you stay within {:.1f} m",
                 seen.handle, best.action, best.model ? best.model->name : "?", best.distance,
                 best.radius);
    recent = seen;
    channel.PublishNearby({now, seen.handle, seen.rig, true, seen.angle});
}

// Twice a second while the player is near the armed lever (the XR side's request): is it still in
// place, selectable and at rest (or parked by the hand)? Republishes it (armed or not) or disarms
// it; never a grab. All of it under the busy guard: Bind reads the parked capstan's record, which
// a hold's cleanup writes (a busy guard skips this check; the XR side asks again).
void CheckArmed() {
    auto &channel = L::Channel();
    const bool requested =
        channel.nearby_check.exchange(0, std::memory_order_acq_rel) != 0;
    if (!requested)
      return;
    Guard guard;
    if (!guard.acquired)
      return;
    PiNativeLeverServiceFacts service{};
    service.kind = PI_NLS_NEARBY;
    service.requested = requested;
    service.motion_test = g_motion_test;
    service.owned = edit.Owned();
    service.work = channel.work.load(std::memory_order_acquire) != 0;
    service.enabled = L::Enabled();
    PiNativeLeverServiceResult intent{};
    if (!VR::Interactions::NativeLeverService(settle_module, service, intent) ||
        !intent.allowed)
      return;
    const auto seen = recent;
    if (!seen.row) {
        channel.PublishNearby({});
        return;
    }
    if (!StillInPlace(seen)) {
        DisarmLocked(seen, "its area or the player changed");
        return;
    }
    u64 param{};
    u32 handle{};
    s32 state{}, action{};
    Pose pose{};
    L::BoneBinding binding{};
    L::Rig rig{};
    float angle{};
    const char *why = nullptr;
    if (intent.disabled)
      why = "physical levers off";
    else if (!Row(seen.row, seen.block, param, handle, state, &action))
        why = "no longer a lever row";
    else if (!Selectable(state))
        why = "used or running";
    else if (!ReadPose(handle, pose))
        why = "animating";
    else if (!Bind(pose, binding, rig, nullptr, action, &angle))
        why = "not at rest";
    const auto now = NowNs();
    // B or a refused pull gave it back to the game's button: no hand pull meanwhile, so the
    // game's own pull never meets a held lever.
    if (!why && prompt_policy.Demoted(seen.row, seen.handle, now))
        why = "its normal prompt and button are back for now";
    const bool armed = why == nullptr;
    if (recent.armed != armed)
        LOG_INFO(Core, "VR lever: handle={:#x} {}", seen.handle,
                 armed ? "armed again" : fmt::format("not grabbable now ({})", why));
    recent.armed = armed;
    if (armed) {
        recent.rig = rig;
        recent.angle = angle;
    }
    channel.PublishNearby({now, recent.handle, recent.rig, armed, recent.angle});
}
// Temporary rehearsal only: one bounded lookup on a fresh squeeze, independent of
// the game's selected prompt and row state. Never changes action state or permission.
void MotionRequest() {
    auto &channel = L::Channel();
    const auto request = channel.acquire.load(std::memory_order_acquire);
    if (!request)
      return;
    PiNativeLeverServiceFacts service{};
    service.kind = PI_NLS_MOTION_GATE;
    service.requested = 1;
    service.request_ns = request;
    service.motion_test = g_motion_test;
    service.work = channel.work.load(std::memory_order_acquire) != 0;
    PiNativeLeverServiceResult intent{};
    if (!VR::Interactions::NativeLeverService(settle_module, service, intent) ||
        !intent.allowed)
      return;
    L::ProbeHands hands{};
    u64 manager{}, blocks{}, world{}, actor{};
    s32 count{};
    u64 best_row{}, best_block{};
    float best = std::numeric_limits<float>::max();
    float nearest = best;
    u32 matching{}, posed{}, bound{};
    bool probe_failed{};
    service.kind = PI_NLS_MOTION_DATA;
    service.now_ns = NowNs();
    service.have_snapshot = channel.RequestedHands(hands);
    service.sample_ns = hands.ns;
    service.radius = hands.radius;
    const bool accepted =
        VR::Interactions::NativeLeverService(settle_module, service, intent) &&
        intent.allowed;
    if (accepted &&
        ReadAt(reinterpret_cast<u64>(g_base) + WorldChrManRva, world) &&
        ReadAt(world + 0x60, actor) &&
        ReadAt(reinterpret_cast<u64>(g_base) + ObjActManRva, manager) &&
        ReadAt(manager + 0x20, count) && count > 0 && count <= 256 &&
        ReadAt(manager + 0x28, blocks)) {
      u32 inspected{};
      for (s32 b = 0; b < count && !probe_failed; ++b) {
        const u64 block = blocks + static_cast<u64>(b) * 0xD8;
        u64 rows{};
        s32 rows_count{};
        if (!ReadAt(block + 0x90, rows) || !ReadAt(block + 0x98, rows_count) ||
            rows_count < 0 || rows_count > 65536)
          continue;
        for (s32 r = 0; r < rows_count && inspected < 65536 && !probe_failed;
             ++r, ++inspected) {
          const u64 row = rows + static_cast<u64>(r) * 0x140;
          s32 type{};
          if (!ReadAt(row + 0x80, type) || !PhysicalLeverAction(type))
            continue;
          u64 param{};
          u32 handle{};
          s32 state{};
          Pose pose{};
          L::BoneBinding binding{};
          L::Rig rig{};
          ++matching;
          if (!Row(row, block, param, handle, state) || !ReadPose(handle, pose))
            continue;
          ++posed;
          if (!Bind(pose, binding, rig, nullptr, type))
            continue;
          ++bound;
          PiNativeLeverProbeFacts probe{};
          probe.rig = PhysicalInteractions::NativeLeverRigPackets::Pack(rig);
          probe.radius = hands.radius;
          probe.best_distance = best;
          probe.nearest_distance = nearest;
          for (int h = 0; h < 2; ++h) {
            probe.enabled[h] = hands.enabled[h];
            probe.hands[h][0] = hands.world[h].x;
            probe.hands[h][1] = hands.world[h].y;
            probe.hands[h][2] = hands.world[h].z;
          }
          PiNativeLeverProbeResult choice{};
          if (!VR::Interactions::NativeLeverProbe(settle_module, probe,
                                                  choice)) {
            best_row = 0;
            probe_failed = true;
            break;
          }
          if (choice.replace_nearest)
            nearest = choice.nearest_distance;
          if (choice.replace_best) {
            best = choice.best_distance;
            best_row = row;
            best_block = block;
          }
        }
      }
    }
    auto expected = request;
    if (best_row) {
        Selected(best_row, actor, best_block);
    } else if (channel.acquire.compare_exchange_strong(expected, 0,
                                                       std::memory_order_acq_rel)) {
        channel.PublishOffer({NowNs(), {}, request});
        LOG_INFO(Core, "VR lever development: motion request={} rejected rows={} poses={} rigs={} "
                       "nearest={} radius={} hands=({}, {}) L=({},{},{}) R=({},{},{})",
                 request, matching, posed, bound, nearest, hands.radius,
                 hands.enabled[0], hands.enabled[1],
                 hands.world[0].x, hands.world[0].y, hands.world[0].z,
                 hands.world[1].x, hands.world[1].y, hands.world[1].z);
    }
}
void Retire(const L::Command &command) {
    if (command.kind == L::CommandKind::Complete && reported_completion != command.serial)
        ReportCompletion(command, false);
    edit.Forget();
    settling.Clear();
    ClearOffer();
    L::Channel().Retire(command.serial);
}
// A completed pull of a lever whose own clip stays at its stop (lever_models.h end_deg: the game
// shows it pulled from now on, and its script puts it there after a reload): the bone stays where
// the hand left it, at the stop. Ownership ends without restoring the rest pose (nothing animates
// the idle object; the clip was not replayed), and the offer retires.
void StayPulled(const L::Command &command) {
    LOG_INFO(Core, "VR lever development: handle={:#x} ({}) stays pulled, as its own clip leaves it",
             command.id, offer.model ? offer.model->name : "?");
    Retire(command);
}
// A capstan let go short of its stop, or one the game refused (lever_models.h Parked): its bar
// stays where the hand left it, never turned back. Ownership ends without restoring the rest pose
// (nothing animates the idle object), the bone is recorded byte for byte, and the offer retires;
// the lever still binds there (Bind: StillParked), so its prompt stays replaced and a new squeeze
// carries on from this angle. The armed lever's word now says where its bar is.
void Park(const L::Command &command, s64 now, const char *why) {
    // (A request made while the hold ran, as physical levers went off or the display mode left
    // the hand pull, stands: Unpark serves it now, and this bar goes back to rest.)
    parked = {offer.pose.object, offer.pose.entry, offer.handle, offer.model,
              edit.Original(), edit.Last(), applied_angle, offer.action};
    LOG_INFO(Core, "VR lever development: handle={:#x} ({}) parked at {:.1f} of {:.1f} degrees "
                   "({}): it stays there; a squeeze turns it on from there",
             command.id, offer.model ? offer.model->name : "?", applied_angle / Body::Deg,
             offer.binding.sweep / Body::Deg, why);
    if (recent.row == offer.row && recent.handle == offer.handle) {
        recent.angle = applied_angle;
        L::Channel().PublishNearby({now, recent.handle, recent.rig, recent.armed, recent.angle});
    }
    Retire(command);
}
// The game ended a hold the hand still had: why, once per hold (the XR side's end line follows).
u64 dropped_generation{};
void Dropped(const L::Command &command, const char *why) {
    if (command.kind != L::CommandKind::Hold || dropped_generation == command.generation)
        return;
    dropped_generation = command.generation;
    LOG_INFO(Core, "VR lever hold dropped by the game: handle={:#x} generation={} angle={:.3f}: {}",
             command.id, command.generation, command.angle, why);
}
// The scheduler queue `queue` that runs the object at `pose.entry` (the object `pose.object`, a
// slot of `objects`), as the game's own model refresh reads it: its slot index, capacity, index
// list, pending count and the entry's flags. Unreadable: the queue or the entry cannot be read;
// Mismatch: they disagree with the object or are not writable.
enum class Slot { Ok, Unreadable, Mismatch };
Slot QueueSlot(u64 queue, u64 objects, const Pose &pose, std::optional<L::UpdateQueue> &out) {
    std::array<u8, 0x20> q{};
    u16 index{};
    if (!Copy(queue, q.data(), q.size()) || !ReadAt(pose.entry + 8, index))
        return Slot::Unreadable;
    const auto capacity = Field<u32>(q, 0);
    const auto entries = Field<u64>(q, 8);
    const auto indices = Field<u64>(q, 0x18);
    if (capacity == 0 || capacity > 65536 || index >= capacity ||
        pose.entry != entries + static_cast<u64>(index) * 16 ||
        pose.object != objects + static_cast<u64>(index) * 0x2E0 || !Writable(queue + 0x10, 4) ||
        !Writable(pose.entry + 0xA, 4) || !Writable(indices, capacity * sizeof(u16)))
        return Slot::Mismatch;
    out.emplace(L::UpdateQueue{index,
                               capacity,
                               {reinterpret_cast<u16 *>(indices), capacity},
                               *reinterpret_cast<u32 *>(queue + 0x10),
                               *reinterpret_cast<u16 *>(pose.entry + 0xA),
                               *reinterpret_cast<u16 *>(pose.entry + 0xC)});
    return Slot::Ok;
}
// After a write to the lever bone's locals: sync the pose and queue its matrix provider.
void PublishPose(const Pose &pose) {
    using Sync = void(PS4_SYSV_ABI *)(u64);
    reinterpret_cast<Sync>(g_base + SyncRva)(pose.node + 0x38);
    // Match the game's compare/exchange gate: queue this matrix provider and its
    // dependants once. Do not dirty the animation source (which could resample a clip).
    auto& pending = *reinterpret_cast<u32*>(pose.matrix_node + 0x10);
    u32 idle = 0;
    if (std::atomic_ref<u32>(pending).compare_exchange_strong(idle, 1, std::memory_order_acq_rel)) {
        using Dirty = void(PS4_SYSV_ABI *)(u64);
        reinterpret_cast<Dirty>(g_base + DirtyRva)(pose.matrix_node);
    }
}
void Update(u64 queue, u64 objects) {
    if (!g_runtime_enabled)
        return;
    Guard guard;
    if (!guard.acquired)
        return;
    L::Command command{};
    if (!L::Channel().Read(command))
        return;
    Pose pose{};
    std::array<u8, 0x2E0> object{};
    if (!offer.handle || command.id != offer.handle || command.generation != offer.generation ||
        !Object(offer.handle, pose.object, object) || pose.object != offer.pose.object ||
        Field<u64>(object, 0x10) != offer.pose.entry) {
        Dropped(command, "its offer or object changed");
        Retire(command); // resolve live identity before using any saved entry address
        return;
    }
    pose.entry = Field<u64>(object, 0x10);
    u64 owner{};
    if (!ReadAt(pose.entry, owner)) {
        Dropped(command, "its scheduler entry is gone");
        Retire(command);
        return;
    }
    if (owner != queue)
        return; // other map queues do not inspect skeleton/provider/storage or permissions
    pose.handle = offer.handle;
    if (!ReadPoseStorage(object, pose) || !SamePose(offer.pose, pose)) {
        LOG_INFO(Core, "VR lever hold rejected: pose or identity changed");
        Retire(command);
        return;
    }
    std::optional<L::UpdateQueue> slot;
    switch (QueueSlot(queue, objects, pose, slot)) {
    case Slot::Unreadable:
        Dropped(command, "its scheduler queue is unreadable");
        Retire(command);
        return;
    case Slot::Mismatch:
        LOG_INFO(Core, "VR lever hold rejected: scheduler storage mismatch");
        Retire(command);
        return;
    case Slot::Ok:
        break;
    }
    const L::UpdateQueue refresh = *slot;
    const auto now = NowNs();
    u64 param{};
    u32 handle{};
    s32 state{};
    u32 actor_handle{};
    // Resolve model behavior before pose writes or the irreversible dispatch.
    // A later accepted completion needs no fresh metadata query to stay accepted.
    const auto model_geometry = ModelGeometry(offer.model,offer.binding.sweep);
    bool fault=VR::Interactions::ProcessFaultLatch().Tripped();
    const bool capstan=model_geometry.capstan;
    PiNativeLeverCommandFacts command_facts{};
    command_facts.now_ns = now;command_facts.command_ns = command.ns;command_facts.offer_ns = offer.ns;
    command_facts.serial = command.serial;command_facts.attempted_serial = attempted_completion;
    command_facts.settle_serial = settling.Serial();command_facts.kind = uint32_t(command.kind);
    command_facts.fault = fault;command_facts.owned = edit.Owned();command_facts.parked = offer.parked;
    command_facts.model_present = offer.model != nullptr;command_facts.capstan = capstan;
    command_facts.same_parked_model = parked.model == offer.model;
    command_facts.same_parked_object = parked.object == offer.pose.object;
    command_facts.same_parked_handle = parked.handle == offer.handle;
    command_facts.angle = command.angle;command_facts.sweep = offer.binding.sweep;
    command_facts.applied_angle = applied_angle;command_facts.offer_angle = offer.angle;
    PiNativeLeverCommandResult intent{};
    const bool planned = VR::Interactions::NativeLeverCommand(settle_module,command_facts,intent);
    const bool angle_valid = intent.angle_valid;
    // Release/stale-input cleanup never needs another permission call or a live actor.
    const char *refused = nullptr;
    if (intent.refused == PI_NLC_STALE)
        refused = "the hand's command went stale (no XR update within 150 ms)";
    else if (intent.refused == PI_NLC_ANGLE)
        refused = "its angle left the lever's stops";
    if (intent.needs_checks && !refused) {
        if (!Row(offer.row, offer.block, param, handle, state) || param != offer.param ||
                 handle != offer.handle)
            refused = "its action row changed";
        else if (state != offer.state)
            refused = "its action state changed";
        else if (!ReadAt(offer.actor + 8, actor_handle) || actor_handle != offer.actor_handle)
            refused = "the player changed";
        else if (!g_motion_test && !Permitted(offer.row, offer.actor))
            refused = "the game's permission refused it";
    }
    command_facts.native_allowed = refused == nullptr;
    command_facts.fault = VR::Interactions::ProcessFaultLatch().Tripped();
    PiNativeLeverCommandResult decision{};
    if (planned) VR::Interactions::NativeLeverCommand(settle_module,command_facts,decision);
    const bool hold = decision.hold, complete = decision.complete;
    if (!edit.Owned()) {
        // Pose storage and ownership writes stay guarded in the native adapter.
        const bool owned = decision.acquire &&
            (offer.parked ? edit.Resume(offer.binding,pose.Buffers(),offer.model->lever,parked.original,parked.last)
                          : edit.Begin(offer.binding,pose.Buffers(),offer.model->lever));
        if (!owned) {
            LOG_INFO(Core, "VR lever hold rejected: hold={} fresh={} pose ownership unavailable",
                     decision.take, decision.fresh_offer);
            Retire(command);
            return;
        }
        if (offer.parked) {
            parked = {}; // the hold owns the bar now; it parks again if let go short
            L::Channel().unpark.store(0, std::memory_order_release);
        }
        applied_angle = offer.angle;
        logged_hold_angle = command.angle;
        L::Channel().held_object.store(offer.pose.object, std::memory_order_release);
        // A lever whose verdict was not bound (taken from beside the housing before its recheck)
        // hides its prompt and button on the next selection pass, not a recheck later.
        L::Channel().PromptChanged();
        LOG_INFO(Core, "VR lever hold acquired: handle={:#x} hand={} angle={}{}",
                 command.id, command.hand, command.angle,
                 offer.parked ? fmt::format(" (resumed at {:.1f} degrees)", offer.angle / Body::Deg)
                              : std::string{});
    }
    bool wants_settle = decision.wants_settle;
    if (decision.begin_settle) {
        settling.Begin(command.serial,now,applied_angle,offer.binding.sweep,
            intent.fresh_command,angle_valid,command.angle,
            capstan ? L::Motion::Capstan : L::Motion::Pull);
        fault=VR::Interactions::ProcessFaultLatch().Tripped();
        if(fault)wants_settle=false;
        else if(settling.Park()) {Park(command,now,"let go before its stop");return;}
    }
    // A capstan's hold ended any other way (refused, stale, cancelled; or the game refused its
    // completion): parked where it is, never restored.
    if (!wants_settle && decision.park_if_not_settling) {
        if (refused)
            Dropped(command, refused);
        Park(command, now,
             refused                                  ? refused
             : attempted_completion == command.serial ? "the game refused its completion"
                                                      : "its hold ended");
        return;
    }
    float settle_angle{};
    if (wants_settle) {
        settle_angle = settling.Tick(now);
        fault=VR::Interactions::ProcessFaultLatch().Tripped();if(fault)wants_settle=false;
    }
    const bool changed = wants_settle ? edit.Apply(settle_angle, pose.Buffers(), refresh)
                         : (hold || complete) ? edit.Apply(command.angle, pose.Buffers(), refresh)
                                              : edit.Restore(pose.Buffers(), refresh);
    if (!changed) {
        if (!edit.Owned()) {
            Dropped(command, "the game changed the lever's pose under the hold");
            Retire(command);
        }
        // A full queue leaves the pose untouched. Keep ownership/wake for a cleanup retry.
        return;
    }
    PublishPose(pose);
    if (wants_settle) {
        applied_angle = settle_angle;
        if (!settling.Done()) return;
        if (settling.Commit()) {
            // Revalidate immediately before the one irreversible dispatch. Permission
            // at grab time is not authority to activate a changed action later.
            const bool permitted = L::Enabled() && Row(offer.row, offer.block, param, handle, state) &&
                param == offer.param && handle == offer.handle && state == offer.state &&
                ReadAt(offer.actor + 8, actor_handle) && actor_handle == offer.actor_handle &&
                (g_motion_test || Permitted(offer.row, offer.actor));
            const bool accepted = permitted && Complete(command);
            if (reported_completion != command.serial) ReportCompletion(command, false);
            if (accepted && model_geometry.stays_at_stop) {
                StayPulled(command);
                return;
            }
            if (!accepted && capstan) {
                Park(command, now, "the game refused its completion"); // at its stop
                return;
            }
            // The original o240001 clip returns the handle to rest after activation (o321110's
            // stops partway back: end_deg). Return procedurally as well, without dispatching
            // twice or replaying a clip. A refused activation returns to rest.
            // PoseEdit relinquishes ownership if the game changes the pose meanwhile.
            settling.Return(accepted ? model_geometry.end_target : 0.f);
            return;
        }
        // The endpoint is rest (or where the model's own clip leaves it after an accepted pull):
        // the bone stays as the settle left it; no visible restoration snap.
        if (applied_angle > 0.001f)
            LOG_INFO(Core, "VR lever development: handle={:#x} ({}) stays at {:.1f} degrees, as "
                           "its own clip leaves it",
                     command.id, offer.model ? offer.model->name : "?", applied_angle / Body::Deg);
        Retire(command);
    } else if (hold) {
        applied_angle = command.angle;
        if (g_motion_test && std::abs(command.angle - logged_hold_angle) >= 0.1f) {
            logged_hold_angle = command.angle;
            const auto local =
                Body::FromHavok(pose.Buffers().locals[offer.model ? offer.model->lever : 1]);
            LOG_INFO(Core, "VR lever motion applied: handle={:#x} angle={} bone_q=({},{},{},{})",
                     command.id, command.angle, local.q.x, local.q.y, local.q.z, local.q.w);
        }
        L::Channel().RecordApplied({now, command.id, command.generation, command.angle});
        PublishOffer(now);
    } else if (complete) {
        applied_angle = command.angle;
        if (Complete(command)) {
            if (model_geometry.stays_at_stop) {
                StayPulled(command);
                return;
            }
            settling.BeginTo(command.serial,now,command.angle,model_geometry.end_target);
        }
        // A rejected attempt remains awake for cleanup next tick, without redispatch (a capstan's
        // cleanup parks it at its stop).
    } else {
        if (refused)
            Dropped(command, refused);
        Retire(command);
    }
}
// The parked capstan whose hand pull is over for now (the XR side asks as
// physical levers are switched off or no display mode a hand pulls in is left;
// Classify as its prompt is given back by B or a refused pull): the game holds
// it unturned, and its own button plays its clip from rest. Its own scheduler
// puts the bar back at its true rest once, as a cancelled hold of any other
// lever would have, so flat play sees the capstan as the game holds it. Only
// the very bar as the hand left it (StillParked and Resume compare every byte);
// a record that no longer is (the game animating the bar, its object gone or
// reloaded) is dropped, writing nothing. A request its own scheduler does not
// serve within its restore window (its map not running, its queue full) lapses
// and the record stays: the hand can still carry on, and the next request tries
// again.
void Unpark(u64 queue, u64 objects) {
    auto &channel = L::Channel();
    auto request = channel.unpark.load(std::memory_order_acquire);
    if (!request)
        return;
    PiNativeLeverServiceFacts service{};
    service.kind = PI_NLS_UNPARK_GATE;
    service.requested = 1;
    service.request_ns = request;
    service.runtime_enabled = g_runtime_enabled;
    service.motion_test = g_motion_test;
    PiNativeLeverServiceResult intent{};
    if (!VR::Interactions::NativeLeverService(settle_module, service, intent) ||
        intent.clear_request) {
      channel.unpark.compare_exchange_strong(request, 0,
                                             std::memory_order_acq_rel);
      return;
    }
    if (!intent.allowed)
      return;
    Guard guard;
    if (!guard.acquired)
        return; // busy: the next tick
    const auto now = NowNs();
    service.kind = PI_NLS_UNPARK_READY;
    service.now_ns = now;
    service.owned = edit.Owned();
    service.work = channel.work.load(std::memory_order_acquire) != 0;
    service.parked_present = parked.model != nullptr;
    if (!VR::Interactions::NativeLeverService(settle_module, service, intent) ||
        intent.clear_request) {
      channel.unpark.compare_exchange_strong(request, 0,
                                             std::memory_order_acq_rel);
      return;
    }
    if (intent.defer_request) {
      channel.unpark.compare_exchange_strong(request, now,
                                             std::memory_order_acq_rel);
      return;
    }
    if (!intent.allowed)
      return;
    const auto served = [&] {
        auto expected = request;
        channel.unpark.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    };
    const auto drop = [&](const char *why) {
        if (parked.model)
            LOG_INFO(Core, "VR lever: handle={:#x} ({}) parked bar not restored: {}",
                     parked.handle, parked.model->name, why);
        parked = {};
        served();
    };
    const auto lapse = [&](const char *why) {
      if (!intent.expired)
        return; // the DLL-approved window permits another attempt
      LOG_INFO(Core,
               "VR lever: handle={:#x} ({}) parked bar left parked for now: {}",
               parked.handle, parked.model->name, why);
      served();
    };
    Pose pose{};
    std::array<u8, 0x2E0> object{};
    if (!Object(parked.handle, pose.object, object) || pose.object != parked.object ||
        Field<u64>(object, 0x10) != parked.entry) {
        drop("its object is gone");
        return;
    }
    u64 owner{};
    if (!ReadAt(parked.entry, owner)) {
        drop("its scheduler entry is gone");
        return;
    }
    if (owner != queue) {
        lapse("its map's scheduler did not run"); // other map queues leave it to its own
        return;
    }
    pose.handle = parked.handle;
    L::BoneBinding binding{};
    L::Rig rig{};
    const L::Model *model = nullptr;
    float angle{};
    bool is_parked{};
    std::optional<L::UpdateQueue> slot;
    if (!ReadPoseStorage(object, pose) ||
        !Bind(pose, binding, rig, &model, parked.action, &angle, &is_parked) || !is_parked ||
        model != parked.model) {
        drop("the game moved it since (it is the game's again)");
        return;
    }
    if (QueueSlot(queue, objects, pose, slot) != Slot::Ok) {
        drop("its scheduler storage changed");
        return;
    }
    if (!edit.Resume(binding, pose.Buffers(), model->lever, parked.original, parked.last)) {
        drop("its bar changed");
        return;
    }
    if (!edit.Restore(pose.Buffers(), *slot)) {
        edit.Forget(); // a full queue: nothing written
        lapse("its scheduler queue stayed full");
        return;
    }
    PublishPose(pose);
    LOG_INFO(Core, "VR lever: handle={:#x} ({}) parked bar back at rest from {:.1f} degrees: its "
                   "hand pull is over for now, and the game holds it unturned",
             parked.handle, model->name, angle / Body::Deg);
    if (recent.row && recent.handle == parked.handle) {
        recent.angle = 0.0f;
        channel.PublishNearby({now, recent.handle, recent.rig, recent.armed, 0.0f});
    }
    parked = {};
    served();
}
} // namespace LiveLever

void Observe(const char *stage, u64 row, u64 actor, s32 allowed,
             const std::array<s32, 8> *event = nullptr) {
    std::array<u8, 0x140> action{};
    std::array<u8, 0x30> param{};
    if (!Copy(row, action.data(), action.size()) ||
        !Copy(Field<u64>(action, 0x88), param.data(), param.size()) ||
        !OperateAnimation(Field<s32>(param, 0x0C))) {
        return;
    }
    const auto handle = Field<u32>(action, 0x58);
    const auto param_id = Field<s32>(action, 0x80);
    const auto state = Field<s32>(action, 0x5C);
    if (allowed < 0 && !event) {
        // Periodic snapshots capture changes while the candidate stays selected, keeping the
        // ordinary per-frame selection path inexpensive when the probe is enabled.
        thread_local u64 last_row{};
        thread_local u32 last_handle{};
        thread_local s32 last_param{}, last_state{};
        thread_local std::chrono::steady_clock::time_point last_time{};
        const auto now = std::chrono::steady_clock::now();
        if (row == last_row && handle == last_handle && param_id == last_param &&
            state == last_state && now - last_time < std::chrono::seconds(1)) {
            return;
        }
        last_row = row;
        last_handle = handle;
        last_param = param_id;
        last_state = state;
        last_time = now;
    }
    std::array<u8, 0x2E0> object{};
    u64 object_address{};
    const bool object_ok = Object(handle, object_address, object);
    LOG_INFO(Core,
             "VR lever probe: {} row={:#x} actor={:#x} handle={:#x} param={} state={} "
             "permission={} player_anim={} param_10={} controller={:#x} kick_event={} "
             "object_ok={} object={:#x} object_208={} pos=({:.4f},{:.4f},{:.4f})",
             stage, row, actor, handle, param_id, state, allowed, Field<s32>(param, 0x0C),
             Field<s32>(param, 0x10), Field<u64>(action, 0x90), Field<s32>(action, 0x0C), object_ok,
             object_address, Field<s32>(object, 0x208), Field<float>(object, 0x24C),
             Field<float>(object, 0x25C), Field<float>(object, 0x26C));
    if (object_ok) {
        g_observe_until.store(NowMs() + 8000, std::memory_order_relaxed);
        g_observed_object.store(object_address, std::memory_order_release);
        LOG_INFO(
            Core,
            "VR lever probe: object basis=({:.4f},{:.4f},{:.4f}) "
            "({:.4f},{:.4f},{:.4f}) ({:.4f},{:.4f},{:.4f})",
            Field<float>(object, 0x240), Field<float>(object, 0x244), Field<float>(object, 0x248),
            Field<float>(object, 0x250), Field<float>(object, 0x254), Field<float>(object, 0x258),
            Field<float>(object, 0x260), Field<float>(object, 0x264), Field<float>(object, 0x268));
    }
    if (event) {
        LOG_INFO(Core, "VR lever probe: event words={:x},{:x},{:x},{:x},{:x},{:x},{:x},{:x}",
                 (*event)[0], (*event)[1], (*event)[2], (*event)[3], (*event)[4], (*event)[5],
                 (*event)[6], (*event)[7]);
    }
}

void PS4_SYSV_ABI Pose(u64 object, u64 node) noexcept {
    try {
        // Discover idle instances too: requiring a selected prompt would hide the very pose
        // updates needed to establish whether a resting lever can follow a held hand.
        // Cache per object, not per thread globally, so one rig cannot starve another.
        struct Observation {
            u64 object{};
            s64 next{};
        };
        thread_local std::array<Observation, 64> observations{};
        const auto now = NowMs();
        auto &observation = observations[(object >> 5) % observations.size()];
        if (observation.object == object && now < observation.next) {
            return;
        }
        observation = {object, now + 1000};
        u64 skeleton{}, locals{}, bones{};
        s32 local_count{}, bone_count{}, animation{};
        // A bounded callback sample distinguishes an inactive hook from unsupported rigs.
        // This runs only in the explicitly enabled diagnostic build path.
        static std::atomic<u32> samples{};
        if (samples.fetch_add(1, std::memory_order_relaxed) < 8) {
            ReadAt(node + 0x38, skeleton);
            ReadAt(node + 0x48, local_count);
            ReadAt(skeleton + 0x30, bone_count);
            LOG_INFO(Core,
                     "VR lever probe: pose callback object={:#x} node={:#x} "
                     "skeleton={:#x} locals={} bones={}",
                     object, node, skeleton, local_count, bone_count);
        }
        if (!ReadAt(node + 0x38, skeleton) || !ReadAt(node + 0x40, locals) ||
            !ReadAt(node + 0x48, local_count) || local_count != 4 ||
            !ReadAt(skeleton + 0x28, bones) || !ReadAt(skeleton + 0x30, bone_count) ||
            bone_count != 4 || !ReadAt(object + 0x208, animation)) {
            return;
        }
        std::array<std::array<float, 12>, 4> transforms{};
        if (!Copy(locals, transforms.data(), sizeof(transforms))) {
            return;
        }
        // Bind by verified names: the FLVER and Havok orders differ for the last two bones.
        constexpr std::array<const char *, 4> expected = {"o240001", "o240001_00", "o240001_01",
                                                          "o240001_02"};
        for (size_t i = 0; i < expected.size(); ++i) {
            u64 name{};
            std::array<char, 16> text{};
            if (!ReadAt(bones + i * 16, name) || !Copy(name & ~u64{1}, text.data(), text.size()) ||
                std::strncmp(text.data(), expected[i], text.size()) != 0) {
                return;
            }
        }
        const bool selected = object == g_observed_object.load(std::memory_order_acquire) &&
                              now <= g_observe_until.load(std::memory_order_relaxed);
        observation.next = now + (selected ? 100 : 5000);
        std::array<float, 12> frame{};
        if (Copy(object + 0x240, frame.data(), sizeof(frame))) {
            LOG_INFO(Core,
                     "VR lever probe: rig object={:#x} selected={} pos=({:.4f},{:.4f},{:.4f}) "
                     "basis=({:.4f},{:.4f},{:.4f}) ({:.4f},{:.4f},{:.4f}) ({:.4f},{:.4f},{:.4f})",
                     object, selected, frame[3], frame[7], frame[11], frame[0], frame[1], frame[2],
                     frame[4], frame[5], frame[6], frame[8], frame[9], frame[10]);
        }
        for (size_t i = 0; i < transforms.size(); ++i) {
            const auto &t = transforms[i];
            LOG_INFO(Core,
                     "VR lever probe: pose object={:#x} node={:#x} anim={} bone={} "
                     "t=({:.5f},{:.5f},{:.5f}) q=({:.5f},{:.5f},{:.5f},{:.5f}) "
                     "s=({:.5f},{:.5f},{:.5f})",
                     object, node, animation, expected[i], t[0], t[1], t[2], t[4], t[5], t[6], t[7],
                     t[8], t[9], t[10]);
        }
    } catch (...) {
    }
}

void PS4_SYSV_ABI UpdateObjects(u64 queue, u64 objects) noexcept {
    try {
        LiveLever::MotionRequest();
        LiveLever::Update(queue, objects);
        LiveLever::Unpark(queue, objects);
        LiveLever::CheckArmed();
        LiveLever::Scan();
        LiveLever::ServiceArmed();
    } catch (...) {
    }
    if (!g_diagnostics)
        return;
    // Development probe only. After its bounded sample, do not inspect game memory.
    // Production pose work must be gated by an owned hold or its one-shot cleanup.
    static std::atomic<u32> samples{};
    if (samples.load(std::memory_order_relaxed) >= 8) {
        return;
    }
    try {
        // Validate this boundary in the actual game before enabling physical writes here.
        // An empty queue is still a tick: release/stale-command recovery must not depend
        // on the owned lever already being animated or selected by the prompt checker.
        u32 capacity{}, count{};
        u64 entries{}, indices{};
        if (!ReadAt(queue, capacity) || capacity == 0 || capacity > 65536 ||
            !ReadAt(queue + 0x10, count) || count > capacity || !ReadAt(queue + 8, entries) ||
            !ReadAt(queue + 0x18, indices) || !Hook::Plausible(reinterpret_cast<u8 *>(objects)) ||
            !Hook::Plausible(reinterpret_cast<u8 *>(entries)) ||
            !Hook::Plausible(reinterpret_cast<u8 *>(indices))) {
            return;
        }
        const auto sample = samples.fetch_add(1, std::memory_order_relaxed);
        if (sample >= 7) {
            g_update_probe_active.store(0, std::memory_order_release);
        }
        if (sample < 8) {
            LOG_INFO(Core,
                     "VR lever probe: update queue={:#x} objects={:#x} "
                     "capacity={} pending={}",
                     queue, objects, capacity, count);
        }
    } catch (...) {
    }
}

void PS4_SYSV_ABI Selected(u64 row, u64 actor, u64 block) noexcept {
    try {
        s32 action{};
        if (g_runtime_enabled && ReadAt(row + 0x80, action) && PhysicalLeverAction(action)) {
            LiveLever::Classify(row, actor, block);
            // Only a lever whose own rig binds takes the squeeze here; any other leaves it to
            // the armed lever (the scheduler hook's ServiceArmed).
            auto &verdict = VR::Lever::Channel().verdict;
            u32 handle{};
            if (!g_motion_test && verdict.bound && verdict.row == row &&
                ReadAt(row + 0x58, handle) && verdict.handle == handle)
                LiveLever::Selected(row, actor, block);
        }
        if (g_diagnostics)
            Observe("candidate", row, actor, -1);
    } catch (...) {
        // Diagnostics must never unwind into guest code.
    }
}
void PS4_SYSV_ABI Permission(u64 row, u64 actor, u32 allowed) noexcept {
    try {
        Observe("permission", row, actor, allowed != 0);
    } catch (...) {
    }
}
void PS4_SYSV_ABI Event(u64 block, u64 record, u32 remote) noexcept {
    try {
        LiveLever::RewriteEvent(block, record, remote);
        if (!g_diagnostics)
            return;
        std::array<s32, 8> event{};
        s32 count{};
        u64 rows{};
        if (!Copy(record, event.data(), sizeof(event)) || !ReadAt(block + 0x98, count) ||
            count <= 0 || count > 65536 || event[1] < 0 || event[1] >= count ||
            !ReadAt(block + 0x90, rows) || !Hook::Plausible(reinterpret_cast<u8 *>(rows))) {
            return;
        }
        // Receipt does not prove the event succeeded. Remote receipt also does not imply
        // that the local permission checker ran. Retain -1 ("unknown") in both cases.
        Observe(remote ? "event_remote" : "event_local", rows + static_cast<u64>(event[1]) * 0x140,
                0, -1, &event);
    } catch (...) {
    }
}
} // namespace

void InstallLeverProbe(VAddr base, u64 size) {
    const char *enabled = std::getenv("VR_LEVER_PROBE");
    const char *physical = std::getenv("VR_LEVER_PHYSICAL_TEST");
    const char *motion = std::getenv("VR_LEVER_MOTION_TEST");
    const bool motion_test = motion && std::strcmp(motion, "1") == 0;
    // Install for ordinary VR launches, even if the live setting starts off: the
    // user can turn it on in-game. An explicit zero retains the diagnostic-only escape hatch.
    const bool runtime_enabled = motion_test || !physical || std::strcmp(physical, "0") != 0;
    const bool diagnostics = enabled && std::strcmp(enabled, "1") == 0;
    if ((!runtime_enabled && !diagnostics) || g_code || !base) {
        return;
    }
#ifndef _WIN32
    LOG_WARNING(Core, "VR lever probe: safe diagnostic reads currently require Windows");
    return;
#endif
    auto *module = reinterpret_cast<u8 *>(base);
    if (size < (motion_test ? WorldChrManRva : ObjectManRva) + sizeof(u64) ||
        std::memcmp(module + ScriptPromptRva, ScriptPromptBytes.data(), ScriptPromptBytes.size()) != 0 ||
        std::memcmp(module + SelectRva, SelectBytes.data(), SelectBytes.size()) != 0 ||
        std::memcmp(module + PermissionRva, PermissionBytes.data(), PermissionBytes.size()) != 0 ||
        std::memcmp(module + EventRva, EventBytes.data(), EventBytes.size()) != 0 ||
        std::memcmp(module + UpdateRva, UpdateBytes.data(), UpdateBytes.size()) != 0 ||
        std::memcmp(module + PoseRva, PoseBytes.data(), PoseBytes.size()) != 0 ||
        std::memcmp(module + SyncRva, SyncBytes.data(), SyncBytes.size()) != 0) {
        LOG_WARNING(Core, "VR lever probe: 1.09 signature mismatch; probe not installed");
        return;
    }
    if (runtime_enabled &&
        (size < BlockedInitRva + sizeof(u64) ||
         std::memcmp(module + AcceptRva, AcceptBytes.data(), AcceptBytes.size()) != 0 ||
         std::memcmp(module + CheckRva, CheckBytes.data(), CheckBytes.size()) != 0 ||
         std::memcmp(module + BlockedRva, BlockedBytes.data(), BlockedBytes.size()) != 0 ||
         std::memcmp(module + DirtyRva, DirtyBytes.data(), DirtyBytes.size()) != 0)) {
        LOG_WARNING(Core, "VR lever development: accept/permission/interaction/matrix-update "
                          "signature mismatch; disabled");
        return;
    }
    try {
        auto code = std::make_unique<Xbyak::CodeGenerator>(16384);
        // The gates outgrew rel8: every label jump is near (rel32), never "label is too far".
        code->setDefaultJmpNEAR(true);
        using namespace Xbyak::util;
        // Generate the same exact allowlist used by row validation and rehearsal
        // discovery. No host call or range match that could include other devices.
        const auto reject_other_action = [&](Xbyak::Label &reject) {
            Xbyak::Label supported;
            for (const auto action : PhysicalLeverActions) {
                code->cmp(dword[rax + 0x80], action);
                code->je(supported);
            }
            code->jmp(reject);
            code->L(supported);
        };
        // Both the cheap selection gate and the acquisition callback converge here.
        // 13ADCC0 uses 9D to offer next frame's prompt and 9C to accept this
        // frame's button. Clear both only while on, for a listed lever action whose exact
        // instance (this row and its object handle) the host found its rig binds for, with a
        // hand able to pull it (core/vr/lever_prompt.h). Any other selected action, a lever model
        // without a rig on a shared row, a demoted lever, or any lever while no hand can pull
        // (virtual screen, third person, untracked) keeps the game's prompt and button.
        static_assert(std::atomic<bool>::is_always_lock_free && sizeof(std::atomic<bool>) == 1);
        static_assert(std::atomic<u64>::is_always_lock_free && sizeof(std::atomic<u64>) == 8);
        auto &verdict = Lever::Channel().verdict;
        const auto verdict_at = reinterpret_cast<u64>(&verdict);
        void *selection_finish = const_cast<u8 *>(code->getCurr());
        code->mov(dword[r13 + 0xA8], eax);
        if (runtime_enabled) {
            Xbyak::Label original_prompt;
            code->lea(rsp, ptr[rsp - 128]);
            code->push(rax);
            code->push(rdx);
            code->mov(rax, reinterpret_cast<u64>(&Lever::physical_levers));
            code->cmp(byte[rax], 0);
            code->je(original_prompt);
            code->mov(rax, ptr[rbp - 0x70]);
            code->add(rax, ptr[rbp - 0x78]);
            reject_other_action(original_prompt);
            code->mov(rdx, verdict_at);
            code->cmp(qword[rdx + offsetof(Lever::PromptVerdict, row)], rax);
            code->jne(original_prompt);
            code->mov(eax, dword[rax + 0x58]);
            code->cmp(dword[rdx + offsetof(Lever::PromptVerdict, handle)], eax);
            code->jne(original_prompt);
            code->cmp(byte[rdx + offsetof(Lever::PromptVerdict, bound)], 0);
            code->je(original_prompt);
            code->mov(byte[r13 + 0x9C], 0);
            code->mov(byte[r13 + 0x9D], 0);
            code->mov(qword[r13 + 0xA0], -1);
            code->mov(dword[r13 + 0xA8], 0);
            code->L(original_prompt);
            code->pop(rdx);
            code->pop(rax);
            code->lea(rsp, ptr[rsp + 128]);
        }
        code->cmp(byte[r13 + 0x9C], 0);
        code->db(0xFF); code->db(0x25); code->dd(0);
        code->dq(base + SelectRva + SelectBytes.size());
        void *selected_callback = Hook::EmitHostCallStubWith(
            *code, reinterpret_cast<void *>(&Selected), SelectBytes.data(), 0,
            reinterpret_cast<u64>(selection_finish), [](auto &c) {
                c.mov(rdi, ptr[rbp - 0x70]);
                c.add(rdi, ptr[rbp - 0x78]);
                c.mov(rsi, r15);
                c.mov(rdx, r13);
            });
        // No per-frame host call: recognize the already-selected action's parameter, then
        // reuse the host's verdict for this row and object. The host classifies a different
        // row/object, a B fallback, a prompt change (prompt_serial) or a due recheck (about once
        // a second), and resolves
        // bones/permission for a grip request. Only a bound lever advances the cheap hint.
        code->align(16);
        void *selected = const_cast<u8 *>(code->getCurr());
        Xbyak::Label selection_resume, selection_call, selection_not_lever;
        code->lea(rsp, ptr[rsp - 128]);
        code->pushfq();
        code->push(rax);
        code->push(rdx);
        code->push(rcx);
        code->mov(rax, ptr[rbp - 0x70]);
        code->add(rax, ptr[rbp - 0x78]);
        reject_other_action(selection_not_lever);
        if (runtime_enabled) {
            code->mov(rdx, reinterpret_cast<u64>(&Lever::physical_levers));
            code->cmp(byte[rdx], 0);
            code->je(selection_not_lever);
            code->mov(rdx, verdict_at);
            code->cmp(qword[rdx + offsetof(Lever::PromptVerdict, row)], rax);
            code->jne(selection_call);
            code->mov(ecx, dword[rax + 0x58]);
            code->cmp(dword[rdx + offsetof(Lever::PromptVerdict, handle)], ecx);
            code->jne(selection_call);
            code->mov(rcx, reinterpret_cast<u64>(&Lever::Channel().prompt_fallback));
            code->mov(rcx, qword[rcx]);
            code->cmp(qword[rdx + offsetof(Lever::PromptVerdict, fallback)], rcx);
            code->jne(selection_call);
            // A refused pull, a hold or the XR side's readiness changed (any thread): ask.
            code->mov(rcx, reinterpret_cast<u64>(&Lever::Channel().prompt_serial));
            code->mov(rcx, qword[rcx]);
            code->cmp(qword[rdx + offsetof(Lever::PromptVerdict, serial)], rcx);
            code->jne(selection_call);
            // 1 -> 0 (due) and 0 -> wrap both ask the host, which treats either as due
            // (lever_prompt.h NeedsClassify); nothing but this gate counts it down.
            code->sub(word[rdx + offsetof(Lever::PromptVerdict, recheck)], 1);
            code->jbe(selection_call);
            code->cmp(byte[rdx + offsetof(Lever::PromptVerdict, bound)], 0);
            code->je(selection_not_lever);
            code->mov(rdx, reinterpret_cast<u64>(&Lever::Channel().selected_hint));
            code->lock();
            code->inc(qword[rdx]);
            code->mov(rdx, reinterpret_cast<u64>(&Lever::Channel().acquire));
            code->cmp(qword[rdx], 0);
            code->jne(selection_call);
        }
        code->L(selection_not_lever);
        if (diagnostics)
            code->jmp(selection_call);
        else
            code->jmp(selection_resume);
        code->L(selection_call);
        code->pop(rcx);
        code->pop(rdx);
        code->pop(rax);
        code->popfq();
        code->lea(rsp, ptr[rsp + 128]);
        code->db(0xFF); code->db(0x25); code->dd(0);
        code->dq(reinterpret_cast<u64>(selected_callback));
        code->L(selection_resume);
        code->pop(rcx);
        code->pop(rdx);
        code->pop(rax);
        code->popfq();
        code->lea(rsp, ptr[rsp + 128]);
        code->db(0xFF); code->db(0x25); code->dd(0);
        code->dq(reinterpret_cast<u64>(selection_finish));
        void *permission = Hook::EmitHostCallStubWith(
            *code, reinterpret_cast<void *>(&Permission), PermissionBytes.data(),
            PermissionBytes.size(), base + PermissionRva + PermissionBytes.size(), [](auto &c) {
                c.movzx(edx, al);
                c.mov(rdi, ptr[rbp - 0x70]);
                c.add(rdi, rbx);
                c.mov(rsi, r15);
            });
        void *event = Hook::EmitHostCallStubWith(
            *code, reinterpret_cast<void *>(&Event), EventBytes.data(), EventBytes.size(),
            base + EventRva + EventBytes.size(), [](auto &) {});
        void *update_callback = Hook::EmitHostCallStubWith(
            *code, reinterpret_cast<void *>(&UpdateObjects), UpdateBytes.data(), UpdateBytes.size(),
            base + UpdateRva + UpdateBytes.size(), [](auto &) {});
        // Inactive: one byte check, no host callback, SIMD saves or game-memory reads.
        // Preserve flags, rax and the SysV red zone on both branches.
        code->align(16);
        void *update = const_cast<u8 *>(code->getCurr());
        Xbyak::Label inactive, active;
        code->lea(rsp, ptr[rsp - 128]);
        code->pushfq();
        code->push(rax);
        code->mov(rax, reinterpret_cast<u64>(&g_update_probe_active));
        code->cmp(byte[rax], 0);
        code->jne(active);
        if (runtime_enabled) {
            code->mov(rax, reinterpret_cast<u64>(&Lever::Channel().work));
            code->cmp(byte[rax], 0);
            code->jne(active);
            // A pending squeeze (one per grip press near the armed lever, cleared by its reply
            // or the XR side's 100 ms timeout) may be for a lever the hunter has stepped away
            // from the prompt zone of: the scheduler serves it (ServiceArmed). Near the armed
            // lever the XR side also asks twice a second whether it still is (CheckArmed).
            code->mov(rax, reinterpret_cast<u64>(&Lever::Channel().acquire));
            code->cmp(qword[rax], 0);
            code->jne(active);
            code->mov(rax, reinterpret_cast<u64>(&Lever::Channel().nearby_check));
            code->cmp(qword[rax], 0);
            code->jne(active);
            // A few times a second while no armed lever is near: the proximity scan (Scan).
            code->mov(rax, reinterpret_cast<u64>(&Lever::Channel().scan));
            code->cmp(qword[rax], 0);
            code->jne(active);
            // Once, when a parked capstan's hand pull is over for now: its bar back at rest
            // (Unpark, on the scheduler that runs it).
            code->mov(rax, reinterpret_cast<u64>(&Lever::Channel().unpark));
            code->cmp(qword[rax], 0);
            code->jne(active);
        }
        code->jmp(inactive);
        code->L(active);
        code->pop(rax);
        code->popfq();
        code->lea(rsp, ptr[rsp + 128]);
        code->db(0xFF);
        code->db(0x25);
        code->dd(0);
        code->dq(reinterpret_cast<u64>(update_callback));
        code->L(inactive);
        code->pop(rax);
        code->popfq();
        code->lea(rsp, ptr[rsp + 128]);
        for (const auto instruction : UpdateBytes) {
            code->db(instruction);
        }
        code->db(0xFF);
        code->db(0x25);
        code->dd(0);
        code->dq(base + UpdateRva + UpdateBytes.size());
        // Relay preserves the original sync call's register effects and its following load.
        // Calling through RIP memory does not add an extra rax clobber before the original.
        code->align(16);
        void *relay = const_cast<u8 *>(code->getCurr());
        code->db(0xFF);
        code->db(0x15);
        code->dd(2); // call qword [rip+2]
        code->db(0xEB);
        code->db(8); // skip the embedded address after the call returns
        code->dq(base + SyncRva);
        code->mov(rdi, ptr[r14 + 0x2A8]);
        code->db(0xFF);
        code->db(0x25);
        code->dd(0);
        code->dq(base + PoseRva + PoseBytes.size());
        void *pose =
            Hook::EmitHostCallStubWith(*code, reinterpret_cast<void *>(&Pose), PoseBytes.data(), 7,
                                       reinterpret_cast<u64>(relay), [](auto &c) {
                                           c.mov(rdi, r14);
                                           c.mov(rsi, rbx);
                                       });
        // No host callback, search, or cached target: inspect only the object the
        // game has already resolved and found in range for this script prompt (an EMEVD
        // action button: a used lever's "Device is not currently operable", a lift's call).
        // Ordinary launches hide it only on the lever a physical hold or settle owns right now:
        // a used or unrigged lever has no hand pull, so it keeps the game's prompt and button.
        // Motion-only rehearsal (which pulls used levers by hand) hides it on the o240001
        // skeleton, as before.
        code->align(16);
        void *script_prompt = const_cast<u8 *>(code->getCurr());
        if (runtime_enabled && !motion_test) {
            Xbyak::Label original, suppress;
            code->pushfq();
            code->push(rax);
            code->push(rdx);
            code->mov(rax, reinterpret_cast<u64>(&Lever::physical_levers));
            code->cmp(byte[rax], 0);
            code->je(original);
            code->test(r12, r12);
            code->jz(original);
            code->mov(rax, reinterpret_cast<u64>(&Lever::Channel().held_object));
            code->cmp(qword[rax], r12);
            code->je(suppress);
            code->L(original);
            code->pop(rdx);
            code->pop(rax);
            code->popfq();
            code->db(ScriptPromptBytes.data(), ScriptPromptBytes.size());
            code->db(0xFF); code->db(0x25); code->dd(0);
            code->dq(base + ScriptPromptRva + ScriptPromptBytes.size());
            code->L(suppress);
            code->pop(rdx);
            code->pop(rax);
            code->popfq();
            code->db(0xFF); code->db(0x25); code->dd(0);
            code->dq(base + 0x144E7D9); // normal no-interaction epilogue
        } else if (runtime_enabled) {
            Xbyak::Label original, suppress;
            code->pushfq();
            code->push(rax);
            code->push(rdx);
            code->mov(rax, reinterpret_cast<u64>(&Lever::physical_levers));
            code->cmp(byte[rax], 0);
            code->je(original);
            code->test(r12, r12);
            code->jz(original);
            code->mov(rax, ptr[r12 + 0x290]); // animation controller
            code->test(rax, rax);
            code->jz(original);
            code->mov(rax, ptr[rax + 0x20]); // pose node
            code->test(rax, rax);
            code->jz(original);
            code->mov(rax, ptr[rax + 0x38]); // skeleton
            code->test(rax, rax);
            code->jz(original);
            code->cmp(dword[rax + 0x30], 4);
            code->jne(original);
            code->mov(rax, ptr[rax + 0x28]); // bones
            code->test(rax, rax);
            code->jz(original);
            code->mov(rax, ptr[rax]); // hkStringPtr root bone name
            code->and_(rax, -2);
            code->test(rax, rax);
            code->jz(original);
            code->mov(rdx, 0x003130303034326FULL); // "o240001\0"
            code->cmp(qword[rax], rdx);
            code->je(suppress);
            code->L(original);
            code->pop(rdx);
            code->pop(rax);
            code->popfq();
            code->db(ScriptPromptBytes.data(), ScriptPromptBytes.size());
            code->db(0xFF); code->db(0x25); code->dd(0);
            code->dq(base + ScriptPromptRva + ScriptPromptBytes.size());
            code->L(suppress);
            code->pop(rdx);
            code->pop(rax);
            code->popfq();
            code->db(0xFF); code->db(0x25); code->dd(0);
            code->dq(base + 0x144E7D9); // normal no-interaction epilogue
        } else {
            code->db(ScriptPromptBytes.data(), ScriptPromptBytes.size());
            code->db(0xFF); code->db(0x25); code->dd(0);
            code->dq(base + ScriptPromptRva + ScriptPromptBytes.size());
        }
        code->ready();
        Hook::FlushCode(const_cast<u8 *>(code->getCode()), code->getSize());
        if (!Hook::MakeWritable(module + ScriptPromptRva, ScriptPromptBytes.size()) ||
            !Hook::MakeWritable(module + SelectRva, SelectBytes.size()) ||
            !Hook::MakeWritable(module + PermissionRva, PermissionBytes.size()) ||
            !Hook::MakeWritable(module + EventRva, EventBytes.size()) ||
            !Hook::MakeWritable(module + UpdateRva, UpdateBytes.size()) ||
            !Hook::MakeWritable(module + PoseRva, PoseBytes.size())) {
            LOG_WARNING(Core, "VR lever probe: cannot patch code; probe not installed");
            return;
        }
        if(runtime_enabled && VR::Interactions::DllRequested()){
            LiveLever::settle_module=new VR::Interactions::Module();
            LiveLever::settle_module->EnableFaultBroadcast();
            if(!LiveLever::settle_module->LoadBesideExecutable()){
                LiveLever::settle_fault=true;
                LOG_ERROR(Core,"VR lever: DLL native settling unavailable: {}",LiveLever::settle_module->Diagnostic());
                return;
            }
            LiveLever::settling.Bind(*LiveLever::settle_module,LiveLever::settle_fault);
            LiveLever::prompt_policy.Bind(*LiveLever::settle_module,LiveLever::settle_fault);
            LiveLever::edit.Policy().Bind(*LiveLever::settle_module);
        }
        g_base = module;
        g_runtime_enabled = runtime_enabled;
        g_motion_test = motion_test;
        g_diagnostics = diagnostics;
        g_update_probe_active.store(diagnostics ? 1 : 0, std::memory_order_release);
        g_code = std::move(code); // Own every stub before publishing any jump.
        Hook::WriteJump(module + ScriptPromptRva, ScriptPromptBytes.size(), script_prompt);
        Hook::WriteJump(module + SelectRva, SelectBytes.size(), selected);
        if (diagnostics)
            Hook::WriteJump(module + PermissionRva, PermissionBytes.size(), permission);
        Hook::WriteJump(module + EventRva, EventBytes.size(), event);
        Hook::WriteJump(module + UpdateRva, UpdateBytes.size(), update);
        if (diagnostics)
            Hook::WriteJump(module + PoseRva, PoseBytes.size(), pose);
        if (runtime_enabled) {
            Lever::Channel().motion_test.store(motion_test, std::memory_order_relaxed);
            Lever::Channel().installed.store(true, std::memory_order_release);
            if (motion_test)
                LOG_WARNING(Core, "VR levers: MOTION ONLY: permission bypass enabled; gameplay events disabled");
            else
                LOG_INFO(Core, "VR levers: physical interaction installed; controlled by physical_levers");
        }
        if (diagnostics)
            LOG_INFO(Core, "VR lever probe: candidate, permission and event diagnostics installed");
    } catch (const Xbyak::Error &error) {
        LOG_ERROR(Core, "VR lever probe: code generation failed ({})", error.what());
    }
}
} // namespace VR::Bloodborne
