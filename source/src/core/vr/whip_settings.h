// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <iosfwd>
#include <string>

// The physical whip (core/vr/whip_chain.h, core/vr/whip_spin.h, core/vr/bloodborne_whip.h): its
// vr.ini keys. Parsed by VR::LoadSettings (keys it does not know go to ParseWhipSetting) and written
// with the defaults when vr.ini is created. Read at install (a restart changes `whip`).
namespace VR::Whip {

// The Cutter Chain Lab's free chain (its "Copy settings" names; the user's free-swing setting of
// 2026-11-08 by default) on the Beast Cutter's chain, with its length following the chain's own
// momentum: out while the chain's front moves fast, pulling in gradually as it slows, a smash
// keeping its force a moment (mhold). The trigger and the flick back still bring it in.
struct CutterLab {
    // On by default since 2026-11-08: the swing-path lash below, tuned on the user's recordings
    // (vr.ini whip_cutter_lash=0: the original Cutter).
    bool on{true};
    float air{0.6f};      // ChainConfig::damp
    float inner{5.0f};    // ::link_velocity_damp
    float tipmass{1.35f}; // ::tip_mass
    float drive{25.0f};   // ::swing_drive
    float dspan{3.0f};    // ::drive_span
    float daccel{500.0f}; // ::drive_accel
    float bend{50.0f};    // ::max_bend, degrees
    int iters{64};        // ::constraint_iters
    int pinned{1};        // ::handle_links
    float tout{0.14f};    // ::t_out
    float tin{0.30f};     // ::t_in
    bool mompay{true};    // the length follows the momentum meter
    float mout{6.0f};     // m/s: the front half's speed around the hand that keeps it fully out
    float min{1.5f};      // ... below this, fully in
    float mhold{0.18f};   // s: the hand stopped, the meter halves in this (it comes back)
    float vact{1.0f};     // m/s: the hand's own movement (its swing, not a wrist turn) that
                          // feeds the meter
    float hithold{0.15f}; // s: after the chain hits the ground, its force held this long first
    // The guide vector (ChainConfig::guide...; 0 = the free chain alone).
    float guide{0.0f};
    float gzeta{0.3f};
    float gtaper{0.5f};
    float w0{25.0f};
    float zeta{0.35f};
    float rdrag{0.5f};
    float aim{1.0f}; // the pulls aim along the arm (chest to hand): 1 .. the controller's line: 0
    // The swing-path lash (model=path; ChainConfig::path, core/vr/cutter_path.h) instead of the
    // drive and the guide: its pull (rad/s at the root, its share left at the tip, its damping),
    // the tip's delay behind the arm's line (s) and the most it curls (degrees), the share of
    // the pull left once the hand stops (the chain flies on its own), and the swing's speed around
    // the hunter 40 cm out along the weapon (m/s) that holds it fully out (pout) or lets it all
    // in (pin); the
    // momentum halves every mhold once the hand slows, a landing holds it hithold first.
    bool path_model{true};
    float path{60.0f};
    float ptaper{0.3f};
    float pzeta{0.5f};
    float pdelay{0.18f};
    float pcurl{30.0f}; // the most it curls: its middle off the line from the hand to its tip
    float pkeep{0.3f};
    float pout{9.0f}; // (the swing's speed 40 cm out along the weapon)
    float pin{2.5f};
    float pmin{0.5f}; // once out, at least this share of the full length (until it falls below pin)
    float pturn{0.0f};  // s of slowness before the momentum fades (a turn of the stroke keeps it)
    float pslow{0.7f};  // s from full to closed as the swing slows (0: tin) ...
    float paccel{0.3f}; // ... quickening to tin after this much slowness (s; 0: stays pslow)
    float pfollow{30.0f}; // the shape's pointer (ChainConfig::path_follow, rad/s; 0: the arm's line)
    float prate{0.0f};   // ... at most this fast (deg/s; 0: any)
    float pground{0.15f}; // the pull's share on a link lying on the ground
    float pspin{1.0f};     // spun round, the shape leans out from the hand's circle this much
    float pspinacc{12.0f}; // ... in full from this much circling (m/s^2 across the hand's motion)
    float pgrav{1.4f};     // the chain's weight in its shape (1: as heavy as it is; 0: none)
};

// The whip_cutter_lab value into `lab`'s dials ("0" or "1": the built-in ones; else name=value pairs
// from them; model=guided / model=free: the Chain Lab's other chains). Whether it is on is
// whip_cutter_lash's (lab.on kept).
void ParseCutterLab(const std::string& value, CutterLab& lab);

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
    // The Beast Cutter thrown more like the game's chain (core/vr/whip_chain.h; the user,
    // 2026-11-08: tuned live, one dial at a time; every default as before):
    // whip_cutter_start: the share of whip_payout_speed that starts a payout (0.45; 0.8 tried)
    float cutter_start{0.45f};
    // whip_cutter_keep: slower than this share the pause starts (0.36; 0.65 tried)
    float cutter_keep{0.36f};
    // whip_cutter_hold_s: the pause before the chain comes back (0: HoldFor's 1.4 s; 0.35 tried)
    float cutter_hold_s{0.0f};
    // whip_cutter_throw: a payout starts only while the weapon points ahead (W::CutterThrowShare)
    bool cutter_throw{false};
    // whip_cutter_follow: a throw's front follows the hand (W::CutterThrowDrive), 0 .. 1
    float cutter_follow{0.0f};
    // whip_cutter_turn: a stroke turning back brings the chain in, the next throws it out again
    // (W::CutterStrokeTurn)
    bool cutter_turn{false};
    // whip_cutter_tip_keep: m/s; out, the chain's tip faster than this keeps it out (its flight
    // after the hand stops; 0 off: only the hand's speed)
    float cutter_tip_keep{0.0f};
    // whip_cutter_spine: ChainConfig::spine (0 off)
    float cutter_spine{0.0f};
    // whip_cutter_unfold: ChainConfig::unfold (0 off)
    float cutter_unfold{0.0f};
    // whip_cutter_full: m/s at the weapon's far end that pays the chain fully out; a weaker swing
    // pays out less of it, from whip_cutter_start up (0: whip_payout_speed)
    float cutter_full{0.0f};
    // whip_cutter_lab: 0 off, 1 the lab's free-swing setting, or its values as name=value,...
    CutterLab cutter_lab{};
    // whip_cutter_reel_s: the passive reel-in, s from out to closed (0: the chain's 0.40 s); the
    // flick back, the trigger and a turn stay their own
    float cutter_reel_s{0.0f};
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
    bool arm_lash{false}; // Amygdalan Arm in physical form B, its hook on; right trigger held
    float arm_speed{0.0f}; // raw controller motion relative to the head, with a 0.5 m lever
    std::array<float, 3> arm_direction{}, arm_velocity{}; // tracking axes, metres/s
};
inline SwingInput FreshSwingInput(SwingInput input, std::int64_t now_ns,
                                 std::int64_t max_age_ns) {
    if (input.ns <= 0 || now_ns < input.ns || now_ns - input.ns > max_age_ns) return {};
    if (!std::isfinite(input.speed) || input.speed < 0.0f) input.speed = 0.0f;
    if (!std::isfinite(input.arm_speed) || input.arm_speed < 0.0f) input.arm_speed = 0.0f;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(input.arm_direction[i]) || !std::isfinite(input.arm_velocity[i])) {
            input.arm_speed = 0;
            input.arm_direction = input.arm_velocity = {};
            break;
        }
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
    std::int32_t variation{0};
    bool fast{false};       // same recent-tip-speed gate as the game's chain attack
};
// Attack-window policy only; callers also require the physical whip form. No physics tuning.
inline constexpr bool ContactHeldWhip(std::int32_t variation) {
    return variation == 2200 || variation == 2400; // Threaded Cane, Beast Cutter
}
inline bool WhipContactMotion(const WhipFeel& f, std::int64_t now,
                              std::int32_t variation) {
    return ContactHeldWhip(variation) && f.variation == variation &&
           f.ns > 0 && now >= f.ns && now - f.ns <= 100'000'000 &&
           f.driving && f.asked_out && !f.lifting && f.out > 0.0f && f.fast;
}
void PublishWhipFeel(const WhipFeel& f);
WhipFeel LatestWhipFeel();

} // namespace VR::Whip
