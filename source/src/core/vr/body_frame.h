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

#include "core/vr/arm_ik.h"
#include "core/vr/c0000_skeleton.h"
#include "core/vr/space_shift.h"

// VR body: the frames between the headset and the hunter's skeleton. Pure math (no OpenXR or
// emulator dependencies), shared by the hands channel (core/vr/body_channel.h), the pose hook
// (core/vr/bloodborne_body.cpp) and the unit tests (tests/test_body_frame.cpp).
//
// Spaces
//  - Tracking: OpenXR LOCAL, metres, right-handed, -Z forward. Head and grips are located there,
//    both for the same predicted time.
//  - Game world: the camera bridge's mapping (VR::OnCameraTick). A tracking offset d becomes
//        TrackingToWorld(d) = s * Ry(b) * P * d = s * (cb dx - sb dz, dy, -sb dx - cb dz)
//    with P = diag(1, 1, -1), b = the camera's base yaw (tracking -Z is the game heading b) and
//    s = world_scale. Rotations: q_world = q_y(b) * (-qx, -qy, qz, qw) (P conjugation, then the
//    yaw), so a grip's blade (grip -Z) is +Z of that rotation and its up (grip +Y) is +Y.
//  - Model: c0000 model space (metres, +Y up, the hunter faces -Z, his right arm on -X). World =
//    W * model with W = the 3x4 row-major [R | t] at ChrCtrl+0x90.
//
// Anchor: every rendered frame the render-camera hook stores the mono eye E (the final camera
// position minus its stereo eye offset) relative to the character, eye_rel = E - c (c = W's
// translation read in the same tick), with the head position the camera was placed for. The pose
// hook of the next frame (it runs before that frame's camera) places a hand at
//        p_model = R^T * (eye_rel + TrackingToWorld(hand - head))
// The character's own motion between the two cancels where the camera follows it, so the hand
// keeps its place relative to the view; only a turn of W after the pose copy would show (W-drift
// probe). In stereo pairs the second eye's camera stays where the first eye's was (the pair lock)
// while the character moves on, so NextEyeRel picks the eye that frame is really rendered from.
namespace VR::Body {

// ---- Hands (OpenXR frame thread -> game) --------------------------------------------------------

// One controller grip (OpenXR grip pose), located for the head's target time.
struct TrackedHand {
    bool position_valid{false};
    bool position_tracked{false}; // false while the runtime only infers the position
    bool orientation_valid{false};
    bool orientation_tracked{false};
    bool velocity_valid{false}; // linear
    bool angular_valid{false};
    Vec3 p{}; // tracking space, metres
    Quat q{};
    Vec3 v{};            // m/s
    Vec3 w{};            // rad/s
    float grip{0.0f};    // squeeze 0..1 (from the previous XR frame's input sync)
    float trigger{0.0f}; // 0..1
};

// Usable for an arm: the position tracked (not only inferred) and a valid orientation.
inline bool IsTracked(const TrackedHand& h) {
    return h.position_valid && h.position_tracked && h.orientation_valid && IsFinite(h.p) &&
           IsFinite(h.q) && Dot(h.q, h.q) > 0.5f;
}

// Published once per XR frame (seqlock, core/vr/body_channel.h).
struct HandsSample {
    std::uint64_t seq{0};      // publications so far (1 = the first)
    std::int64_t xr_time{0};   // XrTime the head and both grips were located for
    std::int64_t steady_ns{0}; // steady clock at publication
    bool head_valid{false};
    bool simulated{false}; // --vr-sim scripted hands (VR_BODY_SIM_HANDS)
    Vec3 head_p{};
    Quat head_q{};
    std::array<TrackedHand, 2> hand{}; // C0000::Left = 0, C0000::Right = 1 (OpenXR hand order)
    // The tracking space head and hands are in (VR::ShiftTrackingSpace; changes on a recenter).
    std::uint32_t space_serial{0};
    // The two-handed grip's weight (the presenter's ApplyTwoHand): the left hand on the right
    // weapon's handle (0: on its controller).
    float two_hand{0.0f};
    // ... and the weapon's offset in the hunter's right fist (the right hand slid along it while
    // two-handed, core/vr/two_hand_grip.h SupportGrip::WeaponShift): in the right weapon bone's
    // own frame, game units; zero: the game's own grip point.
    Vec3 two_hand_weapon_shift{};
    std::int8_t bow_hand{-1}; // native Bowblade, -1 when gameplay does not own it
    bool bow_arrow{false};
    bool bow_nocked{false};
    // Music-box knob contact, separate from raw tracked hands (tracking-loss filters
    // must never mistake the visual snap for a controller jump).
    float music_box_weight{0.0f};
    Vec3 music_box_contact{};
    Quat music_box_frame{};
};

// ---- Anchor (render-camera hook -> next frame's pose hook) --------------------------------------

struct Anchor {
    bool valid{false};         // a driven first-person follow-camera frame
    std::int64_t steady_ns{0}; // steady clock of the render tick
    std::uint64_t record{0};   // its camera record
    std::int32_t eye{-1};      // stereo eye of that frame (-1 = mono)
    bool aer{false};           // alternate-eye stereo (stereo_mode 1)
    Vec3 eye_rel{};            // mono eye - character translation (world axes, game units)
    Vec3 eye_world{};          // mono eye (game units; logs)
    Vec3 chr_t{};              // W's translation in the same tick (logs)
    Vec3 head_k{};             // tracking-space head position the camera was placed for
    float base_yaw{0.0f};      // radians: game heading of tracking -Z
    float view_yaw{0.0f};      // radians (logs)
    float view_pitch{0.0f};    // radians, + = down (the render camera's; body_back_down_m)
    float world_scale{1.0f};   // game units per metre
    // The first-person camera's base height (before your real head's offset and the stereo eye)
    // minus where fp_eye_height_m puts it over W (game units): the comfort camera's vertical
    // smoothing on stairs and slopes, and terrain where its physics position and the hunter's
    // model disagree. With body_height_from_head the body's height goal leaves it out.
    float camera_dip{0.0f};
    // The entire hunter, not an upper-body lean: world-space translation matching
    // the visceral camera. While held, its presented root stays fixed as W moves.
    Vec3 rig_offset{};
    bool rig_held{false};
    Vec3 unheld_base{}; // previous rendered comfort base, before throw/head/stereo
    Vec3 head_offset{}; // tracked mono offset + first-person lead, for the next pose ray
    // Stereo pairs (VR::CameraOverride::pair_first): this tick started a pair (every tick in mono
    // and DIBR). `paired`: stereo_mode 1 (AER) or 0, where the tick after a pair_first one is that
    // pair's second eye. `lock_ns`: that second eye keeps this tick's camera position in the
    // world (the pair lock) when its pose copy comes within this time of this tick (0 = no lock);
    // the pose hook decides it there and the comfort camera follows (PairLock).
    bool pair_first{true};
    bool paired{false};
    std::int64_t lock_ns{0};
};

// Arm pair sync: the frame after the first eye of a stereo pair (AER or stereo_mode 0) is that
// pair's second eye, rendered with the same head sample. Its arms take the same hand sample, so
// both eyes show the hand at one instant (a hand at 2 m/s would otherwise sit 3.3 cm apart between
// the eyes: a depth error). Each eye still updates at 30 Hz either way.
inline bool ReuseHands(const Anchor& last) {
    return last.paired && last.pair_first;
}

// ---- Character frame W -------------------------------------------------------------------------

struct ChrFrame {
    bool valid{false};
    std::array<float, 9> R{}; // row-major rotation
    Vec3 t{};
    Quat q{}; // the same rotation
};

// W as stored at ChrCtrl+0x90: three float4 rows {R row i, t_i}. Rejects non-finite values, rows
// that are not unit length (1 +- 0.01) or orthogonal (|dot| <= 0.01), and reflections.
inline bool ChrFrameFromRows(const std::array<float, 12>& rows, ChrFrame& out) {
    out = {};
    for (const float value : rows) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    const Vec3 r0{rows[0], rows[1], rows[2]};
    const Vec3 r1{rows[4], rows[5], rows[6]};
    const Vec3 r2{rows[8], rows[9], rows[10]};
    for (const Vec3& r : {r0, r1, r2}) {
        if (std::abs(Length(r) - 1.0f) > 0.01f) {
            return false;
        }
    }
    if (std::abs(Dot(r0, r1)) > 0.01f || std::abs(Dot(r0, r2)) > 0.01f ||
        std::abs(Dot(r1, r2)) > 0.01f || Dot(Cross(r0, r1), r2) <= 0.0f) {
        return false;
    }
    out.R = {r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z};
    out.t = {rows[3], rows[7], rows[11]};
    out.q = FromBasis({r0.x, r1.x, r2.x}, {r0.y, r1.y, r2.y}, {r0.z, r1.z, r2.z});
    out.valid = true;
    return true;
}

inline Vec3 RotateToWorld(const ChrFrame& f, Vec3 v) {
    return {f.R[0] * v.x + f.R[1] * v.y + f.R[2] * v.z, f.R[3] * v.x + f.R[4] * v.y + f.R[5] * v.z,
            f.R[6] * v.x + f.R[7] * v.y + f.R[8] * v.z};
}
inline Vec3 RotateToModel(const ChrFrame& f, Vec3 v) {
    return {f.R[0] * v.x + f.R[3] * v.y + f.R[6] * v.z, f.R[1] * v.x + f.R[4] * v.y + f.R[7] * v.z,
            f.R[2] * v.x + f.R[5] * v.y + f.R[8] * v.z};
}
inline Vec3 ToWorld(const ChrFrame& f, Vec3 model) {
    return RotateToWorld(f, model) + f.t;
}
inline Vec3 ToModel(const ChrFrame& f, Vec3 world) {
    return RotateToModel(f, world - f.t);
}

// Where the next frame's camera is. The anchor's eye_rel holds while the camera follows the
// character; a pair-locked second eye does not (it stays at the first eye's world position while
// the character moves on: v * dt off, in opposite directions in the two eyes).
enum class EyeSource : unsigned char {
    Anchor,    // the newest anchor's eye_rel (mono, DIBR, an unlocked second eye)
    Locked,    // a pair-locked second eye: the first eye's world position, relative to W now
    PairFirst, // the frame after a second eye starts a pair: the last pair's first eye
};

constexpr std::int64_t PairFirstMaxNs = 100'000'000; // an older pair-first anchor is not used

// The eye (relative to the character, world axes) for the frame after the newest anchor `last`,
// whose pose copy reads W = `now` at `now_ns`. `first`: the last pair-first anchor (nullptr =
// none). `drift`: W's predicted move between this pose copy and the frame's camera (zero when W
// is final at the pose copy).
inline Vec3 NextEyeRel(const Anchor& last, const Anchor* first, const ChrFrame& now,
                       std::int64_t now_ns, Vec3 drift, EyeSource& source) {
    source = EyeSource::Anchor;
    if (!last.paired) {
        return last.eye_rel;
    }
    if (last.pair_first) {
        if (last.lock_ns > 0 && now.valid && now_ns - last.steady_ns <= last.lock_ns) {
            source = EyeSource::Locked;
            return last.eye_world - (now.t + drift);
        }
        return last.eye_rel; // too late for the lock: that second eye moves with the body
    }
    if (first != nullptr && first->paired && first->pair_first &&
        last.steady_ns >= first->steady_ns && last.steady_ns - first->steady_ns <= PairFirstMaxNs) {
        source = EyeSource::PairFirst;
        return first->eye_rel;
    }
    return last.eye_rel;
}

inline const char* EyeSourceName(EyeSource source) {
    switch (source) {
    case EyeSource::Anchor:
        return "anchor";
    case EyeSource::Locked:
        return "locked";
    case EyeSource::PairFirst:
        return "pair_first";
    }
    return "?";
}

// The pair lock of a stereo pair's second eye, decided once: at that frame's pose copy, where the
// body places the hands for it (before the camera runs). The comfort camera's second-eye tick
// follows this decision (core/vr/motion_camera.h LockDecision; published through
// core/vr/body_channel.h), so the eye the hands were placed for is the eye the frame is rendered
// from.
struct PairLock {
    std::int64_t first_ns{0};   // the pair's first-eye anchor (its steady_ns)
    std::int64_t decided_ns{0}; // the pose copy that decided it (steady clock)
    bool locked{false};
};

// W's move between a pose copy and the camera after it is measured from a camera this soon after
// the copy, and only this far (metres, times world_scale).
constexpr std::int64_t DriftMaxNs = 50'000'000;
constexpr float DriftMaxM = 1.0f;
constexpr float DriftMaxRatio = 4.0f; // the copy interval over the last one, at most

struct EyePlacement {
    Vec3 eye_rel{}; // the eye this frame is rendered from, relative to W now (valid with an anchor)
    EyeSource source{EyeSource::Anchor};
    Vec3 drift{};        // W's predicted move between this copy and the frame's camera
    bool decided{false}; // this copy decided a second eye's pair lock (`lock`)
    PairLock lock{};
    Vec3 rig_offset{}; // same anchor/pair choice as eye_rel
};

// The pose hook's view of the stereo pairs (core/vr/bloodborne_body.cpp Gather), fed every pose
// copy. Place() gives a fresh copy's eye (NextEyeRel; after a second eye it locked, the pair's
// first eye however long the gap between the two was), the predicted W drift and, for a pair's
// second eye, the pair lock decision. The drift is W's move between the last pose copy and the
// camera after it, scaled by this copy interval over that copy's (clamped 0..4, so a copy after
// a stall of up to two missed vblanks between a copy and its camera is still covered): W moves
// with the game's frame, so a hitch measured last frame does not move this frame's eye (zero when
// W is already final at the copy).
class EyeTracker {
public:
    // Every pose copy, fresh (through Place) or stale: W then (`w.valid` false: none) and when.
    void NoteCopy(const ChrFrame& w, std::int64_t now_ns) {
        interval_ns = copy_ns != 0 && now_ns > copy_ns ? now_ns - copy_ns : 0;
        copy_valid = w.valid;
        copy_t = w.t;
        copy_ns = now_ns;
    }

