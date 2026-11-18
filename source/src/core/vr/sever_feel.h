// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

#include "core/vr/sever_mesh.h"
#include "core/vr/sever_policy.h"

// Beast Claw dismemberment, the feel (2026-11-12: "Should feel visceral and very fun
// in VR"). What a sever does to the hand, the piece and the stump, as plain numbers for the live
// adapter: first values for the headset, every one named and tested. The style is the game's:
// heavy and wet, never cartoony (no floaty pieces, no fountains).
//  - The piece is thrown by your slash: a share of the claw tip's velocity, a pop off the stump
//    along the cut, a lift (a head arcs up, a limb drops), clamped so it never falls limp nor
//    rockets away, and always tumbling.
//  - The stump gushes once at the cut, then with the heart's last beats, each weaker.
//  - The claw hand feels the bite, the tear, then the weight leaving it.
//  - The hit-stop is a beat longer than a hit's.
// A charged sever (the trigger held to full) is more violent on every count.
namespace VR::Sever::Feel {

inline constexpr float CarryShare = 0.22f;        // of the claw tip's velocity (an arm ~2 m: the user)
inline constexpr float ChargedCarryShare = 0.3f;
inline constexpr float PopMps = 1.2f;             // off the stump, along the cut
inline constexpr float ChargedPopMps = 1.7f;
inline constexpr float HeadLiftMps = 1.0f;        // a head arcs up first
inline constexpr float LimbLiftMps = 0.8f;
inline constexpr float MinLaunchMps = 2.5f;       // never limp
inline constexpr float MaxLaunchMps = 8.0f;       // never a rocket
inline constexpr float AwayMps = 1.2f;            // a piece leaves the hunter at least this fast
// An arc, never a slam (2026-11-13: "when we chop a limb it should fly and fall"): a
// downward chop's blade would drive the piece straight into the ground. Its upward speed is at
// least this much (a big piece's less, as heavy as it is); the slash's level motion is kept.
inline constexpr float HeadMinUpMps = 3.3f;  // up 0.55 m: a head still arcs above a limb
// A limb flies, never just drops (2026-11-13: "make it fly a bit more, not just drop
// right away"): up about half a metre, an arm in the air about a second.
inline constexpr float LimbMinUpMps = 3.0f;
inline constexpr float TorsoMinUpMps = 1.0f;
// A piece bigger than this (its vertices' RMS distance from its middle, m) is heavy: the slash
// throws it and turns it slower, by HeavySizeM / its size, no less than MinHeavyScale (a beast's
// upper body is not a head).
inline constexpr float HeavySizeM = 0.35f;
inline constexpr float MinHeavyScale = 0.3f;
inline constexpr float SpinPerMps = 2.2f;         // rad/s of tumble per m/s of slash across it
inline constexpr float MinSpinRadS = 3.0f;        // always tumbling
inline constexpr float MaxSpinRadS = 12.5f;       // about two turns a second
inline constexpr float HitstopS = 0.11f;          // a hit's own is 0.06 - 0.12 s
inline constexpr float ChargedHitstopS = 0.14f;

struct Launch {
    Vec3 velocity{}; // m/s, world (up +Y)
    Vec3 spin{};     // rad/s, world axis * rate
};

namespace Detail {
inline Vec3 Add(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 Mul(Vec3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
inline float Length(Vec3 a) {
    return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}
inline Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline bool Finite(Vec3 a) {
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}
inline float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 Sub(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
// v turned by the unit quaternion q (x y z w).
inline Vec3 Rotate(const std::array<float, 4>& q, Vec3 v) {
    const Vec3 u{q[0], q[1], q[2]};
    const Vec3 t = Mul(Cross(u, v), 2.0f);
    return Add(Add(v, Mul(t, q[3])), Cross(u, t));
}
// a then b... as one turn: b * a (b applied after a), unit.
inline std::array<float, 4> Then(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    std::array<float, 4> r{b[3] * a[0] + b[0] * a[3] + b[1] * a[2] - b[2] * a[1],
                           b[3] * a[1] - b[0] * a[2] + b[1] * a[3] + b[2] * a[0],
                           b[3] * a[2] + b[0] * a[1] - b[1] * a[0] + b[2] * a[3],
                           b[3] * a[3] - b[0] * a[0] - b[1] * a[1] - b[2] * a[2]};
    const float n = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    if (!(n > 1e-6f)) {
        return {0, 0, 0, 1};
    }
    for (float& x : r) {
        x /= n;
    }
    return r;
}
inline std::array<float, 4> AxisAngle(Vec3 axis, float angle) {
    const float s = std::sin(0.5f * angle);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(0.5f * angle)};
}
inline Vec3 Clamped(Vec3 v, float lo, float hi, Vec3 fallback) {
    const float len = Length(v);
    if (!Finite(v) || !std::isfinite(len) || len < 1e-6f) {
        return Mul(fallback, lo);
    }
    const float to = std::clamp(len, lo, hi);
    return Mul(v, to / len);
}
} // namespace Detail

// The piece's flight from the cut (its normal, toward the piece: CutPlane's) and the claw tip's
// velocity at the hit (MeleeHitInfo tip_vel). A degenerate input still throws it up off the
// stump, tumbling. `away`: level, from the hunter to the piece (zero: not known). A piece never
// flies at the hunter: its level motion toward them is taken out and it leaves at AwayMps (the
// 2026-11-13 mock: a hunched Scourge Beast's upper body, thrown along its spine, flew into the
// view and lay over it).
inline Launch PieceLaunch(Vec3 cut_normal, Vec3 tip_velocity, Limb limb, bool charged,
                          Vec3 away = {}, float size_m = 0.0f) {
    using namespace Detail;
    const float n_len = Length(cut_normal);
    const Vec3 n = Finite(cut_normal) && n_len > 1e-6f ? Mul(cut_normal, 1.0f / n_len) : Vec3{0, 1, 0};
    const Vec3 tip = Finite(tip_velocity) ? tip_velocity : Vec3{};
    const float carry = charged ? ChargedCarryShare : CarryShare;
    const float pop = charged ? ChargedPopMps : PopMps;
    const float lift = limb == Limb::Head ? HeadLiftMps : LimbLiftMps;
    const float heavy = std::isfinite(size_m) && size_m > HeavySizeM
                            ? std::max(HeavySizeM / size_m, MinHeavyScale)
                            : 1.0f;
    Launch out;
    Vec3 v = Add(Mul(Add(Mul(tip, carry), Mul(n, pop)), heavy), Vec3{0, lift, 0});
    const float level = std::sqrt(away.x * away.x + away.z * away.z);
    if (Finite(away) && level > 1e-3f && Finite(v)) {
        const Vec3 a{away.x / level, 0.0f, away.z / level};
        const float toward = -(v.x * a.x + v.z * a.z);
        if (toward > 0.0f) {
            v = Add(v, Mul(a, toward));
        }
        v = Add(v, Mul(a, AwayMps));
    }
    const float up = (limb == Limb::Head    ? HeadMinUpMps
                      : limb == Limb::Torso ? TorsoMinUpMps
                                            : LimbMinUpMps) *
                     heavy;
    if (Finite(v) && v.y < up) {
        v.y = up;
    }
    // Too fast: its level part slowed, never its rise below that (a head always clears the body).
    if (Finite(v) && Length(v) > MaxLaunchMps) {
        const float rise = std::min(v.y, MaxLaunchMps);
        const float level_v = std::sqrt(v.x * v.x + v.z * v.z);
        const float room = std::sqrt(std::max(MaxLaunchMps * MaxLaunchMps - rise * rise, 0.0f));
        const float k = level_v > 1e-6f ? std::min(1.0f, room / level_v) : 0.0f;
        v = {v.x * k, rise, v.z * k};
    }
    out.velocity = Clamped(v, MinLaunchMps, MaxLaunchMps, Vec3{0, 1, 0});
    // Tumbling about the axis the slash crosses the cut on (a sideways slash flips a head end
    // over end), at a rate from the slash, bounded; with no slash, a slow roll about the cut.
    const Vec3 axis = Cross(n, tip);
    const float across = Length(axis);
    const Vec3 unit = across > 1e-4f ? Mul(axis, 1.0f / across) : Cross(n, Vec3{1, 0, 0});
    const float rate = std::clamp(across * SpinPerMps, MinSpinRadS, MaxSpinRadS) * heavy;
    out.spin = Mul(Detail::Clamped(unit, 1.0f, 1.0f, Vec3{1, 0, 0}), rate);
    return out;
}

// One gush of the stump: delay, duration, relative strength and nominal launch speed.
// The live adapter maps strength and speed into the existing spray's size and motion scale.
struct Gush {
    float delay_s, duration_s, strength, speed_mps;
};
// The cut's burst, then the heart's last beats, each weaker and slower, down to a dribble (the
// user, 2026-11-13: "blood fly out of the hole/fill for a bit": about 4 s in all). A cut-off
// piece bleeds its first PieceGushes of them.
inline constexpr std::size_t PieceGushes = 3;
inline constexpr std::array<Gush, 8> StumpGushes(bool charged) {
    const float k = charged ? 1.25f : 1.0f;
    // Only the initial tear: ~18% more spray scale (~20% charged), with the same particles,
    // lifetime and later beats. The original wound-size clamp still bounds the effect. Keep it
    // big (2026-11-16: "the first cut ... needs to be cool and cinematic").
    return {{{0.0f, 0.18f, 1.45f * k, 3.9f * k},
             {0.45f, 0.24f, 0.72f * k, 2.7f * k},
             {0.95f, 0.24f, 0.54f * k, 2.2f * k},
             {1.5f, 0.26f, 0.41f * k, 1.9f * k},
             {2.1f, 0.26f, 0.31f * k, 1.6f * k},
             {2.7f, 0.28f, 0.23f * k, 1.3f * k},
             {3.3f, 0.3f, 0.16f * k, 1.1f * k},
             {3.9f, 0.3f, 0.1f * k, 0.9f * k}}};
}

// Only skip after the last scheduled burst's particles have expired. Match the render
// loop's subtraction/comparison exactly (adding delay + lifetime rounds differently).
inline bool GushesExpired(float since, bool stump, float particle_lifetime) {
    constexpr auto gushes = StumpGushes(false); // charge changes strength/speed, not timing
    static_assert(PieceGushes > 0 && PieceGushes <= gushes.size());
    const float last_delay = stump ? gushes.back().delay_s : gushes[PieceGushes - 1].delay_s;
    return since - last_delay >= particle_lifetime;
}

// Kept with the cut so the bite and the delayed tear go to the same striking hand(s).
// MeleeEvent::b carries these values as floats. Controller indices are left = 0, right = 1.
enum class StrikeHands : std::uint8_t { Weapon, LeftClaw, TwoHanded };
struct HapticRoute {
    int primary;
    bool support;
};
constexpr HapticRoute RouteHaptics(float hands, bool two_hand_haptics) {
    return {hands == static_cast<float>(StrikeHands::LeftClaw) ? 0 : 1,
            hands == static_cast<float>(StrikeHands::TwoHanded) && two_hand_haptics};
}

// One haptic pulse on the weapon hand (frequency 0: the hand's usual one for its strength).
struct Pulse {
    float delay_s, duration_s, amplitude, frequency_hz;
};
// The cut's feel, in two parts (2026-11-16: the whole of it came about 0.1 s after the
// blow, as a second, late hit). The bite, at the blow itself (SeverKill): a high crack, then the
// hit's own impact (haptic_output.h MeleeImpact: its weight, its crunch, the hit gain), in the
// hit's place: sharper, never weaker. The tear and the weight leaving the hand, as the limb comes
// off (its piece first drawn): after the bite's full blow (the crack and the impact's hold), never
// over a newer hit (openxr_presenter.cpp MeleeEventKind::Sever).
inline constexpr Pulse SeverCrack{0.0f, 0.03f, 1.0f, 320.0f};
inline std::array<Pulse, 2> SeverTear(bool charged) {
    const float k = charged ? 1.0f : 0.85f;
    return {{{0.0f, 0.12f, 0.75f * k, 70.0f}, {0.12f, 0.1f, 0.35f * k, 40.0f}}};
}
// The bite `elapsed_s` into it, its full blow `blow_s` long: how much of the blow is still to come
// before the tear (s).
inline float BiteBlowLeftS(float elapsed_s, float blow_s) {
    if (!std::isfinite(elapsed_s) || !std::isfinite(blow_s) || !(blow_s > 0.0f)) {
        return 0.0f;
    }
    return std::clamp(blow_s - elapsed_s, 0.0f, blow_s);
}

inline float Hitstop(bool charged) {
    return charged ? ChargedHitstopS : HitstopS;
}

// ---- The piece's flight ---------------------------------------------------------------------
// A severed piece is one rigid body: its mesh stays as the cut left it (the kill's pose) and
// moves by one transform (Flight::Transform). Heavy and wet: gravity, a little air, and a ground
// that takes almost all of it (a dull thud, a short slide, a roll that dies), then it rests.
inline constexpr float GravityMps2 = 9.81f;
inline constexpr float AirDrag = 0.05f;      // 1/s of its speed
inline constexpr float Bounce = 0.15f;       // of its speed into the ground, back
inline constexpr float GroundSlip = 0.55f;   // of its speed along the ground, kept at a landing
inline constexpr float GroundSpin = 0.35f;   // of its tumble kept at a landing
inline constexpr float RestMps = 0.15f;      // slower than this on the ground: it rests
inline constexpr float RestRadS = 0.4f;
// The slump (2026-11-13: "the chopped meshes need to ragdoll or at least fall down"):
// a piece come to rest standing on a narrow base (its middle this much higher than its nearest
// broad side would hold it, plus a margin: a hunched upper body on its arms, a leg on its foot)
// tips over that side about its leading bottom edge, faster as it goes, and lands with a thud.
inline constexpr float SlumpRatio = 1.25f;
inline constexpr float SlumpMarginM = 0.05f;
inline constexpr float SlumpMinRad = 0.1f;       // about 6 deg: already near lying, it stays
inline constexpr float SlumpStartRadS = 0.6f;
inline constexpr float SlumpAccelRadS2 = 14.0f;  // a body a metre tall falls in about half a second
inline constexpr float SlumpMaxRadS = 8.0f;

inline constexpr float MaxStepS = 1.0f / 30; // longer frames are stepped in parts
// Its outline (SetShape): its own extreme points (along 26 directions) it lands and lies on, and
// its flattest side, the one it settles onto (2026-11-13: a cut arm lay on its upper arm
// with the forearm and the hand up in the air). The flattest side: of RestDirections directions,
// the one that leaves it lowest (its height above the floor, and a little of its middle's).
inline constexpr std::size_t MaxOutline = 32;
inline constexpr int RestDirections = 128;
inline constexpr float RestMiddleWeight = 0.25f;

struct Flight {
    Vec3 middle{};                     // its centre of mass at the cut (the kill's pose, world)
    float radius{0.1f};                // its reach from the middle (m): it lands on that ...
    // ... or, given (SetBox, SetShape), on the lowest of its outline's points as it is turned now.
    bool boxed{false};
    std::array<Vec3, MaxOutline> points{}; // about the middle, world axes at the cut
    std::uint8_t point_count{0};
    Vec3 box_lo{}, box_hi{};               // its box (SetBox)
    bool has_rest{false};
    Vec3 rest_down{}; // its flattest side's way out of it (unit, world axes at the cut: SetShape)
    Vec3 offset{};                     // how far it has moved
    std::array<float, 4> turn{0, 0, 0, 1}; // how far it has turned (x y z w)
    Vec3 velocity{}, spin{};
    bool resting{false};
    unsigned landings{0};              // thuds so far (each one a sound and a splash)
    // The slump: tipping over now (about `slump_axis`, world, level, toward `slump_dir`), done once.
    bool slumping{false};
    bool slumped{false};
    Vec3 slump_axis{}, slump_dir{};
    float slump_left{0.0f}; // rad still to turn
    float slump_rate{0.0f}; // rad/s
};

// Its box (world axes at the cut, about the middle): it lands on its lowest corner.
inline void SetBox(Flight& f, Vec3 lo, Vec3 hi) {
    if (!Detail::Finite(lo) || !Detail::Finite(hi)) {
        return;
    }
    for (int i = 0; i < 8; ++i) {
        f.points[static_cast<std::size_t>(i)] = {i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y,
                                                 i & 4 ? hi.z : lo.z};
    }
    f.point_count = 8;
    f.box_lo = lo;
    f.box_hi = hi;
    f.boxed = true;
}

// Its own shape (points about its middle, world axes at the cut; a sample of its vertices is
// enough): its outline (the extreme points along 26 directions) and its flattest side. Fewer than
// four finite points change nothing.
inline void SetShape(Flight& f, std::span<const Vec3> shape) {
    using namespace Detail;
    std::size_t finite = 0;
    for (const Vec3& p : shape) {
        finite += Finite(p) ? 1 : 0;
    }
    if (finite < 4) {
        return;
    }
    // The outline: for each of the 26 directions of a cube's faces, edges and corners, its point
    // furthest that way (each kept once).
    std::uint8_t count = 0;
    for (int a = -1; a <= 1; ++a) {
        for (int b = -1; b <= 1; ++b) {
            for (int c = -1; c <= 1; ++c) {
                if (a == 0 && b == 0 && c == 0) {
                    continue;
                }
                const Vec3 d{static_cast<float>(a), static_cast<float>(b), static_cast<float>(c)};
                const Vec3* best = nullptr;
                float furthest = -INFINITY; // (not "far": a Windows macro)
                for (const Vec3& p : shape) {
                    if (Finite(p) && Dot(p, d) > furthest) {
                        furthest = Dot(p, d);
                        best = &p;
                    }
                }
                bool seen = false;
                for (std::uint8_t i = 0; i < count && best != nullptr; ++i) {
                    seen = seen || (f.points[i].x == best->x && f.points[i].y == best->y &&
                                    f.points[i].z == best->z);
                }
                if (best != nullptr && !seen && count < MaxOutline) {
                    f.points[count++] = *best;
                }
            }
        }
    }
    f.point_count = count;
    f.boxed = true;
    // The flattest side: of RestDirections directions (spread over the sphere), the "up" that
    // leaves its points the least height above its lowest, its middle low too.
    float best_score = INFINITY;
    Vec3 best_up{0, 1, 0};
    for (int k = 0; k < RestDirections; ++k) {
        const float y = 1.0f - 2.0f * (static_cast<float>(k) + 0.5f) / RestDirections;
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float phi = 2.39996323f * static_cast<float>(k); // the golden angle
        const Vec3 up{r * std::cos(phi), y, r * std::sin(phi)};
        float lo = INFINITY, hi = -INFINITY;
        for (const Vec3& p : shape) {
            if (!Finite(p)) {
                continue;
            }
            const float h = Dot(p, up);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
        const float score = (hi - lo) + RestMiddleWeight * (-lo);
        if (score < best_score) {
            best_score = score;
            best_up = up;
        }
    }
    // Refined about it (the directions are ~18 deg apart: a long piece "lying flat" stood a few
    // cm too high): small turns each way, halved while none helps.
    const auto score_of = [&](Vec3 up) {
        float lo = INFINITY, hi = -INFINITY;
        for (const Vec3& p : shape) {
            if (Finite(p)) {
                const float h = Dot(p, up);
                lo = std::min(lo, h);
                hi = std::max(hi, h);
            }
        }
        return (hi - lo) + RestMiddleWeight * (-lo);
    };
    const Vec3 seed = std::abs(best_up.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    Vec3 t1 = Cross(best_up, seed);
    t1 = Mul(t1, 1.0f / std::max(Length(t1), 1e-6f));
    Vec3 t2 = Cross(best_up, t1);
    for (float step = 0.16f; step > 0.004f;) {
        bool better = false;
        for (const Vec3& t : {t1, Mul(t1, -1.0f), t2, Mul(t2, -1.0f)}) {
            Vec3 up = Add(best_up, Mul(t, step));
            up = Mul(up, 1.0f / Length(up));
            const float score = score_of(up);
            if (score < best_score) {
                best_score = score;
                best_up = up;
                better = true;
            }
        }
        if (!better) {
            step *= 0.5f;
        }
        // (the tangents follow the turned up)
        t1 = Cross(best_up, seed);
        t1 = Mul(t1, 1.0f / std::max(Length(t1), 1e-6f));
        t2 = Cross(best_up, t1);
    }
    f.rest_down = Mul(best_up, -1.0f);
    f.has_rest = true;
}

inline Flight StartFlight(Vec3 middle, float radius, const Launch& launch) {
    Flight f;
    f.middle = middle;
    f.radius = std::isfinite(radius) && radius > 0.01f ? radius : 0.1f;
    f.velocity = Detail::Finite(launch.velocity) ? launch.velocity : Vec3{};
    f.spin = Detail::Finite(launch.spin) ? launch.spin : Vec3{};
    return f;
}

// Its outline's points about the middle, turned as it is now (world); the first point_count hold.
inline std::array<Vec3, MaxOutline> TurnedPoints(const Flight& f) {
    std::array<Vec3, MaxOutline> out{};
    for (std::size_t i = 0; i < f.point_count; ++i) {
        out[i] = Detail::Rotate(f.turn, f.points[i]);
    }
    return out;
}

// Come to rest: with its shape known (SetShape), it settles onto its flattest side unless it lies
// on it already; else, standing on a narrow base, it slumps onto its box's nearest broad side (the
// face nearest its middle: the lowest it can lie). False: it lies as it is.
inline bool StartSlump(Flight& f) {
    using namespace Detail;
    const Vec3 down{0, -1, 0};
    if (f.has_rest) {
        // Its flattest side has two faces, as thin either way: the one nearer the floor now (the
        // review, 2026-11-13: one lying on the other face stood up on an edge and turned over).
        Vec3 n = Rotate(f.turn, f.rest_down);
        if (Dot(n, down) < 0.0f) {
            n = Mul(n, -1.0f);
        }
        const float angle = std::acos(std::clamp(Dot(n, down), -1.0f, 1.0f));
        if (angle < SlumpMinRad) {
            return false;
        }
        Vec3 axis = Cross(n, down);
        float across = Length(axis);
        if (across < 1e-3f) { // upside down: over any level axis
            axis = {1, 0, 0};
            across = 1.0f;
        }
        axis = Mul(axis, 1.0f / across);
        const float level = std::sqrt(n.x * n.x + n.z * n.z);
        f.slump_axis = axis;
        f.slump_dir = level > 1e-3f ? Vec3{n.x / level, 0.0f, n.z / level} : Cross(axis, down);
        f.slump_left = angle;
        f.slump_rate = SlumpStartRadS;
        return true;
    }
    const std::array<Vec3, MaxOutline> c = TurnedPoints(f);
    float low = 0.0f;
    for (std::size_t i = 0; i < f.point_count; ++i) {
        low = std::min(low, c[i].y);
    }
    const float height = -low; // its middle above the ground
    // The box's faces (world axes at the cut): their outward normals and distances from the middle.
    const Vec3& lo = f.box_lo;
    const Vec3& hi = f.box_hi;
    const std::array<std::pair<Vec3, float>, 6> faces{{{{-1, 0, 0}, -lo.x},
                                                       {{1, 0, 0}, hi.x},
                                                       {{0, -1, 0}, -lo.y},
                                                       {{0, 1, 0}, hi.y},
                                                       {{0, 0, -1}, -lo.z},
                                                       {{0, 0, 1}, hi.z}}};
    std::size_t best = 0;
    for (std::size_t i = 1; i < faces.size(); ++i) {
        if (faces[i].second < faces[best].second) {
            best = i;
        }
    }
    const float lying = std::max(faces[best].second, 0.0f);
    if (!(height > lying * SlumpRatio + SlumpMarginM)) {
        return false;
    }
    // Turn that face's normal down: about the level axis across it.
    const Vec3 n = Rotate(f.turn, faces[best].first);
    const float angle = std::acos(std::clamp(Dot(n, down), -1.0f, 1.0f));
    Vec3 axis = Cross(n, down);
    const float across = Length(axis);
    if (angle < SlumpMinRad || across < 1e-3f) {
        return false; // lying already, or that face straight up (it would roll over itself)
    }
    axis = Mul(axis, 1.0f / across);
    const float level = std::sqrt(n.x * n.x + n.z * n.z);
    f.slump_axis = axis;
    f.slump_dir = level > 1e-3f ? Vec3{n.x / level, 0.0f, n.z / level} : Vec3{1, 0, 0};
    f.slump_left = angle;
    f.slump_rate = SlumpStartRadS;
    return true;
}

// One step of the slump: turned about its leading bottom edge (it keeps its place on the ground),
// then set on the ground. True when it lands (the thud).
inline bool StepSlump(Flight& f, float dt, float ground) {
    using namespace Detail;
    f.slump_rate = std::min(f.slump_rate + SlumpAccelRadS2 * dt, SlumpMaxRadS);
    const float step = std::min(f.slump_rate * dt, f.slump_left);
    f.slump_left -= step;
    // The pivot: of its outline's points near the ground, the one furthest toward the fall.
    const std::array<Vec3, MaxOutline> c = TurnedPoints(f);
    float low = 0.0f;
    for (std::size_t i = 0; i < f.point_count; ++i) {
        low = std::min(low, c[i].y);
    }
    Vec3 pivot = c[0];
    float ahead = -INFINITY;
    for (std::size_t i = 0; i < f.point_count; ++i) {
        if (c[i].y < low + 0.1f && Dot(c[i], f.slump_dir) > ahead) {
            ahead = Dot(c[i], f.slump_dir);
            pivot = c[i];
        }
    }
    // The middle turned about the pivot (both about the middle: the middle at 0).
    const std::array<float, 4> q = AxisAngle(f.slump_axis, step);
    const Vec3 moved = Add(pivot, Rotate(q, Mul(pivot, -1.0f)));
    f.offset = Add(f.offset, moved);
    f.turn = Then(f.turn, q);
    // Set on the ground (never through it).
    float reach = 0.0f;
    const std::array<Vec3, MaxOutline> turned = TurnedPoints(f);
    for (std::size_t i = 0; i < f.point_count; ++i) {
        reach = std::max(reach, -turned[i].y);
    }
    f.offset.y = ground + reach - f.middle.y;
    if (f.slump_left <= 1e-4f) {
        f.slumping = false;
        f.slumped = true;
        f.resting = true;
        ++f.landings;
        return true;
    }
    return false;
}

// One step of `dt` seconds over ground at height `ground` (world y under the piece: the live
// adapter's floor probe; the kill's feet when it has none). True when it landed in this step.
inline bool StepFlight(Flight& f, float dt, float ground) {
    using namespace Detail;
    if (f.slumping && dt > 0.0f && std::isfinite(dt) && std::isfinite(ground)) {
        return StepSlump(f, std::min(dt, 0.05f), ground);
    }
    if (f.resting || !(dt > 0.0f) || !std::isfinite(dt) || !std::isfinite(ground)) {
        return false;
    }
    bool landed = false;
    for (float left = std::min(dt, 0.25f); left > 0.0f; left -= MaxStepS) {
        const float h = std::min(left, MaxStepS);
        f.velocity.y -= GravityMps2 * h;
        f.velocity = Mul(f.velocity, std::max(0.0f, 1.0f - AirDrag * h));
        f.offset = Add(f.offset, Mul(f.velocity, h));
        // The turn: integrate the spin (q += 0.5 * (spin, 0) * q * h), renormalized.
        const auto& q = f.turn;
        const Vec3 w = f.spin;
        std::array<float, 4> dq{0.5f * h * (w.x * q[3] + w.y * q[2] - w.z * q[1]),
                                0.5f * h * (-w.x * q[2] + w.y * q[3] + w.z * q[0]),
                                0.5f * h * (w.x * q[1] - w.y * q[0] + w.z * q[3]),
                                0.5f * h * (-w.x * q[0] - w.y * q[1] - w.z * q[2])};
        float n = 0;
        for (int i = 0; i < 4; ++i) {
            f.turn[i] += dq[i];
            n += f.turn[i] * f.turn[i];
        }
        n = std::sqrt(n);
        for (int i = 0; i < 4; ++i) {
            f.turn[i] = n > 1e-6f ? f.turn[i] / n : (i == 3 ? 1.0f : 0.0f);
        }
        // The ground: its lowest reach at it (its outline's lowest point, turned).
        float reach = f.radius;
        if (f.boxed) {
            reach = 0.0f;
            const float x = f.turn[0], y = f.turn[1], z = f.turn[2], w = f.turn[3];
            for (std::size_t i = 0; i < f.point_count; ++i) {
                const Vec3& c = f.points[i];
                // The turned corner's height: row 1 of the rotation.
                const float cy = 2 * (x * y + w * z) * c.x + (1 - 2 * (x * x + z * z)) * c.y +
                                 2 * (y * z - w * x) * c.z;
                reach = std::max(reach, -cy);
            }
        }
        const float low = f.middle.y + f.offset.y - reach;
        if (low < ground) {
            f.offset.y += ground - low;
            if (f.velocity.y < 0.0f) {
                const bool hit = f.velocity.y < -1.0f;
                f.velocity.y = -f.velocity.y * Bounce;
                f.velocity.x *= GroundSlip;
                f.velocity.z *= GroundSlip;
                f.spin = Mul(f.spin, GroundSpin);
                if (hit) {
                    ++f.landings;
                    landed = true;
                }
            }
            const float along = std::sqrt(f.velocity.x * f.velocity.x + f.velocity.z * f.velocity.z);
            if (along < RestMps && std::abs(f.velocity.y) < 1.0f && Length(f.spin) < RestRadS) {
                f.velocity = {};
                f.spin = {};
                f.resting = true;
                if (f.boxed && !f.slumped && StartSlump(f)) {
                    f.resting = false;
                    f.slumping = true;
                }
                break;
            }
        }
    }
    return landed;
}

// Its transform now, row vectors (the cut mesh's vertices, world, times this): turned about its
// middle, then moved.
inline std::array<float, 16> FlightTransform(const Flight& f) {
    const auto& q = f.turn;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    // Rotation as row vectors: row i = the image of axis i.
    const float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)},
                           {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)},
                           {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)}};
    const Vec3 c = f.middle;
    // v' = (v - c) R + c + offset
    const Vec3 t{c.x + f.offset.x - (c.x * r[0][0] + c.y * r[1][0] + c.z * r[2][0]),
                 c.y + f.offset.y - (c.x * r[0][1] + c.y * r[1][1] + c.z * r[2][1]),
                 c.z + f.offset.z - (c.x * r[0][2] + c.y * r[1][2] + c.z * r[2][2])};
    return {r[0][0], r[0][1], r[0][2], 0, r[1][0], r[1][1], r[1][2], 0,
            r[2][0], r[2][1], r[2][2], 0, t.x,     t.y,     t.z,     1};
}

