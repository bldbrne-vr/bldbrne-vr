// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common/logging/log.h"
#include "core/vr/bloodborne_status.h"
#include "core/vr/conditions_channel.h"
#include "core/vr/hook_util.h"
#include "core/vr/live_tune.h"
#include "core/vr/status_drain.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

using Hook::Plausible;
using Hook::Read;
namespace D = VR::StatusDrain;
namespace C = VR::Ui::Conditions;

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000).
// ChrIns's common update calls the resist module's Update with the module from the chr's modules:
// mov rax, [rbx+0x3B0]; mov rdi, [rax+0x28]; ... call 0x1A5C440 (the offsets used here).
constexpr u64 ResistCallRva = 0x18BDCC4;
constexpr std::array<u8, 48> ResistCallSig = {
    0x48, 0x8B, 0x83, 0xB0, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x78, 0x28, 0x48, 0x8D, 0x05, 0x5A, 0x20,
    0xA3, 0x03, 0x48, 0x83, 0xC0, 0x10, 0x48, 0x89, 0x45, 0x80, 0xC5, 0xFA, 0x10, 0x85, 0x7C, 0xFF,
    0xFF, 0xFF, 0xC5, 0xFA, 0x11, 0x45, 0x88, 0x48, 0x8D, 0x75, 0x80, 0xE8, 0x4C, 0xE7, 0x19, 0x00,
};
// The resist module's Update: vcvtsi2ss xmm0, [rip+disp] (the recovery RATE, s32); vmulss
// [rsi+8] (dt); vaddss [rdi+0x38] (the type's fraction).
constexpr u64 ResistUpdateRva = 0x1A5C440;
constexpr std::array<u8, 18> ResistUpdateSig = {
    0xC5, 0xFA, 0x2A, 0x05, 0x44, 0xBF, 0x6C, 0x03, 0xC5,
    0xFA, 0x59, 0x46, 0x08, 0xC5, 0xFA, 0x58, 0x47, 0x38,
};
constexpr size_t RateDispAt = 4;
constexpr size_t RateInsnEnd = 8;
// The chr debug page's "Poison Resist : [%d / %d]": current at +0x10, max at +0x24.
constexpr u64 ResistDebugRva = 0x1A5C9D3;
constexpr std::array<u8, 10> ResistDebugSig = {0x41, 0x8B, 0x54, 0x24, 0x10,
                                               0x41, 0x8B, 0x4C, 0x24, 0x24};
// The HUD update's data block: mov rdi, rbx; lea rsi, [rip+disp]; call (0x1F62EA4).
constexpr u64 HudDataLeaRva = 0x1F62EA4;
constexpr std::array<u8, 11> HudDataLeaSig = {0x48, 0x89, 0xDF, 0x48, 0x8D, 0x35,
                                              0x72, 0xBB, 0x5D, 0x03, 0xE8};
constexpr size_t HudDataDispAt = 6;
constexpr size_t HudDataInsnEnd = 10;
// The row filler: every row's icon to -1 (mov dword [rax+0x13F8], -1) ...
constexpr u64 RowClearRva = 0x1BFBF19;
constexpr std::array<u8, 10> RowClearSig = {0xC7, 0x80, 0xF8, 0x13, 0x00,
                                            0x00, 0xFF, 0xFF, 0xFF, 0xFF};
// ... and its compaction (the rows' +0x18 / +0x10 qwords moved up).
constexpr u64 RowMoveRva = 0x1BFC1B8;
constexpr std::array<u8, 24> RowMoveSig = {
    0x48, 0x8B, 0x94, 0x39, 0x08, 0x14, 0x00, 0x00, 0x48, 0x89, 0x94, 0xD9,
    0x08, 0x14, 0x00, 0x00, 0x48, 0x8B, 0x94, 0x39, 0x00, 0x14, 0x00, 0x00,
};

constexpr u32 OffsetChrModules = 0x3B0; // PlayerIns -> modules
constexpr u32 OffsetResistModule = 0x28;
constexpr u32 OffsetResistCur = 0x10; // s32 x5: the remaining resistance
constexpr u32 OffsetResistMax = 0x24; // s32 x5
constexpr u32 OffsetFrameTime = 0xE0; // PlayerIns: f32 game seconds of this update
constexpr u32 OffsetRows = 0x13F0;    // the HUD data block: the meter's rows
constexpr u32 RowStride = 0x28;
constexpr u32 OffsetRowIcon = 0x08; // s32, -1 = row off
constexpr u32 OffsetRowLen = 0x18;  // s32, the bar's length (L %)

bool g_drain_ok = false;
const s32* g_rate = nullptr;
std::atomic<const u8*> g_rows{nullptr};
D::State g_state{};
s32 g_logged_pct = -1;
u64 g_logged_points = 0;
u32 g_updates = 0;

bool Matches(const u8* module, u64 size, u64 rva, const u8* sig, size_t n) {
    return rva + n <= size && std::memcmp(module + rva, sig, n) == 0;
}

template <size_t N>
bool Matches(const u8* module, u64 size, u64 rva, const std::array<u8, N>& sig) {
    return Matches(module, size, rva, sig.data(), N);
}

s32 RipDisp(const u8* at) {
    return Read<s32>(at);
}

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

