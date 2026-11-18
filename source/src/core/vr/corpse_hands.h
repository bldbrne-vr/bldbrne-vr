// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/vr/arm_ik.h"

// Bare hands on dead enemies (2026-11-02, unarmed only: "drag dead enemy rag dolls ...
// with real physics. If we touch them ... have the hands push them around. And if the hands close
// (by pressing grip) while touching the ragdoll, to be able to lift it ... but enemies should [be]
// heavy, and the rest of the rag doll should still have lots of gravity").
//
// A corpse is the game's own ragdoll: rigid parts (capsules or spheres) joined by the game's joints
// and stepped by the game's physics. On each update of a corpse near a bare hand (on that corpse's
// own thread, bloodborne_corpse.cpp) these rules turn the hand into impulses on its parts:
// - Push: a hand moving into a part shoves it along, at the point it touches (the part turns too),
//   with at most push_n of strength; a hand resting on a part leaves it be, one sunk into it eases
//   it out.
// - Hold: a squeeze while touching takes the part at the point under the palm. Each update that
//   point is pulled toward where the hand now holds it, at that point (a limb hangs from your grip):
//   its own velocity set to follow the hand, plus a pull that builds while the point stays behind
//   (the weight the game's joints hang on it), together at most hold_n per hand. A light corpse
//   lifts; a big one only drags, or lifts by a limb while the rest stays down. Too heavy to follow
//   is no reason to let go (the drawn hand stays on it: bloodborne_corpse.h CorpseGrip); only a
//   runaway gap (slip_m for slip_s: run off with it, or it caught on something) slips out.
// - Drag and tow: a hand straining with the corpse behind it along the level (drag_from of its
//   strength or more; lifted or on the ground) also slides the whole corpse that way, with up to
//   drag_g of its own weight (at most drag_n) spread over its parts by their mass: the floor's grip
//   eased, never lifting it. Any corpse then comes along, dragged or carried as you walk, a big one
//   slowly; a small one that follows the hand anyway gets none of it. The user, 2026-11-02 (the
//   headset): "make our hands stronger ... lift a bit heavier enemies or a bit more higher ... I
//   lift an enemy, I am holding it and then I try to move - it let goes". The user, 2026-11-02: "not super floaty not
//   super heavy ... big enemies should [be] heavier than smaller ones, but maybe we can still lift
//   like their arm, or a leg, or slowly drag them".
// - A closing hand (the squeeze between push_open and grab_on, and grab_window_s after it passes
//   grab_on) shoves nothing: what it is about to take stays under the fingers.
// - The rest of a held corpse falls with extra_g more gravity (and for heavy_after_s after it is
//   let go): it hangs and drops heavily, never floating.
// - Let go: the corpse keeps its motion, its parts at most throw_max.
// Pure math. Units are the game's (metres, kg, s); the hand's sizes scale with the world scale.
namespace VR::Corpse {

using Vec3 = VR::Body::Vec3;
using Quat = VR::Body::Quat;

constexpr int MaxParts = 64;

// Input may run between corpse updates. Expire contact hints promptly; a claimed
// squeeze itself is latched by the item controller until physical release.
[[nodiscard]] inline bool GrabHintFresh(std::int64_t seen, std::int64_t now) {
    return seen > 0 && now >= seen && now - seen <= 100'000'000;
}

// PlayerGameData stores Strength in the range 1..99. Missing or invalid stats use the base grip.
constexpr float HoldForce(int strength) {
    constexpr float base = 720.0f;
    if (strength < 1 || strength > 99) return base;
    if (strength >= 40) return 1380.0f + 20.0f * (std::min(strength, 42) - 40);
    const float progress = (std::max(strength, 10) - 10) / 30.0f;
    // Half linear, half quadratic: each early level helps, later ones add more.
    return base + 330.0f * progress * (1.0f + progress);
}

struct Config {
    float hand_r{0.05f};       // the palm's sphere (m; the solid weapons' bare hand)
    float grab_slack{0.04f};   // a squeeze this far off a part still takes it (m)
    float grab_on{0.6f};       // the squeeze that takes hold ...
    float grab_off{0.35f};     // ... and the one that lets go
    float grab_window_s{0.3f}; // a squeeze this long before the touch still takes hold
    float push_n{300.0f};      // an open hand's strength against a corpse (N)
    float push_open{0.2f};     // ... a hand squeezed more than this is closing (no shove)
    float push_from{0.1f};     // moving into a part slower than this: no shove (m/s)
    float sink_free{0.03f};    // a palm deeper in a part than this eases it out (m) ...
    float sink_s{0.1f};        // ... in about this long (s) ...
    float sink_speed{0.5f};    // ... at most this fast (m/s)
    float friction{0.3f};      // a shove's drag along the part, at most this share of it
    float hold_n{HoldForce(0)}; // maximum force each hand can apply while holding a corpse (N)
    float hold_tau{0.06f};     // a held point closes its gap in about this (s)
    float hold_catch{3.0f};    // ... at most this fast (m/s)
    float build_from{0.03f};   // the pull builds while the point is more than this behind (m),
    float build_m{0.1f};       // reaching hold_n with it this much further behind (m) ...
    float build_s{0.3f};       // ... in this long (s)
    float settle_s{1.5f};      // and lets go of it over this once the point is back (s)
    float slip_m{1.2f};        // the controller this far from the held point (m) ...
    float slip_s{0.5f};        // ... this long: it slips out (never from weight alone)
    float drag_g{0.6f};        // a straining pull's slide of the whole corpse: this share of its
    float drag_n{1600.0f};     // weight, at most this (N) ...
    float drag_from{0.6f};     // ... from this share of the hand's strength (full at all of it)
    float extra_g{0.5f};       // the rest of a held corpse: this much more gravity (x g)
    float heavy_after_s{1.0f}; // ... and for this long after it is let go (s)
    float throw_max{7.0f};     // a let-go corpse's parts at most this fast (m/s)
    float gravity{9.81f};      // the world's (m/s^2, down -y)
};

// One rigid part of the corpse as the game's physics has it now (world axes).
struct Part {
    Vec3 a{}, b{};          // its capsule's ends (a sphere: a == b) ...
    float r{0.0f};          // ... and radius
    Vec3 com{};             // its centre of mass
    Quat q{};               // its motion frame (the principal axes)
    Vec3 inv_inertia{};     // in that frame (1 / kg m^2)
    float inv_mass{0.0f};   // 0: not dynamic (never moved)
    Vec3 v{};               // linear velocity
    Vec3 w{};               // angular velocity (world axes)
};

struct Hand {
    bool bare{false}; // unarmed, tracked and yours (not the game's animation)
    Vec3 p{};         // the palm (world)
    Quat q{};
    Vec3 v{};         // its velocity (world: the hunter's own motion included)
    float squeeze{0.0f};
};

// An impulse on a part: at `at` (it turns the part), or at its centre of mass (`point` false).
struct Impulse {
    int part{-1};
    Vec3 j{};
    Vec3 at{};
    bool point{false};
};

struct Hold {
    bool on{false};
    int part{-1};
    Vec3 local{};      // the held point in the part's motion frame
    Vec3 grip{};       // ... from the palm, in the hand's frame
    Vec3 pull{};       // the pull built up for the weight hanging on it (N)
    Vec3 drag{};       // the last update's slide of the whole corpse (N, level, at drag_n; Drag
                       // scales it to the corpse's weight)
    float far_s{0.0f}; // how long the hand has been more than slip_m from it
    float load{0.0f};  // the share of hold_n the last update used (1: all of it)
};

enum class HoldStep : unsigned char { Held, Slipped, Lost };

[[nodiscard]] inline Vec3 ClosestOnSegment(Vec3 a, Vec3 b, Vec3 p) {
    const Vec3 ab = b - a;
    const float l2 = VR::Body::LengthSq(ab);
    const float t =
        l2 > 1e-12f ? std::clamp(VR::Body::Dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    return a + ab * t;
}

// A palm of radius hand_r at p against the part: gap < 0 is how deep they overlap; n points from
// the part toward the palm; at is the part's surface under the palm (the palm's centre when that
// is inside it).
struct Touch {
    float gap{0.0f};
    Vec3 n{0.0f, 1.0f, 0.0f};
    Vec3 at{};
};
[[nodiscard]] inline Touch TouchOf(const Part& part, Vec3 p, float hand_r) {
    const Vec3 c = ClosestOnSegment(part.a, part.b, p);
    const Vec3 d = p - c;
    const float len = VR::Body::Length(d);
    Touch t{};
    if (len > 1e-6f) {
        t.n = d * (1.0f / len);
    }
    t.gap = len - part.r - hand_r;
    t.at = c + t.n * std::min(len, part.r);
    return t;
}

[[nodiscard]] inline bool Dynamic(const Part& p) {
    return p.inv_mass > 0.0f && std::isfinite(p.inv_mass);
}

[[nodiscard]] inline Vec3 PointVelocity(const Part& p, Vec3 at) {
    return p.v + VR::Body::Cross(p.w, at - p.com);
}

// The world inverse inertia times u.
[[nodiscard]] inline Vec3 InvInertia(const Part& p, Vec3 u) {
    namespace B = VR::Body;
    return B::Rotate(p.q, B::Mul(p.inv_inertia, B::Rotate(B::Conjugate(p.q), u)));
}

// The velocity change at r (from the centre of mass) per impulse j there.
[[nodiscard]] inline Vec3 Mobility(const Part& p, Vec3 r, Vec3 j) {
    return j * p.inv_mass + VR::Body::Cross(InvInertia(p, VR::Body::Cross(r, j)), r);
}

// The impulse at r that changes that point's velocity by dv (the part alone).
[[nodiscard]] inline Vec3 ImpulseFor(const Part& p, Vec3 r, Vec3 dv) {
    namespace B = VR::Body;
    const Vec3 c0 = Mobility(p, r, {1.0f, 0.0f, 0.0f});
    const Vec3 c1 = Mobility(p, r, {0.0f, 1.0f, 0.0f});
    const Vec3 c2 = Mobility(p, r, {0.0f, 0.0f, 1.0f});
    const Vec3 c12 = B::Cross(c1, c2);
    const float det = B::Dot(c0, c12);
    if (!(std::abs(det) > 1e-20f) || !std::isfinite(det)) {
        return dv * (1.0f / p.inv_mass);
    }
    const float inv = 1.0f / det;
    return Vec3{B::Dot(dv, c12), B::Dot(c0, B::Cross(dv, c2)), B::Dot(c0, B::Cross(c1, dv))} *
           inv;
}

// The impulse along the unit n at r that changes that point's speed along n by dv.
[[nodiscard]] inline float ImpulseAlong(const Part& p, Vec3 r, Vec3 n, float dv) {
    const float k = VR::Body::Dot(n, Mobility(p, r, n));
    return k > 1e-9f && std::isfinite(k) ? dv / k : 0.0f;
}

[[nodiscard]] inline Vec3 ClampLength(Vec3 v, float max) {
    const float len = VR::Body::Length(v);
    return len > max && len > 0.0f ? v * (max / len) : v;
}

// The shoves of a hand on the parts it touches, appended to out[written..room). Returns how many
// parts it touches; `fastest` its fastest approach among them (m/s, for the touch's tick).
// `closing`: the hand is closing to take hold (no shove, the touch still counted).
inline int Push(const Config& cfg, float scale, float dt, const Hand& hand, const Part* parts,
                int n, Impulse* out, int room, int& written, float& fastest,
                bool closing = false) {
    namespace B = VR::Body;
    fastest = 0.0f;
    if (!hand.bare || dt <= 0.0f || parts == nullptr) {
        return 0;
    }
    const float hand_r = cfg.hand_r * scale;
    const float sink_free = cfg.sink_free * scale;
    const int first = written;
    int touching = 0;
    float total = 0.0f;
    for (int i = 0; i < n && written < room; ++i) {
        const Part& part = parts[i];
        if (!Dynamic(part)) {
            continue;
        }
        const Touch t = TouchOf(part, hand.p, hand_r);
        if (!(t.gap < 0.0f)) {
            continue;
        }
        ++touching;
        const Vec3 r = t.at - part.com;
        const Vec3 rel = hand.v - PointVelocity(part, t.at);
        const float approach = -B::Dot(rel, t.n); // the palm moving into the part
        fastest = std::max(fastest, approach);
        if (closing) {
            continue;
        }
        float dv = approach > cfg.push_from ? approach : 0.0f;
        const float sink = -t.gap;
        if (sink > sink_free) {
            dv = std::max(dv, std::min((sink - sink_free) / std::max(cfg.sink_s, 1e-3f),
                                       cfg.sink_speed));
        }
        if (!(dv > 0.0f)) {
            continue;
        }
        const Vec3 into = -t.n;
        const float jn = ImpulseAlong(part, r, into, dv);
        if (!(jn > 0.0f)) {
            continue;
        }
        Vec3 j = into * jn;
        // Drag along the part: the palm's sideways motion, as friction (at most `friction` of the
        // shove).
        const Vec3 side = B::Perp(rel, t.n);
        const float side_speed = B::Length(side);
        if (side_speed > 1e-4f && cfg.friction > 0.0f) {
            const Vec3 dir = side * (1.0f / side_speed);
            const float jt = std::min(ImpulseAlong(part, r, dir, side_speed), cfg.friction * jn);
            if (jt > 0.0f) {
                j += dir * jt;
            }
        }
        if (!B::IsFinite(j)) {
            continue;
        }
        out[written++] = {i, j, t.at, true};
        total += B::Length(j);
    }
    // The hand's strength over everything it shoves.
    const float cap = cfg.push_n * dt;
    if (total > cap && total > 0.0f) {
        const float k = cap / total;
        for (int i = first; i < written; ++i) {
            out[i].j = out[i].j * k;
        }
    }
    return touching;
}

// A squeeze: take the part nearest the palm within grab_slack of it (dynamic ones only). False:
// none near.
inline bool TryGrab(const Config& cfg, float scale, const Hand& hand, const Part* parts, int n,
                    Hold& h) {
    namespace B = VR::Body;
    if (!hand.bare || parts == nullptr) {
        return false;
    }
    const float hand_r = cfg.hand_r * scale;
    const float slack = cfg.grab_slack * scale;
    int best = -1;
    Touch bt{};
    for (int i = 0; i < n; ++i) {
        if (!Dynamic(parts[i])) {
            continue;
        }
        const Touch t = TouchOf(parts[i], hand.p, hand_r);
        if (t.gap <= slack && (best < 0 || t.gap < bt.gap)) {
            best = i;
            bt = t;
        }
    }
    if (best < 0) {
        return false;
    }
    const Part& part = parts[best];
    h = {};
    h.on = true;
    h.part = best;
    h.local = B::Rotate(B::Conjugate(part.q), bt.at - part.com);
    h.grip = B::Rotate(B::Conjugate(hand.q), bt.at - hand.p);
    return true;
}

// Where the held point is now, and where the hand holds it.
[[nodiscard]] inline Vec3 HeldPoint(const Part& part, const Hold& h) {
    return part.com + VR::Body::Rotate(part.q, h.local);
}
[[nodiscard]] inline Vec3 GripPoint(const Hand& hand, const Hold& h) {
    return hand.p + VR::Body::Rotate(hand.q, h.grip);
}

// One update of a held part: its impulse (at the held point) in `out`. Slipped: the hand stayed
// too far ahead; Lost: the part is gone or no longer dynamic (both let go: h is cleared).
inline HoldStep StepHold(const Config& cfg, float scale, float dt, const Hand& hand,
                         const Part* parts, int n, Hold& h, Impulse& out) {
    namespace B = VR::Body;
    out = {};
    if (!h.on || parts == nullptr || h.part < 0 || h.part >= n || !Dynamic(parts[h.part])) {
        h = {};
        return HoldStep::Lost;
    }
    if (dt <= 0.0f) {
        return HoldStep::Held;
    }
    const Part& part = parts[h.part];
    const Vec3 at = HeldPoint(part, h);
    const Vec3 to = GripPoint(hand, h);
    const Vec3 gap = to - at;
    const float dist = B::Length(gap);
    h.far_s = dist > cfg.slip_m * scale ? h.far_s + dt : 0.0f;
    if (h.far_s > cfg.slip_s) {
        h = {};
        return HoldStep::Slipped;
    }
    const Vec3 r = at - part.com;
    // Follow the hand (and carry the part's own weight) ...
    const Vec3 want = hand.v + ClampLength(gap * (1.0f / std::max(cfg.hold_tau, 1e-3f)),
                                           cfg.hold_catch);
    Vec3 dv = want - PointVelocity(part, at);
    dv.y += cfg.gravity * dt;
    Vec3 j = ImpulseFor(part, r, dv);
    // ... and pull for what hangs on it: it builds while the point stays behind and settles once it
    // is back.
    const float build = cfg.hold_n / std::max(cfg.build_m * scale * cfg.build_s, 1e-6f);
    const float from = cfg.build_from * scale;
    const Vec3 behind = dist > from ? gap * (1.0f - from / dist) : Vec3{};
    h.pull = h.pull * std::max(0.0f, 1.0f - dt / std::max(cfg.settle_s, 1e-3f)) +
             behind * (build * dt);
    h.pull = ClampLength(h.pull, cfg.hold_n);
    j += h.pull * dt;
    const float cap = cfg.hold_n * dt;
    const float len = B::Length(j);
    h.load = cap > 0.0f ? std::min(len / cap, 1.0f) : 0.0f;
    if (len > cap) {
        j = j * (cap / len);
    }
    // Straining with it behind along the level (dragged, or carried as the hunter walks): the whole
    // corpse slides that way (Drag); straight up (a lift) has no level part, no slide.
    const Vec3 level{gap.x, 0.0f, gap.z};
    const float level_len = B::Length(level);
    const float level_from = cfg.build_from * scale;
    const float strain =
        B::Smoothstep(cfg.drag_from, 1.0f, h.load) *
        std::clamp((level_len - level_from) / std::max(cfg.build_m * scale, 1e-6f), 0.0f, 1.0f);
    h.drag = level_len > 1e-6f ? level * (cfg.drag_n * strain / level_len) : Vec3{};
    if (!B::IsFinite(j) || !B::IsFinite(h.drag)) {
        h.pull = {};
        h.drag = {};
        return HoldStep::Held;
    }
    out = {h.part, j, at, true};
    return HoldStep::Held;
}

// The extra gravity on a held (or just let go) corpse's other parts: appended to out[written..room).
// `held` marks the parts a hand holds (at most two; -1 none).
inline void Heavier(const Config& cfg, float dt, const Part* parts, int n, int held_a, int held_b,
                    Impulse* out, int room, int& written) {
    if (cfg.extra_g <= 0.0f || dt <= 0.0f || parts == nullptr) {
        return;
    }
    for (int i = 0; i < n && written < room; ++i) {
        if (i == held_a || i == held_b || !Dynamic(parts[i])) {
            continue;
        }
        const float m = 1.0f / parts[i].inv_mass;
        out[written++] = {i, {0.0f, -cfg.extra_g * cfg.gravity * dt * m, 0.0f}, parts[i].com, false};
    }
}

// A straining hand's slide (Hold::drag, both hands' together) over the corpse's dynamic parts by
// their mass, at their centres of mass: drag_g of its weight at full strain, at most drag_n.
// Appended to out[written..room).
inline void Drag(const Config& cfg, float dt, Vec3 strain, const Part* parts, int n, Impulse* out,
                 int room, int& written) {
    namespace B = VR::Body;
    if (dt <= 0.0f || parts == nullptr || !(B::LengthSq(strain) > 0.0f) || !B::IsFinite(strain) ||
        cfg.drag_n <= 0.0f) {
        return;
    }
    float mass = 0.0f;
    for (int i = 0; i < n; ++i) {
        mass += Dynamic(parts[i]) ? 1.0f / parts[i].inv_mass : 0.0f;
    }
    if (!(mass > 0.0f)) {
        return;
    }
    // Hold::drag is at drag_n per full strain: its weight's share of that instead, both hands at
    // most drag_n.
    const float weight = std::min(cfg.drag_g * mass * cfg.gravity, cfg.drag_n);
    const Vec3 force = ClampLength(strain * (weight / cfg.drag_n), cfg.drag_n);
    const Vec3 per_kg = force * (dt / mass);
    for (int i = 0; i < n && written < room; ++i) {
        if (Dynamic(parts[i])) {
            out[written++] = {i, per_kg * (1.0f / parts[i].inv_mass), parts[i].com, false};
        }
    }
}

// A let-go corpse: its parts slowed to throw_max (appended to out[written..room)).
inline void Calm(const Config& cfg, const Part* parts, int n, Impulse* out, int room,
                 int& written) {
    namespace B = VR::Body;
    if (parts == nullptr) {
        return;
    }
    for (int i = 0; i < n && written < room; ++i) {
        const Part& p = parts[i];
        const float speed = B::Length(p.v);
        if (!Dynamic(p) || !(speed > cfg.throw_max)) {
            continue;
        }
        const Vec3 dv = p.v * (cfg.throw_max / speed - 1.0f);
        out[written++] = {i, dv * (1.0f / p.inv_mass), p.com, false};
    }
}

} // namespace VR::Corpse