// ---- Where it starts: on its bone at the swap -----------------------------------------------
// The piece is built from the kill's pose, but the corpse ragdolls while its cut is prepared: at
// the swap it is put on its bone as the corpse holds it then (2026-11-16: the limb
// vanished in one place and the piece appeared in another). The bone's whole motion since the
// kill (turn and move) is applied; the mesh stays the kill's (a limb's later bending is not).

// a * inverse(b), 3x3 row-major. False when b is singular or a value is not finite.
inline bool MulInverse(const std::array<float, 9>& a, const std::array<float, 9>& b,
                       std::array<float, 9>& out) {
    const float c00 = b[4] * b[8] - b[5] * b[7], c01 = b[5] * b[6] - b[3] * b[8],
                c02 = b[3] * b[7] - b[4] * b[6];
    const float det = b[0] * c00 + b[1] * c01 + b[2] * c02;
    if (!std::isfinite(det) || std::abs(det) < 1e-12f) {
        return false;
    }
    const float k = 1.0f / det;
    const std::array<float, 9> inv{c00 * k,
                                   (b[2] * b[7] - b[1] * b[8]) * k,
                                   (b[1] * b[5] - b[2] * b[4]) * k,
                                   c01 * k,
                                   (b[0] * b[8] - b[2] * b[6]) * k,
                                   (b[2] * b[3] - b[0] * b[5]) * k,
                                   c02 * k,
                                   (b[1] * b[6] - b[0] * b[7]) * k,
                                   (b[0] * b[4] - b[1] * b[3]) * k};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out[i * 3 + j] =
                a[i * 3] * inv[j] + a[i * 3 + 1] * inv[3 + j] + a[i * 3 + 2] * inv[6 + j];
        }
    }
    for (float v : out) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

