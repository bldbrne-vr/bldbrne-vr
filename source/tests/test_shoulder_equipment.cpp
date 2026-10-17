// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/logging/log.h"
#include "core/vr/bloodborne_shoulder.h"
#include "core/vr/body_channel.h"
#include "core/vr/shoulder_equipment.h"
#include "core/vr/hook_util.h"
#include "core/vr/vr_session.h"
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <windows.h>
#include <xbyak/xbyak.h>

namespace Common {
std::string GetCurrentThreadName() { return "shoulder-equipment-test"; }
namespace Log {
std::unordered_map<std::string_view, std::shared_ptr<spdlog::logger>> ALL_LOGGERS;
}
} // namespace Common
namespace {
s64 now = 1'000'000'000;
bool cinematic = false, menu = false, immersive = true;
VR::Body::BodyStatus body;
} // namespace
namespace VR {
Settings& GetSettings() {
    static Settings settings;
    return settings;
}
bool IsImmersive() { return immersive; }
bool CinematicActive() { return cinematic; }
bool MenuPanelActive() { return menu; }
namespace Body {
s64 SteadyNowNs() { return now; }
bool LatestBodyStatus(BodyStatus& out) {
    out = body;
    return true;
}
} // namespace Body
} // namespace VR
namespace {
namespace E = VR::ShoulderEquipment;
namespace B = VR::Bloodborne;
template <class T> void Put(u8* p, T value) { std::memcpy(p, &value, sizeof(value)); }
int unequips{}, equips{}, copies{};
constexpr s32 FistHandle = 900, FistIndex = 4;
void PS4_SYSV_ABI NativeCopyRef(u8* destination, const u8* source) {
    ++copies;
    // Real game copies maintain reference counts; an invalid fabricated reference
    // is not a valid bare hand, even when its displayed weapon ID says bare fists.
    EXPECT_GT(E::Read<s32>(source), 0);
    E::Write(destination, E::Read<s32>(source));
}
void PS4_SYSV_ABI NativeUnequip(u8* egd, s32 index) {
    ++unequips;
    ASSERT_GE(index, 0);
    ASSERT_LT(index, 4);
    E::Write(egd + 0xd4 + 4 * index, E::BareFists);
    E::Write(egd + 0x94 + 4 * index, FistHandle);
    E::Write(egd + 0x24 + 4 * index, FistIndex);
    E::Write(egd + 0x78, 1);
}
void PS4_SYSV_ABI NativeSetEquip(u8* egd, s32 index, const s32* handle, s32 inventory, s32 force) {
    ++equips;
    ASSERT_EQ(force, 1);
    const auto list = E::Read<u8*>(egd + 0x1a0);
    const auto item = list + 16 * inventory;
    ASSERT_EQ(E::Read<s32>(item), *handle);
    E::Write(egd + 0xd4 + 4 * index, E::Read<s32>(item + 4));
    E::Write(egd + 0x94 + 4 * index, *handle);
    E::Write(egd + 0x24 + 4 * index, inventory);
    E::Write(egd + 0x78, 1);
}
TEST(ShoulderEquipment, NativeTransactionsRememberOtherSlotWithoutOverridingIdleFrames) {
    constexpr size_t size = 0x553E900, site = 0x18F5249;
    auto* module = static_cast<u8*>(
        VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(module, nullptr);
    const u8 refresh[] = {0x41, 0x8b, 0x87, 0x20, 3, 0, 0, 0x41, 0x89, 0x85, 0xf8, 4, 0, 0};
    const u8 copy[] = {0x41, 0x8b, 0x84, 0x1f, 0xa4, 2, 0, 0, 0x41, 0x89, 0x44, 0x1c, 0x64};
    const u8 select[] = {0x41, 0x8b, 0x87, 0x4c, 2, 0, 0, 0x41, 0x89, 0x44, 0x24, 0x0c,
                         0x41, 0x8b, 0x87, 0x50, 2, 0, 0, 0x41, 0x89, 0x44, 0x24, 0x10};
    const u8 main[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89,
                       0xfb, 0x4c, 0x8d, 0x35, 0x67, 0xbb, 0xc7, 3};
    const u8 unequip[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                          0x53, 0x48, 0x81, 0xec, 0x98, 0,    0,    0,    0x89, 0xf3, 0x49, 0x89};
    const u8 set[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
                      0x48, 0x83, 0xec, 0x28, 0x89, 0xcb, 0x49, 0x89, 0xd4, 0x41, 0x89, 0xf7};
    const u8 ref[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                      0x53, 0x50, 0x49, 0x89, 0xf6, 0x48, 0x89, 0xfb};
    std::memcpy(module + site, refresh, sizeof(refresh));
    std::memcpy(module + 0x18F5220, copy, sizeof(copy));
    std::memcpy(module + 0x18F51CF, select, sizeof(select));
    std::memcpy(module + 0x18C2D00, main, sizeof(main));
    std::memcpy(module + 0x14CD1C0, unequip, sizeof(unequip));
    std::memcpy(module + 0x14CCB90, set, sizeof(set));
    std::memcpy(module + 0x1A8BD80, ref, sizeof(ref));
    module[0x1894f60] = 0x55; // save copy must remain untouched
    module[0x14CD1C0] ^= 1;
    B::InstallShoulderEquipment(reinterpret_cast<VAddr>(module), size);
    EXPECT_NE(module[site], 0xff);
    EXPECT_FALSE(B::RequestShoulderEquipment(1, true));
    module[0x14CD1C0] ^= 1;
    B::InstallShoulderEquipment(reinterpret_cast<VAddr>(module), size);
    ASSERT_EQ(module[site], 0xff);
    EXPECT_EQ(module[0x1894f60], 0x55);
    VR::Hook::WriteJump(module + 0x14CD1C0, 14, reinterpret_cast<void*>(&NativeUnequip));
    VR::Hook::WriteJump(module + 0x14CCB90, 14, reinterpret_cast<void*>(&NativeSetEquip));
    VR::Hook::WriteJump(module + 0x1A8BD80, 14, reinterpret_cast<void*>(&NativeCopyRef));
    module[site + sizeof(refresh)] = 0xc3;
    alignas(8) std::array<u8, 0x600> player{};
    alignas(8) std::array<u8, 0x400> saved{};
    alignas(8) std::array<u8, 0x100> equipment{}, world{}, inventory{};
    Put(module + 0x553E878, world.data());
    Put(world.data() + 0x60, player.data());
    Put(player.data() + 0x3c0, saved.data());
    Put(player.data() + 0x4b8, equipment.data());
    Put(saved.data() + 0x370, inventory.data());
    E::Write(saved.data() + 0x338, 16);
    E::Write(saved.data() + 0x3b0, 4);
    for (int i = 0; i < 15; ++i) E::Write(saved.data() + 0x1f4 + 4*i, -1);
    for (int i = 0; i < 4; ++i) {
        E::Write(saved.data() + 0x2a4 + 4 * i, 2000000 + 100000 * i);
        E::Write(saved.data() + 0x264 + 4 * i, 100 + i);
        E::Write(saved.data() + 0x1f4 + 4 * i, i);
        E::Write(inventory.data() + 16 * i, 100 + i);
        E::Write(inventory.data() + 16 * i + 4, 2000000 + 100000 * i);
        E::Write(inventory.data() + 16 * i + 8, 1);
    }
    E::Write(saved.data() + 0x248, 2);
    VR::GetSettings().shoulder_switch = true;
    VR::GetSettings().controls_classic = false;
    body.driven = true;
    body.arm_weight = {1, 1};
    Xbyak::CodeGenerator run(256);
    using namespace Xbyak::util;
    run.push(r12);
    run.push(r13);
    run.push(r15);
    run.mov(r12, reinterpret_cast<u64>(equipment.data()));
    run.mov(r13, reinterpret_cast<u64>(player.data()));
    run.mov(r15, reinterpret_cast<u64>(saved.data()));
    run.mov(rax, reinterpret_cast<u64>(module + site));
    run.call(rax);
    run.pop(r15);
    run.pop(r13);
    run.pop(r12);
    run.ret();
    run.ready();
    const auto frame = [&] {
        now += 16'000'000;
        body.steady_ns = now;
        std::memcpy(equipment.data(), saved.data() + 0x240, 0xa4);
        run.getCode<void (*)()>()();
    };
    frame();
    const auto original = saved;
    for (int i = 0; i < 10; ++i)
        frame();
    EXPECT_EQ(saved, original);
    EXPECT_EQ(unequips + equips + copies, 0); // normal weapon switching has no native override
    ASSERT_TRUE(B::RequestShoulderEquipment(1, true));
    frame();
    EXPECT_EQ(unequips, 1);
    EXPECT_TRUE(B::ShoulderUnarmed(1));
    EXPECT_EQ(B::ShoulderRestoreSlot(1), 1); // restore slot 2, never hard-coded slot 1
    EXPECT_EQ(E::Read<s32>(equipment.data() + 0x10), 1);
    EXPECT_EQ(E::Read<s32>(equipment.data() + 0x70), E::BareFists);
    EXPECT_EQ(E::Read<s32>(equipment.data() + 0x30), FistHandle);
    EXPECT_EQ(E::Read<s32>(saved.data() + 0x2a8), 2100000);  // original held slot remains equipped
    EXPECT_EQ(E::Read<s32>(inventory.data() + 3 * 16), 103); // removed weapon remains owned
    const auto unarmed = saved;
    now += 2'000'000'000; // a pause must not lose the remembered weapon
    for (int i = 0; i < 20; ++i)
        frame();
    EXPECT_EQ(saved, unarmed);
    EXPECT_EQ(unequips, 1);
    EXPECT_EQ(copies, 1);
    ASSERT_TRUE(B::RequestShoulderEquipment(1, false));
    frame();
    EXPECT_FALSE(B::ShoulderUnarmed(1));
    EXPECT_EQ(equips, 1);
    EXPECT_EQ(E::Read<s32>(equipment.data() + 0x70), 2300000);
    EXPECT_EQ(E::Read<s32>(equipment.data() + 0x30), 103);
    EXPECT_EQ(E::Read<s32>(equipment.data() + 0x10), 1);
    // If the removed weapon was manually equipped in the other slot, use that
    // slot instead of putting the same inventory instance into two slots.
    ASSERT_TRUE(B::RequestShoulderEquipment(1, true));
    frame();
    const s32 moved_handle = 101;
    NativeSetEquip(saved.data() + 0x1d0, 3, &moved_handle, 1, 1);
    frame();
    const int equips_before_moved_restore = equips;
    ASSERT_TRUE(B::RequestShoulderEquipment(1, false));
    frame();
    EXPECT_EQ(equips, equips_before_moved_restore);
    EXPECT_FALSE(B::ShoulderUnarmed(1));
    EXPECT_EQ(E::Read<s32>(equipment.data() + 0x10), 1);
    EXPECT_EQ(E::Read<s32>(saved.data() + 0x2a8), E::BareFists);
    EXPECT_EQ(E::Read<s32>(saved.data() + 0x2b0), 2100000);
    const s32 original_handle = 103;
    NativeSetEquip(saved.data() + 0x1d0, 3, &original_handle, 3, 1);
    NativeSetEquip(saved.data() + 0x1d0, 1, &moved_handle, 1, 1);
    frame();
    // Both hands are independent; from slot 2 the next hold targets slot 1.
    ASSERT_TRUE(B::RequestShoulderEquipment(1, true));
    ASSERT_TRUE(B::RequestShoulderEquipment(0, true));
    frame();
    EXPECT_EQ(B::ShoulderRestoreSlot(1), 0);
    EXPECT_EQ(B::ShoulderRestoreSlot(0), 1);
    ASSERT_TRUE(B::RequestShoulderEquipment(0, false));
    frame();
    EXPECT_FALSE(B::ShoulderUnarmed(0));
    EXPECT_TRUE(B::ShoulderUnarmed(1));
    // A manual edit of an emptied slot is respected and drops the old memory.
    E::Write(saved.data() + 0x2a8, 9990000);
    frame();
    EXPECT_FALSE(B::ShoulderUnarmed(1));
    EXPECT_EQ(E::Read<s32>(saved.data() + 0x2a8), 9990000);
    E::Write(saved.data() + 0x2a8, 2100000);
    // Refuse to restore an inventory index that now belongs to a different item.
    ASSERT_TRUE(B::RequestShoulderEquipment(1, true));
    frame();
    const int before = equips;
    E::Write(inventory.data() + 3 * 16, 999);
    ASSERT_TRUE(B::RequestShoulderEquipment(1, false));
    frame();
    EXPECT_EQ(equips, before);
    EXPECT_FALSE(B::ShoulderUnarmed(1)); // abandon an unavailable item instead of trapping taps
    EXPECT_EQ(E::Read<s32>(equipment.data() + 0x10), 0);
    EXPECT_EQ(E::Read<s32>(equipment.data() + 0x68), 2100000);
    // Subsequent ordinary slot switches stay ordinary; the stale memory is gone.
    E::Write(saved.data() + 0x250, 1);
    frame();
    EXPECT_FALSE(B::ShoulderUnarmed(1));
    E::Write(inventory.data() + 3 * 16, 103);
    // An already empty next slot switches there and a tap draws the prior slot.
    NativeUnequip(saved.data() + 0x1d0, 3);
    E::Write(saved.data() + 0x250, 0);
    frame();
    ASSERT_TRUE(B::RequestShoulderEquipment(1, true));
    frame();
    EXPECT_EQ(B::ShoulderRestoreSlot(1), 0);
    ASSERT_TRUE(B::RequestShoulderEquipment(1, false));
    frame();
    EXPECT_EQ(E::Read<s32>(equipment.data() + 0x10), 0);
    // Expired and menu-time requests never execute later.
    const int calls = unequips;
    ASSERT_TRUE(B::RequestShoulderEquipment(0, true));
    now += 300'000'000;
    frame();
    EXPECT_EQ(unequips, calls);
    menu = true;
    ASSERT_TRUE(B::RequestShoulderEquipment(0, true));
    frame();
    EXPECT_EQ(unequips, calls);
    menu = false;
    ASSERT_TRUE(B::RequestShoulderEquipment(0, true));
    frame();
    VR::GetSettings().shoulder_switch = false;
    frame();
    EXPECT_FALSE(B::ShoulderUnarmed(0));
    EXPECT_EQ(module[0x1894f60], 0x55);
}
} // namespace