void InstallStatus(VAddr base, u64 size) {
    const auto* const module = reinterpret_cast<const u8*>(base);
    const bool call_ok = Matches(module, size, ResistCallRva, ResistCallSig);
    const bool update_ok = Matches(module, size, ResistUpdateRva, ResistUpdateSig);
    const bool debug_ok = Matches(module, size, ResistDebugRva, ResistDebugSig);
    g_drain_ok = call_ok && update_ok && debug_ok;
    s32 rate = D::DefaultRate;
    if (update_ok) {
        const u8* const insn = module + ResistUpdateRva;
        const u8* const at = insn + RateInsnEnd + RipDisp(insn + RateDispAt);
        if (at >= module && at + sizeof(s32) <= module + size) {
            g_rate = reinterpret_cast<const s32*>(at);
            rate = Read<s32>(at);
        }
    }
    const bool lea_ok = Matches(module, size, HudDataLeaRva, HudDataLeaSig);
    const bool rows_ok = lea_ok && Matches(module, size, RowClearRva, RowClearSig) &&
                         Matches(module, size, RowMoveRva, RowMoveSig);
    const u8* rows = nullptr;
    if (rows_ok) {
        const u8* const insn = module + HudDataLeaRva;
        const u8* const block = insn + HudDataInsnEnd + RipDisp(insn + HudDataDispAt);
        if (block >= module && block + OffsetRows + RowStride * C::MaxRows <= module + size) {
            rows = block + OffsetRows;
        }
    }
    g_rows.store(rows, std::memory_order_release);
    const auto& s = VR::GetSettings();
    LOG_INFO(Core,
             "VR status: drain {} (status_drain_pct={}: +{:.2f} points/s on the game's {}; main "
             "player only; slow/rapid poison, bleed, frenzy, never beasthood; signatures call {} "
             "update {} fields {}), build-up meter rows {} (ui_conditions={})",
             g_drain_ok ? (s.status_drain_pct > 0 ? "on" : "installed, off") : "MISMATCH: off",
             s.status_drain_pct, D::ExtraPerSecond(rate, s.status_drain_pct), rate,
             call_ok ? "ok" : "MISMATCH", update_ok ? "ok" : "MISMATCH",
             debug_ok ? "ok" : "MISMATCH",
             rows != nullptr ? "found" : "MISMATCH: the meter stays where the game draws it",
             s.ui_conditions == static_cast<s32>(C::Mode::View) ? "view" : "game");
}

void StatusSiteA(u8* player) {
    if (!g_drain_ok || player == nullptr) {
        return;
    }
    u8* const modules = Read<u8*>(player + OffsetChrModules);
    if (!Plausible(modules)) {
        return;
    }
    u8* const resist = Read<u8*>(modules + OffsetResistModule);
    if (!Plausible(resist)) {
        return;
    }
    // VR_TUNE status.drain_pct (tests: the game's rate and ours in one run).
    const s32 pct = static_cast<s32>(std::lround(
        VR::Tune::Get("status.drain_pct", static_cast<float>(VR::GetSettings().status_drain_pct))));
    std::array<s32, D::Types> cur{};
    std::array<s32, D::Types> max{};
    for (u32 i = 0; i < D::Types; ++i) {
        cur[i] = Read<s32>(resist + OffsetResistCur + 4 * i);
        max[i] = Read<s32>(resist + OffsetResistMax + 4 * i);
    }
    const s32 rate = g_rate != nullptr ? Read<s32>(reinterpret_cast<const u8*>(g_rate))
                                       : D::DefaultRate;
    const float dt = Read<float>(player + OffsetFrameTime);
    const s64 now = NowNs();
    const u32 points =
        D::Step(g_state, player, cur, max, rate, dt, pct, now, [&](u32 type, s32 expected, s32 value) {
            std::atomic_ref<s32> ref(*reinterpret_cast<s32*>(resist + OffsetResistCur + 4 * type));
            return ref.compare_exchange_strong(expected, value);
        });
    ++g_updates;
    if (pct != g_logged_pct) {
        if (g_logged_pct >= 0) {
            LOG_INFO(Core, "VR status: drain status_drain_pct={} now (+{:.2f} points/s){}", pct,
                     D::ExtraPerSecond(rate, pct), pct == 0 ? ": the game's own rate" : "");
        }
        g_logged_pct = pct;
    }
    // VR_STATUS_LOG=1: each extra point written, with the build-ups (max - current).
    static const bool log = std::getenv("VR_STATUS_LOG") != nullptr;
    if (log && points > 0) {
        LOG_INFO(Core,
                 "VR status: +{} (total {} in {} writes, {} lost) build-ups poison {}/{} rapid "
                 "{}/{} bleed {}/{} frenzy {}/{} dt {:.4f} rate {}",
                 points, g_state.points, g_state.writes, g_state.lost, max[0] - cur[0], max[0],
                 max[1] - cur[1], max[1], max[2] - cur[2], max[2], max[3] - cur[3], max[3], dt,
                 rate);
    }
    if (!log && g_state.points >= g_logged_points + 50) {
        g_logged_points = g_state.points;
        LOG_INFO(Core, "VR status: drain +{} points so far ({} writes, {} lost to a build-up)",
                 g_state.points, g_state.writes, g_state.lost);
    }
}

} // namespace VR::Bloodborne

namespace VR::Conditions {

VR::Ui::Conditions::Rows Latest() {
    namespace C = VR::Ui::Conditions;
    const u8* const rows = VR::Bloodborne::g_rows.load(std::memory_order_acquire);
    if (rows == nullptr) {
        return {};
    }
    std::array<s32, C::MaxRows> icon{};
    std::array<s32, C::MaxRows> len{};
    for (u32 i = 0; i < C::MaxRows; ++i) {
        const u8* const row = rows + VR::Bloodborne::RowStride * i;
        icon[i] = VR::Hook::Read<s32>(row + VR::Bloodborne::OffsetRowIcon);
        len[i] = VR::Hook::Read<s32>(row + VR::Bloodborne::OffsetRowLen);
    }
    return C::CountRows(icon, len);
}

} // namespace VR::Conditions