// The unit quaternion (x y z w) with Rotate(q, v) == m v, m a rotation (3x3, row-major, column
// vectors) up to rounding. False when m is none: not finite, its axes not unit within 5% (a scale
// or a shear), not square to each other within about 3 degrees, or mirrored.
inline bool RotationOf(const std::array<float, 9>& m, std::array<float, 4>& q) {
    using namespace Detail;
    const Vec3 c0{m[0], m[3], m[6]}, c1{m[1], m[4], m[7]}, c2{m[2], m[5], m[8]};
    for (const Vec3& c : {c0, c1, c2}) {
        const float len = Length(c);
        if (!Finite(c) || !(len > 0.95f && len < 1.05f)) {
            return false;
        }
    }
    if (std::abs(Dot(c0, c1)) > 0.05f || std::abs(Dot(c0, c2)) > 0.05f ||
        std::abs(Dot(c1, c2)) > 0.05f || Dot(Cross(c0, c1), c2) <= 0.0f) {
        return false;
    }
    const float tr = m[0] + m[4] + m[8];
    if (tr > 0.0f) {
        const float s = 2.0f * std::sqrt(tr + 1.0f);
        q = {(m[7] - m[5]) / s, (m[2] - m[6]) / s, (m[3] - m[1]) / s, 0.25f * s};
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = 2.0f * std::sqrt(1.0f + m[0] - m[4] - m[8]);
        q = {0.25f * s, (m[1] + m[3]) / s, (m[2] + m[6]) / s, (m[7] - m[5]) / s};
    } else if (m[4] > m[8]) {
        const float s = 2.0f * std::sqrt(1.0f + m[4] - m[0] - m[8]);
        q = {(m[1] + m[3]) / s, 0.25f * s, (m[5] + m[7]) / s, (m[2] - m[6]) / s};
    } else {
        const float s = 2.0f * std::sqrt(1.0f + m[8] - m[0] - m[4]);
        q = {(m[2] + m[6]) / s, (m[5] + m[7]) / s, 0.25f * s, (m[3] - m[1]) / s};
    }
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 1e-6f) || !std::isfinite(n)) {
        return false;
    }
    for (float& x : q) {
        x /= n;
    }
    return true;
}

