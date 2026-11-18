// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/vr/bloodborne_respec.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/vr/doll_respec.h"
#include "core/vr/hook_util.h"
#include "core/vr/respec_feel.h"
#include "core/vr/test_stats.h"
#include "core/vr/vr_session.h"

// The Doll's respec (core/vr/doll_respec.h has the rules). Game side, all verified in the 1.09
// executable (CUSA03173):
// - The talk scripts' commands reach the talk listener (0x203FFD0: listener, event; the event's id
//   at +8, its arguments from +0x18, 16 bytes each, their count at +0x200). Command 19 adds a line
//   to the talk list (index, text, flag), 20 clears it, 10 shows it. Our line goes in right after
//   "Channel Blood Echoes" through the same listener, with an event built as the script engine
//   builds its own (0x2745CB0).
// - Function 23 (the talk functions' switch at 0x203AD60) answers the line picked: MenuMan +0xEC0.
//   Ours is answered as "Channel Blood Echoes" (1), so her script runs her own Level Up (her line,
//   the hunter's kneel, command 31), and the next Level Up job is the respec's.
// - The Level Up job (constructor 0x1B2C640: job, owner, build) keeps the original build (+0xE78:
//   level, then the six attributes) and the pending one (+0xE94). Each attribute's row checks a
//   step with a small function (0x1B2F5E0: this {vtable, job, &original}, &current, &requested ->
//   the value taken): down to the original at the lowest; up when the Blood Echoes cover the next
//   level. Stub: shared origin minimums; only redistribute at the original level.
// - The required Blood Echoes field (0x1B2F800, value 10009) prices the next level from the pending
//   level: 0 for a freed level.
// - The confirm (0x1B2E600; r15 the job past its prologue) goes to its "No attribute selected."
//   branch while a freed level is left; its texts are ours then (the text lookup 0xF6F340: the
//   message repository, set, category, id -> UTF-16 or null), as are the menu's help lines and the
//   question for a same-level build. The menu's own level-up (0x1C13D00) writes the build.
// - The menus' screens fill their "@StaticText_<id>" fields through the lookup as they are built
//   (0x1F58C30: static texts are category 200), the Level Up title (211000) before the job's
//   constructor: it is answered "Respec" from the pick on.
// - The menu keeps the pointers the lookup returns (its texts, 0x1AE8CC0, copy nothing): the
//   respec's help lines are buffers of ours, rewritten in place by the required field's refresh
//   (after every step) to count the freed levels left. The list menus put the focused row's help
//   line on the screen only when the cursor moves (0x1B1AB20: the job); the refresh calls it too.
namespace VR::Bloodborne {
namespace {

using Hook::Plausible;
using Hook::Read;
namespace R = VR::Respec;

constexpr u64 GameDataManRva = 0x553B130; // +8: the main player's PlayerGameData
constexpr u64 WorldChrManRva = 0x553E878; // +0x60: the main player
constexpr u64 MenuManRva = 0x5562878;     // +0xEC0: the talk list's picked index
constexpr u32 OffsetListResult = 0xEC0;
constexpr u32 OffsetChrModules = 0x3B0; // +0x20: the data module (HP +0xF8, max HP +0xFC)
constexpr u32 OffsetHp = 0xF8, OffsetMaxHp = 0xFC;

constexpr u64 TalkEventRva = 0x203FFD0;
constexpr std::array<u8, 20> TalkEventSig = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41,
                                             0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                             0x81, 0xEC, 0xC8, 0x03, 0x00, 0x00};
constexpr u64 ListResultRva = 0x203BB5D; // function 23's load of MenuMan +0xEC0 and its jump out
constexpr std::array<u8, 18> ListResultSig = {0x48, 0x8B, 0x05, 0x14, 0x6D, 0x52,
                                              0x03, 0x8B, 0x80, 0xC0, 0x0E, 0x00,
                                              0x00, 0xE9, 0x60, 0x33, 0x00, 0x00};
constexpr u64 ListResultDoneRva = 0x203EECF; // stores eax as the function's int value
constexpr u64 EventInitRva = 0x2745CB0;      // (event, id): vtable, id (an int), no arguments
constexpr std::array<u8, 7> EventInitSig = {0x48, 0x8D, 0x05, 0x89, 0x15, 0xB8, 0x02};
constexpr u32 EventArgs = 0x18, EventArgSize = 0x10, EventCount = 0x200, EventSize = 0x210;
constexpr s32 ValueInt = 2, ValueFloat = 1;
constexpr s32 CommandShowList = 10, CommandAddLine = 19, CommandClearList = 20;

constexpr u64 TextRva = 0xF6F340;
constexpr std::array<u8, 15> TextSig = {0x89, 0xF0, 0x48, 0x8B, 0x77, 0x08, 0x48, 0x8B,
                                        0x34, 0xC6, 0x31, 0xC0, 0x48, 0x85, 0xF6};
constexpr u64 JobRva = 0x1B2C640;
constexpr std::array<u8, 20> JobSig = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41,
                                       0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                       0x81, 0xEC, 0x28, 0x09, 0x00, 0x00};
