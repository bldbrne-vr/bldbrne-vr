// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "core/vr/arm_ik.h"
#include "core/vr/whip_settings.h"

// The Kos Parasite's tendril lash (2026-11-09: "like the Beast Cutter's whip, but it
// rolls back much sooner, momentum based"; home 0.2 s after a stop, confirmed on the mock page).
// The game draws the Kos Parasite's tendrils with a hidden weapon it swaps in (40000000, model
// wp_a_1711): form A one tendril of 9 joints on the right hand, 0.40 m at rest, which the game's
// own attacks grow to 1.6-1.9 m (R1, R2) and 2.7 m (the charged R2). Ours throws that tendril on
// the Beast Cutter's swing-path lash (core/vr/cutter_path.h, ChainConfig::path): the same code,
// the Kos's dials below. None of the game's attack animations play: the swing is yours.
//
// Two things of its own, both measured from the game's clips: a recoil (as a stroke turns back in
// the tendril snaps in and springs out again, 5-69 cm, 2-4 times a second) and, once the stroke is
// spent, a little sway (turns only, never in or out: alive, not reaching).
namespace VR::Whip::Kos {

inline constexpr float RestM = 0.40f;         // the game's tendril at rest
inline constexpr float ReachM = 1.95f;        // swung (the game's R1 / R2 reach)
inline constexpr float ChargedReachM = 2.7f;  // a charged swing (the game's charged R2)

// The girth multiplier grows to 2.25 at twice the rest length. BoneScale applies
// it to each native tendril's own proportions; the hook converts model scales to locals.
inline float Girth(float extension) {
    const float t = std::clamp(extension - 1.0f, 0.0f, 1.0f);
    return 1.0f + 1.25f * t * t * (3.0f - 2.0f * t);
}

// Each native tendril has its own skinning proportions. In particular the two
// thinner right tendrils use 0.6 girth, while the left fingers use shorter links
// with roughly unit length scale. A shared 10 cm/unit rule distorts that hand.
// Capture once from idle; the hot path needs only multiplication and bounds.
struct BoneScale {
    VR::Body::Vec3 rest{1.0f, 1.0f, 1.0f};
    float per_m{0.0f};

    bool Set(VR::Body::Vec3 scale, float link) {
        if (!VR::Body::IsFinite(scale) || !std::isfinite(link) || link <= 1e-4f ||
            scale.x <= 0.0f || scale.y <= 0.0f || scale.z <= 0.0f ||
            scale.x > 16.0f || scale.y > 16.0f || scale.z > 16.0f) {
            return false;
        }
        rest = scale;
        per_m = scale.x / link;
        return true;
    }

