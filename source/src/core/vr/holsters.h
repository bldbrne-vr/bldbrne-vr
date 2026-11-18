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
#include "core/vr/belt_vial.h"
#include "core/vr/shoulder_switch.h"

namespace VR::Holsters {
using P3 = Belt::P3;
inline float Distance(P3 a, P3 b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                     (a.z - b.z) * (a.z - b.z));
}
// Right: vials. Left pocket/chest: equipped quick slots 1/2, in game inventory order. The fourth,
// the right shoulder (shoulder_vial, the tester 2026-10-26: the left hand over the right shoulder
// draws a vial, a third way beside the pouch and the arc), is vials too and has no mount drawn:
// its zone is the shoulder switch's own (ShoulderSwitch::ZoneAt, from the headset; the hunter's
// chest bone's 9 cm ball before was hard to find), taken only where no pocket holds the hand.
inline constexpr int SlotCount = 4;
inline constexpr int WaistSlot = 1; // the hip pocket (quick slot 1)
inline constexpr int ShoulderSlot = 3;
inline constexpr std::array<int, SlotCount> InventoryAssignment{1, 2, 3, 1};
struct Config {
    std::array<int, SlotCount> assignment{InventoryAssignment}; // 0 disables a target
    float scale{1}, reach{0.12f};
};
// Tracking-space positions shared by hit testing and the world renderer. No independent
// renderer placement: the visible case and the target can never drift apart.
struct Layout {
    std::array<P3, SlotCount> centre{};
    std::array<int, SlotCount> assignment{};
    int hover{-1};
    bool valid{false}, entered{false};
    // Input-only proximity to the existing volumes, including hover hysteresis.
    // Does not move the visible mounts or enlarge the actual pickup zone.
    int near_assignment{0};
    std::uint32_t late_mask{0};
    // Unit inward axis of the rendered vial pouch (toward the body), in tracking space.
    P3 pouch_back{0, 0, 1};
    // Unit right/up/back axes of the rendered chest mount, in tracking space.
    std::array<P3, 3> chest_axes{P3{1, 0, 0}, P3{0, 1, 0}, P3{0, 0, 1}};
};
// The rendered positions, returned to the left-hand pickup test. Published by the
// render-camera hook, so grabbing uses the actual displayed mount, not an HMD estimate.
struct Targets {
    Layout layout{};
    std::int64_t ns{};
    std::uint32_t space{};
};
inline bool Fresh(const Targets& t, std::int64_t now, std::uint32_t space) {
    if (!t.layout.valid || t.space != space || t.ns <= 0 || now < t.ns || now - t.ns > 100'000'000)
        return false;
    for (const auto& p : t.layout.centre)
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            return false;
    return true;
}
// Mounted pockets need current rendered positions. The shoulder is independently located
// from the current headset/torso pose; never carry stale pocket coordinates into its fallback.
inline Layout PickupLayout(const Layout& mounted, bool fresh, const Config& config,
                           bool shoulder_available) {
    bool pockets = fresh && mounted.valid;
    for (int i = 0; i < ShoulderSlot; ++i)
        pockets = pockets && mounted.assignment[i] == config.assignment[i];
    Layout out = pockets ? mounted : Layout{};
    out.assignment[ShoulderSlot] = shoulder_available ? config.assignment[ShoulderSlot] : 0;
    out.valid = pockets || out.assignment[ShoulderSlot] > 0;
    return out;
}
// Additional vertical half-height for the chest grab ellipsoid (metres).
inline constexpr float ChestVerticalExtraM = .02f;
// The chest mount went ChestLoweredM lower (body_mounts.h, 2026-11-05); its zone's lower half is
// that much shorter, so its bottom edge stays where it was, as far from the vial pouch and the
// waist pocket in every pose as before, within 2 mm (c0000's bind pose: 8.5 and 11.9 cm; bent
// forward 30 degrees, 2.5 and 4.5; 40 degrees, 0.3 and 1.8).
inline constexpr float ChestLoweredM = .03f;
inline constexpr float ChestDepthReductionM = .09f;
// The chest zone's inner half (toward the hunter's spine, +back) reaches this much deeper; its front
// face stays where it was (2026-11-10: "make it deeper but do not move it forward"): at
// the default reach 5.5 cm in front of its centre and 11.5 behind it, 17 cm deep instead of 11, its
// back face 1 cm in front of the chest bone. A hand at your chest where the hunter's is further
// forward (a bigger player, a lean) still takes it; a hand stopping short still does not.
inline constexpr float ChestInwardM = .06f;
// ... and its outer half (out in front of the chest) this much: 7 cm in front of its centre instead
// of 5.5 (2026-11-10: "add 1.5cm in front of the chest pocket too to make it a little
// easier to grab"); 18.5 cm deep in all at the default reach.
inline constexpr float ChestFrontM = .015f;
inline constexpr float PouchVerticalExtraM = .01f;
// The vials' pouch zone's reach toward the left mount (metres, Tracker::Follow).
inline constexpr float PouchLeftM = .06f;
inline constexpr float PouchBackM = .03f;
inline constexpr double EarlySqueezeS = .15;
inline constexpr double LateSqueezeS = .08;
inline constexpr float EarlyShellM = .04f;
inline constexpr float LateShellM = .02f;

