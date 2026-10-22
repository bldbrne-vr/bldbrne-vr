// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>

#include "core/vr/body_frame.h"

// VR body: a confirmed injection holds the published hand poses for a fixed wall-clock
// duration, then eases back to live tracking. Detection continues using live controllers.
// Existing combat/context interruptions are retained; pulling away adds no early release.
//
// VialButton: X in gameplay with the stab-to-inject vial (inject=1). Only X's press counts: it
// arms a vial, or puts the armed one back. Holding X does nothing more - never the game's own
// Triangle vial with its animation and movement lock (until 2026-10-06 a 0.6 s hold pressed it;
// the tester's headset test drank three vials that way without meaning to).
//
// Pure (no OpenXR or emulator state): the presenter runs both on the OpenXR frame thread, the
// unit tests (tests/test_inject_hold.cpp) on scripted inputs.
namespace VR::Body {

enum class VialKey : std::uint8_t {
    None,
    Arm,    // a press with no vial armed: arm one
    Disarm, // a press with a vial armed: put it back
    Held,   // the press has been held VialButton::LongHoldS (once per press; log only: nothing)
};

class VialButton {
public:
    static constexpr float LongHoldS = 0.6f;

    // One input frame: X down now, a vial armed, an injection with the game (`waiting`: a press
    // then arms nothing), `dt` seconds since the last frame.
    VialKey Update(bool x, bool armed, bool waiting, float dt) {
        const bool press = x && !down_;
        down_ = x;
        if (!x) {
            held_s_ = 0.0f;
            long_noted_ = false;
            return VialKey::None;
        }
        if (press) {
            held_s_ = 0.0f;
            long_noted_ = false;
            return armed ? VialKey::Disarm : waiting ? VialKey::None : VialKey::Arm;
        }
        held_s_ += std::max(dt, 0.0f);
        if (!long_noted_ && held_s_ >= LongHoldS) {
            long_noted_ = true;
            return VialKey::Held;
        }
        return VialKey::None;
    }

    // A context switch (the presenter ignores buttons held through it until they are let go).
    void Reset() {
        *this = {};
    }

    [[nodiscard]] bool Down() const {
        return down_;
    }
    [[nodiscard]] float HeldS() const {
        return held_s_;
    }

private:
    bool down_{false};
    float held_s_{0.0f};
    bool long_noted_{false};
};

// Ownership of a vial physically drawn from a pouch. X/legacy draws never Start this.
// Checked before stab detection; returning only disarms the prop, never touches inventory.
class VialGripHold {
public:
    void Start(float release) {
        release_ = std::clamp(release, 0.0f, 1.0f);
        active_ = true;
    }
    bool Return(float left_grip, bool left_tracked) {
        if (!active_ || (left_tracked && std::isfinite(left_grip) && left_grip > release_))
            return false;
        Reset();
        return true;
    }
    void Reset() { active_ = false; }
private:
    float release_{0.45f};
    bool active_{false};
};

// The weapon hand's freeze ends at once when the controller moves this fast (m/s, its own speed):
// a stroke under way. It is the swing detector's re-arm speed, below its 2 m/s fire speed
// (core/vr/swing_detector.h), so the hunter's blade is back on the controller before any swing
// can commit: a swing during the freeze is never delayed nor lost (the detector reads the live
// controller anyway), the freeze only ever gives way to it.
constexpr float SwingBreakSpeed = 1.4f;

// Legacy frame-counted implementation (retained for regression coverage; runtime uses FreezeTime).
// The freeze counted in the game's own frames (vial_inject_hold_frames). A game frame is one
// main-player pose copy (core/vr/body_channel.h BodyStatus::seq, published after each copy and
// stamped with its start, when it reads the newest hands): every copy that began after the
// freeze began drew the hunter's arms still, and the stillness ends at the start of the last one
// counted, so the next copy already sees the release. Without the pose hook (no fresh status:
// loading, third person, the body off) the frames are timed at the game's frame time instead,
// and a stall never holds the hands longer than CapFactor times that.
class FreezeFrames {
public:
    static constexpr std::int64_t CapFactorNum = 3; // 1.5x
    static constexpr std::int64_t CapFactorDen = 2;
    static constexpr int MaxFrames = 15;

