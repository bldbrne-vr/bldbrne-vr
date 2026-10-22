// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <cmath>
#include <cstdint>
#include <iosfwd>
#include <string>

// The physical whip (core/vr/whip_chain.h, core/vr/whip_spin.h, core/vr/bloodborne_whip.h): its
// vr.ini keys. Parsed by VR::LoadSettings (keys it does not know go to ParseWhipSetting) and written
// with the defaults when vr.ini is created. Read at install (a restart changes `whip`).
namespace VR::Whip {

struct WhipSettings {
    // physical: a whip form (the Beast Cutter's and the Threaded Cane's form B) is a chain
    // simulated on your hand: swung fast it pays out toward its reach cap, stays out while you
    // keep it moving (spin it overhead: it circles like a helicopter blade) and reels back in when
    // you slow down; its hit capsules ride the chain you see (no attack animation, the game's
    // attack rows; the default since 2026-10-06); canned: a swing presses R1 / R2 (the game's
    // attack animations), as before. InstallWhip leaves this saying what is installed (VR_WHIP, a
    // failed check: canned).
    bool physical{true};
    float reach_scale{1.0f};   // the payout's reach scale (1 = the default tuned caps)
    float hold_s{0.6f};        // slow this long, the whip reels back in
    float payout_speed{5.5f};  // m/s (the weapon's far end): the whip fully out from this speed
    bool flick_recall{true};   // the Beast Cutter's whip: a snap of the wrist reels the chain straight in
    // ... in any direction (whip_flick_recall=any, as before 2026-10-15: a crack reels it in too);
    // else (=1) only a snap lifting the weapon back, never a crack thrown out (whip_flick.h)
    bool flick_any{false};
    // The whip forms' damage held to the game's own pace (core/vr/whip_damage.h PaceMeter): their
    // windows register sooner than the game's (the budget's quicker refills, spins, cracks), but a
    // window that hurts before the game could have started its move deals less by the share of
    // that move's recovery it is early. 0: every whip window that registers hits in full (before
    // 2026-10-15).
    bool pace{true};
};

WhipSettings& GetWhipSettings();

bool ParseWhipSetting(const std::string& key, const std::string& value);
void WriteWhipDefaults(std::ostream& out);
std::string WhipSettingsText();

// ---- Between the presenter and the game's update (lock-free, latest value) ---------------------

// One coherent presenter sample: speed and compact-mode intent must belong to the same frame.
struct SwingInput {
    std::int64_t ns{0};
    float speed{0.0f};
    bool compact{false}; // Beast Cutter in physical form B; right trigger held
};
inline SwingInput FreshSwingInput(SwingInput input, std::int64_t now_ns,
                                 std::int64_t max_age_ns) {
    if (input.ns <= 0 || now_ns < input.ns || now_ns - input.ns > max_age_ns) return {};
    if (!std::isfinite(input.speed) || input.speed < 0.0f) input.speed = 0.0f;
    return input;
}
void PublishSwingInput(const SwingInput& input);
SwingInput LatestSwingInput(std::int64_t now_ns, std::int64_t max_age_ns);

// The chain as the hook left it (the presenter's haptics: the hum, the lock-out tick).
struct WhipFeel {
    std::int64_t ns{0};
    float out{0.0f};       // the payout: 0 the idle stick .. 1 the light's full reach (more: heavy)
    float tip_speed{0.0f}; // m/s, the chain's tip (world)
    std::uint32_t locks{0};    // the chain reached its full payout (counted)
    std::uint32_t reels{0};    // ... and reeled back in to the stick (counted)
    bool driving{false};
    std::uint32_t lash{0};  // the lash (counted: the chain asked out by a swing, until it is back)
    bool asked_out{false};  // the payout asks it out now (not reeling in)
    bool lifting{false};    // the Beast Cutter's flick recall lifting it back: no crack
};
void PublishWhipFeel(const WhipFeel& f);
WhipFeel LatestWhipFeel();

} // namespace VR::Whip
