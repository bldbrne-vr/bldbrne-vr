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

#include "core/vr/daze.h"

// The red hit vignette's ring (vr.ini hit_vignette_inner, 2026-10-09): ONE shape for the three ways
// the red shows - the hit flash (hit_vignette), its wedge toward the blow (hit_vignette_directional,
// core/vr/hit_direction.h) and the hit-stun hold (hitstun_vignette, core/vr/hit_stun.h) - all drawn
// by the presenter's one AppendVignette call from RingFor below.
//
// Fitted (hit_vignette_fit=1, the default; vr_overlay.frag's fitted profile): r = 1 at the edge of
// what the headset shows in every direction (per-axis angles over that side's half-angle); clear
// inside `inner` (a fraction of the displayed field from the centre), a smoothstep up to the full
// hit_vignette_strength at r = 1 (and beyond it, the corners), to the power `power` (only the very
// edges). The tester, 2026-10-09: "make the vignette a little smaller (it covers too much of the
// screen) at least the red vignette (attack)". Until then the inner edge was OldInner (26 / 56 =
// 0.464: the old 26 .. 56 degree profile as fractions); DefaultInner moves it out a little (the
// user, ~01:10: "do not make much softer, just a little bit": 0.53), so the red covers a bit less
// of the view while the visible edge keeps its full strength (a blow still reads in the
// periphery); then 0.58 (2026-10-09 morning: "I was wondering if the red vignette can be
// made a tiny bit smaller"): about a tenth less of the view than 0.53, a fifth less than the old
// ring. The black stagger tunnel, the heal glow and the low-HP edge are not this ring.
//
// Over the stagger's black (stagger_vignette_red < 1) the inner edge moves further out toward
// VR::Daze::RedFitInner by the tunnel's share of its full darkness (Daze::RedInner, from this base;
// never inward), the power up to Daze::RedPowerScale times, as before.
// Not fitted (hit_vignette_fit=0): the old degree ring (clear inside 26, full from 56 degrees off the
// view axis); a larger `inner` moves its clear inside out to inner x 56 degrees.
namespace VR::HitVig {

constexpr float OldInner = 26.0f / 56.0f; // the ring until 2026-10-09
constexpr float DefaultInner = 0.58f; // 0.53 until the 2026-10-09 morning
constexpr float MinInner = 0.3f;
constexpr float MaxInner = 0.95f;
// The ring's profile power: the plain ring cubed; the wedge squared (it covers a third of the ring
// at most, and the top edge, ahead, is the least visible part of the view). Fitted, both double
// (the fitted smoothstep is not squared like the degree ring's).
constexpr float ProfilePower = 3.0f;
constexpr float ProfilePowerDirectional = 2.0f;
// The degree ring (VrOverlay::VignetteInnerDeg / VignetteFullDeg).
constexpr float DegInner = 26.0f;
constexpr float DegFull = 56.0f;

struct Ring {
    float fit_inner{0.0f}; // > 0: fitted, clear inside this fraction of the displayed field
    float fit_full{1.0f};  // fitted: full from this fraction
    float inner_deg{0.0f}; // not fitted: > 0 its clear inside (degrees), 0 = the mesh's own ring
    float full_deg{0.0f};
    float power{1.0f}; // the profile's power
};

// The ring for `inner` (hit_vignette_inner), `fit` (hit_vignette_fit), `wedge` (the directional
// flash) over a stagger tunnel at `daze_share` of its full darkness (`red`: stagger_vignette_red).
inline Ring RingFor(float inner, bool fit, bool wedge, float daze_share, float red) {
    inner = std::clamp(inner, MinInner, MaxInner);
    Ring ring{};
    ring.power = (wedge ? ProfilePowerDirectional : ProfilePower) * (fit ? 2.0f : 1.0f) *
                 VR::Daze::RedPower(daze_share, red);
    if (fit) {
        ring.fit_inner = VR::Daze::RedInner(daze_share, red, inner);
        ring.fit_full = 1.0f;
    } else if (inner * DegFull > DegInner + 0.01f) {
        ring.inner_deg = inner * DegFull;
        ring.full_deg = DegFull;
    }
    return ring;
}

// The fitted ring's opacity factor (of its edge opacity) at `r` (1 = the displayed edge), before the
// wedge: the shader's smoothstep and power.
inline float FitProfile(float r, const Ring& ring) {
    const float full = std::max(ring.fit_full, ring.fit_inner + 0.01f);
    const float t = std::clamp((r - ring.fit_inner) / (full - ring.fit_inner), 0.0f, 1.0f);
    return std::pow(t * t * (3.0f - 2.0f * t), std::max(ring.power, 0.01f));
}

} // namespace VR::HitVig