constexpr u64 StepRva = 0x1B2F5E0;
constexpr std::array<u8, 17> StepSig = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                        0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x18};
constexpr u64 RequiredRva = 0x1B2F8FF; // the pending level loaded for the next level's price
constexpr std::array<u8, 14> RequiredSig = {0x41, 0x8B, 0x9D, 0x94, 0x0E, 0x00, 0x00,
                                            0xC7, 0x45, 0x80, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr u64 RequiredShowRva = 0x1B2FA7B; // sets field 10009 to r14d, its colour to ebx
constexpr u64 ConfirmRva = 0x1B2E626;
constexpr std::array<u8, 20> ConfirmSig = {0x41, 0x8B, 0x87, 0x94, 0x0E, 0x00, 0x00,
                                           0x41, 0x3B, 0x87, 0x78, 0x0E, 0x00, 0x00,
                                           0x0F, 0x85, 0x2C, 0x01, 0x00, 0x00};
constexpr size_t ConfirmLen = 14; // the load and the compare; the jne after them is kept
constexpr u64 NoChangeRva = 0x1B2E6B2;
constexpr u64 JobGoneRva = 0x1B31020; // the Level Up job's destructor (job)
constexpr std::array<u8, 17> JobGoneSig = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                           0x55, 0x41, 0x54, 0x53, 0x50, 0x49, 0x89, 0xFE};
constexpr u64 CharaInitRowRva = 0x1F14E20;
constexpr std::array<u8, 8> CharaInitRowSig = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56};
constexpr u32 JobOriginalLevel = 0xE78, JobOriginalStats = 0xE7C, JobPendingLevel = 0xE94;
constexpr u64 ShowHelpRva = 0x1B1AB20; // (job): the focused row's help line onto the screen
constexpr std::array<u8, 40> ShowHelpSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x81, 0xEC, 0x18, 0x01,
    0x00, 0x00, 0x49, 0x89, 0xFE, 0x4C, 0x8B, 0x3D, 0xA6, 0x90, 0x8C, 0x03, 0x49, 0x8B,
    0x07, 0x48, 0x89, 0x45, 0xE0, 0x41, 0x8B, 0x9E, 0xE8, 0x0A, 0x00, 0x00};

struct RowOut {
    s32 id{-1};
    u32 pad{};
    u8* row{};
};
using RowFn = void(PS4_SYSV_ABI*)(RowOut*, s32);
using EventInitFn = void(PS4_SYSV_ABI*)(u8* event, s32 id);
using TalkEventFn = void(PS4_SYSV_ABI*)(u8* listener, u8* event);
using ShowHelpFn = void(PS4_SYSV_ABI*)(u8* job);

u8* g_base{};
bool g_ready{};
std::unique_ptr<Xbyak::CodeGenerator> g_code;

// Read by the stubs (guest code): the respec's open Level Up job (0: none) and its floors (the
// lowest starting attributes across origins). The floors are written before the job is published.
std::atomic<u64> g_job{};
std::array<s32, 6> g_floor{};
// Set by function 23's stub when our line was picked (the next Level Up job is the respec's).
std::atomic<u8> g_picked{};
// Set by the job's destructor stub when the respec's job went (g_job is 0 from then on).
std::atomic<u8> g_closed{};
static_assert(sizeof(std::atomic<u64>) == 8 && sizeof(std::atomic<u8>) == 1);
// The talk list's last line was "Channel Blood Echoes" (the script thread's own state).
bool g_after_channel{};
// The texts the text stub answers while the respec's menu is open (the menu keeps the pointers):
// the attributes' help line, the Confirm row's and the confirm's refusal. Set when a respec opens,
// then rewritten in place by the menu's thread as freed levels come and go; each one's last
// character stays the terminator.
using Line = std::array<char16_t, 48>;
Line g_help_line{}, g_confirm_line{}, g_place_first{};
// One more for every respec opened.
std::atomic<u32> g_generation{};
// What the lines show (the menu's thread only: the required field's refresh).
struct Shown {
    u32 generation{}; // the respec (g_generation)
    s32 left{};       // the freed levels counted (0: the plain lines)
};
Shown g_shown;