    VR::Body::Vec3 At(float link, float girth) const {
        if (per_m <= 0.0f || !std::isfinite(link) || !std::isfinite(girth)) return rest;
        // All native lash caps are below 32x. Keep corrupt inputs bounded too.
        const float g = std::clamp(girth, 0.25f, 4.0f);
        return {std::clamp(link * per_m, 0.05f * rest.x, 32.0f * rest.x),
                rest.y * g, rest.z * g};
    }
};

// The bones' local scales that give them the model scales `target` (X along, Y/Z girth), under a
// parent of model scale `parent`: the game composes a bone's model scale as its parent's times its
// own, so each local is the bone's model scale over the one before (in the first test every bone
// took the girth itself: 2.25 to the ninth power, a slab filling the view). The target is first
// blended with the clip's own model scales `clip` by `weight`, so a hand-over to or from a clip
// (with a stretch of its own) blends as the pose does.
inline void LocalScales(const VR::Body::Vec3* target, const VR::Body::Vec3* clip,
                        VR::Body::Vec3 parent, int n, float weight, VR::Body::Vec3* local) {
    const float w = std::clamp(weight, 0.0f, 1.0f);
    const auto over = [](float a, float b) { return std::abs(b) > 1e-6f ? a / b : a; };
    VR::Body::Vec3 above = parent;
    for (int k = 0; k < n; ++k) {
        const VR::Body::Vec3 m{clip[k].x + (target[k].x - clip[k].x) * w,
                               clip[k].y + (target[k].y - clip[k].y) * w,
                               clip[k].z + (target[k].z - clip[k].z) * w};
        local[k] = {over(m.x, above.x), over(m.y, above.y), over(m.z, above.z)};
        above = m;
    }
}

// The Cutter's lab dials with the Kos's: pays out from 1.8 m/s and is full at 5 (the Cutter 2.5
// and 9), its momentum halves every 0.08 s once the swing slows (0.18), it reels in from 0.25 s
// quickening to 0.15 s (0.7 to 0.3; Return below sets these per stroke), out in 0.10 s (0.14); the
// tip trails 0.15 s behind the arm's line (0.18) and curls up to 58 degrees (30: the user,
// 2026-11-09, "more curving... it should feel alive, it's not just a whip"); weight 0.6, a living
// tendril holds itself up (1.4).
inline CutterLab Lab() {
    CutterLab l{};
    l.on = true;
    l.path_model = true;
    l.air = 0.8f;
    l.tout = 0.10f;
    l.tin = 0.15f;
    l.mhold = 0.08f;
    l.path = 70.0f;
    l.ptaper = 0.35f;
    l.pzeta = 0.45f;
    l.pdelay = 0.15f;
    l.pcurl = 58.0f;
    l.pkeep = 0.4f;
    l.pout = 5.0f;
    l.pin = 1.8f;
    l.pmin = 0.45f;
    l.pslow = 0.25f;
    l.paccel = 0.12f;
    l.pgrav = 0.6f;
    return l;
}

// Its mood, rolled with each stroke (2026-11-09: "roll a dice and 1/4 make it wiggle way
// more, and another 50% after the 1/4 to wiggle even more (even while the attack is going and it's
// flying around)"): 3 strokes in 4 calm; of the rest half wild (twice the sway once spent and while
// out) and half frenzied (nearly three times once spent, writhing four times as hard while it
// flies, pulsing half again as much).
enum class Mood : unsigned char { Calm, Wild, Frenzied };
struct MoodGain {
    float peak{1.0f};  // the sway once a stroke is spent
    float out{1.0f};   // ... while it is out, flying
    float pulse{1.0f}; // the pulse coming home
};
inline MoodGain GainOf(Mood m) {
    switch (m) {
    case Mood::Wild:
        return {2.0f, 2.0f, 1.2f};
    case Mood::Frenzied:
        return {2.8f, 4.0f, 1.5f};
    default:
        return {};
    }
}
inline const char* MoodName(Mood m) {
    return m == Mood::Wild ? "wild" : m == Mood::Frenzied ? "frenzied" : "calm";
}

// The return (2026-11-09: "from 220-340 ms, variable"): each stroke comes home in a time
// of its own, drawn as it is thrown, from the hand's stop to the tendril back at its rest length.
// It sets the lab's reel (pslow quickening to tin over paccel) and the momentum's half-life, in the
// proportions tests/test_kos_lash.cpp measured to give that time.
inline constexpr float ReturnMinS = 0.22f;
inline constexpr float ReturnMaxS = 0.34f;
struct Return {
    float pslow_k{1.43f}, tin_k{0.86f}, paccel_k{0.71f}, mhold_k{0.43f}; // x the return time

    void Reset() {
        out_ = false;
        time_ = 0.5f * (ReturnMinS + ReturnMaxS);
    }
    // `share`: how far out the tendril is (0 at rest, 1 at full length). A stroke thrown from rest
    // draws its return and rolls its mood; true then.
    bool Step(float share) {
        if (!out_ && share > 0.15f) {
            out_ = true;
            time_ = ReturnMinS + (ReturnMaxS - ReturnMinS) * Draw();
            if (cycle_) {
                mood_ = static_cast<Mood>((static_cast<int>(mood_) + 1) % 3);
            } else {
                const float roll = Draw();
                mood_ = roll < 0.75f ? Mood::Calm : roll < 0.875f ? Mood::Wild : Mood::Frenzied;
            }
            return true;
        }
        if (out_ && share < 0.03f) {
            out_ = false;
        }
        return false;
    }
    float Seconds() const {
        return time_;
    }
    Mood Current() const {
        return mood_;
    }
    // Every stroke the next mood in turn instead of a roll (videos, tuning).
    void Cycle(bool on) {
        cycle_ = on;
    }
    // A return time of its own (tests, tuning).
    void Set(float seconds) {
        time_ = seconds;
    }
    // Where its draws start (the hook seeds it with the clock, so a weapon switch or a load does
    // not repeat the same returns).
    void Seed(unsigned seed) {
        seed_ = seed != 0 ? seed : 0x2545F491u;
    }
    void Apply(CutterLab& l) const {
        // (Measured: these proportions bring it home in 1.2 x - 0.042 s; inverted here.)
        const float t = (time_ + 0.042f) / 1.2f;
        l.pslow = pslow_k * t;
        l.tin = tin_k * t;
        l.paccel = paccel_k * t;
        l.mhold = mhold_k * t;
    }

private:
    float Draw() {
        seed_ = seed_ * 1103515245u + 12345u;
        return static_cast<float>((seed_ >> 8) & 0xFFFFu) / 65535.0f;
    }
    bool out_{false};
    bool cycle_{false};
    Mood mood_{Mood::Frenzied}; // (a cycle starts calm)
    float time_{0.5f * (ReturnMinS + ReturnMaxS)};
    unsigned seed_{0x2545F491u};
};

// The recoil, on the tendril's length: as a stroke turns back in (its extension falling after it
// rose), a spring of its own is kicked inward, so the tendril snaps in and springs back out once or
// twice before it settles. `Offset` (m) goes on top of the length asked for.
struct Recoil {
    float hz{3.0f};    // its own rhythm (the game's: 2-4 a second)
    float zeta{0.35f}; // damping (1-3 springs back before it settles)
    float amp{0.25f};  // m, for a stroke from full length

