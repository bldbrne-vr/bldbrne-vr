// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "core/vr/anim_classes.h"
#include "core/vr/daze.h"

// Hit-stun vignette (vr.ini hitstun_vignette, 2026-10-08): while a blow's hit reaction holds the
// hunter on his feet and the game would refuse a swing ("soft-staggered": a swing now does no
// damage), the red hit vignette stays up; the moment the game takes an attack again it is gone
// within FadeOutS, so the clearing red is the "you can swing again" cue. The game's own behaviour
// is unchanged: this only shows it.
//
// The state (StunClip): the arm gate's held damage clip of the on-feet reactions (a000_020000 ..
// 020399, the clips arm_keep_on_hit keeps) before its right-hand attack window (R1 / R2 taken
// again: GateOut::cancelled[Right]). That is exactly when the physical melee refuses a swing:
// "the game takes no attack yet" for a kept hit (GateOut::attack_ok follows cancelled; the melee's
// AttackWaitWhy, bloodborne_melee.cpp BlockReason) or "the game has the right arm" with
// arm_keep_on_hit=0. In the tester's playtest of 2026-10-08 (usertest_hold10) 5 of the 8 refused
// swings were that refusal, each inside such a clip (a003_020100 / 020110 / 020120, a000_020103:
// window at 183 ms; a000_020201: 580 ms). A swing made in the last swing_buffer_ms of it is held
// and goes out at the window (the red clears at that moment). A knockdown, launch or get-up
// (20400 .. 23999: the hunter on the floor), a dodge, a guard reaction, a grab, a visceral, death,
// an event or a ladder never counts (the knockdown keeps the black stagger vignette).
//
// The look: the red hit flash's own ring (hit_vignette_color, its profile, fitted, the blow's
// direction wedge) held at that blow's flash peak (70 .. 100% of hit_vignette_strength by the HP
// it took) times hitstun_vignette_strength (Red); in over FadeInS (the flash's own rise), out over
// FadeOutS; the flash's own fade-out (hit_vignette_fade_ms, 500) is cut to FadeOutS while the hold
// shows (FlashFadeS), so the red does not linger after the window. A hit that does not stun
// flashes as before. Needs hit_vignette=1 (it is the hit flash, held).
//
// With the black stagger vignette (core/vr/daze.h; BlackLevel):
//  - hitstun_vignette=1 (the default): a light stagger (20000 .. 20199: the small flinches, their
//    window 0.18 s after stagger_shorten_ms) shows the red alone (the black tunnel is not drawn
//    for it); a heavy stagger (20200 .. 20399: 0.58 s) keeps the black tunnel ("dizzy") with the
//    red over it as the black allows (stagger_vignette_red: a thin rim at 35%), but held until the
//    window instead of fading within 220 ms; a knockdown keeps the black as before.
//  - hitstun_vignette=2: every on-feet stagger shows the red alone (the black only for knockdowns).
//  - hitstun_vignette=0: as before (the black tunnel for every stagger, the red flash brief).
//
// Pure (no emulator state): core/vr/bloodborne_body.cpp publishes StunClip with the body status,
// the OpenXR presenter steps the Hold; tests/test_hitstun.cpp drives both.
namespace VR::HitStun {

// GateOut's arrays are C0000::Left, C0000::Right.
constexpr std::size_t RightHand = 1;
// The hold comes up as fast as the flash (its HitFadeIn) and goes out quickly at the window.
constexpr float FadeInS = 0.06f;
constexpr float FadeOutS = 0.12f;
// A stun without a hit counted this recently (a blow that took no HP) holds the plain ring at the
// flash's lowest peak instead of an old blow's direction.
constexpr float HitRecentS = 1.0f;

enum class Mode : std::uint8_t { Off = 0, Light = 1, OnFeet = 2 };

constexpr std::int32_t BaseOf(std::int32_t id) {
    return id < 0 ? -1 : id % 1'000'000;
}
// The on-feet hit reactions (the kept hit clips: KeepKind::Hit).
constexpr bool IsOnFeetHit(std::int32_t id) {
    const std::int32_t base = BaseOf(id);
    return base >= 20000 && base <= 20399;
}
// The light ones (the small flinches 200x0 and 201xx); 202xx and 203xx are the heavy staggers.
constexpr bool IsLightHit(std::int32_t id) {
    const std::int32_t base = BaseOf(id);
    return base >= 20000 && base <= 20199;
}

// The hit-stun's clip (its TimeAct ring id) for this pose copy's arm gate, -1 = none.
constexpr std::int32_t StunClip(const VR::Body::GateOut& g) {
    if (!g.held || g.reason != VR::Body::GateReason::Anim ||
        g.owner != VR::Body::AnimClass::Damage || g.cancelled[RightHand] ||
        !IsOnFeetHit(g.owner_id)) {
        return -1;
    }
    return g.owner_id;
}

constexpr Mode ModeOf(int setting) {
    return setting == 1 ? Mode::Light : setting == 2 ? Mode::OnFeet : Mode::Off;
}

// The red shows alone for this stun clip (the black stagger tunnel steps aside).
constexpr bool RedAlone(Mode mode, std::int32_t stun_clip) {
    return mode == Mode::OnFeet  ? IsOnFeetHit(stun_clip)
           : mode == Mode::Light ? IsLightHit(stun_clip)
                                 : false;
}

// The black stagger vignette's level with the hit-stun vignette (`level`: core/vr/daze.h LevelOf
// of the same gate, `stun_clip`: StunClip of it). A Stagger implies a stun (both before the
// window), so the view is never left without either.
constexpr VR::Daze::Level BlackLevel(VR::Daze::Level level, Mode mode, std::int32_t stun_clip) {
    return level == VR::Daze::Level::Stagger && RedAlone(mode, stun_clip) ? VR::Daze::Level::None
                                                                          : level;
}

// The red's opacity factor of hit_vignette_strength: the flash (0 .. 1, its peak in it) or the
// hold (its eased level x the blow's peak x hitstun_vignette_strength), the larger.
inline float Red(float flash, float hold, float peak, float strength) {
    return std::max(std::max(flash, 0.0f), std::clamp(hold, 0.0f, 1.0f) *
                                               std::clamp(peak, 0.0f, 1.0f) *
                                               std::clamp(strength, 0.0f, 1.5f));
}

// The flash's fade-out (seconds) while the hold shows: at most FadeOutS.
inline float FlashFadeS(float fade_s, bool hold_shown) {
    return hold_shown ? std::min(fade_s, FadeOutS) : fade_s;
}

// The presenter's hold (every XR frame): Step returns its eased level, 0 .. 1.
class Hold {
public:
    // `stun`: a fresh body status with a stun clip the red takes; `reset`: death or the setting
    // off (to 0 at once).
    float Step(bool stun, bool reset, float dt) {
        dt = std::clamp(dt, 0.0f, 0.1f);
        if (reset) {
            level_ = 0.0f;
            on_ = false;
            since_ = 0.0f;
            return 0.0f;
        }
        if (stun) {
            if (!on_) {
                since_ = 0.0f;
            }
            on_ = true;
            since_ += dt;
            level_ = std::min(level_ + dt / FadeInS, 1.0f);
        } else {
            on_ = false;
            level_ = std::max(level_ - dt / FadeOutS, 0.0f);
        }
        return level_ * level_ * (3.0f - 2.0f * level_); // smoothstep
    }
    // The stun holds now.
    bool On() const {
        return on_;
    }
    // It holds or still fades out.
    bool Shown() const {
        return on_ || level_ > 0.0f;
    }
    // Seconds the current (or the last) stun has held.
    float Since() const {
        return since_;
    }
    float Linear() const {
        return level_;
    }

private:
    float level_{0.0f}; // 0 .. 1, linear
    bool on_{false};
    float since_{0.0f};
};

} // namespace VR::HitStun
