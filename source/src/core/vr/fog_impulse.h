// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <mutex>

#include "common/types.h"

namespace VR::FogImpulse {

using Point = std::array<float, 3>;
struct Puff {
    Point at{};
    float vx{}, vz{}, radius{}, strength{};
    bool gun{};
};

// The short pressure wave travels ahead of the actual muzzle. A downward shot
// can reach a bank beneath the gun. Its reach ends within a metre of the muzzle.
inline Puff Gun(Point at, Point dir, bool cannon) {
    const float reach = cannon ? 1.0f : 0.75f;
    for (int i = 0; i < 3; ++i) at[i] += reach * dir[i];
    return {at, 7.0f * dir[0], 7.0f * dir[2], cannon ? 1.15f : 0.80f, cannon ? 1.0f : 0.85f, true};
}

// Only a nearby, moving attack capsule qualifies. The contact patch is halfway
// through its sweep. A stationary damage volume or a warp has no wake.
inline Puff Sweep(Point a, Point b, Point travel) {
    const float d2 = travel[0] * travel[0] + travel[1] * travel[1] + travel[2] * travel[2];
    if (!(d2 >= 0.06f * 0.06f && d2 <= 1.2f * 1.2f)) return {};
    Point at{};
    for (int i = 0; i < 3; ++i) at[i] = (a[i] + b[i] + travel[i]) * 0.5f;
    return {at, 8.0f * travel[0], 8.0f * travel[2], 0.9f, std::min(0.85f, 0.4f + d2)};
}

// A shot counts in full up to eye height and not at all from just over his head (GunTopM): a gun
// aimed as one aims in VR is at 1.5 m, which the swings' rule (all of it up to 0.8 m, none from
// 1.9 m) left a third of (a headset run of 2026-11-20: of five shots two were low enough to count
// in full; the tester: "Yes, full strength"). A shot into the sky still parts nothing.
inline constexpr float GunTopM = 2.1f;
inline constexpr float GunFadeM = 0.4f;
inline float GroundStrength(const Puff& p, const Point& feet) {
    const float x = p.at[0] - feet[0], z = p.at[2] - feet[2], y = p.at[1] - feet[1];
    const float top = p.gun ? GunTopM : 1.9f, fade = p.gun ? GunFadeM : 1.1f;
    if (!(x * x + z * z <= 36.0f && y >= -0.45f && y < top)) return 0.0f;
    return p.strength * std::min(1.0f, (top - y) / fade);
}

// Render-thread only. Deform the existing body wake towards a fixed point beyond the muzzle.
// On a second shot, start from the current deformation so a burst cannot snap back to the feet.
class GunWake {
public:
    struct Offset { float x{}, z{}, pressure{}; };
    bool Active() const { return start_ns != 0; }
    void Reset() { start_ns = 0; }

    void Fire(s64 now, const Point& feet, const Puff& shot, float strength) {
        from = Sample(now, feet);
        origin = feet;
        // A short push beyond the initial pressure front. Limit the lever arm on unusual gun poses.
        float x = shot.at[0] + 0.05f * shot.vx - feet[0];
        float z = shot.at[2] + 0.05f * shot.vz - feet[2];
        const float d2 = x * x + z * z;
        if (d2 > 1.8f * 1.8f) {
            const float scale = 1.8f / std::sqrt(d2);
            x *= scale;
            z *= scale;
        }
        target = {x, z};
        gain = strength;
        start_ns = now;
    }

