// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <ostream>
#include <sstream>
#include <string>

#include "core/vr/arm_ik.h"

// Blade sounds (vr.ini blade_sounds; the tester's 2026-10-13 "when we clang our weapons together ...
// a sword sharpening sound"): your two weapons against each other ring like steel on steel: the
// blade and the gun, or the two blades of the Blade of Mercy / Rakuyo. The weapons need not be
// solid (weapon_wall): with it off they pass through each other and the body's pose copy still
// hears them meet (bloodborne_body.cpp BladeSounds). How a meeting sounds:
//  - a touch that comes in (into the other blade, no slide): a tap (a clink of steel);
//  - a blade drawn along the other: a scrape (a sharpening stroke), once it has slid StrokeStartM
//    (no sound for a blade resting or jiggling on the other); a hit that glances off along the
//    blade is a scrape at once;
//  - back and forth (a whetstone's rhythm): each reversal is a new stroke; so is a draw again after
//    a pause; one draw on and on is one stroke.
// Two kinds only, told apart by the motion the body already has (a few dot products an update).
// Each sound is one of its kind's pool, never the same twice in a row (and the game's events
// themselves pick among their own recordings: c935 among four). The sounds are the game's own
// (SoundType 1 events in sprj_main), played where the blades meet: the mix turns with your head
// and goes through audio_binaural.
//
// Pure (tests/test_blade_sounds.cpp). Metres, seconds.
namespace VR::BladeSound {

using Body::Vec3;

// m/s into the other blade as they meet: a tap (0.35 until 2026-10-15: a brush rang "CLAAANG").
constexpr float TapSpeed = 0.6f;
// A tap comes in head-on: at least this much into the blade for each m/s along it (1: 45 degrees
// or steeper). A blade brushing along the other at an angle is a slide (the grind, a scrape).
constexpr float TapIntoPerAlong = 1.0f;
constexpr float SlideSpeed = 0.2f;   // m/s along it: sliding
constexpr float StrokeStartM = 0.025f; // slid this far one way: a stroke sounds
constexpr float GlanceSpeed = 1.2f;  // m/s along it as they meet (faster than into): a scrape
constexpr float GapS = 0.08f;        // contact lost this briefly: the same contact
constexpr float PauseS = 0.15f;      // a slide stopped this long: the stroke ended
constexpr float MinGapS = 0.12f;     // never two sounds closer than this
constexpr float StrokeRepeatS = 0.25f; // a new stroke no sooner after the last one's sound
constexpr float ReverseDot = -0.3f;  // the slide turned this far back: a new stroke
// One draw on and on rings again (the strokes' rings are ~0.4 s loud: a long scrape heard as "just
// a little", 2026-10-13): this often, once it slid this much more.
constexpr float ScrapeRepeatS = 0.35f;
constexpr float ScrapeRepeatM = 0.10f;
// Touching still (solid weapons rest against each other after a tap), a new blow tapping in: it
// came in this fast after hardly moving in (2026-10-13: a second tap was silent "for a while").
constexpr float RestrikeFromSpeed = 0.15f;
constexpr float GlitchSpeed = 40.0f; // a blade "moving" faster: a jump (a recenter), nothing
// The grind (vr.ini blade_grind, core/vr/grind_audio.h): a blade sliding along the other grinds on
// at this level, silent from GrindFromSpeed, full at GrindFullSpeed (the auditions' curve, the
// user's 2026-10-14 pick). With it the strokes ring once each (no ScrapeRepeatS re-ring).
constexpr float GrindFromSpeed = 0.12f;
constexpr float GrindFullSpeed = 1.32f;

inline float GrindLevel(float along) {
    const float u = std::clamp((along - GrindFromSpeed) / (GrindFullSpeed - GrindFromSpeed), 0.0f,
                               1.0f);
    return u > 0.0f ? std::pow(u, 0.8f) : 0.0f;
}

// A pool of the game's SoundType 1 ids (at most Max; vr.ini: comma separated).
struct Pool {
    static constexpr int Max = 6;
    std::array<std::int32_t, Max> id{};
    int n{0};