    // From `from`: the time the held pose was published (the grips of the stab's frame: a copy
    // that began after it already reads the held pose, even before the freeze was decided).
    // `frames` game frames (clamped to MaxFrames; false and off for none), `frame_ns` the game's
    // frame time. `counting`: a fresh status, `seq` and `status_ns` the newest one's sequence and
    // stamp (that copy counts when it began after `from`).
    bool Start(std::int64_t from, int frames, std::int64_t frame_ns, bool counting,
               std::uint64_t seq = 0, std::int64_t status_ns = 0) {
        on_ = false;
        if (frames <= 0) {
            return false;
        }
        frames_ = std::min(frames, MaxFrames);
        start_ns_ = from;
        frame_ns_ = std::max<std::int64_t>(frame_ns, 1);
        counting_ = counting;
        base_ = seq;
        counted_ = 0;
        first_ns_ = end_ns_ = 0;
        const std::int64_t span = frame_ns_ * frames_;
        cap_ns_ = from + (counting ? span * CapFactorNum / CapFactorDen : span);
        on_ = true;
        if (counting && seq > 0 && status_ns > from) {
            base_ = seq - 1; // the newest copy already drew the held pose
            Note(seq, status_ns);
        }
        return true;
    }

    // The newest status (its sequence, its copy's start): counts the copies since the last note
    // that began after the freeze. Several copies between two notes (a catch-up) all count.
    void Note(std::uint64_t seq, std::int64_t status_ns) {
        if (!on_ || !counting_ || seq <= base_ + static_cast<std::uint64_t>(counted_)) {
            return;
        }
        if (status_ns <= start_ns_) {
            base_ = seq - static_cast<std::uint64_t>(counted_); // began before: not ours
            return;
        }
        const int was = counted_;
        counted_ = static_cast<int>(std::min<std::uint64_t>(seq - base_, frames_));
        if (was == 0) {
            first_ns_ = status_ns;
        }
        if (counted_ >= frames_ && end_ns_ == 0) {
            end_ns_ = status_ns;
        }
    }

    // The stillness is over at `now`: every frame counted (it ended at the last one's start),
    // the frames timed (no status) or the cap (a stall).
    [[nodiscard]] bool Done(std::int64_t now) const {
        return on_ && (end_ns_ != 0 || now >= cap_ns_);
    }

    // When the stillness ended (Done): the last counted frame's start, else the cap.
    [[nodiscard]] std::int64_t EndNs() const {
        return end_ns_ != 0 ? end_ns_ : cap_ns_;
    }
    // The longest it can last (the hands' InjectHold runs to it unless released before).
    [[nodiscard]] std::int64_t CapNs() const {
        return cap_ns_;
    }
    // How the stillness ended: counted (every frame seen), else timed or capped.
    [[nodiscard]] bool Counted() const {
        return end_ns_ != 0;
    }
    [[nodiscard]] bool Counting() const {
        return counting_;
    }
    [[nodiscard]] int Frames() const {
        return frames_;
    }
    [[nodiscard]] int CountedFrames() const {
        return counted_;
    }
    [[nodiscard]] std::int64_t StartNs() const {
        return start_ns_;
    }
    // The first counted frame's start (0: none yet).
    [[nodiscard]] std::int64_t FirstNs() const {
        return first_ns_;
    }
    // How long the still frames were on screen: from the first counted frame's start to the last
    // one's, plus one frame (the game's measured frame time when 2+ were counted). The timed
    // fallback: the frames at the given frame time.
    [[nodiscard]] std::int64_t StillNs() const {
        if (end_ns_ == 0 || first_ns_ == 0) {
            return frame_ns_ * frames_;
        }
        const std::int64_t frame =
            counted_ > 1 ? (end_ns_ - first_ns_) / (counted_ - 1) : frame_ns_;
        return end_ns_ - first_ns_ + frame;
    }

    [[nodiscard]] bool On() const {
        return on_;
    }
    void Stop() {
        on_ = false;
    }

private:
    bool on_{false};
    bool counting_{false};
    int frames_{0};
    int counted_{0};
    std::uint64_t base_{0};
    std::int64_t start_ns_{0};
    std::int64_t frame_ns_{1};
    std::int64_t cap_ns_{0};
    std::int64_t first_ns_{0};
    std::int64_t end_ns_{0};
};

// Monotonic deadline, independent of game pose copies and XR/game frame rates.
class FreezeTime {
public:
    bool Start(std::int64_t now, int milliseconds) {
        start_ = now;
        end_ = now + std::int64_t{std::clamp(milliseconds, 0, 1000)} * 1'000'000;
        on_ = end_ > start_;
        return on_;
    }
    bool On() const { return on_; }
    bool Done(std::int64_t now) const { return on_ && now >= end_; }
    std::int64_t StartNs() const { return start_; }
    std::int64_t EndNs() const { return end_; }
    void Stop() { on_ = false; }
private:
    bool on_{};
    std::int64_t start_{}, end_{};
};

class InjectHold {
public:
    enum class Phase : std::uint8_t {
        Off,     // the live grip passes through
        Held,    // the held pose, still
        Release, // easing back to the live grip
    };

