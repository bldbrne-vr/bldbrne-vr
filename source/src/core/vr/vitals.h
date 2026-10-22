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

// Immersive vitals (vr.ini hud_vitals): the hunter's HP, stamina and bullets as a brass-and-glass
// Blood Gauge strapped to his left wrist (drawn into the game's frame, core/vr/
// bloodborne_vitals.cpp), plus the low-HP safety net (a heartbeat in the left controller and the
// creeping dark at the edges of the view, openxr_presenter.cpp). Everything here is pure (no
// emulator state): the values' mapping onto the gauge, the change edges, the animation, the
// heartbeat's timing, the edge darkness, the gauge's fit to the arm's real surface (from the
// game's own depth) and the glance test. Unit tested in tests/test_vitals.cpp.
namespace VR::Vitals {

// vr.ini hud_vitals.
enum class Mode : std::int32_t {
    Classic = 0, // the game's HUD as before (top-left HP / stamina / items)
    Wrist = 1,   // the Blood Gauge on the left wrist; the game's HP cluster only looking down
};

// ---- What site A reads (bloodborne_vitals.cpp, every main-player update) ------------------------
struct Status {
    std::uint64_t serial{0};   // bumps whenever a field below changes
    std::int64_t steady_ns{0}; // when it was read (stale reads mean the player update stopped)
    std::int32_t hp{0};
    std::int32_t max_hp{0};
    // The far end of the game's orange "rally" segment (HP that hitting back wins back; the HUD's
    // own end is HP + (int)pool, 0x1A0D3F0); == hp when there is none.
    std::int32_t rally_hp{0};
    std::int32_t stamina{0};
    std::int32_t max_stamina{0};
    std::int32_t qs_bullets{0};    // Quicksilver Bullets (goods 900) carried
    std::int32_t blood_bullets{0}; // Blood Bullets (goods 901): spent first, shown in red
    std::int32_t bullet_cap{20};   // the quicksilver tube's full scale
    bool ok{false};                // every read above succeeded
};

// The orange segment's end from the rally pool (ChrDataModule +0x108): HP + (int)pool, never past
// max HP; no pool (or a nonsense one) = no segment.
constexpr std::int32_t RallyEnd(std::int32_t hp, std::int32_t max_hp, float pool) {
    if (!(pool >= 1.0f) || !(pool < 1.0e7f) || hp <= 0) {
        return hp;
    }
    const std::int64_t end = static_cast<std::int64_t>(hp) + static_cast<std::int64_t>(pool);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(end, hp, std::max(hp, max_hp)));
}

// The quicksilver tube's full scale: 20 (the game's carry limit without the Formless Oedon rune)
// or the most ever seen.
constexpr std::int32_t BulletCap(std::int32_t seen_max) {
    return std::max<std::int32_t>(20, seen_max);
}

// ---- The gauge's levels ---------------------------------------------------------------------------
struct Fill {
    float hp{};      // 0 .. 1 of max HP
    float rally{};   // the orange segment's end, hp .. 1
    float stamina{}; // 0 .. 1
    float bullets{}; // quicksilver: 0 .. 1 of the cap
};

constexpr float Fraction(std::int32_t value, std::int32_t max) {
    if (max <= 0 || value <= 0) {
        return 0.0f;
    }
    return std::clamp(static_cast<float>(value) / static_cast<float>(max), 0.0f, 1.0f);
}

constexpr Fill FillOf(const Status& s) {
    Fill f{};
    f.hp = Fraction(s.hp, s.max_hp);
    f.rally = std::max(f.hp, Fraction(s.rally_hp, s.max_hp));
    f.stamina = Fraction(s.stamina, s.max_stamina);
    f.bullets = Fraction(s.qs_bullets, std::max(s.bullet_cap, 1));
    return f;
}

// The counter's number: the Blood Bullets while he has any (the game spends them first and its
// HUD shows them in red), else the Quicksilver Bullets.
constexpr std::int32_t BulletsShown(const Status& s) {
    return s.blood_bullets > 0 ? s.blood_bullets : std::max(s.qs_bullets, 0);
}

// The counter's ink.
enum class Tone : std::uint8_t {
    Bone,     // plenty
    Amber,    // 3 or fewer
    DarkRed,  // none
    FreshRed, // Blood Bullets
};

constexpr Tone ToneOf(const Status& s) {
    if (s.blood_bullets > 0) {
        return Tone::FreshRed;
    }
    if (s.qs_bullets <= 0) {
        return Tone::DarkRed;
    }
    return s.qs_bullets <= 3 ? Tone::Amber : Tone::Bone;
}