    static Pool Of(std::initializer_list<std::int32_t> ids) {
        Pool p{};
        for (const std::int32_t i : ids) {
            if (p.n < Max) {
                p.id[static_cast<size_t>(p.n++)] = i;
            }
        }
        return p;
    }
};

// "935, 9044441" -> the pool; false (the pool unchanged) when nothing in it is an id.
inline bool ParsePool(const std::string& value, Pool& out) {
    Pool p{};
    std::stringstream in(value);
    std::string item;
    while (std::getline(in, item, ',') && p.n < Pool::Max) {
        const size_t a = item.find_first_not_of(" \tc");
        if (a == std::string::npos) {
            continue;
        }
        try {
            size_t used = 0;
            const long long v = std::stoll(item.substr(a), &used);
            if (v >= 0 && v <= 999'999'999) {
                p.id[static_cast<size_t>(p.n++)] = static_cast<std::int32_t>(v);
            }
        } catch (...) {
        }
    }
    if (p.n == 0) {
        return false;
    }
    out = p;
    return true;
}

inline std::string PoolText(const Pool& p) {
    std::string s;
    for (int i = 0; i < p.n; ++i) {
        s += (i ? "," : "") + std::to_string(p.id[static_cast<size_t>(i)]);
    }
    return s;
}

struct Settings {
    bool on{true};
    // sprj_main (the sample names of their recordings; chosen 2026-10-13):
    Pool tap{Pool::Of({9044441, 935})}; // sword-x-sword1; iron-cut-iron x3 + sword-x-sword1
    // the katana's draw from its sheath (1.5 s, 3 s), the Blade of Mercy's deploy (0.8 s)
    Pool scrape{Pool::Of({10601, 10600, 6103})}; // sprj_katana_iai1, sprj_katana_iai0,
                                                 // souken_tenkai
    int grind{1};             // 0 off, 1 the sparks loop as it is ("A"), 2 lower, heavier ("B")
    float grind_volume{1.0f}; // 0..2
};

inline Settings& GetSettings() {
    static Settings settings;
    return settings;
}

inline bool ParseBladeSoundSetting(const std::string& key, const std::string& value) {
    auto& s = GetSettings();
    if (key == "blade_sounds") {
        s.on = value == "1" || value == "true";
    } else if (key == "blade_sound_tap") {
        ParsePool(value, s.tap);
    } else if (key == "blade_sound_scrape") {
        ParsePool(value, s.scrape);
    } else if (key == "blade_grind") {
        if (value == "0" || value == "1" || value == "2") {
            s.grind = value[0] - '0';
        }
    } else if (key == "blade_grind_volume") {
        try {
            const float v = std::stof(value);
            if (std::isfinite(v)) {
                s.grind_volume = std::clamp(v, 0.0f, 2.0f);
            }
        } catch (...) {
        }
    } else {
        return false;
    }
    return true;
}

inline void WriteBladeSoundDefaults(std::ostream& out) {
    const Settings d{};
    out << "# 1 = your two weapons ring against each other (the blade and the gun, the two blades "
           "of the\n# Blade of Mercy / Rakuyo; solid or not): a touch taps, a blade drawn along the "
           "other\n# scrapes like a sharpening stroke, back and forth a stroke each way\n"
        << "blade_sounds=" << (d.on ? 1 : 0) << "\n"
        << "# the game's sounds for each (SoundType 1 ids, comma separated, at most 6; never the "
           "same twice\n# in a row): a touch, a blade drawn along the other\n"
        << "blade_sound_tap=" << PoolText(d.tap) << "\n"
        << "blade_sound_scrape=" << PoolText(d.scrape) << "\n"
        << "# a blade drawn along the other grinds on while it slides (louder and brighter the "
           "faster):\n# 0 = off (the stroke sounds ring on instead), 1 = the game's sparks, 2 = "
           "lower and heavier\n"
        << "blade_grind=" << d.grind << "\n"
        << "# the grind's volume (0..2)\n"
        << "blade_grind_volume=" << d.grind_volume << "\n";
}

enum class Kind : std::uint8_t { None, Tap, Scrape };

constexpr const char* KindName(Kind k) {
    return k == Kind::Tap ? "tap" : k == Kind::Scrape ? "scrape" : "none";
}

// One update of the two weapons: whether they touch (or passed through each other since the last
// update), the normal between them (from the left weapon toward the right, unit) and the right
// weapon's velocity at that point relative to the left's (m/s).
struct Sample {
    float dt{0.0f};
    bool contact{false};
    Vec3 normal{};
    Vec3 v{};
};

struct Out {
    Kind kind{Kind::None};
    std::int32_t id{-1}; // the sound to play now (-1: none)
    float speed{0.0f};   // m/s: into the blade (tap), along it (scrape)
    float grind{0.0f};   // 0..1: the grind's level now (GrindLevel; 0: not sliding)
};

// Each kind's pool in turn, never the one just played (unless it is the only one).
class Picker {
public:
    std::int32_t Pick(const Pool& p, Kind k) {
        if (p.n <= 0) {
            return -1;
        }
        const size_t slot = static_cast<size_t>(k);
        int i = static_cast<int>(Next() % static_cast<std::uint32_t>(p.n));
        if (p.n > 1 && p.id[static_cast<size_t>(i)] == last[slot]) {
            i = (i + 1 + static_cast<int>(Next() % static_cast<std::uint32_t>(p.n - 1))) % p.n;
        }
        last[slot] = p.id[static_cast<size_t>(i)];
        return last[slot];
    }

private:
    std::uint32_t Next() { // xorshift32
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    std::uint32_t state{0x9E3779B9u};
    std::array<std::int32_t, 3> last{-1, -1, -1};
};

class Director {
public:
    // `grinding`: the grind loop plays (on and ready): the strokes ring once each.
    Out Step(const Sample& s, const Settings& set, bool grinding = false) {
        Out out = Decide(s, set, grinding);
        out.grind = grind;
        return out;
    }
    float Grind() const { return grind; }

private:
    Out Decide(const Sample& s, const Settings& set, bool grinding) {
        Out out{};
        since_sound += s.dt;
        since_stroke += s.dt;
        if (!s.contact || !(Body::Length(s.v) <= GlitchSpeed)) {
            gap += s.dt;
            if (gap > GapS) {
                touching = false;
                grind = 0.0f;
                EndStroke();
            } // (lost this briefly: the same contact, grinding on)
            return out;
        }
        gap = 0.0f;
        const bool fresh = !touching;
        touching = true;
        const float into = -Body::Dot(s.v, s.normal);
        const Vec3 slide = s.v + s.normal * into;
        const float along = Body::Length(slide);
        const Vec3 dir = along > 1e-6f ? slide * (1.0f / along) : Vec3{};
        const float came_in = prev_into;
        prev_into = into;
        // Sliding along it (not struck into it): it grinds.
        grind = along >= SlideSpeed && along >= into ? GrindLevel(along) : 0.0f;
        if (fresh) {
            EndStroke();
            if (along >= GlanceSpeed && along > into) {
                // A hit that glances off along the blade: a scrape at once.
                return Stroke(set, along, dir);
            }
            if (into >= TapSpeed && into >= TapIntoPerAlong * along) {
                return Play(Kind::Tap, set.tap, into, false);
            }
        } else if (into >= TapSpeed && came_in < RestrikeFromSpeed &&
                   into >= TapIntoPerAlong * along) {
            // Still touching, struck again: a tap (not a slide's jitter: into the blade).
            return Play(Kind::Tap, set.tap, into, false);
        }
        if (along < SlideSpeed) {
            still += s.dt;
            if (still >= PauseS) {
                EndStroke();
            }
            return out;
        }
        still = 0.0f;
        if (stroking) {
            if (Body::Dot(dir, stroke_dir) >= ReverseDot) {
                stroke_dir = dir; // one draw on and on (the blade turns as it goes) ...
                stroke_slid += along * s.dt;
                if (!grinding && since_stroke >= ScrapeRepeatS &&
                    stroke_slid >= ScrapeRepeatM) {
                    return Stroke(set, along, dir); // ... ringing on while it slides
                }
                return out;
            }
            EndStroke(); // turned back: the next stroke
        }
        if (slid > 0.0f && Body::Dot(dir, slid_dir) < ReverseDot) {
            slid = 0.0f;
        }
        slid += along * s.dt;
        slid_dir = dir;
        if (slid < StrokeStartM || since_stroke < StrokeRepeatS) {
            return out;
        }
        return Stroke(set, along, dir);
    }