    Offset Sample(s64 now, const Point& feet) {
        if (!Active()) return {};
        const s64 age = now - start_ns;
        const float dx = feet[0] - origin[0], dy = feet[1] - origin[1], dz = feet[2] - origin[2];
        const float nearby = 1.0f - (dx*dx + dy*dy + dz*dz) / 2.25f;
        if (age < 0 || age >= Life || !(nearby > 0.0f)) {
            Reset();
            return {};
        }
        const bool opening = age < Rise;
        const float t = opening ? float(age) / float(Rise) : float(Life - age) / float(Life - Rise);
        const float blend = t*t*(3.0f - 2.0f*t);
        const float pressure = blend * gain * nearby;
        const float carry = opening ? (1.0f - blend) * nearby : 0.0f;
        return {carry * from.x + pressure * 0.72f * (target[0] - dx),
                carry * from.z + pressure * 0.72f * (target[1] - dz),
                carry * from.pressure + pressure};
    }
private:
    static constexpr s64 Rise = 80'000'000, Life = 900'000'000;
    s64 start_ns{};
    Point origin{};
    std::array<float, 2> target{};
    Offset from{};
    float gain{};
};

// The blast: what a shot or a swing does to the fog round it, for the eye (2026-11-20:
// "make the fog react to the gun firing a bit more violently ... it spreads apart more and then
// closes in again ... same for swings ... a bit more cinematic (be careful to keep it
// lightweight)"). A round hole that opens fast from a third of its width to all of it (GrowS),
// its fog piled in a ring round it, the wisps flung outward and carried a little along the shot
// or the swing; held a moment, then it closes the way fog does, from its rim: first the hole
// narrows (the ring and the flung wisps roll back in, its heart still open), then what is left of
// it fills (BlastFillFrom). Its outline is not a circle (the shader bends it by the wisps' waves).
// (And its drama: BlastWave*, BlastTwistShare, BlastSlosh below.)
// All of it a function of its age alone (no state on the GPU, the same at any frame rate): the
// world volume's pass multiplies what stays of a column's fog by it (vr_fog_grid.comp,
// WORLD_VOLUME), over the stir field's own slow clearing, and only while one is live.
// The cost (2026-11-20: "remember it needs to be hyper-optimized and fast"): no pass,
// picture or memory of its own; with none live the shader's loop runs no round; with one live a
// column four radii or more away pays a subtraction, a dot and a compare, a nearer one about ten
// multiplies and one division (no exp, sqrt or pow). What changes with the time is worked out
// here once a frame.
struct BlastNow {
    float x{}, z{}, inv_r2{}, open{};          // its middle now, 1 / its radius^2, the hole's depth
    // The wisps: carried (m), flung outward (per metre from its middle: the shader's reach is
    // times r / (1 + x^3), most at 0.76 of its radius), churned.
    float carry_x{}, carry_z{}, shove{}, churn{};
    // The ring of piled fog running outward (in the shader's x = d^2 / r^2: where it is, 1 / its
    // width, how much thicker the fog is on it) and the wisps' turn round the middle on their way
    // back (per metre from it, as the shove).
    float wave_x{}, wave_k{}, wave_amp{}, twist{};
    // Its long way (a unit way along the ground times sqrt(1 - 1 / stretch^2); none: round), and
    // the height (the world's y) the fog has come back up to.
    float axis_x{}, axis_z{}, front_y{}, pad{};
};
// (sqrt(x) / (1 + x^3) is 0.637 at its most: the shove's metres are reached there.)
inline constexpr float BlastShovePeak = 0.637f;

inline constexpr float BlastRiseS = 0.06f;     // the hole is there within this
inline constexpr float BlastGrowS = 0.13f;     // ... and widens with this time constant
inline constexpr float BlastStartShare = 0.3f; // of its radius, at the start
inline constexpr float BlastHoldShare = 0.2f;  // of its life before it begins to close ...
// ... and never later than this, however long it lives (2026-11-20, of a longer life:
// "it's the actual closing I wanted it to be slower not the delay before it starts closing"): a
// longer life is a longer way back, the hole stands open as long as it did at 3.2 s.
inline constexpr float BlastHoldS = 0.84f; // (0.64 until 2026-11-21: BlastFrontFromS)
inline constexpr float BlastFrontFromS = 1.0f; // (the same for its front: BlastFrontFrom of a short life)
// (0.8 until 2026-11-21, the user: the fog starts to come back "0.98s after", then the same day:
// "make the fog part after 1s ? maybe we can give it some variability between 0.94s - 1.2s";
// BlastHoldS, BlastLifeS and BurstBlastLifeS later with it: the wait is longer, the way back as
// long as before.) Each long blast waits its own BlastWaitS instead of BlastFrontFromS: half of
// them under 1 s, a fifth over 1.1 s, each plus BlastCloseDelayS. Its rim, its front and its life move with it (Add), so its
// way back stays as long. Worked out once as it is added: nothing more a frame.
inline constexpr float BlastWaitMinS = 0.94f;
inline constexpr float BlastWaitMaxS = 1.2f;
// THE SETTING for how long the parted fog stays open: this much longer than the pick above before
// it starts to close in, after a shot, an explosion or a swing (2026-11-26: "make the
// fog start to close in (after it has been disturbed eg from a bullet or explosion by 250ms more,
// it should be just a setting in code)"). Its rim, its front, its heart full and its end all move
// with it (BlastWaitS, Add): the way back keeps its length and speed. 0: the wait of 2026-11-21
// (0.94 to 1.2 s); 0.40: 1.34 to 1.6 s, its middle 1.405 s. (0.25 at first; the user, the same
// day: "ok do 150ms more please".)
inline constexpr float BlastCloseDelayS = 0.40f;
// THE SETTING for how long the closing itself takes: every blast's way back (its rim narrowing,
// its front rising, its heart filling and settling) takes this much longer (2026-11-26:
// "can we make the closing itself a tiny bit slower? (like 100ms more slower)"). It lengthens
// BlastLifeS and BurstBlastLifeS; when the fog starts to close does not move (BlastCloseDelayS),
// its waits are absolute. 0: the closing of 2026-11-21 (a shot's 3.9 s, an explosion's 5.8 s).
inline constexpr float BlastCloseLongerS = 0.1f;
// (The same blast gets the same wait, with no state: u^2 of an even pick, its middle 1.005 s plus
// BlastCloseDelayS.)
inline float BlastWaitS(s64 now, float x, float z) {
    u64 h = static_cast<u64>(now) ^ (u64{std::bit_cast<u32>(x)} << 32) ^ u64{std::bit_cast<u32>(z)};
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    const float u = static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
    return BlastCloseDelayS + BlastWaitMinS + (BlastWaitMaxS - BlastWaitMinS) * u * u;
}
// Its heart begins to fill when this share of the front's rise has passed.
inline constexpr float BlastFillAfter = 0.33f;
inline constexpr float BlastCloseShare = 0.4f; // of its radius lost as it closes
// The way back (2026-11-20: "when it comes back, it needs to appear lower first higher
// after, as if it closes in from the hole that was left. But it must also have a fade thing as it
// appears not just movement so it doesn't feel as too much of a 'pushed away pushed back' ... like
// a triple A game"): the fog comes back from the ground up. A front rises from under the blast's
// ground (BlastFrontFrom of its life) to BlastFrontTopM over it (BlastFillTo of its life); under
// the front the fog is back, over it the hole still is, and across the front (the shader's
// BLAST_FRONT_M) it fades in. The hole's depth fades with it; the hole narrows only a little and
// the wisps turn only a little (the movement is the lesser part).
inline constexpr float BlastFrontFrom = 0.25f;
inline constexpr float BlastFrontTopM = 4.0f;
inline constexpr float BlastFrontSoftM = 1.2f; // host_shaders/vr_fog_grid.comp BLAST_FRONT_M
inline constexpr float BlastFillFrom = 0.45f;  // (of its life: its heart began to fill; BlastFillAfter now)
inline constexpr float BlastOpenMax = 0.92f;   // never a clean hole
// A blast's force (2026-11-20: "strengthen the fog parting of an explosion (and of a
// bullet too, but of an explosion more)"): what its ring and its fling of the wisps are times, and
// from BlastForcefulFrom its hole goes deeper (BlastOpenMaxForceful). A swing's: 1.
inline constexpr float BlastForcefulFrom = 1.3f;
inline constexpr float BlastOpenMaxForceful = 0.97f;
inline constexpr float GunBlastForce = 1.15f;
inline constexpr float BurstBlastForce = 1.5f;
// ... and an explosion's stays longer than BlastLifeS.
inline constexpr float BurstBlastLifeS = 6.8f + BlastCloseLongerS; // (4 s at first; 6.8 until 2026-11-26)
inline constexpr float BlastShoveShare = 0.45f; // of its radius: the wisps flung outward at most
// The drama (2026-11-20: "a bit more dramatic, like as if it parts and fades out, and
// then moves and fades back in ... cool and impressive visually but without compromising
// performance"): the parting throws a ring of piled fog outward, from the hole's edge to
// BlastWaveReach radii in BlastWaveS, fading as it goes; the fog coming back turns
// round the middle (the wisps' twist, most half-way home, none at the end); and as the heart fills
// (by BlastFillTo of its life) it sloshes: a little too thick in the middle, a little thin round
// it, then even. All of it worked out here once a frame: the shader pays one more division.
inline constexpr float BlastWaveS = 0.6f;     // its ring's run, seconds (not a share of its life: a slow way back does not slow the ring)
inline constexpr float BlastWaveFrom = 0.8f;   // radii
inline constexpr float BlastWaveReach = 2.8f;  // radii
inline constexpr float BlastWaveWidth = 0.25f; // radii
inline constexpr float BlastWaveAmp = 0.55f;   // the fog this much thicker on it at its start
inline constexpr float BlastTwistShare = 0.15f; // of its radius: the wisps' turn at most
inline constexpr float BlastFillTo = 0.85f;    // of its life: the heart is full, the slosh begins
inline constexpr float BlastSlosh = 0.15f;     // the heart this much too thick at the slosh's top
inline constexpr float BlastDriftShare = 0.35f; // of its radius: its middle's way along the shot
inline constexpr float BlastCarryShare = 0.2f; // of its radius: the wisps carried along it
// Every blast is closed again this long after it opened (2026-11-20: "make it close at
// 1s", then, of a blast he could not make out: "so maybe it needs to come back a bit more
// slowly?"): it opens as fast as before, the way back takes the rest.
// (3.2 until the evening, the user: "make the closing of the fog a little slower, it's nice now
// but feels too fast": the way back took 1.9 s of it, 0.8 to 2.7 s; now 3.2 s, 0.8 to 4.0 s.)
inline constexpr float BlastLifeS = 4.9f + BlastCloseLongerS; // (4.9 until 2026-11-26) (4.7, then 4.88, until 2026-11-21: BlastFrontFromS) (1.6 before; the user, the same day: "it either happens too fast or it's too abrupt ... it needs to close naturally")
// A shot's: from its puff (a pistol's 0.8 m, a cannon's 1.15): 2.1 m and 3 m wide.
inline constexpr float GunBlastRadius = 2.6f;  // times the puff's radius
// A shot along the ground opens a channel along its way instead (2026-11-20: "YES IF IT
// CAN BE SUPER LIGHTWEIGHT"; the round blast at the muzzle opened only the fog round the hunter,
// which his own wake and the banks' clear distance from the eye had thinned already: nobody saw
// it). As wide as GunChannelWidth puffs (a pistol's 1.1 m to a side, a cannon's 1.55), GunChannelStretch
// times as long from its middle, begun at the puff: a pistol's reaches 10 m, a cannon's 14. By
// how level the shot is: under GunChannelLevel of its way along the ground, the round one.
inline constexpr float GunChannelWidth = 1.5f;   // (1.35 at first; a pistol's 1.2 m to a side now, a cannon's 1.73)
inline constexpr float GunChannelStretch = 5.0f; // (4.5 at first; a pistol's reaches 12 m now, a cannon's 17)
inline constexpr float GunChannelLevel = 0.35f;
struct Lane {
    float x{}, z{}, radius{}, stretch{1.0f};
};
inline Lane GunLane(const Puff& p) {
    // (Gun(): vx and vz are 7 times the shot's way along the ground.)
    const float level = std::sqrt(p.vx * p.vx + p.vz * p.vz) / 7.0f;
    if (!(level >= GunChannelLevel)) return {};
    const float radius = GunChannelWidth * p.radius, half = GunChannelStretch * radius * level;
    const float k = half / (7.0f * level);
    return {p.at[0] + p.vx * k, p.at[2] + p.vz * k, radius, std::max(half / radius, 1.0f)};
}
// An enemy's swing near him, and his own (a hand at SwingBlastFrom of fog_grid.h's HandStrength
// or more, by its speed against his body's; once a SwingBlastGapNs a hand).
inline constexpr float SweepBlastRadiusM = 1.6f;
inline constexpr float SwingBlastRadiusM = 1.3f;
inline constexpr float SwingBlastFrom = 0.45f;
inline constexpr float SwingBlastStrength = 0.75f;
inline constexpr s64 SwingBlastGapNs = 450'000'000;

// Render-thread only.
class Blasts {
public:
    // (Six: a shot every 0.9 s with 4.5 s each and an explosion's 6 s are five or six at once;
    // with four a new one took the place of one still half open, which went in a frame.)
    static constexpr int Max = 6;
    void Reset() { list = {}; }
    // `dir`: the shot's or the swing's way along the ground (any length; none: it stays put).
    // `base_y`: its ground (the world's y: the fog comes back from there up). `stretch`: its length
    // along `dir` over its width (1: round; a shot's channel is longer than wide).
    void Add(s64 now, float x, float z, float dir_x, float dir_z, float radius, float strength, float life_s,
             float base_y = 0.0f, float stretch = 1.0f, float force = 1.0f) {
        if (!(radius > 0.05f && strength > 0.01f && life_s > 0.1f) || !std::isfinite(x + z + dir_x + dir_z + base_y)) return;
        // (A free place, else the one nearest its end: the least of it is left to lose.)
        One* slot = &list[0];
        const auto done = [now](const One& b) { return static_cast<float>(now - b.start) / static_cast<float>(b.life); };
        for (One& b : list) {
            if (!Live(b, now)) {
                slot = &b;
                break;
            }
            if (done(b) > done(*slot)) slot = &b;
        }
        const float len = std::sqrt(dir_x * dir_x + dir_z * dir_z);
        const float k = len > 1e-3f ? 1.0f / len : 0.0f;
        const float long_way = stretch > 1.0f ? std::sqrt(1.0f - 1.0f / (stretch * stretch)) : 0.0f;
        // Its wait: a short life's shares; a long one's own BlastWaitS, all of its way back (its
        // rim, its front, its heart full, its end) as much later with it: the same way back.
        float hold = std::min(BlastHoldShare * life_s, BlastHoldS);
        float front = std::min(BlastFrontFrom * life_s, BlastFrontFromS);
        float full = BlastFillTo * life_s;
        if (BlastFrontFrom * life_s > BlastFrontFromS) {
            const float later = BlastWaitS(now, x, z) - BlastFrontFromS;
            hold += later;
            front += later;
            full += later;
            life_s += later;
        }
        *slot = {x, z, dir_x * k, dir_z * k, radius, std::min(strength, 1.0f), now, static_cast<s64>(life_s * 1e9f),
                 base_y, long_way, std::clamp(force, 0.25f, 2.0f), hold, front, full};
    }
    int Sample(s64 now, std::array<BlastNow, Max>& out) const {
        int n = 0;
        for (const One& b : list) {
            if (!Live(b, now)) continue;
            const float age = static_cast<float>(now - b.start) * 1e-9f, life = static_cast<float>(b.life) * 1e-9f;
            const float rise = Smooth(age / BlastRiseS);
            const float grow = 1.0f - std::exp(-age / BlastGrowS);
            const float hold = b.hold;
            // (`close`: the rim's way back in; `fall`: the heart filling, later.)
            const float close = Smooth((age - hold) / (life - hold));
            const float full = b.full, front_from = b.front_from;
            const float fill = front_from + BlastFillAfter * (full - front_from);
            const float fall = Smooth((age - fill) / (full - fill));
            // (The slosh: the hole's depth below 0 for the last of its life, back to 0 at its end.)
            const float slosh = age > full ? std::sin(3.14159265f * (age - full) / (life - full)) : 0.0f;
            const float level = b.strength * (rise * (1.0f - fall) - BlastSlosh * slosh);
            const float out_now = b.strength * rise * (1.0f - close);
            const float radius = b.radius * (BlastStartShare + (1.0f - BlastStartShare) * grow) * (1.0f - BlastCloseShare * close);
            const float drift = BlastDriftShare * b.radius * grow, carry = BlastCarryShare * b.radius * out_now;
            const float per_m = 1.0f / (BlastShovePeak * radius);
            // The ring: where it is now in the hole's own measure, as wide as BlastWaveWidth radii.
            const float way = std::clamp(age / BlastWaveS, 0.0f, 1.0f);
            const float ring = b.radius * (BlastWaveFrom + (BlastWaveReach - BlastWaveFrom) * (1.0f - (1.0f - way) * (1.0f - way)));
            const float amp = b.strength * b.force * BlastWaveAmp * rise * (1.0f - way) * (1.0f - way);
            if (!(std::abs(level) > 0.004f || amp > 0.004f)) continue;
            // (The turn's way: one blast's left, the next one's right.)
            const float turn = (&b - list.data()) % 2 ? -1.0f : 1.0f;
            out[n++] = {b.x + b.dx * drift, b.z + b.dz * drift, 1.0f / (radius * radius),
                        std::min(level, b.force >= BlastForcefulFrom ? BlastOpenMaxForceful : BlastOpenMax),
                        b.dx * carry, b.dz * carry, b.force * BlastShoveShare * b.radius * out_now * per_m,
                        b.strength * rise * (1.0f - fall * fall),
                        ring * ring / (radius * radius), radius * radius / (2.0f * ring * BlastWaveWidth * b.radius), amp,
                        turn * BlastTwistShare * b.radius * b.strength * 4.0f * close * (1.0f - close) * per_m,
                        b.dx * b.long_way, b.dz * b.long_way,
                        // (The front: far under the ground while the hole stands open.)
                        age < front_from
                            ? b.base_y - 1000.0f
                            : b.base_y - BlastFrontSoftM +
                                  (BlastFrontTopM + BlastFrontSoftM) *
                                      Smooth((age - front_from) / (full - front_from)),
                        0.0f};
        }
        return n;
    }

private:
    struct One {
        float x{}, z{}, dx{}, dz{}, radius{}, strength{};
        s64 start{}, life{};
        float base_y{}, long_way{}, force{1.0f};
        float hold{}, front_from{}, full{}; // s: its rim begins to close, its front to rise, its heart is full
    };
    static bool Live(const One& b, s64 now) { return b.life > 0 && now >= b.start && now - b.start < b.life; }
    static float Smooth(float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
    std::array<One, Max> list{};
};

// Cosmetic events may be dropped. Neither the game thread nor the renderer
// waits for the other, and there is no queue to drain after combat or a load.
class Channel {
public:
    void Listen(s64 now) {
        if (until.load(std::memory_order_relaxed) < now + 250'000'000)
            until.store(now + 500'000'000, std::memory_order_relaxed);
    }
    bool Ready(s64 now) const {
        return now <= until.load(std::memory_order_relaxed) &&
               now - published.load(std::memory_order_relaxed) >= Interval;
    }
    void Publish(s64 now, u32 map, const Puff& p) {
        if (!map || !(p.strength > 0.0f) ||
            !std::isfinite(p.at[0] + p.at[1] + p.at[2] + p.vx + p.vz)) return;
        std::unique_lock lock{mutex, std::try_to_lock};
        if (!lock || !Ready(now)) return;
        puff = p;
        area = map;
        published.store(now, std::memory_order_release);
    }
    bool Take(s64 now, u32 map, s64& seen, Puff& out) {
        const s64 stamp = published.load(std::memory_order_acquire);
        if (stamp == seen || stamp > now) return false;
        if (now - stamp > Fresh) {
            seen = stamp;
            return false;
        }
        std::unique_lock lock{mutex, std::try_to_lock};
        if (!lock || published.load(std::memory_order_relaxed) != stamp) return false;
        seen = stamp;
        if (area != map) return false;
        out = puff;
        return true;
    }

private:
    static constexpr s64 Interval = 160'000'000, Fresh = 120'000'000;
    std::atomic<s64> until{}, published{};
    std::mutex mutex;
    Puff puff{};
    u32 area{};
};
inline Channel combat;

// An explosion (2026-11-20: "if I throw a molotov or shoot a molotov mid air, or fire
// the canon (and it hits and explodes)"): the game launches each as a bullet that stands where it
// is launched with a hit radius of BurstFromM or more (BulletParam: the Molotov's fire 101 / 112,
// 1.4 m; the delayed one's 109 / 119, growing to 2.2 m; the Cannon shell's 701, 1.8 m, which the
// Molotov shot in the air bursts as too). Seen at the game's bullet launch
// (bloodborne_quickitems.cpp LaunchHook), whoever launched it. Its blast is wider than its fire.
// A channel of its own: a shot and what it blows up come within one Interval.
inline Channel bursts;
inline constexpr float BurstFromM = 1.0f;
inline constexpr float BurstStillMPerS = 0.5f; // its launch speed at most (a flying sphere is no blast)
inline Puff Burst(Point at, float hit_radius) {
    // (1.5 x + 1 m, 2.5 to 5 m at first; a Molotov's 4 m now, the delayed one's 5.5, the Cannon shell's 4.7.)
    return {at, 0.0f, 0.0f, std::clamp(1.9f * hit_radius + 1.3f, 3.2f, 6.5f), 1.0f};
}
// Anywhere the world volume reaches round him (its box is 64 m), by its height over his feet: all
// of it from 2.5 m under them (thrown downhill) to 2.5 m over them, none from 4.5 m (a burst high
// in the air leaves the ground fog to its fire, which lands as an explosion of its own).
inline float BurstStrength(const Puff& p, const Point& feet) {
    const float x = p.at[0] - feet[0], z = p.at[2] - feet[2], y = p.at[1] - feet[1];
    if (!(x * x + z * z <= 28.0f * 28.0f && y >= -2.5f && y < 4.5f)) return 0.0f;
    return p.strength * std::min(1.0f, (4.5f - y) / 2.0f);
}
// One blast an explosion: a bullet of the same id within BurstSameM of the last one's place and
// BurstSameNs of the last time it was seen is the same thing still burning (a lingering hazard
// that launches itself again and again blasts once, then not again while it lasts).
inline constexpr float BurstSameM = 3.0f;
inline constexpr s64 BurstSameNs = 1'200'000'000;
class BurstGate {
public:
    bool Pass(s64 now, s32 id, const Point& at) {
        std::unique_lock lock{mutex, std::try_to_lock};
        if (!lock) return false;
        // (The last few it saw: hazards that take turns, two traps side by side, stay known.)
        One* oldest = &seen[0];
        for (One& o : seen) {
            const float x = at[0] - o.at[0], y = at[1] - o.at[1], z = at[2] - o.at[2];
            if (o.id == id && now >= o.ns && now - o.ns < BurstSameNs && x * x + y * y + z * z < BurstSameM * BurstSameM) {
                o.ns = now; // still going
                return false;
            }
            if (o.ns < oldest->ns) oldest = &o;
        }
        *oldest = {at, id, now};
        return true;
    }

private:
    struct One {
        Point at{};
        s32 id{-1};
        s64 ns{};
    };
    std::mutex mutex;
    std::array<One, 4> seen{};
};
inline BurstGate burst_gate;

} // namespace VR::FogImpulse