    // A fresh pose copy at `now_ns` reading W = `w`, with the newest valid anchor (nullptr: none).
    EyePlacement Place(const Anchor* anchor, const ChrFrame& w, std::int64_t now_ns) {
        EyePlacement out{};
        if (anchor == nullptr) {
            have_first = have_lock = false;
            NoteCopy(w, now_ns);
            return out;
        }
        out.drift = PredictDrift(*anchor, now_ns);
        NoteCopy(w, now_ns);
        if (anchor->pair_first) {
            first = *anchor;
            have_first = true;
        }
        out.eye_rel =
            NextEyeRel(*anchor, have_first ? &first : nullptr, w, now_ns, out.drift, out.source);
        if (out.source == EyeSource::Anchor && anchor->paired && !anchor->pair_first &&
            have_first && have_lock && lock.locked && lock.first_ns == first.steady_ns &&
            anchor->steady_ns >= first.steady_ns) {
            // That second eye was locked (decided here): its eye_rel is the first eye's position
            // against W then, however long the gap between the eyes was; the new pair's camera
            // follows the body like the pair's first eye did.
            out.eye_rel = first.eye_rel;
            out.source = EyeSource::PairFirst;
        }
        // A held rig uses the latest absolute placement, including entry or a new
        // clearance correction. Reusing an earlier pair-first anchor loses it.
        const Anchor& selected = !anchor->rig_held && out.source == EyeSource::PairFirst && have_first
                                     ? first : *anchor;
        const bool rig_fresh = now_ns >= selected.steady_ns &&
                               now_ns - selected.steady_ns <= 250'000'000;
        out.rig_offset = rig_fresh && IsFinite(selected.rig_offset) ? selected.rig_offset : Vec3{};
        if (selected.rig_held && w.valid && rig_fresh) {
            const Vec3 motion = w.t + out.drift - selected.chr_t;
            if (IsFinite(motion) && LengthSq(motion) <= 9.0f * selected.world_scale * selected.world_scale) {
                // The old predictor followed W on each new first eye: that moved
                // the hands/shoulders into the enemy even while the camera held.
                out.eye_rel = selected.eye_world - (w.t + out.drift);
                out.rig_offset = selected.rig_offset - motion;
            } else {
                out.rig_offset = {}; // a real warp does not preserve a stale held body
            }
        }
        if (anchor->paired && anchor->pair_first) {
            out.decided = true;
            out.lock = {anchor->steady_ns, now_ns, out.source == EyeSource::Locked};
            lock = out.lock;
            have_lock = true;
        }
        return out;
    }