void SetLine(Line& line, std::u16string_view text) {
    const size_t n = std::min(text.size(), line.size() - 1);
    std::copy_n(text.begin(), n, line.begin());
    line[n] = 0;
}

std::u16string Utf16(const std::string& ascii) {
    return {ascii.begin(), ascii.end()};
}

struct State {
    std::mutex mutex;
    enum class Phase : u8 { Idle, Open, Closing };
    std::atomic<Phase> phase{Phase::Idle};
    u8* job{};
    u8* pgd{};
    R::Build before{}; // the hunter's build when the menu opened
    R::Build kept{};   // the last build written while it is open (the heartbeat's)
    s64 picked_ns{}; // when the tick first saw g_picked (dropped after 15 s without the menu)
    s64 closing_ns{};
};
State g;

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

u8* PlayerGameData() {
    u8* const gdm = Read<u8*>(g_base + GameDataManRva);
    if (!Plausible(gdm))
        return nullptr;
    u8* const pgd = Read<u8*>(gdm + 8);
    return Plausible(pgd) ? pgd : nullptr;
}

R::Build ReadBuild(const u8* pgd) {
    R::Build b{Read<s32>(pgd + VR::TestStats::LevelOffset), {}};
    for (size_t i = 0; i < b.stats.size(); ++i)
        b.stats[i] = Read<s32>(pgd + VR::TestStats::Offsets[i]);
    return b;
}

std::string Text(const R::Build& b) {
    return fmt::format("level {} ({}/{}/{}/{}/{}/{})", b.level, b.stats[0], b.stats[1], b.stats[2],
                       b.stats[3], b.stats[4], b.stats[5]);
}

// The origin's starting build, from the game's own CharaInitParam row.
std::optional<R::Build> OriginBuild(u8 origin) {
    if (origin >= R::OriginCount)
        return std::nullopt;
    RowOut out{};
    reinterpret_cast<RowFn>(g_base + CharaInitRowRva)(&out, R::OriginRowBase + origin);
    if (!Plausible(out.row, 1))
        return std::nullopt;
    R::Build b{Read<s16>(out.row + 0xC0), {}};
    constexpr std::array<u32, 6> Fields{0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC8};
    for (size_t i = 0; i < Fields.size(); ++i)
        b.stats[i] = Read<u8>(out.row + Fields[i]);
    return b;
}

// Resolve and validate every origin, including after loading another character. No
// persistent cache: params may not yet be ready the first time the Doll is approached.
std::optional<R::Stats> RespecFloors() {
    std::array<R::Build, R::OriginCount> origins{};
    for (u8 i = 0; i < R::OriginCount; ++i) {
        const auto row = OriginBuild(i);
        if (!row)
            return std::nullopt;
        origins[i] = *row;
    }
    return R::Floors(origins);
}

// The menu's level-up moves HP by the change of max HP (0x1C13E65): a lower Vitality from a hunter
// below full health would reach 0 and kill him (mock 2026-11-06, a 99 -> 15 build: HP 0/750). Full
// health when the respec opens: a new build starts at its own max.
void FillHealth() {
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    u8* const player = Plausible(world) ? Read<u8*>(world + 0x60) : nullptr;
    u8* const modules = Plausible(player) ? Read<u8*>(player + OffsetChrModules) : nullptr;
    u8* const data = Plausible(modules) ? Read<u8*>(modules + 0x20) : nullptr;
    if (Plausible(data) && Read<s32>(data + OffsetHp) > 0) {
        const s32 max_hp = Read<s32>(data + OffsetMaxHp);
        std::memcpy(data + OffsetHp, &max_hp, 4);
    }
}

// The respec's end, its menu gone (g.mutex held): the build the hunter left with (the heartbeat
// when it is new to the last one kept).
void End() {
    u8* const pgd = PlayerGameData();
    const R::Build now_build = pgd != nullptr && pgd == g.pgd ? ReadBuild(pgd) : g.kept;
    if (now_build != g.kept)
        R::Feel::kept.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO(Core, "VR respec: ended, {}: {} -> {}",
             now_build == g.before ? "the hunter's build as it was" : "a new build",
             Text(g.before), Text(now_build));
    g.phase = State::Phase::Idle;
    g.job = nullptr;
    g.pgd = nullptr;
}

