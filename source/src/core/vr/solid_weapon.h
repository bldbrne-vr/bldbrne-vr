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

#include "core/vr/arm_ik.h"

// Solid weapons (vr.ini weapon_wall; 2026-10-12, not yet tried in the game): the weapon in each
// hand stops at the walls instead of passing through them, and the two weapons stop each other.
//
// Every body update (core/vr/bloodborne_body.cpp, before the arms are solved) the controller's pose
// (the target) is where the weapon wants to be. It moves there from where it was last update,
// which is outside the map, by sweeping the game's own sphere cast against the static map
// (core/vr/bloodborne_evasion.h CastMapSphere): the hand (a sphere), the weapon's line from the
// hand out, and the path of its tip. At the first contact it stops short and slides the rest of
// the move along the surface (move and slide, a few passes for a corner); then it turns toward the
// controller's angle, laid along what it meets (its tip sliding along a wall as the controller
// turns on). It never passes through: pushed or swung on into a wall it stays at the wall,
// however far the controller goes. A weapon caught far from the controller (let_go_m, or turned
// let_go_arc_m of its reach: on something the hunter moved past, around a pole) goes back to it
// when the weapon there can be reached from a point known to be outside (the hunter's chest).
// Where it is not yet (the first update, after the game had the arm) it is reached from there
// too: the hand toward the controller as far as it goes, the weapon turned out of what it meets.
// The arm then holds the weapon where it is.
//
// The weapon chases the hand and is pushed out of what it meets, swept through the real level with
// the game's own cast, and never let through.
//
// Pure: world space, game units (the caller scales metres by the world scale); the cast is the
// caller's. tests/test_solid_weapon.cpp.
namespace VR::Solid {

using Body::Quat;
using Body::Vec3;

struct Capsule {
    Vec3 a{};
    Vec3 b{};
    float r{0.0f};
    bool carried{false}; // thicker than what it starts on (a hammer's head): its start's own way
    bool detached{false}; // authored start: no earlier capsule proves it clear
                         // from the last pose is swept first (it was clear there), then itself
    bool sweep_tip{false}; // a separate prong: also sweep its free tip (Beast Claw only)
};

constexpr int MaxMore = 6;

// The weapon in a hand, on its weapon bone (R_Weapon / L_Weapon): the hand, a sphere at the bone's
// origin; the weapon's line, a capsule from `base` to `tip` (none: the hand alone); and, fitted to
// its model (vr.ini weapon_wall_shape, core/vr/weapon_fit.h: the folded Saw Cleaver's plate, an
// axe's head, the scythe's blade), up to MaxMore capsules more (only with the line), each starting
// on a point already proven clear when it is swept (the hand, the line, an earlier one) and no
// thicker than what it starts on, or carried (a thick head: its start swept along its own way from
// where it was clear, then itself). In Bloodborne (bloodborne_body.cpp SolidWeapons) the fitted
// capsules meet the other weapon (Separate); walls normally meet the line, with
// Beast Claw's two authored prongs using the existing multi-capsule sweep path.
struct Shape {
    float hand_r{0.05f};
    bool line{false};
    Vec3 base{};
    Vec3 tip{};
    float line_r{0.02f};
    std::array<Capsule, MaxMore> more{};
    int extra{0};
};

// How far out of the hand the weapon reaches (its tip's arc per radian of turn).
inline float ReachOf(const Shape& s) {
    if (!s.line) {
        return s.hand_r;
    }
    float r = std::max(Body::Length(s.tip), Body::Length(s.base));
    for (int k = 0; k < std::min(s.extra, MaxMore); ++k) {
        const Capsule& c = s.more[static_cast<std::size_t>(k)];
        r = std::max(r, std::max(Body::Length(c.a), Body::Length(c.b)) + c.r);
    }
    return r;
}

struct Pose {
    Vec3 p{};
    Quat q{};
};

inline Vec3 At(const Pose& x, Vec3 local) {
    return x.p + Body::Rotate(x.q, local);
}

// A weapon's line seen in the world (`base` the end nearer the hand) on a hand posed at `hand`:
// the Shape's line in the hand (its weapon bone). False when it is no line on that hand (shorter
// than `min_m`, its base further than `reach_m` out, its tip not further out than its base).
inline bool LineOnHand(const Pose& hand, Vec3 base, Vec3 tip, float min_m, float reach_m,
                       Shape& s) {
    const Quat back = Body::Conjugate(hand.q);
    const Vec3 b = Body::Rotate(back, base - hand.p);
    const Vec3 t = Body::Rotate(back, tip - hand.p);
    if (!Body::IsFinite(b) || !Body::IsFinite(t) ||
        !(Body::Length(t - b) >= min_m) || !(Body::Length(b) <= reach_m) ||
        !(Body::Length(t) > Body::Length(b))) {
        return false;
    }
    s.line = true;
    s.base = b;
    s.tip = t;
    return true;
}

template <class ReadPoint>
bool PartLine(ReadPoint&& read, Shape& s) {
    const auto accept = [&](Vec3 a, Vec3 b) {
        if (Body::LengthSq(a) > Body::LengthSq(b)) {
            std::swap(a, b);
        }
        return Body::Length(b) <= 2.5f && LineOnHand({}, a, b, 0.005f, 0.5f, s);
    };
    Vec3 a{}, b{};
    if (read(100, a) && read(110, b) && accept(a, b)) {
        return true;
    }
    constexpr std::array<int, 22> suffixes{100, 101, 102, 103, 104, 105, 106, 110,
                                          111, 112, 113, 120, 121, 122, 123, 130,
                                          131, 132, 133, 140, 141, 142};
    bool found = false;
    for (const int suffix : suffixes) {
        Vec3 p{};
        if (!read(suffix, p) || !Body::IsFinite(p) || Body::Length(p) > 2.5f) {
            continue;
        }
        if (!found || Body::LengthSq(p) < Body::LengthSq(a)) {
            a = p;
        }
        if (!found || Body::LengthSq(p) > Body::LengthSq(b)) {
            b = p;
        }
        found = true;
    }
    return found && (accept(a, b) || (Body::Length(a) <= 0.5f && accept({}, b)));
}

template <class ReadAttachment, class ReadPoint>
int AttachedPartLine(int hand, ReadAttachment&& attachment, ReadPoint&& read, Shape& s) {
    s.line = false;
    if (hand < 0 || hand > 1) {
        return -1;
    }
    const int first = hand == 0 ? 340 : 300;
    const int last = hand == 0 ? 359 : 339;
    for (int part = 0; part < 4; ++part) {
        const int attach = attachment(part);
        if (attach < first || attach > last) {
            continue;
        }
        const int group = 10 + part;
        if (PartLine([&](int suffix, Vec3& p) { return read(group * 1000 + suffix, p); }, s)) {
            return group;
        }
    }
    return -1;
}

inline void BladeOrPartLine(bool have_reach, float base, float tip, float min_m,
                           const Shape& part, Shape& s) {
    s.line = false;
    if (have_reach && std::isfinite(base) && std::isfinite(tip) && tip > base + min_m) {
        s.line = true;
        s.base = {0.0f, -base, 0.0f};
        s.tip = {0.0f, -tip, 0.0f};
    } else if (part.line) {
        s.line = true;
        s.base = part.base;
        s.tip = part.tip;
    }
}

// One sweep of a sphere of radius `r` from `from` to `to` against the map: its first contact's
// fraction of the way and the surface's normal there (out of the surface). A sweep that starts
// inside the map reports nothing (the game's own: no information).
struct Hit {
    bool hit{false};
    float f{1.0f};
    Vec3 n{};
};

struct Config {
    float skin{0.005f};       // a weapon stopped at a surface rests this far off it (the next
                              // sweep then starts outside it and the cast's own tolerance, so it
                              // still sees it) ...
    float back_m{0.002f};     // ... and every sweep starts this far behind its start (one that
                              // starts touching what it moves toward still sees it)
    float let_go_m{0.45f};    // the weapon caught this far from the controller: back to it when
                              // it can be reached there ...
    float let_go_arc_m{1.5f}; // ... or turned this far from it (the angle times its reach)
    int slides{3};            // move and slide passes (a corner takes two)
    int search{5};            // halvings of a blocked move
    float step_m{0.08f};      // a substep moves no part of the weapon further (a railing is
                              // never stepped over) ...
    int max_steps{24};        // ... in at most this many ...
    int max_casts{64};        // ... and no more sweeps than this an update (on with the next)
};

// The surface a weapon met this update.
struct Contact {
    bool any{false};
    Vec3 point{};     // on the surface (under the sphere that touched it)
    Vec3 normal{};    // out of it
    bool line{false}; // the weapon's line or tip, or one of its further capsules (else the hand)
    int more{-1};     // ... which further capsule (-1: the line, its tip, or the hand)
};

struct Result {
    Pose pose{}; // where the weapon is (the arm holds it there)
    Contact contact{};
    bool through{false}; // not reached yet (nothing around the hand clear: a crevice): the
                         // controller's pose until it is
    bool back{false};    // caught too far from the controller: back at it this update
    bool replaced{false}; // its new shape met something where it rested: from the chest
    float off{0.0f};     // the hand's distance from the controller's pose
};

class Weapon {
public:
    void Reset() {
        have_ = false;
    }
    // Where it was left last update (false: nowhere yet).
    bool Held(Pose& out) const {
        out = solved_;
        return have_;
    }