    void Reset() {
        b_ = v_ = 0.0f;
        rising_ = false;
        last_ = -1.0f;
    }
    // `share`: how far out the tendril is now (0 at rest, 1 at full length). True on the frame a
    // stroke turns back in (the kick).
    bool Step(float dt, float share) {
        if (!(dt > 0.0f) || !std::isfinite(share)) {
            return false;
        }
        if (dt > MaxGapS) {
            // A pause, a load, a weapon switch: start again from rest.
            Reset();
            last_ = share;
            return false;
        }
        bool turned = false;
        if (last_ >= 0.0f) {
            if (share > last_ + 1e-4f) {
                rising_ = true;
            } else if (rising_ && share < last_ - 1e-4f) {
                rising_ = false;
                turned = true;
                v_ -= amp * 2.0f * Pi * hz * std::clamp(last_, 0.0f, 1.0f);
            }
        }
        last_ = share;
        // The spring in steps of at most 1/240 s: one explicit step of a long frame blows it up (a
        // 150 ms hitch threw the tendril a metre past its reach, a review found).
        const float w = 2.0f * Pi * hz;
        const int steps = std::clamp(static_cast<int>(std::ceil(dt * 240.0f)), 1, 64);
        const float h = dt / static_cast<float>(steps);
        for (int i = 0; i < steps; ++i) {
            v_ += (-w * w * b_ - 2.0f * zeta * w * v_) * h;
            b_ += v_ * h;
        }
        b_ = std::clamp(b_, -1.5f * amp, 1.5f * amp);
        return turned;
    }
    // The share the last turn came back from (the sway's kick).
    float Last() const {
        return std::max(last_, 0.0f);
    }
    float Offset() const {
        return b_;
    }
    bool Rising() const {
        return rising_;
    }

private:
    static constexpr float Pi = 3.14159265f;
    static constexpr float MaxGapS = 0.25f;
    float b_{0.0f}, v_{0.0f};
    bool rising_{false};
    float last_{-1.0f};
};

// The pulse (2026-11-09: "when it retracts make it pulsate/wiggle a bit too... organic
// and alive"): as a stroke comes home, swells travel down the tendril toward the hand, a muscle
// drawing itself in; at rest it breathes, barely. A girth factor along the tendril (the hook folds
// it into the bones' scales, as Girth).
struct Pulse {
    float hz{6.0f};         // swells a second, coming home
    float amp{0.28f};       // girth +-28% on a full stroke's return
    float half_s{0.22f};    // ... fading by half every half_s
    float rest_amp{0.04f};  // at rest, +-4%
    float rest_hz{1.1f};

    void Reset() {
        level_ = 0.0f;
        t_ = 0.0;
    }
    // `turned`: a stroke turning back in this frame (Recoil's kick), from `share` out.
    void Step(float dt, bool turned, float share) {
        if (turned) {
            level_ = std::max(level_, std::clamp(share, 0.4f, 1.0f));
        }
        if (dt > 0.0f) {
            level_ *= std::exp2(-dt / std::max(half_s, 0.05f));
            t_ = std::fmod(t_ + dt, 1000.0);
        }
    }
    // The girth factor at `u` along the tendril (0 at the hand .. 1 at the tip): the swell's crest
    // moves toward the hand. `gain`: the stroke's mood's (MoodGain::pulse).
    float At(float u, float gain = 1.0f) const {
        constexpr float TwoPi = 6.2831853f;
        const float t = static_cast<float>(t_);
        return 1.0f + amp * gain * level_ * std::sin(TwoPi * hz * t + 6.0f * u) +
               rest_amp * std::sin(TwoPi * rest_hz * t + 2.0f * u);
    }

private:
    float level_{0.0f};
    double t_{0.0};
};

// The sway's strength: a little at rest (breathing), some while it is out (it writhes as it
// lashes), the most once a stroke is spent, fading back (2026-11-09: more wiggle, alive).
struct Sway {
    float idle_deg{5.0f};  // at the tip, at rest
    float out_deg{8.0f};   // ... added while it is out, by how far
    float peak_deg{24.0f}; // ... just after a stroke
    float half_s{0.45f};   // the stroke's share halves every half_s