// The Level Up job's constructor (the menu's thread): the respec's when our line was picked.
void PS4_SYSV_ABI JobOpening(u8* job, u8* /*saved_ymm*/, u8* build) {
    if (g_picked.exchange(0) == 0) {
        // An ordinary Level Up where an old respec job was: never the respec's.
        u64 expected = reinterpret_cast<u64>(job);
        g_job.compare_exchange_strong(expected, 0);
        return;
    }
    std::lock_guard lock(g.mutex);
    g.picked_ns = 0;
    // Picked again as soon as the Doll's list was back: the last respec ends now.
    if (g.phase != State::Phase::Idle && g_job.load() == 0)
        End();
    if (!VR::GetSettings().doll_respec || g.phase != State::Phase::Idle)
        return;
    u8* const pgd = PlayerGameData();
    if (pgd == nullptr || !Plausible(build, 4)) {
        LOG_WARNING(Core, "VR respec: refused, no player game data at the Level Up menu");
        return;
    }
    const R::Build before = ReadBuild(pgd);
    R::Build given{Read<s32>(build), {}};
    for (size_t i = 0; i < given.stats.size(); ++i)
        given.stats[i] = Read<s32>(build + 4 + 4 * i);
    const auto floors = RespecFloors();
    const char* why = given != before ? "the menu was not given the hunter's build"
                      : !floors       ? "origin starting builds unavailable or invalid"
                                      : R::RefuseReason(before, *floors);
    if (why != nullptr) {
        LOG_WARNING(Core, "VR respec: refused ({}): {}; the game's own Level Up", why,
                    Text(before));
        return;
    }
    g_floor = *floors;
    FillHealth();
    g.before = before;
    g.kept = before;
    g.job = job;
    g.pgd = pgd;
    g.phase = State::Phase::Open;
    g_closed = 0;
    SetLine(g_help_line, R::RespecHelp);
    SetLine(g_confirm_line, R::RespecConfirmHelp);
    SetLine(g_place_first, R::SpendFreed);
    g_generation.fetch_add(1, std::memory_order_relaxed);
    g_job.store(reinterpret_cast<u64>(job), std::memory_order_release);
    LOG_INFO(Core, "VR respec: open for {}; floors {}/{}/{}/{}/{}/{}; same level, origin unchanged",
             Text(before), g_floor[0], g_floor[1], g_floor[2], g_floor[3], g_floor[4], g_floor[5]);
}

// The required field's refresh (the menu's thread, after every step): the help lines and the
// confirm's refusal count the freed levels left, and the focused row's goes onto the screen at
// once (the menu shows it again only when the cursor moves).
void PS4_SYSV_ABI LinesRefresh(u8* job, u8* /*saved_ymm*/) {
    if (reinterpret_cast<u64>(job) != g_job.load(std::memory_order_acquire))
        return;
    const u32 generation = g_generation.load(std::memory_order_relaxed);
    if (generation != g_shown.generation)
        g_shown = {generation, 0};
    const s32 left =
        std::max(Read<s32>(job + JobOriginalLevel) - Read<s32>(job + JobPendingLevel), 0);
    if (left == g_shown.left)
        return;
    g_shown.left = left;
    if (left == 0) {
        SetLine(g_help_line, R::RespecHelp);
        SetLine(g_confirm_line, R::RespecConfirmHelp);
    } else {
        const std::u16string first = Utf16(R::PlaceFirst(left));
        SetLine(g_help_line, Utf16(R::LeftToPlace(left)));
        SetLine(g_confirm_line, first);
        SetLine(g_place_first, first);
    }
    reinterpret_cast<ShowHelpFn>(g_base + ShowHelpRva)(job);
}

// Whether the hunter the game has now can respec (the line is offered only then; again as soon as
// the last respec's menu is gone).
bool RespecOffered() {
    if (!VR::GetSettings().doll_respec || g_job.load(std::memory_order_relaxed) != 0)
        return false;
    u8* const pgd = PlayerGameData();
    if (pgd == nullptr)
        return false;
    const auto floors = RespecFloors();
    return floors && R::RefuseReason(ReadBuild(pgd), *floors) == nullptr;
}

s32 EventInt(const u8* event, u32 index) {
    const u8* const value = index == 0 ? event + 8 : event + EventArgs + EventArgSize * (index - 1);
    const s32 type = Read<s32>(value + 8);
    return type == ValueInt     ? Read<s32>(value)
           : type == ValueFloat ? static_cast<s32>(Read<float>(value))
                                : 0;
}

