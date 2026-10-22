// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>

// The torch held up (vr.ini torch_lift; core/vr/bloodborne_held.cpp; 2026-10-12, not yet tried in
// the game): the Torch or the Hunter's Torch in the left hand lights more the higher you hold it.
// Raised to your shoulder: one more copy of its own flame on its flame dummy (a brighter, bigger
// fire and its light); above your eyes: two. Lowered: the game's torch as it is.
//
// The flame: explicit torch_lift_ffx / torch_lift_dummy, else a character slot effect
// (PickFlame), then the verified retail resident effect (ResidentFlame). If none matches,
// add nothing and log the available effects/dummies. The resident fallback still needs a
// headset check of the visible lighting increase.
//
// Pure (no emulator dependencies; tests/test_torch_lift.cpp). Heights are OpenXR LOCAL metres:
// the lift is the left hand's height minus the head's.
namespace VR::Torch {

struct Settings {
    bool enabled{true};
    float raised_m{-0.30f};    // the hand this far below the eyes or higher: raised (1 copy)
    float high_m{0.0f};        // ... at or above the eyes: held high (2 copies)
    float hysteresis_m{0.10f}; // a level is left this much below its threshold (for Lift::DropS)
    int copies{2};             // at most this many added flames
    std::int32_t ffx{0};       // the flame's effect id (0: the torch's own, found at runtime)
    std::int32_t dummy{0};     // its dummy (0: the torch's own)
};

inline Settings& GetTorchSettings() {
    static Settings settings;
    return settings;
}

inline bool ParseTorchSetting(const std::string& key, const std::string& value) {
    auto& s = GetTorchSettings();
    if (key == "torch_lift") {
        s.enabled = value == "1" || value == "true";
    } else if (key == "torch_lift_raised_cm") {
        s.raised_m = std::clamp(std::stof(value), -80.0f, 20.0f) * 0.01f;
    } else if (key == "torch_lift_high_cm") {
        s.high_m = std::clamp(std::stof(value), -50.0f, 40.0f) * 0.01f;
    } else if (key == "torch_lift_copies") {
        s.copies = std::clamp(std::stoi(value), 0, 3);
    } else if (key == "torch_lift_ffx") {
        s.ffx = std::max(std::stoi(value), 0);
    } else if (key == "torch_lift_dummy") {
        s.dummy = std::max(std::stoi(value), 0);
    } else {
        return false;
    }
    return true;
}

inline void WriteTorchDefaults(std::ostream& out) {
    const Settings d{};
    out << "# ---- The torch held up (the Torch / Hunter's Torch in the left hand) ----\n"
        << "# 1 = holding it up lights more: from this height (cm, the hand against your eyes) "
           "one more\n# copy of its flame, from the second height two (at most torch_lift_copies)\n"
        << "torch_lift=" << (d.enabled ? 1 : 0) << "\n"
        << "torch_lift_raised_cm=" << d.raised_m * 100.0f << "\n"
        << "torch_lift_high_cm=" << d.high_m * 100.0f << "\n"
        << "torch_lift_copies=" << d.copies << "\n"
        << "# the flame's effect id and dummy (0 = the torch's own, found in the game; the log "
           "says)\n"
        << "torch_lift_ffx=" << d.ffx << "\n"
        << "torch_lift_dummy=" << d.dummy << "\n";
}

// The Torch (201) and the Hunter's Torch (200): EquipParamWeapon id / 100000.
constexpr bool IsTorchFamily(std::int32_t family) {
    return family == 200 || family == 201;
}

// Retail 1.09: both torch families have EquipParamWeapon residentSpEffectId=404;
// SpEffect 404 stateInfo=158 -> SpEffectVfx 158: midstSfxId=21051, midstDmyId=206,
// effectType=2 (left weapon). This resident effect is absent from character slot effects.
struct Flame { std::int32_t ffx{}, dummy{}; };
constexpr Flame ResidentFlame(std::int32_t family, std::int32_t variation,
                              std::int32_t resident, std::int32_t attachment) {
    return IsTorchFamily(family) && variation == 2000 && resident == 404 && attachment == 340
               ? Flame{21051, 20206} : Flame{};
}

// 0 lowered, 1 raised, 2 held high. Raised at once; a level is let go only once the hand has been
// `hysteresis_m` below its threshold for DropS (the torch bobbing or swung never flickers it).
class Lift {
public:
    static constexpr float DropS = 0.4f;

