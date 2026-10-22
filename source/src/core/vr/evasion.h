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
#include <cstddef>

// Physical evasion (vr.ini physical_evasion=1). The player's real 6DoF lean, step and duck move
// the hunter's hurtboxes (the 16 ragdoll capsules the game's attack casts test) by the same
// amount the first-person camera moves, times `gain` (default 1.3, a little extra to cover the
// game's coarse hit shapes). No dodge animation, no i-frames, no buttons: a strike aimed at where
// you were misses for purely geometric reasons.
//
// Body profile: a lean up to `lean_m` pivots at the ankles (the head moves the full amount, the
// pelvis about two thirds, the feet nearly nothing); beyond it the whole body steps. A duck lowers
// the upper body (head fully, pelvis about half, legs not at all). Rising never lifts hurtboxes.
//
// Anti-exploit (enemies aim at the character root, so a permanent offset would be a free miss):
// a lean budget. A lean (sideways or down by `engage_m` from the stance reference) counts fully
// for `hold_s`; after that the stance follows the neck with time constant `hold_tau_s` whatever
// the head does (fidgeting does not keep it), and standing within `release_m` of the stance for
// `release_s`, or leaning the other way (across the recentre spot), recharges it. Small offsets
// (drift off the recentre spot) settle silently once the neck has been still for `settle_s`
// (`settle_tau_s`); settle_s = 0 turns all settling off.
// The fixed `gain` applies after the caps. A tracking glitch (a per-frame jump over `glitch_m`)
// is absorbed into the stance. The effect fades in over `fade_in_s` when the gates open (idle,
// locomotion, own attacks, guard, items) and out over `fade_out_s` when they close (game
// dodges, falls, grabs, damage, events, ladders, cinematics).
//
// Walls: the caller measures how far the head may go along the lean before a hurtbox touches the
// map (`Input::wall_m`, the game's own collision casts); the gained head offset, the largest of
// all bodies, stays `wall_margin_m` short of it, so no hurtbox is ever pushed into geometry. The
// limit drops at once and is released at `wall_rise_mps` along the same direction (rounding a door
// frame never snaps the hurtboxes 30 cm in one frame); a lean in a new direction uses its own
// measurement.
//
// The pose may already carry part of the lean (the body build's UpperPos shift `s`, which moves
// Spine01, Spine02, Head01 and the arms, by `shift_reach` of it; with body_posture=lean the trunk
// lean `l`, the eye's move, which moves each of them by `lean_reach` times its LeanReach share of
// it): that much is subtracted so each hurtbox ends at the vanilla animation body plus its profile
// offset, whatever the visual body does. IK-owned arms already sit at the real controllers, which
// carry your lean once: they only get the gain's extra ((gain - 1) of the profile), so the whole
// body ends at gain times the lean.
//
// A kept hit or dodge (arm_keep_on_hit / arm_keep_on_dodge, Input::look_only): the game's clip
// moves the body while the torso alignment keeps the shoulders under your head so the arms reach
// your controllers. That shift and lean are the look only: they are subtracted at full weight
// whatever the fade (the game's own animation decides where the trunk can be hit), the arms on IK
// stay where they are drawn. After the clip the full subtraction holds while the lean fades back
// in (Output::comp_weight), or eases out over fade_out_s if evasion stays off.
//
// Pure (no OpenXR or emulator dependencies) so unit tests can drive it
// (tests/test_evasion.cpp). Conventions: game world axes with +y up, meters, seconds.
namespace VR::Evade {

// Ragdoll bodies of the hunter (c0000), in the expected ragdoll order. The hook resolves the
// game's order by name and maps through this enum.
enum Body : size_t {
    Pelvis01,
    L_Thigh01,
    R_Thigh01,
    Spine01,
    L_Calf01,
    R_Calf01,
    Spine02,
    L_Foot01,
    R_Foot01,
    Head01,
    L_UpperArm01,
    R_UpperArm01,
    L_Forearm01,
    R_Forearm01,
    L_Hand01,
    R_Hand01,
    BodyCount,
};

inline constexpr std::array<const char*, BodyCount> BodyNames{
    "Pelvis01",    "L_Thigh01",   "R_Thigh01", "Spine01",  "L_Calf01",     "R_Calf01",
    "Spine02",     "L_Foot01",    "R_Foot01",  "Head01",   "L_UpperArm01", "R_UpperArm01",
    "L_Forearm01", "R_Forearm01", "L_Hand01",  "R_Hand01",
};

// Rest centre heights (m) of each capsule on the 1.58 m (head centre) hunter.
inline constexpr float HeadHeight = 1.58f;
inline constexpr std::array<float, BodyCount> RestHeight{
    1.01f, 0.74f, 0.74f, 1.14f, 0.35f, 0.35f, 1.28f, 0.08f,
    0.08f, 1.58f, 1.40f, 1.40f, 1.18f, 1.18f, 0.98f, 0.98f,
};

// Bodies the UpperPos bone shift moves (they already carry `s`).
constexpr bool UnderUpperPos(size_t b) {
    return b == Spine01 || b == Spine02 || b == Head01 || b >= L_UpperArm01;
}
constexpr bool IsLeftArm(size_t b) {
    return b == L_UpperArm01 || b == L_Forearm01 || b == L_Hand01;
}
constexpr bool IsRightArm(size_t b) {
    return b == R_UpperArm01 || b == R_Forearm01 || b == R_Hand01;
}

// The ragdoll follows a downward UpperPos shift only so far: on the mock (duck3, a held straight
// duck at the Clinic lamp, nothing counted) Head01 and Spine02 came 0.35-0.37 m down with the
// shift at 0.49, 0.60 and 0.80 m alike. Config::shift_reach (0.5) of a shift deeper than
// ShiftDownFollowM (0.72 m) would lift the hurtboxes above the vanilla body (+4 cm at the 0.80 m
// duck), so the downward shift they compensate stops there. A shallower one (all the 0.60 m the
// body's duck went before 2026-10-08 evening) is compensated as before, bit for bit.
inline constexpr float ShiftDownFollowM = 0.72f;
/// The vertical UpperPos shift the hurtboxes compensate (`y`, in `unit`s per metre: 1 = metres,
/// world_scale = game units).
inline float FollowedShiftY(float y, float unit = 1.0f) {
    return std::max(y, -ShiftDownFollowM * unit);
}

// The visual body's trunk lean (body_posture=lean) turns the upper body about the waist: the
// share of the eye's move that reaches a body under UpperPos, before Config::lean_reach. Its
// height over the waist against the head's, h (0..1), to the power 1.4: the game's ragdoll
// follows the spine's rotations more toward the top (measured in the mock, 25 cm sideways leans
// at the clinic lamp, the eye moved 19 cm by the lean: Head01 0.8 - 0.9 and Spine02 0.26 - 0.37
// of it, Pelvis01 none).
inline constexpr float WaistHeight = 1.0f;
inline constexpr float LeanReachPower = 1.4f;
inline float LeanReach(size_t b) {
    const float h =
        std::clamp((RestHeight[b] - WaistHeight) / (HeadHeight - WaistHeight), 0.0f, 1.0f);
    return std::pow(h, LeanReachPower);
}

// Lean weight of a body for a horizontal lean that pivots at the ankles.
constexpr float LeanWeight(size_t b) {
    return RestHeight[b] / HeadHeight;
}
// Duck weight: nothing below 0.5 m, the head fully.
inline float DuckWeight(size_t b) {
    return std::clamp((RestHeight[b] - 0.5f) / (HeadHeight - 0.5f), 0.0f, 1.0f);
}

struct Vec3 {
    float x{}, y{}, z{};
};

struct Config {
    float gain{1.3f};           // hurtbox offset per meter of counted lean
    float max_m{0.40f};         // horizontal lean cap (before gain)
    float lean_m{0.30f};        // ankle-pivot lean range; beyond it the whole body steps
    float duck_m{0.75f};        // counted head drop cap (before gain; 0.45, then 0.60 on 2026-10-08)
    float wall_margin_m{0.03f}; // hurtbox skin stays this far from a wall (body-sized casts)
    float wall_rise_mps{1.5f};  // a wall limit is released at most this fast (<= 0: at once)
    float hold_s{1.5f};         // a lean counts fully this long, then settles (0 = no hold limit)
    float hold_tau_s{0.5f};
    float engage_m{0.08f};  // a lean this far from the stance (sideways or down) uses the budget
    float release_m{0.05f}; // ... within this of the stance for release_s recharges it
    float release_s{0.2f};
    float settle_s{1.0f}; // drift: small offsets held still this long settle; 0 = never settle
    float settle_tau_s{1.0f};
    float settle_speed{0.3f}; // m/s: the neck counts as still below this
    float glitch_m{0.5f};     // per-frame neck jump treated as a tracking glitch
    float fade_in_s{0.15f};
    float fade_out_s{0.08f};
    float lost_fade_s{0.2f}; // tracking lost: hold, then fade out
    bool pelvis{true};       // false: Pelvis01 never moves
    // How much of the visual body's UpperPos shift reaches the hurtboxes under it: the game maps
    // the pose onto the ragdoll through a spine chain anchored at the pelvis, so a translated
    // upper body moves Spine02 and Head01 by about half of it (measured 0.50 forward, 0.53
    // sideways, the pelvis 0).
    float shift_reach{0.5f};
    // How much of the visual body's trunk lean reaches the hurtboxes (times LeanReach per body;
    // the head: measured 0.8 - 0.9).
    float lean_reach{0.85f};
};

struct Input {
    double t{};         // seconds
    bool enabled{true}; // physical_evasion on and the camera anchor valid (first person, driven)
    bool active{true};  // gates open (no dodge, fall, grab, damage, event, ladder, cinematic)
    bool tracked{true}; // headset position tracked
    bool recenter{false};
    Vec3 neck; // neck pivot offset from the recentre origin (world axes, m)
    // The gained head offset (m) along the current lean at which a hurtbox would touch the map
    // (the caller's casts: body-sized spheres from the root, and from the hurtboxes where they
    // are); < 0 = no wall within reach.
    float wall_m{-1.0f};
    Vec3 pose_shift; // the UpperPos shift already in this frame's pose (world, m)
    Vec3 pose_lean;  // the eye's move by the trunk lean already in this frame's pose (world, m)
    float ik_left{}; // arm IK weights (0 = game animation, 1 = real controller)
    float ik_right{};
    // pose_shift and pose_lean are the look only (a kept hit or dodge): cancel them at full weight.
    bool look_only{false};
};

struct Output {
    std::array<Vec3, BodyCount> offset{}; // per-body hurtbox offset (world, m)
    float weight{};                       // current fade weight
    Vec3 counted;                         // lean after settle (before caps)
    float lean_want_m{};       // horizontal lean after caps, before the wall (before gain)
    float lean_m{};            // horizontal lean after caps and wall (before gain)
    float duck_m{};            // head drop after caps (<= 0, before gain)
    float settled_m{};         // horizontal stance reference length
    float wall_limit_m{-1.0f}; // the wall's limit on the gained head offset (m), -1 = none
    float comp_weight{};       // the weight pose_shift / pose_lean are subtracted with
    float held_s{};            // how long the current lean has used the hold budget
    bool hold_settling{false}; // the budget is spent: the stance follows the neck
    bool wall_limited{false};
    bool glitch{false};
};

// Neck pivot from the eye centre and head orientation (OpenXR view axes: +y up, -z forward):
// the eyes sit 7.5 cm above and 8 cm in front of the neck. Turning or nodding the head moves
// the eyes but not the neck, so it never moves the torso hurtboxes.
inline Vec3 NeckFromEye(const Vec3& eye, float qx, float qy, float qz, float qw) {
    constexpr float Up = 0.075f, Fwd = 0.08f;
    const float vx = 0.0f, vy = Up, vz = -Fwd;
    // v' = v + 2w(q x v) + 2 q x (q x v)
    const float cx = qy * vz - qz * vy;
    const float cy = qz * vx - qx * vz;
    const float cz = qx * vy - qy * vx;
    const float rx = vx + 2.0f * (qw * cx + qy * cz - qz * cy);
    const float ry = vy + 2.0f * (qw * cy + qz * cx - qx * cz);
    const float rz = vz + 2.0f * (qw * cz + qx * cy - qy * cx);
    return {eye.x - rx, eye.y - ry, eye.z - rz};
}

class Model {
public:
    Output Step(const Input& in, const Config& cfg) {
        Output out{};
        const float dt = have_t ? static_cast<float>(std::max(in.t - last_t, 0.0)) : 0.0f;
        have_t = true;
        last_t = in.t;

        if (!in.enabled || in.recenter) {
            ResetLean();
            weight = 0.0f;
            look_w = 0.0f;
            return out;
        }

        // Tracking: hold the last counted lean while lost and fade out.
        Vec3 neck = in.neck;
        if (!in.tracked) {
            lost_s += dt;
            neck = have_neck ? last_neck : Vec3{};
        } else {
            lost_s = 0.0f;
            if (have_neck) {
                const float jump = Len(Sub(neck, last_neck));
                if (jump > cfg.glitch_m) {
                    // Absorb the jump: the counted lean stays continuous.
                    stance = Add(stance, Sub(neck, last_neck));
                    out.glitch = true;
                }
                const float speed = dt > 0.0f ? jump / dt : 0.0f;
                still_s = (out.glitch || speed >= cfg.settle_speed) ? 0.0f : still_s + dt;
            }
            last_neck = neck;
            have_neck = true;
        }

        // Lean budget: a lean (sideways or down by engage_m) counts fully for hold_s, then the
        // stance follows the neck (hold_tau_s) whatever the head does; standing within release_m
        // of the stance for release_s recharges it. Small offsets (drift) settle after settle_s
        // still. settle_s = 0: never settle.
        // A lean that reverses (the head crosses the recentre spot to the other side: the stance
        // never lies beyond it) is a new dodge with a fresh budget, so swaying left and right
        // keeps counting.
        const Vec3 c0 = Sub(neck, stance);
        const float h0 = std::hypot(c0.x, c0.z);
        const float e = std::max(h0, std::max(-c0.y, 0.0f));
        if (h0 >= cfg.engage_m) {
            const float hx = c0.x / h0;
            const float hz = c0.z / h0;
            if (engaged_s > 0.0f && hx * engage_ux + hz * engage_uz < 0.0f) {
                engaged_s = 0.0f;
            }
            if (engaged_s == 0.0f) {
                engage_ux = hx;
                engage_uz = hz;
            }
        }
        if (e >= cfg.engage_m) {
            engaged_s += dt;
        }
        if (e < cfg.release_m) {
            released_s += dt;
            if (released_s >= cfg.release_s - 1e-4f) {
                engaged_s = 0.0f;
            }
        } else {
            released_s = 0.0f;
        }
        const bool settle_on = cfg.settle_s > 0.0f;
        const bool hold_settle = settle_on && cfg.hold_s > 0.0f && engaged_s > cfg.hold_s;
        const bool drift_settle = settle_on && e < cfg.engage_m && still_s >= cfg.settle_s;
        if ((hold_settle || drift_settle) && dt > 0.0f) {
            const float tau = hold_settle ? cfg.hold_tau_s : cfg.settle_tau_s;
            const float a = 1.0f - std::exp(-dt / std::max(tau, 1e-3f));
            stance.x += (neck.x - stance.x) * a;
            stance.y += (neck.y - stance.y) * a;
            stance.z += (neck.z - stance.z) * a;
        }
        out.held_s = engaged_s;
        out.hold_settling = hold_settle;
        // The stance never lies beyond the neck as seen from the origin: walking back to the
        // centre after a settle counts as zero lean, not as a lean the other way.
        const auto between = [](float s, float n) {
            return std::clamp(s, std::min(0.0f, n), std::max(0.0f, n));
        };
        stance = {between(stance.x, neck.x), between(stance.y, neck.y), between(stance.z, neck.z)};
        const Vec3 counted = Sub(neck, stance);
        out.counted = counted;
        out.settled_m = std::hypot(stance.x, stance.z);

        // Fade weight.
        const bool on = in.active && (in.tracked || lost_s < 1e-6f);
        if (on) {
            weight = cfg.fade_in_s > 0.0f ? std::min(1.0f, weight + dt / cfg.fade_in_s) : 1.0f;
        } else if (!in.tracked && in.active) {
            weight = cfg.lost_fade_s > 0.0f ? std::max(0.0f, weight - dt / cfg.lost_fade_s) : 0.0f;
        } else {
            weight = cfg.fade_out_s > 0.0f ? std::max(0.0f, weight - dt / cfg.fade_out_s) : 0.0f;
        }
        out.weight = weight;
        // The look-only shift (a kept clip): subtracted fully meanwhile; after it, held while the
        // lean fades back in, else eased out with the rest.
        if (in.look_only) {
            look_w = 1.0f;
        } else if (in.active && weight >= 1.0f) {
            look_w = 0.0f;
        } else if (!in.active) {
            look_w = cfg.fade_out_s > 0.0f ? std::max(0.0f, look_w - dt / cfg.fade_out_s) : 0.0f;
        }
        const float cw = std::max(weight, look_w);
        out.comp_weight = cw;

        // Horizontal lean: capped, then limited by the wall (the wall limit is on the gained
        // head offset, so divide it back by the gain).
        float d = std::hypot(counted.x, counted.z);
        const float ux = d > 1e-6f ? counted.x / d : 0.0f;
        const float uz = d > 1e-6f ? counted.z / d : 0.0f;
        d = std::min(d, cfg.max_m);
        out.lean_want_m = d;
        const float gain = std::max(cfg.gain, 0.0f);
        // Wall: drops at once; released at wall_rise_mps while the lean keeps its direction (a
        // lean more than 45 degrees off the measured one starts from its own measurement).
        const bool measured = in.wall_m >= 0.0f;
        float limit = measured ? std::max(in.wall_m - cfg.wall_margin_m, 0.0f) : NoWall;
        const bool same_way = d < 1e-3f || ux * wall_ux + uz * wall_uz > 0.7071f;
        if (wall_limit < NoWall && limit > wall_limit && same_way && cfg.wall_rise_mps > 0.0f) {
            limit = std::min(limit, wall_limit + cfg.wall_rise_mps * dt);
        }
        wall_limit = limit > WallFar ? NoWall : limit;
        if (measured && d >= 1e-3f) {
            wall_ux = ux;
            wall_uz = uz;
        }
        if (wall_limit < NoWall && gain > 0.0f && d * gain > wall_limit) {
            d = wall_limit / gain;
            out.wall_limited = true;
        }
        out.wall_limit_m = wall_limit < NoWall ? wall_limit : -1.0f;
        out.lean_m = d;

        // Duck: only a drop counts.
        const float dy = std::clamp(counted.y, -cfg.duck_m, 0.0f);
        out.duck_m = dy;

        const float a = std::max(cfg.lean_m, 0.0f);
        const float pivot = std::min(d, a);
        const float step = std::max(d - a, 0.0f);
        for (size_t b = 0; b < BodyCount; ++b) {
            if (b == Pelvis01 && !cfg.pelvis) {
                continue;
            }
            const float h = gain * (LeanWeight(b) * pivot + step);
            Vec3 o{ux * h, gain * DuckWeight(b) * dy, uz * h};
            Vec3 comp{};
            if (UnderUpperPos(b)) {
                const float k = std::max(cfg.shift_reach, 0.0f);
                const float kl = std::max(cfg.lean_reach, 0.0f) * LeanReach(b);
                comp = {in.pose_shift.x * k + in.pose_lean.x * kl,
                        FollowedShiftY(in.pose_shift.y) * k + in.pose_lean.y * kl,
                        in.pose_shift.z * k + in.pose_lean.z * kl};
            }
            // An arm on IK sits at your real controller, which already carries your lean once:
            // it only gets the gain's extra, so it stays with the rest of the body instead of
            // trailing it by (gain - 1) of the lean (and keeps the shift: it is drawn there).
            const float ik = IsLeftArm(b)    ? std::clamp(in.ik_left, 0.0f, 1.0f)
                             : IsRightArm(b) ? std::clamp(in.ik_right, 0.0f, 1.0f)
                                             : 0.0f;
            if (ik > 0.0f) {
                const float extra = gain > 1.0f ? (gain - 1.0f) / gain : 0.0f;
                const Vec3 real{ux * h * extra, gain * DuckWeight(b) * dy * extra, uz * h * extra};
                o = {o.x + (real.x - o.x) * ik, o.y + (real.y - o.y) * ik,
                     o.z + (real.z - o.z) * ik};
            }
            const float c = cw * (1.0f - ik);
            out.offset[b] = {o.x * weight - comp.x * c, o.y * weight - comp.y * c,
                             o.z * weight - comp.z * c};
        }
        return out;
    }