// The palette (display sRGB, 0..255), section 1.5 of the design.
struct Srgb {
    std::uint8_t r, g, b;
};
constexpr Srgb ColorDriedBlood{0x3a, 0x0a, 0x0a};
constexpr Srgb ColorBlood{0x9b, 0x1b, 0x1e};
constexpr Srgb ColorFreshBlood{0xd0, 0x2a, 0x22};
constexpr Srgb ColorRally{0xc9, 0x82, 0x2f};
constexpr Srgb ColorStamina{0x6f, 0x8f, 0x4e};
constexpr Srgb ColorBone{0xe6, 0xdc, 0xc6};
constexpr Srgb ColorBrass{0xa8, 0x83, 0x4a};
constexpr Srgb ColorQuicksilver{0xc9, 0xce, 0xd3};
constexpr Srgb ColorSoot{0x12, 0x0d, 0x0b};
constexpr Srgb ColorEdgeDark{0x2a, 0x06, 0x06};

inline float SrgbToLinear(std::uint8_t c) {
    const float v = static_cast<float>(c) / 255.0f;
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

inline std::array<float, 3> Linear(Srgb c) {
    return {SrgbToLinear(c.r), SrgbToLinear(c.g), SrgbToLinear(c.b)};
}

constexpr Srgb ToneColor(Tone t) {
    switch (t) {
    case Tone::Amber:
        return ColorRally;
    case Tone::DarkRed:
        return Srgb{0x7a, 0x14, 0x12};
    case Tone::FreshRed:
        return ColorFreshBlood;
    case Tone::Bone:
        break;
    }
    return ColorBone;
}

// ---- Change edges ---------------------------------------------------------------------------------
enum ChangeBits : std::uint32_t {
    ChangeHit = 1u << 0,          // HP went down
    ChangeHeal = 1u << 1,         // HP went up past the rally segment (an item, a lamp)
    ChangeRally = 1u << 2,        // HP won back within the rally segment (hitting back)
    ChangeStaminaSpent = 1u << 3, // stamina went down
    ChangeStaminaEmpty = 1u << 4, // ... to nothing
    ChangeShot = 1u << 5,         // the counter went down
    ChangeRestock = 1u << 6,      // ... up
    ChangeMaxHp = 1u << 7,        // max HP changed (a level up, an effect): no hit or heal
};

struct ChangeTracker {
    bool valid{false};
    Status last{};

    std::uint32_t Step(const Status& s) {
        if (!s.ok) {
            return 0;
        }
        if (!valid) {
            valid = true;
            last = s;
            return 0;
        }
        std::uint32_t bits = 0;
        if (s.max_hp != last.max_hp) {
            bits |= ChangeMaxHp;
        } else if (s.hp < last.hp) {
            bits |= ChangeHit;
        } else if (s.hp > last.hp) {
            bits |= s.hp <= last.rally_hp && last.rally_hp > last.hp ? ChangeRally : ChangeHeal;
        }
        if (s.stamina < last.stamina) {
            bits |= ChangeStaminaSpent;
            if (s.stamina <= 0 && last.stamina > 0) {
                bits |= ChangeStaminaEmpty;
            }
        }
        const std::int32_t before = BulletsShown(last);
        const std::int32_t now = BulletsShown(s);
        if (now < before) {
            bits |= ChangeShot;
        } else if (now > before) {
            bits |= ChangeRestock;
        }
        last = s;
        return bits;
    }
};

// ---- The heartbeat (the low-HP safety net) -------------------------------------------------------
// Beats per minute by the HP fraction: none above `low` (vitals_low_hp, 0.30), 60 at it, 110 at
// 10%, 130 at 5% and below.
constexpr float BeatsPerMinute(float hp, float low = 0.30f) {
    if (!(hp < low) || hp <= 0.0f) {
        return 0.0f;
    }
    const float mid = std::min(0.10f, low);
    if (hp >= mid) {
        const float t = low > mid ? (low - hp) / (low - mid) : 1.0f;
        return 60.0f + 50.0f * std::clamp(t, 0.0f, 1.0f);
    }
    if (hp >= 0.05f) {
        return 110.0f + 20.0f * (mid - hp) / std::max(mid - 0.05f, 1e-3f);
    }
    return 130.0f;
}

// The "lub-dub" seen and felt: the lub at the beat (40 ms), the dub 150 ms later (35 ms).
struct BeatShape {
    float lub{0.35f}; // haptic amplitudes (before haptic_floor: well under a shot's 1.0)
    float dub{0.28f};
    float lub_s{0.040f};
    float dub_s{0.035f};
    float dub_at_s{0.150f};
};

// The visual pulse `t` seconds after a beat: 1 at the lub, 0.7 at the dub, each decaying over
// about 0.12 s; 0 between beats.
inline float BeatEnvelope(float t, const BeatShape& shape = {}) {
    if (!(t >= 0.0f)) {
        return 0.0f;
    }
    const auto bump = [](float x) { // rises in 25 ms, decays with tau 70 ms
        if (x < 0.0f) {
            return 0.0f;
        }
        if (x < 0.025f) {
            return x / 0.025f;
        }
        return std::exp(-(x - 0.025f) / 0.07f);
    };
    return std::max(bump(t), 0.7f * bump(t - shape.dub_at_s));
}

struct Heartbeat {
    bool beating{false};
    float since_beat_s{0.0f};
    float bpm{0.0f};
    std::uint64_t beats{0};

    // One step: the HP fraction, whether it may beat at all (gameplay, alive, the safety net on).
    // True on a new beat (the haptic's moment). The rate follows the HP at every beat.
    bool Step(float dt, float hp, bool active, float low = 0.30f) {
        dt = std::clamp(dt, 0.0f, 0.25f);
        const float rate = active ? BeatsPerMinute(hp, low) : 0.0f;
        if (rate <= 0.0f) {
            beating = false;
            since_beat_s = 0.0f;
            bpm = 0.0f;
            return false;
        }
        if (!beating) {
            beating = true;
            since_beat_s = 0.0f;
            bpm = rate;
            ++beats;
            return true;
        }
        since_beat_s += dt;
        const float period = 60.0f / std::max(bpm, 1.0f);
        if (since_beat_s >= period) {
            since_beat_s = std::fmod(since_beat_s - period, period);
            bpm = rate;
            ++beats;
            return true;
        }
        return false;
    }

    [[nodiscard]] float Pulse(const BeatShape& shape = {}) const {
        return beating ? BeatEnvelope(since_beat_s, shape) : 0.0f;
    }
};

// The heartbeat's haptic: only within `mute_s` (20 s) of the last HP lost (a long low-HP
// stretch, exploring with little HP, falls silent; the next hit wakes it).
constexpr bool HeartbeatFelt(std::int64_t now_ns, std::int64_t last_loss_ns, float mute_s = 20.0f) {
    return last_loss_ns != 0 && now_ns >= last_loss_ns &&
           static_cast<double>(now_ns - last_loss_ns) * 1e-9 < static_cast<double>(mute_s);
}

// ---- The creeping dark at the edges (vitals_edge) ------------------------------------------------
// Its strength (opacity at the edge of the view) by the HP fraction: 0 above 60%, 0.18 at 30%,
// 0.42 at 5% and below.
constexpr float EdgeStrength(float hp) {
    if (!(hp < 0.60f)) {
        return 0.0f;
    }
    if (hp >= 0.30f) {
        return 0.18f * (0.60f - hp) / 0.30f;
    }
    if (hp >= 0.05f) {
        return 0.18f + 0.24f * (0.30f - hp) / 0.25f;
    }
    return 0.42f;
}

// Clear inside EdgeInnerDeg of the view's centre, full at EdgeFullDeg; below `low` the heartbeat
// swells it (+25% at the lub) and draws it EdgeSwellDeg further in.
constexpr float EdgeInnerDeg = 30.0f;
constexpr float EdgeFullDeg = 58.0f;
constexpr float EdgeSwellDeg = 3.0f;
constexpr float EdgeSwell = 0.25f;
// The amber rally rim's opacity when the rally appears (fading as the game drains it): 0.05, half
// the first 0.10, which read too strongly in dark places (the clinic).
constexpr float RallyRimAlpha = 0.05f;

struct Edge {
    float alpha{};    // the dark's opacity at the edges
    float inner_deg{EdgeInnerDeg};
    float full_deg{EdgeFullDeg};
    float rim{};      // the amber rally rim's opacity
};

// `pulse`: the heartbeat's envelope (0 above `low`); `regain`: the rally segment's share of max HP
// now, `regain_peak` its largest since it appeared (the rim fades as the game drains it); `flash`:
// the hit / heal vignettes' opacity now (combined by max: the edges never go fully opaque).
inline Edge EdgeOf(float hp, float pulse, float regain, float regain_peak, float flash,
                   float low = 0.30f) {
    Edge e{};
    const float base = EdgeStrength(hp);
    const bool beat = hp < low && hp > 0.0f;
    const float p = beat ? std::clamp(pulse, 0.0f, 1.0f) : 0.0f;
    const float dark = std::min(base * (1.0f + EdgeSwell * p), 0.6f);
    e.alpha = std::max(0.0f, dark - std::max(flash, 0.0f));
    e.inner_deg = EdgeInnerDeg - EdgeSwellDeg * p;
    e.full_deg = EdgeFullDeg - EdgeSwellDeg * p;
    if (regain > 0.0f && regain_peak > 0.0f) {
        e.rim = RallyRimAlpha * std::clamp(regain / regain_peak, 0.0f, 1.0f);
    }
    return e;
}

// ---- The gauge's animation ------------------------------------------------------------------------
struct GaugeLook {
    float hp{};      // the levels shown (0 .. 1)
    float rally{};
    float stamina{};
    float bullets{};
    float glow{};    // the soft inner glow of a glance or a change (0 .. 0.3)
    float pulse{};   // the heartbeat (0 .. 1, below the low-HP line)
    float fog{};     // the stamina tube's glass fogged grey (0 .. 1)
    float bubbles{}; // a heal's bubbles: their phase 0 .. 1 (0 = none)
    float shake_mm{}; // the tubes' shake after a hit (mm, signed)
    float alpha{};   // the whole gauge
    std::int32_t digits{0};      // the counter's number now
    std::int32_t digits_old{0};  // ... rolling away from this one
    float roll{1.0f};            // 0 .. 1 (1 = settled)
    Tone tone{Tone::Bone};
};

struct GaugeTiming {
    float heal_s{0.30f};     // a heal's rise
    float shake_s{0.20f};    // a hit's shake
    float shake_mm{0.5f};
    float fog_hold_s{0.25f}; // empty stamina: fogged, then clearing over fog_s
    float fog_s{0.40f};
    float bubbles_s{0.60f};
    float roll_s{0.12f};     // the counter's roll
    float glow{0.30f};       // a glance or a change
    float glow_in_s{0.15f};
    float glow_out_s{0.60f};
    float change_glow_s{1.5f}; // a change keeps the glow this long
    float fade_in_s{0.15f};
    float fade_out_s{0.30f};
};

struct GaugeAnim {
    bool valid{false};
    float hp{};
    float heal_from{};
    float heal_t{1.0f}; // 0 .. 1 of the rise
    float stamina{};
    float shake_t{1.0e3f};
    float fog_t{1.0e3f};
    float bubbles_t{1.0e3f};
    float glow{};
    float change_t{1.0e3f};
    float alpha{};
    float time{};
    std::int32_t digits{0};
    std::int32_t digits_old{0};
    float roll_t{1.0e3f};

    // One frame: the newest status, its change bits since the last step, whether you look at it
    // and whether it may show (gameplay, alive), the heartbeat's pulse.
    GaugeLook Step(const Status& s, std::uint32_t changes, float dt, bool glanced, bool visible,
                   float pulse, const GaugeTiming& t = {}) {
        dt = std::clamp(dt, 0.0f, 0.25f);
        time += dt;
        const Fill f = FillOf(s);
        const std::int32_t count = BulletsShown(s);
        if (!valid) {
            valid = true;
            hp = heal_from = f.hp;
            stamina = f.stamina;
            digits = digits_old = count;
        }
        // HP: a drop shows at once, a rise fills over heal_s.
        if (f.hp < hp - 1e-6f) {
            hp = f.hp;
            heal_t = 1.0f;
        } else if (f.hp > hp + 1e-6f && heal_t >= 1.0f) {
            heal_from = hp;
            heal_t = 0.0f;
        }
        if (heal_t < 1.0f) {
            heal_t = std::min(heal_t + dt / std::max(t.heal_s, 1e-3f), 1.0f);
            hp = heal_from + (f.hp - heal_from) * heal_t;
            if (f.hp < hp) {
                hp = f.hp;
            }
        } else {
            hp = f.hp;
        }
        // Stamina follows live (the busiest element), a little smoothed.
        stamina += (f.stamina - stamina) * (1.0f - std::exp(-dt / 0.05f));
        if (std::abs(f.stamina - stamina) < 0.002f) {
            stamina = f.stamina;
        }
        if (changes & ChangeHit) {
            shake_t = 0.0f;
        }
        if (changes & ChangeStaminaEmpty) {
            fog_t = 0.0f;
        }
        if (changes & ChangeHeal) {
            bubbles_t = 0.0f;
        }
        if (changes & (ChangeHit | ChangeHeal | ChangeRally | ChangeShot | ChangeRestock)) {
            change_t = 0.0f;
        }
        if (count != digits) {
            digits_old = digits;
            digits = count;
            roll_t = 0.0f;
        }
        shake_t += dt;
        fog_t += dt;
        bubbles_t += dt;
        change_t += dt;
        roll_t += dt;
        const bool glow_on = glanced || change_t < t.change_glow_s;
        glow = glow_on ? std::min(glow + dt * t.glow / std::max(t.glow_in_s, 1e-3f), t.glow)
                       : std::max(glow - dt * t.glow / std::max(t.glow_out_s, 1e-3f), 0.0f);
        alpha = visible ? std::min(alpha + dt / std::max(t.fade_in_s, 1e-3f), 1.0f)
                        : std::max(alpha - dt / std::max(t.fade_out_s, 1e-3f), 0.0f);
        GaugeLook look{};
        look.hp = hp;
        look.rally = std::max(f.rally, hp);
        look.stamina = std::clamp(stamina, 0.0f, 1.0f);
        look.bullets = f.bullets;
        look.glow = glow;
        look.pulse = std::clamp(pulse, 0.0f, 1.0f);
        look.fog = fog_t < t.fog_hold_s ? 1.0f
                   : fog_t < t.fog_hold_s + t.fog_s
                       ? 1.0f - (fog_t - t.fog_hold_s) / std::max(t.fog_s, 1e-3f)
                       : 0.0f;
        look.bubbles = bubbles_t < t.bubbles_s ? std::max(bubbles_t / t.bubbles_s, 1e-3f) : 0.0f;
        if (shake_t < t.shake_s) {
            const float k = 1.0f - shake_t / t.shake_s;
            look.shake_mm = t.shake_mm * k * std::sin(shake_t * 2.0f * 3.14159265f * 25.0f);
        }
        look.alpha = alpha;
        look.digits = digits;
        look.digits_old = digits_old;
        look.roll = roll_t < t.roll_s ? roll_t / t.roll_s : 1.0f;
        look.tone = ToneOf(s);
        return look;
    }
};

// ---- The fit to the arm's surface ----------------------------------------------------------------
// The gauge rests on whatever the hunter wears (a bare wrist, a glove's cuff, a gauntlet): each
// frame the render tick asks the in-frame renderer for the game's depth at points along lines
// from the forearm's axis outward through the gauge (FitLines lines, FitSteps points each, the
// radii FitRadius(i)), and gets back, a frame or two later, how far behind (+) or in front (-)
// of the scene each point was. A line's exit is where its points come out of the arm.
constexpr int FitLines = 5;  // the centre, 22 mm toward the elbow / the hand, 23 degrees round
constexpr int FitSteps = 15; // radii 12 .. 96 mm
constexpr float FitFirstM = 0.012f;
constexpr float FitStepM = 0.006f;
constexpr int FitPoints = FitLines * FitSteps;

constexpr float FitRadius(int step) {
    return FitFirstM + FitStepM * static_cast<float>(step);
}

struct FitConfig {
    float tolerance_m{0.002f}; // a point this close behind the surface counts as on it
    float occluder_m{0.12f};   // something this far in front of a point: not the arm, a hand
    float standoff_m{0.003f};  // the plate's underside above the surface
    float min_m{0.022f};       // a bare wrist's radius can be this small
    float max_m{0.110f};       // a gauntlet's this large
    float default_m{0.036f};   // before the first measurement (a sleeve, slightly above a bare wrist)
    float rise_s{0.05f};       // the plate lifts at once (it must never sink into a cuff) ...
    float fall_s{0.60f};       // ... and settles slowly (no jitter)
};

// One line's exit radius (metres from the axis) from its points' gaps (point depth - scene depth,
// metres; + = behind the scene, inside the arm), `ok` false for a point the probe could not read
// (off the frame). -1: no exit seen (the start already outside: the axis is off the arm; never
// outside: the arm is hidden by something nearer, or bigger than the probe).
inline float LineExit(const float* gap, const bool* ok, int n, const FitConfig& c = {}) {
    // The first points may have no depth (a hole in the game's depth at the arm's middle: a
    // dithered cut, a texel off the frame's edge): from the first of the first three that has.
    int first = 0;
    while (first < std::min(n, 3) && !ok[first]) {
        ++first;
    }
    if (first >= std::min(n, 3) || gap[first] <= c.tolerance_m) {
        return -1.0f;
    }
    for (int i = first + 1; i < n; ++i) {
        if (!ok[i]) {
            return -1.0f;
        }
        if (gap[i - 1] > c.occluder_m) {
            return -1.0f; // something big in front of the arm (a hand, a weapon, an enemy)
        }
        if (gap[i] <= c.tolerance_m) {
            // Where the gap crosses 0: the surface itself.
            const float g0 = gap[i - 1];
            const float g1 = gap[i];
            float frac = g0 / std::max(g0 - g1, 1e-6f);
            // A jump to the background (seen from the side: the arm's silhouette) gives no slope:
            // the midpoint.
            if (gap[i] < -0.02f) {
                frac = 0.5f;
            }
            return FitRadius(i - 1) + FitStepM * std::clamp(frac, 0.0f, 1.0f);
        }
    }
    return -1.0f;
}

// The plate must clear the highest of the sleeve under it: the largest exit of the lines that saw
// one, from at least two lines (else -1: no measurement this frame), leaving out a line more than
// `outlier_m` above the lines' median (not the sleeve: the gun's grip beside the wrist, the other
// hand's fingers).
inline float CombineExits(const float* exits, int lines, float outlier_m = 0.012f) {
    std::array<float, 16> valid{};
    int n = 0;
    for (int i = 0; i < lines && n < static_cast<int>(valid.size()); ++i) {
        if (exits[i] > 0.0f) {
            valid[static_cast<std::size_t>(n++)] = exits[i];
        }
    }
    if (n < 2) {
        return -1.0f;
    }
    std::sort(valid.begin(), valid.begin() + n);
    const float median = (n % 2) != 0 ? valid[static_cast<std::size_t>(n / 2)]
                                      : 0.5f * (valid[static_cast<std::size_t>(n / 2 - 1)] +
                                                valid[static_cast<std::size_t>(n / 2)]);
    float best = -1.0f;
    for (int i = 0; i < n; ++i) {
        if (valid[static_cast<std::size_t>(i)] <= median + outlier_m) {
            best = std::max(best, valid[static_cast<std::size_t>(i)]);
        }
    }
    return best;
}

// The plate's radius over time: the median of the last 5 measurements, plus the stand-off,
// approached quickly upward and slowly downward; held while nothing is measured.
struct FitFilter {
    bool valid{false};
    float radius{0.0f};
    std::array<float, 5> recent{};
    int count{0};
    int next{0};
    float since_s{1.0e3f}; // since the last measurement
    std::uint64_t measured{0};

    float Step(float measured_m, float dt, const FitConfig& c = {}) {
        dt = std::clamp(dt, 0.0f, 0.5f);
        since_s += dt;
        if (measured_m > 0.0f) {
            ++measured;
            since_s = 0.0f;
            recent[static_cast<std::size_t>(next)] = measured_m;
            next = (next + 1) % static_cast<int>(recent.size());
            count = std::min(count + 1, static_cast<int>(recent.size()));
            std::array<float, 5> sorted = recent;
            std::sort(sorted.begin(), sorted.begin() + count);
            const float median = sorted[static_cast<std::size_t>(count / 2)];
            const float target = std::clamp(median + c.standoff_m, c.min_m, c.max_m);
            if (!valid) {
                valid = true;
                radius = target;
            } else {
                const float tau = target > radius ? c.rise_s : c.fall_s;
                radius += (target - radius) * (1.0f - std::exp(-dt / std::max(tau, 1e-3f)));
            }
        } else if (!valid) {
            radius = c.default_m;
        }
        return radius;
    }
};

// Resize the sleeve fit only while the arm and its view are settled. This never
// filters the watch transform: it remains attached directly to the current arm.
// Remember eligibility at probe submission too, so a delayed moving-frame result
// cannot resize the watch just after the player stops.
struct FitMotionGate {
    // World wrist, view wrist, view forearm direction, view dorsal direction.
    using Pose = std::array<float, 12>;
    Pose previous{};
    bool have{false};
    float settled_s{};
    std::uint32_t epoch{1};
    struct Probe {
        std::uint32_t id{}, epoch{};
    };
    std::array<Probe, 16> probes{};

    bool Step(const Pose& pose, float dt) {
        bool quiet = have && dt > 0.0f && dt <= 0.1f;
        for (std::size_t v = 0; v < 4; ++v) {
            float distance_sq = 0.0f;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const auto k = v * 3 + axis;
                const float d = pose[k] - previous[k];
                distance_sq += d * d;
            }
            const float limit = (v < 2 ? 0.20f : 1.0f) * dt;
            quiet = quiet && std::isfinite(distance_sq) && distance_sq <= limit * limit;
        }
        previous = pose;
        have = true;
        if (!quiet) {
            settled_s = 0.0f;
            if (++epoch == 0)
                ++epoch;
        } else {
            settled_s = std::min(settled_s + dt, 0.2f);
        }
        return Ready();
    }
    bool Ready() const {
        return settled_s >= 0.15f;
    }
    void Submitted(std::uint32_t id) {
        probes[id % probes.size()] = {id, Ready() ? epoch : 0};
    }
    bool Accept(std::uint32_t id) const {
        const auto& p = probes[id % probes.size()];
        return id != 0 && Ready() && p.id == id && p.epoch == epoch;
    }
};

