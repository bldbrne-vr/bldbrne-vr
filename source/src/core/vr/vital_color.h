// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/vr/vitals.h"
// The view's colour as combat feedback (vr.ini health_color / stamina_color), the same strengths
// for both eyes (native stereo: native_present.inl's pass; DIBR: the late warp):
//  - health: below 30% HP a dark red creeps in from the edges of the view (full near 5%);
//  - desaturation: out of stamina (10% or less) the view drains of colour and dims (the shaders
//    take 30% of its value off the brightness), deepest while it is empty, easing as it refills,
//    gone once it is past 25% again. Stamina that only runs low, never out, leaves it alone.
//    (2026-10-15, the tester: much more noticeable by default: 0.85 at strength 1 from 0.75, from
//    10% from 5%, the default strength 0.7 from 0.5 (0.9 and 0.8 briefly: too strong).)
namespace VR::VitalColor {
struct Config {
    bool health{}, stamina{};
    float health_gain{.5f}, stamina_gain{.5f};
};
struct Output {
    float health{}, desaturation{};
};
constexpr float MaxHealth = .45f;        // the red at the edges at health_color_strength 1
constexpr float MaxDesaturation = .85f;  // grey at stamina_color_strength 1 (never all grey)
constexpr float OutOfStamina = .10f;     // at or below this share: out
constexpr float RecoveredStamina = .25f; // above it: recovered
class State {
public:
    Output Update(const Vitals::Status& s, const Config& c, bool gameplay, std::int64_t now,
                  float dt) {
        dt = std::isfinite(dt) ? std::clamp(dt, 0.f, .1f) : 0.f;
        if (!c.health && !c.stamina) {
            *this = {};
            return {};
        }
        const bool live = gameplay && s.ok && s.hp > 0 && s.max_hp > 0 && s.max_stamina > 0 &&
                          now >= s.steady_ns && now - s.steady_ns < 500'000'000;
        if (!live) {
            *this = {};
            return {};
        }
        const float hp = Vitals::Fraction(s.hp, s.max_hp),
                    stamina = Vitals::Fraction(s.stamina, s.max_stamina);
        const float danger = std::clamp((.30f - hp) / .25f, 0.f, 1.f);
        const float target = c.health ? MaxHealth * Gain(c.health_gain) * danger : 0;
        health_ += (target - health_) * (1 - std::exp(-dt / (target > health_ ? .2f : .45f)));
        if (!c.health)
            health_ = 0;
        if (c.stamina && stamina <= OutOfStamina)
            exhausted_ = true;
        if (!c.stamina || stamina > RecoveredStamina)
            exhausted_ = false;
        // 60% of the full strength just before recovering, all of it while empty.
        const float depth =
            .6f + .4f * std::clamp((RecoveredStamina - stamina) / RecoveredStamina, 0.f, 1.f);
        const float exhaustion =
            exhausted_ ? MaxDesaturation * Gain(c.stamina_gain) * depth : 0.f;
        desat_ += (exhaustion - desat_) * (1 - std::exp(-dt / (exhaustion > desat_ ? .08f : .3f)));
        if (!c.stamina)
            desat_ = 0;
        // Keep health red legible while both conditions are active.
        return {health_, desat_ * (1 - std::clamp(health_ / .2f, 0.f, 1.f) * .6f)};
    }

private:
    static float Gain(float value) {
        return std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.f;
    }
    float health_{}, desat_{};
    bool exhausted_{};
};
} // namespace VR::VitalColor