// Our line through the talk listener, as the script's own command 19 would add it.
void AddRespecLine(u8* listener) {
    alignas(16) std::array<u8, EventSize> event{};
    reinterpret_cast<EventInitFn>(g_base + EventInitRva)(event.data(), CommandAddLine);
    const std::array<s32, 3> args{R::RespecIndex, R::RespecText, -1};
    for (u32 i = 0; i < args.size(); ++i) {
        u8* const value = event.data() + EventArgs + EventArgSize * i;
        std::memcpy(value, &args[i], 4);
        std::memcpy(value + 8, &ValueInt, 4);
    }
    const s32 count = static_cast<s32>(args.size());
    std::memcpy(event.data() + EventCount, &count, 4);
    reinterpret_cast<TalkEventFn>(g_base + TalkEventRva)(listener, event.data());
}

// The talk listener's entry (the script's thread): our line goes in when the list moves on from
// "Channel Blood Echoes" (its next line, or the list shown with it last). Re-entered by our own
// line, which is not hers.
void PS4_SYSV_ABI TalkEvent(u8* listener, u8* /*saved_ymm*/, u8* event) {
    const s32 id = EventInt(event, 0);
    if (id == CommandClearList) {
        g_after_channel = false;
        return;
    }
    const bool add = id == CommandAddLine && Read<s32>(event + EventCount) == 3;
    if (g_after_channel && (add || id == CommandShowList)) {
        g_after_channel = false;
        if (RespecOffered())
            AddRespecLine(listener);
    }
    if (add && EventInt(event, 2) == R::ChannelText)
        g_after_channel = true;
}

bool Matches(u64 rva, const u8* sig, size_t length, u64 size, const char* what) {
    if (size < rva + length || std::memcmp(g_base + rva, sig, length) != 0) {
        LOG_WARNING(Core, "VR respec: OFF, {} differs at {:#x}: {}", what, rva,
                    size < rva + length ? std::string{"eboot too small"}
                                        : Hook::HexBytes(g_base + rva, length));
        return false;
    }
    return true;
}

// jmp qword [rip+0]; dq target.
void JumpTo(Xbyak::CodeGenerator& c, u64 target) {
    c.db(0xFF);
    c.db(0x25);
    c.dd(0);
    c.dq(target);
}

// The replayed bytes of a site, then back to the game after them.
template <size_t N>
void Replay(Xbyak::CodeGenerator& c, const std::array<u8, N>& sig, size_t length, u64 rva) {
    for (size_t i = 0; i < length; ++i)
        c.db(sig[i]);
    JumpTo(c, reinterpret_cast<u64>(g_base + rva + length));
}

// rax = the respec's job, or the game's way (`original`) when none is open.
void LoadJob(Xbyak::CodeGenerator& c, Xbyak::Label& original) {
    using namespace Xbyak::util;
    c.mov(rax, reinterpret_cast<u64>(&g_job));
    c.mov(rax, qword[rax]);
    c.test(rax, rax);
    c.jz(original, Xbyak::CodeGenerator::T_NEAR);
}

// The text lookup's entry (repo, set, category edx, id ecx; r10 free): our line's text, the
// respec's menu title, and while that menu is open its help lines, its same-level question and its
// freed-level message.
void* EmitTextStub(Xbyak::CodeGenerator& c) {
    using namespace Xbyak::util;
    Xbyak::Label menu, id_main, id_freed, id_confirm, original;
    c.align(16);
    void* const entry = const_cast<u8*>(c.getCurr());
    const auto answer = [&](const char16_t* text) {
        c.mov(rax, reinterpret_cast<u64>(text));
        c.ret();
    };
    c.cmp(ecx, R::RespecText);
    c.jne(menu);
    c.cmp(edx, R::EventTextCategory);
    c.jne(menu);
    answer(R::RespecLine);
    c.L(menu);
    // The title (static text 211000): filled as the menu's screen is built, before its job, so
    // answered from the pick on.
    Xbyak::Label not_title;
    c.cmp(ecx, R::LevelUpText);
    c.jne(not_title);
    c.cmp(edx, R::TitleTextCategory);
    c.jne(not_title);
    c.mov(rax, reinterpret_cast<u64>(&g_picked));
    c.movzx(eax, byte[rax]);
    c.mov(r10, reinterpret_cast<u64>(&g_job));
    c.or_(rax, qword[r10]);
    c.jz(original);
    answer(R::RespecTitle);
    c.L(not_title);
    LoadJob(c, original);
    c.mov(r10d, dword[rax + JobPendingLevel]);
    c.sub(r10d, dword[rax + JobOriginalLevel]); // < 0: a freed level left
    c.cmp(ecx, R::LevelUpText);
    c.je(id_main);
    c.cmp(ecx, R::NoChangeText);
    c.je(id_freed);
    c.cmp(ecx, R::ConfirmRowText);
    c.je(id_confirm);
    c.jmp(original);
    c.L(id_main); // 211000: the attributes' help line; the dialog's question at the same level
    Xbyak::Label not_help;
    c.cmp(edx, R::HelpTextCategory);
    c.jne(not_help);
    answer(g_help_line.data());
    c.L(not_help);
    c.cmp(edx, R::DialogTextCategory);
    c.jne(original);
    c.test(r10d, r10d);
    c.jnz(original);
    answer(R::ConfirmQuestion);
    c.L(id_freed); // 211002: the dialog's "No attribute selected." while a freed level is left
    c.cmp(edx, R::DialogTextCategory);
    c.jne(original);
    c.test(r10d, r10d);
    c.jns(original);
    answer(g_place_first.data());
    c.L(id_confirm); // 211010: the Confirm row's help line
    c.cmp(edx, R::HelpTextCategory);
    c.jne(original);
    answer(g_confirm_line.data());
    c.L(original);
    Replay(c, TextSig, TextSig.size(), TextRva);
    return entry;
}