    void Reset() {
        *this = {};
    }

private:
    Vec3 PredictDrift(const Anchor& a, std::int64_t now_ns) const {
        if (!copy_valid || a.steady_ns < copy_ns || a.steady_ns - copy_ns > DriftMaxNs) {
            return {};
        }
        const Vec3 d = a.chr_t - copy_t;
        if (!IsFinite(d) || !(Length(d) <= DriftMaxM * a.world_scale)) {
            return {};
        }
        if (interval_ns <= 0) {
            return d;
        }
        const float ratio = static_cast<float>(now_ns - copy_ns) / static_cast<float>(interval_ns);
        return d * std::clamp(ratio, 0.0f, DriftMaxRatio);
    }

    // W at the last pose copy (fresh or stale) and the copy interval that ended there.
    bool copy_valid{false};
    Vec3 copy_t{};
    std::int64_t copy_ns{0};
    std::int64_t interval_ns{0};
    // The last pair-first anchor and the last second-eye decision.
    bool have_first{false};
    Anchor first{};
    bool have_lock{false};
    PairLock lock{};
};

// Heading of the character's forward (-column 2 of R; the model faces -Z), radians, the game's
// yaw convention: atan2(x, z).
inline float ChrYaw(const ChrFrame& f) {
    return std::atan2(-f.R[2], -f.R[8]);
}

// How W moved between two reads (the pose copy and the render-camera hook).
struct FrameDrift {
    float yaw{};      // radians, signed
    float angle{};    // radians, the whole rotation
    float distance{}; // game units
};
inline FrameDrift Drift(const ChrFrame& from, const ChrFrame& to) {
    return {WrapPi(ChrYaw(to) - ChrYaw(from)), AngleBetween(from.q, to.q), Length(to.t - from.t)};
}

// ---- Tracking -> world -> model -----------------------------------------------------------------

// A tracking-space offset in the game world (the camera bridge's 6DoF mapping).
inline Vec3 TrackingToWorld(Vec3 d, float base_yaw, float scale) {
    const float cb = std::cos(base_yaw), sb = std::sin(base_yaw);
    return Vec3{cb * d.x - sb * d.z, d.y, -sb * d.x - cb * d.z} * scale;
}
// Inverse of TrackingToWorld.
inline Vec3 WorldToTracking(Vec3 w, float base_yaw, float scale) {
    const float cb = std::cos(base_yaw), sb = std::sin(base_yaw);
    const float inv = std::abs(scale) > 1e-6f ? 1.0f / scale : 1.0f;
    return Vec3{cb * w.x - sb * w.z, w.y, -(sb * w.x + cb * w.z)} * inv;
}
// A tracking-space orientation in the game world.
inline Quat TrackingToWorldRotation(const Quat& q, float base_yaw) {
    return Normalize(AxisAngle({0.0f, 1.0f, 0.0f}, base_yaw) * Quat{-q.x, -q.y, q.z, q.w});
}

struct ModelTarget {
    Vec3 p{}; // effector position (the weapon bone on the grip), model space
    Quat q{}; // effector rotation, model space
};

// `grip`: DefaultGrip (weapon bone -> controller frame).
inline ModelTarget HandToModel(const Anchor& a, const ChrFrame& chr, const TrackedHand& h,
                               const Quat& grip) {
    const Vec3 rel = a.eye_rel + TrackingToWorld(h.p - a.head_k, a.base_yaw, a.world_scale);
    return {RotateToModel(chr, rel),
            Normalize(Conjugate(chr.q) * TrackingToWorldRotation(h.q, a.base_yaw) * grip)};
}
// The tracking-space position a model target stands for (logs; inverse of HandToModel).
inline Vec3 ModelToTracking(const Anchor& a, const ChrFrame& chr, Vec3 model) {
    return a.head_k +
           WorldToTracking(RotateToWorld(chr, model) - a.eye_rel, a.base_yaw, a.world_scale);
}

// ---- Weapon grip -------------------------------------------------------------------------------

// Grip calibration: the rotation from the weapon bone's local frame (R_Weapon 98 / L_Weapon 69) to
// the controller frame (the P-conjugated grip: +Z = grip -Z, +Y = grip +Y, +X = grip +X), so that
// weapon bone (model) = controller (model) * DefaultGrip(...).
//  - The blade runs along the weapon bone's -Y (in the bind pose (-0.16, -0.30, -0.94) in model
//    space: forward) and lands on the controller's blade axis, grip -Z pitched `blade_pitch_deg`
//    toward grip +Y (the swing detector's convention).
//  - The weapon bone's +X lands on the controller's up (grip +Y made perpendicular to the blade).
//  - The left weapon bone is the mirror image of the right one with its Z axis flipped, so the same
//    axes hold for it; the yaw and roll trims are mirrored.
// Trims (vr.ini weapon_grip_*), in the controller frame: pitch + = the blade up, yaw + = the blade
// outward (away from the body's midline), roll + = the grip's top rolled inward.
// [U]: the axes are from the skeleton's bind pose; checked in the headset against GetDmyPolyMtx.
//
// The trim rotation alone: yaw about the controller's up (+Y), then pitch about its +X, then roll
// about its blade (+Z), intrinsic; the left hand's yaw and roll mirrored.
inline Quat GripTrimRotation(int side, float yaw_deg, float pitch_deg, float roll_deg) {
    const float mirror = side == C0000::Left ? -1.0f : 1.0f;
    return Normalize(AxisAngle({0.0f, 1.0f, 0.0f}, mirror * yaw_deg * Deg) *
                     AxisAngle({1.0f, 0.0f, 0.0f}, -pitch_deg * Deg) *
                     AxisAngle({0.0f, 0.0f, 1.0f}, mirror * roll_deg * Deg));
}

inline Quat DefaultGrip(int side, float blade_pitch_deg, float yaw_deg = 0.0f,
                        float pitch_deg = 0.0f, float roll_deg = 0.0f) {
    const float a = blade_pitch_deg * Deg;
    const Vec3 blade{0.0f, std::sin(a), std::cos(a)};
    const Vec3 up{0.0f, std::cos(a), -std::sin(a)};
    const Quat controller = FromBasis(blade, up, Cross(blade, up));
    const Vec3 weapon_blade{0.0f, -1.0f, 0.0f};
    const Vec3 weapon_up{1.0f, 0.0f, 0.0f};
    const Quat weapon = FromBasis(weapon_blade, weapon_up, Cross(weapon_blade, weapon_up));
    const Quat trim = GripTrimRotation(side, yaw_deg, pitch_deg, roll_deg);
    return Normalize(trim * controller * Conjugate(weapon));
}

// The grip moved by a weapon offset (metres, in the OpenXR grip frame of `h`): where the weapon
// bone goes for that hand. Velocities are unchanged (the offset is rigid).
inline TrackedHand OffsetGrip(const TrackedHand& h, Vec3 offset) {
    TrackedHand out = h;
    out.p = h.p + Rotate(Normalize(h.q), offset);
    return out;
}

// ---- Tracking loss -----------------------------------------------------------------------------

enum class HandUse : unsigned char {
    Tracked, // this sample
    Held,    // tracking lost: the last tracked position (orientation from the IMU while valid)
    Lost,    // lost for longer than the hold: the arm goes back to the animation
};

inline const char* HandUseName(HandUse use) {
    switch (use) {
    case HandUse::Tracked:
        return "tracked";
    case HandUse::Held:
        return "held";
    case HandUse::Lost:
        return "lost";
    }
    return "?";
}

// The head of a hands sample: the glitch check's reference (a runtime recentre moves the head and
// the hands together).
struct HeadPose {
    bool valid{false};
    Vec3 p{};
    Quat q{};
};

// The last tracked pose is held while tracking is lost (for `hold_ns`), with the IMU orientation
// while the runtime still reports one. A tracked sample whose position jump cannot be real motion
// is a tracking glitch: it is held over like a lost one (the whole sample, its orientation too).
//
// Real motion must never trip the check (Glitch): a hand held during a swing is far worse than a
// spike shown for one frame. The grips are located for the display time + pose_prediction_ms, so
// the runtime extrapolates them up to MaxHorizon ahead (not published; assumed), and at the end of
// a fast swing that prediction overshoots and pulls back: a jump the reported velocities alone do
// not explain. A jump is a glitch only when it is over SlackM and strays from the trapezoid of the
// two samples' reported velocities v0, v1 over their interval dt by more than Tolerance:
//  - SlackM: tracking noise;
//  - MaxAccel * dt^2 / 4: the trapezoid's own error for a hand accelerating at up to MaxAccel (a
//    0.9 m swing peaking at 20 m/s: about 730 m/s^2);
//  - |v1 - v0| * (MaxHorizon + dt / 2): a constant-velocity extrapolation adds (v1 - v0) times its
//    horizon to the jump; dt / 2 bounds the trapezoid's error for an abrupt stop (an impact);
//  - (mean |v| + HiddenSpeed) * dt: a constant-acceleration extrapolation reports v + a * horizon,
//    not the hand's own velocity, so the trapezoid misses the jump by up to the reported plus the
//    hand's own speed times dt (HiddenSpeed: above any hand).
// Without the runtime's velocities nothing tells an extrapolated swing from a glitch: the jump is
// taken. In practice that holds a spike of 1 m while the reported velocity says the hand is still
// when it comes within 21 ms of the last sample (at 60 fps mono: every sample at 120 Hz, 4 in 5 at
// 72 Hz, every other one at 90 Hz), 1.6 m within 33 ms (stereo pairs, 30 fps); spikes while the
// hand moves fast are mostly shown. A shorter jump cannot be told from a fast swing under a
// constant-acceleration runtime (a 0.5 m flick at 25 m/s predicted 50 ms ahead jumps about 0.9 m
// in 22 ms with both reported velocities near 0).
// A jump that keeps the hand where it was relative to the head (the head jumped with it: a runtime
// recentre, a new tracking space) is never a glitch. A hand that comes back after a hold or a loss
// eases from the pose shown last onto the controller over RegainSteps new samples instead of
// jumping there in one frame; the same sample again (a stereo pair's second eye reusing its first
// eye's sample) shows the same pose.
struct HandHold {
    static constexpr float SlackM = 0.25f;      // m
    static constexpr float MaxAccel = 1000.0f;  // m/s^2
    static constexpr float MaxHorizon = 0.060f; // s: the runtime's pose extrapolation, at most
    static constexpr float HiddenSpeed = 30.0f; // m/s
    static constexpr int RegainSteps = 3;