    // A stroke starts with its sound (one held back by MinGapS starts on a later update).
    Out Stroke(const Settings& set, float along, Vec3 dir) {
        const Out out = Play(Kind::Scrape, set.scrape, along, true);
        if (out.id >= 0) {
            stroking = true;
            stroke_dir = dir;
            stroke_slid = 0.0f;
        }
        return out;
    }
    Out Play(Kind k, const Pool& p, float speed, bool stroke) {
        Out out{};
        if (since_sound < MinGapS) {
            return out;
        }
        out.id = picker.Pick(p, k);
        if (out.id < 0) {
            return out;
        }
        out.kind = k;
        out.speed = speed;
        since_sound = 0.0f;
        if (stroke) {
            since_stroke = 0.0f;
        }
        return out;
    }
    void EndStroke() {
        stroking = false;
        slid = 0.0f;
        still = 0.0f;
    }

    Picker picker{};
    bool touching{false};
    bool stroking{false};
    Vec3 stroke_dir{};
    Vec3 slid_dir{};
    float slid{0.0f};      // m slid one way toward the next stroke
    float stroke_slid{0.0f}; // m slid since the stroke's last ring
    float prev_into{0.0f};   // m/s into the blade last update (a restrike while touching)
    float still{0.0f};     // s the slide has stopped
    float gap{0.0f};
    float since_sound{10.0f};
    float since_stroke{10.0f};
    float grind{0.0f}; // the grind's level (held through a brief gap)
};

} // namespace VR::BladeSound