// An attribute's step check, at its entry (rdi {vtable, job, &original}, rsi &current, rdx
// &requested; eax the value taken; r10, r11 free): R::Accept for the respec's job.
void* EmitStepStub(Xbyak::CodeGenerator& c) {
    using namespace Xbyak::util;
    Xbyak::Label down, take, keep, original;
    c.align(16);
    void* const entry = const_cast<u8*>(c.getCurr());
    LoadJob(c, original);
    c.cmp(rax, qword[rdi + 8]);
    c.jne(original, Xbyak::CodeGenerator::T_NEAR);
    c.mov(r10d, dword[rdx]); // requested
    c.cmp(r10d, 1);
    c.jl(keep);
    c.cmp(r10d, R::MaxStat);
    c.jg(keep);
    c.cmp(r10d, dword[rsi]);
    c.je(original, Xbyak::CodeGenerator::T_NEAR);
    c.jl(down);
    // Check the entire requested delta, not just a single point.
    c.mov(r11d, dword[rax + JobPendingLevel]);
    c.add(r11d, r10d);
    c.sub(r11d, dword[rsi]);
    c.cmp(r11d, dword[rax + JobOriginalLevel]);
    c.jg(keep);
    c.mov(rax, reinterpret_cast<u64>(&R::Feel::placed));
    c.lock();
    c.inc(dword[rax]);
    c.jmp(take);
    c.L(down);
    c.mov(r11d, dword[rax + JobPendingLevel]);
    c.add(r11d, r10d);
    c.sub(r11d, dword[rsi]);
    c.cmp(r11d, R::MinPendingLevel);
    c.jl(keep);
    // The shared floor is indexed by the row's own original's place.
    c.mov(r11, qword[rdi + 0x10]);
    c.sub(r11, rax);
    c.sub(r11, JobOriginalStats);
    c.cmp(r11, 4 * 5);
    c.ja(original, Xbyak::CodeGenerator::T_NEAR);
    c.test(r11, 3);
    c.jnz(original, Xbyak::CodeGenerator::T_NEAR);
    c.mov(rax, reinterpret_cast<u64>(g_floor.data()));
    c.cmp(r10d, dword[rax + r11]);
    c.jl(keep);
    c.mov(rax, reinterpret_cast<u64>(&R::Feel::freed));
    c.lock();
    c.inc(dword[rax]);
    c.L(take);
    c.mov(eax, r10d);
    c.ret();
    c.L(keep);
    c.mov(eax, dword[rsi]);
    c.ret();
    c.L(original);
    Replay(c, StepSig, StepSig.size(), StepRva);
    return entry;
}

// The required Blood Echoes field (r13 the job, r12 the field, rbx / r14 free there): 0 and the
// normal colour for a freed level. For the respec's job its help lines first (LinesRefresh).
void* EmitRequiredStub(Xbyak::CodeGenerator& c) {
    using namespace Xbyak::util;
    Xbyak::Label original, other;
    c.align(16);
    void* const field = const_cast<u8*>(c.getCurr());
    LoadJob(c, original);
    c.cmp(rax, r13);
    c.jne(original);
    c.mov(eax, dword[r13 + JobPendingLevel]);
    c.cmp(eax, dword[r13 + JobOriginalLevel]);
    c.jg(original);
    c.xor_(ebx, ebx);
    c.xor_(r14d, r14d);
    JumpTo(c, reinterpret_cast<u64>(g_base + RequiredShowRva));
    c.L(original);
    Replay(c, RequiredSig, RequiredSig.size(), RequiredRva);
    void* const lines = Hook::EmitHostCallStub(c, reinterpret_cast<void*>(&LinesRefresh), nullptr,
                                               0, reinterpret_cast<u64>(field), r13);
    c.align(16);
    void* const entry = const_cast<u8*>(c.getCurr());
    c.mov(rax, reinterpret_cast<u64>(&g_job));
    c.cmp(r13, qword[rax]);
    c.jne(other);
    JumpTo(c, reinterpret_cast<u64>(lines));
    c.L(other);
    JumpTo(c, reinterpret_cast<u64>(field));
    return entry;
}