    // Moved `by` from where Step left it (the other weapon pushing it, Separate): only when the
    // way is clear, all or nothing; `pose` where it is then. False (not moved) when something is
    // in the way.
    template <class Cast>
    bool Push(Vec3 by, const Shape& s, const Config& c, Cast&& cast, Pose& pose) {
        Probe<Cast> probe{s, cast, c.back_m};
        const Pose to{solved_.p + by, solved_.q};
        Hit h{};
        Vec3 at{};
        if (!have_ || !probe.Clear(solved_, to, h, at)) {
            return false;
        }
        solved_ = pose = to;
        return true;
    }

    // `target`: the controller's pose for the weapon bone; `safe`: a point outside the map (the
    // hunter's chest) the hand can be reached from.
    template <class Cast>
    Result Step(const Pose& target, Vec3 safe, const Shape& s, const Config& c, Cast&& cast) {
        Probe<Cast> probe{s, cast, c.back_m};
        Result out{};
        Pose cur = solved_;
        // Its shape changed (a new weapon or form, its fitted shape arriving): nothing showed the
        // new one clear where the weapon rests, and a controller held still moves nothing that
        // would. Tested there (a carried start grown, Around); placed again from the chest only
        // when something is in it.
        if (ShapeChanged(s) && have_) {
            Hit h{};
            Vec3 at{};
            probe.unproven = true;
            have_ = probe.Clear(cur, cur, h, at, false);
            probe.unproven = false;
            out.replaced = !have_;
        }
        if (!have_) {
            if (!Reach(target, safe, s, c, cast, cur)) {
                out.pose = target;
                out.through = true;
                return out;
            }
            have_ = true;
        }
        Chase(cur, target, probe, s, c, out.contact);
        // Caught far from the controller: back to it when the weapon there can be reached from the
        // chest (pressed into a wall it cannot: it stays at the wall).
        out.off = Body::Length(cur.p - target.p);
        const float arc = Body::AngleBetween(cur.q, target.q) * ReachOf(s);
        if (out.off > c.let_go_m || arc > c.let_go_arc_m) {
            Hit h{};
            Vec3 at{};
            probe.unproven = true;
            if (probe.Clear({safe, target.q}, target, h, at, false)) {
                cur = target;
                out.contact = {};
                out.off = 0.0f;
                out.back = true;
            }
            probe.unproven = false;
        }
        solved_ = cur;
        out.pose = cur;
        return out;
    }

private:
    // The test of a move: the hand's sweep, the weapon's line from the hand out at the end, its
    // tip's sweep. False with the first contact: its hit and the surface point under the sphere.
    template <class Cast>
    struct Probe {
        const Shape& s;
        Cast& cast;
        float back{0.0f};
        int casts{0};
        bool line_hit{false};
        bool across{false}; // ... by the line from the hand out (not the tip's way), at:
        int more_hit{-1};   // ... by this further capsule (-1: none of them)
        bool unproven{false}; // `from` was never shown clear: carried starts grown instead (More)
        Pose last{};