// How far a piece must start straight away from the hunter's head (`away`: level, unit) for none
// of its points (about `middle`, world) to come nearer than clear_m: as little as it takes, in
// steps of step_m, at most max_m. 0: clear already, or no direction known.
inline float ClearanceShift(std::span<const Vec3> points, Vec3 middle, Vec3 head, Vec3 away,
                            float clear_m, float max_m, float step_m = 0.05f) {
    using namespace Detail;
    if (!Finite(middle) || !Finite(head) || !Finite(away) || !(Length(away) > 0.5f) ||
        !(step_m > 0.0f)) {
        return 0.0f;
    }
    const float m2 = clear_m * clear_m;
    const Vec3 from = Sub(middle, head);
    const auto clear = [&](float s) {
        for (const Vec3& p : points) {
            const float dx = from.x + p.x + away.x * s, dy = from.y + p.y,
                        dz = from.z + p.z + away.z * s;
            if (dx * dx + dy * dy + dz * dz < m2) {
                return false;
            }
        }
        return true;
    };
    float shift = 0.0f;
    while (shift < max_m && !clear(shift)) {
        shift += step_m;
    }
    return std::min(shift, max_m);
}

// Whether a piece's bone moved and turned since the kill as a body can (a lethal blow's knock, a
// ragdoll's fall) in `since_s`: a pose read wrong, a loose bone, a corpse moved by the game are no
// bodies, and the piece then starts where the kill had it. Never past RebaseMaxM, whatever the
// time. (Mock 2026-11-16: c9020's arm on a loose root, 0.83 m and 177 deg within a cut's wait,
// refused.)
inline constexpr float RebaseMaxM = 4.0f;
inline constexpr float RebaseSlackM = 0.3f, RebaseMaxMps = 8.0f;
inline constexpr float RebaseSlackDeg = 60.0f, RebaseMaxDegS = 900.0f;
enum class Rebase : std::uint8_t { Place, TooFar, TooFast };
inline Rebase RebaseCheck(float moved_m, float turn_deg, float since_s) {
    if (!(moved_m < RebaseMaxM) || !std::isfinite(turn_deg)) {
        return Rebase::TooFar;
    }
    const float t = std::isfinite(since_s) ? std::max(0.0f, since_s) : 0.0f;
    if (!(moved_m < RebaseSlackM + RebaseMaxMps * t) ||
        !(turn_deg < RebaseSlackDeg + RebaseMaxDegS * t)) {
        return Rebase::TooFast;
    }
    return Rebase::Place;
}

