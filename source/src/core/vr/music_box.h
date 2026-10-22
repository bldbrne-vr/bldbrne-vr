// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "core/vr/quick_items.h"
#include "core/vr/seqlock.h"

// The Tiny Music Box played by hand (vr.ini quick_music_box_crank=1; the tester's 2026-10-06
// request: "when grabbed - the right hand should become free, and the player should be able to
// grab its little lever and spin it (for the music to be heard). Dropping it/cancelling it,
// should immediately result in the right hand weapon being equipped. (no custom animation btw,
// fluid VR movement only)").
//
// Taken from the arc (core/vr/quick_items.h: the trigger, left_scheme=1 the grip), the box stays
// in the hunter's left hand while that button is held: a small wooden box with a brass crank on
// its side toward the right hand (drawn in-frame, core/vr/bloodborne_quickitems.cpp). Meanwhile
// the RIGHT HAND IS FREE: his right weapon is put away (the game's own weapon-part hide, the
// bytes TAE event 322 writes, rewritten every update: core/vr/bloodborne_held.cpp), swings and the
// right grip's transform are off. The right grip squeezed with the right hand within grab_m of the
// crank's knob takes the knob (a click; the hunter's right hand snaps onto it, eased over
// snap_s); turning the hand round the crank's axis turns the crank with it (a ratchet tick every
// quarter turn); after play_turns of turning the box plays: the game's own use of the item
// (UseGoods: its bullet, what makes Father Gascoigne stagger) and its tune, with no animation.
// Turning on keeps it playing: the next play once the tune is over (replay_s, the game's own use
// takes as long) and another turn has gone round. Letting go of the right grip lets go of the
// knob; letting go of the button that holds the box puts it back and the right weapon is in his
// hand again on the very next frame, swings and the transform with it. A hit that takes the right
// arm knocks the hand off the knob (what was turned is kept).
//
// Pure (no emulator, OpenXR or Vulkan): the geometry, the crank, the restore rule, and the small
// channels between the presenter (input thread), the render-camera hook and the pose hook.
// tests/test_music_box.cpp drives it.
namespace VR::MusicBox {

using Quick::Cross;
using Quick::Dot;
using Quick::Length;
using Quick::Normalize;
using Quick::Qt;
using Quick::V3;

inline constexpr std::int32_t Goods = 700; // Tiny Music Box (EquipParamGoods)
inline constexpr float Pi = 3.14159265f;
inline constexpr float TwoPi = 2.0f * Pi;

// ---- The box in the left hand (LOCAL tracking space, metres) -----------------------------------

// Native model dimensions; gameplay uses FrameForModel and the reviewed side grip.
// FrameOf remains the controller-space fallback used by the pure interaction tests.
struct Geometry {
    float width_m{0.078f};  // along X
    float height_m{0.050f}; // along Y (our own box, while the model loads)
    float depth_m{0.080f};  // along Z
    V3 centre{0.062f, 0.006f, -0.010f};
    float hub_out_m{0.006f};  // the crank's hub this far out of the box's +X face
    float axle_in_m{0.039f}; // hub inward to the native case bearing
    float arm_m{0.032f};      // hub -> knob
    float knob_out_m{0.036f}; // the knob's handle sticks out this far along the axis
};

// The crank's frame: hub, axis and the arm's angle 0 (u) and 90 degrees (v = axis x u).
struct Frame {
    bool valid{false};
    V3 box{};      // the box's centre
    Qt q{};        // the box's orientation (grip followed by a -90 degree Z turn)
    V3 hub{};
    V3 axis{1.0f, 0.0f, 0.0f};
    V3 u{0.0f, 1.0f, 0.0f};
    V3 v{0.0f, 0.0f, 1.0f};
};

inline Frame FrameOf(const Geometry& g, V3 grip_p, Qt grip_q) {
    Frame f{};
    f.valid = true;
    constexpr float h = .7071067812f;
    f.q = {h * (grip_q.x - grip_q.y), h * (grip_q.x + grip_q.y),
           h * (grip_q.z - grip_q.w), h * (grip_q.w + grip_q.z)};
    f.box = grip_p + Quick::Rotate(grip_q, g.centre);
    f.axis = Normalize(Quick::Rotate(grip_q, {0.0f, -1.0f, 0.0f}), {0.0f, -1.0f, 0.0f});
    f.u = Normalize(Quick::Rotate(grip_q, {1.0f, 0.0f, 0.0f}), {1.0f, 0.0f, 0.0f});
    f.v = Cross(f.axis, f.u);
    f.hub = f.box + f.axis * (0.5f * g.width_m + g.hub_out_m);
    return f;
}

// Render hook -> presenter: contact uses the actual hunter palm, including body IK.
struct Attachment {
    std::int64_t steady_ns{};
    Frame frame{};
    Geometry geometry{};
};
inline SeqLock<Attachment>& AttachmentChannel() {
    static SeqLock<Attachment> channel;
    return channel;
}
inline bool LatestAttachment(Attachment& out, std::int64_t now) {
    return AttachmentChannel().Load(out) && out.frame.valid &&
           now - out.steady_ns >= -250'000'000 && now - out.steady_ns < 250'000'000;
}
inline Frame FrameFromAxes(const Geometry& g, V3 centre, V3 x, V3 y) {
    Frame f{};
    f.valid = true;
    f.box = centre;
    f.axis = Normalize(x);
    f.u = Normalize(y);
    f.v = Cross(f.axis, f.u);
    f.hub = f.box + f.axis * (0.5f * g.width_m + g.hub_out_m);
    return f;
}

// The working crank sits on native +Y: the wearer's right with the left palm
// facing up and fingers forward. Game-to-tracking reflection matters here.
// The original fixed front crank is removed by music_box_mesh.h. These coordinates
// are shared by the renderer, contact channel and offline inspection exporter.
inline constexpr V3 NativeHub{.02645f, .097f, .042f};
inline constexpr V3 NativeAxis{0, 1, 0};
inline constexpr V3 NativeUp{0, 0, -1};
inline Frame FrameForModel(V3 origin, V3 x, V3 y, V3 z, float metres, V3 midpoint) {
    Frame f{};
    f.valid = true;
    x = Normalize(x); y = Normalize(y); z = Normalize(z);
    f.box = origin + (x*midpoint.x + y*midpoint.y + z*midpoint.z)*metres;
    f.hub = origin + (x*NativeHub.x + y*NativeHub.y + z*NativeHub.z)*metres;
    f.axis = x*NativeAxis.x + y*NativeAxis.y + z*NativeAxis.z;
    f.u = x*NativeUp.x + y*NativeUp.y + z*NativeUp.z;
    f.v = Cross(f.axis, f.u);
    return f;
}

// The point `p`'s angle round the axis (radians, -pi..pi; + turns right-handed about the axis) and
// its distance from it.
inline float AngleOf(const Frame& f, V3 p, float* radius = nullptr) {
    const V3 d = p - f.hub;
    const float a = Dot(d, f.u);
    const float b = Dot(d, f.v);
    if (radius != nullptr) {
        *radius = std::sqrt(a * a + b * b);
    }
    return std::atan2(b, a);
}

// The knob (the middle of its handle, where the fist closes) at the crank's angle `angle`.
inline V3 KnobAt(const Geometry& g, const Frame& f, float angle) {
    return f.hub + f.u * (std::cos(angle) * g.arm_m) + f.v * (std::sin(angle) * g.arm_m) +
           f.axis * (0.5f * g.knob_out_m);
}

// The angle a - b wrapped to -pi..pi.
inline float WrapPi(float a) {
    while (a > Pi) {
        a -= TwoPi;
    }
    while (a < -Pi) {
        a += TwoPi;
    }
    return a;
}

// ---- The tune ---------------------------------------------------------------------------------

// The box's use clip (a00 100170, goodsUseAnim 27) plays c100171 and c100170 33 ms later as the
// lid opens (the tune); the crank's play queues them (core/vr/bloodborne_quickitems.cpp, the use
// sounds' player) under this cue id instead of a goodsUseAnim.
inline constexpr std::int32_t TuneCue = -27;
constexpr Quick::UseSounds Tune() {
    Quick::UseSounds u{};
    u.clip = 100170;
    u.own = true;
    u.count = 2;
    u.sound[0] = {100171, 0};
    u.sound[1] = {100170, 33};
    return u;
}

// ---- The crank -------------------------------------------------------------------------------

struct Config {
    float grab_m{0.06f};     // the right grip this near the knob takes it
    float release_m{0.16f};  // ... and this far from the crank's circle lets go (a tracking jump)
    float min_radius_m{0.008f}; // nearer the axis than this the angle means nothing: no turn
    float lock_deg{30.0f};      // the way round is chosen after this much turning one way
    float reverse_deg{120.0f};  // turned back this far (net) the other way round counts instead
    float jump_deg{100.0f};     // more in one frame is a tracking jump: not counted
    float play_turns{1.0f};     // turned this much, the box plays
    float tick_turns{0.25f};    // a ratchet tick every quarter turn
    float replay_s{4.9f};       // the next play at the earliest this long after one
    float snap_s{0.08f};        // the hunter's hand eases onto the knob this fast
};

// What one update of the crank did.
struct Step {
    int ticks{0};          // ratchet ticks this frame (quarter turns counted)
    bool play{false};      // the box plays now
    bool jumped{false};    // a jump of the hand was not counted
    float progress{0.0f};  // 0..1 toward the next play
};

// The turning of the crank: grabbed by the right hand near its knob, it follows the hand's angle
// round the axis (both ways); what counts toward a play is the turning one way round (the first
// lock_deg of net turning picks it; a ratchet: turning back is free, until reverse_deg of it picks
// the other way, keeping what was counted). A play every play_turns, at most one per replay_s:
// turned further meanwhile, it plays as soon as the tune is over.
class Crank {
public:
    // A new box in the hand (the progress starts at 0, the crank's arm up).
    void Reset() {
        *this = {};
    }