        // A carried capsule's start nothing showed clear: a sphere of the line's radius there
        // (clear: what it starts on was swept just before) swept out from it 26 ways (to a cube's
        // faces, edges and corners), each 1/0.88 of the way to the capsule's radius: every
        // direction lies within acos 0.886 of one of them, so a surface anywhere within the radius
        // stops one (and one up to 14% beyond it may: a pose refused that was clear).
        bool Around(Vec3 a, float r, Hit& h, Vec3& at) {
            static const std::array<Vec3, 26> dirs = [] {
                std::array<Vec3, 26> d{};
                std::size_t n = 0;
                for (int x = -1; x <= 1; ++x) {
                    for (int y = -1; y <= 1; ++y) {
                        for (int z = -1; z <= 1; ++z) {
                            if (x != 0 || y != 0 || z != 0) {
                                d[n++] = Body::Normalize(Vec3{static_cast<float>(x),
                                                              static_cast<float>(y),
                                                              static_cast<float>(z)});
                            }
                        }
                    }
                }
                return d;
            }();
            const float out = (r - s.line_r) / 0.88f;
            if (!(out > 0.0f)) {
                return true;
            }
            for (const Vec3& d : dirs) {
                if (Sweep(a, a + d * out, s.line_r, h, at)) {
                    return false;
                }
            }
            return true;
        }

        bool Clear(const Pose& from, const Pose& to, Hit& h, Vec3& at, bool sweep_tip = true) {
            more_hit = -1;
            if (Body::LengthSq(to.p - from.p) > 1e-10f && Sweep(from.p, to.p, s.hand_r, h, at)) {
                line_hit = false;
                across = false;
                return false;
            }
            if (!s.line) {
                return true;
            }
            const Vec3 tip_to = At(to, s.tip);
            // A structural handle can extend behind the grip. Prove its base from the hand,
            // then sweep the full capsule. The old outward line already contains its base.
            const float tip2 = Body::LengthSq(s.tip);
            const float u = tip2 > 1e-12f ? std::clamp(Body::Dot(s.base, s.tip) / tip2, 0.0f, 1.0f) : 0.0f;
            const bool full = Body::LengthSq(s.base - s.tip * u) > 1e-8f;
            const Vec3 base_to = full ? At(to, s.base) : to.p;
            if ((full && Sweep(to.p, base_to, s.line_r, h, at)) ||
                Sweep(base_to, tip_to, s.line_r, h, at)) {
                line_hit = true;
                across = true;
                last = to;
                return false;
            }
            if (!More(from, to, h, at)) {
                line_hit = true;
                across = false;
                return false;
            }
            const Vec3 tip_from = At(from, s.tip);
            if (sweep_tip && Body::LengthSq(tip_to - tip_from) > 1e-10f &&
                Sweep(tip_from, tip_to, s.line_r, h, at)) {
                line_hit = true;
                across = false;
                return false;
            }
            return true;
        }
        // The further capsules on `to`, in their order (each starts on what is swept before it; a
        // carried one's start is swept first along its way from `from`, where it was clear, or,
        // `unproven` (placed from the chest, turned out of a wall: `from` never shown clear),
        // grown out of the line's sphere it starts on, Around). False with the first that meets
        // something (more_hit says which).
        bool More(const Pose& from, const Pose& to, Hit& h, Vec3& at) {
            for (int k = 0; k < std::min(s.extra, MaxMore); ++k) {
                const Capsule& c = s.more[static_cast<std::size_t>(k)];
                const Vec3 a = At(to, c.a);
                if (c.detached && unproven &&
                    Sweep(to.p, a, std::min(s.hand_r, c.r), h, at)) {
                    more_hit = k;
                    return false;
                }
                if (c.carried) {
                    const Vec3 was = At(from, c.a);
                    const bool met = unproven ? !Around(a, c.r, h, at)
                                              : Body::LengthSq(a - was) > 1e-10f &&
                                                    Sweep(was, a, c.r, h, at);
                    if (met) {
                        more_hit = k;
                        return false;
                    }
                }
                if (Sweep(a, At(to, c.b), c.r, h, at)) {
                    more_hit = k;
                    return false;
                }
                // A fork's second free tip needs the same continuous check as
                // the primary tip, or a fast sideways move can skip a thin bar.
                // Never sweep from an unproven pose (initial placement/reshape).
                if (c.sweep_tip && !unproven) {
                    const Vec3 tip_from = At(from, c.b), tip_to = At(to, c.b);
                    if (Body::LengthSq(tip_to - tip_from) > 1e-10f &&
                        Sweep(tip_from, tip_to, c.r, h, at)) {
                        more_hit = k;
                        return false;
                    }
                }
            }
            return true;
        }
        // The surface's normal at the last contact `h`. The line met from the hand out sees the
        // near side of a pole or an edge across it, its normal tilted back toward the hand (it
        // would slide the weapon back along itself); seen from the tip too, the two together are
        // the normal across the line. A wall the tip is in is seen from the hand only; a plank
        // the line crosses (the two cancel): `away`, the way the weapon came.
        Vec3 Normal(const Hit& h, Vec3 away) {
            Hit t{};
            Vec3 at{};
            if (!across || !Sweep(At(last, s.tip), last.p, s.line_r, t, at)) {
                return h.n;
            }
            return Body::Normalize(h.n + t.n, Body::Normalize(away, h.n));
        }
        bool Sweep(Vec3 a, Vec3 b, float r, Hit& h, Vec3& at) {
            ++casts;
            // From `back` behind `a`: the cast reports nothing for a sweep that starts touching.
            const Vec3 way = b - a;
            const float len = Body::Length(way);
            h = cast(len > 1e-6f ? a - way * (back / len) : a, b, r);
            if (!h.hit) {
                return false;
            }
            h.f = len > 1e-6f ? std::max((h.f * (len + back) - back) / len, 0.0f) : 0.0f;
            // The surface faces the sweep's start, whatever sign the cast gives (none: straight
            // back along the sweep).
            h.n = Body::Normalize(h.n, Body::Normalize(-way, {0.0f, 1.0f, 0.0f}));
            if (Body::Dot(h.n, way) > 0.0f) {
                h.n = -h.n;
            }
            at = Body::Lerp(a, b, std::clamp(h.f, 0.0f, 1.0f)) - h.n * r;
            return true;
        }
    };