// A unit quaternion's angle, degrees (0 - 180: the shorter way).
inline float TurnDegrees(const std::array<float, 4>& q) {
    return 2.0f * std::acos(std::min(1.0f, std::abs(q[3]))) * 57.29578f;
}

// A launch never slower than the body carried the piece along its own way (a knocked, falling
// corpse: it does not stop as it comes off), up to max_mps of it, and without adding to a launch
// already that fast that way (no second push). `carried`: what was added along it.
struct Carry {
    Vec3 velocity{};
    float carried{0.0f};
};
inline Carry CarryAlong(Vec3 launch, Vec3 body, float max_mps) {
    using namespace Detail;
    const float speed = Length(body);
    if (!(speed > 1e-3f) || !std::isfinite(speed) || !Finite(launch)) {
        return {launch, 0.0f};
    }
    const Vec3 u = Mul(body, 1.0f / speed);
    const float carried = std::max(0.0f, std::min(speed, max_mps) - Dot(launch, u));
    return {Add(launch, Mul(u, carried)), carried};
}

// A detached piece's world transform, never its camera-relative transform. Owned by the
// piece under the existing piece lock; no allocation or game pointers. External flight
// corrections must Invalidate even if they leave the piece resting. Moving corpse stumps
// use their live bone transform instead. Resetting/replacing a Piece resets this too.
struct SettledTransform {
    std::array<float, 9> linear{};
    Vec3 middle{};
    bool valid{false};

    void Invalidate() { valid = false; }

    void Update(const Flight& f, const std::array<float, 9>& model_linear) {
        if (valid && f.resting) return;
        const auto t = FlightTransform(f);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                linear[i * 3 + j] = t[i] * model_linear[j] +
                                     t[4 + i] * model_linear[3 + j] +
                                     t[8 + i] * model_linear[6 + j];
            }
        }
        middle = {f.middle.x + f.offset.x, f.middle.y + f.offset.y,
                  f.middle.z + f.offset.z};
        valid = f.resting;
    }
};

} // namespace VR::Sever::Feel