// The hunter going somewhere (walking, running, a dodge or a backstep: his own ground speed, from
// the body anchor's W, whatever moved him): the waist pocket's touch tick is left out while he
// does (2026-11-03: "it keeps vibrating in an annoying way as I run"; standing still it
// ticks, and the grab always works). His speed over at least SampleNs, held HoldNs past the last
// fast sample (a stride's slow moment, a stop); a gap of MaxGapNs or more (a load, a pause)
// measures nothing. Fixed size, no history.
class Moving {
public:
    static constexpr float SpeedMps = 0.5f;
    static constexpr std::int64_t SampleNs = 50'000'000;
    static constexpr std::int64_t HoldNs = 350'000'000;
    static constexpr std::int64_t MaxGapNs = 500'000'000;
    // `w`: his translation at `ns` (game units, world), `scale` game units per metre; `now` the
    // clock to answer for. True while he moves (or did HoldNs ago).
    bool Update(bool valid, std::int64_t ns, P3 w, float scale, std::int64_t now) {
        if (!valid || ns <= 0 || !(scale > 0.0f) || !std::isfinite(scale) ||
            !std::isfinite(w.x) || !std::isfinite(w.z)) {
            have_ = false;
            return now < until_;
        }
        if (!have_ || ns < from_ns_ || ns - from_ns_ >= MaxGapNs) {
            have_ = true;
            from_ns_ = ns;
            from_ = w;
            return now < until_;
        }
        const std::int64_t dt = ns - from_ns_;
        if (dt >= SampleNs) {
            const float dx = w.x - from_.x, dz = w.z - from_.z;
            const float mps = std::sqrt(dx * dx + dz * dz) / scale /
                              (static_cast<float>(dt) * 1e-9f);
            if (mps > SpeedMps) {
                until_ = ns + HoldNs;
            }
            from_ns_ = ns;
            from_ = w;
        }
        return now < until_;
    }

private:
    P3 from_{};
    std::int64_t from_ns_{0};
    std::int64_t until_{0};
    bool have_{false};
};

// Pickup/holding only: accept a currently valid inferred pose briefly after position tracking
// drops. Velocity is deliberately irrelevant here; stabs and throws keep their own tracking gate.
class VialPoseGrace {
public:
    bool Update(std::int64_t now, std::uint32_t space, bool enabled, bool tracked,
                bool pose_valid) {
        if (space != space_ || !enabled || !pose_valid || now <= 0 || now < tracked_ns_)
            Reset();
        space_ = space;
        if (!enabled || !pose_valid || now <= 0)
            return false;
        if (tracked)
            tracked_ns_ = now;
        return tracked_ns_ > 0 && now - tracked_ns_ <= 150'000'000;
    }
    void Reset() {
        *this = {};
    }

private:
    std::int64_t tracked_ns_{};
    std::uint32_t space_{};
};