    // From `cur` toward `target` in substeps: no part of the weapon (the hand, the tip) goes
    // further than step_m in one, so nothing thicker than a railing is stepped over (the hand alone
    // in one: its sweep misses nothing). Once it meets a surface, what is left of the hand's way
    // into it goes along it (a controller deep in a wall: the substeps along the wall, not into
    // it), and a turn that got no closer is not tried again this update (a tip held off a floor
    // while the hand walks on). A blocked weapon stops trying once a substep gets it no closer.
    template <class Cast>
    static void Chase(Pose& cur, const Pose& target, Probe<Cast>& probe, const Shape& s,
                      const Config& c, Contact& contact) {
        Pose goal = target;
        for (int i = 0; i < c.max_steps && probe.casts < c.max_casts; ++i) {
            const float way = Way(cur, goal, s);
            if (way < 1e-6f) {
                break;
            }
            const float t = s.line ? std::min(1.0f, c.step_m / way) : 1.0f;
            const float turn = Body::AngleBetween(cur.q, goal.q);
            Contact k{};
            Move(cur, {Body::Lerp(cur.p, goal.p, t), Body::Slerp(cur.q, goal.q, t)}, probe, s, c,
                 k);
            if (k.any) {
                contact = k;
                const Vec3 left = goal.p - cur.p;
                if (Body::Dot(left, k.normal) < 0.0f) {
                    goal.p = cur.p + Body::Perp(left, k.normal);
                }
                if (Body::AngleBetween(cur.q, goal.q) > turn - 1e-4f) {
                    goal.q = cur.q;
                }
            } else if (t >= 1.0f) {
                break;
            }
            if (Way(cur, goal, s) > way - 1e-4f) {
                break;
            }
        }
    }

    // How far the weapon is from `to`: its hand's way and its tip's arc of the turn (no part of it
    // goes further; a tip held off where it cannot go, under a floor, never counts as farther).
    static float Way(const Pose& from, const Pose& to, const Shape& s) {
        const float hand = Body::Length(to.p - from.p);
        return s.line ? hand + ReachOf(s) * Body::AngleBetween(from.q, to.q) : hand;
    }

    // Reached from the chest: the hand alone toward the controller as far as it goes (sliding
    // along what it meets), the weapon from there at the controller's angle turned out of what
    // that meets (TurnOut). False when nothing around the hand is clear.
    template <class Cast>
    static bool Reach(const Pose& target, Vec3 safe, const Shape& s, const Config& c, Cast& cast,
                      Pose& out) {
        Shape hand = s;
        hand.line = false;
        Probe<Cast> hand_probe{hand, cast, c.back_m};
        Contact k{};
        out = {safe, target.q};
        Chase(out, target, hand_probe, hand, c, k);
        Probe<Cast> probe{s, cast, c.back_m};
        probe.unproven = true;
        return !s.line || TurnOut(out, probe, s);
    }

    // The weapon's line from the hand turned out of what it meets: laid along it (pointing down
    // along it when it went straight in) and tilted out of it, again for a corner, else straight
    // out of one of the surfaces it met. False when none of these is clear.
    template <class Cast>
    static bool TurnOut(Pose& cur, Probe<Cast>& probe, const Shape& s) {
        const Vec3 from = Body::Rotate(cur.q, s.tip);
        const auto clear = [&](Vec3 dir, Vec3& n) {
            Hit h{};
            Vec3 at{};
            const Pose turned{cur.p, Body::Normalize(Body::FromTo(from, dir) * cur.q)};
            if (!probe.Clear(turned, turned, h, at, false)) {
                n = h.n;
                return false;
            }
            cur = turned;
            return true;
        };
        // Within 3 degrees of straight into it (or of level, for `down`): no way along it.
        const auto along = [](Vec3 v, Vec3 n, Vec3 none) {
            const Vec3 a = Body::Perp(v, n);
            return Body::LengthSq(a) > 0.0025f ? Body::Normalize(a) : none;
        };
        Vec3 met[3]{};
        Vec3 dir = Body::Normalize(from);
        for (Vec3& n : met) {
            if (clear(dir, n)) {
                return true;
            }
            const Vec3 down = along({0.0f, -1.0f, 0.0f}, n, Body::AnyPerpendicular(n));
            dir = Body::Normalize(along(dir, n, down) + n * 0.35f, n);
        }
        Vec3 n{};
        if (clear(dir, n)) {
            return true;
        }
        for (const Vec3& m : met) {
            if (clear(m, n)) {
                return true;
            }
        }
        return false;
    }