// ---- The straps' fit: each half of a strap hugs the side of the sleeve -----------------------------
// A strap is a ring segment about the forearm's axis: at the plate's level on top (under its lugs,
// semi-axis `a` along the plate's normal), and round each side of the arm an ellipse whose other
// semi-axis `b` makes it reach the sleeve where the probe line at StrapPolarRad round the arm (at
// the strap's place along it) came out of the sleeve; it fades out there (`end` in the ring's own
// angle). A round strap at the plate's radius stood off a slimmer forearm by up to 2 cm at its
// sides (the plate rests on the highest cuff), visible as bands and horns in the air beside the
// arm. Seen from the far side, a line comes out at the arm's silhouette: the strap then ends
// there instead of sticking out of it.
constexpr int StrapLines = 4; // (hand strap, elbow strap) x (round +v, round -v)
constexpr float StrapPolarRad = 58.0f * 3.14159265f / 180.0f;
constexpr int ProbeLines = FitLines + StrapLines;
constexpr int ProbePoints = ProbeLines * FitSteps;
constexpr float StrapEndCircle = 0.95f; // a round strap's fade end (the ring's angle, radians)

struct StrapSide {
    bool ok{false};
    float b{0.0f};               // the semi-axis round the arm (metres)
    float end{StrapEndCircle};   // where it fades out (the ring's angle, radians)
};