    void Reset() {
        level_ = 0.0f;
    }
    // `spent`: a stroke turning back in this frame (Recoil's rising -> falling), from `share` out.
    void Step(float dt, bool spent, float share) {
        if (spent) {
            level_ = std::max(level_, std::clamp(share, 0.4f, 1.0f));
        }
        if (dt > 0.0f) {
            level_ *= std::exp2(-dt / std::max(half_s, 0.05f));
        }
    }
    // Radians at the tip now; `out`: how far out the tendril is (0 at rest, 1 at full length);
    // `gain`: the stroke's mood's.
    float Amplitude(float out = 0.0f, MoodGain gain = {}) const {
        constexpr float Deg = 3.14159265f / 180.0f;
        return (idle_deg + (peak_deg - idle_deg) * level_ * gain.peak +
                out_deg * std::clamp(out, 0.0f, 1.0f) * gain.out) *
               Deg;
    }

private:
    float level_{0.0f};
};

// Form B: a whip on each hand, three tendrils each (the hidden weapon's parts 2 and 3: on the right
// RarmA, B and C, 9 joints each; on the left Finger0 and Finger2, 9 each, and the short Finger1, 3,
// all under Finger0_00, which stays the game's). Each hand is one swing; each tendril its own mind
// (the mock page's, from the game's own clips): how far it reaches swung and on a heavy swing (m,
// the game's R1 / R2), how far its tip trails the arm's line (s), how much it curls (x the lab's),
// which side it fans to (rad round the line), its recoil's rhythm (Hz) and size (m), its sway's
// phase.
struct TendrilLook {
    float reach, heavy, delay, curl, side, hz, amp, phase;
};
inline constexpr std::array<TendrilLook, 3> RightLooks{{
    {2.15f, 3.00f, 0.10f, 1.00f, 0.0f, 2.3f, 0.30f, 0.4f},
    {1.90f, 2.80f, 0.14f, 1.25f, 2.1f, 2.9f, 0.12f, 2.3f},
    {2.00f, 2.70f, 0.18f, 0.80f, 4.2f, 3.6f, 0.20f, 4.1f},
}};
inline constexpr std::array<TendrilLook, 3> LeftLooks{{
    {2.00f, 2.75f, 0.11f, 1.10f, 0.6f, 2.5f, 0.28f, 1.1f},
    {1.85f, 2.60f, 0.15f, 0.90f, 2.8f, 3.1f, 0.16f, 3.0f},
    {1.00f, 1.30f, 0.06f, 1.30f, 4.9f, 4.2f, 0.08f, 5.2f}, // the short one (3 joints)
}};
inline constexpr float FanRad = 14.0f * 3.14159265f / 180.0f; // a tendril's spread off the line

// Form B's strokes (2026-11-09: "make them curve a bit more during the attack, and delay
// a bit the return"): each tendril's trail behind the arm's line this much longer (its curl), and
// the hand's swing held this long (s) once it slows before its tendrils start back.
inline constexpr float BundleCurlK = 1.4f;
inline constexpr float BundleHoldS = 0.08f;

// A form B tendril's own writhe (2026-11-09: "add independent wiggles for each
// tentacle"): its sway on a clock of its own, at its own tempo (its look's hz against the bundle's
// 3, times one drawn each stroke) and strength (drawn too, and stronger still while it flies out),
// so the three never move in step. A new tempo eases in: the clock never jumps.
class Wiggle {
public:
    static constexpr float BaseHz = 3.0f;
    static constexpr float RateMin = 0.75f, RateMax = 1.4f;
    static constexpr float GainMin = 0.85f, GainMax = 1.35f;
    static constexpr float FlightBoost = 0.5f; // flying out (out = 1): this much more again
    static constexpr float EaseS = 0.15f;