// The confirm past its prologue (r15 the job): require exactly the original level.
void* EmitConfirmStub(Xbyak::CodeGenerator& c) {
    using namespace Xbyak::util;
    Xbyak::Label original;
    c.align(16);
    void* const entry = const_cast<u8*>(c.getCurr());
    LoadJob(c, original);
    c.cmp(rax, r15);
    c.jne(original);
    c.mov(eax, dword[r15 + JobPendingLevel]);
    c.cmp(eax, dword[r15 + JobOriginalLevel]);
    c.je(original);
    c.mov(rax, reinterpret_cast<u64>(&R::Feel::refused));
    c.lock();
    c.inc(dword[rax]);
    JumpTo(c, reinterpret_cast<u64>(g_base + NoChangeRva));
    c.L(original);
    Replay(c, ConfirmSig, ConfirmLen, ConfirmRva);
    return entry;
}

// The Level Up job's destructor entry (rdi the job): the respec's job going turns the stubs off.
void* EmitJobGoneStub(Xbyak::CodeGenerator& c) {
    using namespace Xbyak::util;
    Xbyak::Label original;
    c.align(16);
    void* const entry = const_cast<u8*>(c.getCurr());
    c.mov(rax, reinterpret_cast<u64>(&g_job));
    c.cmp(rdi, qword[rax]);
    c.jne(original);
    c.mov(qword[rax], 0);
    c.mov(rax, reinterpret_cast<u64>(&g_closed));
    c.mov(byte[rax], 1);
    c.L(original);
    Replay(c, JobGoneSig, JobGoneSig.size(), JobGoneRva);
    return entry;
}

// Function 23 (the line picked): ours answered as "Channel Blood Echoes", and the respec armed.
void* EmitListResultStub(Xbyak::CodeGenerator& c) {
    using namespace Xbyak::util;
    Xbyak::Label done;
    c.align(16);
    void* const entry = const_cast<u8*>(c.getCurr());
    c.mov(rax, reinterpret_cast<u64>(g_base + MenuManRva));
    c.mov(rax, qword[rax]);
    c.mov(eax, dword[rax + OffsetListResult]);
    c.cmp(eax, R::RespecIndex);
    c.jne(done);
    c.mov(rax, reinterpret_cast<u64>(&g_picked));
    c.mov(byte[rax], 1);
    c.mov(eax, R::ChannelIndex);
    c.L(done);
    JumpTo(c, reinterpret_cast<u64>(g_base + ListResultDoneRva));
    return entry;
}

} // Anonymous namespace

