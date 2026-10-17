// SPDX-License-Identifier: GPL-2.0-or-later

// These tests execute the actual installed guest detours in synthetic executable memory.
// No copyrighted game data, live game, or headset is required.
#include <array>
#include <cstring>
#include <functional>
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <sstream>
#include <windows.h>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/vr/bloodborne_levers.h"
#include "core/vr/lever_channel.h"
#include "core/vr/lever_models.h"
#include "core/vr/lever_settings.h"
#include <chrono>
#include <thread>
#include <vector>

namespace Common {
std::string GetCurrentThreadName() { return "lever-probe-test"; }
namespace Log {
std::unordered_map<std::string_view, std::shared_ptr<spdlog::logger>> ALL_LOGGERS;
}
} // namespace Common

namespace {
// Up to the interaction gate's static-initialization guard (0x5562888), which the adapter reads.
constexpr size_t ModuleSize = 0x5562890;
constexpr size_t Select = 0x13AE171, Permission = 0x13AE19A, Event = 0x13AE600;
// Independent fixture of the original instructions. A changed/incompatible build must not install.
constexpr std::array<u8, 15> SelectBytes{0x41, 0x89, 0x85, 0xA8, 0, 0, 0, 0x41,
                                         0x80, 0xBD, 0x9C, 0,    0, 0, 0};
constexpr std::array<u8, 14> PermissionBytes{0x48, 0x8B, 0x4D, 0x90, 0x48, 0x8B, 0x8C,
                                             0x19, 0x88, 0,    0,    0,    0x84, 0xC0};
constexpr std::array<u8, 20> EventBytes{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                        0x41, 0x54, 0x53, 0x48, 0x81, 0xEC, 0xA8, 0x01, 0,    0};

constexpr size_t ScriptPrompt = 0x144E7A9;
constexpr std::array<u8, 15> ScriptPromptBytes{0xC7, 0x44, 0x24, 8, 0, 0, 0, 0,
                                             0xC7, 0x04, 0x24, 0, 0, 0, 0};
constexpr size_t Update = 0x1CB4DD0;
constexpr std::array<u8, 17> UpdateBytes{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                         0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x38};
constexpr size_t Pose = 0x15A434B, Sync = 0x6C4F20;
constexpr std::array<u8, 19> PoseBytes{0x48, 0x83, 0xC3, 0x38, 0x48, 0x89, 0xDF, 0xE8, 0xC9, 0x0B,
                                       0x12, 0xFF, 0x49, 0x8B, 0xBE, 0xA8, 0x02, 0x00, 0x00};
constexpr std::array<u8, 16> SyncBytes{0x80, 0x7F, 0x38, 0,    0x0F, 0x85, 0x83, 1,
                                       0,    0,    0x48, 0x8B, 7,    0x44, 0x8B, 0x40};

constexpr size_t Accept = 0x13AE370;
constexpr std::array<u8, 17> AcceptBytes{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                         0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x38};
constexpr size_t Check = 0x13A4A30, Dirty = 0x1D3C500;
constexpr std::array<u8, 17> CheckBytes{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                        0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x48};
constexpr std::array<u8, 16> DirtyBytes{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
                                          0x53, 0x50, 0x49, 0x89, 0xFF, 0x4C, 0x8D, 0x35};
// The game's interaction gate (its selector's "may the player interact now" query), its static-
// initialization guard and the menu state it reads.
constexpr size_t Blocked = 0x176B7E0, BlockedInit = 0x5562888, BlockedState = 0x5562878;
constexpr std::array<u8, 16> BlockedBytes{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
                                          0x53, 0x48, 0x83, 0xEC, 0x48, 0x4C, 0x8B, 0x3D};
s64 SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Module {
    u8 *data = static_cast<u8 *>(
        VirtualAlloc(nullptr, ModuleSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ~Module() {
        if (data)
            VirtualFree(data, 0, MEM_RELEASE);
    }
    void Seed() {
        std::memcpy(data + ScriptPrompt, ScriptPromptBytes.data(), ScriptPromptBytes.size());
        std::memcpy(data + Select, SelectBytes.data(), SelectBytes.size());
        std::memcpy(data + Permission, PermissionBytes.data(), PermissionBytes.size());
        std::memcpy(data + Event, EventBytes.data(), EventBytes.size());
        std::memcpy(data + Pose, PoseBytes.data(), PoseBytes.size());
        std::memcpy(data + Sync, SyncBytes.data(), SyncBytes.size());
        std::memcpy(data + Update, UpdateBytes.data(), UpdateBytes.size());
        std::memcpy(data + Accept, AcceptBytes.data(), AcceptBytes.size());
        std::memcpy(data + Check, CheckBytes.data(), CheckBytes.size());
        std::memcpy(data + Dirty, DirtyBytes.data(), DirtyBytes.size());
        std::memcpy(data + Blocked, BlockedBytes.data(), BlockedBytes.size());
    }
    void Install() { VR::Bloodborne::InstallLeverProbe(reinterpret_cast<VAddr>(data), ModuleSize); }
};
template <typename T, size_t N> void Put(std::array<u8, N> &bytes, size_t offset, T value) {
    ASSERT_LE(offset + sizeof(T), N);
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

TEST(LeverProbe, DisabledAndMismatchedModulesAreUntouched) {
    Module module;
    ASSERT_NE(module.data, nullptr);
    module.Seed();
    _putenv_s("VR_LEVER_PROBE", "0");
    _putenv_s("VR_LEVER_PHYSICAL_TEST", "0");
    module.Install();
    EXPECT_EQ(std::memcmp(module.data + Select, SelectBytes.data(), SelectBytes.size()), 0);
    _putenv_s("VR_LEVER_PROBE", "1");
    module.data[Permission] ^= 1;
    module.Install();
    EXPECT_EQ(std::memcmp(module.data + Select, SelectBytes.data(), SelectBytes.size()), 0);
    EXPECT_EQ(std::memcmp(module.data + Event, EventBytes.data(), EventBytes.size()), 0);
    module.Seed();
    VR::Bloodborne::InstallLeverProbe(reinterpret_cast<VAddr>(module.data), Event + 20);
    EXPECT_EQ(std::memcmp(module.data + Select, SelectBytes.data(), SelectBytes.size()), 0);
    _putenv_s("VR_LEVER_PROBE", "");
    _putenv_s("VR_LEVER_PHYSICAL_TEST", "");
}

// A build whose interaction gate differs never installs the hand pull (it would be served
// without the game's own "may the player interact now" check).
TEST(LeverProbe, ChangedInteractionGateDisablesTheHandPull) {
    EXPECT_EXIT(([] {
        Module module;
        if (!module.data) std::_Exit(2);
        module.Seed();
        module.data[Blocked + 4] ^= 1;
        _putenv_s("VR_LEVER_PROBE", "0");
        _putenv_s("VR_LEVER_PHYSICAL_TEST", "");
        _putenv_s("VR_LEVER_MOTION_TEST", "");
        module.Install();
        std::_Exit(VR::Lever::Channel().installed.load() ||
                           std::memcmp(module.data + Select, SelectBytes.data(),
                                       SelectBytes.size()) != 0
                       ? 3
                       : 0);
    }()), ::testing::ExitedWithCode(0), "");
}

// `rig_model`: the measured rig the synthetic lever object carries (core/vr/lever_models.h): three
// or four Havok bones under its own FLVER -> Havok mesh table (as many entries as its mesh has
// bones, unmapped ones negative), any rest and hinge axis, the lever bone's own scale in its
// locals, its root at its own reference offset (o280001's is raised), and for a lever whose clip
// stays pulled (o253010) or ends partway (o321110), a completed pull that ends there.
void RunInstalledDetours(bool motion = false, s32 supported_action = 2410000,
                         const VR::Lever::Model &rig_model = VR::Lever::Models[0]) {
    using namespace Xbyak::util;
    Module module;
    ASSERT_NE(module.data, nullptr);
    module.Seed();
    std::ostringstream messages;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(messages);
    Common::Log::ALL_LOGGERS[Common::Log::Class::Core] =
        std::make_shared<spdlog::logger>("probe", sink);

    // Synthetic continuations consume exactly the registers/flags produced by the stolen code.
    // The original selection stores eax and sets ZF from the normal interact-button result.
    Xbyak::CodeGenerator selection_tail(32, module.data + Select + SelectBytes.size());
    selection_tail.setz(dl);
    selection_tail.movzx(edx, dl);
    selection_tail.shl(rdx, 32);
    selection_tail.or_(rax, rdx);
    selection_tail.ret();
    Xbyak::CodeGenerator permission_tail(32, module.data + Permission + PermissionBytes.size());
    permission_tail.setnz(dl);
    permission_tail.movzx(edx, dl);
    permission_tail.shl(rdx, 32);
    permission_tail.or_(rax, rdx);
    permission_tail.ret();
    Xbyak::CodeGenerator event_tail(64, module.data + Event + EventBytes.size());
    event_tail.add(rsp, 0x1A8);
    for (const auto &r : {rbx, r12, r13, r14, r15, rbp})
        event_tail.pop(r);
    event_tail.mov(rax, rdx);
    event_tail.ret();

    _putenv_s("VR_LEVER_PROBE", "1");
    _putenv_s("VR_LEVER_PHYSICAL_TEST", "");
    _putenv_s("VR_LEVER_MOTION_TEST", motion ? "1" : "0");
    module.Install();
    _putenv_s("VR_LEVER_MOTION_TEST", "");
    _putenv_s("VR_LEVER_PROBE", "");
    _putenv_s("VR_LEVER_PHYSICAL_TEST", "");
    ASSERT_EQ(module.data[Select], 0xFF) << messages.str();
    ASSERT_EQ(module.data[Permission], 0xFF);
    ASSERT_EQ(module.data[Event], 0xFF);

    // The script path includes used levers. Exercise its actual detour with an
    // object, actor/null target, another four-bone model, and the setting disabled.
    Xbyak::CodeGenerator prompt_entry(256);
    prompt_entry.push(r12);
    prompt_entry.sub(rsp, 32);
    prompt_entry.mov(r12, rdi);
    prompt_entry.mov(rax, 0x12345678);
    prompt_entry.mov(rdx, 0x87654321);
    prompt_entry.stc();
    prompt_entry.mov(r11, reinterpret_cast<u64>(module.data + ScriptPrompt));
    prompt_entry.jmp(r11);
    auto cleanup = reinterpret_cast<u64>(prompt_entry.getCurr());
    prompt_entry.add(rsp, 32);
    prompt_entry.pop(r12);
    prompt_entry.ret();
    prompt_entry.ready();
    const auto prompt_tail = [&](size_t at, int offered) {
        Xbyak::CodeGenerator c(48, module.data + at);
        c.mov(ptr[rsi], rax);
        c.mov(ptr[rsi + 8], rdx);
        c.pushfq(); c.pop(rax); c.mov(ptr[rsi + 16], rax);
        c.mov(eax, offered);
        c.mov(r11, cleanup); c.jmp(r11);
    };
    prompt_tail(ScriptPrompt + ScriptPromptBytes.size(), 1);
    prompt_tail(0x144E7D9, 0);
    std::array<u8, 0x2E0> prompt_object{};
    std::array<u8, 0x40> prompt_controller{}, prompt_node{}, prompt_skeleton{}, prompt_bones{};
    std::array<char, 8> prompt_name{'o','2','4','0','0','0','1',0};
    Put<u64>(prompt_object, 0x290, reinterpret_cast<u64>(prompt_controller.data()));
    Put<u64>(prompt_controller, 0x20, reinterpret_cast<u64>(prompt_node.data()));
    Put<u64>(prompt_node, 0x38, reinterpret_cast<u64>(prompt_skeleton.data()));
    Put<u64>(prompt_skeleton, 0x28, reinterpret_cast<u64>(prompt_bones.data()));
    Put<s32>(prompt_skeleton, 0x30, 4);
    Put<u64>(prompt_bones, 0, reinterpret_cast<u64>(prompt_name.data()));
    using PromptFn = int(PS4_SYSV_ABI *)(u8 *, u64 *);
    auto prompt = prompt_entry.getCode<PromptFn>();
    std::array<u64, 3> preserved{};
    VR::Lever::SetEnabled(true);
    auto &held_object = VR::Lever::Channel().held_object;
    if (motion) {
        // Rehearsal pulls used levers by hand: their script prompt is hidden on the skeleton.
        EXPECT_EQ(prompt(prompt_object.data(), preserved.data()), 0);
        EXPECT_EQ(preserved[0], 0x12345678);
        EXPECT_EQ(preserved[1], 0x87654321);
        EXPECT_EQ(preserved[2] & 1, 1);
        EXPECT_EQ(prompt(nullptr, preserved.data()), 1);
        prompt_name[6] = '2';
        EXPECT_EQ(prompt(prompt_object.data(), preserved.data()), 1);
        prompt_name[6] = '1';
        Put<s32>(prompt_skeleton, 0x30, 5);
        EXPECT_EQ(prompt(prompt_object.data(), preserved.data()), 1);
        Put<s32>(prompt_skeleton, 0x30, 4);
    } else {
        // Ordinary launches: a used lever ("Device is not currently operable"), or any lever
        // with no hand pull, keeps its script prompt, even on the measured o240001 skeleton.
        EXPECT_EQ(prompt(prompt_object.data(), preserved.data()), 1);
        EXPECT_EQ(preserved[0], 0x12345678);
        EXPECT_EQ(preserved[1], 0x87654321);
        EXPECT_EQ(preserved[2] & 1, 1);
        // Only the very object a physical hold owns has it hidden (registers kept either way).
        held_object.store(reinterpret_cast<u64>(prompt_object.data()));
        EXPECT_EQ(prompt(prompt_object.data(), preserved.data()), 0);
        EXPECT_EQ(preserved[0], 0x12345678);
        EXPECT_EQ(preserved[1], 0x87654321);
        EXPECT_EQ(preserved[2] & 1, 1);
        std::array<u8, 0x2E0> other_object = prompt_object;
        EXPECT_EQ(prompt(other_object.data(), preserved.data()), 1);
        EXPECT_EQ(prompt(nullptr, preserved.data()), 1);
        VR::Lever::SetEnabled(false);
        EXPECT_EQ(prompt(prompt_object.data(), preserved.data()), 1);
        VR::Lever::SetEnabled(true);
        held_object.store(0);
    }
    VR::Lever::SetEnabled(false);
    EXPECT_EQ(prompt(prompt_object.data(), preserved.data()), 1);
    VR::Lever::SetEnabled(true);
    std::array<u8, 0x140> row{};
    std::array<u8, 0xD8> block{};
    std::array<u8, 0x30> param{};
    Put<s32>(param, 0x0C, 101150);
    Put<u64>(row, 0x88, reinterpret_cast<u64>(param.data()));
    Put<s32>(row, 0x80, supported_action);
    Put<u32>(row, 0x58, 0xFFFFFFFF); // Deliberately unresolved object must not be dereferenced.
    Put<u64>(block, 0x90, reinterpret_cast<u64>(row.data()));
    Put<s32>(block, 0x98, 1);

    // Guest stack locals and callee-saved registers match 13ADCC0 at both mid-function sites.
    // rdi=row, rsi=block, rdx=actor, rcx=the original eax/permission result.
    Xbyak::CodeGenerator selection(1024);
    const auto emit = [&](Xbyak::CodeGenerator &c, size_t site) {
        for (const auto &r : {rbp, rbx, r13, r15})
            c.push(r);
        c.mov(rbp, rsp);
        c.sub(rsp, 0x80);
        c.mov(ptr[rbp - 0x70], rdi);
        c.mov(qword[rbp - 0x78], 0);
        c.xor_(ebx, ebx);
        c.mov(r13, rsi);
        c.mov(r15, rdx);
        c.mov(rax, rcx);
        c.mov(r11, reinterpret_cast<u64>(module.data + site));
        c.call(r11);
        c.mov(rsp, rbp);
        for (const auto &r : {r15, r13, rbx, rbp})
            c.pop(r);
        c.ret();
        c.ready();
    };
    emit(selection, Select);
    Xbyak::CodeGenerator permission(1024);
    emit(permission, Permission);
    using MidFn = u64(PS4_SYSV_ABI *)(u8 *, u8 *, u64, u64);
    auto selected_entry = selection.getCode<MidFn>();
    auto allowed = permission.getCode<MidFn>();
    // The XR side refreshes its "a hand can pull a lever" word every frame in the game; here,
    // with every selection pass (selected_entry alone leaves it as it is).
    const auto selected = [&](u8 *r, u8 *b, u64 actor_address, u64 result) -> u64 {
        VR::Lever::Channel().hand_pull_ready.store(SteadyNs());
        return selected_entry(r, b, actor_address, result);
    };
    VR::Lever::SetEnabled(false);
    for (int accepted = 0; accepted < 2; ++accepted) {
        block[0x9C] = static_cast<u8>(accepted);
        const u64 value = selected(row.data(), block.data(), 0x12345678, 0xA5B6);
        EXPECT_EQ(value, 0xA5B6ULL | (static_cast<u64>(!accepted) << 32));
        u32 stored{};
        std::memcpy(&stored, block.data() + 0xA8, 4);
        EXPECT_EQ(stored, 0xA5B6);
        EXPECT_EQ(allowed(row.data(), block.data(), 0x12345678, accepted),
                  static_cast<u64>(accepted) | (static_cast<u64>(accepted) << 32));
    }
    // Enabled clears the next prompt and current button result only for a lever instance whose
    // rig binds. A listed row whose object cannot be resolved (or has no rig) keeps them, and so
    // does every other action.
    VR::Lever::SetEnabled(true);
    // The actor the selector passes (r15): the main player once the game's tables are seeded.
    u64 prompt_actor = 0x12345678;
    const auto expect_prompt = [&](bool hidden, const char *what, bool ready = true) {
        block[0x9C] = block[0x9D] = 1;
        Put<s32>(block, 0xA0, 101);
        Put<s32>(block, 0xA4, 102);
        if (ready)
            VR::Lever::Channel().hand_pull_ready.store(SteadyNs());
        EXPECT_EQ(selected_entry(row.data(), block.data(), prompt_actor, 0xA5B6),
                  0xA5B6ULL | (static_cast<u64>(hidden) << 32))
            << what;
        EXPECT_EQ(block[0x9C], hidden ? 0 : 1) << what;
        EXPECT_EQ(block[0x9D], hidden ? 0 : 1) << what;
        s32 prompt{}, button{}; u32 stored{};
        std::memcpy(&prompt, block.data() + 0xA0, 4);
        std::memcpy(&button, block.data() + 0xA4, 4);
        std::memcpy(&stored, block.data() + 0xA8, 4);
        EXPECT_EQ(prompt, hidden ? -1 : 101) << what;
        EXPECT_EQ(button, hidden ? -1 : 102) << what;
        EXPECT_EQ(stored, hidden ? 0 : 0xA5B6) << what;
    };
    for (s32 action : {2400000, 2410000, 2420000, 100, 2410001, 2410020, 12345}) {
        Put<s32>(row, 0x80, action);
        expect_prompt(false, "unresolved object");
    }
    Put<s32>(row, 0x80, supported_action);
    EXPECT_NE(messages.str().find("candidate"), std::string::npos);
    EXPECT_NE(messages.str().find("permission=0"), std::string::npos);
    EXPECT_NE(messages.str().find("permission=1"), std::string::npos);
    EXPECT_NE(messages.str().find("actor=0x12345678"), std::string::npos);
    EXPECT_NE(messages.str().find("object_ok=false"), std::string::npos);

    using EventFn = u64(PS4_SYSV_ABI *)(u8 *, s32 *, u32);
    auto receive = reinterpret_cast<EventFn>(module.data + Event);
    std::array<s32, 8> record{17, 0, -1, 0x1020304, 41, 51, 61, 71};
    EXPECT_EQ(receive(block.data(), record.data(), 0), 0);
    EXPECT_EQ(receive(block.data(), record.data(), 1), 1);
    EXPECT_NE(messages.str().find("event_local"), std::string::npos);
    EXPECT_NE(messages.str().find("event_remote"), std::string::npos);
    EXPECT_NE(messages.str().find("event words=11,0,"), std::string::npos);
    // Resolve a nonzero map block and object index through the independently seeded tables.
    alignas(8) std::array<u8, 0x30> manager{};
    alignas(8) std::array<u8, 2 * 0x1F8> maps{};
    alignas(8) std::array<u8, 2 * 0x2E0> objects{};
    const u64 manager_address = reinterpret_cast<u64>(manager.data());
    std::memcpy(module.data + 0x553B168, &manager_address, sizeof(manager_address));
    Put<s32>(manager, 0x20, 2);
    Put<u64>(manager, 0x28, reinterpret_cast<u64>(maps.data()));
    Put<s32>(maps, 0x1F8 + 0xC8, 2);
    Put<u64>(maps, 0x1F8 + 0xD0, reinterpret_cast<u64>(objects.data()));
    Put<s32>(objects, 0x2E0 + 0x208, 7100);
    Put<float>(objects, 0x2E0 + 0x24C, 1.25f);
    Put<float>(objects, 0x2E0 + 0x25C, 2.5f);
    Put<float>(objects, 0x2E0 + 0x26C, -3.75f);
    Put<u32>(row, 0x58, 0x100001);
    const auto original_objects = objects;
    EXPECT_EQ(allowed(row.data(), block.data(), 0, 1), 0x100000001ULL);
    EXPECT_NE(messages.str().find("object_ok=true"), std::string::npos);
    EXPECT_NE(messages.str().find("object_208=7100"), std::string::npos);
    EXPECT_NE(messages.str().find("pos=(1.2500,2.5000,-3.7500)"), std::string::npos);
    EXPECT_EQ(objects, original_objects);
    Put<u32>(row, 0x58, 0x200000); // block == count is outside the table
    EXPECT_EQ(allowed(row.data(), block.data(), 0, 0), 0);
    Put<u32>(row, 0x58, 0x100002); // object index == count is outside the table
    EXPECT_EQ(allowed(row.data(), block.data(), 0, 0), 0);

    // Read failures and invalid event indices are harmless; the original path still resumes.
    Put<u64>(row, 0x88, 0xDEAD0000);
    EXPECT_EQ(allowed(row.data(), block.data(), 0, 1), 0x100000001ULL);
    record[1] = -1;
    EXPECT_EQ(receive(block.data(), record.data(), 1), 1);
    record[1] = 1;
    EXPECT_EQ(receive(block.data(), record.data(), 0), 0);
    EXPECT_EQ(receive(block.data(), reinterpret_cast<s32 *>(0xDEAD0000), 1), 1);
    // The pose detour relocates a real call rather than blindly copying its relative offset.
    // A synthetic original sync records its pose argument and incoming rax, and runs once.
    ASSERT_EQ(module.data[Pose], 0xFF);
    Xbyak::CodeGenerator sync_body(64, module.data + Sync);
    sync_body.inc(qword[rdi + 0x70]);
    sync_body.mov(ptr[rdi + 0x78], rax);
    sync_body.ret();
    Xbyak::CodeGenerator pose_tail(16, module.data + Pose + PoseBytes.size());
    pose_tail.mov(rax, rdi);
    pose_tail.ret();
    std::array<u8, 0x200> node{};
    Put<u64>(objects, 0x2A8, 0xABCDEF);
    Xbyak::CodeGenerator pose_entry(256);
    for (const auto &r : {rbp, rbx, r14})
        pose_entry.push(r);
    pose_entry.sub(rsp, 8); // mid-function call site must enter with rsp = 0 mod 16
    pose_entry.mov(r14, rdi);
    pose_entry.mov(rbx, rsi);
    pose_entry.mov(rax, 0x112233);
    pose_entry.mov(r11, reinterpret_cast<u64>(module.data + Pose));
    pose_entry.call(r11);
    pose_entry.add(rsp, 8);
    for (const auto &r : {r14, rbx, rbp})
        pose_entry.pop(r);
    pose_entry.ret();
    using PoseFn = u64(PS4_SYSV_ABI *)(u8 *, u8 *);
    EXPECT_EQ(pose_entry.getCode<PoseFn>()(objects.data(), node.data()), 0xABCDEF);
    u64 calls{}, incoming{};
    std::memcpy(&calls, node.data() + 0x38 + 0x70, 8);
    std::memcpy(&incoming, node.data() + 0x38 + 0x78, 8);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(incoming, 0x112233);
    // Exercise the actual guarded four-bone reader on the object selected above.
    std::array<u8, 0x50> skeleton{};
    std::array<u8, 4 * 16> bone_names{};
    alignas(16) std::array<std::array<float, 12>, 4> locals{};
    constexpr std::array<const char *, 4> names{"o240001", "o240001_00", "o240001_01",
                                                "o240001_02"};
    for (size_t i = 0; i < 4; ++i) {
        Put<u64>(bone_names, i * 16, reinterpret_cast<u64>(names[i]));
        locals[i][7] = 1;
        locals[i][8] = locals[i][9] = locals[i][10] = 1;
    }
    Put<u64>(skeleton, 0x28, reinterpret_cast<u64>(bone_names.data()));
    Put<s32>(skeleton, 0x30, 4);
    Put<u64>(node, 0x38, reinterpret_cast<u64>(skeleton.data()));
    Put<u64>(node, 0x40, reinterpret_cast<u64>(locals.data()));
    Put<s32>(node, 0x48, 4);
    Put<u64>(objects, 0x2E0 + 0x2A8, 0xCDEF);
    const auto before_locals = locals;
    EXPECT_EQ(pose_entry.getCode<PoseFn>()(objects.data() + 0x2E0, node.data()), 0xCDEF);
    EXPECT_NE(messages.str().find("anim=7100 bone=o240001_00"), std::string::npos);
    EXPECT_EQ(locals, before_locals);
    // An idle supported rig must be discoverable without ever becoming the selected action.
    std::array<u8, 0x2E0> idle_object{};
    Put<u64>(idle_object, 0x2A8, 0xFEDC);
    Put<s32>(idle_object, 0x208, 7200);
    const auto before_idle = idle_object;
    messages.str("");
    EXPECT_EQ(pose_entry.getCode<PoseFn>()(idle_object.data(), node.data()), 0xFEDC);
    EXPECT_NE(messages.str().find("selected=false"), std::string::npos);
    EXPECT_NE(messages.str().find("anim=7200 bone=o240001_00"), std::string::npos);
    EXPECT_EQ(idle_object, before_idle);
    EXPECT_EQ(locals, before_locals);
    // The scheduler must still tick an empty queue. All six register arguments and
    // the seventh stack argument survive the new entry detour and original prologue.
    Xbyak::CodeGenerator update_tail(128, module.data + Update + UpdateBytes.size());
    update_tail.mov(rax, ptr[rbp + 0x10]);
    update_tail.add(rax, rdx);
    update_tail.add(rax, rcx);
    update_tail.add(rax, r8);
    update_tail.add(rax, r9);
    update_tail.mov(ptr[rdi + 0x38], rsi);
    update_tail.add(rsp, 0x38);
    for (const auto &r : {rbx, r12, r13, r14, r15, rbp})
        update_tail.pop(r);
    update_tail.ret();
    using UpdateFn = u64(PS4_SYSV_ABI *)(u8 *, u8 *, u64, u64, u64, u64, u64);
    std::array<u8, 0x40> queue{};
    std::array<u8, 4 * 16> entries{};
    std::array<u16, 4> indices{0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE};
    Put<u32>(queue, 0, 4);
    Put<u64>(queue, 8, reinterpret_cast<u64>(entries.data()));
    Put<u64>(queue, 0x18, reinterpret_cast<u64>(indices.data()));
    messages.str("");
    const auto before_queue = queue;
    auto update = reinterpret_cast<UpdateFn>(module.data + Update);
    EXPECT_EQ(update(queue.data(), objects.data(), 3, 5, 7, 11, 13), 39u);
    EXPECT_NE(messages.str().find("capacity=4 pending=0"), std::string::npos);
    EXPECT_EQ(std::memcmp(queue.data(), before_queue.data(), 0x38), 0);
    u64 forwarded_objects{};
    std::memcpy(&forwarded_objects, queue.data() + 0x38, 8);
    EXPECT_EQ(forwarded_objects, reinterpret_cast<u64>(objects.data()));
    EXPECT_EQ(indices, (std::array<u16, 4>{0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE}));
    // Exhaust the diagnostic sample, then make entry into its host stub fatal.
    // This proves idle calls bypass the entire stub, not just its C++ body/logging.
    for (int i = 1; i < 8; ++i) {
        EXPECT_EQ(update(queue.data(), objects.data(), 3, 5, 7, 11, 13), 39u);
    }
    u8 *gate{};
    std::memcpy(&gate, module.data + Update + 6, sizeof(gate));
    u8 *callback{};
    for (size_t i = 0; i < 512; ++i) {
        if (gate[i] == 0xFF && gate[i + 1] == 0x25 && gate[i + 2] == 0 && gate[i + 3] == 0 &&
            gate[i + 4] == 0 && gate[i + 5] == 0) {
            std::memcpy(&callback, gate + i + 6, sizeof(callback));
            break;
        }
    }
    ASSERT_NE(callback, nullptr);
    const std::array<u8, 2> saved{callback[0], callback[1]};
    callback[0] = 0x0F; // ud2: any unexpected entry fails the native test.
    callback[1] = 0x0B;
    FlushInstructionCache(GetCurrentProcess(), callback, 2);
    messages.str("");
    EXPECT_EQ(update(queue.data(), objects.data(), 17, 19, 23, 29, 31), 119u);
    EXPECT_TRUE(messages.str().empty());
    EXPECT_EQ(std::memcmp(queue.data(), before_queue.data(), 0x38), 0);
    std::memcpy(callback, saved.data(), saved.size());
    FlushInstructionCache(GetCurrentProcess(), callback, 2);
    // Exercise the live resolver and consumer through their real detours. Guest helper
    // stubs report permission and record sync/publication; the pose and queue are real memory.
    namespace L = VR::Lever;
    namespace B = VR::Body;
    auto &channel = L::Channel();
    ASSERT_TRUE(channel.installed.load());
    const auto now = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    u8 permission_value = motion ? 0 : 1;
    u64 sync_calls{}, dirty_calls{}, dirty_node{}, permission_calls{};
    Xbyak::CodeGenerator check_body(64, module.data + Check);
    check_body.mov(rax, reinterpret_cast<u64>(&permission_calls));
    check_body.inc(qword[rax]);
    check_body.mov(rax, reinterpret_cast<u64>(&permission_value));
    check_body.movzx(eax, byte[rax]);
    check_body.ret();
    // The game's interaction gate: its bitmask (0: free), past its static initialization.
    u32 blocked_value = 0;
    Xbyak::CodeGenerator blocked_body(64, module.data + Blocked);
    blocked_body.mov(rax, reinterpret_cast<u64>(&blocked_value));
    blocked_body.mov(eax, dword[rax]);
    blocked_body.ret();
    alignas(8) std::array<u8, 0x400> menu_state{};
    const u64 menu_address = reinterpret_cast<u64>(menu_state.data());
    std::memcpy(module.data + BlockedState, &menu_address, sizeof(menu_address));
    module.data[BlockedInit] = 1;
    Xbyak::CodeGenerator physical_sync(64, module.data + Sync);
    physical_sync.mov(rax, reinterpret_cast<u64>(&sync_calls));
    physical_sync.inc(qword[rax]);
    physical_sync.mov(byte[rdi + 0x38], 1);
    physical_sync.ret();
    Xbyak::CodeGenerator dirty_body(96, module.data + Dirty);
    dirty_body.mov(rax, reinterpret_cast<u64>(&dirty_calls));
    dirty_body.inc(qword[rax]);
    dirty_body.mov(rax, reinterpret_cast<u64>(&dirty_node));
    dirty_body.mov(qword[rax], rdi);
    dirty_body.mov(dword[rdi + 0x10], 0); // synthetic renderer consumes its update
    dirty_body.ret();
    alignas(16) std::array<B::HkQsTransform, 4> model_bones{};
    std::array<u32, 4> bone_flags{};
    std::array<s16, 4> parents = rig_model.parents;
    std::array<u8, 0x30> controller{};
    std::array<u8, 0x120> model{};
    alignas(8) std::array<u8, 0x80> matrix_node{};
    std::array<u8, 0x10> mapping{};
    // The lever object now carries the rig model's own skeleton (names, parents, rest pose), with
    // its own Havok bone count in every table the reader checks, and its own FLVER -> Havok mesh
    // table (as many entries as its mesh has bones: o260040 2 over 4 Havok bones, o321110 4 over 3).
    ASSERT_TRUE(rig_model.count == 3 || rig_model.count == 4) << rig_model.name;
    const s32 bones = static_cast<s32>(rig_model.count);
    const size_t lever = rig_model.lever;
    ASSERT_TRUE(lever >= 1 && lever < rig_model.count) << rig_model.name;
    std::array<s16, 4> mesh_map = rig_model.mesh_map;
    for (size_t i = 0; i < 4; ++i)
        Put<u64>(bone_names, i * 16, reinterpret_cast<u64>(rig_model.bones[i]));
    Put<s32>(skeleton, 0x30, bones);
    Put<s32>(node, 0x48, bones);
    Put<u64>(matrix_node, 0, reinterpret_cast<u64>(module.data + 0x53A12B0));
    Put<u8>(matrix_node, 0xC, 1);
    Put<u64>(matrix_node, 0x60, reinterpret_cast<u64>(node.data() + 0x18));
    Put<u64>(matrix_node, 0x78, reinterpret_cast<u64>(mapping.data()));
    Put<u32>(mapping, 0, rig_model.mesh_count);
    Put<u64>(mapping, 8, reinterpret_cast<u64>(mesh_map.data()));
    Put<u64>(objects, 0x2E0 + 0x2A0, reinterpret_cast<u64>(matrix_node.data()));
    Put<u64>(objects, 0x2E0 + 0x298, reinterpret_cast<u64>(mapping.data()));
    std::array<u8, 0x60> action_vtable{};
    std::array<u8, 0x18> actor{};
    Put<u64>(action_vtable, 0x58, reinterpret_cast<u64>(module.data + Check));
    Put<u64>(controller, 0, reinterpret_cast<u64>(action_vtable.data()));
    Put<u64>(controller, 0x20, reinterpret_cast<u64>(node.data()));
    Put<u64>(actor, 0, reinterpret_cast<u64>(action_vtable.data()));
    Put<u64>(model, 0x108, 0); // levers have no cloth; it must not be needed
    Put<u64>(skeleton, 0x18, reinterpret_cast<u64>(parents.data()));
    Put<s32>(skeleton, 0x20, bones);
    Put<u64>(node, 0x50, reinterpret_cast<u64>(model_bones.data()));
    Put<s32>(node, 0x58, bones);
    Put<u64>(node, 0x60, reinterpret_cast<u64>(bone_flags.data()));
    Put<s32>(node, 0x68, bones);
    Put<u16>(node, 0x70, 0x0101);
    // The lever bone's ancestors at their rests: the root at its reference offset (zero but
    // o280001's), unrotated for every lever stick; o353010's root turned -90 degrees about X and
    // its column turned back. `parent_t` / `parent_q`: the lever bone's parent in the object.
    const auto chain = L::Chain(rig_model);
    ASSERT_GT(chain.n, 0u) << rig_model.name;
    B::Vec3 parent_t{};
    B::Quat parent_q{};
    for (size_t i = 0; i < chain.n; ++i) {
        const size_t b = chain.idx[i];
        const auto &r = rig_model.chain_rest[i];
        const B::Vec3 t = i == 0 ? r.t + rig_model.root_t : r.t;
        locals[b][0] = t.x;
        locals[b][1] = t.y;
        locals[b][2] = t.z;
        locals[b][4] = r.q.x;
        locals[b][5] = r.q.y;
        locals[b][6] = r.q.z;
        locals[b][7] = r.q.w;
        parent_t = parent_t + B::Rotate(parent_q, t);
        parent_q = parent_q * r.q;
    }
    const bool capstan = rig_model.motion == L::Motion::Capstan;
    locals[lever][0] = rig_model.rest_t.x;
    locals[lever][1] = rig_model.rest_t.y;
    locals[lever][2] = rig_model.rest_t.z;
    // The game holds a unit quaternion; a measured value can be a float rounding off unit
    // (o250010's, o280001's), which the pull's own normalization would then not return to bit for
    // bit after a settle.
    const auto unit_rest = B::Normalize(rig_model.rest_q);
    locals[lever][4] = unit_rest.x;
    locals[lever][5] = unit_rest.y;
    locals[lever][6] = unit_rest.z;
    locals[lever][7] = unit_rest.w;
    // The lever bone's own constant scale (Cainhurst's and the Fishing Hamlet's are not 1): the
    // game holds it in the bone's locals (its clips carry it as a constant track).
    locals[lever][8] = rig_model.rest_s.x;
    locals[lever][9] = rig_model.rest_s.y;
    locals[lever][10] = rig_model.rest_s.z;
    const auto rest = locals;
    // A bone-local mesh point as drawn (the bone's own scale first).
    const auto scaled = [&](B::Vec3 v) { return B::Mul(rig_model.rest_s, v); };
    // The lever bone the pull writes at `angle` (the rig turns it about its own measured axis).
    const auto pulled = [&](float angle) {
        return B::Normalize(rig_model.rest_q * B::AxisAngle(rig_model.axis, angle));
    };
    const auto expect_bone = [&](float angle, const char *what) {
        const auto q = pulled(angle);
        EXPECT_NEAR(locals[lever][4], q.x, 0.00001f) << what;
        EXPECT_NEAR(locals[lever][5], q.y, 0.00001f) << what;
        EXPECT_NEAR(locals[lever][6], q.z, 0.00001f) << what;
        EXPECT_NEAR(locals[lever][7], q.w, 0.00001f) << what;
        // The pull only turns it: its translation and its own scale stay, bit for bit.
        for (const size_t k : {0, 1, 2, 8, 9, 10})
            EXPECT_EQ(locals[lever][k], rest[lever][k]) << what << " component " << k;
    };
    // The lever bone's signed angle from rest about the rig's own axis (any rest and hinge).
    const auto pull_angle = [&] {
        const B::Quat now_q{locals[lever][4], locals[lever][5], locals[lever][6], locals[lever][7]};
        const auto relative = B::Conjugate(B::Normalize(rig_model.rest_q)) * B::Normalize(now_q);
        return 2.0f * std::atan2(B::Dot(B::Vec3{relative.x, relative.y, relative.z},
                                        B::Normalize(rig_model.axis)),
                                 relative.w);
    };
    const B::Vec3 object_at{1.25f, 2.5f, -3.75f};
    const B::Vec3 pivot = object_at + parent_t + B::Rotate(parent_q, rig_model.rest_t);
    // The lever bone at `q` (its local rotation) in the object (its parent's rotation first).
    const auto in_object = [&](B::Quat q) { return B::Normalize(parent_q * q); };
    Put<s32>(objects, 0x2E0 + 0x208, -1);
    Put<u64>(objects, 0x2E0 + 0x290, reinterpret_cast<u64>(controller.data()));
    Put<u64>(objects, 0x2E0 + 0x270, reinterpret_cast<u64>(model.data()));
    Put<u64>(objects, 0x2E0 + 0x10, reinterpret_cast<u64>(entries.data() + 16));
    for (size_t offset : {0x240, 0x254, 0x268})
        Put<float>(objects, 0x2E0 + offset, 1.0f);
    Put<u64>(entries, 16, reinterpret_cast<u64>(queue.data()));
    Put<u16>(entries, 24, 1);
    Put<u64>(row, 0x88, reinterpret_cast<u64>(param.data()));
    Put<u64>(row, 0x90, reinterpret_cast<u64>(controller.data()));
    Put<u32>(row, 0x58, 0x100001);
    Put<s32>(row, 0x80, supported_action);
    // The game's own tables: the player and the ObjAct manager with this one action block.
    std::array<u8, 0x68> world_manager{};
    std::array<u8, 0x30> action_manager{};
    const u64 world_address = reinterpret_cast<u64>(world_manager.data());
    const u64 actions_address = reinterpret_cast<u64>(action_manager.data());
    std::memcpy(module.data + 0x553E878, &world_address, 8);
    std::memcpy(module.data + 0x553B0F0, &actions_address, 8);
    Put<u64>(world_manager, 0x60, reinterpret_cast<u64>(actor.data()));
    Put<s32>(action_manager, 0x20, 1);
    Put<u64>(action_manager, 0x28, reinterpret_cast<u64>(block.data()));
    Put<u32>(actor, 8, 0x1234);
    prompt_actor = reinterpret_cast<u64>(actor.data());
    L::Offer offered{};
    const auto hint_before = channel.selected_hint.load();
    selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
    EXPECT_FALSE(channel.LatestOffer(offered));
    EXPECT_EQ(permission_calls, 0u);
    // This instance's rig binds: the prompt and button are replaced, and the hint advances.
    expect_prompt(true, "bound instance");
    EXPECT_GT(channel.selected_hint.load(), hint_before);
    EXPECT_NE(messages.str().find("pulled by hand, prompt hidden"), std::string::npos);
    // The same row with another object (no rig at index 0) keeps its prompt and no hint.
    Put<u32>(row, 0x58, 0x100000);
    const auto hint_unbound = channel.selected_hint.load();
    expect_prompt(false, "other object on the same row");
    expect_prompt(false, "other object on the same row, again");
    EXPECT_EQ(channel.selected_hint.load(), hint_unbound);
    Put<u32>(row, 0x58, 0x100001);
    expect_prompt(true, "bound instance again");
    // A model without a measured rig for this row (another skeleton on a listed, generic row)
    // keeps the prompt, re-checked as the game keeps selecting it.
    {
        // A root bone no model has (the Chalice call levers' "o9902"): no rig under these names.
        static constexpr const char *unmeasured = "o9902";
        Put<u64>(bone_names, 0, reinterpret_cast<u64>(unmeasured));
        channel.verdict.recheck = 1; // due on the next pass
        expect_prompt(false, "unmeasured model");
        Put<u64>(bone_names, 0, reinterpret_cast<u64>(rig_model.bones[0]));
        channel.verdict.recheck = 1;
        expect_prompt(true, "measured model restored");
    }
    // The mesh table is the model's own, entry for entry: one entry more or fewer, the lever's
    // FLVER bone drawn from another Havok bone, an unmapped entry (o321110's mechanism block)
    // drawn from a real bone, or the lever bone at another scale than its own: the prompt stays,
    // and the first such miss for this model and row says what the game holds (once).
    {
        const auto mismatch = [&](const char *what, auto &&change, auto &&undo) {
            change();
            channel.verdict.recheck = 1;
            expect_prompt(false, what);
            undo();
            channel.verdict.recheck = 1;
            expect_prompt(true, what);
        };
        for (const s32 delta : {-1, 1})
            mismatch(
                "a mesh table one entry shorter or longer",
                [&] { Put<u32>(mapping, 0, static_cast<u32>(rig_model.mesh_count + delta)); },
                [&] { Put<u32>(mapping, 0, rig_model.mesh_count); });
        for (u32 f = 0; f < rig_model.mesh_count; ++f) {
            if (rig_model.mesh_map[f] < 0) {
                // Any negative entry is the game's "no Havok bone"; a real bone is another mesh.
                mesh_map[f] = -2;
                channel.verdict.recheck = 1;
                expect_prompt(true, "an unmapped entry, another negative value");
                mismatch(
                    "an unmapped entry drawn from the root", [&] { mesh_map[f] = 0; },
                    [&] { mesh_map[f] = rig_model.mesh_map[f]; });
            } else if (rig_model.mesh_map[f] == static_cast<s16>(lever)) {
                mismatch(
                    "the lever's FLVER bone drawn from another bone",
                    [&] { mesh_map[f] = static_cast<s16>((lever + 1) % rig_model.count); },
                    [&] { mesh_map[f] = rig_model.mesh_map[f]; });
            }
        }
        mismatch(
            "the lever bone at another scale",
            [&] {
                const bool unit = rig_model.rest_s.x == 1 && rig_model.rest_s.y == 1;
                locals[lever][8] = locals[lever][10] = unit ? 0.8077924f : 1.0f;
                locals[lever][9] = unit ? 0.4565846f : 1.0f;
            },
            [&] { locals = rest; });
        EXPECT_NE(messages.str().find(fmt::format("VR lever: {} bones on row {} do not bind (logged "
                                                  "once): mesh table",
                                                  rig_model.name, supported_action)),
                  std::string::npos);
        EXPECT_EQ(locals, rest);
    }
    // The Chalice Dungeon levers are not hand-pulled (main game only): their rows keep the game's
    // prompt even on this bound lever object, and so does o290020's skeleton (o250010's bone names
    // under a four-entry mesh table, its root turned -90 degrees about Y and its hinge 0.414 m up),
    // each difference on its own, on the Cainhurst rows.
    for (const s32 chalice : {9902, 320, 2902000, 2931100}) {
        Put<s32>(row, 0x80, chalice);
        channel.verdict.recheck = 1;
        expect_prompt(false, "a Chalice Dungeon row");
    }
    Put<s32>(row, 0x80, supported_action);
    channel.verdict.recheck = 1;
    expect_prompt(true, "own row after the Chalice rows");
    if (std::strcmp(rig_model.bones[0], "lever") == 0) {
        const std::array<s16, 4> door_table{0, 1, -1, -1};
        const auto door_table_on = [&] {
            Put<u32>(mapping, 0, 4u);
            mesh_map = door_table;
        };
        const auto door_root_on = [&] {
            locals[0][5] = -0.7074513f;
            locals[0][7] = 0.7067621f;
        };
        const auto door_hinge_on = [&] { locals[lever][1] = 0.4144199f; };
        const auto undo = [&] {
            Put<u32>(mapping, 0, rig_model.mesh_count);
            mesh_map = rig_model.mesh_map;
            locals = rest;
        };
        for (const auto &change : std::array<std::function<void()>, 3>{door_table_on, door_root_on,
                                                                       door_hinge_on}) {
            change();
            channel.verdict.recheck = 1;
            expect_prompt(false, "one of o290020's differences from Cainhurst's lever");
            undo();
        }
        door_table_on();
        door_root_on();
        door_hinge_on();
        for (const s32 any : {supported_action, 2500010, 2500000, 2902000, 9902, 320}) {
            Put<s32>(row, 0x80, any);
            channel.verdict.recheck = 1;
            expect_prompt(false, "o290020's skeleton");
        }
        Put<s32>(row, 0x80, supported_action);
        undo();
        channel.verdict.recheck = 1;
        expect_prompt(true, "Cainhurst's lever again");
    }
    // A listed row of another model's (o240001's rows on this rig, or the reverse) keeps it too:
    // a row no model with this skeleton and rest (the rig model or its twins) is listed with.
    const auto binds_row = [&](s32 other) {
        for (const auto &m : L::Models) {
            bool same = m.count == rig_model.count && m.mesh_count == rig_model.mesh_count &&
                        m.mesh_map == rig_model.mesh_map && m.lever == rig_model.lever &&
                        B::Length(m.rest_t - rig_model.rest_t) < 0.001f &&
                        B::Length(m.rest_s - rig_model.rest_s) < 0.001f;
            for (u32 i = 0; same && i < m.count; ++i)
                same = std::strcmp(m.bones[i], rig_model.bones[i]) == 0;
            if (same && L::ListedWith(m, other))
                return true;
        }
        return false;
    };
    for (const s32 other : {2410000, 100, 2600000}) {
        if (binds_row(other))
            continue;
        Put<s32>(row, 0x80, other);
        channel.verdict.recheck = 1;
        expect_prompt(false, "row of another model");
        Put<s32>(row, 0x80, supported_action);
        channel.verdict.recheck = 1;
        expect_prompt(true, "own row");
        break;
    }
    // No hand can pull it now (the virtual screen, third person: the XR side clears its word and
    // bumps the prompt serial): the prompt and button are back on the very next pass.
    channel.hand_pull_ready.store(0);
    channel.PromptChanged();
    expect_prompt(false, "no hand can pull it now", false);
    EXPECT_NE(messages.str().find("normal prompt kept (no hand can pull it now"), std::string::npos);
    // The hand can pull again: the XR side bumps the serial and the prompt is hidden at once.
    channel.PromptChanged();
    expect_prompt(true, "a hand can pull again");
    // A word that stopped coming (the headset off, a stopped XR loop): at the next recheck.
    channel.hand_pull_ready.store(SteadyNs() - 2 * L::ReadyNs);
    expect_prompt(true, "a stale word before its recheck", false);
    channel.verdict.recheck = 1;
    expect_prompt(false, "a stale word", false);
    channel.verdict.recheck = 1;
    expect_prompt(true, "a fresh word again");
    // A lever the game's action tables do not hold cannot be armed: it keeps its prompt.
    Put<s32>(action_manager, 0x20, 0);
    channel.verdict.recheck = 1;
    expect_prompt(false, "not in the game's action tables");
    EXPECT_NE(messages.str().find("not found in the game's action tables"), std::string::npos);
    Put<s32>(action_manager, 0x20, 1);
    channel.verdict.recheck = 1;
    expect_prompt(true, "in the tables again");
    const auto request_ns = now();
    if (motion) {
        Put<s32>(row, 0x5C, 3); // original selection skips this used action entirely
        L::ProbeHands hands{};
        hands.ns = request_ns;
        // Grab the lower shaft, well outside the old head-only radius.
        hands.world[0] = pivot + B::Rotate(in_object(rig_model.rest_q), scaled(rig_model.shaft_start));
        hands.enabled[0] = true;
        hands.radius = L::GrabRadius;
        channel.RequestMotionOffer(hands);
        // No selected() call: only the scheduler can discover this used lever.
        update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    } else {
        channel.RequestOffer(request_ns);
        selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
    }
    ASSERT_TRUE(channel.LatestOffer(offered));
    EXPECT_EQ(offered.candidate.id, 0x100001u);
    EXPECT_EQ(offered.request_ns, request_ns);
    EXPECT_EQ(channel.acquire.load(), 0);
    EXPECT_EQ(permission_calls, motion ? 0u : 1u);
    for (int idle = 0; idle < 10; ++idle)
        selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
    EXPECT_EQ(permission_calls, motion ? 0u : 1u); // no repeated permission/pose resolution nearby
    const auto handle_at_rest = L::HandleAt(offered.candidate.rig, 0);
    const auto grip_at_rest = pivot + B::Rotate(in_object(rig_model.rest_q), scaled(rig_model.handle));
    EXPECT_NEAR(handle_at_rest.x, grip_at_rest.x, 0.0001f);
    EXPECT_NEAR(handle_at_rest.y, grip_at_rest.y, 0.0001f);
    EXPECT_NEAR(handle_at_rest.z, grip_at_rest.z, 0.0001f);
    // Rigid mesh vertex coordinates are bone-local: apply the pivot exactly once,
    // including during a partial pull. This independently models its render matrix.
    const auto drawn_grip = pivot + B::Rotate(in_object(pulled(0.6f)), scaled(rig_model.handle));
    EXPECT_LT(B::Length(L::HandleAt(offered.candidate.rig, 0.6f) - drawn_grip), 0.0001f);
    const auto send = [&](float angle, L::CommandKind kind, s64 ns) {
        return channel.Publish(
            {0, ns, offered.candidate.id, offered.candidate.generation, angle, 0, kind});
    };
    // Another map's scheduler must leave this command alone without inspecting
    // the lever's pose buffers. Restore the deliberately unavailable provider before
    // the owning scheduler runs; its full validation is still required there.
    ASSERT_TRUE(send(0.4f, L::CommandKind::Hold, now()));
    u64 saved_provider{};
    std::memcpy(&saved_provider, objects.data() + 0x2E0 + 0x2A0, sizeof(saved_provider));
    Put<u64>(objects, 0x2E0 + 0x2A0, 0);
    std::array<u8, 0x20> unrelated_queue{};
    const auto permissions_before_other_queue = permission_calls;
    update(unrelated_queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(channel.work.load(), 1);
    EXPECT_EQ(locals, rest);
    EXPECT_EQ(permission_calls, permissions_before_other_queue);
    Put<u64>(objects, 0x2E0 + 0x2A0, saved_provider);
    if (motion) {
        // Denied by the real permission stub, but motion rehearsal may still move it.
        ASSERT_TRUE(offered.candidate.eligible);
        module.data[Accept] = 0x0F; module.data[Accept + 1] = 0x0B;
        FlushInstructionCache(GetCurrentProcess(), module.data + Accept, 2);
        ASSERT_TRUE(send(0.4f, L::CommandKind::Hold, now()));
        update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
        expect_bone(0.4f, "rehearsal hold");
        ASSERT_TRUE(send(offered.candidate.rig.sweep, L::CommandKind::Complete, now()));
        update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
        update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
        EXPECT_EQ(locals, rest);
        EXPECT_EQ(channel.work.load(), 0);
        EXPECT_EQ(permission_calls, 0u);
        L::Completion completion{};
        ASSERT_TRUE(channel.LatestCompletion(completion));
        EXPECT_FALSE(completion.accepted);
        // Another used state stays unreachable from a distant squeeze.
        Put<s32>(row, 0x5C, 4);
        L::ProbeHands retry{};
        retry.ns = now();
        // 1 m off the handle: along a stick's hinge axis; above a capstan's bar (+X would lie
        // along its bar).
        retry.world[1] = handle_at_rest + (capstan ? B::Vec3{0, 1, 0} : B::Vec3{1, 0, 0});
        retry.enabled[1] = true;
        retry.radius = L::GrabRadius;
        channel.RequestMotionOffer(retry);
        update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
        ASSERT_TRUE(channel.LatestOffer(offered));
        EXPECT_FALSE(offered.candidate.eligible);
        EXPECT_EQ(offered.request_ns, retry.ns);
        EXPECT_EQ(channel.acquire.load(), 0);
        EXPECT_EQ(locals, rest);
        // A fresh right-hand squeeze at the handle finds it without selected().
        retry.ns = now();
        retry.world[1] = handle_at_rest;
        channel.RequestMotionOffer(retry);
        update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
        ASSERT_TRUE(channel.LatestOffer(offered));
        EXPECT_TRUE(offered.candidate.eligible);
        EXPECT_EQ(offered.candidate.id, 0x100001u);
        EXPECT_EQ(permission_calls, 0u);
        EXPECT_EQ(locals, rest); // detection itself never moves the bone
        // A capstan's release commits within its last 3% (CommitFor): rehearsed there too.
        std::vector<float> progresses{0.59f, 0.61f};
        if (capstan)
            progresses.push_back(0.98f);
        for (float progress : progresses) {
            const float angle = offered.candidate.rig.sweep * progress;
            ASSERT_TRUE(send(angle, L::CommandKind::Hold, now()));
            update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
            const auto released_pose = locals[lever];
            const float released_angle = pull_angle();
            ASSERT_TRUE(send(angle, L::CommandKind::Settle, now()));
            update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
            EXPECT_EQ(locals[lever], released_pose); // no snap on the release frame
            bool moved_forward = false;
            for (int tick = 0; tick < 24 && channel.work.load(); ++tick) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
                moved_forward |= pull_angle() > released_angle + .002f;
            }
            EXPECT_EQ(moved_forward, progress >= L::CommitFor(rig_model.motion)) << progress;
            EXPECT_EQ(channel.work.load(), 0); // no ongoing callback when settled
            EXPECT_EQ(locals, rest); // rehearsal never activates; returns smoothly
            EXPECT_EQ(permission_calls, 0u);
            retry.ns = now();
            channel.RequestMotionOffer(retry);
            update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
            ASSERT_TRUE(channel.LatestOffer(offered));
            ASSERT_TRUE(offered.candidate.eligible);
        }
        return;
    }
    if (capstan) {
        // A capstan (o353010) is walked round by hand. Let go short of its stop, it stays where the
        // hand left it (parked) and a new squeeze carries on from there; refused, it stays too;
        // completed, it stays turned (its own clip ends there). Nothing ever turns it back.
        const float stop = offered.candidate.rig.sweep;
        EXPECT_EQ(offered.candidate.rig.motion, L::Motion::Capstan);
        EXPECT_FLOAT_EQ(offered.candidate.angle, 0.0f);
        u64 accepts{};
        std::array<s32, 8> dispatched{17, 0, -1, 0, 101150, 191, 1, 0};
        const auto original_dispatch = dispatched;
        u8 accept_result = 1;
        Xbyak::CodeGenerator accept_body(160, module.data + Accept);
        accept_body.mov(rax, reinterpret_cast<u64>(&accepts));
        accept_body.inc(qword[rax]);
        accept_body.mov(rax, reinterpret_cast<u64>(dispatched.data()));
        accept_body.mov(dword[rax + 4], esi);
        accept_body.mov(dword[rax + 12], edx);
        accept_body.mov(rsi, rax);
        accept_body.xor_(edx, edx);
        accept_body.mov(rax, reinterpret_cast<u64>(module.data + Event));
        accept_body.sub(rsp, 8);
        accept_body.call(rax);
        accept_body.add(rsp, 8);
        accept_body.mov(rax, reinterpret_cast<u64>(&accept_result));
        accept_body.movzx(eax, byte[rax]);
        accept_body.ret();
        Put<u32>(actor, 8, 0x1234);
        const auto tick = [&] { update(queue.data(), objects.data(), 3, 5, 7, 11, 13); };
        const auto settle_out = [&] {
            for (int i = 0; i < 16 && channel.work.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                tick();
            }
        };
        const auto idle_ticks = [&] {
            for (int i = 0; i < 4; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                tick();
            }
        };
        const auto regrab = [&](const char *what) {
            channel.RequestOffer(now());
            selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
            EXPECT_TRUE(channel.LatestOffer(offered)) << what;
            EXPECT_TRUE(offered.candidate.eligible) << what;
        };
        const auto parked_line = [&](float angle) {
            return fmt::format("({}) parked at {:.1f} of {:.1f} degrees", rig_model.name,
                               angle / B::Deg, stop / B::Deg);
        };
        L::Nearby nearby{};
        L::Completion completion{};

        // A hold turns the bar, and nothing else.
        ASSERT_TRUE(send(0.6f, L::CommandKind::Hold, now()));
        tick();
        expect_bone(0.6f, "capstan hold");
        for (size_t i = 0; i < locals.size(); ++i)
            if (i != lever)
                EXPECT_EQ(locals[i], rest[i]) << i;
        // Let go short of its stop: parked where the hand left it.
        ASSERT_TRUE(send(0.6f, L::CommandKind::Settle, now()));
        tick();
        EXPECT_EQ(channel.work.load(), 0);
        EXPECT_EQ(channel.held_object.load(), 0u);
        expect_bone(0.6f, "parked");
        EXPECT_NE(messages.str().find(parked_line(0.6f)), std::string::npos) << messages.str();
        idle_ticks();
        expect_bone(0.6f, "still parked");
        EXPECT_EQ(accepts, 0u);
        // Still the hand's: its prompt stays replaced, and the armed lever's word says where its bar
        // stands; the game's guarded re-check says the same.
        channel.verdict.recheck = 1;
        expect_prompt(true, "parked: still bound");
        ASSERT_TRUE(channel.LatestNearby(nearby));
        EXPECT_TRUE(nearby.armed);
        EXPECT_FLOAT_EQ(nearby.angle, 0.6f);
        channel.RequestNearbyCheck(now());
        tick();
        EXPECT_EQ(channel.nearby_check.load(), 0);
        ASSERT_TRUE(channel.LatestNearby(nearby));
        EXPECT_TRUE(nearby.armed);
        EXPECT_FLOAT_EQ(nearby.angle, 0.6f);
        // A new squeeze carries on from there: offered at its parked angle (its rig at rest), the
        // hold resumes the very bone.
        regrab("parked");
        EXPECT_FLOAT_EQ(offered.candidate.angle, 0.6f);
        EXPECT_LT(B::Length(L::HandleAt(offered.candidate.rig, 0) - grip_at_rest), 1e-4f);
        EXPECT_NE(messages.str().find("parked at 34.4 degrees"), std::string::npos);
        ASSERT_TRUE(send(0.9f, L::CommandKind::Hold, now()));
        tick();
        expect_bone(0.9f, "resumed");
        EXPECT_NE(messages.str().find("(resumed at 34.4 degrees)"), std::string::npos);
        // Its hold cancelled (B, a recenter, the hand lost: Release): parked again, not restored.
        ASSERT_TRUE(send(0.9f, L::CommandKind::Release, now()));
        tick();
        EXPECT_EQ(channel.work.load(), 0);
        expect_bone(0.9f, "parked again");
        // The game touching the bar (one byte) ends the record: not at rest either, so its prompt is
        // back; as left, it binds again.
        const auto parked_bone = locals[lever];
        locals[lever][4] = std::nextafter(locals[lever][4], 2.0f);
        channel.verdict.recheck = 1;
        expect_prompt(false, "a parked bar the game moved");
        locals[lever] = parked_bone;
        channel.verdict.recheck = 1;
        expect_prompt(true, "the parked bar as left");
        // The proximity scan finds it where it stands too (the head beside its bar).
        {
            L::ScanRequest where{};
            where.ns = now();
            where.scale = 1.0f;
            where.head = L::HandleAt(offered.candidate.rig, 0.9f) + B::Vec3{0, 0.5f, 0};
            where.hands = {where.head, where.head};
            where.tracked = {true, true};
            channel.hand_pull_ready.store(SteadyNs());
            channel.PublishNearby({});
            channel.RequestScan(where);
            tick();
            EXPECT_EQ(channel.scan.load(), 0);
            ASSERT_TRUE(channel.LatestNearby(nearby));
            EXPECT_TRUE(nearby.armed);
            EXPECT_FLOAT_EQ(nearby.angle, 0.9f);
        }

        // Turned to its stop (the XR side completes it there and sends its release, Settle, at the
        // stop: it never sends Complete), the game refusing its completion: the settle's one
        // dispatch is refused, and it stays parked at the stop, no settle back.
        regrab("parked at 0.9");
        EXPECT_FLOAT_EQ(offered.candidate.angle, 0.9f);
        ASSERT_TRUE(send(stop, L::CommandKind::Hold, now()));
        tick();
        expect_bone(stop, "held at its stop");
        accept_result = 0;
        dispatched = original_dispatch;
        ASSERT_TRUE(send(stop, L::CommandKind::Settle, now()));
        tick();
        EXPECT_EQ(accepts, 0u); // not before its settle has run
        settle_out();
        EXPECT_EQ(accepts, 1u);
        ASSERT_TRUE(channel.LatestCompletion(completion));
        EXPECT_FALSE(completion.accepted);
        EXPECT_EQ(channel.work.load(), 0);
        expect_bone(stop, "refused: parked at its stop");
        EXPECT_NE(messages.str().find(parked_line(stop) + " (the game refused its completion)"),
                  std::string::npos)
            << messages.str();
        idle_ticks();
        expect_bone(stop, "refused: still at its stop");
        EXPECT_EQ(accepts, 1u);
        // A squeeze there completes on the hand's very first step, so its release can reach the
        // game before any hold: it takes the bar then and completes, once.
        accept_result = 1;
        dispatched = original_dispatch;
        regrab("parked at its stop");
        EXPECT_FLOAT_EQ(offered.candidate.angle, stop);
        ASSERT_TRUE(send(stop, L::CommandKind::Settle, now()));
        settle_out();
        EXPECT_EQ(channel.work.load(), 0);
        EXPECT_EQ(accepts, 2u);
        ASSERT_TRUE(channel.LatestCompletion(completion));
        EXPECT_TRUE(completion.accepted);
        expect_bone(stop, "completed: stays turned");
        EXPECT_NE(messages.str().find("stays pulled, as its own clip leaves it"), std::string::npos);
        // Its event: no hunter animation, no object clip (the hand turned it), no alignment.
        EXPECT_EQ(dispatched[3], 0x1234);
        EXPECT_EQ(dispatched[4], -1);
        EXPECT_EQ(dispatched[5], 191);
        EXPECT_EQ(dispatched[6], -1);
        EXPECT_EQ(dispatched[7], 2);
        idle_ticks();
        expect_bone(stop, "stays turned");
        ASSERT_TRUE(send(stop, L::CommandKind::Complete, now())); // a late duplicate
        tick();
        EXPECT_EQ(accepts, 2u);

        // (Its map script now lifts the column and ends the action.) An unused capstan again: let go
        // within its last 3%, the settle carries the last degrees and it completes.
        locals = rest;
        dispatched = original_dispatch;
        regrab("at rest");
        EXPECT_FLOAT_EQ(offered.candidate.angle, 0.0f);
        ASSERT_TRUE(send(0.98f * stop, L::CommandKind::Hold, now()));
        tick();
        ASSERT_TRUE(send(0.98f * stop, L::CommandKind::Settle, now()));
        tick();
        EXPECT_EQ(accepts, 2u); // not before the stop
        settle_out();
        EXPECT_EQ(accepts, 3u);
        EXPECT_EQ(channel.work.load(), 0);
        expect_bone(stop, "committed within its last 3%");
        EXPECT_EQ(dispatched[6], -1);

        // Parked where the hand let go, from rest: a hold, then the release.
        const auto park_at = [&](float angle, const char *what) {
            locals = rest;
            channel.verdict.recheck = 1; // classified again first (a hand can pull it now)
            regrab(what);
            EXPECT_FLOAT_EQ(offered.candidate.angle, 0.0f) << what;
            ASSERT_TRUE(send(angle, L::CommandKind::Hold, now()));
            tick();
            ASSERT_TRUE(send(angle, L::CommandKind::Settle, now()));
            tick();
            EXPECT_EQ(channel.work.load(), 0) << what;
            expect_bone(angle, what);
        };
        // A fast push: the last hold the game applied is short of the last 3% (93%), and the
        // release, at the stop where the XR side completed it, replaced the holds after it. It
        // commits from the release's angle: carried to its stop, dispatched once.
        locals = rest;
        dispatched = original_dispatch;
        regrab("at rest, a fast push");
        ASSERT_TRUE(send(0.93f * stop, L::CommandKind::Hold, now()));
        tick();
        expect_bone(0.93f * stop, "a fast push, its last applied hold");
        ASSERT_TRUE(send(stop, L::CommandKind::Settle, now()));
        tick();
        EXPECT_EQ(accepts, 3u); // not before the stop
        settle_out();
        EXPECT_EQ(accepts, 4u);
        EXPECT_EQ(channel.work.load(), 0);
        expect_bone(stop, "a fast push: committed from its release");
        // The same from a parked bar whose first command to reach the game is its release.
        dispatched = original_dispatch;
        park_at(0.5f, "parked before a fast push");
        regrab("parked, a fast push");
        EXPECT_FLOAT_EQ(offered.candidate.angle, 0.5f);
        ASSERT_TRUE(send(stop, L::CommandKind::Settle, now()));
        settle_out();
        EXPECT_EQ(accepts, 5u);
        expect_bone(stop, "a parked bar's fast push: committed from its release");
        // Short of the last 3% both ways (the hold at 90%, the release at 93%): parked where the
        // game shows it, at the hold.
        locals = rest;
        regrab("at rest, a short push");
        ASSERT_TRUE(send(0.9f * stop, L::CommandKind::Hold, now()));
        tick();
        ASSERT_TRUE(send(0.93f * stop, L::CommandKind::Settle, now()));
        tick();
        EXPECT_EQ(channel.work.load(), 0);
        expect_bone(0.9f * stop, "short both ways: parked at the hold");
        EXPECT_EQ(accepts, 5u);

        // Let go just off rest (1 degree: past the half degree that counts as rest, within the
        // rest match's 1.6): still parked, offered at 1 degree with its true rest, never a rest of
        // its own (a hold could then pass the game's stop by that degree).
        const float nudge = 1.0f * B::Deg;
        park_at(nudge, "parked just off rest");
        EXPECT_NE(messages.str().find(parked_line(nudge)), std::string::npos);
        channel.verdict.recheck = 1;
        expect_prompt(true, "parked just off rest: still bound");
        regrab("parked just off rest");
        EXPECT_FLOAT_EQ(offered.candidate.angle, nudge);
        EXPECT_NEAR(offered.candidate.rig.sweep, stop, 1e-6f);
        EXPECT_LT(B::Length(L::HandleAt(offered.candidate.rig, 0) - grip_at_rest), 1e-4f);
        ASSERT_TRUE(send(stop, L::CommandKind::Hold, now()));
        tick();
        expect_bone(stop, "from just off rest to the stop, no further");
        ASSERT_TRUE(send(stop, L::CommandKind::Release, now()));
        tick();
        expect_bone(stop, "cancelled: parked at the stop");

        // B at a parked capstan: its prompt and button come back, and its own scheduler puts the
        // bar back at rest (the game holds it unturned; its own action plays its clip from there).
        // No hand pull meanwhile.
        dispatched = original_dispatch;
        park_at(0.5f, "parked before B");
        channel.verdict.recheck = 1;
        expect_prompt(true, "parked before B");
        EXPECT_EQ(channel.unpark.load(), 0);
        channel.prompt_fallback.fetch_add(1);
        expect_prompt(false, "B at a parked capstan");
        EXPECT_NE(channel.unpark.load(), 0);
        expect_bone(0.5f, "the selection itself writes nothing");
        const auto syncs_before_unpark = sync_calls;
        tick();
        EXPECT_EQ(channel.unpark.load(), 0);
        EXPECT_EQ(locals, rest);
        EXPECT_EQ(sync_calls, syncs_before_unpark + 1);
        EXPECT_NE(messages.str().find("(o353010) parked bar back at rest from 28.6 degrees"),
                  std::string::npos)
            << messages.str();
        ASSERT_TRUE(channel.LatestNearby(nearby));
        EXPECT_FLOAT_EQ(nearby.angle, 0.0f);
        channel.RequestNearbyCheck(now());
        tick();
        ASSERT_TRUE(channel.LatestNearby(nearby));
        EXPECT_FALSE(nearby.armed);
        const auto permissions_in_fallback = permission_calls;
        channel.RequestOffer(now());
        tick();
        ASSERT_TRUE(channel.LatestOffer(offered));
        EXPECT_FALSE(offered.candidate.eligible);
        EXPECT_EQ(permission_calls, permissions_in_fallback);
        EXPECT_EQ(locals, rest);
        tick();
        EXPECT_EQ(locals, rest);
        EXPECT_EQ(accepts, 5u);
        channel.prompt_state = {};

        // A word merely late (a tracking gap, the headset off): it stays parked for the hand to
        // carry on. The virtual screen or third person (the XR side clears its word, and asks):
        // back at rest; so with physical levers switched off (the XR side asks).
        park_at(0.7f, "parked before a late word");
        channel.hand_pull_ready.store(SteadyNs() - 2 * L::ReadyNs);
        channel.verdict.recheck = 1;
        expect_prompt(false, "a late word", false);
        EXPECT_EQ(channel.unpark.load(), 0);
        idle_ticks();
        expect_bone(0.7f, "a late word: still parked");
        channel.hand_pull_ready.store(0);
        channel.PromptChanged();
        expect_prompt(false, "the virtual screen", false);
        EXPECT_NE(channel.unpark.load(), 0);
        tick();
        EXPECT_EQ(locals, rest);
        EXPECT_EQ(channel.unpark.load(), 0);
        park_at(0.7f, "parked before physical levers off");
        L::SetEnabled(false);
        channel.RequestUnpark(now()); // the XR side, as the setting goes off
        tick();
        EXPECT_EQ(locals, rest);
        L::SetEnabled(true);
        // Switched off during a hold: the request waits while the hold runs; the hold's end (the
        // XR side's Release) parks the bar, and the request then puts it back at rest.
        locals = rest;
        channel.verdict.recheck = 1;
        regrab("held as physical levers go off");
        ASSERT_TRUE(send(0.6f, L::CommandKind::Hold, now()));
        tick();
        L::SetEnabled(false);
        channel.RequestUnpark(now());
        tick();
        expect_bone(0.6f, "held while the request waits");
        EXPECT_NE(channel.unpark.load(), 0);
        ASSERT_TRUE(send(0.6f, L::CommandKind::Release, now()));
        tick();
        EXPECT_EQ(channel.work.load(), 0);
        EXPECT_EQ(locals, rest);
        EXPECT_EQ(channel.unpark.load(), 0);
        L::SetEnabled(true);
        // A request with nothing parked: the host runs once and writes nothing.
        channel.RequestUnpark(now());
        const auto syncs_idle = sync_calls;
        tick();
        EXPECT_EQ(channel.unpark.load(), 0);
        EXPECT_EQ(sync_calls, syncs_idle);
        // A bar the game has moved since (one ulp): the record is dropped, nothing written.
        park_at(0.7f, "parked before the game moves it");
        locals[lever][4] = std::nextafter(locals[lever][4], 2.0f);
        const auto moved = locals;
        channel.RequestUnpark(now());
        tick();
        EXPECT_EQ(channel.unpark.load(), 0);
        EXPECT_EQ(locals, moved);
        EXPECT_NE(messages.str().find("parked bar not restored: the game moved it since"),
                  std::string::npos);
        // Another map's scheduler leaves it to its own, and lets a request lapse after 2 s with
        // the record kept: the hand can still carry on, and the next request restores it.
        park_at(0.7f, "parked, its map not running");
        std::array<u8, 0x20> other_queue{};
        channel.RequestUnpark(now() - 3'000'000'000);
        update(other_queue.data(), objects.data(), 3, 5, 7, 11, 13);
        EXPECT_EQ(channel.unpark.load(), 0);
        expect_bone(0.7f, "its map not running: still parked");
        EXPECT_NE(messages.str().find("parked bar left parked for now"), std::string::npos);
        regrab("still parked after a lapsed request");
        EXPECT_FLOAT_EQ(offered.candidate.angle, 0.7f);
        ASSERT_TRUE(send(0.7f, L::CommandKind::Hold, now()));
        tick();
        ASSERT_TRUE(send(0.7f, L::CommandKind::Release, now()));
        tick();
        expect_bone(0.7f, "taken and let go: parked again");
        channel.RequestUnpark(now());
        tick();
        EXPECT_EQ(locals, rest);

        // Its column lifted by the map script's clip: it binds neither at rest nor parked.
        locals = rest;
        locals[1][2] = 10.2f;
        channel.verdict.recheck = 1;
        expect_prompt(false, "its column lifted");
        locals = rest;
        channel.verdict.recheck = 1;
        expect_prompt(true, "unused again");
        EXPECT_EQ(accepts, 5u);
        Common::Log::ALL_LOGGERS.clear();
        return;
    }
    ASSERT_TRUE(send(0.6f, L::CommandKind::Hold, now()));
    EXPECT_EQ(update(queue.data(), objects.data(), 3, 5, 7, 11, 13), 39u);
    expect_bone(0.6f, "hold");
    for (size_t i = 0; i < locals.size(); ++i) // every other bone stays untouched
        if (i != lever)
            EXPECT_EQ(locals[i], rest[i]) << i;
    EXPECT_EQ(sync_calls, 1u);
    EXPECT_EQ(dirty_calls, 1u);
    EXPECT_EQ(dirty_node, reinterpret_cast<u64>(matrix_node.data()));
    EXPECT_EQ(indices[0], 1);
    L::Applied applied{};
    ASSERT_TRUE(channel.LatestApplied(applied));
    EXPECT_FLOAT_EQ(applied.angle, 0.6f);
    ASSERT_TRUE(send(0.2f, L::CommandKind::Hold, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    expect_bone(0.2f, "reversal follows the hand");
    ASSERT_TRUE(send(0.2f, L::CommandKind::Release, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(locals, rest);
    EXPECT_EQ(channel.work.load(), 0);
    EXPECT_EQ(sync_calls, 3u);
    EXPECT_EQ(dirty_calls, 3u);
    const auto calls_after_release = sync_calls;
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(sync_calls, calls_after_release); // inactive native gate

    // A matrix provider already queued by the renderer must not be queued twice.
    channel.RequestOffer(now());
    selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
    ASSERT_TRUE(channel.LatestOffer(offered));
    Put<u32>(matrix_node, 0x10, 1);
    const auto dirty_before_pending = dirty_calls;
    ASSERT_TRUE(send(0.35f, L::CommandKind::Hold, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(dirty_calls, dirty_before_pending);
    expect_bone(0.35f, "already queued provider");
    Put<u32>(matrix_node, 0x10, 0); // renderer consumed the previous pending job
    ASSERT_TRUE(send(0.35f, L::CommandKind::Release, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(dirty_calls, dirty_before_pending + 1);
    EXPECT_EQ(locals, rest);

    // Permission is checked again at acquisition; an old permitted offer cannot write.
    channel.RequestOffer(now());
    selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
    ASSERT_TRUE(channel.LatestOffer(offered));
    permission_value = 0;
    ASSERT_TRUE(send(0.4f, L::CommandKind::Hold, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(locals, rest);
    EXPECT_EQ(channel.work.load(), 0);
    permission_value = 1;
    channel.RequestOffer(now());
    selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
    ASSERT_TRUE(channel.LatestOffer(offered));
    ASSERT_TRUE(send(0.4f, L::CommandKind::Hold, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    // The input producer can disappear without publishing release (tracking/runtime loss).
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(locals, rest);
    EXPECT_EQ(channel.work.load(), 0);
    channel.RequestOffer(now());
    selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
    ASSERT_TRUE(channel.LatestOffer(offered));
    ASSERT_TRUE(send(0.4f, L::CommandKind::Hold, now() - 200'000'000));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(locals, rest);
    EXPECT_EQ(channel.work.load(), 0);

    // Revoked permission while held restores once; it never activates anything.
    channel.RequestOffer(now());
    selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
    ASSERT_TRUE(channel.LatestOffer(offered));
    ASSERT_TRUE(send(0.4f, L::CommandKind::Hold, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    permission_value = 0;
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(locals, rest);
    EXPECT_EQ(channel.work.load(), 0);
    permission_value = 1;

    // An external game pose must survive cancellation instead of being overwritten by rest.
    channel.RequestOffer(now());
    selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
    ASSERT_TRUE(channel.LatestOffer(offered));
    ASSERT_TRUE(send(0.4f, L::CommandKind::Hold, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    locals[lever][0] += 0.01f;
    const auto external = locals;
    ASSERT_TRUE(send(0.4f, L::CommandKind::Release, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(locals, external);
    EXPECT_EQ(channel.work.load(), 0);
    // A complete pull calls the original acceptance entry once. Its matching event
    // keeps gameplay fields, omits the already-performed pull, and does not align the actor.
    locals = rest;
    u64 accepts{};
    std::array<s32, 8> dispatched{17, 0, -1, 0, 101150, 191, 1, 0};
    const auto original_dispatch = dispatched;
    u8 accept_result = 1;
    Xbyak::CodeGenerator accept_body(160, module.data + Accept);
    accept_body.mov(rax, reinterpret_cast<u64>(&accepts));
    accept_body.inc(qword[rax]);
    accept_body.mov(rax, reinterpret_cast<u64>(dispatched.data()));
    accept_body.mov(dword[rax + 4], esi);
    accept_body.mov(dword[rax + 12], edx);
    accept_body.mov(rsi, rax);
    accept_body.xor_(edx, edx);
    accept_body.mov(rax, reinterpret_cast<u64>(module.data + Event));
    accept_body.sub(rsp, 8);
    accept_body.call(rax);
    accept_body.add(rsp, 8);
    accept_body.mov(rax, reinterpret_cast<u64>(&accept_result));
    accept_body.movzx(eax, byte[rax]);
    accept_body.ret();
    Put<u32>(actor, 8, 0x1234);
    // The game refuses its interact button only on some of its gate's bits (13ADDA0); this one
    // does not stop the dispatch.
    blocked_value = 0x08;
    channel.RequestOffer(now());
    selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
    ASSERT_TRUE(channel.LatestOffer(offered));
    const float stop = offered.candidate.rig.sweep;
    ASSERT_TRUE(send(stop * .61f, L::CommandKind::Hold, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(send(stop * .61f, L::CommandKind::Settle, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(accepts, 0u); // releasing beyond the threshold does not dispatch early
    for (int tick = 0; tick < 12 && accepts == 0; ++tick) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    }
    EXPECT_EQ(accepts, 1u);
    expect_bone(stop, "dispatched at the stop");
    if (L::StaysAtStop(rig_model)) {
        // Its own clip stays at the stop (o253010): so does the lever. The hold ends at once,
        // nothing returns it, and no later tick moves it.
        EXPECT_EQ(channel.work.load(), 0);
        EXPECT_NE(messages.str().find("stays pulled, as its own clip leaves it"), std::string::npos);
        for (int tick = 0; tick < 4; ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
        }
        expect_bone(stop, "stays pulled");
        EXPECT_EQ(accepts, 1u);
        locals = rest; // the scenarios below start from an unused lever again
    } else {
        // Back toward rest, as the model's own clip goes: all the way (a reusable lever), or to
        // where its clip leaves it (o321110: 61.4 degrees, then it stays there).
        const float end = L::EndAngle(rig_model);
        EXPECT_EQ(channel.work.load(), 1);
        float previous = pull_angle();
        for (int tick = 0; tick < 12 && channel.work.load(); ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
            EXPECT_LE(pull_angle(), previous + 0.00001f);
            EXPECT_GE(pull_angle(), end - 0.0001f);
            previous = pull_angle();
            EXPECT_EQ(accepts, 1u);
        }
        EXPECT_EQ(channel.work.load(), 0);
        EXPECT_EQ(messages.str().find("stays pulled"), std::string::npos);
        if (end > 0) {
            expect_bone(end, "settled where its own clip leaves it");
            EXPECT_NE(messages.str().find(fmt::format("({}) stays at {:.1f} degrees", rig_model.name,
                                                      rig_model.end_deg)),
                      std::string::npos);
            for (int tick = 0; tick < 4; ++tick) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
            }
            expect_bone(end, "stays where its own clip leaves it");
            EXPECT_EQ(accepts, 1u);
            locals = rest; // the scenarios below start from an unused lever again
        } else {
            EXPECT_EQ(messages.str().find("degrees, as its own clip leaves it"), std::string::npos);
        }
    }
    EXPECT_EQ(locals, rest);
    EXPECT_EQ(dispatched[0], 17);
    EXPECT_EQ(dispatched[1], 0);
    EXPECT_EQ(dispatched[2], -1);
    EXPECT_EQ(dispatched[3], 0x1234);
    EXPECT_EQ(dispatched[4], -1);
    EXPECT_EQ(dispatched[5], 191);
    EXPECT_EQ(dispatched[6], -1);
    EXPECT_EQ(dispatched[7], 2);
    EXPECT_EQ(channel.work.load(), 0);
    L::Completion completion{};
    ASSERT_TRUE(channel.LatestCompletion(completion));
    EXPECT_TRUE(completion.accepted);
    EXPECT_EQ(completion.id, offered.candidate.id);
    blocked_value = 0;
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(accepts, 1u);
    // A late duplicate command cannot activate again after its offer was retired.
    ASSERT_TRUE(send(stop, L::CommandKind::Complete, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(accepts, 1u);

    // Normal B receipt and remote receipt retain every byte even for the same lever.
    dispatched = original_dispatch;
    receive(block.data(), dispatched.data(), 0);
    EXPECT_EQ(dispatched, original_dispatch);
    receive(block.data(), dispatched.data(), 1);
    EXPECT_EQ(dispatched, original_dispatch);

    // Reacquire the returned handle without an external pose reset. A failed native
    // acceptance is never retried; the following tick restores our pose.
    channel.RequestOffer(now());
    selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
    ASSERT_TRUE(channel.LatestOffer(offered));
    ASSERT_TRUE(send(0.4f, L::CommandKind::Hold, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    accept_result = 0;
    dispatched = original_dispatch;
    ASSERT_TRUE(send(stop, L::CommandKind::Complete, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(accepts, 2u);
    ASSERT_TRUE(channel.LatestCompletion(completion));
    EXPECT_FALSE(completion.accepted);
    EXPECT_EQ(channel.work.load(), 1);
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(accepts, 2u);
    EXPECT_EQ(locals, rest);
    EXPECT_EQ(channel.work.load(), 0);

    // Neither a partial pull marked "complete", nor changed actor/action identity, dispatches.
    accept_result = 1;
    for (int change = 0; change < 3; ++change) {
        channel.RequestOffer(now());
    selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
        ASSERT_TRUE(channel.LatestOffer(offered));
        ASSERT_TRUE(send(0.4f, L::CommandKind::Hold, now()));
        update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
        if (change == 1)
            Put<u32>(actor, 8, 0x1235);
        if (change == 2)
            Put<s32>(row, 0x5C, 99);
        ASSERT_TRUE(send(change == 0 ? 0.4f : stop, L::CommandKind::Complete, now()));
        update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
        EXPECT_EQ(accepts, 2u);
        EXPECT_EQ(locals, rest);
        EXPECT_EQ(channel.work.load(), 0);
        Put<u32>(actor, 8, 0x1234);
        Put<s32>(row, 0x5C, 0);
    }
    // A dialog or event opened before the stop (the game refuses its interact button on this
    // bit): no dispatch, and the lever returns to rest without an event.
    channel.RequestOffer(now());
    selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
    ASSERT_TRUE(channel.LatestOffer(offered));
    ASSERT_TRUE(send(0.4f, L::CommandKind::Hold, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    blocked_value = 0x200;
    ASSERT_TRUE(send(stop, L::CommandKind::Complete, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(accepts, 2u);
    ASSERT_TRUE(channel.LatestCompletion(completion));
    EXPECT_FALSE(completion.accepted);
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(accepts, 2u);
    EXPECT_EQ(locals, rest);
    EXPECT_EQ(channel.work.load(), 0);
    EXPECT_NE(messages.str().find("not dispatched: the game blocks interaction now"),
              std::string::npos);
    blocked_value = 0;

    // The armed lever (selected once, its rig bound): published with its rig for the XR side.
    channel.verdict.recheck = 1;
    expect_prompt(true, "bound and armed");
    L::Nearby armed{};
    ASSERT_TRUE(channel.LatestNearby(armed));
    EXPECT_TRUE(armed.armed);
    EXPECT_EQ(armed.id, 0x100001u);
    EXPECT_LT(B::Length(armed.rig.pivot - offered.candidate.rig.pivot), 0.0001f);
    EXPECT_NE(messages.str().find("armed: grabbable while you stay within"), std::string::npos);
    // A squeeze for it with no selection this frame (the hunter beside or behind the housing):
    // the scheduler serves it with the full checks, including the game's permission.
    const auto permissions_before_armed = permission_calls;
    const auto armed_request = now();
    channel.RequestOffer(armed_request);
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(channel.LatestOffer(offered));
    EXPECT_EQ(offered.request_ns, armed_request);
    EXPECT_TRUE(offered.candidate.eligible);
    EXPECT_EQ(offered.candidate.id, 0x100001u);
    EXPECT_EQ(channel.acquire.load(), 0);
    EXPECT_EQ(permission_calls, permissions_before_armed + 1);
    EXPECT_NE(messages.str().find("(armed) handle=0x100001"), std::string::npos);
    // The game blocks interaction (a menu, a dialog, talk, an event: its selector selects nothing
    // then): the squeeze is answered at once, without the game's permission, and it is no
    // refusal of the lever (its prompt stays replaced).
    blocked_value = 0x200;
    const auto permissions_before_blocked = permission_calls;
    const auto blocked_request = now();
    channel.RequestOffer(blocked_request);
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(channel.LatestOffer(offered));
    EXPECT_EQ(offered.request_ns, blocked_request);
    EXPECT_FALSE(offered.candidate.eligible);
    EXPECT_EQ(channel.acquire.load(), 0);
    EXPECT_EQ(permission_calls, permissions_before_blocked);
    EXPECT_NE(messages.str().find("(armed) not served: the game blocks interaction now"),
              std::string::npos);
    expect_prompt(true, "blocked: not a refusal");
    // For selection every bit counts (13AE034), even one the button ignores. And before the
    // game has ever asked its gate (its static state not set up), nothing is served.
    blocked_value = 0x08;
    channel.RequestOffer(now());
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(channel.LatestOffer(offered));
    EXPECT_FALSE(offered.candidate.eligible);
    blocked_value = 0;
    module.data[BlockedInit] = 0;
    channel.RequestOffer(now());
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(channel.LatestOffer(offered));
    EXPECT_FALSE(offered.candidate.eligible);
    EXPECT_EQ(permission_calls, permissions_before_blocked);
    module.data[BlockedInit] = 1;
    // Twice a second while near, the game re-checks it: used -> not grabbable, then again.
    Put<s32>(row, 0x5C, 3);
    channel.RequestNearbyCheck(now());
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(channel.LatestNearby(armed));
    EXPECT_FALSE(armed.armed);
    EXPECT_EQ(channel.nearby_check.load(), 0);
    EXPECT_NE(messages.str().find("not grabbable now (used or running)"), std::string::npos);
    // ... and a squeeze then is refused without the game's permission being asked.
    const auto permissions_before_used = permission_calls;
    channel.RequestOffer(now());
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(channel.LatestOffer(offered));
    EXPECT_FALSE(offered.candidate.eligible);
    EXPECT_EQ(permission_calls, permissions_before_used);
    Put<s32>(row, 0x5C, 0);
    // (The game offers no prompt for a used or running lever: that refusal does not demote it.)
    channel.RequestNearbyCheck(now());
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(channel.LatestNearby(armed));
    EXPECT_TRUE(armed.armed);
    EXPECT_NE(messages.str().find("armed again"), std::string::npos);
    expect_prompt(true, "used: no demotion");
    // Another action block streams in (the game's block count changes): this lever's own block,
    // rows and object are unchanged, so it stays armed.
    Put<s32>(action_manager, 0x20, 2);
    channel.RequestNearbyCheck(now());
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(channel.LatestNearby(armed));
    EXPECT_TRUE(armed.armed);
    EXPECT_EQ(messages.str().find("disarmed"), std::string::npos);
    Put<s32>(action_manager, 0x20, 1);

    // A pull the game refuses (here its permission), with no B before it: the prompt and button
    // are back on the very next selection pass, and the verdict keeps being re-checked (never a
    // frozen, wrapped counter).
    channel.verdict.recheck = 1;
    expect_prompt(true, "bound before the refusal");
    permission_value = 0;
    const auto refused_request = now();
    channel.RequestOffer(refused_request);
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(channel.LatestOffer(offered));
    EXPECT_EQ(offered.request_ns, refused_request);
    EXPECT_FALSE(offered.candidate.eligible);
    EXPECT_NE(messages.str().find("(armed) rejected: game permission denied"), std::string::npos);
    permission_value = 1;
    expect_prompt(false, "refused pull: prompt back");
    EXPECT_EQ(channel.verdict.bound, 0);
    EXPECT_LE(channel.verdict.recheck, L::UnboundRecheck);
    for (int pass = 0; pass < 2 * L::UnboundRecheck; ++pass)
        selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
    expect_prompt(false, "refused pull: prompt stays back");
    EXPECT_LE(channel.verdict.recheck, L::UnboundRecheck);
    // While it is back, no hand pull: the game's re-check says not grabbable, and a squeeze is
    // answered without the game's permission (and does not extend the window).
    channel.RequestNearbyCheck(now());
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(channel.LatestNearby(armed));
    EXPECT_FALSE(armed.armed);
    EXPECT_NE(messages.str().find("not grabbable now (its normal prompt and button are back"),
              std::string::npos);
    const auto permissions_while_back = permission_calls;
    channel.RequestOffer(now());
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(channel.LatestOffer(offered));
    EXPECT_FALSE(offered.candidate.eligible);
    EXPECT_EQ(permission_calls, permissions_while_back);
    EXPECT_NE(messages.str().find("rejected: its normal prompt and button are back for now"),
              std::string::npos);
    // Its window ends (RejectedNs; the test ends it): the next due recheck hides the prompt
    // again, and the next check arms it.
    channel.prompt_state = {};
    channel.verdict.recheck = 1;
    expect_prompt(true, "after the refusal's window");
    channel.RequestNearbyCheck(now());
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(channel.LatestNearby(armed));
    EXPECT_TRUE(armed.armed);
    // The same through the game's own selection (the hunter in the prompt zone).
    permission_value = 0;
    const auto refused_selected = now();
    channel.RequestOffer(refused_selected);
    selected(row.data(), block.data(), reinterpret_cast<u64>(actor.data()), 0);
    ASSERT_TRUE(channel.LatestOffer(offered));
    EXPECT_EQ(offered.request_ns, refused_selected);
    EXPECT_FALSE(offered.candidate.eligible);
    EXPECT_NE(messages.str().find("(selected) rejected: game permission denied"),
              std::string::npos);
    permission_value = 1;
    expect_prompt(false, "refused in the prompt zone: prompt back");
    EXPECT_LE(channel.verdict.recheck, L::UnboundRecheck);
    channel.prompt_state = {};
    channel.verdict.recheck = 1;
    expect_prompt(true, "after that window");

    // A hold taken from beside the housing while this lever's verdict is not bound yet (its
    // recheck not due): its prompt and button are hidden on the very next pass.
    channel.verdict.bound = 0;
    channel.verdict.recheck = L::UnboundRecheck;
    expect_prompt(false, "not bound yet");
    channel.RequestOffer(now());
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(channel.LatestOffer(offered));
    ASSERT_TRUE(offered.candidate.eligible);
    ASSERT_TRUE(send(0.3f, L::CommandKind::Hold, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    expect_bone(0.3f, "a hold from beside the housing");
    expect_prompt(true, "held: hidden at once");
    ASSERT_TRUE(send(0.3f, L::CommandKind::Release, now()));
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_EQ(locals, rest);
    EXPECT_EQ(channel.work.load(), 0);

    // B at the lever (the XR side bumps the fallback serial): its prompt and button come back.
    messages.str("");
    channel.verdict.recheck = 1;
    expect_prompt(true, "bound before B");
    channel.prompt_fallback.fetch_add(1);
    expect_prompt(false, "B fallback");
    expect_prompt(false, "B fallback holds");
    EXPECT_NE(messages.str().find("normal prompt kept (B or a refused pull: for now)"),
              std::string::npos);
    // While it is back, the hand path is off: not armed, and a squeeze is answered without the
    // game's permission, so the game's own pull never meets a held lever.
    channel.RequestNearbyCheck(now());
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(channel.LatestNearby(armed));
    EXPECT_FALSE(armed.armed);
    const auto permissions_in_fallback = permission_calls;
    channel.RequestOffer(now());
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    ASSERT_TRUE(channel.LatestOffer(offered));
    EXPECT_FALSE(offered.candidate.eligible);
    EXPECT_EQ(permission_calls, permissions_in_fallback);
    expect_prompt(false, "B fallback after a squeeze");

    // The area unloads (its action block leaves the game's table): disarmed, nothing is served.
    Put<s32>(action_manager, 0x20, 0);
    channel.RequestNearbyCheck(now());
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_FALSE(channel.LatestNearby(armed));
    EXPECT_NE(messages.str().find("disarmed: its area or the player changed"), std::string::npos);
    Put<s32>(action_manager, 0x20, 1);
    const auto permissions_after_disarm = permission_calls;
    channel.RequestOffer(now());
    update(queue.data(), objects.data(), 3, 5, 7, 11, 13);
    EXPECT_NE(channel.acquire.load(), 0); // not served: the XR side times it out
    EXPECT_EQ(permission_calls, permissions_after_disarm);
    channel.acquire.store(0);
    EXPECT_EQ(locals, rest); // none of this ever moved the lever
    Common::Log::ALL_LOGGERS.clear();
}
} // namespace

// The child process gives this configuration its own installed-hook singleton.
TEST(LeverProbe, DefaultLaunchIdleSkipsHostCallbacksAndDiagnosticHooks) {
    EXPECT_EXIT(([] {
        using namespace Xbyak::util;
        Module module;
        if (!module.data) std::_Exit(2);
        module.Seed();
        _putenv_s("VR_LEVER_PROBE", "0");
        _putenv_s("VR_LEVER_PHYSICAL_TEST", "");
        _putenv_s("VR_LEVER_MOTION_TEST", "");
        module.Install();
        auto &channel = VR::Lever::Channel();
        if (!channel.installed.load() ||
            std::memcmp(module.data + Permission, PermissionBytes.data(), PermissionBytes.size()) ||
            std::memcmp(module.data + Pose, PoseBytes.data(), PoseBytes.size()))
            std::_Exit(3);
        const auto trap_callback = [&](size_t site) {
            u8 *gate{};
            std::memcpy(&gate, module.data + site + 6, 8);
            for (size_t i = 0; i < 512; ++i) {
                if (gate[i] == 0xFF && gate[i+1] == 0x25 &&
                    gate[i+2] == 0 && gate[i+3] == 0 && gate[i+4] == 0 && gate[i+5] == 0) {
                    u8 *callback{};
                    std::memcpy(&callback, gate + i + 6, 8);
                    callback[0] = 0x0F; callback[1] = 0x0B;
                    FlushInstructionCache(GetCurrentProcess(), callback, 2);
                    return;
                }
            }
            std::_Exit(4);
        };
        Xbyak::CodeGenerator selected_tail(32, module.data + Select + SelectBytes.size());
        selected_tail.setz(dl);
        selected_tail.movzx(edx, dl);
        selected_tail.shl(rdx, 32);
        selected_tail.or_(rax, rdx);
        selected_tail.ret();
        Xbyak::CodeGenerator selected_entry(256);
        for (const auto &r : {rbp, r13}) selected_entry.push(r);
        selected_entry.mov(rbp, rsp);
        selected_entry.sub(rsp, 0x88);
        selected_entry.mov(ptr[rbp - 0x70], rdi);
        selected_entry.mov(qword[rbp - 0x78], 0);
        selected_entry.mov(r13, rsi);
        selected_entry.mov(eax, 23);
        selected_entry.mov(r11, reinterpret_cast<u64>(module.data + Select));
        selected_entry.call(r11);
        selected_entry.add(rsp, 0x88);
        selected_entry.pop(r13); selected_entry.pop(rbp); selected_entry.ret();
        std::array<u8, 0x140> row{};
        std::array<u8, 0xD8> block{};
        const s32 param = 2410000;
        std::memcpy(row.data() + 0x80, &param, 4);
        using SelectFn = u64(PS4_SYSV_ABI *)(u8 *, u8 *);
        auto selected = selected_entry.getCode<SelectFn>();
        // A listed row the host classifies once (no object resolves here: no rig, so the
        // prompt stays and no hint). After that, the idle selection never enters the host
        // until its recheck is due, and an unbound lever never advances the hint.
        if (selected(row.data(), block.data()) != (23ULL | (1ULL << 32)) ||
            channel.verdict.row != reinterpret_cast<u64>(row.data()) || channel.verdict.bound ||
            channel.verdict.recheck != VR::Lever::UnboundRecheck)
            std::_Exit(8);
        trap_callback(Select);
        trap_callback(Update);
        for (u64 i = 1; i <= 20; ++i) {
            if (selected(row.data(), block.data()) != (23ULL | (1ULL << 32)) ||
                channel.selected_hint.load() != 0 ||
                channel.verdict.recheck != VR::Lever::UnboundRecheck - i)
                std::_Exit(5);
        }
        row[0x80] = 0; // an unrelated selected action never reaches the host either
        selected(row.data(), block.data());
        if (channel.selected_hint.load() != 0 ||
            channel.verdict.recheck != VR::Lever::UnboundRecheck - 20)
            std::_Exit(6);
        Xbyak::CodeGenerator update_tail(64, module.data + Update + UpdateBytes.size());
        update_tail.add(rsp, 0x38);
        for (const auto &r : {rbx, r12, r13, r14, r15, rbp}) update_tail.pop(r);
        update_tail.mov(eax, 91); update_tail.ret();
        using UpdateFn = u64(PS4_SYSV_ABI *)();
        if (reinterpret_cast<UpdateFn>(module.data + Update)() != 91) std::_Exit(7);
        std::_Exit(0);
    }()), ::testing::ExitedWithCode(0), "");
}

TEST(LeverProbe, InstalledDetoursPreserveSelectionPermissionAndEventArguments) {
    RunInstalledDetours();
}
TEST(LeverProbe, MotionOnlyMovesDeniedLeverWithoutCallingGameActivation) {
    EXPECT_EXIT(([] {
        RunInstalledDetours(true);
        std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
    }()), ::testing::ExitedWithCode(0), "");
}

// The Forbidden Woods lever (o270003, row 2700000): o240001's skeleton on a taller housing.
// The real detours bind it by its own rest pose and drive it through the same hold, release,
// activation and rehearsal paths.
TEST(LeverProbe, ForbiddenWoodsLeverBindsItsOwnRig) {
    const auto &woods = VR::Lever::Models[1];
    ASSERT_STREQ(woods.name, "o270003");
    for (bool motion : {false, true}) {
        EXPECT_EXIT(( [&] {
            RunInstalledDetours(motion, 2700000, woods);
            std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
        }()), ::testing::ExitedWithCode(0), "");
    }
}

// The Central Yharnam lift levers (o243005 on the generic row 100): their own four-bone skeleton
// resting tilted -12 degrees. The real detours classify and bind them per instance (other models
// on row 100 keep the prompt), and drive them through hold, release, activation and rehearsal.
TEST(LeverProbe, CentralYharnamLiftLeverBindsOnTheGenericRow) {
    const VR::Lever::Model *lift = nullptr;
    for (const auto &m : VR::Lever::Models)
        if (std::strcmp(m.name, "o243005") == 0)
            lift = &m;
    ASSERT_NE(lift, nullptr);
    for (bool motion : {false, true}) {
        EXPECT_EXIT(( [&] {
            RunInstalledDetours(motion, 100, *lift);
            std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
        }()), ::testing::ExitedWithCode(0), "");
    }
}

TEST(LeverProbe, AdditionalMapActionsPreserveNativeActivationAndRehearsal) {
    for (const s32 action : {2400000, 2420000}) {
        for (bool motion : {false, true}) {
            EXPECT_EXIT(( [&] {
                RunInstalledDetours(motion, action);
                std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
            }()), ::testing::ExitedWithCode(0), "");
        }
    }
}

// The rigs added from the lever catalog (2026-10-14) through the real detours, each on its own
// row: the twins (o342202 and o352001 on o240001's skeleton, o352000 on o243005's rest under its
// own names, the library lever o253010, which stays pulled, with Cainhurst's bone scale), and the
// measured o220005, o260041, o280001 (three bones, a raised root) and o360010 (three bones, a
// rotated rest about -Y, its own bone scale).
TEST(LeverProbe, CatalogLeversBindTheirOwnRigsOnTheirOwnRows) {
    for (const char *name : {"o342202", "o352001", "o352000", "o253010", "o220005", "o260041",
                             "o280001", "o360010"}) {
        const VR::Lever::Model *model = nullptr;
        for (const auto &m : VR::Lever::Models)
            if (std::strcmp(m.name, name) == 0)
                model = &m;
        ASSERT_NE(model, nullptr) << name;
        for (bool motion : {false, true}) {
            EXPECT_EXIT(([&] {
                RunInstalledDetours(motion, model->actions[0], *model);
                std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
            }()), ::testing::ExitedWithCode(0), "") << name << (motion ? " (motion)" : "");
        }
    }
}

namespace {
const VR::Lever::Model &ProbeModel(const char *name) {
    for (const auto &m : VR::Lever::Models)
        if (std::strcmp(m.name, name) == 0)
            return m;
    static const VR::Lever::Model none{};
    ADD_FAILURE() << name;
    return none;
}
void RunInChild(const VR::Lever::Model &model, s32 row) {
    for (bool motion : {false, true}) {
        EXPECT_EXIT(([&] {
            RunInstalledDetours(motion, row, model);
            std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
        }()), ::testing::ExitedWithCode(0), "") << model.name << (motion ? " (motion)" : "");
    }
}
} // namespace

// Cainhurst's shortcut elevator lever (o250010, row 2500010): its lever bone's own scale (0.81,
// 0.46, 0.81) is in the live locals, so the rig binds at it, draws the knob 1.15 m out as the game
// does, and every write keeps the scale. A Chalice door lever's skeleton (the same bone names)
// keeps the game's prompt there.
TEST(LeverProbe, CainhurstLeverBindsItsScaledRig) {
    const auto &cainhurst = ProbeModel("o250010");
    ASSERT_GT(VR::Body::Length(cainhurst.rest_s - VR::Body::Vec3{1, 1, 1}), 0.2f);
    RunInChild(cainhurst, 2500010);
}

// The Mensis cage and lift levers (o260040, row 2600000): a four-bone skeleton under a two-bone
// mesh, so the matrix provider's table has two entries.
TEST(LeverProbe, MensisCageLeverBindsWithFewerMeshBonesThanHavokBones) {
    const auto &cages = ProbeModel("o260040");
    ASSERT_EQ(cages.mesh_count, 2u);
    ASSERT_EQ(cages.count, 4u);
    RunInChild(cages, 2600000);
}

// Byrgenwerth's Lecture Building lever (o321110, the generic row 100): a three-bone skeleton under
// a four-bone mesh whose last bone draws from no Havok bone; a completed pull settles to where its
// own clip leaves it (61.4 degrees) and stays there.
TEST(LeverProbe, LectureLeverBindsWithAnUnmappedMeshBone) {
    const auto &lecture = ProbeModel("o321110");
    ASSERT_EQ(lecture.mesh_count, 4u);
    ASSERT_EQ(lecture.count, 3u);
    ASSERT_LT(lecture.mesh_map[3], 0);
    ASSERT_GT(lecture.end_deg, 0.0f);
    ASSERT_LT(lecture.end_deg, lecture.sweep_deg);
    RunInChild(lecture, 100);
}

// The Research Hall's capstan (o353010, row 3500110): three bones, the bar turning under a root
// turned -90 degrees about X and a column turned back, its mesh table read FLVER -> Havok ({1, 2,
// 0}). Through the real detours: it binds at rest, a hold turns the bar, a hand letting go short of
// its stop parks it there (the prompt stays replaced, the game's word and its scan carry the angle,
// a squeeze resumes the very bone), a refused completion parks it at its stop, a squeeze there
// completes even when its release reaches the game first, and a completed or 97% turn stays turned;
// B at a parked capstan gives the game its button back; rehearsal returns it to rest.
TEST(LeverProbe, ResearchHallCapstanParksResumesAndStaysTurned) {
    const auto &capstan = ProbeModel("o353010");
    ASSERT_EQ(capstan.motion, VR::Lever::Motion::Capstan);
    ASSERT_EQ(capstan.lever, 2);
    ASSERT_EQ(VR::Lever::Chain(capstan).n, 2u);
    RunInChild(capstan, 3500110);
}
