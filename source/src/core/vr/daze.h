// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cstdint>

#include "core/vr/anim_classes.h"

// Stagger vignette (vr.ini stagger_vignette): while a blow's stagger or a knockdown holds the
// hunter, the edges of the view darken ("dizzy, incapacitated"): in over FadeInS at the stagger's
// start, held, out over FadeOutS the moment the game's attack window opens (R1 or L2 taken again,
// or a move cancels the clip), so the clearing view is also the "you can act again" cue.
//  - a stagger (a000_020000 .. 020399, the on-feet reactions; the kept hit clips): StaggerAlpha at
//    the edge, clear inside StaggerInnerDeg;
//  - a knockdown, launch or get-up (20400 .. 23999: the hunter on the floor): KnockdownAlpha,
//    clear inside KnockdownInnerDeg (a little narrower);
// both times vr.ini stagger_vignette_strength (at most MaxAlpha: the edges never go opaque). Never
// for a dodge, a guard reaction, a grab or a visceral; death and loads end it at once. The
// presenter draws it under the red hit flash (the blow's direction still reads), which yields to
// it (RedScale below: a black tunnel, the blow's side a brief red rim), in gameplay only.
//
// A flurry (vr.ini stagger_vignette_merge_ms, 400): a clearing fades only down to MergeFloor at
// once (the "you can act" cue: about four fifths of the darkness gone in 0.1 s) and holds that
// faint rest until merge_ms after the clear, then fades it out (45 ms). A blow that staggers again
// inside that time fades in from the rest, so a flurry of blows reads as one continuous daze
// instead of the view flashing clear and dark (against the scourge beast, run da_c: 7 of 43
// re-staggers came 0-192 ms after a clear, most others 0.4-0.75 s). 0 = the full fade-out at once
// (as before).
//
// Fitted to what the headset shows (vr.ini stagger_vignette_fit, 1 since 2026-10-06): the profile
// above is in degrees off the view axis and full only from 52-58 degrees, mostly beyond a Quest
// 3's view (it shows about 54 degrees out, 44 up and 55 down; the game's render is wider for the
// late warp), so the tester saw at most a faint shade (0.17 at the top edge, 0.05 at the nasal
// edge). Fitted, the presenter draws it against the eyes' displayed field (the runtime's located
// FOV, the union of both eyes so both see the same darkness in every direction): r = 1 at the
// visible edge in every direction, clear inside FitStaggerInner (FitKnockdownInner), full from
// FitStaggerFull (FitKnockdownFull): the dark band covers the outer ~30% of what you see, at
// FitStaggerAlpha (0.75) at the visible edge for a stagger and FitKnockdownAlpha (0.9) for a
// knockdown. A fresh blow's first moment is darker (the onset dip, vr.ini stagger_vignette_dip_ms,
// 120): DipBoost more for dip_ms, then easing back over DipEaseS, so it reads as being dazed.
//
// Pure (no emulator state): core/vr/bloodborne_body.cpp publishes the level from the arm gate,
// the OpenXR presenter steps the Fader; tests/test_body_solver.cpp drives both.
namespace VR::Daze {

enum class Level : std::uint8_t { None, Stagger, Knockdown };

constexpr float StaggerAlpha = 0.55f;
constexpr float KnockdownAlpha = 0.75f;
constexpr float MaxAlpha = 0.9f;
constexpr float FadeInS = 0.09f;
constexpr float FadeOutS = 0.15f;
// The faint rest a clearing holds through stagger_vignette_merge_ms (linear level: smoothstep
// 0.216 of the edge opacity, 0.12 at a stagger's edge).
constexpr float MergeFloor = 0.3f;
// The profile (degrees off the view axis): clear inside inner, full from full.
constexpr float StaggerInnerDeg = 30.0f;
constexpr float StaggerFullDeg = 58.0f;
constexpr float KnockdownInnerDeg = 27.0f;
constexpr float KnockdownFullDeg = 52.0f;
// Fitted (stagger_vignette_fit): fractions of the displayed field (1 = its edge in that direction).
constexpr float FitStaggerAlpha = 0.75f;
constexpr float FitKnockdownAlpha = 0.9f;
constexpr float FitMaxAlpha = 0.95f;
constexpr float FitStaggerInner = 0.62f;
constexpr float FitStaggerFull = 1.0f;
constexpr float FitKnockdownInner = 0.6f;
constexpr float FitKnockdownFull = 0.92f;
// The onset dip: this much darker (a factor on the edge opacity) for dip_ms, easing back over
// DipEaseS.
constexpr float DipBoost = 0.25f;
constexpr float DipEaseS = 0.15f;

inline const char* LevelName(Level l) {
    switch (l) {
    case Level::None:
        return "none";
    case Level::Stagger:
        return "stagger";
    case Level::Knockdown:
        return "knockdown";
    }
    return "?";
}

// The level for this pose copy's arm gate: a held damage clip (the gate's own reason, not a
// death, grab, event or ladder) before its attack window opened (either hand's attack taken
// again: GateOut::cancelled); a move cancel or the clip's end released it already (held false).
constexpr Level LevelOf(const VR::Body::GateOut& g) {
    if (!g.held || g.reason != VR::Body::GateReason::Anim ||
        g.owner != VR::Body::AnimClass::Damage || g.cancelled[0] || g.cancelled[1]) {
        return Level::None;
    }
    const std::int32_t base = g.owner_id % 1'000'000;
    return base >= 20000 && base <= 20399 ? Level::Stagger : Level::Knockdown;
}

constexpr float EdgeAlpha(Level l, bool fit = false) {
    if (fit) {
        return l == Level::Stagger     ? FitStaggerAlpha
               : l == Level::Knockdown ? FitKnockdownAlpha
                                       : 0.0f;
    }
    return l == Level::Stagger ? StaggerAlpha : l == Level::Knockdown ? KnockdownAlpha : 0.0f;
}

// The onset dip's weight `since_s` after a blow's stagger began: 1 through dip_s, easing to 0
// over DipEaseS (0 with dip_s <= 0).
inline float DipWeight(float since_s, float dip_s) {
    if (!(dip_s > 0.0f) || since_s < 0.0f) {
        return 0.0f;
    }
    if (since_s <= dip_s) {
        return 1.0f;
    }
    const float t = std::clamp((since_s - dip_s) / DipEaseS, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

// The red hit flash over the stagger tunnel (vr.ini stagger_vignette_red, 0.35 since 2026-10-07:
// with both fitted, the red flash drawn over the black band made the tunnel read RED, and the tester
// asked for a black daze). While the stagger vignette shows, the flash drawn over it keeps `red`
// of its opacity at the tunnel's full darkness (by the darkness's share of its full edge opacity,
// so a blow's first moment, while the black still fades in over FadeInS, keeps most of its red:
// the blow's side reads as a brief rim), is drawn thinner (its profile power up to RedPowerScale
// times, and fitted, its clear inside moved out toward RedFitInner of the displayed field: a rim
// in the outer ~5% of the view, the black band inside it) and fades out over at most RedFadeS.
// 1 = the flash as before. (The first mock run with the power alone, 2026-10-07: the red at a
// third of its old strength still covered the whole band at +100 ms and read dark red.)
constexpr float RedPowerScale = 1.5f;
constexpr float RedFitInner = 0.8f;
constexpr float RedFadeS = 0.22f;

// The stagger vignette's share of its full darkness (0 .. 1): `drawn` the Fader's opacity this
// frame, `shown` its level, `fit` / `strength` stagger_vignette_fit / _strength (the onset dip
// counts as full).
inline float Share(float drawn, Level shown, bool fit, float strength) {
    const float full = EdgeAlpha(shown, fit) * std::max(strength, 0.0f);
    return full > 0.0f && drawn > 0.0f ? std::clamp(drawn / full, 0.0f, 1.0f) : 0.0f;
}

// The red flash's opacity factor and profile-power factor over a tunnel at `share` of its full
// darkness (`red`: stagger_vignette_red; 1 = unchanged).
inline float RedScale(float share, float red) {
    return 1.0f - (1.0f - std::clamp(red, 0.0f, 1.0f)) * std::clamp(share, 0.0f, 1.0f);
}
inline float RedPower(float share, float red) {
    return red >= 1.0f ? 1.0f : 1.0f + (RedPowerScale - 1.0f) * std::clamp(share, 0.0f, 1.0f);
}
// The fitted flash's clear inside (`inner`, a fraction of the displayed field; 0 = not fitted,
// unchanged) over a tunnel at `share`: toward RedFitInner, never inward.
inline float RedInner(float share, float red, float inner) {
    if (red >= 1.0f || !(inner > 0.0f) || inner >= RedFitInner) {
        return inner;
    }
    return inner + (RedFitInner - inner) * std::clamp(share, 0.0f, 1.0f);
}

// The presenter's fade (every XR frame): Step returns the edge opacity to draw.
class Fader {
public:
    // `level`: the body's newest level (None when its status is stale); `reset`: death, a load or
    // the setting off (to 0 at once); `strength`: stagger_vignette_strength; `merge_s`:
    // stagger_vignette_merge_ms (the faint rest's hold after a clear, 0 = none); `fit`:
    // stagger_vignette_fit (the fitted opacities, FitStaggerAlpha .. FitMaxAlpha, and the onset
    // dip over `dip_s`, stagger_vignette_dip_ms; the old opacities and no dip without it).
    float Step(Level level, bool reset, float dt, float strength, float merge_s = 0.0f,
               bool fit = false, float dip_s = 0.0f) {
        dt = std::clamp(dt, 0.0f, 0.1f);
        if (reset) {
            level_ = 0.0f;
            shown_ = Level::None;
            since_clear_ = 0.0f;
            since_start_ = 1e9f;
            merged_ = false;
            return 0.0f;
        }
        since_start_ = std::min(since_start_ + dt, 1e9f);
        if (level != Level::None) {
            if (shown_ == Level::None || since_clear_ > 0.0f) {
                // A stagger begins: a fresh one from clear, or one inside the last one's rest.
                merged_ = level_ > 0.0f;
                since_start_ = 0.0f;
            }
            if (level != shown_ && level_ > 0.0f && shown_ != Level::None) {
                // A stagger turning into a knockdown: from the opacity drawn now.
                level_ = std::min(level_ * EdgeAlpha(shown_, fit) / EdgeAlpha(level, fit), 1.0f);
                since_start_ = 0.0f;
            }
            shown_ = level;
            since_clear_ = 0.0f;
            level_ = std::min(level_ + dt / FadeInS, 1.0f);
        } else if (shown_ != Level::None) {
            since_clear_ += dt;
            // Down to the rest at once, the rest after merge_s (never up: a short stagger's
            // lower level holds as it is).
            const float rest = since_clear_ < merge_s ? std::min(MergeFloor, level_) : 0.0f;
            level_ = std::max(level_ - dt / FadeOutS, rest);
            if (level_ <= 0.0f) {
                shown_ = Level::None;
                since_clear_ = 0.0f;
            }
        }
        const float s = level_ * level_ * (3.0f - 2.0f * level_); // smoothstep
        if (fit) {
            const float dip = 1.0f + DipBoost * DipWeight(since_start_, dip_s);
            return std::min(s * EdgeAlpha(shown_, true) * dip * std::max(strength, 0.0f),
                            FitMaxAlpha);
        }
        return std::min(s * EdgeAlpha(shown_) * std::max(strength, 0.0f), MaxAlpha);
    }
    // Seconds since the shown stagger began (a fresh one, one inside the last one's rest, or a
    // stagger turning into a knockdown).
    float SinceStart() const {
        return since_start_;
    }
    // The level whose profile is drawn (the last one while it fades out).
    Level Shown() const {
        return shown_;
    }
    float Linear() const {
        return level_;
    }
    // The current stagger began inside the last one's fade or rest (no clear view between them).
    bool Merged() const {
        return merged_;
    }

private:
    float level_{0.0f}; // 0 .. 1, linear
    Level shown_{Level::None};
    float since_clear_{0.0f}; // seconds since the level went None (while it still shows)
    float since_start_{1e9f}; // seconds since the shown stagger began (the onset dip)
    bool merged_{false};
};

} // namespace VR::Daze