    bool have{false};
    TrackedHand last{}; // the last tracked sample accepted
    HeadPose last_head{};
    std::int64_t last_ns{0};
    std::int64_t last_sample_ns{0};
    TrackedHand shown{}; // the last pose handed out
    HandUse last_use{HandUse::Lost};
    bool rejected{false}; // the last update dropped a glitch
    int ease_left{0};     // regain: new samples still easing
    Vec3 ease_p{};        // regain: shown - controller at the regain
    Quat ease_q{};

    // How far (m) a move over `dt` seconds may stray from the trapezoid of the reported velocities
    // `v0` and `v1` at its two ends.
    static float Tolerance(float dt, Vec3 v0, Vec3 v1) {
        const float mean_speed = 0.5f * (Length(v0) + Length(v1));
        return SlackM + 0.25f * MaxAccel * dt * dt + Length(v1 - v0) * (MaxHorizon + 0.5f * dt) +
               (mean_speed + HiddenSpeed) * dt;
    }

    // Does `moved` over `dt` seconds agree with the reported velocities `v0`, `v1`?
    static bool Explained(Vec3 moved, Vec3 v0, Vec3 v1, float dt) {
        return Length(moved - (v0 + v1) * (0.5f * dt)) <= Tolerance(dt, v0, v1);
    }

