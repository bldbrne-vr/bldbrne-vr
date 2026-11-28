// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Chalice Dungeon levers struck into action (2026-11-26: "only for chalice dungeon
// levers - we keep all the options we have + we make them smackable (weapon attack or being shot
// by the player = activation IF the lever can be activated)"; a turning lever's bar "can be
// grabbed but it is too big too far to be pulled"). A blow of the right weapon's blade through a
// Chalice lever's stem or bar, or one of the player's shots along a clear line to it, operates it
// as the game's own button does: its own clip plays from rest and its event runs (the door opens,
// the lift comes, the wheel turns), without the hunter's animation. Only the levers measured for
// the Chalice Dungeons (lever_models.h: the lift-call and gate levers o290010 / 12 / 14, the door
// levers o290020 / 22 / 24, the turning levers o293110 / 12 / 14), only while the game would let
// that lever be used (bloodborne_levers.cpp: its row selectable, at rest, permitted, interaction
// not blocked). The hand pull and the button are kept as they are.
//
// This file is pure (the blade and shot geometry, the inbox the melee and gun code post to); the
// live side, scanning the nearby Chalice levers and dispatching the game's acceptance, is in
// bloodborne_levers.cpp (ServeSmack). tests/test_physical_lever.cpp drives the geometry.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>

#include "core/vr/game_area.h"
#include "core/vr/lever_channel.h"
#include "core/vr/physical_lever.h"
#include "core/vr/visceral_clearance.h"

namespace VR::Lever::Smack {

// The Chalice levers' models are all o29xxxx (the Chalice Dungeons' own object range).
inline bool Chalice(const char *model_name) {
    return model_name && std::strncmp(model_name, "o29", 3) == 0;
}

// How long a Chalice lever's own clip holds still before it moves: the game's button pull plays it
// with the hunter's animation, and it waits for his hand on it (the clips, a000_000001: the lift
// and door levers first move 0.5 degrees at 1.33 s, the turning levers at 0.50 s). A struck lever's
// clip goes on from just before (bloodborne_levers.cpp, as a door's), so it moves at once; the
// user, 2026-11-26: "there is a delay from when it gets hit to when the lever is activated".
inline float ClipLeadS(const char *model_name) {
    if (!Chalice(model_name))
        return 0.0f;
    if (std::strncmp(model_name, "o29311", 6) == 0)
        return 0.45f; // the turning levers o293110 / 12 / 14
    if (std::strncmp(model_name, "o29001", 6) == 0 || std::strncmp(model_name, "o29002", 6) == 0)
        return 1.30f; // the lift levers o290010 / 12 / 14, the door levers o290020 / 22 / 24
    return 0.0f;
}

// Generous on purpose: a blow that clearly meets the lever counts. Metres (game units).
inline constexpr float LeverRadiusM = 0.12f;   // the stem or the bar
inline constexpr float BladeRadiusM = 0.04f;   // the weapon's edge
inline constexpr float BulletRadiusM = 0.05f;  // a shot's line
inline constexpr float MinTipSpeedMps = 1.8f;  // a swing, not a touch: the blade tip's speed
inline constexpr float ShotRangeM = 40.0f;     // a shot further than this strikes nothing
inline constexpr float BladeReachM = 4.0f;     // levers nearer than this to the blade are tested
inline constexpr std::int64_t BladeFreshNs = 120'000'000; // a blade sample older is no blow
// A blade's path is drawn between two samples only this close in time (the melee tick's own limit
// for a velocity, bloodborne_melee.cpp BladeTickMaxNs); further apart nothing is drawn between them.
inline constexpr std::int64_t PairMaxNs = 100'000'000;
// A shot or a solid weapon's strike older than this is served no more (a pass that could not take
// it, then a later one, must not replay it).
inline constexpr std::int64_t ShotFreshNs = 500'000'000;
// After a lever is struck and the game took it, nothing strikes it again for this long (its row
// runs at once anyway; this does not rely on that).
inline constexpr std::int64_t StruckAgainNs = 2'000'000'000;
// A parked turning lever (the hand left its bar part-turned) is put back at rest first: the blow
// waits this long for it.
inline constexpr std::int64_t PendingNs = 1'500'000'000;
// Nearby Chalice levers are looked up again after this long (a blow or shot uses the last list).
inline constexpr std::int64_t RescanNs = 400'000'000;

// What of a lever a blow or a shot strikes: its stem from its lowest grip (or its hinge) to its
// handle, at rest. Game units, as the rig.
struct Segment {
    Vec3 a{}, b{};
};
inline Segment Strikeable(const Rig &rig) {
    return {rig.pivot + (rig.shaft ? rig.shaft_start : Vec3{}), HandleAt(rig, 0.0f)};
}

// The squared distance between two segments (p1 -> q1, p2 -> q2).
inline float SegmentDistanceSq(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2) {
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = Body::Dot(d1, d1), e = Body::Dot(d2, d2), f = Body::Dot(d2, r);
    float s = 0.0f, t = 0.0f;
    if (a <= 1e-12f && e <= 1e-12f) {
        return Body::Dot(r, r);
    }
    if (a <= 1e-12f) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = Body::Dot(d1, r);
        if (e <= 1e-12f) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = Body::Dot(d1, d2), denom = a * e - b * b;
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
    }
    const Vec3 gap = (p1 + d1 * s) - (p2 + d2 * t);
    return Body::Dot(gap, gap);
}

