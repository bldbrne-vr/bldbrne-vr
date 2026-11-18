// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>

// The Doll's respec (vr.ini doll_respec, default 1). The user, 2026-11-06: respec "at the same
// level" from the Doll "in the game's level up", see "health, stamina, dmg ... how they change",
// "if cancel while at it = we retain our stats", "all points need to be added for the respec to
// complete", and "have the character's current stats and allow to subtract from one and add to
// another". Pure rules, unit-tested in tests/test_doll_respec.cpp; the game side is
// core/vr/bloodborne_respec.cpp (its stubs carry out Accept, RequiredShown and Gate).
//
// The Doll's list gets "Rechannel Blood Echoes" under "Channel Blood Echoes"; it opens her own
// Level Up (its help line "Respec: move points between attributes.") on the hunter's own build (the game's original values: its stat previews compare with
// the hunter as he is). The minimum of each stat across all nine origins is its floor.
// Freed levels are placed for free; confirmation requires exactly the original level.
// The saved origin never changes. Cancelling leaves the saved build untouched.
namespace VR::Respec {

// PlayerGameData's order: Vitality, Endurance, Strength, Skill, Bloodtinge, Arcane.
using Stats = std::array<std::int32_t, 6>;

struct Build {
    std::int32_t level{0};
    Stats stats{};
    bool operator==(const Build&) const = default;
};

inline constexpr std::int32_t MaxStat = 99;
// The combined per-stat floors need not describe a valid build. Never let the menu's
// temporary level reach zero/negative while points await redistribution.
inline constexpr std::int32_t MinPendingLevel = 1;
inline constexpr int OriginCount = 9;               // Milquetoast (0) .. Waste of Skin (8)
inline constexpr std::int32_t OriginRowBase = 3000; // CharaInitParam row of origin 0

// The Doll's list and our line in it. Her scripts use the indices 1, 2, 3 and 50 (Leave); 0 is the
// list closed with Back. No talk script uses ours or its text id.
inline constexpr std::int32_t ChannelText = 15000100; // "Channel Blood Echoes" (event texts)
inline constexpr std::int32_t ChannelIndex = 1;
inline constexpr std::int32_t RespecIndex = 7701;
inline constexpr std::int32_t RespecText = 15000101;
inline constexpr std::int32_t EventTextCategory = 30;
inline constexpr std::int32_t TitleTextCategory = 200;  // the menus' static texts (titles)
inline constexpr std::int32_t HelpTextCategory = 201;   // the menus' help lines
inline constexpr std::int32_t DialogTextCategory = 204; // the menus' dialogs
inline constexpr std::int32_t LevelUpText = 211000;     // title "Level Up"; help "Select
                                                        // attribute to boost."; dialog "Spend
                                                        // <?demandSoul?> ..."
inline constexpr std::int32_t NoChangeText = 211002;    // dialog "No attribute selected."
inline constexpr std::int32_t ConfirmRowText = 211010;  // help "Spend Blood Echoes and level up."
inline constexpr char16_t RespecLine[] = u"Rechannel Blood Echoes";
inline constexpr char16_t RespecTitle[] = u"Respec";
inline constexpr char16_t RespecHelp[] = u"Respec: move points between attributes.";
inline constexpr char16_t RespecConfirmHelp[] = u"Keep the new build.";
inline constexpr char16_t ConfirmQuestion[] = u"Keep this build?";
inline constexpr char16_t SpendFreed[] = u"Spend every freed level first.";

// While freed levels are left (`left` > 0), the attributes' help line and the Confirm row's (and
// the confirm's refusal) count them, live as points move.
inline std::string LeftToPlace(std::int32_t left) {
    return "Respec: " + std::to_string(left) + (left == 1 ? " level" : " levels") +
           " left to place.";
}
inline std::string PlaceFirst(std::int32_t left) {
    return "Place " + std::to_string(left) + (left == 1 ? " more level" : " more levels") +
           " first.";
}

// Every Bloodborne build: level = the six attributes' sum - 50 (the origins too), each 1..99.
inline bool Consistent(const Build& b) {
    std::int32_t sum = 0;
    for (const auto v : b.stats) {
        if (v < 1 || v > MaxStat)
            return false;
        sum += v;
    }
    return b.level >= 1 && b.level == sum - 50;
}

// Read all rows before accepting any floors; a missing/malformed origin must not
// silently change the rules. Only run at menu entry, never on the frame path.
inline std::optional<Stats> Floors(const std::array<Build, OriginCount>& origins) {
    Stats floors{};
    floors.fill(MaxStat);
    for (const auto& origin : origins) {
        if (!Consistent(origin))
            return std::nullopt;
        for (std::size_t i = 0; i < floors.size(); ++i)
            floors[i] = std::min(floors[i], origin.stats[i]);
    }
    return floors;
}

// Already-respecced hunters can be below their saved origin's stats. Validate against
// the shared floors instead, so they can respec again after a save/reload.
inline const char* RefuseReason(const Build& now, const Stats& floors) {
    if (!Consistent(now))
        return "the hunter's build is not a valid build";
    bool movable = false;
    for (std::size_t i = 0; i < now.stats.size(); ++i) {
        if (floors[i] < 1 || floors[i] > MaxStat)
            return "invalid respec floor";
        if (now.stats[i] < floors[i])
            return "an attribute is below the respec minimum";
        movable |= now.stats[i] > floors[i];
    }
    return movable && now.level > MinPendingLevel ? nullptr : "no levels available to move";
}

// One attribute's step in the menu (its own check, 0x1B2F5E0, returns the value it takes). Down:
// to the shared floor without making the pending level invalid. Up: only with freed points.
// These rules are mirrored by EmitStepStub; regular levelling remains a separate Doll option.
enum class Step : std::uint8_t { Take, Keep, Game };
inline Step Accept(std::int32_t current, std::int32_t requested, std::int32_t floor,
                   std::int32_t level, std::int32_t original_level) {
    if (requested < 1 || requested > MaxStat)
        return Step::Keep;
    if (requested == current)
        return Step::Game;
    if (requested < current)
        return requested >= floor &&
                       static_cast<std::int64_t>(level) + requested - current >= MinPendingLevel
                   ? Step::Take : Step::Keep;
    return static_cast<std::int64_t>(level) + requested - current <= original_level
               ? Step::Take : Step::Keep;
}

// The respec costs no Blood Echoes, including when all freed points have been placed.
inline bool RequiredIsFree(std::int32_t level, std::int32_t original_level) {
    return level <= original_level;
}

// The menu's confirm. Blocked: a freed level is left (the message SpendFreed). Respec: the same
// level, a new build (the question ConfirmQuestion, no Blood Echoes). Game: unchanged (its "No
// attribute selected."). An unexpected level above the original is also refused.
enum class Gate : std::uint8_t { Blocked, Respec, Game };
inline Gate ConfirmGate(std::int32_t level, std::int32_t original_level, bool changed) {
    if (level != original_level)
        return Gate::Blocked;
    return level == original_level && changed ? Gate::Respec : Gate::Game;
}

} // namespace VR::Respec