    // Is `to`, taken `dt` seconds after the accepted sample `from`, a tracking glitch? `from_head`
    // and `to_head`: the heads of the two samples (the recentre check; invalid = no check).
    static bool Glitch(const TrackedHand& from, const HeadPose& from_head, const TrackedHand& to,
                       const HeadPose& to_head, float dt) {
        dt = std::isfinite(dt) ? std::max(dt, 0.0f) : 0.0f;
        const Vec3 jump = to.p - from.p;
        if (!(Length(jump) > SlackM) || !from.velocity_valid || !to.velocity_valid ||
            !IsFinite(from.v) || !IsFinite(to.v) || Explained(jump, from.v, to.v, dt)) {
            return false;
        }
        if (from_head.valid && to_head.valid && IsFinite(from_head.p) && IsFinite(to_head.p) &&
            IsFinite(from_head.q) && IsFinite(to_head.q) && Dot(from_head.q, from_head.q) > 0.5f &&
            Dot(to_head.q, to_head.q) > 0.5f) {
            // Relative to the head (invariant under a change of the tracking space): the hand's
            // move there against its velocities there.
            const Quat q0 = Conjugate(Normalize(from_head.q));
            const Quat q1 = Conjugate(Normalize(to_head.q));
            const Vec3 moved = Rotate(q1, to.p - to_head.p) - Rotate(q0, from.p - from_head.p);
            if (Explained(moved, Rotate(q0, from.v), Rotate(q1, to.v), dt)) {
                return false;
            }
        }
        return true;
    }