    // The right grip squeezed at `hand`: takes the knob when it is within grab_m of the knob or of
    // the knob's circle (wherever the knob was left: it jumps to the hand's angle; a knob left at
    // the bottom, against the left hand, needs no reach round it). True: grabbed.
    bool TryGrab(const Config& c, const Geometry& g, const Frame& f, V3 hand, float* distance =
                                                                                  nullptr) {
        float r = 0.0f;
        const float a = AngleOf(f, hand, &r);
        const float off_axis = Dot(hand - f.hub, f.axis) - 0.5f * g.knob_out_m;
        const float d = std::min(Length(hand - KnobAt(g, f, angle_)),
                                 std::hypot(r - g.arm_m, off_axis));
        if (distance != nullptr) {
            *distance = d;
        }
        if (!f.valid || !std::isfinite(a) || !std::isfinite(r) ||
            !std::isfinite(off_axis) || !std::isfinite(d) || d > c.grab_m) {
            return false;
        }
        grabbed_ = true;
        // The knob jumps to the hand's angle (the hand is on it: within grab_m).
        have_angle_ = r >= c.min_radius_m;
        prev_ = have_angle_ ? a : angle_;
        angle_ = prev_;
        return true;
    }

    void Release() {
        grabbed_ = false;
    }

    // The game refused the last play (a roll, a stagger: the item cannot be used now): it never
    // played, so the next full turn may play at once (no replay_s wait for a tune never heard).
    void Refused() {
        if (refundable_) {
            refundable_ = false;
            --plays_;
            last_play_ = prev_play_;
        }
    }