void RespecEbootLoaded(VAddr address, u64 size) {
    if (g_ready)
        return;
    g_base = reinterpret_cast<u8*>(address);
    struct Site {
        u64 rva;
        const u8* sig;
        size_t length;
        size_t patched;
        const char* what;
    };
    const std::array<Site, 10> sites{{
        {TalkEventRva, TalkEventSig.data(), TalkEventSig.size(), TalkEventSig.size(),
         "the talk listener"},
        {ListResultRva, ListResultSig.data(), ListResultSig.size(), ListResultSig.size(),
         "talk function 23"},
        {EventInitRva, EventInitSig.data(), EventInitSig.size(), 0, "the script event"},
        {TextRva, TextSig.data(), TextSig.size(), TextSig.size(), "the text lookup"},
        {JobRva, JobSig.data(), JobSig.size(), JobSig.size(), "the Level Up job"},
        {StepRva, StepSig.data(), StepSig.size(), StepSig.size(), "the attribute step"},
        {RequiredRva, RequiredSig.data(), RequiredSig.size(), RequiredSig.size(),
         "the required Blood Echoes"},
        {ConfirmRva, ConfirmSig.data(), ConfirmSig.size(), ConfirmLen, "the Level Up confirm"},
        {JobGoneRva, JobGoneSig.data(), JobGoneSig.size(), JobGoneSig.size(),
         "the Level Up job's destructor"},
        {ShowHelpRva, ShowHelpSig.data(), ShowHelpSig.size(), 0, "the menus' help line"},
    }};
    for (const Site& s : sites)
        if (!Matches(s.rva, s.sig, s.length, size, s.what))
            return;
    if (!Matches(CharaInitRowRva, CharaInitRowSig.data(), CharaInitRowSig.size(), size,
                 "the CharaInitParam row lookup"))
        return;
    for (const Site& s : sites) {
        if (s.patched != 0 && !Hook::MakeWritable(g_base + s.rva, s.patched)) {
            LOG_WARNING(Core, "VR respec: OFF, {} could not be made writable", s.what);
            return;
        }
    }
    try {
        g_code = std::make_unique<Xbyak::CodeGenerator>(4096);
        g_code->setDefaultJmpNEAR(true);
        void* const text = EmitTextStub(*g_code);
        void* const step = EmitStepStub(*g_code);
        void* const required = EmitRequiredStub(*g_code);
        void* const confirm = EmitConfirmStub(*g_code);
        void* const gone = EmitJobGoneStub(*g_code);
        void* const result = EmitListResultStub(*g_code);
        const Xbyak::Reg64 event = Xbyak::util::rsi;
        void* const talk = Hook::EmitHostCallStub(
            *g_code, reinterpret_cast<void*>(&TalkEvent), TalkEventSig.data(),
            TalkEventSig.size(), reinterpret_cast<u64>(g_base + TalkEventRva + TalkEventSig.size()),
            Xbyak::util::rdi, &event);
        const Xbyak::Reg64 build = Xbyak::util::rdx;
        void* const job = Hook::EmitHostCallStub(
            *g_code, reinterpret_cast<void*>(&JobOpening), JobSig.data(), JobSig.size(),
            reinterpret_cast<u64>(g_base + JobRva + JobSig.size()), Xbyak::util::rdi, &build);
        // The menu's sites before the talk's that lead to them.
        Hook::WriteJump(g_base + TextRva, TextSig.size(), text);
        Hook::WriteJump(g_base + StepRva, StepSig.size(), step);
        Hook::WriteJump(g_base + RequiredRva, RequiredSig.size(), required);
        Hook::WriteJump(g_base + ConfirmRva, ConfirmLen, confirm);
        Hook::WriteJump(g_base + JobGoneRva, JobGoneSig.size(), gone);
        Hook::WriteJump(g_base + JobRva, JobSig.size(), job);
        Hook::WriteJump(g_base + ListResultRva, ListResultSig.size(), result);
        Hook::WriteJump(g_base + TalkEventRva, TalkEventSig.size(), talk);
    } catch (const Xbyak::Error& e) {
        LOG_WARNING(Core, "VR respec: OFF, stubs not generated ({})", e.what());
        return;
    }
    g_ready = true;
    LOG_INFO(Core, "VR respec: installed (doll_respec={})", VR::GetSettings().doll_respec);
}

void RespecSiteA(u8* player) {
    // Two relaxed loads a frame while nothing is going on.
    if (!g_ready || player == nullptr ||
        (g_picked.load(std::memory_order_relaxed) == 0 &&
         g.phase.load(std::memory_order_relaxed) == State::Phase::Idle))
        return;
    std::lock_guard lock(g.mutex);
    const s64 now = NowNs();
    if (g_picked.load() != 0) {
        if (g.picked_ns == 0) {
            g.picked_ns = now;
        } else if (now - g.picked_ns > 15'000'000'000) {
            g_picked = 0;
            g.picked_ns = 0;
            LOG_INFO(Core, "VR respec: no Level Up menu after the Doll's line; dropped");
        }
    }
    // The stubs end with the respec's job (its destructor), never earlier: a build below the
    // hunter's level must never reach the game's own confirm.
    if (g.phase == State::Phase::Open) {
        // A build confirmed while the menu stays up (its level-up runs a little after the yes):
        // the heartbeat in both hands, and the next confirm compares with it.
        if (u8* const pgd = PlayerGameData(); pgd == g.pgd) {
            if (const R::Build now_build = ReadBuild(pgd); now_build != g.kept) {
                LOG_INFO(Core, "VR respec: a new build kept: {} -> {}", Text(g.kept),
                         Text(now_build));
                g.kept = now_build;
                R::Feel::kept.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (g_closed.exchange(0) == 0)
            return;
        g.phase = State::Phase::Closing;
        g.closing_ns = now;
    }
    if (g.phase != State::Phase::Closing)
        return;
    // The build the hunter left with, once the menu's level-up ran (its task queue, a little after
    // the confirm) or 1 s passed.
    u8* const pgd = PlayerGameData();
    if (pgd != nullptr && pgd == g.pgd && ReadBuild(pgd) == g.kept &&
        now - g.closing_ns < 1'000'000'000)
        return;
    End();
}

} // namespace VR::Bloodborne