    // One substep: the hand's way with the weapon's angle as it was (move and slide), then the
    // goal's angle, laid along what it meets (turn and slide) or as far as it stays clear. Every
    // stop rests `skin` off the surface (Rest).
    template <class Cast>
    static void Move(Pose& cur, const Pose& goal, Probe<Cast>& probe, const Shape& s,
                     const Config& c, Contact& contact) {
        {
            // Free air (nearly always): the whole substep in one test.
            Hit h{};
            Vec3 at{};
            if (probe.Clear(cur, goal, h, at)) {
                cur = goal;
                return;
            }
        }
        Vec3 to = goal.p;
        for (int pass = 0; pass < c.slides; ++pass) {
            Hit h{};
            Vec3 at{};
            if (probe.Clear(cur, {to, cur.q}, h, at)) {
                cur.p = to;
                break;
            }
            bool line = probe.line_hit;
            int more = probe.more_hit;
            // The hand alone: where its sweep touched. With the weapon's line (no sweep of its
            // own), halved down to it.
            float lo = s.line ? 0.0f : std::clamp(h.f, 0.0f, 1.0f);
            float hi = 1.0f;
            for (int i = 0; s.line && i < c.search; ++i) {
                const float mid = 0.5f * (lo + hi);
                Hit hm{};
                Vec3 am{};
                if (probe.Clear(cur, {Body::Lerp(cur.p, to, mid), cur.q}, hm, am)) {
                    lo = mid;
                } else {
                    hi = mid;
                    h = hm;
                    at = am;
                    line = probe.line_hit;
                    more = probe.more_hit;
                }
            }
            const Vec3 n = probe.Normal(h, cur.p - to);
            cur.p = Body::Lerp(cur.p, to, lo);
            contact = {true, at, n, line, line ? more : -1};
            Rest(cur, s, c, contact, false, probe);
            // The rest of the way along the surface it met.
            to = cur.p + Body::Perp(to - cur.p, n);
            if (Body::LengthSq(to - cur.p) < 1e-8f) {
                break;
            }
        }
        if (!s.line || Body::AngleBetween(cur.q, goal.q) < 1e-6f) {
            cur.q = goal.q; // the hand alone: its turn is free
            return;
        }
        const Pose want{cur.p, goal.q};
        Hit h{};
        Vec3 at{};
        if (probe.Clear(cur, want, h, at)) {
            cur = want;
            return;
        }
        // The turn into what it met: the controller's angle laid along it instead (the part of the
        // turn into it dropped, its tip resting line_r + skin off it), the way a stopped move
        // slides; the roll about the blade the controller's. Else as far as it turns clear.
        const Vec3 n = probe.Normal(h, At(cur, s.tip) - At(want, s.tip));
        const float reach = Body::Length(s.tip);
        const float rest = s.line_r + c.skin;
        const float k = (rest - Body::Dot(cur.p - at, n)) / std::max(reach, 1e-4f);
        const Vec3 g = Body::Normalize(Body::Rotate(goal.q, s.tip));
        if (Body::Dot(g, n) < k && k > -1.0f && k < 1.0f) {
            const Vec3 side = Body::Normalize(Body::Perp(g, n), Body::AnyPerpendicular(n));
            const Vec3 dir = n * k + side * std::sqrt(1.0f - k * k);
            const Pose laid{cur.p, Body::Normalize(Body::FromTo(g, dir) * goal.q)};
            Hit hl{};
            Vec3 al{};
            if (probe.Clear(cur, laid, hl, al)) {
                cur = laid;
                contact = {true, At(cur, s.tip) - n * rest, n, true, -1};
                return;
            }
        }
        float lo = 0.0f;
        float hi = 1.0f;
        int more = probe.more_hit;
        for (int i = 0; i < c.search; ++i) {
            const float mid = 0.5f * (lo + hi);
            Hit hm{};
            Vec3 am{};
            if (probe.Clear(cur, {cur.p, Body::Slerp(cur.q, goal.q, mid)}, hm, am)) {
                lo = mid;
            } else {
                hi = mid;
                h = hm;
                at = am;
                more = probe.more_hit;
            }
        }
        cur.q = Body::Slerp(cur.q, goal.q, lo);
        contact = {true, at, probe.Normal(h, At(cur, s.tip) - At(want, s.tip)), true, more};
        Rest(cur, s, c, contact, true, probe);
    }

    // A weapon stopped at a surface rests `skin` off it (the next sweep then starts outside it and
    // still sees it): the part that touched it, the hand or the line's point nearest the contact.
    // A stopped move backs off along the surface's normal; a stopped turn turns back (so a blade
    // pressed into a wall never walks the hand away from the controller). Only when that way is
    // clear (a blade on a pole's edge, the hand on a wall behind it: the pole's normal can point
    // into the wall).
    template <class Cast>
    static void Rest(Pose& cur, const Shape& s, const Config& c, const Contact& k, bool turn,
                     Probe<Cast>& probe) {
        const Pose stopped = cur;
        Vec3 x = cur.p;
        float r = s.hand_r;
        if (k.line && s.line) {
            const bool more = k.more >= 0 && k.more < std::min(s.extra, MaxMore);
            const Capsule& m = s.more[static_cast<std::size_t>(more ? k.more : 0)];
            const Vec3 a = At(cur, more ? m.a : s.base);
            const Vec3 b = At(cur, more ? m.b : s.tip);
            const float len2 = Body::LengthSq(b - a);
            const float u =
                len2 > 1e-12f ? std::clamp(Body::Dot(k.point - a, b - a) / len2, 0.0f, 1.0f)
                              : 0.0f;
            x = Body::Lerp(a, b, u);
            r = more ? m.r : s.line_r;
        }
        const float m = Body::Dot(x - k.point, k.normal) - r;
        if (!(m < c.skin)) {
            return;
        }
        const Vec3 arm = x - cur.p;
        const float lever = Body::Length(Body::Perp(arm, k.normal));
        if (turn && lever > 0.05f) {
            // About the grip, the touching point out along the normal.
            const Vec3 axis = Body::Normalize(Body::Cross(arm, k.normal));
            cur.q = Body::Normalize(Body::AxisAngle(axis, (c.skin - m) / lever) * cur.q);
        } else {
            cur.p = cur.p + k.normal * (c.skin - m);
        }
        Hit h{};
        Vec3 at{};
        if (!probe.Clear(stopped, cur, h, at)) {
            cur = stopped;
        }
    }