    // Every frame while grabbed: the hand at `hand`, `t` seconds (monotonic). `can_play`: the
    // game may use the item now (the box's answer to the last play is in, nothing refuses).
    Step Update(const Config& c, const Geometry& g, const Frame& f, V3 hand, double t,
                bool can_play = true) {
        Step s{};
        s.progress = Progress(c);
        if (!grabbed_) {
            return s;
        }
        if (!f.valid) {
            Release();
            return s;
        }
        float r = 0.0f;
        const float a = AngleOf(f, hand, &r);
        // Pulled away from the crank's circle (a tracking jump, the hand gone): let go.
        const float off_axis = std::abs(Dot(hand - f.hub, f.axis) - 0.5f * g.knob_out_m);
        if (!std::isfinite(a) || !std::isfinite(r) || !std::isfinite(off_axis) ||
            std::hypot(r - g.arm_m, off_axis) > c.release_m) {
            grabbed_ = false;
            released_far_ = true;
            return s;
        }
        if (r < c.min_radius_m) {
            have_angle_ = false;
            return s; // on the axis: no angle; the crank stays
        }
        if (!have_angle_) {
            // Reestablish contact without counting rotation we could not observe.
            have_angle_ = true;
            prev_ = angle_ = a;
            return s;
        }
        const float d = WrapPi(a - prev_);
        prev_ = a;
        if (std::abs(d) > c.jump_deg * (Pi / 180.0f)) {
            s.jumped = true;
            angle_ = a;
            return s;
        }
        angle_ = WrapPi(angle_ + d);
        turned_ += std::abs(d);
        const float lock = c.lock_deg * (Pi / 180.0f);
        bool forward = false;
        if (dir_ == 0) {
            net_ += d;
            if (std::abs(net_) >= lock) {
                dir_ = net_ > 0.0f ? 1 : -1;
                counted_ += std::abs(net_);
                forward = true;
                back_ = 0.0f;
            }
        } else if (d * static_cast<float>(dir_) > 0.0f) {
            counted_ += std::abs(d);
            forward = true;
            back_ = std::max(0.0f, back_ - std::abs(d));
        } else {
            back_ += std::abs(d);
            if (back_ >= c.reverse_deg * (Pi / 180.0f)) {
                dir_ = -dir_; // the other way round from now (what was counted is kept)
                back_ = 0.0f;
            }
        }
        const float tick = std::max(c.tick_turns, 0.01f) * TwoPi;
        const int quarters = static_cast<int>(std::floor(counted_ / tick));
        if (quarters > quarters_) {
            s.ticks = quarters - quarters_;
            quarters_ = quarters;
        }
        const float full = std::max(c.play_turns, 0.05f) * TwoPi;
        if (counted_ >= full) {
            counted_ = full; // a full turn waits for the tune (never more than one ahead)
            const bool rested = last_play_ < 0.0 || t - last_play_ >= c.replay_s - 1e-4;
            if (forward && rested && can_play) {
                s.play = true;
                prev_play_ = last_play_;
                refundable_ = true;
                last_play_ = t;
                ++plays_;
                counted_ = 0.0f;
                quarters_ = 0;
            }
        }
        s.progress = Progress(c);
        return s;
    }