// The right weapon's blade (bloodborne_melee.cpp TrackBlade): base -> tip and their velocities,
// game units and units a second. `follows`: this sample carries on from the one posted before it
// (no gap, turn or weapon change in between: TrackBlade posts a sample that does not with no
// velocity), so the path between the two is the blade's.
struct Blade {
    Vec3 base{}, tip{}, base_vel{}, tip_vel{};
    std::int64_t ns{};
    bool follows{};
};

// A blow: the blade now, or the path its tip or its middle took since the sample before (when it
// follows on from it, within PairMaxNs), passes within the stem's and the edge's radii of the
// stem, while its tip moves at least MinTipSpeedMps. `scale`: game units a metre (metres).
inline bool BladeStrikes(const Segment &stem, const Blade &now, const Blade &before,
                         float scale = 1.0f) {
    if (!(Body::Length(now.tip_vel) >= MinTipSpeedMps * scale))
        return false;
    const float r = (LeverRadiusM + BladeRadiusM) * scale;
    const float r2 = r * r;
    if (SegmentDistanceSq(stem.a, stem.b, now.base, now.tip) <= r2)
        return true;
    if (!now.follows || before.ns == 0 || before.ns >= now.ns || now.ns - before.ns > PairMaxNs)
        return false;
    if (SegmentDistanceSq(stem.a, stem.b, before.tip, now.tip) <= r2)
        return true;
    const Vec3 mid0 = (before.base + before.tip) * 0.5f, mid1 = (now.base + now.tip) * 0.5f;
    return SegmentDistanceSq(stem.a, stem.b, mid0, mid1) <= r2;
}

// One of the player's shots (bloodborne_gun.cpp): where it leaves the gun and its unit direction.
struct Shot {
    Vec3 start{}, dir{};
    std::int64_t ns{};
};

// What a shot's line meets this near the stem is the lever's own body, not a wall in between:
// the door levers' wheel rim stands 0.29 m in front of the stem as the hunter faces it (mock,
// 2026-11-26: a shot on the stem was refused for it).
inline constexpr float OwnBodyM = 0.45f;
// A solid weapon's strike counts on less: the lever's own bar and collar, not a wall or a gate
// beside it (the door levers' rim met at 0.29 m still counts).
inline constexpr float ContactBodyM = 0.33f;
inline bool OwnBody(const Segment &stem, Vec3 met, float scale = 1.0f, float within = OwnBodyM) {
    const float r = within * scale;
    return SegmentDistanceSq(stem.a, stem.b, met, met) <= r * r;
}

// How far along a shot (game units) the stem is first met, within ShotRangeM; none otherwise.
inline std::optional<float> ShotStrikes(const Segment &stem, const Shot &shot,
                                        float scale = 1.0f) {
    const float length = Body::Length(shot.dir);
    if (!(length > 1e-6f))
        return std::nullopt;
    const Vec3 dir = shot.dir * (1.0f / length);
    const auto hit = GrabCamera::RayCapsule(
        shot.start, dir, {stem.a, stem.b, (LeverRadiusM + BulletRadiusM) * scale});
    if (!hit || *hit > ShotRangeM * scale)
        return std::nullopt;
    return std::max(*hit, 0.0f);
}

// A weapon driven into something solid (the solid weapons' strike, weapon_wall.h: StrikeSpeed into
// the surface): where it met it, game units. A solid weapon stops at a lever's collision, which
// stands out from the stem (0.29 m on the door levers, mock 2026-11-26), short of BladeStrikes'
// reach: one that lands on a Chalice lever's own body (OwnBody) is a blow all the same.
struct Contact {
    Vec3 point{};
    float speed{}; // m/s into the surface
    std::int64_t ns{};
};

// Whether the hunter stands in the Chalice Dungeons (the game's own map id: area 29).
inline bool InChalice() {
    return VR::GameArea::AreaSlot().load(std::memory_order_relaxed) == VR::GameArea::ChaliceArea;
}

// A blade or a solid weapon's strike wakes the lever side only this soon after the hand-pull
// scan last saw a Chalice lever near the hunter (bloodborne_levers.cpp Scan, twice a second while
// one is armed: CheckArmed). Swinging anywhere else, a dungeon's fights, costs nothing more (the
// user, 2026-11-26: "is there a way to minimize that?").
inline constexpr std::int64_t NearNs = 2'000'000'000;
// How near the hand-pull scan's nearest Chalice lever is to count (its stem or handle from the head
// or a hand, metres): the longest weapon's reach and then some.
inline constexpr float NearM = 4.5f;