// `a` the strap's height on top (metres from the axis), `exit` where the side line came out of the
// sleeve (metres, <= 0: not seen), `thickness` the leather's.
inline StrapSide FitStrapSide(float a, float exit, float thickness = 0.0015f,
                              float polar = StrapPolarRad) {
    StrapSide s{};
    if (!(a > 0.005f) || !(exit > 0.0f)) {
        return s;
    }
    const float r = exit + thickness;
    const float up = r * std::cos(polar);
    const float across = r * std::sin(polar);
    if (up >= 0.97f * a || r > 1.25f * a) {
        return s; // the side as high as the top, or far out (the gun's grip, a hand): not the sleeve
    }
    const float end = std::acos(up / a);
    s.end = std::clamp(end, 0.60f, 1.25f);
    s.b = std::clamp(across / std::max(std::sin(s.end), 0.2f), 0.45f * a, 1.05f * a);
    s.ok = true;
    return s;
}

// One strap side over time: eased toward each new fit (no flicker), held through a short gap
// (a hand passing in front); a line that sees nothing longer (the gun's grip beside a raised
// wrist hides that side) narrows it (StrapStaleRatio, StrapStaleEnd): a strap a little inside the
// sleeve still shows on it (the gauge's depth pull) but one too wide stands off it in the air,
// and a fit made from another view (that side's silhouette) is not that side's surface. A round
// strap until the first fit.
constexpr float StrapStaleS = 0.30f;
constexpr float StrapStaleRatio = 0.85f;
constexpr float StrapStaleEnd = 1.00f;