    int Level() const {
        return level;
    }
    void Reset() {
        level = 0;
        below_s = 0.0f;
    }
    int Step(float lift_m, const Settings& s, float dt) {
        const float h = s.hysteresis_m;
        const int reached = lift_m >= s.high_m ? 2 : lift_m >= s.raised_m ? 1 : 0;
        const int kept = lift_m >= s.high_m - h ? 2 : lift_m >= s.raised_m - h ? 1 : 0;
        if (reached > level || kept >= level) {
            level = std::max(level, reached);
            below_s = 0.0f;
        } else if ((below_s += dt) >= DropS) {
            level = kept;
            below_s = 0.0f;
        }
        return level;
    }

private:
    int level{0};
    float below_s{0.0f}; // under the level's keep this long
};

// Preserve the lift through short missing/invalid samples, bounded by the actual sample age.
// Call after reading the snapshots, with a clock read taken afterwards. sample_ns=0 means
// unavailable; allowed=false is a confirmed hide/stow and clears the light immediately.
class SampledLift {
public:
    static constexpr std::int64_t MaxAgeNs = 150'000'000;

    static bool Fresh(std::int64_t now, std::int64_t sample_ns) {
        return sample_ns > 0 && now >= sample_ns && now - sample_ns <= MaxAgeNs;
    }
    void Reset() {
        *this = {};
    }
    int Step(std::int64_t now, bool allowed, std::int64_t sample_ns, float height,
             const Settings& settings) {
        if (!allowed || now <= 0 || (step_ns != 0 && now < step_ns)) {
            Reset();
            return 0;
        }
        const float dt = step_ns != 0
            ? std::clamp(static_cast<float>((now - step_ns) * 1e-9), 0.0f, 0.1f) : 0.0f;
        step_ns = now;
        if (Fresh(now, sample_ns) && std::isfinite(height)) {
            valid_ns = sample_ns;
            return lift.Step(height, settings, dt);
        }
        if (!Fresh(now, valid_ns)) lift.Reset();
        return lift.Level();
    }

private:
    Lift lift{};
    std::int64_t step_ns{};
    std::int64_t valid_ns{};
};

// The flames added for a level.
constexpr int Copies(int level, const Settings& s) {
    return std::clamp(level, 0, std::max(s.copies, 0));
}

// One slot effect of the character's effect controller.
struct Node {
    std::int32_t slot{-1};
    std::int32_t req{-1};   // the effect asked for this update
    std::int32_t cur{-1};   // the effect spawned
    std::int32_t dummy{-1}; // its dummy
};

// The left weapon's part dummies (20000 + the part's own dummy id).
constexpr bool IsLeftPartDummy(std::int32_t dummy) {
    return dummy >= 20000 && dummy <= 20999;
}

// The torch's own flame among the character's slot effects: one running (or asked for) on one of
// the left weapon's part dummies, not in our own slots [ours_from, ours_to]. The first such.
inline bool PickFlame(const Node* nodes, std::size_t count, std::int32_t ours_from,
                      std::int32_t ours_to, std::int32_t& ffx, std::int32_t& dummy) {
    for (std::size_t i = 0; i < count; ++i) {
        const Node& n = nodes[i];
        if (n.slot >= ours_from && n.slot <= ours_to) {
            continue;
        }
        const std::int32_t id = n.cur >= 0 ? n.cur : n.req;
        if (id > 0 && IsLeftPartDummy(n.dummy)) {
            ffx = id;
            dummy = n.dummy;
            return true;
        }
    }
    return false;
}

} // namespace VR::Torch