// Distance to a bounded parallelogram [0,left] + [0,back]. Sweeping the existing
// pouch volume inward preserves all its old points and its outward boundary.
// Inputs already use the pouch's vertical distance weighting. Edges also handle
// zero or parallel spans (missing left mount or inward axis).
inline float PouchPatchDistance(P3 p, P3 left, P3 back) {
    const auto dot = [](P3 a, P3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
    const auto sub = [](P3 a, P3 b) { return P3{a.x-b.x, a.y-b.y, a.z-b.z}; };
    const auto edge = [&](P3 origin, P3 span) {
        const auto d = sub(p, origin);
        const float length2 = dot(span, span);
        const float t = length2 > 1e-12f ? std::clamp(dot(d, span)/length2, 0.f, 1.f) : 0.f;
        return Distance(d, {span.x*t, span.y*t, span.z*t});
    };
    float best = std::min({edge({}, left), edge(back, left), edge({}, back), edge(left, back)});
    const float aa = dot(left,left), bb = dot(back,back), ab = dot(left,back);
    const float det = aa*bb-ab*ab;
    if (det > 1e-12f) {
        const float ap = dot(left,p), bp = dot(back,p);
        const float u = (ap*bb-bp*ab)/det, v = (bp*aa-ap*ab)/det;
        if (u >= 0 && u <= 1 && v >= 0 && v <= 1)
            best = std::min(best, Distance(p, {left.x*u+back.x*v, left.y*u+back.y*v,
                                               left.z*u+back.z*v}));
    }
    return best;
}

class Tracker {
public:
    void Reset() {
        *this = {};
    }
    // `shoulder`: the right shoulder's zone (ShoulderSwitch::ZoneAt, right side), or none (no head
    // pose, or the left hand belongs to a weapon grab). Inferred poses may target vials only.
    Layout Follow(Layout layout, P3 hand, bool enabled, float reach, float scale,
                  const ShoulderSwitch::Zone* shoulder = nullptr, bool vials_only = false) {
        if (!enabled || !layout.valid || !Finite(hand)) {
            Reset();
            return {};
        }
        // Skipped, not unassigned: the layout stays equal to the drawn one (its hover highlight).
        const auto live = [&](int i) {
            return layout.assignment[i] > 0 && (i != ShoulderSlot || shoulder != nullptr);
        };
        scale = std::isfinite(scale) ? std::clamp(scale, .6f, 1.5f) : 1.f;
        reach = std::isfinite(reach) ? std::clamp(reach, .06f, .18f) : .12f;
        // Beyond `reach`: pouch +4 cm, left pocket +5.5 cm, chest +2.5 cm; the shoulder, its zone's.
        // Relative to 60af5bfb: pouch +3 cm, pocket +2 cm; chest grows vertically below.
        const auto radius = [&](int i) {
            if (i == ShoulderSlot)
                return shoulder != nullptr ? ShoulderSwitch::RadiusM * shoulder->k : 0.f;
            const int a = layout.assignment[i];
            const float slot_extra = i == 1 ? .04f : i == 2 ? .01f : 0.f;
            return reach * scale + (a == 1 ? .04f : a > 1 ? .015f + slot_extra : 0.f);
        };
        // The vials' pouch on the right hip reaches PouchLeftM further toward the left mount
        // (2026-10-15: the tester, "extend it 6cm towards the left", easier to grab): the distance to
        // that segment swept 3 cm inward along the rendered pouch's back axis.
        // Missing directions fall back to the remaining segment or the centre.
        // The chest's zone (its shallow axis perpendicular to the chest through turns and lean);
        // `grown`: with ChestInwardM / ChestFrontM, else as it was before them.
        const auto chest_distance = [&](bool grown) {
            const auto c = layout.centre[2];
            const P3 delta{hand.x - c.x, hand.y - c.y, hand.z - c.z};
            const auto along = [&](P3 axis) {
                return delta.x * axis.x + delta.y * axis.y + delta.z * axis.z;
            };
            const float r = radius(2);
            const float dx = along(layout.chest_axes[0]);
            const float up = along(layout.chest_axes[1]);
            const float dy = up * r / (r + ChestVerticalExtraM - (up < 0 ? ChestLoweredM : 0));
            // Keep a usable positive depth at the smallest reach/body-scale settings; deeper into
            // the body (ChestInwardM) and a little out in front (ChestFrontM).
            const float in = along(layout.chest_axes[2]);
            const float depth = std::max(r - ChestDepthReductionM, .02f) +
                                (grown ? (in > 0 ? ChestInwardM : ChestFrontM) : 0.f);
            const float dz = in * r / depth;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        // A hand inside the right shoulder's vial zone: the chest as it was before its grown depth
        // (review, 2026-11-10: a reach for the shoulder's vial stopping at the collarbone drew the
        // chest's item instead). Where they overlapped before, the chest still wins, as then.
        const bool at_shoulder = live(ShoulderSlot) &&
                                 ShoulderSwitch::Reach(*shoulder, {hand.x, hand.y, hand.z}) <
                                     radius(ShoulderSlot);
        const auto distance = [&](int i) {
            if (i == ShoulderSlot) {
                return ShoulderSwitch::Reach(*shoulder, {hand.x, hand.y, hand.z});
            }
            if (i == 2 && layout.assignment[i] > 1) {
                return chest_distance(!at_shoulder);
            }
            if (i != 0 || layout.assignment[0] != 1) {
                return Distance(hand, layout.centre[i]);
            }
            const P3 c = layout.centre[0];
            const float lx = layout.centre[1].x - c.x, lz = layout.centre[1].z - c.z;
            const float len = std::hypot(lx, lz);
            P3 left{}, back{};
            if (len > .05f && std::isfinite(len)) {
                const float k = std::min(PouchLeftM * scale, len) / len;
                left = {lx*k, 0, lz*k};
            }
            const float back_len = Distance(layout.pouch_back, {});
            if (Finite(layout.pouch_back) && back_len > 1e-4f) {
                const float k = PouchBackM * scale / back_len;
                back = {layout.pouch_back.x*k, layout.pouch_back.y*k, layout.pouch_back.z*k};
            }
            const float r = radius(0), vertical = r / (r + PouchVerticalExtraM);
            back.y *= vertical;
            return PouchPatchDistance({hand.x-c.x, (hand.y-c.y)*vertical, hand.z-c.z}, left, back);
        };
        int next = -1;
        layout.near_assignment = 0;
        layout.late_mask = 0;
        float nearest = 1e9f;
        std::array<float, SlotCount> distances{};
        for (int i = 0; i < SlotCount; ++i) {
            const int a = layout.assignment[i];
            if (a <= 0 || a > 7 || !live(i))
                continue;
            distances[i] = distance(i);
            const float r = radius(i);
            // Account for the stretched vertical axes: the extra shell must remain
            // within 2 cm (late) / 4 cm (early) in physical tracking space.
            const float vertical = i == 2 ? r / (r + ChestVerticalExtraM)
                                   : i == 0 ? r / (r + PouchVerticalExtraM) : 1.f;
            const float gap = (distances[i] - r - .03f * scale) / vertical;
            if (gap <= LateShellM)
                layout.late_mask |= 1u << a;
            if (gap <= EarlyShellM && gap < nearest) {
                nearest = gap;
                layout.near_assignment = a;
            }
        }
        const auto keeps = [&](int i) {
            return i == hover_ && live(i) && distances[i] <= radius(i) + .03f * scale;
        };
        if (hover_ >= 0 && hover_ != ShoulderSlot && keeps(hover_))
            next = hover_;
        if (next < 0) {
            float best = std::max({radius(0), radius(1), radius(2)});
            for (int i = 0; i < ShoulderSlot; ++i) {
                const float d = distances[i];
                if (live(i) && d < radius(i) && d < best) {
                    next = i;
                    best = d;
                }
            }
        }
        // The shoulder's zone is broad (the shoulder switch's): only where no pocket holds the hand.
        if (next < 0 && live(ShoulderSlot) &&
            (distances[ShoulderSlot] < radius(ShoulderSlot) || keeps(ShoulderSlot)))
            next = ShoulderSlot;
        const int geometric_hover = next;
        if (vials_only) {
            // Resolve normal pocket priority first. Losing tracking must not turn a quick-item
            // pocket into a vial target merely because the vial zone overlaps it.
            if (next >= 0 && layout.assignment[next] != 1) {
                next = -1;
                layout.near_assignment = 0;
                layout.late_mask = 0;
            }
            if (layout.near_assignment != 1)
                layout.near_assignment = 0;
            layout.late_mask &= 1u << 1;
        }
        layout.entered = next >= 0 && next != hover_;
        layout.hover = next;
        hover_ = geometric_hover; // retain normal-pocket hysteresis through an inference-only frame
        return layout;
    }

private:
    static bool Finite(P3 p) {
        return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
    }
    int hover_{-1};
};

// A squeeze can belong to one nearby pocket for at most 150 ms. Once reserved,
// cancellation/expiry never hands that same squeeze to another pocket or the arc.
// All times use the input clock; callers clear this state across context changes.
class GrabTiming {
public:
    struct Result {
        int assignment{0};
        bool reserved{false};
    };
    void Reset() { *this = {}; }
    bool VialPending(double now, bool held) const {
        return pending_ == 1 && held && now >= pressed_t_ && now - pressed_t_ <= EarlySqueezeS;
    }
    Result Update(double now, bool edge, bool held, int hover, int near_slot,
                  std::uint32_t late_mask, bool available = true) {
        if (last_t_ >= 0 && (now < last_t_ || now - last_t_ > EarlySqueezeS))
            Reset();
        last_t_ = now;
        if (!held)
            pending_ = 0;
        if (!available) {
            // A brief pose/target gap cannot draw anything. Only an already reserved vial
            // squeeze survives, within its original deadline; recovery never creates an edge.
            recent_ = 0;
            if (!VialPending(now, held))
                pending_ = 0;
            return {0, pending_ == 1};
        }
        if (hover > 0) {
            recent_ = hover;
            recent_t_ = now;
        } else if (now - recent_t_ > LateSqueezeS || !(late_mask & (1u << recent_))) {
            recent_ = 0;
        }
        if (edge) {
            pending_ = 0;
            if (hover > 0 || recent_ > 0) {
                const int pick = hover > 0 ? hover : recent_;
                recent_ = 0;
                return {pick, true};
            }
            if (near_slot > 0) {
                pending_ = near_slot;
                pressed_t_ = now;
            }
        }
        if (pending_ > 0) {
            const int target = pending_;
            if (!held || now - pressed_t_ > EarlySqueezeS || near_slot != target ||
                (hover > 0 && hover != target)) {
                pending_ = 0;
            } else if (hover == target) {
                pending_ = recent_ = 0;
                return {target, true};
            }
            return {0, true};
        }
        return {};
    }

private:
    int pending_{0}, recent_{0};
    double pressed_t_{0}, recent_t_{0}, last_t_{-1};
};
} // namespace VR::Holsters