    // Whether the shape differs from the last step's: another count of capsules, a radius or a
    // carried flag, or an end more than 2 mm off (a re-measured line's noise is less).
    bool ShapeChanged(const Shape& s) {
        const auto moved = [](Vec3 a, Vec3 b) { return Body::LengthSq(a - b) > 4e-6f; };
        const auto resized = [](float a, float b) { return std::abs(a - b) > 1e-3f; };
        const int n = s.line ? std::min(s.extra, MaxMore) : 0;
        bool changed = !have_last_ || s.line != last_.line || n != last_n_ ||
                       resized(s.hand_r, last_.hand_r);
        if (!changed && s.line) {
            changed = moved(s.base, last_.base) || moved(s.tip, last_.tip) ||
                      resized(s.line_r, last_.line_r);
        }
        for (int k = 0; !changed && k < n; ++k) {
            const Capsule& a = s.more[static_cast<std::size_t>(k)];
            const Capsule& b = last_.more[static_cast<std::size_t>(k)];
            changed = moved(a.a, b.a) || moved(a.b, b.b) || resized(a.r, b.r) ||
                      a.carried != b.carried || a.detached != b.detached ||
                      a.sweep_tip != b.sweep_tip;
        }
        last_ = s;
        last_n_ = n;
        have_last_ = true;
        return changed;
    }

