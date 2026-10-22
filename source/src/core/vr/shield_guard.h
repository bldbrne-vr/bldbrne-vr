// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

// shield_guard (vr.ini only, no menu row: simply on; 2026-10-18, the tester: shields a bit more
// effective without breaking the game): the Wooden Shield and the Loch Shield guard better. Their
// EquipParamWeapon rows in the running game get at least these damage cuts (% blocked) and this
// stability (guardBaseRepel: the share of a blocked hit's stamina damage the guard shrugs off).
// Hits still get through and the stamina still runs out; only the two shields' rows change. The
// values the game had are read from the row before it is written and put back when the option
// is turned off (nothing of the game's data is in here). A row whose fields are not plausible
// (another game version's layout) is left alone.
//
// Pure (tests/test_shield_guard.cpp); bloodborne_gun.cpp ShieldGuardTick writes the rows.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace VR::Shield {

// EQUIP_PARAM_WEAPON_ST (the 1.09 layout): physGuardCutRate .. thunGuardCutRate (f32, %),
// guardBaseRepel (u8).
inline constexpr std::uint32_t OffsetPhysCut = 0x30;
inline constexpr std::uint32_t OffsetMagCut = 0x34;
inline constexpr std::uint32_t OffsetFireCut = 0x38;
inline constexpr std::uint32_t OffsetThunCut = 0x3C;
inline constexpr std::uint32_t OffsetRepel = 0xF4;
inline constexpr std::uint32_t RowBytes = OffsetRepel + 1;

struct Guard {
    float phys{}, mag{}, fire{}, thun{}; // % of the damage blocked
    std::uint8_t repel{};                // stability
    bool operator==(const Guard&) const = default;
};

struct Target {
    std::int32_t weapon; // EquipParamWeapon id
    Guard at_least;
};

// The Wooden Shield: physical 75%, arcane 50, fire 35, bolt 40, stability 35. The Loch Shield:
// physical 55%, arcane / fire / bolt 80, stability 35.
inline constexpr std::array<Target, 2> Targets = {{
    {19000000, {75.0f, 50.0f, 35.0f, 40.0f, 35}},
    {19100000, {55.0f, 80.0f, 80.0f, 80.0f, 35}},
}};

inline Guard ReadGuard(const std::uint8_t* row) {
    Guard g{};
    std::memcpy(&g.phys, row + OffsetPhysCut, 4);
    std::memcpy(&g.mag, row + OffsetMagCut, 4);
    std::memcpy(&g.fire, row + OffsetFireCut, 4);
    std::memcpy(&g.thun, row + OffsetThunCut, 4);
    g.repel = row[OffsetRepel];
    return g;
}

inline void WriteGuard(std::uint8_t* row, const Guard& g) {
    std::memcpy(row + OffsetPhysCut, &g.phys, 4);
    std::memcpy(row + OffsetMagCut, &g.mag, 4);
    std::memcpy(row + OffsetFireCut, &g.fire, 4);
    std::memcpy(row + OffsetThunCut, &g.thun, 4);
    row[OffsetRepel] = g.repel;
}

// A shield's guard as the game could have it: cuts within 0 .. 100, a stability within 0 .. 100.
inline bool Plausible(const Guard& g) {
    const auto cut = [](float v) { return std::isfinite(v) && v >= 0.0f && v <= 100.0f; };
    return cut(g.phys) && cut(g.mag) && cut(g.fire) && cut(g.thun) && g.repel <= 100;
}

// The guard with `target`'s values where they are higher (never weaker than the game's own).
inline Guard Raised(const Guard& game, const Guard& target) {
    return {std::max(game.phys, target.phys), std::max(game.mag, target.mag),
            std::max(game.fire, target.fire), std::max(game.thun, target.thun),
            std::max(game.repel, target.repel)};
}

// One shield's row: on, it is raised (the game's values kept); off, they are put back. A row that
// changed under us (the game reloaded its params) is taken as the game's again.
class RowPatch {
public:
    enum class Result { Unchanged, Raised, Restored, Implausible };

    Result Step(std::uint8_t* row, const Guard& target, bool on) {
        const Guard now = ReadGuard(row);
        if (applied && now != written) {
            applied = false; // the game's own values again
        }
        if (on && !applied) {
            if (!Plausible(now)) {
                return Result::Implausible;
            }
            game = now;
            written = Raised(now, target);
            WriteGuard(row, written);
            applied = true;
            return Result::Raised;
        }
        if (!on && applied) {
            WriteGuard(row, game);
            applied = false;
            return Result::Restored;
        }
        return Result::Unchanged;
    }
    bool Applied() const {
        return applied;
    }
    const Guard& Game() const {
        return game;
    }
    const Guard& Written() const {
        return written;
    }

private:
    bool applied{false};
    Guard game{};
    Guard written{};
};

} // namespace VR::Shield
