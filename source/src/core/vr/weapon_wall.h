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

#include "core/vr/arm_ik.h"
#include "core/vr/seqlock.h"

// The weapon against the walls (vr.ini weapon_wall; 2026-10-12, not yet tried in the game): how a
// contact feels. The weapons are solid (core/vr/solid_weapon.h, on the body's pose copy): each
// update the contact a weapon makes (with the walls, or the two weapons with each other) and the
// controller's motion there go through Contact: a weapon driven into a surface strikes it (a thud
// by the speed, a little spray of sparks); one sliding along it scrapes (a grainy buzz rising with
// the speed, sparks as it goes, core/vr/sparks.h); one resting against it is silent. The
// presenter plays the feel and draws the sparks from State.
//
// Pure (tests/test_weapon_wall.cpp). Metres.
namespace VR::WeaponWall {

using Body::Vec3;

constexpr float StrikeSpeed = 1.2f;     // m/s into the surface as it touches: a strike
constexpr float ScrapeSpeed = 0.15f;     // m/s along it while touching: a scrape ...
constexpr float ScrapeFullSpeed = 2.5f; // ... its feel full at this
constexpr float GapS = 0.06f;           // a contact lost this briefly is the same contact
constexpr float CinderS = 0.08f;        // a cinder this often while scraping
constexpr float StrikeRepeatS = 0.25f;  // no second strike sooner (a blade rattling on a wall)
constexpr float GlitchSpeed = 40.0f;    // a blade "moving" faster: a jump (a recenter), nothing
                                        // (the hardest swing's tip stays well under it)

// One update of the blade: whether the cast touched the map, the surface's normal there (out of
// it, unit) and the blade's own velocity at that point (m/s).
struct Sample {
    float dt{0.0f};
    bool contact{false};
    Vec3 normal{};
    Vec3 v{};
};

struct Out {
    bool strike{false};       // driven into the surface: the thud and a burst of sparks
    float strike_speed{0.0f}; // m/s into it
    bool scraping{false};     // sliding along it
    float scrape_speed{0.0f}; // m/s along it
    bool cinder{false};       // a cinder now (the first as the scrape starts, then every CinderS)
};

// 0 .. 1: how strong a scrape feels at `speed` (m/s along the surface).
constexpr float ScrapeLevel(float speed) {
    const float k = (speed - ScrapeSpeed) / (ScrapeFullSpeed - ScrapeSpeed);
    return k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
}

class Contact {
public:
    Out Step(const Sample& s) {
        Out out{};
        since_strike += s.dt;
        if (!s.contact || !(Body::Length(s.v) <= GlitchSpeed)) {
            // No contact (or a jump): one this brief is the same contact, its scrape going on.
            gap += s.dt;
            if (gap > GapS) {
                touching = false;
                scraping = false;
            }
            out.scraping = scraping;
            out.scrape_speed = scraping ? scrape_speed : 0.0f;
            return out;
        }
        gap = 0.0f;
        const bool fresh = !touching;
        touching = true;
        const float into = -Body::Dot(s.v, s.normal);
        const float along = Body::Length(s.v + s.normal * into);
        if (fresh && into >= StrikeSpeed && since_strike >= StrikeRepeatS) {
            out.strike = true;
            out.strike_speed = into;
            since_strike = 0.0f;
        }
        if (along >= ScrapeSpeed) {
            out.scraping = true;
            out.scrape_speed = along;
            cinder_s = scraping ? cinder_s + s.dt : CinderS; // its first cinder at once
            if (cinder_s >= CinderS) {
                cinder_s = std::fmod(cinder_s, CinderS);
                out.cinder = true;
            }
        }
        scraping = out.scraping;
        scrape_speed = out.scrape_speed;
        return out;
    }

private:
    bool touching{false};
    bool scraping{false};
    float scrape_speed{0.0f};
    float gap{0.0f};
    float since_strike{10.0f};
    float cinder_s{0.0f};
};

// One weapon against the walls, or the two against each other: its strikes and its scrape, and
// where (tracking space, metres: the sparks).
struct Touching {
    std::uint32_t strike_serial{0}; // strikes so far
    float strike_speed{0.0f};       // the newest one's (m/s into the surface)
    bool scraping{false};           // this update
    float scrape_speed{0.0f};       // m/s along the surface
    bool steel{false};              // the weapon touches (sparks), not the bare hand
    float point[3]{};               // where it touches ...
    float normal[3]{};              // ... the surface's normal there (unit, out of it)
    float slide[3]{};               // ... and the contact's motion along it (m/s)
};

// Game -> presenter: the body's pose copy writes it every update with the weapons solid
// (core/vr/solid_weapon.h; bloodborne_body.cpp SolidWeapons).
struct State {
    std::array<Touching, 2> hand{}; // 0 left, 1 right: each weapon against the walls
    Touching clash{};               // the two weapons against each other
    std::int64_t ns{0};             // written at (steady clock)
};

inline SeqLock<State>& Channel() {
    static SeqLock<State> channel;
    return channel;
}

} // namespace VR::WeaponWall