    Pose solved_{};
    bool have_{false};
    Shape last_{}; // the shape of the last step (ShapeChanged)
    int last_n_{0};
    bool have_last_{false};
};

// ---- The two weapons against each other ---------------------------------------------------------

// The closest points of the segments a0-a1 and b0-b1: their parameters along each (0 .. 1).
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

struct Touch {
    bool any{false};
    Vec3 point{};  // between the two, where they touch
    Vec3 normal{}; // from b's line toward a's
    float depth{0.0f};
    float a_share{0.5f}; // fraction of the separation assigned to a
};

constexpr float CrossM = 0.15f; // pressed this far across each other, they let each other pass

// Capsule `k` of a weapon in its hand's frame: 0 its line, 1.. its further capsules (fitted to its
// model). False past the last, or for the bare hand.
inline bool CapsuleOf(const Shape& s, int k, Capsule& out) {
    if (!s.line || k < 0 || k > std::min(s.extra, MaxMore)) {
        return false;
    }
    out = k == 0 ? Capsule{s.base, s.tip, s.line_r} : s.more[static_cast<std::size_t>(k - 1)];
    return true;
}

constexpr int SeparatePasses = 4; // in touch: the deepest pair pushed apart, then the next (a crook)
constexpr int SeparateSteps = 8;  // walked from where they were solved: at most this many steps

// Separate along the contact normal (a by +normal, b by -normal). Each weapon is its line and its
// further capsules (fitted to its model: vr.ini weapon_wall_shape); every capsule of one is tested
// against every capsule of the other, the pair pressed deepest pushed apart first, then the next
// one still in touch (up to SeparatePasses: a blade in the crook of another, met on two of its
// capsules at once). A pair further apart than any contact could matter costs one dot product.
//
// Without solved poses (the lines alone, as in 0.7.0; the first update, after a snap turn): `side`
// (the last contact's normal) or else the previous controller poses establish the side, including
// a crossing within one update; else they part the way they are apart. Pressed on across each
// other, a pair keeps to its side up to `cross` across, then the weapons pass. A line against a
// line is one pair, one pass: as before the capsules.
//
// With the weapons' poses as last solved (`solved_a`, `solved_b`: apart there, or just touching),
// the two are walked from there to where they are now in steps no capsule moves further in than
// half the thinnest pair's thickness, each step's contacts pushed apart before the next: every
// push is shallow, out the way the pair really met, so a contact sliding from one capsule onto the
// next, pressed into a crook or swung through within one update ends where it touched, never
// pushed out the far side. Held more than `cross` from where the controllers have them, they let
// each other pass (this update's poses as they are). A pair already well inside the other where
// last solved (passing, a shape that just changed over the other weapon, a push a wall refused)
// lets them pass until they are apart, never popped out a guessed way.
//
// Each hand yields in proportion to its inward motion at first contact (including rotation, from
// the controllers' previous poses); while touching (`side` given) the split is kept (`a_share`)
// until separation. Without history it is equal.
inline Touch Separate(Pose& a, const Shape& sa, Pose& b, const Shape& sb,
                      const Vec3* side = nullptr, float cross = CrossM,
                      const Pose* previous_a = nullptr, const Pose* previous_b = nullptr,
                      float a_share = 0.5f, const Pose* solved_a = nullptr,
                      const Pose* solved_b = nullptr) {
    Touch out{};
    if (!sa.line || !sb.line) {
        return out;
    }
    const bool continuing = side != nullptr;
    const bool history = previous_a != nullptr && previous_b != nullptr;
    const int na = 1 + std::min(sa.extra, MaxMore);
    const int nb = 1 + std::min(sb.extra, MaxMore);
    // The closest points of two capsules on their poses: from the one on y to the one on x.
    const auto gap = [](const Pose& x, const Capsule& cx, const Pose& y, const Capsule& cy) {
        const Vec3 x0 = At(x, cx.a), x1 = At(x, cx.b);
        const Vec3 y0 = At(y, cy.a), y1 = At(y, cy.b);
        float s{}, t{};
        Closest(x0, x1, y0, y1, s, t);
        return Body::Lerp(x0, x1, s) - Body::Lerp(y0, y1, t);
    };
    // A pair pressed into each other: its capsules, where along each they are closest, its normal.
    struct Pair {
        Capsule ca{}, cb{};
        Vec3 pa{}, pb{}, normal{};
        float s{0.0f}, t{0.0f}, reach{0.0f}, depth{0.0f};
    };
    // The first contact: which hand yields (its share kept while they touch).
    const auto first_contact = [&](const Pair& p) {
        out.any = true;
        out.a_share = std::clamp(a_share, 0.0f, 1.0f);
        if (!history || continuing) {
            return;
        }
        // Choose which hand yields on approach, then retain that choice for this contact.
        // Reassigning the entire penetration from per-frame motion turns millimetres of wrist
        // noise into centimetres of hand jumps. Compare the same material points so rotating a
        // blade counts even when its grip stays still.
        const Vec3 da = p.pa - At(*previous_a, Body::Lerp(p.ca.a, p.ca.b, p.s));
        const Vec3 db = p.pb - At(*previous_b, Body::Lerp(p.cb.a, p.cb.b, p.t));
        const float inward_a = -Body::Dot(da, p.normal), inward_b = Body::Dot(db, p.normal);
        const float quiet = p.reach * 0.01f;
        if (inward_a + inward_b > quiet) {
            const float wa = inward_a > quiet ? inward_a : 0.0f;
            const float wb = inward_b > quiet ? inward_b : 0.0f;
            if (wa + wb > quiet)
                out.a_share = wa / (wa + wb);
        }
    };
    const auto note = [&](const Pair& p) {
        out.normal = p.normal;
        out.point = Body::Lerp(p.pa, p.pb, p.ca.r / p.reach);
        out.depth = std::max(out.depth, p.depth);
    };
    constexpr int Pairs = (MaxMore + 1) * (MaxMore + 1);
    const auto index = [](int i, int j) { return static_cast<std::size_t>(i * (MaxMore + 1) + j); };

    if (solved_a == nullptr || solved_b == nullptr) {
        // Each pair's side, found once the first time it is near: 0 not yet, 1 known, 2 none.
        std::array<signed char, Pairs> how{};
        std::array<Vec3, Pairs> sides{};
        const int passes = na * nb > 1 ? SeparatePasses : 1;
        for (int pass = 0; pass < passes; ++pass) {
            Pair best{};
            bool found = false;
            for (int i = 0; i < na; ++i) {
                for (int j = 0; j < nb; ++j) {
                    Capsule ca{}, cb{};
                    if (!CapsuleOf(sa, i, ca) || !CapsuleOf(sb, j, cb)) {
                        continue;
                    }
                    const Vec3 a0 = At(a, ca.a);
                    const Vec3 a1 = At(a, ca.b);
                    const Vec3 b0 = At(b, cb.a);
                    const Vec3 b1 = At(b, cb.b);
                    float s = 0.0f;
                    float t = 0.0f;
                    Closest(a0, a1, b0, b1, s, t);
                    const Vec3 pa = Body::Lerp(a0, a1, s);
                    const Vec3 pb = Body::Lerp(b0, b1, t);
                    const Vec3 d = pa - pb;
                    const float reach = ca.r + cb.r;
                    // Further apart than this, no side makes them touch (apart, or crossed past
                    // `cross`).
                    if (Body::LengthSq(d) > reach * reach + cross * cross) {
                        continue;
                    }
                    const std::size_t k = index(i, j);
                    if (how[k] == 0) {
                        how[k] = 2;
                        if (side != nullptr) {
                            sides[k] = *side;
                            how[k] = 1;
                        } else if (history) {
                            const Vec3 was = gap(*previous_a, ca, *previous_b, cb);
                            if (Body::LengthSq(was) > reach * reach * 1e-6f) {
                                sides[k] = Body::Normalize(was);
                                how[k] = 1;
                            }
                        }
                    }
                    const Vec3* pair_side = how[k] == 1 ? &sides[k] : nullptr;
                    const bool crossed = pair_side != nullptr &&
                                         Body::Dot(d, *pair_side) < 0.0f &&
                                         Body::LengthSq(Body::Perp(d, *pair_side)) < reach * reach;
                    // Coincident lines can leave a tiny sideways subtraction residual in world
                    // space. Its direction is noise: keeping it would erase the approach side and
                    // flip the weapons on the next inward frame. Preserve that side within 1% of
                    // their combined radius (0.4 mm at the default thickness), using the signed
                    // gap for the matching separation depth.
                    const bool keep_side = crossed || (pair_side != nullptr &&
                                                       Body::LengthSq(d) <= reach * reach * 1e-4f);
                    const float dist = keep_side ? Body::Dot(d, *pair_side) : Body::Length(d);
                    if (dist >= reach || -dist > cross ||
                        (found && reach - dist <= best.depth)) {
                        continue;
                    }
                    // Lines crossing exactly: apart the way they met, else across both of them.
                    const Vec3 across =
                        pair_side != nullptr ? *pair_side
                                             : Body::Normalize(Body::Cross(a1 - a0, b1 - b0),
                                                               Body::AnyPerpendicular(a1 - a0));
                    best = {ca, cb, pa, pb, keep_side ? *pair_side : Body::Normalize(d, across),
                            s, t, reach, reach - dist};
                    found = true;
                }
            }
            if (!found) {
                break;
            }
            if (pass == 0) {
                first_contact(best);
                out.depth = best.depth;
                out.normal = best.normal;
                out.point = Body::Lerp(best.pa, best.pb, best.ca.r / best.reach);
            }
            a.p = a.p + best.normal * (out.a_share * best.depth);
            b.p = b.p - best.normal * ((1.0f - out.a_share) * best.depth);
        }
        return out;
    }

    // Near at all where the controllers have them? (Else nothing: they pass or are apart.)
    bool close_by = false;
    float thin_a = 0.0f, thin_b = 0.0f;
    for (int i = 0; i < na; ++i) {
        Capsule ca{};
        CapsuleOf(sa, i, ca);
        thin_a = i == 0 ? ca.r : std::min(thin_a, ca.r);
        for (int j = 0; j < nb; ++j) {
            Capsule cb{};
            CapsuleOf(sb, j, cb);
            thin_b = j == 0 ? cb.r : std::min(thin_b, cb.r);
            const float reach = ca.r + cb.r;
            close_by = close_by || Body::LengthSq(gap(a, ca, b, cb)) <= reach * reach + cross * cross;
        }
    }
    if (!close_by) {
        return out;
    }
    // A pair inside the other where last solved, by more than a quarter of their thickness (a
    // solved touch is within a fraction of a millimetre): it lets them pass this update.
    std::array<bool, Pairs> inside{};
    for (int i = 0; i < na; ++i) {
        for (int j = 0; j < nb; ++j) {
            Capsule ca{}, cb{};
            CapsuleOf(sa, i, ca);
            CapsuleOf(sb, j, cb);
            inside[index(i, j)] = !(Body::Length(gap(*solved_a, ca, *solved_b, cb)) >=
                                    0.75f * (ca.r + cb.r));
        }
    }
    // The steps: no capsule moving further than half the thinnest pair's thickness in one.
    const float turn_a = Body::AngleBetween(solved_a->q, a.q);
    const float turn_b = Body::AngleBetween(solved_b->q, b.q);
    const float moved = Body::Length(a.p - solved_a->p) + turn_a * ReachOf(sa) +
                        Body::Length(b.p - solved_b->p) + turn_b * ReachOf(sb);
    const float step = 0.5f * (thin_a + thin_b);
    const int steps =
        step > 0.0f && moved > step
            ? std::min(SeparateSteps, static_cast<int>(std::ceil(moved / step)))
            : 1;
    Vec3 push_a{}, push_b{};
    bool touched = false;
    for (int k = 1; k <= steps; ++k) {
        const float f = static_cast<float>(k) / static_cast<float>(steps);
        Pose x{Body::Lerp(solved_a->p, a.p, f) + push_a,
               turn_a > 1e-5f && k < steps ? Body::Slerp(solved_a->q, a.q, f) : a.q};
        Pose y{Body::Lerp(solved_b->p, b.p, f) + push_b,
               turn_b > 1e-5f && k < steps ? Body::Slerp(solved_b->q, b.q, f) : b.q};
        for (int pass = 0; pass < SeparatePasses; ++pass) {
            // This step's pairs pressed into each other (their normals and depths; only the ones
            // counted are read), the deepest.
            struct Pressed {
                float x, y, z, depth;
                Vec3 n() const {
                    return {x, y, z};
                }
            };
            std::array<Pressed, Pairs> pressed;
            int count = 0;
            int deepest = 0;
            Pair best{};
            for (int i = 0; i < na; ++i) {
                for (int j = 0; j < nb; ++j) {
                    if (inside[index(i, j)]) {
                        continue;
                    }
                    Capsule ca{}, cb{};
                    CapsuleOf(sa, i, ca);
                    CapsuleOf(sb, j, cb);
                    const Vec3 a0 = At(x, ca.a);
                    const Vec3 a1 = At(x, ca.b);
                    const Vec3 b0 = At(y, cb.a);
                    const Vec3 b1 = At(y, cb.b);
                    float s = 0.0f;
                    float t = 0.0f;
                    Closest(a0, a1, b0, b1, s, t);
                    const Vec3 pa = Body::Lerp(a0, a1, s);
                    const Vec3 pb = Body::Lerp(b0, b1, t);
                    const Vec3 d = pa - pb;
                    const float reach = ca.r + cb.r;
                    const float dist2 = Body::LengthSq(d);
                    // Touching or nearly (within 5% of their thickness): one another's push
                    // could press it in, so it is weighed with them.
                    const float near_by = reach * 1.05f;
                    if (dist2 >= near_by * near_by) {
                        continue;
                    }
                    const float dist = std::sqrt(dist2);
                    // Lines meeting exactly (not after a shallow step: a tie): the way they last
                    // touched, else across both of them.
                    const Vec3 n =
                        dist > reach * 1e-4f ? d * (1.0f / dist)
                        : touched            ? out.normal
                        : side != nullptr    ? *side
                                             : Body::Normalize(Body::Cross(a1 - a0, b1 - b0),
                                                               Body::AnyPerpendicular(a1 - a0));
                    if (count == 0 || reach - dist > best.depth) {
                        best = {ca, cb, pa, pb, n, s, t, reach, reach - dist};
                        deepest = count;
                    }
                    pressed[static_cast<std::size_t>(count++)] = {n.x, n.y, n.z, reach - dist};
                }
            }
            // None pressed in by more than a 1000th of their thickness (a push's rounding): apart.
            const float tolerance = best.reach * 1e-3f;
            if (count == 0 || best.depth <= tolerance) {
                break;
            }
            if (!touched) {
                first_contact(best);
                touched = true;
            }
            note(best);
            // Out of the deepest; if that leaves another pressed in (a crook: out of one is into
            // the other), out of both at once: the least push that clears both.
            Vec3 push = best.normal * best.depth;
            int other = -1;
            float worst = tolerance;
            for (int c = 0; c < count; ++c) {
                const Pressed& q = pressed[static_cast<std::size_t>(c)];
                const float left = q.depth - Body::Dot(q.n(), push);
                if (c != deepest && left > worst) {
                    worst = left;
                    other = c;
                }
            }
            if (other >= 0) {
                const Pressed& q = pressed[static_cast<std::size_t>(other)];
                const Vec3 qn = q.n();
                const float c = Body::Dot(best.normal, qn);
                const float det = 1.0f - c * c;
                const float alpha = det > 1e-4f ? (best.depth - c * q.depth) / det : -1.0f;
                const float beta = det > 1e-4f ? (q.depth - c * best.depth) / det : -1.0f;
                if (alpha >= 0.0f && beta >= 0.0f) {
                    push = best.normal * alpha + qn * beta;
                } else if (best.depth - c * q.depth <= tolerance) {
                    push = qn * q.depth; // out of the other clears the deepest too
                }
            }
            const Vec3 pa = push * out.a_share;
            const Vec3 pb = push * -(1.0f - out.a_share);
            x.p = x.p + pa;
            y.p = y.p + pb;
            push_a = push_a + pa;
            push_b = push_b + pb;
        }
    }
    if (!touched || Body::Length(push_a) + Body::Length(push_b) > cross) {
        return {}; // apart all the way, or held too far from the controllers: they pass
    }
    a.p = a.p + push_a;
    b.p = b.p + push_b;
    return out;
}

} // namespace VR::Solid