    void Seed(unsigned seed) {
        seed_ = seed != 0 ? seed : 0x7F4A7C15u;
    }
    // A new stroke: its tempo and strength drawn again.
    void Stroke() {
        goal_ = RateMin + (RateMax - RateMin) * Draw();
        gain_ = GainMin + (GainMax - GainMin) * Draw();
    }
    // `hz`: the tendril's own (TendrilLook::hz).
    void Step(float dt, float hz) {
        if (!(dt > 0.0f)) {
            return;
        }
        rate_ += (goal_ - rate_) * (1.0f - std::exp(-dt / EaseS));
        clock_ = std::fmod(clock_ + static_cast<double>(dt * rate_ * hz / BaseHz), 1000.0);
    }
    double Clock() const {
        return clock_;
    }
    float Gain(float out) const {
        return gain_ * (1.0f + FlightBoost * std::clamp(out, 0.0f, 1.0f));
    }
    float Rate() const {
        return rate_;
    }

private:
    float Draw() {
        seed_ = seed_ * 1103515245u + 12345u;
        return static_cast<float>((seed_ >> 8) & 0xFFFFu) / 65535.0f;
    }
    double clock_{0.0};
    float rate_{1.0f};
    float goal_{1.0f};
    float gain_{1.0f};
    unsigned seed_{0x7F4A7C15u};
};

// How far a tendril fans off its hand's line: wide as it sets off, gathered as it cracks out (the
// game's: 6-14 degrees apart at the crack), widest as it comes home (50-120 degrees apart).
// `out`: how far out it is (0..1); `rising`: still paying out.
inline float FanAngle(float out, bool rising) {
    const float e = std::clamp(out, 0.0f, 1.0f);
    return FanRad * (rising ? 1.6f + (0.35f - 1.6f) * e : 2.6f + (0.35f - 2.6f) * e);
}

// A hand's bundle through its strokes: each tendril sets off on its own beat, the longest first and
// the others 0.02-0.08 s after (drawn again every stroke), and pays out so much quicker that they
// all crack together (the game's: onsets 0-3 frames apart, the crack within one).
class Bundle {
public:
    static constexpr int Count = 3;

    void Seed(unsigned seed) {
        seed_ = seed != 0 ? seed : 0x9E3779B9u;
    }
    // `share`: the hand's swing (0 at rest .. 1 full); `longest`: the tendril that leads.
    void Step(float dt, float share, int longest) {
        if (!(dt > 0.0f)) {
            return;
        }
        clock_ += dt;
        head_ = (head_ + 1) % History;
        hist_[head_] = {clock_, share};
        count_ = std::min(count_ + 1, History);
        if (!stroke_ && share > 0.0f) {
            stroke_ = true;
            for (int i = 0; i < Count; ++i) {
                onset_[i] = i == longest ? 0.0f : 0.02f + 0.06f * Draw();
            }
        } else if (stroke_ && share <= 0.0f) {
            stroke_ = false;
        }
    }
    // At rest again after a gap in the drive: its history gone (its draws go on).
    void Reset() {
        hist_ = {};
        head_ = count_ = 0;
        clock_ = 0.0;
        stroke_ = false;
        onset_ = {};
    }
    // Tendril `i`'s share: the hand's, its onset ago.
    float Share(int i) const {
        const double when = clock_ - onset_[std::clamp(i, 0, Count - 1)];
        for (int k = 0; k < count_; ++k) {
            const auto& e = hist_[(head_ - k + History) % History];
            if (e.t <= when + 1e-9) {
                return e.share;
            }
        }
        return count_ > 0 ? hist_[(head_ - count_ + 1 + History) % History].share : 0.0f;
    }
    float Onset(int i) const {
        return onset_[std::clamp(i, 0, Count - 1)];
    }
    // Its payout time, so that it cracks with the others: the lab's `tout` less its onset.
    float PayoutTime(int i, float tout) const {
        return std::max(0.04f, tout - Onset(i));
    }

private:
    static constexpr int History = 64;
    struct Entry {
        double t{0.0};
        float share{0.0f};
    };
    float Draw() {
        seed_ = seed_ * 1103515245u + 12345u;
        return static_cast<float>((seed_ >> 8) & 0xFFFFu) / 65535.0f;
    }
    std::array<Entry, History> hist_{};
    int head_{0}, count_{0};
    double clock_{0.0};
    bool stroke_{false};
    std::array<float, Count> onset_{};
    unsigned seed_{0x9E3779B9u};
};

} // namespace VR::Whip::Kos