// Whether a shot's line from `from` to `to` (game units) meets an enemy's hurt capsule (radius
// `radius` around the line): the game's bullet hits it first, never the lever behind it.
inline bool EnemyInTheWay(Vec3 from, Vec3 to, const GrabCamera::CollisionCapsule *capsules,
                          std::size_t count, float radius = BulletRadiusM) {
    for (std::size_t i = 0; capsules && i < count; ++i) {
        const auto &c = capsules[i];
        const float r = radius + c.radius;
        if (SegmentDistanceSq(from, to, c.a, c.b) <= r * r)
            return true;
    }
    return false;
}

// A blade sample and the one posted before it (`before.ns` 0: none kept).
struct BladePair {
    Blade now{}, before{};
};

// What the melee and gun code post and the object update reads (any threads). Every blade sample
// is kept (the last Ring of them), not only the newest: the object update runs less often than
// the blade is sampled, and a fast swing crosses a stem between two samples.
class Inbox {
public:
    static constexpr std::size_t Ring = 16;
    // Wakes asked for (the tests read it). Each asks the object update's native gate to enter
    // its host next frame through the nearby check it already watches (lever_channel.h; the lever
    // side's CheckArmed answers it, ServeSmack before it reads this inbox): the gate has no check
    // of its own for this, so the main game's frames run exactly as they did.
    std::atomic<std::uint32_t> wakes{0};
    // When the hand-pull scan last saw a Chalice lever near the hunter (steady ns; 0: never).
    std::atomic<std::int64_t> near_ns{0};

    void NoteLeverNear(std::int64_t ns) { near_ns.store(ns, std::memory_order_relaxed); }
    bool LeverNear(std::int64_t ns) const {
        const std::int64_t at = near_ns.load(std::memory_order_relaxed);
        return at != 0 && (ns >= at ? ns - at : at - ns) <= NearNs;
    }
    // Outside the Chalice Dungeons each post is one read of the area the game already publishes
    // and nothing more: no lock, no copy, no arithmetic (2026-11-26: "I need it to add
    // 0 performance cost in the main game").
    void PostBlade(const Blade &blade) {
        if (!InChalice())
            return;
        {
            std::scoped_lock lock{mutex_};
            ring_[blade_serial_ % Ring] = blade;
            ++blade_serial_;
        }
        if (Body::Length(blade.tip_vel) >= MinTipSpeedMps && LeverNear(blade.ns))
            Wake(blade.ns);
    }
    // The samples after serial `seen`, oldest first, each with the one before it (at most
    // Ring - 1 of them: older ones are gone). `latest`: the serial to pass next time.
    std::size_t BladesSince(std::uint64_t seen, std::array<BladePair, Ring> &out,
                            std::uint64_t &latest) const {
        std::scoped_lock lock{mutex_};
        latest = blade_serial_;
        const std::uint64_t oldest = latest > Ring ? latest - Ring : 0; // still in the ring
        // A full ring: its oldest sample only as the one before the next.
        const std::uint64_t first = latest > Ring ? std::max(seen, oldest + 1) : seen;
        std::size_t n = 0;
        for (std::uint64_t s = first; s < latest && n < out.size(); ++s)
            out[n++] = {ring_[s % Ring], s >= 1 && s - 1 >= oldest ? ring_[(s - 1) % Ring] : Blade{}};
        return n;
    }
    void PostShot(const Shot &shot) {
        if (!InChalice())
            return;
        {
            std::scoped_lock lock{mutex_};
            shot_ = shot;
            ++shot_serial_;
        }
        Wake(shot.ns);
    }
    // The newest shot after `seen` (its serial written back); false when none is newer.
    bool NewShot(std::uint64_t &seen, Shot &shot) const {
        std::scoped_lock lock{mutex_};
        if (shot_serial_ == seen)
            return false;
        seen = shot_serial_;
        shot = shot_;
        return true;
    }
    void PostContact(const Contact &contact) {
        if (!InChalice())
            return;
        {
            std::scoped_lock lock{mutex_};
            contact_ = contact;
            ++contact_serial_;
        }
        if (LeverNear(contact.ns))
            Wake(contact.ns);
    }
    // The newest contact after `seen` (its serial written back); false when none is newer.
    bool NewContact(std::uint64_t &seen, Contact &contact) const {
        std::scoped_lock lock{mutex_};
        if (contact_serial_ == seen)
            return false;
        seen = contact_serial_;
        contact = contact_;
        return true;
    }

private:
    void Wake(std::int64_t ns) {
        wakes.fetch_add(1, std::memory_order_relaxed);
        Channel().RequestNearbyCheck(ns > 0 ? ns : 1);
    }

    mutable std::mutex mutex_;
    std::array<Blade, Ring> ring_{};
    std::uint64_t blade_serial_{};
    Shot shot_{};
    std::uint64_t shot_serial_{};
    Contact contact_{};
    std::uint64_t contact_serial_{};
};
inline Inbox &Posted() {
    static Inbox inbox;
    return inbox;
}

} // namespace VR::Lever::Smack