    [[nodiscard]] bool Grabbed() const {
        return grabbed_;
    }
    // Let go this frame because the hand went far from the crank (the caller's log).
    [[nodiscard]] bool TakeReleasedFar() {
        const bool r = released_far_;
        released_far_ = false;
        return r;
    }
    [[nodiscard]] float Angle() const {
        return angle_;
    }
    [[nodiscard]] int Direction() const {
        return dir_;
    }
    [[nodiscard]] float Progress(const Config& c) const {
        return std::clamp(counted_ / (std::max(c.play_turns, 0.05f) * TwoPi), 0.0f, 1.0f);
    }
    [[nodiscard]] float TurnedTurns() const {
        return turned_ / TwoPi;
    }
    [[nodiscard]] int Plays() const {
        return plays_;
    }
    [[nodiscard]] double LastPlay() const {
        return last_play_;
    }

private:
    bool grabbed_{false};
    bool have_angle_{false};
    bool released_far_{false};
    float angle_{0.0f};   // the crank's arm (radians; 0 = up the box's u)
    float prev_{0.0f};    // the hand's angle last frame
    float net_{0.0f};     // signed turning before the way round was chosen
    int dir_{0};          // +1 / -1 once chosen
    float back_{0.0f};    // turned back since the last forward turn
    float counted_{0.0f}; // toward the next play
    float turned_{0.0f};  // all the turning (the log)
    int quarters_{0};
    int plays_{0};
    double last_play_{-1.0};
    double prev_play_{-1.0}; // the play before (Refused puts it back)
    bool refundable_{false}; // the last play may still be refused (once)
};

// ---- The restore rule --------------------------------------------------------------------------

// The right hand is free (the weapon put away, no swings, no transform) exactly while the box is
// in the left hand in crank mode during gameplay; anything else (put back, a menu, the screen, a
// cinematic, death: the quick items' controller leaves its Holding phase on all of them) brings
// the weapon back on the very next frame.
struct Hold {
    bool crank_box{false}; // Quick::Output::crank_box (the box in the hand, crank mode)
    bool gameplay{false};  // Quick::Input::gameplay
    bool alive{true};
};
constexpr bool RightHandFree(const Hold& h) {
    return h.crank_box && h.gameplay && h.alive;
}

// The hunter's right hand onto the knob: the blend toward it (0 = the controller, 1 = the knob),
// eased in over snap_s while grabbed and out as fast after.
inline float SnapStep(float weight, bool grabbed, float dt, float snap_s) {
    const float step = snap_s > 1e-4f ? dt / snap_s : 1.0f;
    return std::clamp(weight + (grabbed ? step : -step), 0.0f, 1.0f);
}

// ---- Channels ----------------------------------------------------------------------------------

// Presenter -> render-camera hook (the box's look) and pose hook (the right weapon's hide), every
// input frame while the box is in the hand (crank mode). Older than StaleNs: nothing.
struct Scene {
    std::int64_t steady_ns{0};
    bool active{false};    // the box is in the left hand (draw it; hide the right weapon)
    bool grabbed{false};   // the right hand is on the knob
    float angle{0.0f};     // the crank's arm (radians, round the frame's axis from u)
    float progress{0.0f};  // 0..1 toward the next play
    std::int64_t play_ns{0}; // the last play (steady ns; 0 none): the lid's glow
    Frame frame{};         // LOCAL
    Geometry geometry{};
};
inline constexpr std::int64_t StaleNs = 250'000'000;

inline SeqLock<Scene>& SceneChannel() {
    static SeqLock<Scene> channel;
    return channel;
}
inline void PublishScene(const Scene& s) {
    SceneChannel().Store(s);
}
inline bool LatestScene(Scene& out) {
    return SceneChannel().Load(out);
}
// Fresh and active at `now_ns` (the steady clock).
inline bool ActiveAt(const Scene& s, std::int64_t now_ns) {
    return s.active && now_ns - s.steady_ns >= -StaleNs && now_ns - s.steady_ns < StaleNs;
}

// Pose hook -> presenter: the right weapon can be put away (its two signatures matched and the
// pose hook runs the hide; core/vr/bloodborne_held.cpp). Never set, the box is used the game's way
// (Quick::Config::crank off), so a weapon is never left in a hand that cannot swing. Only ever set.
inline std::atomic_bool& HideReady() {
    static std::atomic_bool ready{false};
    return ready;
}

} // namespace VR::MusicBox
