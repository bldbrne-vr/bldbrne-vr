// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The Blade of Mercy and Rakuyo split in two (form B, a blade in each hand; the tester, 2026-11-09:
// "feel good and fast to use, but not overpowered"):
//  - each hand has a combo budget of its own (melee_dual_charges, 2) at the game's own pace (its
//    light swing's recovery, without melee_budget_offset_ms), where before both hands shared the
//    right hand's: a left-right flurry never runs dry, and each hit deals melee_dual_damage_pct
//    (75%) of its damage, so the weapon's damage per second stays near the game's;
//  - rubbing the blades together melee_dual_sharpen_rubs times (4: the blade sounds' strokes, a
//    draw along the other or each reversal of one, either blade on either), each within
//    SharpenGapS of the last, hones both: the full charge's ring (core/vr/charge_audio.h) and a
//    buzz in both hands, and each blade's next hit that lands deals melee_dual_sharpen_pct (30%)
//    more. A hone unused for HonedS fades; rubbing while both are honed only renews it.
// Joined (form A) or another weapon: nothing here applies.
// The ring sends a highlight from guard to tip. The native blade material then stays
// polished until its own hit spends the hone or the timer fades (Shine, LookOf).
//
// Header-only and pure apart from the strokes' counter (the body's blade sounds, any thread) and
// the shine's channels: tests/test_dual_blades.cpp.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include "core/vr/seqlock.h"

namespace VR::DualBlades {

constexpr bool IsDualBlades(std::int32_t variation) {
    return variation == 400 || variation == 2700; // the Blade of Mercy, Rakuyo
}

constexpr float SharpenGapS = 1.5f; // a rub later than this after the last starts the count over
constexpr float HonedS = 10.0f;     // a hone unused this long fades

// A split blade's hit's damage scale (Combat::MeleeWindow::damage_scale): `hands` (a budget of each
// hand's own: melee_dual_hands) x damage_pct, `honed` (this blade) x (1 + sharpen_pct).
constexpr float DamageScale(bool hands, float damage_pct, bool honed, float sharpen_pct) {
    return (hands ? damage_pct * 0.01f : 1.0f) * (honed ? 1.0f + sharpen_pct * 0.01f : 1.0f);
}

// A landed hit's scale (the one its attack took when it started: Combat::MeleeEvent::scale)
// carried a hone: it is the honed blade's (as the game clamps it, 0.25 .. 3, bloodborne_melee.cpp).
// Only such a hit spends the hone; an attack that started before the hone, or a transform's (x1),
// leaves it for the next.
constexpr bool CarriesHone(float hit_scale, bool hands, float damage_pct, float sharpen_pct) {
    const float honed = std::clamp(DamageScale(hands, damage_pct, true, sharpen_pct), 0.25f, 3.0f);
    const float off = hit_scale - honed;
    return off <= 0.001f * honed && off >= -0.001f * honed;
}

// Counts the rubs; a stroke at `t` (s) that completes `rubs` of them hones (the count starts over).
class Sharpener {
public:
    bool Stroke(double t, int rubs) {
        count = count > 0 && t - last <= SharpenGapS ? count + 1 : 1;
        last = t;
        if (count >= (rubs < 1 ? 1 : rubs)) {
            count = 0;
            return true;
        }
        return false;
    }
    void Reset() {
        count = 0;
    }
    int Count() const {
        return count;
    }

private:
    int count{0};
    double last{0.0};
};

// The blade sounds' strokes so far (bloodborne_body.cpp BladeSounds: a scrape, either weapon on
// the other), read by the presenter: only the count, whatever the weapons are.
inline std::atomic<std::uint32_t>& StrokeCounter() {
    static std::atomic<std::uint32_t> strokes{0};
    return strokes;
}
inline void NoteStroke() {
    StrokeCounter().fetch_add(1, std::memory_order_relaxed);
}
inline std::uint32_t Strokes() {
    return StrokeCounter().load(std::memory_order_relaxed);
}

// The hone on the steel. Times are VR::Body::SteadyNowNs.
constexpr float GlintS = 0.42f;     // the band's run from the guard to the tip, with the ring
constexpr float LineInS = 0.30f;    // the sheen coming up behind the band
constexpr float FadeS = 1.0f;       // a hone left unused: its last second fades the sheen out
constexpr float SpentFadeS = 0.15f; // the honed hit landed: the sheen goes this quickly

// Each blade's hone as the presenter has it (0 left, 1 right).
struct Shine {
    std::array<std::int64_t, 2> honed_ns{}; // the ring that honed it (its glint runs then; 0 none)
    std::array<std::int64_t, 2> until_ns{}; // when it fades unused (0: not honed now)
    std::array<std::int64_t, 2> spent_ns{}; // when its honed hit landed (0: none since the ring)
};

// What a blade shows at `now`.
struct Look {
    float line{0.0f};           // the sheen's strength, 0 .. 1
    float glint{-1.0f};         // the band's place along the blade, 0 the guard .. 1 the tip (< 0 none)
    float glint_strength{0.0f}; // 0 .. 1
    bool Any() const {
        return line > 0.0f || glint >= 0.0f;
    }
};

inline Look LookOf(const Shine& s, int hand, std::int64_t now) {
    Look out{};
    if (hand < 0 || hand > 1) {
        return out;
    }
    const std::int64_t honed = s.honed_ns[hand];
    const std::int64_t until = s.until_ns[hand];
    const std::int64_t spent = s.spent_ns[hand];
    if (honed <= 0 || now < honed) {
        return out;
    }
    const float age = static_cast<float>(now - honed) * 1e-9f;
    const bool spent_since = spent >= honed;
    if (age < GlintS && !spent_since) {
        const float t = age / GlintS;
        const float eased = t * t * (3.0f - 2.0f * t);
        out.glint = -0.06f + 1.12f * eased; // starts at the guard, leaves past the tip
        out.glint_strength = std::min(1.0f, 5.0f * t) * std::min(1.0f, 4.0f * (1.0f - t));
    }
    const float rise = std::clamp((age - 0.5f * GlintS) / LineInS, 0.0f, 1.0f);
    if (until > now) {
        const float left = static_cast<float>(until - now) * 1e-9f;
        out.line = rise * std::clamp(left / FadeS, 0.0f, 1.0f);
    } else if (spent_since && now >= spent) {
        const float gone = static_cast<float>(now - spent) * 1e-9f;
        if (gone < SpentFadeS) {
            out.line = rise * (1.0f - gone / SpentFadeS);
        }
    }
    return out;
}

inline SeqLock<Shine>& ShineChannel() {
    static SeqLock<Shine> channel;
    return channel;
}

// Each split blade's steel as a model in its grip dummy's frame (340 the left, 300 the right;
// core/vr/blade_shine_mesh.h, made by bloodborne_weapon_shape.cpp; VR::Assets::GetModel's id, 0:
// none yet) and its line in that frame (the guard end, the tip: the band's way), published by the
// game's tick (bloodborne_melee.cpp PublishDualBlades) for the shine drawn on it.
struct Lines {
    std::array<std::int32_t, 2> model{};
    std::array<std::array<float, 3>, 2> base{};
    std::array<std::array<float, 3>, 2> tip{};
};

inline SeqLock<Lines>& LinesChannel() {
    static SeqLock<Lines> channel;
    return channel;
}

} // namespace VR::DualBlades
