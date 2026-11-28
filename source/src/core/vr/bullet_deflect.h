// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "core/vr/arm_ik.h"

// Bullet deflection (vr.ini melee_deflect, core/vr/bloodborne_deflect.cpp): an enemy's bullet that
// meets the right weapon bounces off it and the swing adds its own direction. Only a swing sends it
// back at the shooter: by a chance, the likelier the closer its bounce heads at him (ReturnChance,
// within melee_deflect_return_deg). Any other bounce is drawn a little back toward the line it came
// in on (TowardBack) and kept clear of him (AwayFrom). A bullet sent back is steered at the shooter
// on its way (Steer). The geometry only: the bullet a sphere on its next step, the blade the solid
// weapons' capsules (world, game units) where its last update swept them.
namespace VR::Deflect {

using Body::Vec3;

// A committed stroke already passed the head-relative swing classifier. World-space blade
// speed alone would make walking, a turn or a tracking correction a free swat.
template <class Window>
bool Swatting(const Window& w, std::int64_t now) {
    return w.serial != 0 && w.open && w.committed && !w.aborted && !w.unpaid && !w.powered &&
           now >= w.ns && now - w.ns < 100'000'000 && std::isfinite(w.tip_speed) &&
           w.tip_speed >= 3.0f;
}

constexpr std::int64_t GuardRegenNs = 350'000'000;
// Extra physical reach only for a validated hand-driven swat, never a passive guard (2.5 cm until
// 2026-11-04: the user found swatting "too risky to try"; now 9 cm of reach in all in a swing).
constexpr float SwingAssistM = .04f;

// Only paid impacts grant credit to nearby pellets. The launch and hit windows are anchored,
// never extended by another pellet; separate shooters/types never share a charge. Bounded like
// the runtime's 32 tracked projectiles, with no allocations or work until a passive contact.
class GuardCharges {
public:
    int Cost(std::uint32_t owner, std::int32_t bullet, std::int64_t launch, std::int64_t now,
             int full) const {
        if (full <= 0) {
            return 0;
        }
        for (const auto& c : paid) {
            const auto gap = launch >= c.launch ? launch - c.launch : c.launch - launch;
            if (c.at > 0 && c.owner == owner && c.bullet == bullet && now >= c.at &&
                now - c.at <= 100'000'000 && gap <= 10'000'000) {
                return 0;
            }
        }
        return full;
    }
    void Paid(std::uint32_t owner, std::int32_t bullet, std::int64_t launch, std::int64_t now) {
        paid[next] = {owner, bullet, launch, now};
        next = (next + 1) % paid.size();
    }

private:
    struct Charge {
        std::uint32_t owner{};
        std::int32_t bullet{};
        std::int64_t launch{}, at{};
    };
    std::array<Charge, 32> paid{};
    std::size_t next{};
};

inline bool SpendGuard(int& stamina, int maximum, int cost) {
    if (cost <= 0) {
        return true;
    }
    if (maximum <= 0 || stamina > maximum || stamina < cost) {
        return false;
    }
    stamina -= cost;
    return true;
}

constexpr int MaxCapsules = 8;

struct Capsule {
    Vec3 a{};
    Vec3 b{};
    float r{0.0f};
};

// The right weapon's capsules (the hand's sphere first), all in the same order every update.
struct Blade {
    std::array<Capsule, MaxCapsules> caps{};
    int n{0};
};

struct Contact {
    bool hit{false};
    float s{0.0f};    // along the bullet's step (0 .. 1) where it first touches
    Vec3 point{};     // the bullet's centre there
    Vec3 normal{};    // out of the blade toward the bullet (unit)
    Vec3 blade_v{};   // the blade's own velocity there (units / s)
};

// The parameters of the closest points of segments a0-a1 and b0-b1 (0 .. 1 each).
inline void Closest(Vec3 a0, Vec3 a1, Vec3 b0, Vec3 b1, float& s, float& t) {
    const Vec3 d1 = a1 - a0;
    const Vec3 d2 = b1 - b0;
    const Vec3 r = a0 - b0;
    const float a = Body::Dot(d1, d1);
    const float e = Body::Dot(d2, d2);
    const float f = Body::Dot(d2, r);
    s = 0.0f;
    t = 0.0f;
    if (a <= 1e-12f && e <= 1e-12f) {
        return;
    }
    if (a <= 1e-12f) {
        t = std::clamp(f / e, 0.0f, 1.0f);
        return;
    }
    const float c = Body::Dot(d1, r);
    if (e <= 1e-12f) {
        s = std::clamp(-c / a, 0.0f, 1.0f);
        return;
    }
    const float b = Body::Dot(d1, d2);
    const float denom = a * e - b * b;
    s = denom > 1e-12f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
    t = (b * s + f) / e;
    if (t < 0.0f) {
        t = 0.0f;
        s = std::clamp(-c / a, 0.0f, 1.0f);
    } else if (t > 1.0f) {
        t = 1.0f;
        s = std::clamp((b - c) / a, 0.0f, 1.0f);
    }
}

// The bullet (radius `rb`, `margin` more to catch it) stepping p0 -> p1 against the blade as it
// swept from `was` to `now` over `blade_dt` seconds (Samples poses along that sweep: a fast swing
// through the bullet's path catches it). The earliest touch along the step.
inline Contact Touch(Vec3 p0, Vec3 p1, float rb, float margin, const Blade& was, const Blade& now,
                     float blade_dt) {
    constexpr int Samples = 4;
    Contact best{};
    best.s = 2.0f;
    const Vec3 step = p1 - p0;
    const float len = Body::Length(step);
    const int n = std::min(was.n, now.n);
    for (int k = 0; k < n; ++k) {
        const Capsule& c0 = was.caps[static_cast<std::size_t>(k)];
        const Capsule& c1 = now.caps[static_cast<std::size_t>(k)];
        const float reach = std::max(c1.r, 0.0f) + rb + margin;
        const float reach2 = reach * reach;
        for (int j = 0; j <= Samples; ++j) {
            const float f = static_cast<float>(j) / static_cast<float>(Samples);
            const Vec3 a = Body::Lerp(c0.a, c1.a, f);
            const Vec3 b = Body::Lerp(c0.b, c1.b, f);
            float s = 0.0f;
            float t = 0.0f;
            Closest(p0, p1, a, b, s, t);
            const Vec3 gap = p0 + step * s - Body::Lerp(a, b, t);
            const float d2 = Body::Dot(gap, gap);
            if (!(d2 < reach2)) {
                continue;
            }
            // Back from the closest point to where the sphere first reaches the capsule.
            const float back = len > 1e-6f ? std::sqrt(reach2 - d2) / len : 0.0f;
            const float enter = std::max(s - back, 0.0f);
            if (enter >= best.s) {
                continue;
            }
            const Vec3 point = p0 + step * enter;
            float u = 0.0f;
            float v = 0.0f;
            Closest(point, point, a, b, u, v);
            const Vec3 axis = Body::Lerp(a, b, v);
            const Vec3 away = point - axis;
            const Vec3 back_dir = len > 1e-6f ? step * (-1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
            best.hit = true;
            best.s = enter;
            best.point = point;
            best.normal = Body::Normalize(away, back_dir);
            const Vec3 at0 = Body::Lerp(c0.a, c0.b, v);
            const Vec3 at1 = Body::Lerp(c1.a, c1.b, v);
            best.blade_v = blade_dt > 1e-4f ? (at1 - at0) * (1.0f / blade_dt) : Vec3{};
        }
    }
    if (!best.hit) {
        best.s = 0.0f;
    }
    return best;
}

// Where the blade will be after `ahead` more seconds if it keeps the motion it had from `was` to
// `now` (over `blade_dt` seconds): a swing still on its way into the bullet's line meets it on
// the bullet's next step. At most twice the last update's motion.
inline Blade Ahead(const Blade& was, const Blade& now, float blade_dt, float ahead) {
    Blade out = now;
    if (!(blade_dt > 1e-4f) || was.n != now.n) {
        return out;
    }
    const float k = std::clamp(ahead / blade_dt, 0.0f, 2.0f);
    for (int i = 0; i < now.n; ++i) {
        const Capsule& c0 = was.caps[static_cast<std::size_t>(i)];
        Capsule& c = out.caps[static_cast<std::size_t>(i)];
        c.a = c.a + (c.a - c0.a) * k;
        c.b = c.b + (c.b - c0.b) * k;
    }
    return out;
}

// A sphere around every capsule of three blade poses (the sweep behind and the one ahead): Touch
// only samples poses between theirs, so a bullet's step farther than its reach from the sphere
// touches none of them. Built once per update, it turns away every bullet not at the blade.
struct Bound {
    Vec3 c{};
    float r{-1.0f}; // < 0: no capsules
};

inline Bound BoundOf(const Blade& a, const Blade& b, const Blade& c) {
    const Blade* const blades[] = {&a, &b, &c};
    Vec3 lo{};
    Vec3 hi{};
    float cap = 0.0f;
    bool any = false;
    const auto take = [&](Vec3 e) {
        lo = any ? Vec3{std::min(lo.x, e.x), std::min(lo.y, e.y), std::min(lo.z, e.z)} : e;
        hi = any ? Vec3{std::max(hi.x, e.x), std::max(hi.y, e.y), std::max(hi.z, e.z)} : e;
        any = true;
    };
    for (const Blade* blade : blades) {
        for (int i = 0; i < blade->n; ++i) {
            const Capsule& k = blade->caps[static_cast<std::size_t>(i)];
            take(k.a);
            take(k.b);
            cap = std::max(cap, k.r);
        }
    }
    Bound out{};
    if (any) {
        out.c = (lo + hi) * 0.5f;
        out.r = Body::Length(hi - lo) * 0.5f + cap;
    }
    return out;
}

// The step p0 -> p1 comes within `reach` of the bound (no square root).
inline bool Near(const Bound& bound, Vec3 p0, Vec3 p1, float reach) {
    if (bound.r < 0.0f) {
        return false;
    }
    const Vec3 step = p1 - p0;
    const float len2 = Body::Dot(step, step);
    const float t =
        len2 > 1e-12f ? std::clamp(Body::Dot(bound.c - p0, step) / len2, 0.0f, 1.0f) : 0.0f;
    const Vec3 gap = p0 + step * t - bound.c;
    const float r = bound.r + reach;
    return Body::Dot(gap, gap) < r * r;
}

// The earlier of two contacts (either may be none).
inline Contact Earlier(const Contact& a, const Contact& b) {
    if (!a.hit) {
        return b;
    }
    return b.hit && b.s < a.s ? b : a;
}

struct BounceConfig {
    float swing_ref{3.0f}; // m/s of the blade across the bullet that turn it as far as the bounce
    float swing_max{1.5f}; // ... at most this many times the bounce's own weight
    float leave{0.15f};    // the new line leaves the blade's face at least this much (cos)
};

// The new line (unit) of a bullet flying `dir` (unit) off the blade's face `n` (unit) while the
// blade moves `blade_v` (real metres / s): the mirror bounce, turned toward the swing.
inline Vec3 Bounce(Vec3 dir, Vec3 n, Vec3 blade_v, const BounceConfig& c = {}) {
    const float into = Body::Dot(dir, n);
    Vec3 out = into < 0.0f ? dir - n * (2.0f * into) : Body::Normalize(dir + n * 2.0f, n);
    const float speed = Body::Length(blade_v);
    if (speed > 1e-3f && c.swing_ref > 0.0f) {
        const float w = std::clamp(speed / c.swing_ref, 0.0f, c.swing_max);
        out = Body::Normalize(out + blade_v * (w / speed), out);
    }
    const float leaving = Body::Dot(out, n);
    if (leaving < c.leave) {
        out = Body::Normalize(out + n * (c.leave - leaving), n);
    }
    return Body::Normalize(out, n);
}

// A bounce not sent back at the shooter is drawn this share of its angle back toward the line
// the bullet came in on: flatter, back out in front of the hunter, still spread (the user,
// 2026-11-01, after the first headset try: bounces into the ground, the sky and off to the side;
// "halve the bounce's angle ... a bit more softly").
constexpr float BackPull = 0.35f;

// `out` (unit, the bounce) turned `k` of its angle toward `back` (unit, the way the bullet came):
// 0 as it is, 1 straight back.
inline Vec3 TowardBack(Vec3 out, Vec3 back, float k) {
    const float angle = std::acos(std::clamp(Body::Dot(out, back), -1.0f, 1.0f));
    if (!(k > 0.0f) || angle < 1e-4f || angle > 3.1415f) {
        return out; // already straight back, or no plane to turn in
    }
    const float s = std::sin(angle);
    return Body::Normalize(out * (std::sin((1.0f - k) * angle) / s) +
                               back * (std::sin(std::min(k, 1.0f) * angle) / s),
                           out);
}

// The bounce `out` (Bounce's) of a bullet that flew `dir` (unit) at the blade's face `n`, softened
// (BackPull): only a bullet that came into the face, whose way back leaves it as the bounce does
// (so the turn between them leaves it too). One the swing overtook from behind keeps its bounce:
// its way back runs through the blade.
inline Vec3 Soften(Vec3 out, Vec3 dir, Vec3 n) {
    return Body::Dot(dir, n) < 0.0f ? TowardBack(out, dir * -1.0f, BackPull) : out;
}

// A bounce not sent back (a still blade's, or a swing's that missed its chance) stays clear of the
// shooter (2026-11-02: a still blade must not send it back): one heading within `min_deg`
// of `at_him` (unit) is turned away to `min_deg` in the plane of the two (within 5 degrees of
// straight at him: to the side, across the world's up), still leaving the blade's face `n` as
// Bounce's does.
inline Vec3 AwayFrom(Vec3 out, Vec3 at_him, float min_deg, Vec3 n, const BounceConfig& c = {}) {
    const float min_rad = std::min(min_deg, 90.0f) * 0.0174532925f;
    const float along = std::clamp(Body::Dot(out, at_him), -1.0f, 1.0f);
    if (!(min_rad > 0.0f) || along < std::cos(min_rad)) {
        return out; // already clear of him
    }
    Vec3 side = out - at_him * along; // its part across the line at him
    if (Body::Length(side) < 0.0872f) { // sin 5 degrees
        side = Body::Cross({0.0f, 1.0f, 0.0f}, at_him);
    }
    side = Body::Normalize(side, Vec3{1.0f, 0.0f, 0.0f});
    Vec3 turned = at_him * std::cos(min_rad) + side * std::sin(min_rad);
    const float leaving = Body::Dot(turned, n);
    if (leaving < c.leave) {
        turned = turned + n * (c.leave - leaving);
    }
    return Body::Normalize(turned, n);
}

// The aim point a bullet falling `gravity` (units / s^2) must head for to arrive at `target` from
// `from` at `speed` (units / s): raised by its drop over the flight.
inline Vec3 AimFor(Vec3 from, Vec3 target, float speed, float gravity) {
    if (!(speed > 1e-3f) || !(gravity > 0.0f)) {
        return target;
    }
    const float t = Body::Length(target - from) / speed;
    return target + Vec3{0.0f, 0.5f * gravity * t * t, 0.0f};
}

// A swing's return (2026-11-02: "a chance to ricochet the bullet back to the shooter
// ... a specific angle where that happens and if it is within that range then have it be a chance
// based on how accurate the swing was"; the odds theirs): the chance that a swing's bounce heading
// `off_deg` from the shooter goes back at him. The first value applies throughout the snap
// angle. The remaining values are spaced evenly out to the edge of the return window.
constexpr std::array<float, 6> ReturnOdds{0.91f, 0.73f, 0.63f, 0.46f, 0.18f, 0.0f};

inline float ReturnChance(float off_deg, float snap_deg, float window_deg) {
    if (!std::isfinite(off_deg) || !(window_deg > 0.0f) || off_deg > window_deg) {
        return 0.0f;
    }
    if (off_deg <= snap_deg) {
        return ReturnOdds[0];
    }
    const float x = (off_deg - snap_deg) / (window_deg - snap_deg) * 5.0f; // off > snap here
    const int i = std::clamp(static_cast<int>(x), 0, 4);
    const float f = std::clamp(x - static_cast<float>(i), 0.0f, 1.0f);
    const auto at = static_cast<std::size_t>(i);
    return ReturnOdds[at] + (ReturnOdds[at + 1] - ReturnOdds[at]) * f;
}

// A bullet sent back keeps heading at its shooter: its line `dir` (unit) turned toward `want`
// (unit, at him), at most `max_rad` this update (all the way when closer). False: he is more than
// SteerGiveUpDeg off its line (it flew past him, or he got behind it), and `dir` stays.
constexpr float SteerGiveUpDeg = 100.0f;

inline bool Steer(Vec3& dir, Vec3 want, float max_rad) {
    const float c = std::clamp(Body::Dot(dir, want), -1.0f, 1.0f);
    if (c < std::cos(SteerGiveUpDeg * 0.0174532925f)) {
        return false;
    }
    const float angle = std::acos(c);
    dir = angle <= max_rad ? want : TowardBack(dir, want, std::max(max_rad, 0.0f) / angle);
    return true;
}

// The return's roll, one per blast and swing: the pellets of one shot (one shooter, one bullet
// type, launched within 10 ms of each other) that one swing meets all go back or none do, so a
// blunderbuss blast is sent back whole or not at all. Bounded (the newest 16 rolls).
class Rolls {
public:
    // The roll already made for this pellet's blast in this swing (`back`); false: none yet.
    bool Find(std::uint32_t owner, std::int32_t bullet, std::int64_t launch, std::uint32_t swing,
              bool& back) const {
        for (const auto& r : rolls) {
            const auto gap = launch >= r.launch ? launch - r.launch : r.launch - launch;
            if (r.swing != 0 && r.swing == swing && r.owner == owner && r.bullet == bullet &&
                gap <= 10'000'000) {
                back = r.back;
                return true;
            }
        }
        return false;
    }
    void Note(std::uint32_t owner, std::int32_t bullet, std::int64_t launch, std::uint32_t swing,
              bool back) {
        rolls[next] = {owner, bullet, launch, swing, back};
        next = (next + 1) % rolls.size();
    }

private:
    struct Roll {
        std::uint32_t owner{};
        std::int32_t bullet{};
        std::int64_t launch{};
        std::uint32_t swing{}; // 0: none
        bool back{};
    };
    std::array<Roll, 16> rolls{};
    std::size_t next{};
};

} // namespace VR::Deflect