    // Hold `at` (the grip as published this frame, tracking space `serial`) from `now` for at most
    // `hold_ns` (wall-clock duration; existing interruptions may call Release), then ease back over
    // `release_ns` (0: back at once). False (nothing held) for an untracked grip or no hold time.
    // A new start replaces a hold that still runs.
    bool Start(const TrackedHand& at, std::uint32_t serial, std::int64_t now, std::int64_t hold_ns,
               std::int64_t release_ns) {
        if (hold_ns <= 0 || !IsTracked(at)) {
            return false;
        }
        p_ = at.p;
        q_ = Normalize(at.q);
        serial_ = serial;
        start_ns_ = now;
        hold_end_ns_ = now + hold_ns;
        release_ns_ = std::max<std::int64_t>(release_ns, 0);
        on_ = true;
        return true;
    }

    // Leave the stillness at `now` (the game frames counted, the gun's trigger, a swing, a menu, a
    // dodge, death; `now` may be a moment ago: the last still frame's start): the release starts
    // there. Nothing while it already eases back or nothing is held.
    void Release(std::int64_t now) {
        if (on_ && now < hold_end_ns_) {
            hold_end_ns_ = std::max(now, start_ns_);
        }
    }

    void Cancel() {
        on_ = false;
    }

    [[nodiscard]] bool On() const {
        return on_;
    }
    [[nodiscard]] Vec3 HeldPosition() const {
        return p_;
    }
    [[nodiscard]] std::int64_t StartNs() const {
        return start_ns_;
    }
    // When the stillness ended (or ends): the release's start.
    [[nodiscard]] std::int64_t HoldEndNs() const {
        return hold_end_ns_;
    }

    // The phase at `now` (no state change).
    [[nodiscard]] Phase PhaseAt(std::int64_t now) const {
        if (!on_) {
            return Phase::Off;
        }
        if (now < hold_end_ns_) {
            return Phase::Held;
        }
        return now < hold_end_ns_ + release_ns_ ? Phase::Release : Phase::Off;
    }

    // The held pose's weight at `now`: 1 while still, easing (smoothstep) to 0 over the release.
    [[nodiscard]] float Weight(std::int64_t now) const {
        switch (PhaseAt(now)) {
        case Phase::Held:
            return 1.0f;
        case Phase::Release: {
            const float t = std::clamp(static_cast<float>(now - hold_end_ns_) /
                                           static_cast<float>(std::max<std::int64_t>(release_ns_, 1)),
                                       0.0f, 1.0f);
            return 1.0f - t * t * (3.0f - 2.0f * t);
        }
        case Phase::Off:
            break;
        }
        return 0.0f;
    }

    // A pose of tracking space `serial` moved toward the held one by Weight(now) (no state
    // change; the overlay's own vial). False: untouched (nothing held, another tracking space).
    bool Blend(Vec3& p, Quat& q, std::uint32_t serial, std::int64_t now) const {
        const float w = serial == serial_ ? Weight(now) : 0.0f;
        if (w <= 0.0f) {
            return false;
        }
        p = Lerp(p, p_, w);
        q = Slerp(Normalize(q), q_, w);
        return true;
    }

    // This frame's left grip `live` (tracking space `serial`) as the game should see it: the held
    // pose while still (tracked, at rest, whatever the controller's tracking does meanwhile), the
    // blend while easing back, else untouched. Ends the hold after its release, on a recenter
    // (another space: the held pose is in the old one) and when the live grip is not tracked while
    // easing back (the game sees the live grip as it is). Returns the phase used.
    Phase Apply(TrackedHand& live, std::uint32_t serial, std::int64_t now) {
        if (!on_) {
            return Phase::Off;
        }
        if (serial != serial_) {
            on_ = false;
            return Phase::Off;
        }
        const Phase phase = PhaseAt(now);
        if (phase == Phase::Off || (phase == Phase::Release && !IsTracked(live))) {
            on_ = false;
            return Phase::Off;
        }
        const float w = Weight(now);
        if (phase == Phase::Held) {
            live.p = p_;
            live.q = q_;
            live.v = {};
            live.w = {};
            live.position_valid = live.position_tracked = true;
            live.orientation_valid = live.orientation_tracked = true;
            return phase;
        }
        live.p = Lerp(live.p, p_, w);
        live.q = Slerp(Normalize(live.q), q_, w);
        live.v = live.v * (1.0f - w);
        live.w = live.w * (1.0f - w);
        return phase;
    }

private:
    bool on_{false};
    Vec3 p_{};
    Quat q_{};
    std::uint32_t serial_{0};
    std::int64_t start_ns_{0};
    std::int64_t hold_end_ns_{0};
    std::int64_t release_ns_{0};
};

} // namespace VR::Body