struct StrapFilter {
    bool valid{false};
    float b_over_a{1.0f};
    float end{StrapEndCircle};
    float since_s{0.0f}; // since the last fit

    void Step(const StrapSide& s, float a, float dt, float tau_s = 0.12f) {
        dt = std::clamp(dt, 0.0f, 0.5f);
        if (!s.ok || !(a > 0.0f)) {
            since_s += dt;
            if (valid && since_s > StrapStaleS) {
                const float k = 1.0f - std::exp(-dt / 0.25f);
                b_over_a += (std::min(b_over_a, StrapStaleRatio) - b_over_a) * k;
                end += (std::min(end, StrapStaleEnd) - end) * k;
            }
            return;
        }
        since_s = 0.0f;
        const float ratio = s.b / a;
        if (!valid) {
            valid = true;
            b_over_a = ratio;
            end = s.end;
            return;
        }
        const float k = 1.0f - std::exp(-dt / std::max(tau_s, 1e-3f));
        b_over_a += (ratio - b_over_a) * k;
        end += (s.end - end) * k;
    }
};

// ---- The glance (a look at the gauge brightens it) -----------------------------------------------
struct V3 {
    float x{}, y{}, z{};
};
inline float Dot(V3 a, V3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline float Length(V3 a) {
    return std::sqrt(Dot(a, a));
}

struct GlanceConfig {
    float show_m{0.65f};
    float show_deg{22.0f}; // Quest 3: no eye tracking, the cone allows for the eyes' glance
    float show_facing{0.35f};
    float show_hold_s{0.12f};
    float hide_m{0.75f};
    float hide_deg{30.0f};
    float hide_facing{0.20f};
    float hide_hold_s{0.25f};
};

struct GlanceState {
    bool on{false};
    float held_s{0.0f};
};

// `p` the gauge's centre, `n` its outward normal, `h` the eye, `f` the view's forward (unit), one
// space. Shown after the look held show_hold_s, hidden after it was lost for hide_hold_s.
inline bool StepGlance(GlanceState& st, V3 p, V3 n, V3 h, V3 f, float dt,
                       const GlanceConfig& c = {}) {
    const V3 d{p.x - h.x, p.y - h.y, p.z - h.z};
    const float dist = Length(d);
    bool in = false;
    bool out = true;
    if (dist > 1e-4f) {
        const V3 u{d.x / dist, d.y / dist, d.z / dist};
        const float cosang = std::clamp(Dot(f, u), -1.0f, 1.0f);
        const float ang = std::acos(cosang) * 57.29578f;
        const float facing = -Dot(n, u);
        in = dist < c.show_m && ang < c.show_deg && facing > c.show_facing;
        out = dist > c.hide_m || ang > c.hide_deg || facing < c.hide_facing;
    }
    const bool towards = st.on ? out : in;
    st.held_s = towards ? st.held_s + std::max(dt, 0.0f) : 0.0f;
    if (towards && st.held_s >= (st.on ? c.hide_hold_s : c.show_hold_s)) {
        st.on = !st.on;
        st.held_s = 0.0f;
    }
    return st.on;
}

// ---- The counter's reading direction ----------------------------------------------------------------
// The digits stand upright as seen from the eye: of the plate's four in-plane directions (toward
// the hand +u, the elbow -u, round the arm +v / -v), the one nearest the screen's up, given the
// screen-space directions of +u and +v. A turn needs to be better by `hysteresis` (the sine of
// about 20 degrees) so the digits never flicker between two.
inline int DigitUp(float ux, float uy, float vx, float vy, int current, float hysteresis = 0.34f) {
    const auto norm = [](float& x, float& y) {
        const float l = std::sqrt(x * x + y * y);
        if (l > 1e-6f) {
            x /= l;
            y /= l;
        }
    };
    norm(ux, uy);
    norm(vx, vy);
    const std::array<float, 4> up{uy, -uy, vy, -vy};
    int best = 0;
    for (int i = 1; i < 4; ++i) {
        if (up[static_cast<std::size_t>(i)] > up[static_cast<std::size_t>(best)]) {
            best = i;
        }
    }
    if (current < 0 || current > 3) {
        return best;
    }
    return up[static_cast<std::size_t>(best)] > up[static_cast<std::size_t>(current)] + hysteresis
               ? best
               : current;
}

// ---- Gauge geometry (millimetres; the tester's "a tiny bit larger": the design's x1.15) ---------------
// u: along the arm, 0 = the elbow end of the bracket, GaugeLengthMm = its hand end; v: round the
// arm, 0 .. GaugeWidthMm. The tubes read along the arm, full toward the hand.
constexpr float GaugeScale = 1.15f;
constexpr float GaugeLengthMm = 46.0f * GaugeScale; // 52.9
constexpr float GaugeWidthMm = 30.0f * GaugeScale;  // 34.5
constexpr float GaugeDigitMm = 10.5f;               // the counter's digits, ink height
// The bracket's centre: this far toward the elbow from the wrist (the hand bending back never
// covers it).
constexpr float GaugeFromWristMm = 45.0f;

} // namespace VR::Vitals