    void Reset() {
        *this = Model{};
    }

private:
    static Vec3 Sub(const Vec3& a, const Vec3& b) {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }
    static Vec3 Add(const Vec3& a, const Vec3& b) {
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    }
    static float Len(const Vec3& v) {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }

    void ResetLean() {
        stance = {};
        last_neck = {};
        have_neck = false;
        still_s = 0.0f;
        lost_s = 0.0f;
        engaged_s = 0.0f;
        released_s = 0.0f;
        wall_limit = NoWall;
    }

    static constexpr float NoWall = 1e9f;
    static constexpr float WallFar = 2.0f; // a limit beyond this is no limit

    double last_t{};
    bool have_t{false};
    Vec3 stance{};
    Vec3 last_neck{};
    bool have_neck{false};
    float still_s{};
    float lost_s{};
    float weight{};
    float look_w{}; // the look-only shift's own subtraction weight (comp_weight)
    float engaged_s{};  // the current lean's use of the hold budget
    float released_s{}; // time spent near the stance (recharges the budget)
    float engage_ux{};  // the current lean's horizontal direction when it engaged
    float engage_uz{};
    float wall_limit{NoWall};
    float wall_ux{}; // the lean direction the wall limit was measured along
    float wall_uz{};
};

} // namespace VR::Evade