    // `sample_ns`: when the sample was taken (any clock; the glitch check's time base), -1 =
    // now_ns. `head`: the sample's head (HeadPose{} = none: no recentre check).
    HandUse Update(const TrackedHand& in, std::int64_t now_ns, std::int64_t hold_ns,
                   TrackedHand& out, std::int64_t sample_ns = -1, const HeadPose& head = {}) {
        const std::int64_t t = sample_ns >= 0 ? sample_ns : now_ns;
        const bool holding = have && now_ns - last_ns <= hold_ns;
        rejected = false;
        if (IsTracked(in)) {
            if (holding) {
                const float dt =
                    static_cast<float>(std::max<std::int64_t>(t - last_sample_ns, 0)) * 1e-9f;
                rejected = Glitch(last, last_head, in, head, dt);
            }
            if (!rejected) {
                const bool fresh = !have || t != last_sample_ns;
                if (have && last_use != HandUse::Tracked) {
                    ease_left = RegainSteps;
                    ease_p = shown.p - in.p;
                    ease_q = Normalize(shown.q * Conjugate(Normalize(in.q)));
                }
                if (fresh && ease_left > 0) {
                    --ease_left;
                }
                have = true;
                last = in;
                last_head = head;
                last_ns = now_ns;
                last_sample_ns = t;
                out = in;
                if (ease_left > 0) {
                    const float k = static_cast<float>(ease_left) / RegainSteps;
                    out.p = in.p + ease_p * k;
                    out.q = Normalize(ScaleRotation(ease_q, k) * Normalize(in.q));
                }
                shown = out;
                last_use = HandUse::Tracked;
                return HandUse::Tracked;
            }
        }
        if (!holding) {
            ease_left = 0;
            last_use = HandUse::Lost;
            return HandUse::Lost;
        }
        out = shown;
        if (!rejected && in.orientation_valid && IsFinite(in.q) && Dot(in.q, in.q) > 0.5f) {
            out.q = in.q;
        }
        out.v = {};
        out.w = {};
        shown = out;
        ease_left = 0;
        last_use = HandUse::Held;
        return HandUse::Held;
    }
    void Reset() {
        *this = {};
    }
    // A recenter (core/vr/space_shift.h: `s` is the new tracking space in the old one): what is
    // held moves into the new space with the samples, so a held hand stays where it was in the
    // room and a regain eases from where it was shown. An unknown shift drops the hold.
    void MoveSpace(const Space::Shift& s) {
        if (!s.known) {
            Reset();
            return;
        }
        const auto hand = [&s](TrackedHand& h) {
            h.p = Space::PointToNew(s, h.p);
            h.q = Space::RotationToNew(s, h.q);
            h.v = Space::VectorToNew(s, h.v);
            h.w = Space::VectorToNew(s, h.w);
        };
        hand(last);
        hand(shown);
        last_head.p = Space::PointToNew(s, last_head.p);
        last_head.q = Space::RotationToNew(s, last_head.q);
        ease_p = Space::VectorToNew(s, ease_p);
        // ease_q = shown.q * conj(controller q): both turn by r = Ry(-yaw), so r ease_q r^-1.
        const Quat r = Space::RotationToNew(s, Quat{});
        ease_q = r * ease_q * Conjugate(r);
    }
};

inline const char* ArmStatusName(ArmStatus status) {
    switch (status) {
    case ArmStatus::Solved:
        return "Solved";
    case ArmStatus::Skipped:
        return "Skipped";
    case ArmStatus::BadRig:
        return "BadRig";
    case ArmStatus::BadPose:
        return "BadPose";
    case ArmStatus::BadTarget:
        return "BadTarget";
    case ArmStatus::TooFar:
        return "TooFar";
    case ArmStatus::NumericFailure:
        return "NumericFailure";
    }
    return "?";
}

// The hand is visibly off the controller: not reached and more than 3 cm away (ghost hand, tick).
inline bool Detached(const ArmResult& r) {
    return r.status == ArmStatus::Solved && !r.reached && r.error > 0.03f;
}

// ---- VR_BODY_RECORD -----------------------------------------------------------------------------

// One frame per fresh pose copy, after a RecordHeader. Raw structs of this header (the same build
// reads them back: tests/test_body_frame.cpp with VR_BODY_REPLAY=<file>).
enum RecordFlags : std::uint32_t {
    RecordAnchorActive = 1u << 0, // the arms would be driven (anchor, hands, W all usable)
    RecordHandsReused = 1u << 1,  // arm pair sync reused the previous hand sample
    RecordDryRun = 1u << 2,       // VR_BODY_DRYRUN: nothing was written to the game
};

struct RecordFrame {
    std::int64_t steady_ns{0};
    std::array<float, 12> w_rows{}; // W at the pose copy
    Anchor anchor{};
    HandsSample hands{};                                  // the sample the arms used
    std::array<HkQsTransform, C0000::BoneCount> locals{}; // the game's animated locals (input)
    std::array<float, 2> arm_weight{};                    // C0000::Left, C0000::Right
    float body_weight{0.0f};
    std::uint32_t flags{0};
};

inline constexpr std::array<char, 8> RecordMagic{'B', 'B', 'V', 'R', 'B', 'O', 'D', 'Y'};
constexpr std::uint32_t RecordVersion = 5; // 4: whole-body rig; 5: shared throw placement inputs

struct RecordHeader {
    std::array<char, 8> magic{RecordMagic};
    std::uint32_t version{RecordVersion};
    std::uint32_t bones{C0000::BoneCount};
    std::uint32_t frame_size{static_cast<std::uint32_t>(sizeof(RecordFrame))};
    std::uint32_t reserved{0};
};

} // namespace VR::Body
